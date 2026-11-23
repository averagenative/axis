/*
 * axis-tv-follow.c - after the grab (the tv.follow job, every minute), and
 * the scheduler of automatic searches (tv.schedule): the series following
 * the metadata, then the searches of what is wanted
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The follow-up is the acquisition layer's, with the series' part after it
 * (an import asked for every completed grab, axis-tv-import.c).
 *
 * The scheduler, every run:
 *
 *  1. Series whose episodes have not followed the metadata for six hours
 *     (or whose seasons were fetched since) are synced, a few each run, and
 *     their seasons never fetched are asked for.
 *  2. The wanted episodes are those monitored, aired (air date plus
 *     tv.air_delay), not in the library (or below the cutoff), with nothing
 *     in flight, whose series can be searched for (a destination online, a
 *     usable profile) and whose next search time has come.  An episode with
 *     no air date is never wanted automatically.
 *  3. Per season, several wanted at once (at least two, and at least
 *     tv.pack_threshold percent of its aired monitored episodes) make one
 *     season search; otherwise each is searched by itself.
 *  4. The episodes aired in the last week go first, then the backlog; within
 *     each the least recently searched first.  At most tv.searches_per_run
 *     searches a run, and AXIS_TV_PER_SERIES_RUN of one series.
 */

#include "modules/tv/axis-tv-private.h"

#include <string.h>

gboolean
axis_tv_run_follow(
	AxisTv                   *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
){
	return axis_acquire_run_follow(self->acquire, report, error);
}

/* ── Planning ───────────────────────────────────────────────────── */

typedef struct
{
	gint64       series_id;
	AxisTvScope  scope;
	gint64       item;
	gboolean     recent;
	gint64       last_search_us;
	gchar       *latest_air;
} Planned;

static void
planned_free(Planned *p)
{
	g_free(p->latest_air);
	g_free(p);
}

static gint
compare_planned(
	gconstpointer a,
	gconstpointer b
){
	const Planned *x = *(const Planned *const *)a;
	const Planned *y = *(const Planned *const *)b;
	gint c;

	/* Recent first; then never searched, then the least recently; then the
	 * latest aired. */
	if (x->recent != y->recent)
		return x->recent ? -1 : 1;
	if (x->last_search_us != y->last_search_us)
		return x->last_search_us < y->last_search_us ? -1 : 1;
	c = g_strcmp0(y->latest_air, x->latest_air);
	if (c != 0)
		return c;
	if (x->series_id != y->series_id)
		return x->series_id < y->series_id ? -1 : 1;
	return x->item < y->item ? -1 : x->item > y->item;
}

static gboolean
series_searchable(const AxisTvSeries *s)
{
	return s->root_id > 0 && axis_volume_state_is_usable(s->volume_state) && s->profile_usable;
}

/* Wanted by the scheduler: monitored, aired, nothing in flight, nothing
 * queued for it, no file or one below the cutoff. */
static gboolean
scheduled_wanted(
	const AxisTvEpisode *e,
	const AxisTvSeries  *s,
	const AxisTvRules   *rules,
	gboolean             person
){
	if (e->gone || !e->monitored || !axis_tv_episode_aired(e, rules, NULL) || axis_tv_episode_in_flight(e) ||
	    e->search_queued || e->search_running)
		return FALSE;
	/* Brought over and held (the series, or this episode whose file Axis does
	 * not see): it takes none of a run's searches, and no plan counts it.  A
	 * person's own search (@person: Search now on a season of a held series,
	 * and what follows it when no pack is found) passes the series' hold, and
	 * never an episode's. */
	if (axis_tv_held(person ? NULL : s, e))
		return FALSE;
	return e->n_files == 0 || axis_tv_episode_upgradable(e, s);
}

/* @leave_out: (nullable) episode ids (gint64) not to search for now: they
 * still count among a season's aired episodes. */
