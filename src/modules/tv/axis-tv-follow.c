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
 *     (or whose seasons were fetched since) are synced, and their seasons
 *     never fetched are asked for: each run as many as go round every
 *     series in those six hours (axis_tv_sync_batch()), the longest behind
 *     first.
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
 *
 * A run reads the episodes only of the series that have one that can be due
 * (axis_tv_store_schedule_series() asks the database which), a page of
 * AXIS_TV_SCHEDULE_PAGE series at a time, and keeps of what it plans only
 * what it can still queue: its time follows what is due and its memory a
 * page, not the library.
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

gboolean
axis_tv_prune_searches(
	AxisTv  *self,
	guint   *deleted,
	GError **error
){
	return axis_acquire_prune_searches(self->acquire, TRUE, deleted, error);
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
	if (p == NULL)
		return;
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

guint
axis_tv_sync_batch(gint64 n_series)
{
	gint64 n = (MAX(n_series, 0) * AXIS_TV_SCHEDULE_INTERVAL_S + AXIS_TV_SYNC_CYCLE_S - 1) / AXIS_TV_SYNC_CYCLE_S;

	return (guint)CLAMP(n, AXIS_TV_SYNC_MIN, AXIS_TV_SYNC_MAX);
}

void
axis_tv_set_schedule_page(
	AxisTv *self,
	guint   series
){
	g_atomic_int_set(&self->schedule_page, (gint)MIN(series, 10000));
}

/*
 * What is planned so far, cut to what a run can still queue of it: the best
 * few of each series (a run takes AXIS_TV_PER_SERIES_RUN of one), and of
 * those the best few times the run's limit.  The plan of a library of any
 * size is then no longer than that.  Two more of a series and twice the
 * limit and some are kept than the run takes: a search asked for by someone
 * else between the reading and the queueing is passed over, and the next
 * takes its place, as it did when every plan was kept.
 */
static void
keep_best(
	GPtrArray *planned,
	guint      limit
){
	g_autoptr(GHashTable) counts = g_hash_table_new(g_int64_hash, g_int64_equal);
	guint most = limit * 2 + 32;
	guint kept = 0;
	guint i;

	if (planned->len <= most)
		return;
	g_ptr_array_sort(planned, compare_planned);
	for (i = 0; i < planned->len; i++) {
		Planned *p = g_ptr_array_index(planned, i);
		guint n = GPOINTER_TO_UINT(g_hash_table_lookup(counts, &p->series_id));

		planned->pdata[i] = NULL;
		if (kept >= most || n >= AXIS_TV_PER_SERIES_RUN + 2) {
			planned_free(p);
			continue;
		}
		/* (The key is the kept plan's own field: it lives as long.) */
		g_hash_table_insert(counts, &p->series_id, GUINT_TO_POINTER(n + 1));
		planned->pdata[kept++] = p;
	}
	/* (The places past @kept are empty by now.) */
	g_ptr_array_set_size(planned, (gint)kept);
}

/* The series of @ids from @first on, a page of them: their wanted episodes
 * counted and their searches planned. */
static gboolean
schedule_page(
	AxisDbConn            *conn,
	const GArray          *ids,
	guint                  first,
	guint                  n,
	const AxisTvRules     *rules,
	gint64                 threshold,
	AxisTvScheduleReport  *report,
	GPtrArray             *planned,
	GError               **error
){
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GString) list = g_string_new("{");
	guint i;
	guint k;

	for (i = first; i < ids->len && i < first + n; i++)
		g_string_append_printf(list, "%s%" G_GINT64_FORMAT, i > first ? "," : "", g_array_index(ids, gint64, i));
	g_string_append_c(list, '}');
	/* (The series as a list reads them: nothing here reads what only a
	 * series' own page shows, and that costs a look-up or three a series.) */
	if (!axis_tv_store_series(conn, AXIS_TV_SERIES_LIST, list->str, &series, error) ||
	    !axis_tv_store_episodes(conn, list->str, FALSE, &episodes, error))
		return FALSE;
	report->series_read += series->len;
	for (i = 0; i < series->len; i++) {
		const AxisTvSeries *s = g_ptr_array_index(series, i);

		if (!series_searchable(s))
			continue;
		for (k = 0; k < episodes->len; k++) {
			const AxisTvEpisode *e = g_ptr_array_index(episodes, k);

			if (e->series_id != s->id || !scheduled_wanted(e, s, rules, FALSE))
				continue;
			report->considered++;
			if (e->next_search_us <= rules->now_us)
				report->due++;
		}
		plan_one_series(s, episodes, rules, TRUE, threshold, -1, NULL, FALSE, planned);
	}
	return TRUE;
}

