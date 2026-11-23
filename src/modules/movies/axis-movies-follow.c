/*
 * axis-movies-follow.c - after the grab (the movies.follow job, every
 * minute), and the scheduler of automatic searches (movies.schedule)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The follow-up is the acquisition layer's (axis_acquire_run_follow():
 * failures, stalls and downloads that are gone, completions, claims that
 * were never handed over), with the films' part after it: an import asked
 * for every completed grab (axis-movies-import.c).
 *
 * The scheduler picks the monitored films that are wanted, want an upgrade
 * or need attention, are available, have nothing in flight and whose next
 * search time has come; least recently searched first, at most
 * movies.searches_per_run.  It also notices a change of a film's
 * availability, which starts its floor over.  The floor itself (doubling
 * while searches find nothing) is the acquisition layer's pacing.
 */

#include "modules/movies/axis-movies-private.h"

#include <string.h>

static gint64
row_int(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

/* ── The follow-up ──────────────────────────────────────────────── */

gboolean
axis_movies_run_follow(
	AxisMovies             *self,
	AxisMovieFollowReport  *report,
	GError                **error
){
	return axis_acquire_run_follow(self->acquire, report, error);
}

/* ── The scheduler ──────────────────────────────────────────────── */

static gint
compare_due(
	gconstpointer a,
	gconstpointer b
){
	const AxisMovieFacts *x = *(const AxisMovieFacts *const *)a;
	const AxisMovieFacts *y = *(const AxisMovieFacts *const *)b;

	/* Never searched first, then the least recently searched; then the
	 * earliest added. */
	if (x->last_search_us != y->last_search_us)
		return x->last_search_us < y->last_search_us ? -1 : 1;
	if (x->added_us != y->added_us)
		return x->added_us < y->added_us ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

/* Notes what the scheduler sees of a film's availability; a change starts
 * its floor over and makes it due. */
static gboolean
note_availability(
	AxisMovies             *self,
	AxisDbConn             *conn,
	AxisMovieFacts         *facts,
	gboolean                available,
	AxisDbResult           *seen,
	guint                   row,
	AxisMovieScheduleReport *report,
	GError                **error
){
	g_autofree gchar *movie = axis_movies_int_text(facts->id);
	g_autofree gchar *base = axis_movies_int_text(axis_movies_setting_duration(self, AXIS_MOVIES_SETTING_FLOOR,
		6 * 3600));
	const gchar *seen_available = axis_db_result_get_text(seen, row, 1);
	const gchar *seen_date = axis_db_result_get_text(seen, row, 2);
	gboolean first = seen_available == NULL;
	gboolean changed = !first && ((strcmp(seen_available, "t") == 0) != available ||
		g_strcmp0(seen_date, facts->release_date) != 0);
	const gchar *params[5];

	if (!first && !changed)
		return TRUE;
	params[0] = movie;
	params[1] = available ? "true" : "false";
	params[2] = facts->release_date;
	params[3] = changed ? "true" : "false";
	params[4] = base;
	if (!axis_db_conn_exec(conn, "UPDATE movie SET seen_available = $2::boolean, seen_release_date = $3::date,"
	        " search_floor_s = CASE WHEN $4::boolean THEN $5::integer ELSE search_floor_s END,"
	        " next_search_at = CASE WHEN $4::boolean THEN NULL ELSE next_search_at END WHERE id = $1::bigint", 5,
	        params, NULL, error))
		return FALSE;
	if (!changed)
		return TRUE;
	facts->next_search_us = 0;
	report->became_available++;
	if (available)
		return axis_movies_store_eventf(conn, facts, "available", NULL, 0, 0, error, "It is available now (its "
			"search from: %s); automatic searches start over.",
			axis_movie_availability_label(facts->availability));
	return axis_movies_store_eventf(conn, facts, "available", NULL, 0, 0, error, "Its release date changed to %s: "
		"it is not available until then%s.", facts->release_date != NULL ? facts->release_date : "an unknown date",
		facts->availability == AXIS_MOVIE_RELEASED ? " and the time after it that counts as released" : "");
}

gboolean
axis_movies_run_schedule(
	AxisMovies               *self,
	AxisMovieScheduleReport  *report_out,
	GError                  **error
){
	g_autoptr(GPtrArray) films = NULL;
	g_autoptr(GPtrArray) due = g_ptr_array_new();
	g_autoptr(AxisDbResult) seen = NULL;
	g_autoptr(GHashTable) rows = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	AxisMovieScheduleReport report;
	AxisMovieRules rules;
	gint64 per_run = axis_movies_setting_integer(self, AXIS_MOVIES_SETTING_PER_RUN, 5);
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	memset(&report, 0, sizeof(report));
	axis_movies_get_rules(self, &rules);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	ok = axis_movies_store_facts(conn, AXIS_MOVIES_FACTS_MONITORED, NULL, &films, error) &&
		axis_db_conn_exec(conn, "SELECT id, seen_available, seen_release_date::text FROM movie WHERE monitored", 0, NULL,
			&seen, error);
	for (i = 0; ok && i < axis_db_result_get_n_rows(seen); i++) {
		gint64 *key = g_new(gint64, 1);

		*key = row_int(seen, i, 0);
		g_hash_table_insert(rows, key, GUINT_TO_POINTER(i + 1));
	}
	ok = ok && axis_db_conn_begin(conn, error);
	for (i = 0; ok && i < films->len; i++) {
		AxisMovieFacts *facts = g_ptr_array_index(films, i);
		gboolean available = axis_movie_available(facts, &rules, NULL);
		guint row = GPOINTER_TO_UINT(g_hash_table_lookup(rows, &facts->id));
		AxisMovieState state;

		report.considered++;
		if (row > 0)
			ok = note_availability(self, conn, facts, available, seen, row - 1, &report, error);
		if (!ok)
			break;
		state = axis_movie_derive(facts, &rules, NULL);
		if (state != AXIS_MOVIE_WANTED && state != AXIS_MOVIE_UPGRADE_WANTED && state != AXIS_MOVIE_ATTENTION)
			continue;
		if (!available || axis_movie_in_flight(facts) || facts->search_queued || facts->search_running ||
		    facts->root_id == 0 || !axis_volume_state_is_usable(facts->volume_state) || !facts->profile_usable)
			continue;
		if (facts->next_search_us > rules.now_us)
			continue;
		/* Brought over and not started: no search is queued for it, so the
		 * held movies take none of a run's searches from the others (the
		 * search job would skip it besides: load() in axis-movies-search.c). */
		if (axis_movie_hold_stops(facts->search_hold))
			continue;
		g_ptr_array_add(due, facts);
	}
	ok = ok && axis_db_conn_commit(conn, error);
	report.due = due->len;
	g_ptr_array_sort(due, compare_due);
	for (i = 0; ok && i < due->len && (gint64)report.queued < per_run; i++) {
		const AxisMovieFacts *facts = g_ptr_array_index(due, i);
		gboolean created = FALSE;

		ok = axis_movies_store_enqueue_search(conn, facts->id, "scheduled", NULL, NULL, &created, error);
		if (ok && created)
			report.queued++;
	}
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (report_out != NULL)
		*report_out = report;
	return ok;
}
