/*
 * axis-library-scan.c - one scan of one root: the guard, the walk, the
 * batches and the conclusion
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The calling thread holds one pooled connection (with the volume's lock
 * on it) and the volume's read guard for the whole scan, and does all the
 * database work; the walk runs on a thread of its own and hands batches
 * over.  The calling thread checks the guard every
 * AXIS_LIBRARY_GUARD_INTERVAL_MS while the walk runs, so a volume that
 * goes away stops the scan within seconds, and watches for a walk that
 * makes no progress (a hung disk), which it gives up on rather than wait
 * in the kernel with it.
 *
 * The order of the end matters: the walk has ended with no I/O error; the
 * guard passes; the root's path still leads to the directory that was
 * walked; the root was not found empty while the index says it holds
 * files; the guard passes again; then, in one transaction, the conclusion,
 * and the guard once more before COMMIT.  Any failure on the way records
 * the scan as incomplete or unavailable, and no file changes state.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "library/axis-library-private.h"
#include "storage/axis-volume-guard.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* How long the scan waits for a batch before looking at everything else,
 * and for a stopped walk's thread before giving up on it. */
#define POLL_MS       200
#define STOP_WAIT_MS  1000

typedef struct
{
	AxisLibrary           *self;
	AxisDbConn            *conn;
	AxisScanState         *state;
	const AxisRootInfo    *root;
	const gchar           *volume_name;
	const AxisScanOptions *options;
	AxisLibraryTuning      tuning;
	AxisScanResult        *result;
	AxisVolumeGuard       *guard;
	AxisLibraryWalk       *walk;
	AxisWalkTotals         totals;
} Scan;

/* ── Walks given up on ──────────────────────────────────────────── */

static gboolean
volume_has_stuck_walk(
	AxisLibrary *self,
	gint64       volume_id
){
	AxisLibraryWalk *walk;
	gboolean stuck = FALSE;

	g_mutex_lock(&self->lock);
	walk = g_hash_table_lookup(self->abandoned, &volume_id);
	if (walk != NULL) {
		if (axis_library_walk_wait(walk, 0))
			g_hash_table_remove(self->abandoned, &volume_id);
		else
			stuck = TRUE;
	}
	g_mutex_unlock(&self->lock);

	return stuck;
}

/* Stops the walk; one whose thread does not end soon is left to end on its
 * own (it is stuck in the kernel) and remembered, so that the volume is not
 * walked again while it is. */
static void
stop_walk(Scan *scan)
{
	AxisLibraryWalk *walk = scan->walk;

	if (walk == NULL)
		return;
	scan->walk = NULL;

	axis_library_walk_stop(walk);
	if (!axis_library_walk_wait(walk, STOP_WAIT_MS)) {
		gint64 *key = g_new(gint64, 1);

		g_warning("library: the scan of root '%s' on volume '%s' is waiting for the filesystem and "
			"was given up; its thread ends when the filesystem answers", scan->root->name,
			scan->volume_name);
		*key = scan->root->volume_id;
		g_mutex_lock(&scan->self->lock);
		g_hash_table_replace(scan->self->abandoned, key, walk);
		g_mutex_unlock(&scan->self->lock);
		return;
	}

	axis_library_walk_unref(walk);
}

/* ── Outcomes ───────────────────────────────────────────────────── */

static void
take_totals(Scan *scan)
{
	if (scan->walk != NULL)
		axis_library_walk_get_totals(scan->walk, &scan->totals);
}

/* Records the scan as not complete; no file changes state. */
static gboolean
end_scan(
	Scan            *scan,
	AxisScanStatus   status,
	AxisScanReason   reason,
	const gchar     *message,
	GError         **error
){
	take_totals(scan);
	stop_walk(scan);

	scan->result->status = status;
	scan->result->reason = reason;
	g_free(scan->result->message);
	scan->result->message = axis_library_text_clean(message, AXIS_LIBRARY_MESSAGE_CHARS);
	scan->result->walk = scan->totals;
	scan->result->new_files = scan->state->new_files;
	scan->result->changed_files = scan->state->changed_files;
	scan->result->reappeared = scan->state->reappeared;

	return axis_library_index_end(scan->conn, scan->state, status, reason, scan->result->message,
		&scan->totals, error);
}

static gboolean
unavailable(
	Scan         *scan,
	const GError *why,
	GError      **error
){
	g_autofree gchar *message = g_strdup_printf("%s Nothing was concluded from this scan.",
		why != NULL ? why->message : "The volume is unavailable.");

	return end_scan(scan, AXIS_SCAN_UNAVAILABLE, AXIS_SCAN_REASON_VOLUME_UNAVAILABLE, message, error);
}

