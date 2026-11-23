/*
 * axis-web-jobs-data.c - reading jobs for the Jobs pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-jobs-data.h"

#include <stdlib.h>
#include <string.h>

#define ROW_COLUMNS \
	"id, kind, queue, state::text, attempts, max_attempts, progress, " \
	"progress_message, last_error, cancel_requested, " \
	"(extract(epoch from created_at) * 1000000)::bigint, " \
	"(extract(epoch from started_at) * 1000000)::bigint, " \
	"(extract(epoch from finished_at) * 1000000)::bigint, " \
	"(extract(epoch from run_at) * 1000000)::bigint, dedupe_key"

enum
{
	COL_ID,
	COL_KIND,
	COL_QUEUE,
	COL_STATE,
	COL_ATTEMPTS,
	COL_MAX_ATTEMPTS,
	COL_PROGRESS,
	COL_PROGRESS_MESSAGE,
	COL_LAST_ERROR,
	COL_CANCEL_REQUESTED,
	COL_CREATED,
	COL_STARTED,
	COL_FINISHED,
	COL_RUN_AT,
	COL_KEY,
	COL_EARLIER,	/* only when asked for */
	COL_PAYLOAD	/* only when asked for, after the earlier runs */
};

/* What a row has beyond ROW_COLUMNS: the list and the live updates read
 * the plain row; only one job's page reads its earlier runs and payload. */
typedef enum
{
	ROW_PLAIN,
	ROW_EARLIER_AND_PAYLOAD		/* earlier_runs, then the payload */
} RowExtra;

static void
earlier_run_free(gpointer data)
{
	AxisJobEarlierRun *run = data;

	g_free(run->error);
	g_free(run->retried_by);
	g_free(run);
}

void
axis_job_row_free(AxisJobRow *row)
{
	if (row == NULL)
		return;

	g_free(row->kind);
	g_free(row->label);
	g_free(row->queue);
	g_free(row->progress_message);
	g_free(row->last_error);
	g_free(row->payload);
	g_free(row->key);
	g_clear_pointer(&row->earlier, g_ptr_array_unref);
	g_free(row);
}

/* A time job.earlier_runs keeps (JSON from timestamptz), or 0. */
static gint64
earlier_time(
	JsonObject  *object,
	const gchar *member
){
	const gchar *text = json_object_get_string_member_with_default(object, member, NULL);
	g_autoptr(GDateTime) time = text != NULL ? g_date_time_new_from_iso8601(text, NULL) : NULL;

	return time != NULL ? g_date_time_to_unix_usec(time) : 0;
}

/* job.earlier_runs as text, read: an entry that is not an object, or of a
 * state that is not failed or cancelled, is left out. */
static GPtrArray *
earlier_from_text(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	GPtrArray *runs = g_ptr_array_new_with_free_func(earlier_run_free);
	JsonNode *root;
	JsonArray *array;
	guint i;

	if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
		return runs;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root))
		return runs;

	array = json_node_get_array(root);
	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object;
		AxisJobEarlierRun *run;
		AxisJobState state;
		const gchar *error;
		const gchar *by;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		object = json_node_get_object(node);
		if (!axis_job_state_from_string(json_object_get_string_member_with_default(object, "state", NULL), &state) ||
		    (state != AXIS_JOB_STATE_FAILED && state != AXIS_JOB_STATE_CANCELLED))
			continue;
		error = json_object_get_string_member_with_default(object, "error", NULL);
		by = json_object_get_string_member_with_default(object, "retried_by", NULL);
		run = g_new0(AxisJobEarlierRun, 1);
		run->state = state;
		run->attempts = (guint)CLAMP(json_object_get_int_member_with_default(object, "attempts", 0), 0,
			AXIS_JOB_MAX_ATTEMPTS_LIMIT);
		run->error = error != NULL ? g_utf8_make_valid(error, -1) : NULL;
		run->finished_us = earlier_time(object, "finished_at");
		run->retried_us = earlier_time(object, "retried_at");
		run->retried_by = by != NULL ? g_utf8_make_valid(by, -1) : NULL;
		g_ptr_array_add(runs, run);
	}

	return runs;
}

