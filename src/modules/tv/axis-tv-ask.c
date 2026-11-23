/*
 * axis-tv-ask.c - a person's click on a search: what it makes, and the work
 * in hand that answers it instead; what a series' pages show of its searches
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A search a person asks for is made unless that very search (the same
 * key: scope, item, and family) is in hand or just done:
 *
 *  - queued or running: the click is answered with that one
 *    (AXIS_TV_ASKED_IN_HAND), and a queued one the scheduler made (priority
 *    0) is raised to a person's priority rather than waiting behind the
 *    scheduler's backlog;
 *  - ended less than AXIS_ACQUIRE_SEARCH_AGAIN_S ago: the click is answered
 *    with what it found (AXIS_TV_ASKED_RECENT).
 *
 * A wider search in hand holds nothing back: a season's search takes whole
 * season packs only, the series' packs only, so an episode's search asked
 * meanwhile is made (the indexer's cache makes the repeated question
 * cheap).  With the indexer layer's own limits (a limiter per source, the
 * daily allowance and its reserve, the hold after a refusal) and the
 * searches' queue taking a few at a time, clicking never reaches an
 * indexer's limits.
 *
 * Automatic searches and manual searches (which only list what was found)
 * are two families: neither holds the other.  Search all wanted plans as
 * the scheduler does (which leaves out what a search in hand will take up,
 * the fallback of a season's search to its episodes included), leaves out
 * what was searched a moment ago, raises what the scheduler queued, and
 * says which when nothing new is left.
 *
 * Everything here is read and decided in one transaction with the series'
 * row locked, so two clicks at once are one after the other.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

#include "jobs/axis-job-live.h"

/* ── Keys ───────────────────────────────────────────────────────── */

gchar *
axis_tv_ask_key(
	gint64       series_id,
	AxisTvScope  scope,
	gint64       item,
	gboolean     list
){
	if (scope == AXIS_TV_SCOPE_SERIES)
		item = 0;
	return list ? axis_tv_manual_key(series_id, scope, item) : axis_tv_search_key(series_id, scope, item);
}

gboolean
axis_tv_key_parse(
	const gchar  *key,
	gint64       *series_id,
	AxisTvScope  *scope,
	gint64       *item,
	gboolean     *list
){
	g_auto(GStrv) parts = NULL;
	guint n;
	guint at = 2;

	if (key == NULL || !g_str_has_prefix(key, AXIS_TV_SEARCH_KEY_PREFIX))
		return FALSE;
	parts = g_strsplit(key + strlen(AXIS_TV_SEARCH_KEY_PREFIX), ":", 5);
	n = g_strv_length(parts);
	if (n < 2 || !g_ascii_string_to_signed(parts[0], 10, 1, G_MAXINT64, series_id, NULL))
		return FALSE;
	*list = strcmp(parts[1], "list") == 0;
	if (!*list)
		at = 1;
	if (n <= at || !axis_tv_scope_from_name(parts[at], scope))
		return FALSE;
	*item = 0;
	if (*scope == AXIS_TV_SCOPE_SERIES)
		return n == at + 1;
	return n == at + 2 && g_ascii_string_to_signed(parts[at + 1], 10, 0, G_MAXINT64, item, NULL);
}

static gchar *
live_prefix(gint64 series_id)
{
	return g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":", series_id);
}

/* ── The work in hand ───────────────────────────────────────────── */

static void
ended_free(AxisTvEnded *ended)
{
	if (ended == NULL)
		return;
	g_free(ended->key);
	g_free(ended->outcome);
	g_free(ended->outcome_text);
	g_free(ended);
}

void
axis_tv_work_free(AxisTvWork *work)
{
	if (work == NULL)
		return;
	g_clear_pointer(&work->live, g_ptr_array_unref);
	g_clear_pointer(&work->ended, g_ptr_array_unref);
	g_free(work);
}