static void
plan_one_series(
	const AxisTvSeries  *series,
	const GPtrArray     *episodes,
	const AxisTvRules   *rules,
	gboolean             only_due,
	gint64               threshold,
	gint                 only_season,
	GHashTable          *leave_out,
	gboolean             person,
	GPtrArray           *out
){
	g_autoptr(GArray) seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	gchar recent_from[11];
	guint i;
	guint k;

	axis_tv_today(rules->now_us - (gint64)AXIS_TV_RECENT_DAYS * 86400 * G_USEC_PER_SEC, recent_from);
	for (i = 0; i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		gboolean known = FALSE;

		if (e->series_id != series->id)
			continue;
		for (k = 0; k < seasons->len && !known; k++)
			known = g_array_index(seasons, gint, k) == e->season;
		if (!known)
			g_array_append_val(seasons, e->season);
	}
	for (k = 0; k < seasons->len; k++) {
		gint season = g_array_index(seasons, gint, k);
		g_autoptr(GPtrArray) due = g_ptr_array_new();
		guint aired_monitored = 0;

		if (only_season >= 0 && season != only_season)
			continue;
		for (i = 0; i < episodes->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(episodes, i);

			if (e->series_id != series->id || e->season != season || e->gone)
				continue;
			if (e->monitored && axis_tv_episode_aired(e, rules, NULL))
				aired_monitored++;
			if (!scheduled_wanted(e, series, rules, person))
				continue;
			if (only_due && e->next_search_us > rules->now_us)
				continue;
			if (leave_out != NULL && g_hash_table_contains(leave_out, &e->id))
				continue;
			g_ptr_array_add(due, (gpointer)e);
		}
		if (due->len == 0)
			continue;
		if (only_season < 0 && due->len >= 2 && (gint64)due->len * 100 >= threshold * (gint64)aired_monitored) {
			/* Several at once: the whole season first. */
			Planned *p = g_new0(Planned, 1);

			p->series_id = series->id;
			p->scope = AXIS_TV_SCOPE_SEASON;
			p->item = season;
			p->last_search_us = G_MAXINT64;
			for (i = 0; i < due->len; i++) {
				const AxisTvEpisode *e = g_ptr_array_index(due, i);

				p->recent = p->recent || g_strcmp0(e->air_date, recent_from) >= 0;
				p->last_search_us = MIN(p->last_search_us, e->last_search_us);
				if (g_strcmp0(e->air_date, p->latest_air) > 0) {
					g_free(p->latest_air);
					p->latest_air = g_strdup(e->air_date);
				}
			}
			g_ptr_array_add(out, p);
			continue;
		}
		for (i = 0; i < due->len; i++) {
			const AxisTvEpisode *e = g_ptr_array_index(due, i);
			Planned *p = g_new0(Planned, 1);

			p->series_id = series->id;
			p->scope = AXIS_TV_SCOPE_EPISODE;
			p->item = e->id;
			p->recent = g_strcmp0(e->air_date, recent_from) >= 0;
			p->last_search_us = e->last_search_us;
			p->latest_air = g_strdup(e->air_date);
			g_ptr_array_add(out, p);
		}
	}
}

static gboolean
enqueue_planned(
	AxisDbConn       *conn,
	GPtrArray        *planned,
	const gchar      *cause,
	const gchar      *user,
	guint             limit,
	guint             per_series,
	guint            *queued,
	guint            *seasons,
	GError          **error
){
	g_autoptr(GHashTable) counts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;

	g_ptr_array_sort(planned, compare_planned);
	for (i = 0; i < planned->len && *queued < limit; i++) {
		const Planned *p = g_ptr_array_index(planned, i);
		guint n = GPOINTER_TO_UINT(g_hash_table_lookup(counts, &p->series_id));
		gboolean created = FALSE;

		if (per_series > 0 && n >= per_series)
			continue;
		if (!axis_tv_store_enqueue_search(conn, p->series_id, p->scope, p->item, cause, user, &created, error))
			return FALSE;
		if (!created)
			continue;
		(*queued)++;
		if (p->scope == AXIS_TV_SCOPE_SEASON && seasons != NULL)
			(*seasons)++;
		{
			gint64 *key = g_new(gint64, 1);

			*key = p->series_id;
			g_hash_table_insert(counts, key, GUINT_TO_POINTER(n + 1));
		}
	}
	return TRUE;
}

static gboolean
read_one(
	AxisDbConn     *conn,
	gint64          series_id,
	AxisTvSeries  **series,
	GPtrArray     **episodes,
	GError        **error
){
	g_autofree gchar *ids = g_strdup_printf("{%" G_GINT64_FORMAT "}", series_id);

	*episodes = NULL;
	if (!axis_tv_store_series_one(conn, series_id, series, error))
		return FALSE;
	if (*series == NULL)
		return TRUE;
	return axis_tv_store_episodes(conn, ids, FALSE, episodes, error);
}

