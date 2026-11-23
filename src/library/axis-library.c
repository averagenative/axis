/*
 * axis-library.c - the library: its jobs, its roots' settings, and what
 * the pages read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "library/axis-library-private.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"

G_DEFINE_QUARK(axis-library-error-quark, axis_library_error)

/* ── Names ──────────────────────────────────────────────────────── */

static const gchar *const status_names[] = { "running", "complete", "incomplete", "unavailable" };

static const gchar *const reason_names[] = {
	NULL, "cancelled", "shutdown", "volume_unavailable", "io_errors", "root_missing", "root_changed",
	"root_empty", "stalled", "interrupted", "failed", "busy"
};

static const gchar *const reason_sentences[] = {
	"",
	"The scan was cancelled.",
	"Axis was shutting down.",
	"The disk was offline.",
	"Some folders or files could not be read because of errors that may mean the disk is failing.",
	"The library folder was not there.",
	"The library folder was replaced, or is now reached through a link or another filesystem.",
	"The library folder was empty while Axis had files on record in it.",
	"The filesystem stopped answering.",
	"The scan stopped without finishing (Axis was stopped or killed).",
	"The scan failed.",
	"Another scan of this disk was running."
};

G_STATIC_ASSERT(G_N_ELEMENTS(reason_names) == AXIS_SCAN_REASON_BUSY + 1);
G_STATIC_ASSERT(G_N_ELEMENTS(reason_sentences) == G_N_ELEMENTS(reason_names));
G_STATIC_ASSERT(G_N_ELEMENTS(status_names) == AXIS_SCAN_UNAVAILABLE + 1);

const gchar *
axis_scan_status_to_string(AxisScanStatus status)
{
	return (guint)status < G_N_ELEMENTS(status_names) ? status_names[status] : "incomplete";
}

