/*
 * axis-probe-job.c - probing one file, the jobs that ask for it, and the
 * requests that make it happen again
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probe/axis-probe-private.h"

#include <string.h>
#include <unistd.h>

#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "library/axis-library.h"
#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"

/* ── Errors ─────────────────────────────────────────────────────── */

/* The guard's "unavailable" is the probe's: nothing is recorded and the
 * job is tried again. */
static void
propagate_storage_error(
	GError **error,
	GError  *local
){
	if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE)) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_UNAVAILABLE, "%s", local->message);
		g_error_free(local);
		return;
	}
	g_propagate_error(error, local);
}

static void
set_cancelled(GError **error)
{
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "stopped");
}

static gboolean
is_cancelled(AxisJobContext *context)
{
	return context != NULL && axis_job_context_is_cancelled(context);
}

/* Sleeps, but not past a stop; returns whether it was stopped. */
static gboolean
pause_ms(
	AxisJobContext *context,
	guint           ms
){
	if (context != NULL)
		return axis_job_context_wait(context, ms);
	g_usleep((gulong)ms * 1000);
	return FALSE;
}

/* ── Locks ──────────────────────────────────────────────────────── */

static gboolean
volume_lockable(
	gint64   volume_id,
	GError **error
){
	if (volume_id <= 0 || volume_id > G_MAXINT32) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_FAILED,
			"volume %" G_GINT64_FORMAT " cannot be locked", volume_id);
		return FALSE;
	}
	return TRUE;
}

/* Whether a scan of the volume is running: its lock is looked at for an
 * instant, never held, so that probing cannot make a scan fail as busy. */
static gboolean
scan_running(
	AxisDbConn  *conn,
	gint64       volume_id,
	gboolean    *running,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, volume_id);
	const gchar *params[1];
	gboolean got = FALSE;

	*running = FALSE;
	if (!volume_lockable(volume_id, error))
		return FALSE;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock_shared(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")", 1,
	                       params, &result, error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, &got);
	if (got)
		return axis_db_conn_exec(conn, "SELECT pg_advisory_unlock_shared(" AXIS_LIBRARY_VOLUME_LOCK_KEY ")",
		                         1, params, NULL, error);
	*running = TRUE;
	return TRUE;
}

static gboolean
probe_lock_try(
	AxisDbConn  *conn,
	gint64       volume_id,
	gboolean    *got,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, volume_id);
	const gchar *params[1];

	*got = FALSE;
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock(" AXIS_PROBE_VOLUME_LOCK_KEY ")", 1, params,
	                       &result, error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, got);
	return TRUE;
}

static void
probe_lock_release(
	AxisDbConn *conn,
	gint64      volume_id
){
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, volume_id);
	g_autoptr(GError) error = NULL;
	const gchar *params[1];

	params[0] = id;
	/* A connection that broke has lost the lock with its session. */
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_unlock(" AXIS_PROBE_VOLUME_LOCK_KEY ")", 1, params, NULL,
	                       &error))
		g_debug("probe: unlocking volume %" G_GINT64_FORMAT ": %s", volume_id, error->message);
}

/* ── What the tool said, as a result ────────────────────────────── */

/* The last non-empty line of the tool's standard error, cleaned. */
static gchar *
last_error_line(const AxisProbeRunResult *result)
{
	g_autofree gchar *text = NULL;
	gchar **lines;
	gchar *line = NULL;
	gint i;

	if (result->errors->len == 0)
		return NULL;
	text = g_strndup((const gchar *)result->errors->data, result->errors->len);
	lines = g_strsplit_set(text, "\r\n", -1);
	for (i = (gint)g_strv_length(lines) - 1; i >= 0; i--) {
		if (lines[i][0] != '\0') {
			line = axis_library_text_clean(lines[i], 200);
			break;
		}
	}
	g_strfreev(lines);
	return line;
}

static AxisMediaInfo *
failure_info(
	const gchar *status,
	const gchar *text
){
	AxisMediaInfo *info = axis_media_info_new();
	gchar *clean = axis_library_text_clean(text, AXIS_PROBE_ERROR_CHARS);

	info->status = g_strdup(status);
	info->error = clean != NULL && clean[0] != '\0' ? clean : g_strdup("ffprobe could not read the file.");
	if (info->error != clean)
		g_free(clean);
	return info;
}