gboolean
axis_tv_get_work(
	AxisTv       *self,
	gint64        series_id,
	guint         ended_within_s,
	AxisTvWork  **work_out,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *prefix = live_prefix(series_id);
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autofree gchar *within = axis_tv_int_text(ended_within_s);
	AxisTvWork *work;
	AxisDbConn *conn;
	const gchar *params[2];
	gboolean ok;
	guint i;

	*work_out = NULL;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	work = g_new0(AxisTvWork, 1);
	params[0] = id;
	params[1] = within;
	/* Of each key, the latest that ended; and the database's clock. */
	ok = axis_job_live_read(conn, prefix, &work->live, error) &&
		axis_db_conn_exec(conn, "SELECT x.job_key, x.cause = 'list', (extract(epoch FROM x.finished_at) * 1000000)::bigint,"
			" x.outcome, x.outcome_text, x.n_found, x.n_accepted, (extract(epoch FROM now()) * 1000000)::bigint"
			" FROM (SELECT DISTINCT ON (s.job_key) s.job_key, s.cause, s.finished_at, s.outcome, s.outcome_text,"
			"   s.n_found, s.n_accepted FROM series_search s WHERE s.series_id = $1::bigint AND s.job_key IS NOT NULL"
			"   AND s.finished_at > now() - $2::integer * interval '1 second'"
			"   ORDER BY s.job_key, s.finished_at DESC) x"
			" UNION ALL SELECT NULL, false, 0, NULL, NULL, 0, 0, (extract(epoch FROM now()) * 1000000)::bigint"
			" ORDER BY 3 DESC", 2, params, &rows, error);
	axis_db_pool_release(self->pool, conn);
	if (!ok) {
		axis_tv_work_free(work);
		return FALSE;
	}
	work->ended = g_ptr_array_new_with_free_func((GDestroyNotify)ended_free);
	work->now_us = g_get_real_time();
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisTvEnded *ended;
		gint64 value = 0;

		if (axis_db_result_get_int64(rows, i, 7, &value))
			work->now_us = value;
		if (axis_db_result_is_null(rows, i, 0))
			continue;
		ended = g_new0(AxisTvEnded, 1);
		ended->key = g_strdup(axis_db_result_get_text(rows, i, 0));
		axis_db_result_get_boolean(rows, i, 1, &ended->list);
		axis_db_result_get_int64(rows, i, 2, &ended->finished_us);
		ended->outcome = g_strdup(axis_db_result_get_text(rows, i, 3));
		ended->outcome_text = g_strdup(axis_db_result_get_text(rows, i, 4));
		if (axis_db_result_get_int64(rows, i, 5, &value))
			ended->n_found = (guint)MAX(value, 0);
		if (axis_db_result_get_int64(rows, i, 6, &value))
			ended->n_accepted = (guint)MAX(value, 0);
		g_ptr_array_add(work->ended, ended);
	}
	*work_out = work;
	return TRUE;
}

const AxisJobLive *
axis_tv_work_in_hand(
	const AxisTvWork *work,
	gint64            series_id,
	AxisTvScope       scope,
	gint64            item,
	gboolean          list
){
	g_autofree gchar *key = NULL;

	if (work == NULL || work->live == NULL || work->live->len == 0)
		return NULL;
	key = axis_tv_ask_key(series_id, scope, item, list);
	return axis_job_live_find(work->live, key);
}

const AxisTvEnded *
axis_tv_work_ended(
	const AxisTvWork *work,
	gint64            series_id,
	AxisTvScope       scope,
	gint64            item,
	gboolean          list
){
	g_autofree gchar *key = NULL;
	const AxisTvEnded *latest = NULL;
	guint i;

	if (work == NULL || work->ended == NULL || work->ended->len == 0)
		return NULL;
	key = axis_tv_ask_key(series_id, scope, item, list);
	for (i = 0; i < work->ended->len; i++) {
		const AxisTvEnded *ended = g_ptr_array_index(work->ended, i);

		if (g_strcmp0(ended->key, key) == 0 && (latest == NULL || ended->finished_us > latest->finished_us))
			latest = ended;
	}
	return latest;
}

/* ── Asking ─────────────────────────────────────────────────────── */

/* A person's priority (axis_tv_store_enqueue_search()). */
#define PERSON_PRIORITY "10"

/* Whether the search of @key is queued or running; a queued one at a lower
 * priority (the scheduler's) is raised to a person's. */
static gboolean
in_hand(
	AxisDbConn   *conn,
	const gchar  *key,
	gboolean     *found,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[1];

	params[0] = key;
	*found = FALSE;
	if (!axis_db_conn_exec(conn, "SELECT state::text FROM job WHERE dedupe_key = $1 AND state IN ('queued', 'running')",
	        1, params, &rows, error))
		return FALSE;
	*found = axis_db_result_get_n_rows(rows) > 0;
	return !*found || axis_db_conn_exec(conn, "UPDATE job SET priority = " PERSON_PRIORITY " WHERE dedupe_key = $1"
		" AND state = 'queued' AND priority < " PERSON_PRIORITY, 1, params, NULL, error);
}