/* The root's directory could not be opened through the guard: why. */
static gboolean
root_unusable(
	Scan    *scan,
	GError  *why,
	GError **error
){
	g_autofree gchar *message = NULL;

	if (g_error_matches(why, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE))
		return unavailable(scan, why, error);

	if (g_error_matches(why, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED)) {
		message = g_strdup_printf("The root's directory is now reached through a symbolic link or "
			"lies on another filesystem (%s). Nothing was concluded.", why->message);
		return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_ROOT_CHANGED, message, error);
	}
	if (g_error_matches(why, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
	    g_error_matches(why, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY)) {
		message = g_strdup_printf("The root's directory is not on volume '%s' now (it may have been "
			"renamed or removed). Nothing was concluded; its files keep their records.",
			scan->volume_name);
		return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_ROOT_MISSING, message, error);
	}

	message = g_strdup_printf("The root's directory could not be opened: %s Nothing was concluded.",
		why->message);
	return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_IO_ERRORS, message, error);
}

static gboolean
is_stopping(Scan *scan)
{
	return g_cancellable_is_cancelled(scan->options->cancellable);
}

static gboolean
stopped(
	Scan    *scan,
	GError **error
){
	gboolean shutdown = scan->options->is_shutdown != NULL &&
		scan->options->is_shutdown(scan->options->data);

	return end_scan(scan, AXIS_SCAN_INCOMPLETE,
		shutdown ? AXIS_SCAN_REASON_SHUTDOWN : AXIS_SCAN_REASON_CANCELLED,
		shutdown ? "The daemon was shutting down; the scan stopped before it finished and "
			"concluded nothing." :
			"The scan was cancelled before it finished and concluded nothing.", error);
}

static void
report_progress(Scan *scan)
{
	g_autofree gchar *message = NULL;
	gdouble fraction = 0;

	if (scan->options->progress == NULL)
		return;

	take_totals(scan);
	if (scan->state->previous_files > 0)
		fraction = MIN(0.99, (gdouble)scan->totals.files / (gdouble)scan->state->previous_files);
	message = g_strdup_printf("%s: %" G_GUINT64_FORMAT " files in %" G_GUINT64_FORMAT " folders",
		scan->root->name, scan->totals.files, scan->totals.dirs);
	scan->options->progress(fraction, message, scan->options->data);
}

/* The last check before the conclusion is committed: not stopped, and the
 * volume still the one walked. */
static gboolean
guard_still_holds(
	gpointer   data,
	GError   **error
){
	Scan *scan = data;

	if (g_cancellable_set_error_if_cancelled(scan->options->cancellable, error))
		return FALSE;
	return axis_volume_guard_check(scan->guard, error);
}

/* Whether the root's path still leads to the directory that was walked;
 * when it does not, records why and returns FALSE with *@recorded set. */
static gboolean
root_unchanged(
	Scan      *scan,
	guint64    dev,
	guint64    ino,
	gboolean  *recorded,
	GError   **error
){
	GError *local = NULL;
	struct stat st;
	gint fd;

	*recorded = FALSE;
	fd = axis_volume_guard_open_dir(scan->guard, scan->root->rel_path, &local);
	if (fd < 0) {
		*recorded = root_unusable(scan, local, error);
		g_error_free(local);
		return FALSE;
	}
	if (fstat(fd, &st) != 0 || (guint64)st.st_dev != dev || (guint64)st.st_ino != ino) {
		close(fd);
		*recorded = end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_ROOT_CHANGED,
			"The root's directory was replaced while it was scanned (renamed, moved or mounted "
			"over). Nothing was concluded.", error);
		return FALSE;
	}
	close(fd);

	return TRUE;
}

/* ── The scan ───────────────────────────────────────────────────── */