static gchar *
text_or_null(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	if (axis_db_result_is_null(result, row, column))
		return NULL;
	return g_strdup(axis_db_result_get_text(result, row, column));
}

static gint64
time_or_zero(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (axis_db_result_is_null(result, row, column) ||
	    !axis_db_result_get_int64(result, row, column, &value))
		return 0;
	return value;
}

/* The upkeep kinds as a text[] literal: "{}" without a registry.  Kinds are
 * valid job names (letters, digits, '.', '_', '-'), so nothing needs
 * quoting; they are quoted all the same. */
static gchar *
upkeep_literal(AxisJobRegistry *kinds)
{
	g_auto(GStrv) upkeep = kinds != NULL ? axis_job_registry_list_class(kinds, AXIS_JOB_CLASS_UPKEEP) : NULL;
	GString *literal = g_string_new("{");
	guint i;

	for (i = 0; upkeep != NULL && upkeep[i] != NULL; i++)
		g_string_append_printf(literal, "%s\"%s\"", i > 0 ? "," : "", upkeep[i]);
	g_string_append_c(literal, '}');
	return g_string_free(literal, FALSE);
}

static AxisJobRow *
row_from_result(
	AxisDbResult    *result,
	AxisJobRegistry *kinds,
	guint            index,
	RowExtra         extra
){
	AxisJobRow *row = g_new0(AxisJobRow, 1);
	gint64 number = 0;
	gboolean flag = FALSE;

	axis_db_result_get_int64(result, index, COL_ID, &row->id);
	row->kind = g_strdup(axis_db_result_get_text(result, index, COL_KIND));
	if (kinds != NULL) {
		row->label = g_strdup(axis_job_registry_get_label(kinds, row->kind));
		row->upkeep = axis_job_registry_get_class(kinds, row->kind) == AXIS_JOB_CLASS_UPKEEP;
	}
	row->queue = g_strdup(axis_db_result_get_text(result, index, COL_QUEUE));
	if (!axis_job_state_from_string(axis_db_result_get_text(result, index, COL_STATE), &row->state))
		row->state = AXIS_JOB_STATE_QUEUED;

	if (axis_db_result_get_int64(result, index, COL_ATTEMPTS, &number))
		row->attempts = (guint)MAX(number, 0);
	if (axis_db_result_get_int64(result, index, COL_MAX_ATTEMPTS, &number))
		row->max_attempts = (guint)MAX(number, 0);

	row->progress = -1;
	if (!axis_db_result_is_null(result, index, COL_PROGRESS))
		row->progress = CLAMP(g_ascii_strtod(
			axis_db_result_get_text(result, index, COL_PROGRESS), NULL), 0.0, 1.0);

	row->progress_message = text_or_null(result, index, COL_PROGRESS_MESSAGE);
	row->last_error = text_or_null(result, index, COL_LAST_ERROR);
	if (axis_db_result_get_boolean(result, index, COL_CANCEL_REQUESTED, &flag))
		row->cancel_requested = flag;

	row->created_us = time_or_zero(result, index, COL_CREATED);
	row->started_us = time_or_zero(result, index, COL_STARTED);
	row->finished_us = time_or_zero(result, index, COL_FINISHED);
	row->run_at_us = time_or_zero(result, index, COL_RUN_AT);
	row->key = text_or_null(result, index, COL_KEY);

	if (extra != ROW_PLAIN)
		row->earlier = earlier_from_text(axis_db_result_get_text(result, index, COL_EARLIER));
	if (extra == ROW_EARLIER_AND_PAYLOAD)
		row->payload = text_or_null(result, index, COL_PAYLOAD);

	return row;
}

static GPtrArray *
rows_from_result(
	AxisDbResult    *result,
	AxisJobRegistry *kinds,
	guint            limit,
	RowExtra         extra
){
	GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)axis_job_row_free);
	guint n = MIN(axis_db_result_get_n_rows(result), limit);
	guint i;

	for (i = 0; i < n; i++)
		g_ptr_array_add(rows, row_from_result(result, kinds, i, extra));

	return rows;
}