/* Whether the search of @key ended a moment ago. */
static gboolean
recent(
	AxisDbConn   *conn,
	gint64        series_id,
	const gchar  *key,
	gboolean     *found,
	GError      **error
){
	g_autoptr(AxisDbResult) rows = NULL;
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autofree gchar *again = axis_tv_int_text(AXIS_ACQUIRE_SEARCH_AGAIN_S);
	const gchar *params[3];

	params[0] = id;
	params[1] = key;
	params[2] = again;
	*found = FALSE;
	if (!axis_db_conn_exec(conn, "SELECT 1 FROM series_search WHERE series_id = $1::bigint"
	        " AND job_key = $2 AND finished_at > now() - $3::integer * interval '1 second' LIMIT 1",
	        3, params, &rows, error))
		return FALSE;
	*found = axis_db_result_get_n_rows(rows) > 0;
	return TRUE;
}

/* The scheduler's queued searches of the series (its automatic family)
 * raised to a person's priority: Search all wanted does not wait behind the
 * backlog for what the scheduler planned already. */
static gboolean
raise_series(
	AxisDbConn  *conn,
	gint64       series_id,
	GError     **error
){
	g_autofree gchar *prefix = live_prefix(series_id);
	g_autofree gchar *manual = g_strconcat(prefix, "list:", NULL);
	const gchar *params[2];

	params[0] = prefix;
	params[1] = manual;
	return axis_db_conn_exec(conn, "UPDATE job SET priority = " PERSON_PRIORITY " WHERE state = 'queued'"
		" AND priority < " PERSON_PRIORITY " AND starts_with(dedupe_key, $1) AND NOT starts_with(dedupe_key, $2)",
		2, params, NULL, error);
}

static gboolean
lock_series(
	AxisDbConn     *conn,
	gint64          series_id,
	AxisTvSeries  **series,
	GError        **error
){
	gboolean found = FALSE;

	*series = NULL;
	if (!axis_tv_store_lock(conn, series_id, &found, error))
		return FALSE;
	if (!found || !axis_tv_store_series_one(conn, series_id, series, error) || *series == NULL) {
		if (error != NULL && *error == NULL)
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "there is no such series");
		return FALSE;
	}
	return TRUE;
}

/* The wanted episodes Search all wanted leaves out: held by a search in
 * hand, or searched a moment ago (into @leave_out). */
static gboolean
all_held(
	AxisTv      *self,
	AxisDbConn  *conn,
	gint64       series_id,
	GHashTable  *leave_out,
	guint       *held,
	guint       *searched,
	GError     **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) lately = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autofree gchar *again = axis_tv_int_text(AXIS_ACQUIRE_SEARCH_AGAIN_S);
	AxisTvRules rules;
	const gchar *params[2];
	guint i;

	*held = 0;
	*searched = 0;
	params[0] = id;
	params[1] = again;
	if (!axis_tv_store_series_one(conn, series_id, &series, error) || series == NULL ||
	    !axis_tv_store_episodes(conn, ids, FALSE, &episodes, error) ||
	    !axis_db_conn_exec(conn, "SELECT id FROM series_episode WHERE series_id = $1::bigint AND gone_at IS NULL"
	        " AND last_search_at > now() - $2::integer * interval '1 second'", 2, params, &rows, error))
		return series == NULL && (error == NULL || *error == NULL);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		gint64 *episode = g_new(gint64, 1);

		axis_db_result_get_int64(rows, i, 0, episode);
		g_hash_table_add(leave_out, episode);
		g_hash_table_add(lately, episode);
	}
	axis_tv_get_rules(self, &rules);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

		if (e->gone || !e->monitored || !axis_tv_episode_aired(e, &rules, NULL) || axis_tv_episode_in_flight(e) ||
		    axis_tv_hold_stops(e->search_hold) || !(e->n_files == 0 || axis_tv_episode_upgradable(e, series)))
			continue;
		if (e->search_queued || e->search_running)
			(*held)++;
		else if (g_hash_table_contains(lately, &e->id))
			(*searched)++;
	}
	return TRUE;
}