/* The album a track belongs to: the root, the folder and the album tag. */
static gchar *
album_key(
	const AxisProbeTarget *target,
	const gchar           *album
){
	GChecksum *sum;
	const gchar *slash;
	g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, target->root_id);
	gchar *key;

	if (album == NULL || album[0] == '\0')
		return NULL;
	slash = strrchr(target->rel_path, '/');
	sum = g_checksum_new(G_CHECKSUM_SHA256);
	g_checksum_update(sum, (const guchar *)"axis-album", 11);
	g_checksum_update(sum, (const guchar *)root, (gssize)strlen(root) + 1);
	if (slash != NULL)
		g_checksum_update(sum, (const guchar *)target->rel_path, (gssize)(slash - target->rel_path));
	g_checksum_update(sum, (const guchar *)"", 1);
	g_checksum_update(sum, (const guchar *)album, (gssize)strlen(album));
	key = g_strdup(g_checksum_get_string(sum));
	g_checksum_free(sum);
	return key;
}

/* ── Probing one file ───────────────────────────────────────────── */

typedef struct
{
	AxisProbeTarget target;
	AxisProbeFile   file;
	gboolean        file_open;
	gint64          started_us;
} Probing;

static gboolean
defer_file(
	AxisDbConn       *conn,
	const Probing    *probing,
	const gchar      *reason,
	AxisProbeOutcome *outcome,
	GError          **error
){
	*outcome = AXIS_PROBE_OUTCOME_DEFERRED;
	return axis_probe_store_defer(conn, probing->target.file_id, reason, error);
}

/* Runs the tool for the container and streams.  *@result is filled. */
static void
run_streams(
	AxisProbe          *self,
	const gchar        *program,
	Probing            *probing,
	GCancellable       *cancellable,
	AxisProbeRunResult *result
){
	gchar **argv = axis_probe_build_argv(self, program, FALSE, 0);

	(void)lseek(probing->file.fd, 0, SEEK_SET);
	axis_probe_run_tool(self, argv, probing->file.fd, cancellable, result);
	g_strfreev(argv);
}

/* The model from the tool's first call, or a failure to record.
 * Returns %FALSE with an error when nothing should be recorded. */