/* ── Filters ────────────────────────────────────────────────────── */

gboolean
axis_web_jobs_filter_state_is_valid(const gchar *state)
{
	AxisJobState parsed;

	if (state == NULL || state[0] == '\0' || strcmp(state, "active") == 0)
		return TRUE;
	return axis_job_state_from_string(state, &parsed);
}

gboolean
axis_web_jobs_filter_class_is_valid(const gchar *job_class)
{
	return job_class == NULL || job_class[0] == '\0' || strcmp(job_class, "work") == 0 ||
		strcmp(job_class, "upkeep") == 0;
}

gboolean
axis_web_jobs_cursor_is_valid(const gchar *cursor)
{
	gsize length;
	gsize i;

	if (cursor == NULL)
		return FALSE;
	if (cursor[0] != 'a' && cursor[0] != 'f')
		return FALSE;

	length = strlen(cursor);
	if (length < 2 || length > 19 || cursor[1] == '0')
		return FALSE;
	for (i = 1; i < length; i++) {
		if (cursor[i] < '0' || cursor[i] > '9')
			return FALSE;
	}

	return TRUE;
}

static gchar *
make_cursor(const AxisJobRow *row)
{
	gboolean active = row->state == AXIS_JOB_STATE_QUEUED || row->state == AXIS_JOB_STATE_RUNNING;

	return g_strdup_printf("%c%" G_GINT64_FORMAT, active ? 'a' : 'f', row->id);
}

/* ── Queries ────────────────────────────────────────────────────── */

gboolean
axis_web_jobs_list(
	AxisDbPool          *pool,
	AxisJobRegistry     *kinds,
	const AxisJobFilter *filter,
	GPtrArray          **rows,
	gchar              **next,
	GStrv               *queues,
	GError             **error
){
	static const gchar sql[] =
		"SELECT " ROW_COLUMNS " FROM job "
		"WHERE ($1::text[] IS NULL OR state::text = ANY ($1::text[])) "
		"AND ($2::text IS NULL OR queue = $2::text) "
		"AND ($3::boolean IS NULL "
		"     OR (state IN ('queued', 'running'), id) < ($3::boolean, $4::bigint)) "
		"AND ($5::text IS NULL OR (kind = ANY ($6::text[])) = ($5::text = 'upkeep')) "
		"ORDER BY (state IN ('queued', 'running')) DESC, id DESC "
		"LIMIT $7::int + 1";
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisDbResult) queue_result = NULL;
	g_autofree gchar *states = NULL;
	g_autofree gchar *cursor_id = NULL;
	g_autofree gchar *upkeep = upkeep_literal(kinds);
	guint limit = filter->limit != 0 ? MIN(filter->limit, AXIS_JOBS_PAGE_SIZE) : AXIS_JOBS_PAGE_SIZE;
	g_autofree gchar *limit_text = g_strdup_printf("%u", limit);
	const gchar *params[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
	g_autoptr(GPtrArray) list = NULL;
	guint i;

	g_return_val_if_fail(rows != NULL, FALSE);

	if (filter->job_class != NULL && filter->job_class[0] != '\0')
		params[4] = filter->job_class;
	params[5] = upkeep;
	params[6] = limit_text;
	if (filter->state != NULL && filter->state[0] != '\0') {
		if (strcmp(filter->state, "active") == 0)
			states = g_strdup("{queued,running}");
		else
			states = g_strdup_printf("{%s}", filter->state);
		params[0] = states;
	}
	if (filter->queue != NULL && filter->queue[0] != '\0')
		params[1] = filter->queue;
	if (filter->cursor != NULL) {
		params[2] = filter->cursor[0] == 'a' ? "true" : "false";
		cursor_id = g_strdup(filter->cursor + 1);
		params[3] = cursor_id;
	}

	if (!axis_db_pool_exec(pool, sql, 7, params, &result, error))
		return FALSE;

	list = rows_from_result(result, kinds, limit, ROW_PLAIN);
	if (next != NULL) {
		*next = NULL;
		if (axis_db_result_get_n_rows(result) > limit && list->len > 0)
			*next = make_cursor(g_ptr_array_index(list, list->len - 1));
	}

	if (queues != NULL) {
		g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);

		if (!axis_db_pool_exec(pool, "SELECT DISTINCT queue FROM job ORDER BY queue LIMIT 100",
		                       0, NULL, &queue_result, error)) {
			if (next != NULL)
				g_clear_pointer(next, g_free);
			return FALSE;
		}
		/* A name that could not be asked for again is not offered (and
		 * is never put in an attribute). */
		for (i = 0; i < axis_db_result_get_n_rows(queue_result); i++) {
			const gchar *name = axis_db_result_get_text(queue_result, i, 0);

			if (axis_job_name_is_valid(name))
				g_ptr_array_add(names, g_strdup(name));
		}
		g_ptr_array_add(names, NULL);
		*queues = (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
	}

	*rows = g_steal_pointer(&list);
	return TRUE;
}

