/*
 * axis-movies-ask.c - a person's click on a film's search, and what is in
 * hand for a film
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A film has two searches a person can ask for: Search now (the key
 * "movies.search:<id>", shared with the automatic ones) and a manual search
 * ("movies.search:<id>:list"), which only lists.  A click is answered with
 * the one of its kind queued or running, or with the one that ended less
 * than AXIS_ACQUIRE_SEARCH_AGAIN_S ago, and makes nothing new; the job key
 * still makes two clicks at the same moment one job.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

#include "jobs/axis-job-live.h"

static gchar *
key_of(
	gint64   movie_id,
	gboolean list
){
	return g_strdup_printf(AXIS_MOVIES_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT "%s", movie_id, list ? ":list" : "");
}

void
axis_movies_work_free(AxisMoviesWork *work)
{
	if (work == NULL)
		return;
	axis_job_live_free(work->search);
	axis_job_live_free(work->list);
	g_free(work->outcome);
	g_free(work->outcome_text);
	g_free(work);
}

/* Takes the job of @key out of @live, or %NULL. */
static AxisJobLive *
take(
	GPtrArray   *live,
	const gchar *key
){
	guint i;

	for (i = 0; live != NULL && i < live->len; i++) {
		AxisJobLive *job = g_ptr_array_index(live, i);

		if (g_strcmp0(job->key, key) == 0)
			return g_ptr_array_steal_index(live, i);
	}
	return NULL;
}

gboolean
axis_movies_get_work(
	AxisMovies       *self,
	gint64            movie_id,
	guint             ended_within_s,
	AxisMoviesWork  **work_out,
	GError          **error
){
	g_autoptr(GPtrArray) live = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *search = key_of(movie_id, FALSE);
	g_autofree gchar *list = key_of(movie_id, TRUE);
	g_autofree gchar *id = axis_movies_int_text(movie_id);
	g_autofree gchar *within = axis_movies_int_text(ended_within_s);
	const gchar *keys[3];
	const gchar *params[2];
	AxisMoviesWork *work;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	*work_out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	params[1] = within;
	/* Its two keys exactly ("movies.search:1" is also the start of film 10's). */
	keys[0] = search;
	keys[1] = list;
	keys[2] = NULL;
	ok = axis_job_live_read_keys(conn, keys, &live, error) &&
		axis_db_conn_exec(conn, "SELECT x.cause = 'list', (extract(epoch FROM x.finished_at) * 1000000)::bigint,"
			" x.outcome, x.outcome_text, x.n_found FROM (SELECT DISTINCT ON (s.cause = 'list') s.cause, s.finished_at,"
			"  s.outcome, s.outcome_text, s.n_found FROM movie_search s WHERE s.movie_id = $1::bigint"
			"  AND s.finished_at > now() - $2::integer * interval '1 second'"
			"  ORDER BY s.cause = 'list', s.finished_at DESC) x"
			" UNION ALL SELECT NULL, (extract(epoch FROM now()) * 1000000)::bigint, NULL, NULL, -1",
			2, params, &rows, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	work = g_new0(AxisMoviesWork, 1);
	work->search = take(live, search);
	work->list = take(live, list);
	work->now_us = g_get_real_time();
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gboolean is_list = FALSE;
		gint64 when = 0;
		gint64 found = 0;

		axis_db_result_get_int64(rows, i, 1, &when);
		if (axis_db_result_is_null(rows, i, 0)) {
			work->now_us = when;
			continue;
		}
		axis_db_result_get_boolean(rows, i, 0, &is_list);
		axis_db_result_get_int64(rows, i, 4, &found);
		if (is_list) {
			work->list_ended_us = when;
			work->list_n_found = (guint)MAX(found, 0);
			continue;
		}
		work->ended_us = when;
		work->outcome = g_strdup(axis_db_result_get_text(rows, i, 2));
		work->outcome_text = g_strdup(axis_db_result_get_text(rows, i, 3));
		work->n_found = (guint)MAX(found, 0);
	}
	*work_out = work;
	return TRUE;
}

gboolean
axis_movies_ask_search(
	AxisMovies       *self,
	gint64            movie_id,
	gboolean          list,
	const gchar      *user,
	AxisMoviesAsked  *asked,
	GError          **error
){
	g_autoptr(AxisMoviesWork) work = NULL;
	gboolean created = FALSE;
	gint64 ended;

	*asked = AXIS_MOVIES_ASKED_IN_HAND;
	if (!axis_movies_get_work(self, movie_id, AXIS_ACQUIRE_SEARCH_AGAIN_S, &work, error))
		return FALSE;
	/* In hand: one the scheduler queued is raised to a person's priority
	 * rather than waiting behind its backlog. */
	if ((list ? work->list : work->search) != NULL) {
		g_autofree gchar *key = key_of(movie_id, list);
		const gchar *params[1];

		params[0] = key;
		return axis_db_pool_exec(self->pool, "UPDATE job SET priority = 10 WHERE dedupe_key = $1 AND state = 'queued'"
			" AND priority < 10", 1, params, NULL, error);
	}
	ended = list ? work->list_ended_us : work->ended_us;
	if (ended > 0) {
		*asked = AXIS_MOVIES_ASKED_RECENT;
		return TRUE;
	}
	if (list ? !axis_movies_request_manual_search(self, movie_id, user, &created, error) :
	    !axis_movies_request_search(self, movie_id, "manual", user, &created, error))
		return FALSE;
	*asked = created ? AXIS_MOVIES_ASKED_QUEUED : AXIS_MOVIES_ASKED_IN_HAND;
	return TRUE;
}