static gboolean
interpret(
	AxisProbe           *self,
	const gchar         *program,
	Probing             *probing,
	AxisJobContext      *context,
	AxisProbeRunResult  *first,
	AxisMediaInfo      **info_out,
	GError             **error
){
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *line = NULL;
	GCancellable *cancellable = context != NULL ? axis_job_context_get_cancellable(context) : NULL;
	gint frame_stream = 0;

	*info_out = NULL;
	switch (first->status) {
	case AXIS_PROBE_RUN_SPAWN_FAILED:
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL, "ffprobe could not be started: %s",
			first->message != NULL ? first->message : "unknown error");
		return FALSE;
	case AXIS_PROBE_RUN_UNKILLABLE:
		g_set_error_literal(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_UNAVAILABLE,
			"ffprobe could not be stopped; the disk may not be responding.");
		return FALSE;
	case AXIS_PROBE_RUN_CANCELLED:
		set_cancelled(error);
		return FALSE;
	case AXIS_PROBE_RUN_TIMEOUT: {
		guint ms = axis_probe_get_timeout_ms(self);

		problem = ms % 1000 == 0 ?
			g_strdup_printf("ffprobe did not finish within %u seconds and was stopped.", ms / 1000) :
			g_strdup_printf("ffprobe did not finish within %.1f seconds and was stopped.", ms / 1000.0);
		*info_out = failure_info("timeout", problem);
		return TRUE;
	}
	case AXIS_PROBE_RUN_TOO_MUCH:
		problem = g_strdup_printf("ffprobe wrote more than %u KiB of output and was stopped.",
			(guint)(first->output->len / 1024));
		*info_out = failure_info("failed", problem);
		return TRUE;
	case AXIS_PROBE_RUN_SIGNALLED:
		problem = g_strdup_printf("ffprobe crashed (signal %d) while reading the file.", first->exit_status);
		*info_out = failure_info("failed", problem);
		return TRUE;
	case AXIS_PROBE_RUN_OK:
	case AXIS_PROBE_RUN_EXITED:
	default:
		break;
	}

	if (!axis_probe_map_streams((const gchar *)first->output->data, first->output->len, &info, &problem)) {
		/* If it stopped with an error and printed no usable report,
		 * what it said on its error output is the better
		 * explanation. */
		if (first->status == AXIS_PROBE_RUN_EXITED && strstr(problem, "could not be read (") != NULL) {
			line = last_error_line(first);
			g_free(problem);
			problem = line != NULL ? g_strdup_printf("ffprobe could not read the file: %s", line) :
				g_strdup_printf("ffprobe stopped with status %d.", first->exit_status);
		}
		*info_out = failure_info("failed", problem);
		return TRUE;
	}
	if (first->status == AXIS_PROBE_RUN_EXITED) {
		problem = g_strdup_printf("ffprobe stopped with status %d.", first->exit_status);
		*info_out = failure_info("failed", problem);
		return TRUE;
	}

	/* The second call, when the signalling leaves room for HDR. */
	if (axis_media_info_wants_frames(info, &frame_stream)) {
		AxisProbeRunResult second;
		gchar **argv = axis_probe_build_argv(self, program, TRUE, frame_stream);

		(void)lseek(probing->file.fd, 0, SEEK_SET);
		axis_probe_run_tool(self, argv, probing->file.fd, cancellable, &second);
		g_strfreev(argv);
		switch (second.status) {
		case AXIS_PROBE_RUN_SPAWN_FAILED:
			g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL, "ffprobe could not be started: %s",
				second.message != NULL ? second.message : "unknown error");
			axis_probe_run_result_clear(&second);
			return FALSE;
		case AXIS_PROBE_RUN_UNKILLABLE:
			g_set_error_literal(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_UNAVAILABLE,
				"ffprobe could not be stopped; the disk may not be responding.");
			axis_probe_run_result_clear(&second);
			return FALSE;
		case AXIS_PROBE_RUN_CANCELLED:
			set_cancelled(error);
			axis_probe_run_result_clear(&second);
			return FALSE;
		case AXIS_PROBE_RUN_OK:
			axis_probe_map_frames(info, (const gchar *)second.output->data, second.output->len);
			break;
		default:
			/* Without the frame, the streams that needed it are
			 * uncertain. */
			axis_probe_map_frames(info, NULL, 0);
			break;
		}
		axis_probe_run_result_clear(&second);
	}

	axis_media_info_finish(info, probing->target.size);
	g_free(info->status);
	info->status = g_strdup("ok");
	*info_out = g_steal_pointer(&info);
	return TRUE;
}

/* Everything after the volume's probe lock is held. */
static gboolean
probe_locked(
	AxisProbe        *self,
	AxisDbConn       *conn,
	const gchar      *program,
	Probing          *probing,
	AxisJobContext   *context,
	AxisProbeOutcome *outcome,
	GError          **error
){
	AxisProbeTarget *target = &probing->target;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(AxisMediaInfo) info = NULL;
	g_autofree gchar *version = NULL;
	AxisProbeRunResult first;
	AxisProbeTuning tuning;
	GCancellable *cancellable = context != NULL ? axis_job_context_get_cancellable(context) : NULL;
	GError *local = NULL;
	gboolean stored = FALSE;
	gboolean ok;

	guard = axis_volume_guard_acquire(self->storage, target->volume_id, AXIS_VOLUME_INTENT_READ, &local);
	if (guard == NULL) {
		if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND)) {
			/* The volume was removed meanwhile; the file went with it. */
			g_error_free(local);
			return TRUE;
		}
		propagate_storage_error(error, local);
		return FALSE;
	}

	if (!axis_probe_file_open(guard, target->root_rel_path, target->rel_path, &probing->file, &local)) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
		    g_error_matches(local, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)) {
			ok = defer_file(conn, probing, "The file was not where the index says it is.", outcome, error);
			g_error_free(local);
			return ok;
		}
		propagate_storage_error(error, local);
		return FALSE;
	}
	probing->file_open = TRUE;

	/* Before: is this the file the index describes? */
	if (!axis_probe_stamp_matches(&probing->file.st, target))
		return defer_file(conn, probing, "The file changed after it was scanned.", outcome, error);

	version = axis_probe_tool_version(self, program);
	probing->started_us = g_get_monotonic_time();
	run_streams(self, program, probing, cancellable, &first);
	ok = interpret(self, program, probing, context, &first, &info, error);
	axis_probe_run_result_clear(&first);
	if (!ok)
		return FALSE;

	g_mutex_lock(&self->lock);
	tuning = self->tuning;
	g_mutex_unlock(&self->lock);
	if (tuning.after_run != NULL)
		tuning.after_run(target->file_id, tuning.hook_data);

	/* After: the volume is still there, and the file is still the one. */
	if (!axis_volume_guard_check(guard, &local)) {
		propagate_storage_error(error, local);
		return FALSE;
	}
	if (is_cancelled(context)) {
		set_cancelled(error);
		return FALSE;
	}
	if (!axis_probe_file_unchanged(&probing->file, target))
		return defer_file(conn, probing, "The file changed while it was being probed.", outcome, error);

	info->file_id = target->file_id;
	info->probe_version = axis_probe_current_version(self);
	info->probe_ms = (gint)MIN((g_get_monotonic_time() - probing->started_us) / 1000, (gint64)G_MAXINT32);
	info->tool_version = g_steal_pointer(&version);
	if (g_strcmp0(info->status, "ok") == 0)
		info->album_key = album_key(target, info->tag_album);

	if (tuning.before_store != NULL)
		tuning.before_store(target->file_id, tuning.hook_data);
	if (!axis_probe_store_result(conn, target, info, &stored, error))
		return FALSE;

	if (!stored) {
		/* The index moved on while this ran: whatever it recorded is
		 * for the next probe. */
		*outcome = AXIS_PROBE_OUTCOME_DEFERRED;
		return TRUE;
	}
	if (g_strcmp0(info->status, "ok") == 0) {
		*outcome = AXIS_PROBE_OUTCOME_STORED;
	} else {
		g_autofree gchar *shown = axis_library_name_display(target->rel_path);

		*outcome = g_strcmp0(info->status, "timeout") == 0 ? AXIS_PROBE_OUTCOME_TIMEOUT :
			AXIS_PROBE_OUTCOME_FAILED;
		g_info("probe: %s in root %" G_GINT64_FORMAT ": %s", shown, target->root_id, info->error);
	}
	return TRUE;
}