/* TRUE when the outcome was recorded (in scan->result). */
static gboolean
run(
	Scan    *scan,
	GError **error
){
	AxisLibrary *self = scan->self;
	AxisWalkConfig config;
	GError *local = NULL;
	gint64 interval_us;
	gint64 stall_us;
	gint64 last_check;
	gint64 last_step_time;
	guint64 last_steps;
	guint64 dev = 0;
	guint64 ino = 0;
	gboolean recorded;
	gboolean present;
	gint fd;

	if (volume_has_stuck_walk(self, scan->root->volume_id)) {
		g_autofree gchar *message = g_strdup_printf("An earlier scan of volume '%s' is still waiting "
			"for its filesystem to answer; it may be hung.", scan->volume_name);

		return end_scan(scan, AXIS_SCAN_UNAVAILABLE, AXIS_SCAN_REASON_VOLUME_UNAVAILABLE, message,
			error);
	}

	scan->guard = axis_volume_guard_acquire(self->storage, scan->root->volume_id, AXIS_VOLUME_INTENT_READ,
		&local);
	if (scan->guard == NULL) {
		recorded = unavailable(scan, local, error);
		g_error_free(local);
		return recorded;
	}

	fd = axis_volume_guard_open_dir(scan->guard, scan->root->rel_path, &local);
	if (fd < 0) {
		recorded = root_unusable(scan, local, error);
		g_error_free(local);
		return recorded;
	}

	memset(&config, 0, sizeof(config));
	config.batch_entries = scan->tuning.batch_entries;
	config.racy_window_ns = scan->tuning.racy_window_ns;
	config.patterns = scan->state->patterns;
	config.before_directory = scan->tuning.before_directory;
	config.after_last = scan->tuning.after_last;
	config.hook_data = scan->tuning.hook_data;
	scan->walk = axis_library_walk_start(fd, &config, &local);
	if (scan->walk == NULL) {
		g_autofree gchar *message = g_strdup_printf("The scan could not start: %s", local->message);

		g_error_free(local);
		return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_FAILED, message, error);
	}
	axis_library_walk_get_root_identity(scan->walk, &dev, &ino);

	interval_us = (gint64)(scan->tuning.guard_interval_ms > 0 ? scan->tuning.guard_interval_ms :
		AXIS_LIBRARY_GUARD_INTERVAL_MS) * 1000;
	stall_us = (gint64)(scan->tuning.stall_ms > 0 ? scan->tuning.stall_ms : AXIS_LIBRARY_STALL_MS) * 1000;
	last_check = last_step_time = g_get_monotonic_time();
	last_steps = axis_library_walk_get_steps(scan->walk);

	for (;;) {
		gboolean finished = FALSE;
		AxisWalkBatch *batch = axis_library_walk_next(scan->walk, POLL_MS, &finished);
		gint64 now;
		guint64 steps;

		if (batch != NULL) {
			gboolean ok;

			take_totals(scan);
			ok = axis_library_index_write(scan->conn, scan->state, batch, &scan->totals,
				scan->tuning.before_commit, scan->tuning.hook_data, &local);
			axis_walk_batch_free(batch);
			if (!ok) {
				g_autofree gchar *message = g_strdup_printf("The index could not be written: %s",
					local->message);

				/* The connection may be gone: try, and report the first error. */
				end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_FAILED, message, NULL);
				g_propagate_error(error, local);
				return FALSE;
			}
			if (scan->tuning.after_batch != NULL)
				scan->tuning.after_batch(scan->state->batches, scan->tuning.hook_data);
			report_progress(scan);
			last_step_time = g_get_monotonic_time();
		}
		if (finished)
			break;

		if (is_stopping(scan))
			return stopped(scan, error);

		now = g_get_monotonic_time();
		steps = axis_library_walk_get_steps(scan->walk);
		if (steps != last_steps) {
			last_steps = steps;
			last_step_time = now;
		}

		if (now - last_check >= interval_us) {
			if (!axis_volume_guard_check(scan->guard, &local)) {
				recorded = unavailable(scan, local, error);
				g_error_free(local);
				return recorded;
			}
			last_check = g_get_monotonic_time();
		}

		if (now - last_step_time >= stall_us) {
			g_autofree gchar *message = NULL;

			if (!axis_volume_guard_check(scan->guard, &local)) {
				recorded = unavailable(scan, local, error);
				g_error_free(local);
				return recorded;
			}
			message = g_strdup_printf("The filesystem of volume '%s' stopped answering for %u "
				"seconds in the middle of the scan. Nothing was concluded.", scan->volume_name,
				(guint)(stall_us / G_USEC_PER_SEC));
			return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_STALLED, message, error);
		}
	}

	/* The walk is over: every batch is in. */
	take_totals(scan);
	if (axis_library_walk_get_status(scan->walk) != AXIS_WALK_DONE)
		return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_FAILED,
			"The walk ended early. Nothing was concluded.", error);
	if (axis_library_walk_had_io_errors(scan->walk)) {
		g_autofree gchar *message = g_strdup_printf("%" G_GUINT64_FORMAT " folder(s) or file(s) "
			"could not be read because of errors that may mean the disk is failing. Nothing was "
			"concluded.", MAX(scan->totals.io_errors, (guint64)1));

		return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_IO_ERRORS, message, error);
	}
	axis_library_walk_unref(scan->walk);
	scan->walk = NULL;

	/* The volume is still the one walked... */
	if (!axis_volume_guard_check(scan->guard, &local)) {
		recorded = unavailable(scan, local, error);
		g_error_free(local);
		return recorded;
	}
	/* ...and the root still the directory walked. */
	if (!root_unchanged(scan, dev, ino, &recorded, error))
		return recorded;

	/* An empty root where the index has files is what a disk that is not
	 * mounted under the root looks like: never taken for "all gone". */
	if (scan->totals.root_entries == 0) {
		if (!axis_library_index_has_present(scan->conn, scan->root->id, &present, error))
			return FALSE;
		if (present)
			return end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_ROOT_EMPTY,
				"The root's directory is empty, while files were recorded in it. Nothing was "
				"concluded: if they are really gone, remove the root and add it again.", error);
	}

	/* Immediately before the conclusion, and again inside it. */
	if (is_stopping(scan))
		return stopped(scan, error);
	if (!axis_volume_guard_check(scan->guard, &local)) {
		recorded = unavailable(scan, local, error);
		g_error_free(local);
		return recorded;
	}

	scan->result->walk = scan->totals;
	scan->result->new_files = scan->state->new_files;
	scan->result->changed_files = scan->state->changed_files;
	scan->result->reappeared = scan->state->reappeared;
	if (!axis_library_index_complete(scan->conn, scan->state, &scan->totals, scan->result,
	                                 guard_still_holds, scan, scan->tuning.before_commit,
	                                 scan->tuning.hook_data, &local)) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			/* Rolled back: stopped at the last moment. */
			g_error_free(local);
			return stopped(scan, error);
		}
		if (local->domain == AXIS_STORAGE_ERROR) {
			/* Rolled back: the volume went away at the last moment. */
			recorded = unavailable(scan, local, error);
			g_error_free(local);
			return recorded;
		}
		{
			g_autofree gchar *message = g_strdup_printf("The scan's conclusion could not be "
				"written: %s", local->message);

			end_scan(scan, AXIS_SCAN_INCOMPLETE, AXIS_SCAN_REASON_FAILED, message, NULL);
		}
		g_propagate_error(error, local);
		return FALSE;
	}

	scan->result->status = AXIS_SCAN_COMPLETE;
	scan->result->reason = AXIS_SCAN_REASON_NONE;
	return TRUE;
}

