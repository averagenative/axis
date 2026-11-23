/*
 * axis-job-live.c - the jobs queued or running for something a page shows
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "jobs/axis-job-live.h"

#include <string.h>

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

void
axis_job_live_free(AxisJobLive *live)
{
	if (live == NULL)
		return;
	g_free(live->kind);
	g_free(live->key);
	g_free(live->message);
	g_free(live);
}

/* The live jobs are few, and the partial index of the keys holds only them:
 * the prefix or the keys are matched there, never over the finished jobs.
 * Ahead of a queued job is every queued job of its queue the claim takes
 * first. */
#define LIVE_SELECT(where) \
	"SELECT j.id, j.kind, j.dedupe_key, j.state = 'running', " US("j.created_at") ", " \
	US("coalesce(j.started_at, 'epoch'::timestamptz)") ", " US("j.run_at") ", j.progress::text, j.progress_message," \
	" CASE WHEN j.state = 'queued' THEN (SELECT count(*) FROM job o WHERE o.queue = j.queue AND o.state = 'queued'" \
	"   AND o.id <> j.id AND (o.priority > j.priority OR (o.priority = j.priority AND (o.run_at < j.run_at" \
	"   OR (o.run_at = j.run_at AND o.id < j.id))))) ELSE 0 END" \
	" FROM job j WHERE j.dedupe_key IS NOT NULL AND j.state IN ('queued', 'running') AND " where \
	" ORDER BY j.state = 'running' DESC, j.id LIMIT 500"

static void
take_rows(
	AxisDbResult  *rows,
	GPtrArray    **live
);

gboolean
axis_job_live_read(
	AxisDbConn   *conn,
	const gchar  *prefix,
	GPtrArray   **live,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[1];

	g_return_val_if_fail(prefix != NULL && prefix[0] != '\0', FALSE);

	*live = NULL;
	params[0] = prefix;
	if (!axis_db_conn_exec(conn, LIVE_SELECT("starts_with(j.dedupe_key, $1)"), 1, params, &rows, error))
		return FALSE;
	take_rows(rows, live);
	return TRUE;
}

gboolean
axis_job_live_read_keys(
	AxisDbConn          *conn,
	const gchar *const  *keys,
	GPtrArray          **live,
	GError             **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GString) list = g_string_new("{");
	const gchar *params[1];
	guint i;

	*live = NULL;
	for (i = 0; keys != NULL && keys[i] != NULL; i++) {
		const gchar *c;

		g_string_append(list, i > 0 ? ",\"" : "\"");
		for (c = keys[i]; *c != '\0'; c++) {
			if (*c == '"' || *c == '\\')
				g_string_append_c(list, '\\');
			g_string_append_c(list, *c);
		}
		g_string_append_c(list, '"');
	}
	g_string_append_c(list, '}');
	params[0] = list->str;
	if (!axis_db_conn_exec(conn, LIVE_SELECT("j.dedupe_key = ANY ($1::text[])"), 1, params, &rows, error))
		return FALSE;
	take_rows(rows, live);
	return TRUE;
}

static void
take_rows(
	AxisDbResult  *rows,
	GPtrArray    **live
){
	guint i;

	*live = g_ptr_array_new_with_free_func((GDestroyNotify)axis_job_live_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisJobLive *job = g_new0(AxisJobLive, 1);
		const gchar *progress = axis_db_result_get_text(rows, i, 7);
		gint64 value = 0;

		axis_db_result_get_int64(rows, i, 0, &job->id);
		job->kind = g_strdup(axis_db_result_get_text(rows, i, 1));
		job->key = g_strdup(axis_db_result_get_text(rows, i, 2));
		axis_db_result_get_boolean(rows, i, 3, &job->running);
		axis_db_result_get_int64(rows, i, 4, &job->queued_us);
		axis_db_result_get_int64(rows, i, 5, &job->started_us);
		axis_db_result_get_int64(rows, i, 6, &job->run_at_us);
		job->progress = progress != NULL ? CLAMP(g_ascii_strtod(progress, NULL), 0.0, 1.0) : -1.0;
		job->message = g_strdup(axis_db_result_get_text(rows, i, 8));
		if (axis_db_result_get_int64(rows, i, 9, &value))
			job->ahead = (guint)MAX(value, 0);
		if (!job->running)
			job->started_us = 0;
		g_ptr_array_add(*live, job);
	}
}

gboolean
axis_job_live_read_pool(
	AxisDbPool          *pool,
	const gchar *const  *keys,
	GPtrArray          **live,
	GError             **error
){
	AxisDbConn *conn = axis_db_pool_acquire(pool, error);
	gboolean ok;

	*live = NULL;
	if (conn == NULL)
		return FALSE;
	ok = axis_job_live_read_keys(conn, keys, live, error);
	axis_db_pool_release(pool, conn);
	return ok;
}

const AxisJobLive *
axis_job_live_find(
	const GPtrArray *live,
	const gchar     *key
){
	guint i;

	for (i = 0; live != NULL && key != NULL && i < live->len; i++) {
		const AxisJobLive *job = g_ptr_array_index(live, i);

		if (g_strcmp0(job->key, key) == 0)
			return job;
	}
	return NULL;
}