gboolean
axis_probe_file(
	AxisProbe         *self,
	gint64             file_id,
	AxisJobContext    *context,
	AxisProbeOutcome  *outcome,
	GError           **error
){
	Probing probing;
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autofree gchar *program = NULL;
	AxisDbConn *conn;
	gboolean found = FALSE;
	gboolean running = FALSE;
	gboolean locked = FALSE;
	gboolean ok = FALSE;
	gint64 locked_volume = 0;
	gint64 deadline;
	guint wait_ms;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(outcome != NULL, FALSE);

	*outcome = AXIS_PROBE_OUTCOME_NOTHING;
	memset(&probing, 0, sizeof(probing));
	probing.file.fd = -1;
	probing.file.dir_fd = -1;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	if (!axis_probe_store_load_target(conn, file_id, &probing.target, &found, error))
		goto out;
	if (!found || !axis_probe_target_pending(&probing.target, axis_probe_current_version(self))) {
		ok = TRUE;
		goto out;
	}
	if (!volume_lockable(probing.target.volume_id, error))
		goto out;

	volume = axis_storage_get_volume(self->storage, probing.target.volume_id);
	if (volume == NULL) {
		ok = TRUE;
		goto out;
	}
	if (!axis_volume_state_is_usable(volume->state)) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_UNAVAILABLE,
			"Disk '%s' is not online: %s", volume->name, axis_volume_reason_describe(volume->reason));
		goto out;
	}

	program = axis_probe_resolve_tool(self);
	if (program == NULL) {
		g_set_error_literal(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL,
			"ffprobe was not found; install ffmpeg or set the ffprobe program in the settings.");
		goto out;
	}

	if (!scan_running(conn, probing.target.volume_id, &running, error))
		goto out;
	if (running) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_BUSY,
			"Disk '%s' is being scanned; reading waits until the scan ends.", volume->name);
		goto out;
	}

	/* One probe of a volume at a time (the disk is read by one reader). */
	g_mutex_lock(&self->lock);
	wait_ms = self->tuning.lock_wait_ms > 0 ? self->tuning.lock_wait_ms : AXIS_PROBE_LOCK_WAIT_MS;
	g_mutex_unlock(&self->lock);
	deadline = g_get_monotonic_time() + (gint64)wait_ms * 1000;
	for (;;) {
		if (!probe_lock_try(conn, probing.target.volume_id, &locked, error))
			goto out;
		if (locked) {
			locked_volume = probing.target.volume_id;
			break;
		}
		if (is_cancelled(context)) {
			set_cancelled(error);
			goto out;
		}
		if (g_get_monotonic_time() >= deadline) {
			g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_BUSY,
				"Axis is still reading another file on disk '%s'.", volume->name);
			goto out;
		}
		pause_ms(context, 200);
	}

	/* Time has passed (the lock may have been waited for): the file is
	 * looked at again, so that a change is never probed twice. */
	axis_probe_target_clear(&probing.target);
	if (!axis_probe_store_load_target(conn, file_id, &probing.target, &found, error))
		goto out;
	if (!found || !axis_probe_target_pending(&probing.target, axis_probe_current_version(self))) {
		ok = TRUE;
		goto out;
	}

	ok = probe_locked(self, conn, program, &probing, context, outcome, error);