gboolean
axis_library_scan_root(
	AxisLibrary            *self,
	gint64                  root_id,
	const AxisScanOptions  *options,
	AxisScanResult        **result,
	GError                **error
){
	static const AxisScanOptions defaults;
	g_autoptr(AxisRootInfo) root = NULL;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	AxisScanState state;
	Scan scan;
	AxisDbConn *conn;
	GError *local = NULL;
	gboolean locked = FALSE;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (result != NULL)
		*result = NULL;
	if (options == NULL)
		options = &defaults;

	if (!axis_storage_get_root(self->storage, root_id, &root, &local)) {
		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND)) {
			g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND,
				"there is no root %" G_GINT64_FORMAT, root_id);
			g_error_free(local);
		} else {
			g_propagate_error(error, local);
		}
		return FALSE;
	}
	volume = axis_storage_get_volume(self->storage, root->volume_id);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	if (!axis_library_index_lock(conn, root->volume_id, &locked, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	if (!locked) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_BUSY,
			"volume '%s' is being scanned already", volume != NULL ? volume->name : "?");
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}

	memset(&state, 0, sizeof(state));
	state.root_id = root_id;
	state.volume_id = root->volume_id;
	state.downloads = root->media_type == AXIS_MEDIA_DOWNLOADS;
	state.boot_id = g_strdup(self->boot_id);
	state.boottime_us = axis_library_boottime_us();
	if (!axis_library_index_begin(conn, &state, options->trigger != NULL ? options->trigger : "direct",
	                              options->job_id, error)) {
		axis_scan_state_clear(&state);
		axis_library_index_unlock(conn, root->volume_id);
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}

	memset(&scan, 0, sizeof(scan));
	scan.self = self;
	scan.conn = conn;
	scan.state = &state;
	scan.root = root;
	scan.volume_name = volume != NULL ? volume->name : "?";
	scan.options = options;
	g_mutex_lock(&self->lock);
	scan.tuning = self->tuning;
	g_mutex_unlock(&self->lock);
	scan.result = g_new0(AxisScanResult, 1);
	scan.result->scan_id = state.scan_id;
	scan.result->root_id = root_id;
	scan.result->generation = state.generation;
	scan.result->status = AXIS_SCAN_RUNNING;

	ok = run(&scan, error);

	stop_walk(&scan);
	g_clear_pointer(&scan.guard, axis_volume_guard_release);
	axis_library_index_unlock(conn, root->volume_id);
	axis_db_pool_release(self->pool, conn);
	axis_scan_state_clear(&state);

	if (ok && result != NULL)
		*result = scan.result;
	else
		axis_scan_result_free(scan.result);
	return ok;
}