gboolean
axis_web_jobs_get(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	gint64            id,
	gboolean          whole_payload,
	AxisJobRow      **row,
	GError          **error
){
	static const gchar sql[] =
		"SELECT " ROW_COLUMNS ", earlier_runs::text, left(payload::text, $2::int) FROM job WHERE id = $1::bigint";
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	g_autofree gchar *shown = g_strdup_printf("%d",
		whole_payload ? AXIS_JOB_MAX_PAYLOAD_BYTES * 2 : AXIS_JOBS_PAYLOAD_SHOWN_CHARS);
	const gchar *params[2];

	g_return_val_if_fail(row != NULL, FALSE);

	params[0] = text;
	params[1] = shown;
	*row = NULL;
	if (!axis_db_pool_exec(pool, sql, 2, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) > 0)
		*row = row_from_result(result, kinds, 0, ROW_EARLIER_AND_PAYLOAD);

	return TRUE;
}

gboolean
axis_web_jobs_get_many(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	GArray           *ids,
	GPtrArray       **rows,
	gint             *running,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) literal = g_string_new("{");
	const gchar *params[1];
	guint i;

	g_return_val_if_fail(rows != NULL, FALSE);

	*rows = NULL;
	if (ids != NULL && ids->len > 0) {
		for (i = 0; i < ids->len && i < AXIS_JOBS_MAX_BATCH; i++)
			g_string_append_printf(literal, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
				g_array_index(ids, gint64, i));
		g_string_append_c(literal, '}');
		params[0] = literal->str;

		if (!axis_db_pool_exec(pool,
		        "SELECT " ROW_COLUMNS " FROM job WHERE id = ANY ($1::bigint[]) ORDER BY id",
		        1, params, &result, error))
			return FALSE;
		*rows = rows_from_result(result, kinds, AXIS_JOBS_MAX_BATCH, ROW_PLAIN);
	} else {
		*rows = g_ptr_array_new_with_free_func((GDestroyNotify)axis_job_row_free);
	}

	if (running != NULL && !axis_web_jobs_count_running(pool, kinds, running, error)) {
		g_clear_pointer(rows, g_ptr_array_unref);
		return FALSE;
	}

	return TRUE;
}

gboolean
axis_web_jobs_count_running(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	gint             *running,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *upkeep = upkeep_literal(kinds);
	const gchar *params[1] = { upkeep };
	gint64 count = 0;

	if (!axis_db_pool_exec(pool,
	        "SELECT count(*) FROM job WHERE state = 'running' AND NOT (kind = ANY ($1::text[]))",
	        1, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) > 0)
		axis_db_result_get_int64(result, 0, 0, &count);
	*running = (gint)MIN(count, G_MAXINT);

	return TRUE;
}