out:
	if (locked)
		probe_lock_release(conn, locked_volume);
	if (probing.file_open)
		axis_probe_file_close(&probing.file);
	axis_probe_target_clear(&probing.target);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_probe_inspect_fd(
	AxisProbe       *self,
	gint             fd,
	gint64           size,
	AxisMediaInfo  **info,
	GError         **error
){
	g_autofree gchar *program = NULL;
	AxisProbeRunResult first;
	Probing probing;
	gboolean ok;

	g_return_val_if_fail(self != NULL && fd >= 0 && info != NULL, FALSE);

	*info = NULL;
	program = axis_probe_resolve_tool(self);
	if (program == NULL) {
		g_set_error_literal(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL,
			"ffprobe was not found; install ffmpeg or set the ffprobe program in the settings.");
		return FALSE;
	}
	memset(&probing, 0, sizeof(probing));
	probing.file.fd = fd;
	probing.file.dir_fd = -1;
	probing.target.size = size;
	probing.started_us = g_get_monotonic_time();
	run_streams(self, program, &probing, NULL, &first);
	ok = interpret(self, program, &probing, NULL, &first, info, error);
	axis_probe_run_result_clear(&first);
	if (ok && *info != NULL)
		(*info)->probe_ms = (gint)MIN((g_get_monotonic_time() - probing.started_us) / 1000, (gint64)G_MAXINT32);
	return ok;
}

/* ── The jobs ───────────────────────────────────────────────────── */

gboolean
axis_probe_run_file_job(
	AxisProbe       *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	JsonNode *node = json_object_get_member(payload, "file_id");
	AxisProbeOutcome outcome = AXIS_PROBE_OUTCOME_NOTHING;
	GError *local = NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64 ||
	    json_node_get_int(node) <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT,
			"the payload needs file_id, a positive whole number");
		return FALSE;
	}

	if (!axis_probe_file(self, json_node_get_int(node), context, &outcome, &local)) {
		if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_clear_error(&local);
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "stopped");
			return FALSE;
		}
		/* A missing tool is the installation's problem: retrying
		 * does not help, and the schedule does not enqueue meanwhile. */
		if (g_error_matches(local, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_TOOL)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, local->message);
			g_clear_error(&local);
			return FALSE;
		}
		g_propagate_error(error, local);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_probe_run_due_job(
	AxisProbe       *self,
	AxisJobContext  *context,
	GError         **error
){
	return axis_probe_enqueue_due(self, FALSE, NULL, error);
}

gboolean
axis_probe_run_stale_job(
	AxisProbe       *self,
	AxisJobContext  *context,
	GError         **error
){
	return axis_probe_enqueue_due(self, TRUE, NULL, error);
}

/* ── Enqueueing ─────────────────────────────────────────────────── */

static gboolean
enqueue_file(
	AxisDbConn  *conn,
	gint64       file_id,
	gint         priority,
	gint64      *job_id,
	gboolean    *created,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *dedupe = g_strdup_printf(AXIS_PROBE_FILE_KIND ":%" G_GINT64_FORMAT, file_id);
	AxisJobSpec spec;

	json_object_set_int_member(payload, "file_id", file_id);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_PROBE_FILE_KIND;
	spec.queue = AXIS_PROBE_QUEUE;
	spec.payload = payload;
	spec.priority = priority;
	spec.dedupe_key = dedupe;
	spec.max_attempts = AXIS_PROBE_FILE_ATTEMPTS;

	return axis_job_enqueue(conn, &spec, job_id, created, error);
}