static gboolean
ask_all(
	AxisTv        *self,
	AxisDbConn    *conn,
	AxisTvSeries  *series,
	const gchar   *user,
	AxisTvAsked   *asked,
	guint         *queued,
	GError       **error
){
	g_autoptr(GHashTable) leave_out = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint held = 0;
	guint searched = 0;

	if (!raise_series(conn, series->id, error) ||
	    !all_held(self, conn, series->id, leave_out, &held, &searched, error) ||
	    !axis_tv_plan_series(self, conn, series->id, "manual", user, 50, leave_out, queued, error))
		return FALSE;
	if (*queued > 0) {
		*asked = AXIS_TV_ASKED_QUEUED;
		return axis_tv_store_eventf(conn, series, 0, "searched", user, error, "A search of every wanted episode "
			"asked for%s%s: %u %s queued.", user != NULL ? " by " : "", user != NULL ? user : "", *queued,
			*queued == 1 ? "search" : "searches");
	}
	*asked = held > 0 ? AXIS_TV_ASKED_IN_HAND : searched > 0 ? AXIS_TV_ASKED_RECENT : AXIS_TV_ASKED_NOTHING;
	return TRUE;
}

gboolean
axis_tv_ask_search(
	AxisTv       *self,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	gboolean      list,
	const gchar  *user,
	AxisTvAsked  *asked,
	guint        *queued_out,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(AxisDbResult) row = NULL;
	g_autofree gchar *series_text = axis_tv_int_text(series_id);
	g_autofree gchar *item_text = axis_tv_int_text(item);
	g_autofree gchar *what = NULL;
	g_autofree gchar *key = NULL;
	const gchar *params[2];
	AxisDbConn *conn;
	gboolean found = FALSE;
	gboolean made = FALSE;
	guint queued = 0;
	gboolean ok;

	*asked = AXIS_TV_ASKED_NOTHING;
	if (queued_out != NULL)
		*queued_out = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* A series brought over and held: a person's search passes the hold by its
	 * cause.  Search now ends the hold of the one episode it names, or of the
	 * whole series when it is for all of it; a manual search only lists, and
	 * ends nothing. */
	ok = axis_db_conn_begin(conn, error) && lock_series(conn, series_id, &series, error) &&
		(list || axis_tv_store_end_hold(conn, series, scope, item, user, "Search now", error));
	params[0] = series_text;
	params[1] = item_text;
	if (ok && scope == AXIS_TV_SCOPE_EPISODE) {
		ok = axis_db_conn_exec(conn, "SELECT season_number, number FROM series_episode WHERE series_id = $1::bigint"
			" AND id = $2::bigint AND gone_at IS NULL", 2, params, &row, error);
		if (ok && axis_db_result_get_n_rows(row) == 0) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such episode");
			ok = FALSE;
		}
		if (ok) {
			gint64 number = 0;
			gint64 season = 0;

			axis_db_result_get_int64(row, 0, 0, &season);
			axis_db_result_get_int64(row, 0, 1, &number);
			what = axis_tv_episode_label((gint)season, (gint)number);
		}
	} else if (ok && scope == AXIS_TV_SCOPE_SEASON) {
		/* (Compared as a bigint: a number past a season's range is no
		 * season, not an error.) */
		ok = axis_db_conn_exec(conn, "SELECT 1 FROM series_episode WHERE series_id = $1::bigint"
			" AND season_number = $2::bigint AND gone_at IS NULL LIMIT 1", 2, params, &row, error);
		if (ok && axis_db_result_get_n_rows(row) == 0) {
			g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND, "the series has no such season");
			ok = FALSE;
		}
		what = item == 0 ? g_strdup("the specials") : g_strdup_printf("season %" G_GINT64_FORMAT, item);
	} else {
		item = 0;
		what = g_strdup("the whole series");
	}
	if (ok && scope == AXIS_TV_SCOPE_SERIES && !list) {
		ok = ask_all(self, conn, series, user, asked, &queued, error);
	} else if (ok) {
		/* That very search in hand, or just done; a wider one holds nothing
		 * back. */
		key = axis_tv_ask_key(series_id, scope, item, list);
		ok = in_hand(conn, key, &found, error);
		if (ok && found)
			*asked = AXIS_TV_ASKED_IN_HAND;
		ok = ok && (found || recent(conn, series_id, key, &found, error));
		if (ok && found && *asked != AXIS_TV_ASKED_IN_HAND)
			*asked = AXIS_TV_ASKED_RECENT;
		ok = ok && (found || axis_tv_store_enqueue_search(conn, series_id, scope, item, list ? "list" : "manual", user,
			&made, error));
		if (ok && !found) {
			*asked = made ? AXIS_TV_ASKED_QUEUED : AXIS_TV_ASKED_IN_HAND;
			queued = made ? 1 : 0;
		}
		if (ok && made && list)
			ok = axis_tv_store_eventf(conn, series, scope == AXIS_TV_SCOPE_EPISODE ? item : 0, "searched", user, error,
				"A manual search of %s asked for%s%s: it lists what the indexers have and grabs nothing by itself.",
				what, user != NULL ? " by " : "", user != NULL ? user : "");
		else if (ok && made)
			ok = axis_tv_store_eventf(conn, series, scope == AXIS_TV_SCOPE_EPISODE ? item : 0, "searched", user, error,
				"A search of %s asked for%s%s.", what, user != NULL ? " by " : "", user != NULL ? user : "");
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (ok && queued_out != NULL)
		*queued_out = queued;
	return ok;
}