gboolean
axis_scan_status_from_string(
	const gchar    *text,
	AxisScanStatus *status
){
	guint i;

	for (i = 0; text != NULL && i < G_N_ELEMENTS(status_names); i++) {
		if (strcmp(text, status_names[i]) == 0) {
			*status = (AxisScanStatus)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_scan_reason_to_string(AxisScanReason reason)
{
	return (guint)reason < G_N_ELEMENTS(reason_names) ? reason_names[reason] : "failed";
}

gboolean
axis_scan_reason_from_string(
	const gchar    *text,
	AxisScanReason *reason
){
	guint i;

	if (text == NULL) {
		*reason = AXIS_SCAN_REASON_NONE;
		return TRUE;
	}
	for (i = 1; i < G_N_ELEMENTS(reason_names); i++) {
		if (strcmp(text, reason_names[i]) == 0) {
			*reason = (AxisScanReason)i;
			return TRUE;
		}
	}
	return FALSE;
}

const gchar *
axis_scan_reason_describe(AxisScanReason reason)
{
	return (guint)reason < G_N_ELEMENTS(reason_sentences) ? reason_sentences[reason] :
		reason_sentences[AXIS_SCAN_REASON_FAILED];
}

void
axis_scan_result_free(AxisScanResult *result)
{
	if (result == NULL)
		return;

	g_free(result->message);
	g_free(result);
}

gint64
axis_library_boottime_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
		return g_get_monotonic_time();
	return (gint64)ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
}

/* This boot of the machine, so that boot clock readings are compared only
 * within one; NULL when it cannot be read. */
static gchar *
read_boot_id(void)
{
	gchar *text = NULL;

	if (!g_file_get_contents("/proc/sys/kernel/random/boot_id", &text, NULL, NULL))
		return NULL;
	g_strstrip(text);
	if (strlen(text) != 36 || strspn(text, "0123456789abcdef-") != 36) {
		g_free(text);
		return NULL;
	}
	return text;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisLibrary *
axis_library_new(
	AxisDbPool  *pool,
	AxisStorage *storage
){
	AxisLibrary *self;

	g_return_val_if_fail(pool != NULL, NULL);
	g_return_val_if_fail(storage != NULL, NULL);

	self = g_new0(AxisLibrary, 1);
	self->ref_count = 1;
	self->pool = pool;
	self->storage = axis_storage_ref(storage);
	self->boot_id = read_boot_id();
	self->tuning.racy_window_ns = -1;
	g_mutex_init(&self->lock);
	self->abandoned = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)axis_library_walk_unref);

	return self;
}

AxisLibrary *
axis_library_ref(AxisLibrary *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_library_unref(AxisLibrary *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_hash_table_unref(self->abandoned);
	axis_storage_unref(self->storage);
	g_free(self->boot_id);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_library_set_tuning(
	AxisLibrary             *self,
	const AxisLibraryTuning *tuning
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	if (tuning != NULL) {
		self->tuning = *tuning;
	} else {
		memset(&self->tuning, 0, sizeof(self->tuning));
		self->tuning.racy_window_ns = -1;
	}
	g_mutex_unlock(&self->lock);
}

guint
axis_library_get_abandoned_walks(AxisLibrary *self)
{
	GHashTableIter iter;
	gpointer value;
	guint n = 0;

	g_return_val_if_fail(self != NULL, 0);

	g_mutex_lock(&self->lock);
	g_hash_table_iter_init(&iter, self->abandoned);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		if (!axis_library_walk_wait(value, 0))
			n++;
	}
	g_mutex_unlock(&self->lock);

	return n;
}

/* ── Due and requested ──────────────────────────────────────────── */

/* Whether root r (with its state s and last scan l) is due for a scan. */
#define DUE_SQL \
	"(l.id IS NULL" \
	" OR now() >= l.started_at + make_interval(secs => coalesce(s.scan_interval_seconds, 21600))" \
	" OR (l.status = 'unavailable' AND now() >= l.started_at" \
	"   + make_interval(secs => least(coalesce(s.scan_interval_seconds, 21600), 300)))" \
	" OR (l.status IN ('incomplete', 'running') AND now() >= l.started_at" \
	"   + make_interval(secs => least(coalesce(s.scan_interval_seconds, 21600), 3600))))"

G_STATIC_ASSERT(AXIS_LIBRARY_DEFAULT_INTERVAL_SECONDS == 21600);
G_STATIC_ASSERT(AXIS_LIBRARY_UNAVAILABLE_RETRY_SECONDS == 300);
G_STATIC_ASSERT(AXIS_LIBRARY_RETRY_SECONDS == 3600);

static gboolean
enqueue_volume(
	AxisDbConn  *conn,
	gint64       volume_id,
	gint         priority,
	gint64      *id,
	gboolean    *created,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *dedupe = g_strdup_printf(AXIS_LIBRARY_SCAN_KIND ":volume:%" G_GINT64_FORMAT,
		volume_id);
	AxisJobSpec spec;

	json_object_set_int_member(payload, "volume_id", volume_id);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_LIBRARY_SCAN_KIND;
	spec.queue = AXIS_LIBRARY_SCAN_QUEUE;
	spec.payload = payload;
	spec.priority = priority;
	spec.dedupe_key = dedupe;
	spec.max_attempts = AXIS_LIBRARY_SCAN_ATTEMPTS;

	return axis_job_enqueue(conn, &spec, id, created, error);
}

gboolean
axis_library_request_scan(
	AxisLibrary  *self,
	gint64        root_id,
	gint64       *job_id,
	GError      **error
){
	g_autoptr(AxisRootInfo) root = NULL;
	g_autofree gchar *id = axis_library_int_text(root_id);
	const gchar *params[1];
	AxisDbConn *conn;
	GError *local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
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

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	if (!axis_db_conn_begin(conn, error) ||
	    !axis_db_conn_exec(conn, "INSERT INTO library_root_state (root_id, scan_requested_at)"
	                       " VALUES ($1::bigint, now()) ON CONFLICT (root_id) DO UPDATE"
	                       " SET scan_requested_at = now(), updated_at = now()",
	                       1, params, NULL, error) ||
	    !enqueue_volume(conn, root->volume_id, AXIS_LIBRARY_REQUEST_PRIORITY, job_id, NULL, error) ||
	    !axis_db_conn_commit(conn, error)) {
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	axis_db_pool_release(self->pool, conn);

	return TRUE;
}

gboolean
axis_library_enqueue_due(
	AxisLibrary  *self,
	guint        *n_enqueued,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	AxisDbConn *conn;
	guint n = 0;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (n_enqueued != NULL)
		*n_enqueued = 0;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	if (!axis_db_conn_exec(conn,
	                       "SELECT r.volume_id, bool_or(s.scan_requested_at IS NOT NULL)"
	                       " FROM library_root r"
	                       " LEFT JOIN library_root_state s ON s.root_id = r.id"
	                       " LEFT JOIN library_scan l ON l.id = s.last_scan_id"
	                       " WHERE s.scan_requested_at IS NOT NULL OR " DUE_SQL
	                       " GROUP BY r.volume_id ORDER BY r.volume_id",
	                       0, NULL, &result, error))
		goto fail;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		g_autoptr(AxisVolumeInfo) volume = NULL;
		gint64 volume_id = 0;
		gboolean requested = FALSE;
		gboolean created = FALSE;

		axis_db_result_get_int64(result, i, 0, &volume_id);
		axis_db_result_get_boolean(result, i, 1, &requested);
		volume = axis_storage_get_volume(self->storage, volume_id);
		if (volume == NULL)
			continue;
		/* A due scan waits for its volume; a request is answered either
		 * way (with "unavailable" if it must). */
		if (!requested && !axis_volume_state_is_usable(volume->state))
			continue;
		if (!enqueue_volume(conn, volume_id, requested ? AXIS_LIBRARY_REQUEST_PRIORITY : 0, NULL,
		                    &created, error))
			goto fail;
		if (created)
			n++;
	}
	axis_db_pool_release(self->pool, conn);

	if (n_enqueued != NULL)
		*n_enqueued = n;
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

/* The next root of @volume_id to scan in this run: requested ones first,
 * then (with @due) due ones, never one in @done; 0 for none. */
static gboolean
next_root(
	AxisLibrary  *self,
	gint64        volume_id,
	GArray       *done,
	gboolean      due,
	gint64       *root_id,
	gboolean     *requested,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) skip = g_string_new("{");
	g_autofree gchar *volume = axis_library_int_text(volume_id);
	const gchar *params[3];
	guint i;

	*root_id = 0;
	*requested = FALSE;
	for (i = 0; i < done->len; i++)
		g_string_append_printf(skip, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			g_array_index(done, gint64, i));
	g_string_append_c(skip, '}');

	params[0] = volume;
	params[1] = skip->str;
	params[2] = due ? "true" : "false";
	if (!axis_db_pool_exec(self->pool,
	                       "SELECT r.id, s.scan_requested_at IS NOT NULL"
	                       " FROM library_root r"
	                       " LEFT JOIN library_root_state s ON s.root_id = r.id"
	                       " LEFT JOIN library_scan l ON l.id = s.last_scan_id"
	                       " WHERE r.volume_id = $1::bigint AND NOT (r.id = ANY ($2::bigint[]))"
	                       "   AND (s.scan_requested_at IS NOT NULL OR ($3::boolean AND " DUE_SQL "))"
	                       " ORDER BY (s.scan_requested_at IS NOT NULL) DESC, l.started_at NULLS FIRST, r.id"
	                       " LIMIT 1",
	                       3, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) == 1) {
		axis_db_result_get_int64(result, 0, 0, root_id);
		axis_db_result_get_boolean(result, 0, 1, requested);
	}
	return TRUE;
}

/* ── The jobs ───────────────────────────────────────────────────── */

static gboolean
job_is_shutdown(gpointer data)
{
	return axis_job_context_get_stop_reason(data) == AXIS_JOB_STOP_SHUTDOWN;
}

static void
job_progress(
	gdouble      fraction,
	const gchar *message,
	gpointer     data
){
	axis_job_context_set_progress(data, fraction, message);
}

static void
log_result(
	const AxisRootInfo   *root,
	const AxisScanResult *result
){
	switch (result->status) {
	case AXIS_SCAN_COMPLETE:
		g_info("library: root '%s' (id %" G_GINT64_FORMAT ") scanned: %" G_GUINT64_FORMAT " files and %"
			G_GUINT64_FORMAT " links, of which %" G_GUINT64_FORMAT " new, %" G_GUINT64_FORMAT
			" changed, %" G_GUINT64_FORMAT " moved, %" G_GUINT64_FORMAT " back; %" G_GUINT64_FORMAT
			" not found (suspected), %" G_GUINT64_FORMAT " now missing, %" G_GUINT64_FORMAT
			" records removed", root->name, root->id, result->walk.files, result->walk.symlinks,
			result->new_files, result->changed_files, result->moved, result->reappeared,
			result->suspected, result->confirmed, result->removed);
		break;
	case AXIS_SCAN_UNAVAILABLE:
	case AXIS_SCAN_INCOMPLETE:
	default:
		g_message("library: the scan of root '%s' (id %" G_GINT64_FORMAT ") did not complete: %s",
			root->name, root->id, result->message != NULL ? result->message :
			axis_scan_reason_describe(result->reason));
		break;
	}
}

gboolean
axis_library_run_scan_job(
	AxisLibrary     *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	JsonNode *node = json_object_get_member(payload, "volume_id");
	g_autoptr(GArray) done = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) problems = g_string_new(NULL);
	g_autoptr(AxisVolumeInfo) volume = NULL;
	g_autofree gchar *unavailable = NULL;
	AxisScanOptions options;
	gint64 volume_id;
	gboolean due = TRUE;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64 ||
	    json_node_get_int(node) <= 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT,
			"the payload needs volume_id, a positive whole number");
		return FALSE;
	}
	volume_id = json_node_get_int(node);
	volume = axis_storage_get_volume(self->storage, volume_id);
	if (volume == NULL)
		return TRUE;	/* removed meanwhile: nothing to scan */

	memset(&options, 0, sizeof(options));
	options.job_id = axis_job_context_get_id(context);
	options.cancellable = axis_job_context_get_cancellable(context);
	options.is_shutdown = job_is_shutdown;
	options.progress = job_progress;
	options.data = context;

	for (;;) {
		g_autoptr(AxisScanResult) result = NULL;
		g_autoptr(AxisRootInfo) root = NULL;
		GError *local = NULL;
		gint64 root_id = 0;
		gboolean requested = FALSE;

		if (axis_job_context_is_cancelled(context)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "stopped");
			return FALSE;
		}
		if (!next_root(self, volume_id, done, due, &root_id, &requested, error))
			return FALSE;
		if (root_id == 0)
			break;
		g_array_append_val(done, root_id);

		options.trigger = requested ? "request" : "schedule";
		if (!axis_library_scan_root(self, root_id, &options, &result, &local)) {
			if (g_error_matches(local, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_NOT_FOUND)) {
				g_error_free(local);
				continue;
			}
			g_propagate_error(error, local);
			return FALSE;
		}
		if (axis_storage_get_root(self->storage, root_id, &root, NULL))
			log_result(root, result);

		switch (result->status) {
		case AXIS_SCAN_COMPLETE:
			break;
		case AXIS_SCAN_UNAVAILABLE:
			/* Answer the other requests (they find the same), skip the
			 * due ones: they wait for the volume. */
			due = FALSE;
			if (unavailable == NULL)
				unavailable = g_strdup(result->message);
			break;
		case AXIS_SCAN_INCOMPLETE:
		default:
			if (result->reason == AXIS_SCAN_REASON_CANCELLED ||
			    result->reason == AXIS_SCAN_REASON_SHUTDOWN) {
				g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "stopped");
				return FALSE;
			}
			g_string_append_printf(problems, "%s%s: %s", problems->len > 0 ? "; " : "",
				root != NULL ? root->name : "a root", axis_scan_reason_describe(result->reason));
			break;
		}
	}

	/* Retrying at once would only find the same: the schedule tries again
	 * later, and each root's page says what happened. */
	if (unavailable != NULL) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "%s", unavailable);
		return FALSE;
	}
	if (problems->len > 0) {
		g_set_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "Not every scan completed: %s",
			problems->str);
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_library_run_due_job(
	AxisLibrary     *self,
	AxisJobContext  *context,
	GError         **error
){
	return axis_library_enqueue_due(self, NULL, error);
}