gboolean
axis_web_jobs_count_failed(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	guint             timeout_ms,
	gint64           *count,
	gint64           *newest,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *upkeep = upkeep_literal(kinds);
	g_autofree gchar *sql = NULL;

	*count = 0;
	*newest = 0;
	/* The statement takes no parameter (every wait of it is bounded on this
	 * side), so the kinds are written into it: names from the code's own
	 * registry, and none is taken that could end the literal. */
	if (strchr(upkeep, '\'') != NULL || strchr(upkeep, '\\') != NULL) {
		g_free(upkeep);
		upkeep = g_strdup("{}");
	}
	/* The list's own conditions for state=failed&class=work, and no other:
	 * a failed job is listed until it is retried or its keeping time
	 * (jobs.keep_failed) removes it. */
	sql = g_strdup_printf("SELECT count(*), coalesce(max(id), 0) FROM job WHERE state = 'failed'"
		" AND NOT (kind = ANY ('%s'::text[]))", upkeep);
	if (!axis_db_pool_query_bounded(pool, sql, timeout_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, count);
	axis_db_result_get_int64(result, 0, 1, newest);
	return TRUE;
}

/* The kinds are named by what they do: every module's search ends in
 * ".search", the scans are the library's, the probe's and Identify's, the
 * conversions the converter's and the estimator's measurements. */
gboolean
axis_web_jobs_count_activity(
	AxisDbPool        *pool,
	AxisJobRegistry   *kinds,
	AxisJobsActivity  *activity,
	GError           **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *upkeep = upkeep_literal(kinds);
	const gchar *params[1] = { upkeep };
	gint64 values[7];
	guint i;

	memset(activity, 0, sizeof(*activity));
	if (!axis_db_pool_exec(pool,
	        "SELECT count(*) FILTER (WHERE state = 'running'), count(*) FILTER (WHERE state = 'queued'),"
	        " count(*) FILTER (WHERE state = 'running' AND kind LIKE '%.search'),"
	        " count(*) FILTER (WHERE state = 'running' AND kind LIKE 'import.%'),"
	        " count(*) FILTER (WHERE state = 'running' AND (kind LIKE 'library.scan%' OR kind LIKE 'probe.%'"
	        "   OR kind LIKE 'identify.%')),"
	        " count(*) FILTER (WHERE state = 'running' AND (kind LIKE 'convert.%' OR kind LIKE 'estimate.sample%')),"
	        " (SELECT count(*) FROM download WHERE state IN ('sending', 'queued', 'downloading'))"
	        " FROM job WHERE state IN ('queued', 'running') AND NOT (kind = ANY ($1::text[]))",
	        1, params, &result, error))
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(values); i++) {
		values[i] = 0;
		if (axis_db_result_get_n_rows(result) > 0)
			axis_db_result_get_int64(result, 0, i, &values[i]);
		values[i] = CLAMP(values[i], 0, G_MAXINT);
	}
	activity->running = (gint)values[0];
	activity->queued = (gint)values[1];
	activity->searches = (gint)values[2];
	activity->imports = (gint)values[3];
	activity->scans = (gint)values[4];
	activity->conversions = (gint)values[5];
	activity->downloads = (gint)values[6];
	return TRUE;
}

gboolean
axis_web_jobs_count_active(
	AxisDbPool       *pool,
	AxisJobRegistry  *kinds,
	const gchar      *job_class,
	gint64           *running,
	gint64           *queued,
	GError          **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *upkeep = upkeep_literal(kinds);
	const gchar *params[2];

	*running = 0;
	*queued = 0;
	params[0] = upkeep;
	params[1] = g_strcmp0(job_class, "upkeep") == 0 ? "true" : "false";
	if (!axis_db_pool_exec(pool,
	        "SELECT count(*) FILTER (WHERE state = 'running'), count(*) FILTER (WHERE state = 'queued') "
	        "FROM job WHERE state IN ('queued', 'running') AND (kind = ANY ($1::text[])) = $2::boolean",
	        2, params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) > 0) {
		axis_db_result_get_int64(result, 0, 0, running);
		axis_db_result_get_int64(result, 0, 1, queued);
	}
	return TRUE;
}