gboolean
axis_tv_ask_undated(
	AxisTv       *self,
	gint64        series_id,
	gint          season,
	const gchar  *user,
	AxisTvAsked  *asked,
	guint        *queued_out,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(AxisDbResult) rows = NULL;
	g_autoptr(GHashTable) lately = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autofree gchar *again = axis_tv_int_text(AXIS_ACQUIRE_SEARCH_AGAIN_S);
	const gchar *params[2];
	AxisDbConn *conn;
	guint found = 0;
	guint held = 0;
	guint searched = 0;
	guint queued = 0;
	gboolean ok;
	guint i;

	*asked = AXIS_TV_ASKED_NOTHING;
	if (queued_out != NULL)
		*queued_out = 0;
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	params[0] = id;
	params[1] = again;
	/* (A held series: these searches are a person's, and pass by their cause;
	 * an episode held apart is left out below.) */
	ok = axis_db_conn_begin(conn, error) && lock_series(conn, series_id, &series, error) &&
		axis_tv_store_episodes(conn, ids, FALSE, &episodes, error) &&
		axis_db_conn_exec(conn, "SELECT id FROM series_episode WHERE series_id = $1::bigint"
			" AND last_search_at > now() - $2::integer * interval '1 second'", 2, params, &rows, error);
	for (i = 0; ok && i < axis_db_result_get_n_rows(rows); i++) {
		gint64 *episode = g_new(gint64, 1);

		axis_db_result_get_int64(rows, i, 0, episode);
		g_hash_table_add(lately, episode);
	}
	/* Each by its own search: the season's or the series' (packs only) never
	 * takes an episode without an air date. */
	for (i = 0; ok && i < episodes->len && queued < 100; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		g_autofree gchar *key = NULL;
		gboolean created = FALSE;
		gboolean mine = FALSE;

		if (e->season != season || e->gone || !e->monitored || e->air_date != NULL || e->n_files > 0 ||
		    axis_tv_episode_in_flight(e) || axis_tv_hold_stops(e->search_hold))
			continue;
		found++;
		key = axis_tv_search_key(series_id, AXIS_TV_SCOPE_EPISODE, e->id);
		ok = in_hand(conn, key, &mine, error);
		if (!ok || mine) {
			held += mine ? 1 : 0;
			continue;
		}
		if (g_hash_table_contains(lately, &e->id)) {
			searched++;
			continue;
		}
		ok = axis_tv_store_enqueue_search(conn, series_id, AXIS_TV_SCOPE_EPISODE, e->id, "manual", user, &created,
			error);
		queued += created ? 1 : 0;
		held += created ? 0 : 1;
	}
	if (ok && found == 0) {
		g_set_error_literal(error, AXIS_TV_ERROR, AXIS_TV_ERROR_INVALID, "no monitored episode of that season is "
			"missing without an air date");
		ok = FALSE;
	}
	if (ok && queued > 0) {
		g_autofree gchar *where = season == 0 ? g_strdup("the specials") : g_strdup_printf("season %d", season);

		ok = axis_tv_store_eventf(conn, series, 0, "searched", user, error, "A search of the %u %s of %s with no air "
			"date asked for%s%s: %u %s queued.", found, found == 1 ? "episode" : "episodes", where,
			user != NULL ? " by " : "", user != NULL ? user : "", queued, queued == 1 ? "search" : "searches");
	}
	ok = ok && axis_db_conn_commit(conn, error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
	if (!ok)
		return FALSE;
	*asked = queued > 0 ? AXIS_TV_ASKED_QUEUED : held > 0 ? AXIS_TV_ASKED_IN_HAND : searched > 0 ?
		AXIS_TV_ASKED_RECENT : AXIS_TV_ASKED_NOTHING;
	if (queued_out != NULL)
		*queued_out = queued;
	return TRUE;
}