/* ── A root's settings ──────────────────────────────────────────── */

gboolean
axis_library_set_root_config(
	AxisLibrary         *self,
	gint64               root_id,
	guint                interval_seconds,
	const gchar *const  *patterns,
	GError             **error
){
	g_autoptr(AxisRootInfo) root = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GPtrArray) kept = g_ptr_array_new();
	g_autoptr(JsonArray) array = json_array_new();
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *id = axis_library_int_text(root_id);
	g_autofree gchar *interval = g_strdup_printf("%u", interval_seconds);
	g_autofree gchar *json = NULL;
	const gchar *params[3];
	GError *local = NULL;
	guint i;
	guint j;

	g_return_val_if_fail(self != NULL, FALSE);

	if (interval_seconds < AXIS_LIBRARY_MIN_INTERVAL_SECONDS ||
	    interval_seconds > AXIS_LIBRARY_MAX_INTERVAL_SECONDS) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_INVALID,
			"The interval must be from %d to %d hours.", AXIS_LIBRARY_MIN_INTERVAL_SECONDS / 3600,
			AXIS_LIBRARY_MAX_INTERVAL_SECONDS / 3600);
		return FALSE;
	}
	for (i = 0; patterns != NULL && patterns[i] != NULL; i++) {
		const gchar *problem = NULL;
		gboolean duplicate = FALSE;

		if (!axis_library_pattern_check(patterns[i], &problem)) {
			g_set_error_literal(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_INVALID, problem);
			return FALSE;
		}
		for (j = 0; j < kept->len; j++)
			duplicate = duplicate || strcmp(g_ptr_array_index(kept, j), patterns[i]) == 0;
		if (!duplicate)
			g_ptr_array_add(kept, (gpointer)patterns[i]);
	}
	if (kept->len > AXIS_LIBRARY_MAX_PATTERNS) {
		g_set_error(error, AXIS_LIBRARY_ERROR, AXIS_LIBRARY_ERROR_INVALID,
			"A root can have at most %d patterns.", AXIS_LIBRARY_MAX_PATTERNS);
		return FALSE;
	}

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

	for (i = 0; i < kept->len; i++)
		json_array_add_string_element(array, g_ptr_array_index(kept, i));
	node = json_node_new(JSON_NODE_ARRAY);
	json_node_set_array(node, array);
	json = json_to_string(node, FALSE);

	params[0] = id;
	params[1] = interval;
	params[2] = json;
	return axis_db_pool_exec(self->pool,
	                         "INSERT INTO library_root_state (root_id, scan_interval_seconds, ignore_patterns)"
	                         " VALUES ($1::bigint, $2::integer,"
	                         "   ARRAY(SELECT jsonb_array_elements_text($3::jsonb)))"
	                         " ON CONFLICT (root_id) DO UPDATE SET"
	                         "   scan_interval_seconds = EXCLUDED.scan_interval_seconds,"
	                         "   ignore_patterns = EXCLUDED.ignore_patterns, updated_at = now()",
	                         3, params, &result, error);
}