gboolean
axis_probe_enqueue_due(
	AxisProbe  *self,
	gboolean    stale,
	guint      *n_enqueued,
	GError    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GString) list = g_string_new("{");
	g_autofree gchar *program = NULL;
	g_autofree gchar *limit = NULL;
	g_autofree gchar *version = NULL;
	const gchar *params[3];
	AxisDbConn *conn;
	gint64 unfinished = 0;
	gint64 room;
	guint n = 0;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (n_enqueued != NULL)
		*n_enqueued = 0;

	program = axis_probe_resolve_tool(self);
	if (program == NULL) {
		gboolean warn;

		g_mutex_lock(&self->lock);
		warn = !self->tool_missing_logged;
		self->tool_missing_logged = TRUE;
		g_mutex_unlock(&self->lock);
		if (warn)
			g_warning("probe: ffprobe was not found; no media file is probed until it is installed "
				"or the ffprobe program is set in the settings");
		return TRUE;
	}
	g_mutex_lock(&self->lock);
	self->tool_missing_logged = FALSE;
	g_mutex_unlock(&self->lock);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	/* The queue is kept topped up, not flooded. */
	if (!axis_db_conn_exec(conn, "SELECT count(*) FROM job WHERE kind = '" AXIS_PROBE_FILE_KIND "'"
	                       " AND state IN ('queued', 'running')", 0, NULL, &result, error))
		goto fail;
	axis_db_result_get_int64(result, 0, 0, &unfinished);
	g_clear_pointer(&result, axis_db_result_free);
	room = AXIS_PROBE_QUEUE_TARGET - unfinished;
	if (room <= 0)
		goto done;

	/* Volumes that are online and not being scanned. */
	volumes = axis_storage_list_volumes(self->storage);
	for (i = 0; i < volumes->len; i++) {
		const AxisVolumeInfo *volume = g_ptr_array_index(volumes, i);
		gboolean running = FALSE;

		if (!axis_volume_state_is_usable(volume->state))
			continue;
		if (!scan_running(conn, volume->id, &running, error))
			goto fail;
		if (running)
			continue;
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, list->len > 1 ? "," : "", volume->id);
	}
	g_string_append_c(list, '}');
	if (list->len == 2)
		goto done;

	limit = g_strdup_printf("%" G_GINT64_FORMAT, room);
	version = g_strdup_printf("%d", axis_probe_current_version(self));
	params[0] = list->str;
	params[1] = limit;
	params[2] = version;
	/* A volume at a time in turn, so that a long run of one disk's files
	 * does not keep the others waiting.  A file whose probe was put off
	 * waits out its delay.  ($3 is the version, which only the second
	 * statement needs; the first mentions it so that both take the same
	 * three parameters.) */
	if (!axis_db_conn_exec(conn,
	                       stale ?
	                       "SELECT id FROM ("
	                       " SELECT f.id, f.volume_id, row_number() OVER (PARTITION BY f.volume_id ORDER BY f.id) AS rn"
	                       " FROM library_file f JOIN media_info m ON m.file_id = f.id"
	                       " WHERE f.kind IN ('video', 'audio') AND f.state = 'present'"
	                       " AND f.volume_id = ANY ($1::bigint[]) AND m.probe_version < $3::integer"
	                       " AND NOT EXISTS (SELECT 1 FROM media_probe_defer d WHERE d.file_id = f.id"
	                       "   AND d.until > now())) x"
	                       " ORDER BY rn, volume_id, id LIMIT $2::bigint" :
	                       "SELECT id FROM ("
	                       " SELECT f.id, f.volume_id, row_number() OVER (PARTITION BY f.volume_id ORDER BY f.id) AS rn"
	                       " FROM library_file f JOIN library_root_state s ON s.root_id = f.root_id"
	                       " WHERE f.kind IN ('video', 'audio') AND f.state = 'present'"
	                       " AND f.volume_id = ANY ($1::bigint[]) AND f.changed"
	                       " AND f.changed_generation <= s.complete_generation"
	                       " AND NOT EXISTS (SELECT 1 FROM media_probe_defer d WHERE d.file_id = f.id"
	                       "   AND d.until > now())"
	                       " AND $3::integer > 0) x"
	                       " ORDER BY rn, volume_id, id LIMIT $2::bigint",
	                       3, params, &result, error))
		goto fail;

	if (axis_db_result_get_n_rows(result) > 0) {
		if (!axis_db_conn_begin(conn, error))
			goto fail;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			gint64 file_id = 0;
			gboolean created = FALSE;

			axis_db_result_get_int64(result, i, 0, &file_id);
			if (!enqueue_file(conn, file_id, stale ? AXIS_PROBE_STALE_PRIORITY : 0, NULL, &created, error)) {
				axis_db_conn_rollback(conn, NULL);
				goto fail;
			}
			if (created)
				n++;
		}
		if (!axis_db_conn_commit(conn, error)) {
			axis_db_conn_rollback(conn, NULL);
			goto fail;
		}
	}