gboolean
axis_tv_plan_series(
	AxisTv       *self,
	AxisDbConn   *conn,
	gint64        series_id,
	const gchar  *cause,
	const gchar  *user,
	guint         limit,
	GHashTable   *leave_out,
	guint        *queued,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GPtrArray) planned = g_ptr_array_new_with_free_func((GDestroyNotify)planned_free);
	gint64 threshold = axis_tv_setting_integer(self, AXIS_TV_SETTING_PACK_SHARE, 50);
	AxisTvRules rules;
	guint seasons = 0;
	guint i;

	*queued = 0;
	axis_tv_get_rules(self, &rules);
	if (!read_one(conn, series_id, &series, &episodes, error))
		return FALSE;
	if (series == NULL || !series_searchable(series))
		return TRUE;
	plan_one_series(series, episodes, &rules, FALSE, threshold, -1, leave_out, axis_tv_cause_is_person(cause), planned);
	for (i = 0; i < planned->len; i++) {
		if (((Planned *)g_ptr_array_index(planned, i))->scope == AXIS_TV_SCOPE_SEASON)
			seasons++;
	}
	/* Several whole seasons wanted, nothing of the series in the library:
	 * a pack of several seasons, or of all of it, first. */
	if (seasons >= 2 && seasons == planned->len) {
		gboolean any_file = FALSE;
		gboolean created = FALSE;

		for (i = 0; i < episodes->len; i++)
			any_file = any_file || ((AxisTvEpisode *)g_ptr_array_index(episodes, i))->n_files > 0;
		if (!any_file) {
			if (!axis_tv_store_enqueue_search(conn, series_id, AXIS_TV_SCOPE_SERIES, 0, cause, user, &created, error))
				return FALSE;
			*queued = created ? 1 : 0;
			return TRUE;
		}
	}
	return enqueue_planned(conn, planned, cause, user, limit, 0, queued, NULL, error);
}

/* Each episode's search in hand read again for the fallback of a search
 * that found no pack: only the episode's own search holds it (a season's
 * or the series' takes packs only, and the one falling back is still
 * running).  For a person's search (@cause not "scheduled"), an own one
 * that is only queued holds nothing either: it is asked for again, which
 * raises it to a person's priority (axis_tv_store_enqueue_search()). */
static gboolean
release_from(
	AxisDbConn   *conn,
	gint64        series_id,
	const gchar  *cause,
	GPtrArray    *episodes,
	GError      **error
){
	g_autoptr(GPtrArray) live = NULL;
	g_autofree gchar *prefix = g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":", series_id);
	gboolean person = g_strcmp0(cause, "scheduled") != 0;
	guint i;

	if (!axis_job_live_read(conn, prefix, &live, error))
		return FALSE;
	for (i = 0; i < episodes->len; i++) {
		AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		g_autofree gchar *mine = axis_tv_ask_key(series_id, AXIS_TV_SCOPE_EPISODE, e->id, FALSE);
		const AxisJobLive *job = axis_job_live_find(live, mine);

		e->search_running = job != NULL && job->running;
		e->search_queued = job != NULL && !job->running && !person;
	}
	return TRUE;
}

gboolean
axis_tv_queue_narrower(
	AxisTv       *self,
	AxisDbConn   *conn,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *cause,
	const gchar  *user,
	guint        *queued,
	GError      **error
){
	g_autoptr(AxisTvSeries) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GPtrArray) planned = g_ptr_array_new_with_free_func((GDestroyNotify)planned_free);
	gint64 threshold = axis_tv_setting_integer(self, AXIS_TV_SETTING_PACK_SHARE, 50);
	AxisTvRules rules;

	*queued = 0;
	axis_tv_get_rules(self, &rules);
	if (!read_one(conn, series_id, &series, &episodes, error))
		return FALSE;
	if (series == NULL)
		return TRUE;
	/* The search falling back is still running (its job ends after this): it
	 * holds none of them now.  Without it every episode it would have held
	 * looked searched for, and nothing followed a season search that found
	 * no pack. */
	if (!release_from(conn, series_id, cause, episodes, error))
		return FALSE;
	/* A season: each of its wanted episodes; the series: each season, as the
	 * scheduler would. */
	plan_one_series(series, episodes, &rules, FALSE, threshold, scope == AXIS_TV_SCOPE_SEASON ? (gint)item : -1,
		NULL, axis_tv_cause_is_person(cause), planned);
	return enqueue_planned(conn, planned, cause, user, 100, 0, queued, NULL, error);
}