/* ── What the pages read ────────────────────────────────────────── */

void
axis_scan_record_free(AxisScanRecord *record)
{
	if (record == NULL)
		return;

	g_free(record->trigger);
	g_free(record->message);
	g_free(record);
}

static void
scan_finding_free(gpointer data)
{
	AxisScanFinding *finding = data;

	g_free(finding->kind);
	g_free(finding->path);
	g_free(finding->detail);
	g_free(finding);
}

void
axis_root_scan_view_free(AxisRootScanView *view)
{
	if (view == NULL)
		return;

	g_strfreev(view->patterns);
	axis_scan_record_free(view->last);
	axis_scan_record_free(view->last_complete);
	if (view->recent != NULL)
		g_ptr_array_unref(view->recent);
	if (view->findings != NULL)
		g_ptr_array_unref(view->findings);
	if (view->invalid_names != NULL)
		g_ptr_array_unref(view->invalid_names);
	g_free(view);
}

#define SCAN_COLUMNS \
	"id, generation, trigger, status, reason, message," \
	" (extract(epoch FROM started_at) * 1000000)::bigint," \
	" (extract(epoch FROM walk_finished_at) * 1000000)::bigint," \
	" (extract(epoch FROM finished_at) * 1000000)::bigint," \
	" dirs, files, bytes, symlinks, special, ignored, partials, other_fs, too_deep, too_long," \
	" unreadable, io_errors, vanished, replaced, findings, kinds::text," \
	" new_files, changed_files, reappeared, moved, suspected, confirmed, removed," \
	" present_after, suspected_after, missing_after, pinned_after"