done:
	axis_db_pool_release(self->pool, conn);
	if (n_enqueued != NULL)
		*n_enqueued = n;
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

/* ── Requests ───────────────────────────────────────────────────── */

gboolean
axis_probe_request_file(
	AxisProbe  *self,
	gint64      file_id,
	gint64     *job_id,
	GError    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file_id);
	const gchar *params[1];
	AxisDbConn *conn;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	if (!axis_db_conn_begin(conn, error))
		goto fail;
	if (!axis_db_conn_exec(conn,
	                       "UPDATE library_file SET changed = true WHERE id = $1::bigint"
	                       " AND kind IN ('video', 'audio') AND state = 'present'",
	                       1, params, &result, error))
		goto rollback;
	if (axis_db_result_get_affected_rows(result) != 1) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND,
			"there is no video or audio file %" G_GINT64_FORMAT, file_id);
		goto rollback;
	}
	if (!axis_db_conn_exec(conn, "DELETE FROM media_probe_defer WHERE file_id = $1::bigint", 1, params, NULL,
	                       error))
		goto rollback;
	if (!enqueue_file(conn, file_id, AXIS_PROBE_REQUEST_PRIORITY, job_id, NULL, error))
		goto rollback;
	if (!axis_db_conn_commit(conn, error))
		goto rollback;

	axis_db_pool_release(self->pool, conn);
	return TRUE;

rollback:
	axis_db_conn_rollback(conn, NULL);
fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

/* The first characters of a path the prefix index holds (the scanner's). */
#define PREFIX_CHARS 600

