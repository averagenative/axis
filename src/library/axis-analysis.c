/*
 * axis-analysis.c - the analysis: its settings, jobs, and what the pages
 * read
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "library/axis-analysis-private.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"

G_DEFINE_QUARK(axis-analysis-error-quark, axis_analysis_error)

const gchar *const axis_analysis_large_codecs[AXIS_ANALYSIS_N_LARGE_CODECS] = {
	"h264", "hevc", "av1", "vp9"
};

/* ── Settings ───────────────────────────────────────────────────── */

static const AxisSettingInfo setting_table[] = {
	{
		AXIS_ANALYSIS_SETTING_LARGE_H264, AXIS_SETTING_DECIMAL,
		"Large for its class: H.264, in bits per pixel",
		"A file whose video uses at least this many bits for each pixel of each frame is listed as "
		"large for its class. A 1080p movie at 24 frames a second is about 0.15 at 8 Mbit/s and 0.6 at "
		"30 Mbit/s. Three decimals are used.",
		0, "0.30", 0, 0, 0.01, 5.0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_LARGE_HEVC, AXIS_SETTING_DECIMAL,
		"Large for its class: HEVC, in bits per pixel",
		"As for H.264, for HEVC, which needs fewer bits for the same picture.",
		0, "0.15", 0, 0, 0.01, 5.0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_LARGE_AV1, AXIS_SETTING_DECIMAL,
		"Large for its class: AV1, in bits per pixel",
		"As for H.264, for AV1.",
		0, "0.12", 0, 0, 0.01, 5.0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_LARGE_VP9, AXIS_SETTING_DECIMAL,
		"Large for its class: VP9, in bits per pixel",
		"As for H.264, for VP9. Other codecs are not judged.",
		0, "0.15", 0, 0, 0.01, 5.0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_MIN_INTERVAL, AXIS_SETTING_DURATION,
		"Refresh the reports at most every",
		"After scans and file reads have settled, the reports are computed again, but not more often than "
		"this. An administrator can ask for a refresh at any time.",
		0, "1h",
		300, 7 * 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_SETTLE, AXIS_SETTING_DURATION,
		"Scans and file reads have settled after",
		"The reports wait until no scan is running, no file is waiting to be read and the last of them ended this "
		"long ago, so that they describe a library that has stopped changing.",
		0, "10m",
		60, 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_KEEP_DAILY, AXIS_SETTING_DURATION,
		"Keep the last report of each day for",
		"The history of the library's size on the Disk usage page is made of these.",
		0, "400d",
		7 * 24 * 3600, (gint64)3650 * 24 * 3600, 0, 0, NULL
	},
	{
		AXIS_ANALYSIS_SETTING_KEEP_OTHER, AXIS_SETTING_DURATION,
		"Keep the other reports for",
		"Reports that are not the last of their day are dropped after this long.",
		0, "7d",
		24 * 3600, 400 * 24 * 3600, 0, 0, NULL
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_analysis_get_setting_table(void)
{
	return setting_table;
}

static gboolean
declared(
	AxisAnalysis *self,
	const gchar  *key
){
	return self->settings != NULL && axis_settings_lookup(self->settings, key, NULL) != NULL;
}

void
axis_analysis_get_thresholds(
	AxisAnalysis *self,
	gint64        milli[AXIS_ANALYSIS_N_LARGE_CODECS]
){
	static const gchar *const keys[AXIS_ANALYSIS_N_LARGE_CODECS] = {
		AXIS_ANALYSIS_SETTING_LARGE_H264, AXIS_ANALYSIS_SETTING_LARGE_HEVC,
		AXIS_ANALYSIS_SETTING_LARGE_AV1, AXIS_ANALYSIS_SETTING_LARGE_VP9
	};
	static const gdouble defaults[AXIS_ANALYSIS_N_LARGE_CODECS] = {
		AXIS_ANALYSIS_DEFAULT_LARGE_H264, AXIS_ANALYSIS_DEFAULT_LARGE_HEVC,
		AXIS_ANALYSIS_DEFAULT_LARGE_AV1, AXIS_ANALYSIS_DEFAULT_LARGE_VP9
	};
	guint i;

	for (i = 0; i < AXIS_ANALYSIS_N_LARGE_CODECS; i++) {
		gdouble value = declared(self, keys[i]) ? axis_settings_get_decimal(self->settings, keys[i]) :
			defaults[i];

		/* Rounded to the nearest thousandth (the value is positive). */
		milli[i] = (gint64)(value * 1000.0 + 0.5);
		if (milli[i] < 10)
			milli[i] = 10;
		if (milli[i] > 5000)
			milli[i] = 5000;
	}
}

gint64
axis_analysis_get_duration(
	AxisAnalysis *self,
	const gchar  *key,
	gint64        fallback
){
	return declared(self, key) ? axis_settings_get_duration(self->settings, key) : fallback;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisAnalysis *
axis_analysis_new(
	AxisDbPool   *pool,
	AxisSettings *settings
){
	AxisAnalysis *self;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisAnalysis, 1);
	self->ref_count = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	g_mutex_init(&self->lock);
	return self;
}

AxisAnalysis *
axis_analysis_ref(AxisAnalysis *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_analysis_unref(AxisAnalysis *self)
{
	if (self == NULL)
		return;
	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_clear_pointer(&self->settings, axis_settings_unref);
	g_mutex_clear(&self->lock);
	g_free(self);
}

void
axis_analysis_set_tuning(
	AxisAnalysis             *self,
	const AxisAnalysisTuning *tuning
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->tuning = *tuning;
	g_mutex_unlock(&self->lock);
}

/* ── Refreshing ─────────────────────────────────────────────────── */

gboolean
axis_analysis_refresh(
	AxisAnalysis    *self,
	const gchar     *trigger,
	gint64           job_id,
	AxisJobContext  *context,
	gint64          *snapshot_id,
	GError         **error
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(trigger != NULL, FALSE);

	return axis_analysis_refresh_run(self, trigger, job_id, context, snapshot_id, error);
}

static gboolean
enqueue_refresh(
	AxisDbConn  *conn,
	const gchar *trigger,
	gint         priority,
	gint64      *id,
	gboolean    *created,
	GError     **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	AxisJobSpec spec;

	json_object_set_string_member(payload, "trigger", trigger);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_ANALYSIS_REFRESH_KIND;
	spec.queue = AXIS_JOB_MAINTENANCE_QUEUE;
	spec.payload = payload;
	spec.priority = priority;
	spec.dedupe_key = AXIS_ANALYSIS_REFRESH_KIND ":run";
	spec.max_attempts = AXIS_ANALYSIS_REFRESH_ATTEMPTS;
	return axis_job_enqueue(conn, &spec, id, created, error);
}

gboolean
axis_analysis_request_refresh(
	AxisAnalysis  *self,
	gint64        *job_id,
	GError       **error
){
	AxisDbConn *conn;
	gboolean ok;

	g_return_val_if_fail(self != NULL, FALSE);

	if (job_id != NULL)
		*job_id = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = enqueue_refresh(conn, "request", AXIS_ANALYSIS_REQUEST_PRIORITY, job_id, NULL, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

gboolean
axis_analysis_enqueue_due(
	AxisAnalysis  *self,
	gboolean      *enqueued,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *settle = NULL;
	g_autofree gchar *interval = NULL;
	g_autofree gchar *daily = NULL;
	const gchar *params[3];
	AxisDbConn *conn;
	gboolean due = FALSE;
	gboolean created = FALSE;

	g_return_val_if_fail(self != NULL, FALSE);

	if (enqueued != NULL)
		*enqueued = FALSE;
	settle = g_strdup_printf("%" G_GINT64_FORMAT, axis_analysis_get_duration(self,
		AXIS_ANALYSIS_SETTING_SETTLE, AXIS_ANALYSIS_DEFAULT_SETTLE));
	interval = g_strdup_printf("%" G_GINT64_FORMAT, axis_analysis_get_duration(self,
		AXIS_ANALYSIS_SETTING_MIN_INTERVAL, AXIS_ANALYSIS_DEFAULT_MIN_INTERVAL));
	daily = g_strdup_printf("%d", AXIS_ANALYSIS_DAILY_SECONDS);
	params[0] = settle;
	params[1] = interval;
	params[2] = daily;

	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	/* A scan that a job runs, or a probe job not finished, is not settled;
	 * nor is anything that ended less than the settle time ago.  Something
	 * changed when a scan or probe ended after the last snapshot. */
	if (!axis_db_conn_exec(conn,
	        "WITH s AS (SELECT"
	        " (SELECT count(*) FROM library_root) AS roots,"
	        " (SELECT max(taken_at) FROM analysis_snapshot) AS last_taken,"
	        " EXISTS (SELECT 1 FROM library_scan ls LEFT JOIN job j ON j.id = ls.job_id"
	        "   WHERE ls.status = 'running' AND (ls.job_id IS NULL OR j.state = 'running')) AS scanning,"
	        " EXISTS (SELECT 1 FROM job WHERE kind = 'probe.file' AND state IN ('queued', 'running')) AS probing,"
	        " greatest((SELECT max(finished_at) FROM library_scan WHERE status = 'complete'),"
	        "          (SELECT max(finished_at) FROM job WHERE kind = 'probe.file' AND state = 'succeeded'))"
	        "   AS activity)"
	        " SELECT roots > 0 AND NOT scanning AND NOT probing"
	        "  AND (activity IS NULL OR activity <= now() - make_interval(secs => $1::double precision))"
	        "  AND (CASE WHEN last_taken IS NULL THEN activity IS NOT NULL"
	        "       ELSE (activity > last_taken AND last_taken <= now() - make_interval(secs => $2::double precision))"
	        "         OR last_taken <= now() - make_interval(secs => $3::double precision) END)"
	        " FROM s",
	        3, params, &result, error)) {
		axis_db_pool_release(self->pool, conn);
		return FALSE;
	}
	if (axis_db_result_get_n_rows(result) == 1)
		axis_db_result_get_boolean(result, 0, 0, &due);

	if (due) {
		if (!enqueue_refresh(conn, "schedule", 0, NULL, &created, error)) {
			axis_db_pool_release(self->pool, conn);
			return FALSE;
		}
		if (enqueued != NULL)
			*enqueued = created;
	}
	axis_db_pool_release(self->pool, conn);
	return TRUE;
}

gboolean
axis_analysis_run_refresh_job(
	AxisAnalysis    *self,
	AxisJobContext  *context,
	GError         **error
){
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *trigger = "schedule";

	if (payload != NULL && json_object_has_member(payload, "trigger")) {
		trigger = json_object_get_string_member_with_default(payload, "trigger", "");
		if (g_strcmp0(trigger, "schedule") != 0 && g_strcmp0(trigger, "request") != 0) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT,
				"the refresh job's trigger is not one this daemon knows");
			return FALSE;
		}
	}
	return axis_analysis_refresh(self, trigger, axis_job_context_get_id(context), context, NULL, error);
}

gboolean
axis_analysis_run_due_job(
	AxisAnalysis    *self,
	AxisJobContext  *context,
	GError         **error
){
	return axis_analysis_enqueue_due(self, NULL, error);
}

/* ── Reading ────────────────────────────────────────────────────── */

static void
fact_free(gpointer data)
{
	AxisAnalysisFact *fact = data;

	g_free(fact->dim1);
	g_free(fact->dim2);
	if (fact->extra != NULL)
		json_object_unref(fact->extra);
	g_free(fact);
}

void
axis_analysis_snapshot_free(AxisAnalysisSnapshot *snapshot)
{
	if (snapshot == NULL)
		return;

	g_free(snapshot->trigger);
	if (snapshot->coverage != NULL)
		json_object_unref(snapshot->coverage);
	if (snapshot->config != NULL)
		json_object_unref(snapshot->config);
	g_clear_pointer(&snapshot->facts, g_hash_table_unref);
	g_free(snapshot);
}

static gint64
int_at(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

/* A JSON object from text, never NULL (an empty one for text that is not
 * one: it was written by this code, so this is a database that was
 * changed from outside). */
static JsonObject *
object_of(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;

	if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL)) {
		root = json_parser_get_root(parser);
		if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
			return json_node_dup_object(root);
	}
	return json_object_new();
}

const GPtrArray *
axis_analysis_snapshot_facts(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report
){
	return g_hash_table_lookup(snapshot->facts, report);
}

const AxisAnalysisFact *
axis_analysis_snapshot_fact(
	const AxisAnalysisSnapshot *snapshot,
	const gchar                *report,
	const gchar                *dim1,
	const gchar                *dim2
){
	const GPtrArray *facts = axis_analysis_snapshot_facts(snapshot, report);
	guint i;

	for (i = 0; facts != NULL && i < facts->len; i++) {
		const AxisAnalysisFact *fact = g_ptr_array_index((GPtrArray *)facts, i);

		if (g_strcmp0(fact->dim1, dim1 != NULL ? dim1 : "") == 0 &&
		    g_strcmp0(fact->dim2, dim2 != NULL ? dim2 : "") == 0)
			return fact;
	}
	return NULL;
}

gboolean
axis_analysis_load_snapshot(
	AxisAnalysis          *self,
	gint64                 snapshot_id,
	const gchar *const    *reports,
	AxisAnalysisSnapshot **snapshot,
	GError               **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) facts = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *list = NULL;
	AxisAnalysisSnapshot *out;
	const gchar *params[2];
	AxisDbConn *conn;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(snapshot != NULL, FALSE);

	*snapshot = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;

	/* One transaction, so that the facts are those of the snapshot read. */
	if (!axis_db_conn_begin(conn, error))
		goto fail;
	if (snapshot_id > 0)
		id = g_strdup_printf("%" G_GINT64_FORMAT, snapshot_id);
	/* NULL asks for the latest. */
	params[0] = id;
	params[1] = NULL;
	if (!axis_db_conn_exec(conn,
	        "SELECT id, (extract(epoch FROM taken_at) * 1000000)::bigint,"
	        " (extract(epoch FROM finished_at) * 1000000)::bigint, trigger, took_ms, paths, bytes,"
	        " coalesce(volumes_total, -1), coalesce(volumes_used, -1), coverage::text, config::text"
	        " FROM analysis_snapshot WHERE $1::bigint IS NULL OR id = $1::bigint"
	        " ORDER BY taken_at DESC, id DESC LIMIT 1",
	        1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) == 0) {
		axis_db_conn_commit(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		return TRUE;
	}

	out = g_new0(AxisAnalysisSnapshot, 1);
	out->id = int_at(result, 0, 0, 0);
	out->taken_us = int_at(result, 0, 1, 0);
	out->finished_us = int_at(result, 0, 2, 0);
	out->trigger = g_strdup(axis_db_result_get_text(result, 0, 3));
	out->took_ms = int_at(result, 0, 4, 0);
	out->paths = int_at(result, 0, 5, 0);
	out->bytes = int_at(result, 0, 6, 0);
	out->volumes_total = int_at(result, 0, 7, -1);
	out->volumes_used = int_at(result, 0, 8, -1);
	out->coverage = object_of(axis_db_result_get_text(result, 0, 9));
	out->config = object_of(axis_db_result_get_text(result, 0, 10));
	out->facts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);

	g_free(id);
	id = g_strdup_printf("%" G_GINT64_FORMAT, out->id);
	params[0] = id;
	if (reports != NULL) {
		GString *text = g_string_new("{");

		for (i = 0; reports[i] != NULL; i++)
			g_string_append_printf(text, "%s\"%s\"", i > 0 ? "," : "", reports[i]);
		g_string_append_c(text, '}');
		list = g_string_free(text, FALSE);
	}
	params[1] = list;
	if (!axis_db_conn_exec(conn,
	        "SELECT report, dim1, dim2, paths, bytes, extra::text FROM analysis_fact"
	        " WHERE snapshot_id = $1::bigint AND ($2::text[] IS NULL OR report = ANY($2::text[]))"
	        " ORDER BY report, dim1, dim2",
	        2, params, &facts, error)) {
		axis_analysis_snapshot_free(out);
		goto fail;
	}
	axis_db_conn_commit(conn, NULL);
	axis_db_pool_release(self->pool, conn);

	for (i = 0; i < axis_db_result_get_n_rows(facts); i++) {
		const gchar *report = axis_db_result_get_text(facts, i, 0);
		AxisAnalysisFact *fact = g_new0(AxisAnalysisFact, 1);
		GPtrArray *array = g_hash_table_lookup(out->facts, report);

		fact->dim1 = g_strdup(axis_db_result_get_text(facts, i, 1));
		fact->dim2 = g_strdup(axis_db_result_get_text(facts, i, 2));
		fact->paths = int_at(facts, i, 3, 0);
		fact->bytes = int_at(facts, i, 4, 0);
		fact->extra = object_of(axis_db_result_get_text(facts, i, 5));
		if (array == NULL) {
			array = g_ptr_array_new_with_free_func(fact_free);
			g_hash_table_insert(out->facts, g_strdup(report), array);
		}
		g_ptr_array_add(array, fact);
	}
	*snapshot = out;
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	return FALSE;
}

gboolean
axis_analysis_load_history(
	AxisAnalysis  *self,
	guint          limit,
	GPtrArray    **points,
	GError       **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *count = g_strdup_printf("%u", MIN(limit, 5000u));
	g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(g_free);
	const gchar *params[1];
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(points != NULL, FALSE);

	*points = NULL;
	params[0] = count;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT id, taken_us, paths, bytes, vt, vu FROM ("
	        " SELECT id, (extract(epoch FROM taken_at) * 1000000)::bigint AS taken_us, paths, bytes,"
	        "  coalesce(volumes_total, -1) AS vt, coalesce(volumes_used, -1) AS vu"
	        " FROM analysis_snapshot ORDER BY taken_at DESC, id DESC LIMIT $1::integer) h ORDER BY taken_us, id",
	        1, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisAnalysisPoint *point = g_new0(AxisAnalysisPoint, 1);

		point->id = int_at(result, i, 0, 0);
		point->taken_us = int_at(result, i, 1, 0);
		point->paths = int_at(result, i, 2, 0);
		point->bytes = int_at(result, i, 3, 0);
		point->volumes_total = int_at(result, i, 4, -1);
		point->volumes_used = int_at(result, i, 5, -1);
		g_ptr_array_add(out, point);
	}
	*points = g_steal_pointer(&out);
	return TRUE;
}

void
axis_analysis_status_clear(AxisAnalysisStatus *status)
{
	g_free(status->message);
	memset(status, 0, sizeof(*status));
}

gboolean
axis_analysis_get_status(
	AxisAnalysis        *self,
	AxisAnalysisStatus  *status,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(status != NULL, FALSE);

	memset(status, 0, sizeof(*status));
	status->progress = -1;
	if (!axis_db_pool_exec(self->pool,
	        "SELECT id, state::text, coalesce(progress, -1)::text, progress_message FROM job"
	        " WHERE kind = '" AXIS_ANALYSIS_REFRESH_KIND "' AND state IN ('queued', 'running')"
	        " ORDER BY id LIMIT 1", 0, NULL, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 1) {
		status->refreshing = TRUE;
		status->job_id = int_at(result, 0, 0, 0);
		status->running = g_strcmp0(axis_db_result_get_text(result, 0, 1), "running") == 0;
		status->progress = g_ascii_strtod(axis_db_result_get_text(result, 0, 2), NULL);
		status->message = g_strdup(axis_db_result_get_text(result, 0, 3));
	}
	return TRUE;
}

gint64
axis_analysis_extra_int(
	JsonObject  *extra,
	const gchar *member
){
	JsonNode *node;

	if (extra == NULL || !json_object_has_member(extra, member))
		return 0;
	node = json_object_get_member(extra, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return 0;
	switch (json_node_get_value_type(node)) {
	case G_TYPE_INT64:
	case G_TYPE_INT:
		return json_node_get_int(node);
	case G_TYPE_DOUBLE:
		return (gint64)json_node_get_double(node);
	case G_TYPE_BOOLEAN:
		return json_node_get_boolean(node) ? 1 : 0;
	default:
		return 0;
	}
}

const gchar *
axis_analysis_extra_string(
	JsonObject  *extra,
	const gchar *member
){
	JsonNode *node;

	if (extra == NULL || !json_object_has_member(extra, member))
		return "";
	node = json_object_get_member(extra, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		return "";
	return json_node_get_string(node);
}