static guint64
column_u64(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return (guint64)MAX(value, 0);
}

static gint64
column_or(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        otherwise
){
	gint64 value = otherwise;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

static void
kinds_from_json(
	const gchar    *text,
	AxisWalkTotals *walk
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	JsonObject *object;
	guint i;

	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
		return;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
		return;
	object = json_node_get_object(root);
	for (i = 0; i < AXIS_FILE_KIND_COUNT; i++) {
		JsonNode *member = json_object_get_member(object, axis_file_kind_to_string((AxisFileKind)i));
		JsonObject *counts;

		if (member == NULL || !JSON_NODE_HOLDS_OBJECT(member))
			continue;
		counts = json_node_get_object(member);
		walk->kind_files[i] = (guint64)MAX(json_object_get_int_member_with_default(counts, "files", 0), 0);
		walk->kind_bytes[i] = (guint64)MAX(json_object_get_int_member_with_default(counts, "bytes", 0), 0);
	}
}

static AxisScanRecord *
record_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisScanRecord *record = g_new0(AxisScanRecord, 1);

	record->id = column_or(result, row, 0, 0);
	record->generation = column_or(result, row, 1, 0);
	record->trigger = g_strdup(axis_db_result_get_text(result, row, 2));
	if (!axis_scan_status_from_string(axis_db_result_get_text(result, row, 3), &record->status))
		record->status = AXIS_SCAN_INCOMPLETE;
	if (!axis_scan_reason_from_string(axis_db_result_get_text(result, row, 4), &record->reason))
		record->reason = AXIS_SCAN_REASON_FAILED;
	record->message = g_strdup(axis_db_result_get_text(result, row, 5));
	record->started_us = column_or(result, row, 6, 0);
	record->walk_finished_us = column_or(result, row, 7, 0);
	record->finished_us = column_or(result, row, 8, 0);
	record->walk.dirs = column_u64(result, row, 9);
	record->walk.files = column_u64(result, row, 10);
	record->walk.bytes = column_u64(result, row, 11);
	record->walk.symlinks = column_u64(result, row, 12);
	record->walk.special = column_u64(result, row, 13);
	record->walk.ignored = column_u64(result, row, 14);
	record->walk.partials = column_u64(result, row, 15);
	record->walk.other_fs = column_u64(result, row, 16);
	record->walk.too_deep = column_u64(result, row, 17);
	record->walk.too_long = column_u64(result, row, 18);
	record->walk.unreadable = column_u64(result, row, 19);
	record->walk.io_errors = column_u64(result, row, 20);
	record->walk.vanished = column_u64(result, row, 21);
	record->walk.replaced = column_u64(result, row, 22);
	record->walk.findings = column_u64(result, row, 23);
	kinds_from_json(axis_db_result_get_text(result, row, 24), &record->walk);
	record->new_files = column_u64(result, row, 25);
	record->changed_files = column_u64(result, row, 26);
	record->reappeared = column_u64(result, row, 27);
	record->moved = column_u64(result, row, 28);
	record->suspected = column_u64(result, row, 29);
	record->confirmed = column_u64(result, row, 30);
	record->removed = column_u64(result, row, 31);
	record->present_after = column_or(result, row, 32, -1);
	record->suspected_after = column_or(result, row, 33, -1);
	record->missing_after = column_or(result, row, 34, -1);
	record->pinned_after = column_or(result, row, 35, -1);

	return record;
}