/* ── The scheduler ──────────────────────────────────────────────── */

gboolean
axis_tv_run_schedule(
	AxisTv                *self,
	AxisTvScheduleReport  *report_out,
	GError               **error
){
	g_autoptr(AxisDbResult) stale = NULL;
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GPtrArray) planned = g_ptr_array_new_with_free_func((GDestroyNotify)planned_free);
	g_autoptr(GString) ids = g_string_new("{");
	gint64 per_run = axis_tv_setting_integer(self, AXIS_TV_SETTING_PER_RUN, 5);
	gint64 threshold = axis_tv_setting_integer(self, AXIS_TV_SETTING_PACK_SHARE, 50);
	AxisTvScheduleReport report;
	AxisTvRules rules;
	AxisDbConn *conn;
	gboolean ok;
	guint i;
	guint k;

	memset(&report, 0, sizeof(report));
	/* 1. The series follow the metadata. */
	if (!axis_db_pool_exec(self->pool, "SELECT s.id FROM series s WHERE s.synced_at IS NULL"
	        " OR s.synced_at < now() - interval '6 hours' OR EXISTS (SELECT 1 FROM meta_season ms"
	        " WHERE ms.title_id = s.title_id AND ms.source = s.episode_source AND ms.episodes_fetched_at > s.synced_at)"
	        " ORDER BY s.synced_at NULLS FIRST, s.id LIMIT 5", 0, NULL, &stale, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(stale); i++) {
		g_autoptr(GError) local = NULL;
		gint64 id = 0;

		axis_db_result_get_int64(stale, i, 0, &id);
		if (axis_tv_sync(self, id, TRUE, NULL, &local))
			report.synced++;
		else if (!g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND))
			g_message("series: series %" G_GINT64_FORMAT " could not follow the metadata: %s", id, local->message);
	}

	/* 2. to 4. The searches. */
	axis_tv_get_rules(self, &rules);
	conn = axis_db_pool_acquire(self->pool, error);
	if (conn == NULL)
		return FALSE;
	/* An episode held because Axis did not see the other app's file, which
	 * has a file by now (identification, an import, a link by hand): the mark
	 * is let go here, the one place, so that it does not come back to stop a
	 * search when that file is replaced or removed later.  (No reader counts
	 * it held in the meantime: AXIS_TV_EPISODE_HELD_SQL.)  The held are few:
	 * a partial index finds them. */
	ok = axis_db_conn_exec(conn, "UPDATE series_episode e SET search_hold = NULL WHERE e.search_hold IS NOT NULL"
		" AND EXISTS (SELECT 1 FROM series_file_episode sfe JOIN library_file lf ON lf.id = sfe.file_id"
		" AND lf.state = 'present' JOIN series_file sf ON sf.file_id = sfe.file_id"
		" WHERE sfe.episode_id = e.id AND (sf.kept OR sf.hand_at IS NOT NULL OR sf.import_id IS NOT NULL))",
		0, NULL, NULL, error);
	/* (The series as a list reads them: nothing here reads what only a
	 * series' own page shows, and that costs a look-up or three a series.) */
	ok = ok && axis_tv_store_series(conn, AXIS_TV_SERIES_LIST, NULL, &series, error);
	for (i = 0; ok && i < series->len; i++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			((AxisTvSeries *)g_ptr_array_index(series, i))->id);
	g_string_append_c(ids, '}');
	ok = ok && axis_tv_store_episodes(conn, ids->str, FALSE, &episodes, error);
	for (i = 0; ok && i < series->len; i++) {
		const AxisTvSeries *s = g_ptr_array_index(series, i);

		if (!series_searchable(s))
			continue;
		for (k = 0; k < episodes->len; k++) {
			const AxisTvEpisode *e = g_ptr_array_index(episodes, k);

			if (e->series_id != s->id || !scheduled_wanted(e, s, &rules, FALSE))
				continue;
			report.considered++;
			if (e->next_search_us <= rules.now_us)
				report.due++;
		}
		plan_one_series(s, episodes, &rules, TRUE, threshold, -1, NULL, FALSE, planned);
	}
	ok = ok && enqueue_planned(conn, planned, "scheduled", NULL, (guint)MAX(per_run, 0), AXIS_TV_PER_SERIES_RUN,
		&report.queued, &report.seasons, error);
	axis_db_pool_release(self->pool, conn);
	if (report_out != NULL)
		*report_out = report;
	return ok;
}