/* 1. The series follow the metadata: the longest behind first, as many as
 * go round all of them in AXIS_TV_SYNC_CYCLE_S. */
static gboolean
sync_behind(
	AxisTv                *self,
	AxisTvScheduleReport  *report,
	GError               **error
){
	g_autoptr(AxisDbResult) count = NULL;
	g_autoptr(AxisDbResult) stale = NULL;
	g_autofree gchar *batch = NULL;
	g_autofree gchar *cycle = axis_tv_int_text(AXIS_TV_SYNC_CYCLE_S);
	gint64 began = g_get_monotonic_time();
	const gchar *params[2];
	gint64 n_series = 0;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM series", 0, NULL, &count, error))
		return FALSE;
	axis_db_result_get_int64(count, 0, 0, &n_series);
	batch = axis_tv_int_text(axis_tv_sync_batch(n_series));
	params[0] = batch;
	params[1] = cycle;
	if (!axis_db_pool_exec(self->pool, "SELECT s.id FROM series s WHERE s.synced_at IS NULL"
	        " OR s.synced_at < now() - $2::integer * interval '1 second' OR EXISTS (SELECT 1 FROM meta_season ms"
	        " WHERE ms.title_id = s.title_id AND ms.source = s.episode_source AND ms.episodes_fetched_at > s.synced_at)"
	        " ORDER BY s.synced_at NULLS FIRST, s.id LIMIT $1::integer", 2, params, &stale, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(stale); i++) {
		g_autoptr(GError) local = NULL;
		gint64 id = 0;

		if (g_get_monotonic_time() - began > (gint64)AXIS_TV_SYNC_BUDGET_S * G_USEC_PER_SEC) {
			g_message("series: %u of %u series followed the metadata in %d s; the others wait for the next run", i,
				axis_db_result_get_n_rows(stale), AXIS_TV_SYNC_BUDGET_S);
			break;
		}
		axis_db_result_get_int64(stale, i, 0, &id);
		if (axis_tv_sync(self, id, TRUE, NULL, &local))
			report->synced++;
		else if (!g_error_matches(local, AXIS_TV_ERROR, AXIS_TV_ERROR_NOT_FOUND))
			g_message("series: series %" G_GINT64_FORMAT " could not follow the metadata: %s", id, local->message);
	}
	return TRUE;
}

gboolean
axis_tv_run_schedule(
	AxisTv                *self,
	AxisTvScheduleReport  *report_out,
	GError               **error
){
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GPtrArray) planned = g_ptr_array_new_with_free_func((GDestroyNotify)planned_free);
	gint64 per_run = axis_tv_setting_integer(self, AXIS_TV_SETTING_PER_RUN, 5);
	gint64 threshold = axis_tv_setting_integer(self, AXIS_TV_SETTING_PACK_SHARE, 50);
	guint limit = (guint)CLAMP(per_run, 0, 100000);
	guint page = (guint)g_atomic_int_get(&self->schedule_page);
	AxisTvScheduleReport report;
	AxisTvRules rules;
	AxisDbConn *conn;
	gboolean ok;
	guint i;

	memset(&report, 0, sizeof(report));
	if (page == 0)
		page = AXIS_TV_SCHEDULE_PAGE;
	if (!sync_behind(self, &report, error))
		return FALSE;

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
	/*
	 * The database names the series with an episode that can be due; their
	 * episodes are read a page of series at a time, and of what is planned
	 * only what the run can queue is kept from page to page.  (Every series
	 * with every episode, read at once and each series looked for among all
	 * the episodes, was 53 s and 476 MiB for 5,000 series of 60 episodes.)
	 */
	ok = ok && axis_tv_store_schedule_series(conn, &rules, &ids, error);
	for (i = 0; ok && i < ids->len; i += page) {
		ok = schedule_page(conn, ids, i, page, &rules, threshold, &report, planned, error);
		keep_best(planned, limit);
	}
	ok = ok && enqueue_planned(conn, planned, "scheduled", NULL, limit, AXIS_TV_PER_SERIES_RUN,
		&report.queued, &report.seasons, error);
	axis_db_pool_release(self->pool, conn);
	if (report_out != NULL)
		*report_out = report;
	return ok;
}