gboolean
axis_library_get_root_view(
	AxisLibrary       *self,
	gint64             root_id,
	guint              n_recent,
	AxisRootScanView **view,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisRootScanView) out = NULL;
	g_autofree gchar *id = axis_library_int_text(root_id);
	g_autofree gchar *limit = g_strdup_printf("%u", MAX(n_recent, 1));
	g_autofree gchar *last = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gint64 complete_id = 0;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(view != NULL, FALSE);

	*view = NULL;
	out = g_new0(AxisRootScanView, 1);
	out->root_id = root_id;
	out->interval_seconds = AXIS_LIBRARY_DEFAULT_INTERVAL_SECONDS;
	out->recent = g_ptr_array_new_with_free_func((GDestroyNotify)axis_scan_record_free);
	out->findings = g_ptr_array_new_with_free_func(scan_finding_free);
	out->invalid_names = g_ptr_array_new_with_free_func(g_free);

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id;
	if (!axis_db_conn_exec(conn,
	                       "SELECT scan_interval_seconds, array_to_json(ignore_patterns)::text,"
	                       " (extract(epoch FROM scan_requested_at) * 1000000)::bigint,"
	                       " last_complete_scan_id"
	                       " FROM library_root_state WHERE root_id = $1::bigint",
	                       1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) == 1) {
		g_autoptr(JsonParser) parser = json_parser_new();
		GPtrArray *patterns = g_ptr_array_new();
		const gchar *text = axis_db_result_get_text(result, 0, 1);

		out->interval_seconds = (guint)CLAMP(column_or(result, 0, 0, AXIS_LIBRARY_DEFAULT_INTERVAL_SECONDS),
			0, G_MAXINT32);
		out->requested_us = column_or(result, 0, 2, 0);
		complete_id = column_or(result, 0, 3, 0);
		if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL) &&
		    JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) {
			JsonArray *array = json_node_get_array(json_parser_get_root(parser));

			for (i = 0; i < json_array_get_length(array); i++) {
				const gchar *pattern = json_array_get_string_element(array, i);

				if (pattern != NULL)
					g_ptr_array_add(patterns, g_strdup(pattern));
			}
		}
		g_ptr_array_add(patterns, NULL);
		out->patterns = (gchar **)g_ptr_array_free(patterns, FALSE);
	} else {
		out->patterns = g_new0(gchar *, 1);
	}
	g_clear_pointer(&result, axis_db_result_free);

	params[1] = limit;
	if (!axis_db_conn_exec(conn, "SELECT " SCAN_COLUMNS " FROM library_scan WHERE root_id = $1::bigint"
	                       " ORDER BY id DESC LIMIT $2::integer", 2, params, &result, error))
		goto fail;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(out->recent, record_from_row(result, i));
	if (axis_db_result_get_n_rows(result) > 0)
		out->last = record_from_row(result, 0);
	g_clear_pointer(&result, axis_db_result_free);

	if (complete_id > 0) {
		g_autofree gchar *complete = axis_library_int_text(complete_id);

		params[0] = complete;
		if (!axis_db_conn_exec(conn, "SELECT " SCAN_COLUMNS " FROM library_scan WHERE id = $1::bigint",
		                       1, params, &result, error))
			goto fail;
		if (axis_db_result_get_n_rows(result) == 1)
			out->last_complete = record_from_row(result, 0);
		g_clear_pointer(&result, axis_db_result_free);
	}

	if (out->last != NULL) {
		last = axis_library_int_text(out->last->id);
		params[0] = last;
		if (!axis_db_conn_exec(conn, "SELECT kind, rel_path, detail FROM library_scan_finding"
		                       " WHERE scan_id = $1::bigint ORDER BY seq", 1, params, &result, error))
			goto fail;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			AxisScanFinding *finding = g_new0(AxisScanFinding, 1);

			finding->kind = g_strdup(axis_db_result_get_text(result, i, 0));
			finding->path = g_strdup(axis_db_result_get_text(result, i, 1));
			finding->detail = g_strdup(axis_db_result_get_text(result, i, 2));
			g_ptr_array_add(out->findings, finding);
		}
		g_clear_pointer(&result, axis_db_result_free);
	}

	/* Names kept as bytes (library_file_invalid_name_idx keeps this
	 * cheap: such names are few). */
	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT count(*) FROM library_file WHERE root_id = $1::bigint"
	                       " AND name_invalid_utf8", 1, params, &result, error))
		goto fail;
	out->n_invalid_names = (guint64)MAX(column_or(result, 0, 0, 0), 0);
	g_clear_pointer(&result, axis_db_result_free);
	if (out->n_invalid_names > 0) {
		g_autofree gchar *listed = g_strdup_printf("%d", AXIS_LIBRARY_LISTED_NAMES);

		params[1] = listed;
		if (!axis_db_conn_exec(conn, "SELECT rel_path FROM library_file WHERE root_id = $1::bigint"
		                       " AND name_invalid_utf8 ORDER BY rel_path LIMIT $2::integer",
		                       2, params, &result, error))
			goto fail;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++)
			g_ptr_array_add(out->invalid_names, g_strdup(axis_db_result_get_text(result, i, 0)));
	}
	axis_db_pool_release(self->pool, conn);

	*view = g_steal_pointer(&out);
	return TRUE;

fail:
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}