gboolean
axis_probe_request_folder(
	AxisProbe  *self,
	gint64      dir_id,
	guint      *n_requested,
	gboolean   *capped,
	GError    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) list = g_string_new("{");
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, dir_id);
	g_autofree gchar *root = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *low = NULL;
	g_autofree gchar *high = NULL;
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_PROBE_REQUEST_CAP + 1);
	const gchar *params[5];
	AxisDbConn *conn;
	guint count;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (n_requested != NULL)
		*n_requested = 0;
	if (capped != NULL)
		*capped = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT root_id, path FROM library_dir WHERE id = $1::bigint", 1, params,
	                       &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_PROBE_ERROR, AXIS_PROBE_ERROR_NOT_FOUND,
			"there is no folder %" G_GINT64_FORMAT, dir_id);
		goto fail;
	}
	root = g_strdup(axis_db_result_get_text(result, 0, 0));
	path = g_strdup(axis_db_result_get_text(result, 0, 1));
	g_clear_pointer(&result, axis_db_result_free);

	if (!axis_db_conn_begin(conn, error))
		goto fail;
	params[0] = root;
	params[1] = limit;
	if (path[0] == '\0') {
		/* The whole root. */
		if (!axis_db_conn_exec(conn,
		                       "SELECT f.id FROM library_file f WHERE f.root_id = $1::bigint"
		                       " AND f.state = 'present' AND f.kind IN ('video', 'audio')"
		                       " ORDER BY left(f.rel_path, 600), f.id LIMIT $2::integer",
		                       2, params, &result, error))
			goto rollback;
	} else {
		/* Everything that starts with "path/": a range of the prefix index
		 * (as the scanner uses it), rechecked in full. */
		prefix = g_strconcat(path, "/", NULL);
		if (g_utf8_strlen(prefix, -1) < PREFIX_CHARS) {
			low = g_strdup(prefix);
			high = g_strconcat(path, "0", NULL);
		} else {
			const gchar *end = g_utf8_offset_to_pointer(prefix, PREFIX_CHARS);

			low = g_strndup(prefix, (gsize)(end - prefix));
			high = g_strconcat(low, "\xf4\x8f\xbf\xbf", NULL);
		}
		params[2] = low;
		params[3] = high;
		params[4] = prefix;
		if (!axis_db_conn_exec(conn,
		                       "SELECT f.id FROM library_file f WHERE f.root_id = $1::bigint"
		                       " AND left(f.rel_path, 600) >= $3::text AND left(f.rel_path, 600) < $4::text"
		                       " AND starts_with(f.rel_path, $5::text)"
		                       " AND f.state = 'present' AND f.kind IN ('video', 'audio')"
		                       " ORDER BY left(f.rel_path, 600), f.id LIMIT $2::integer",
		                       5, params, &result, error))
			goto rollback;
	}
	count = axis_db_result_get_n_rows(result);
	if (count > AXIS_PROBE_REQUEST_CAP) {
		count = AXIS_PROBE_REQUEST_CAP;
		if (capped != NULL)
			*capped = TRUE;
	}
	for (i = 0; i < count; i++) {
		gint64 file_id = 0;

		axis_db_result_get_int64(result, i, 0, &file_id);
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", file_id);
	}
	g_string_append_c(list, '}');
	if (count > 0) {
		const gchar *list_params[1];

		list_params[0] = list->str;
		if (!axis_db_conn_exec(conn, "UPDATE library_file SET changed = true WHERE id = ANY ($1::bigint[])", 1,
		                       list_params, NULL, error) ||
		    !axis_db_conn_exec(conn, "DELETE FROM media_probe_defer WHERE file_id = ANY ($1::bigint[])", 1,
		                       list_params, NULL, error))
			goto rollback;
	}
	for (i = 0; i < count; i++) {
		gint64 file_id = 0;

		axis_db_result_get_int64(result, i, 0, &file_id);
		if (!enqueue_file(conn, file_id, AXIS_PROBE_REQUEST_PRIORITY, NULL, NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;

	axis_db_pool_release(self->pool, conn);
	if (n_requested != NULL)
		*n_requested = count;
	return TRUE;

rollback:
	axis_db_conn_rollback(conn, NULL);
fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

gboolean
axis_probe_request_failed(
	AxisProbe  *self,
	gint64      root_id,
	guint      *n_requested,
	gboolean   *capped,
	GError    **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) list = g_string_new("{");
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, root_id);
	g_autofree gchar *limit = g_strdup_printf("%d", AXIS_PROBE_REQUEST_CAP + 1);
	const gchar *params[2];
	AxisDbConn *conn;
	guint count;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (n_requested != NULL)
		*n_requested = 0;
	if (capped != NULL)
		*capped = FALSE;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	params[1] = limit;
	if (!axis_db_conn_begin(conn, error))
		goto fail;
	/* The failures of the root, one more than the cap, to know whether
	 * there were more. */
	if (!axis_db_conn_exec(conn,
	                       "SELECT f.id FROM library_file f JOIN media_info m ON m.file_id = f.id"
	                       " WHERE f.root_id = $1::bigint AND f.state = 'present' AND m.status <> 'ok'"
	                       " AND f.kind IN ('video', 'audio') ORDER BY f.id LIMIT $2::integer",
	                       2, params, &result, error))
		goto rollback;
	count = axis_db_result_get_n_rows(result);
	if (count > AXIS_PROBE_REQUEST_CAP) {
		count = AXIS_PROBE_REQUEST_CAP;
		if (capped != NULL)
			*capped = TRUE;
	}
	for (i = 0; i < count; i++) {
		gint64 file_id = 0;

		axis_db_result_get_int64(result, i, 0, &file_id);
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", file_id);
	}
	g_string_append_c(list, '}');
	if (count > 0) {
		const gchar *list_params[1];

		list_params[0] = list->str;
		if (!axis_db_conn_exec(conn, "UPDATE library_file SET changed = true WHERE id = ANY ($1::bigint[])", 1,
		                       list_params, NULL, error) ||
		    !axis_db_conn_exec(conn, "DELETE FROM media_probe_defer WHERE file_id = ANY ($1::bigint[])", 1,
		                       list_params, NULL, error))
			goto rollback;
	}
	for (i = 0; i < count; i++) {
		gint64 file_id = 0;

		axis_db_result_get_int64(result, i, 0, &file_id);
		if (!enqueue_file(conn, file_id, AXIS_PROBE_REQUEST_PRIORITY, NULL, NULL, error))
			goto rollback;
	}
	if (!axis_db_conn_commit(conn, error))
		goto rollback;

	axis_db_pool_release(self->pool, conn);
	if (n_requested != NULL)
		*n_requested = count;
	return TRUE;

rollback:
	axis_db_conn_rollback(conn, NULL);
fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}
