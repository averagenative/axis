/*
 * axis-requests-read.c - reading requests: what the engine's own records say
 * of their titles, the state worked out from that, the lists and one title's
 * state for everyone who can ask
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing of a request's state after its decision is stored.  For a page of
 * titles or of requests, the facts are read in a bounded number of
 * statements: the movies of those titles (the module's own facts, one
 * statement), the movies on disk Axis does not manage (one), the last grab of
 * each managed movie (one), and the seasons of the managed series with their
 * episodes counted by the database (one).
 */

#include "requests/axis-requests-private.h"

#include <string.h>

/* ── The engine's facts ─────────────────────────────────────────── */

static void
series_fact_free(gpointer data)
{
	AxisRequestsSeriesFact *fact = data;

	if (fact == NULL)
		return;
	g_clear_pointer(&fact->seasons, g_array_unref);
	g_free(fact);
}

void
axis_requests_facts_clear(AxisRequestsFacts *facts)
{
	g_clear_pointer(&facts->movies, g_hash_table_unref);
	g_clear_pointer(&facts->series, g_hash_table_unref);
}

static gint64 *
key_of(gint64 id)
{
	gint64 *key = g_new(gint64, 1);

	*key = id;
	return key;
}

const AxisRequestsSeasonFact *
axis_requests_season_fact(
	const AxisRequestsSeriesFact *series,
	gint                          season
){
	guint i;

	for (i = 0; series != NULL && i < series->seasons->len; i++) {
		const AxisRequestsSeasonFact *fact = &g_array_index(series->seasons, AxisRequestsSeasonFact, i);

		if (fact->season == season)
			return fact;
	}
	return NULL;
}

gboolean
axis_requests_season_available(const AxisRequestsSeasonFact *fact)
{
	return fact != NULL && fact->aired > 0 && fact->have >= fact->aired;
}

static gboolean
load_movie_facts(
	AxisRequests       *self,
	const GArray       *titles,
	AxisRequestsFacts  *facts,
	GError            **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) others = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) movie_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GHashTable) by_movie = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GPtrArray) films = NULL;
	g_autoptr(GPtrArray) on_disk = NULL;
	AxisMovieRules rules;
	guint i;

	if (self->movies == NULL || titles->len == 0)
		return TRUE;
	g_array_append_vals(ids, titles->data, titles->len);
	axis_movies_get_rules(self->movies, &rules);
	if (!axis_movies_get_by_titles(self->movies, ids, &films, error))
		return FALSE;
	for (i = 0; i < films->len; i++) {
		const AxisMovieFacts *film = g_ptr_array_index(films, i);
		AxisRequestsMovieFact *fact = g_new0(AxisRequestsMovieFact, 1);
		AxisMovieState state = axis_movie_derive(film, &rules, NULL);

		fact->movie_id = film->id;
		fact->on_disk = film->n_files > 0;
		fact->blocked = !fact->on_disk && film->grab_id != 0 && g_strcmp0(film->import_state, "blocked") == 0;
		fact->in_flight = !fact->on_disk && (state == AXIS_MOVIE_DOWNLOADING || state == AXIS_MOVIE_TO_IMPORT ||
			axis_movie_in_flight(film));
		g_hash_table_insert(facts->movies, key_of(film->title_id), fact);
		g_hash_table_insert(by_movie, key_of(film->id), fact);
		g_array_append_val(movie_ids, film->id);
	}
	for (i = 0; i < ids->len; i++) {
		if (!g_hash_table_contains(facts->movies, &g_array_index(ids, gint64, i)))
			g_array_append_val(others, g_array_index(ids, gint64, i));
	}
	/* A movie on disk that Axis does not manage is in the library all the same. */
	if (others->len > 0) {
		if (!axis_movies_get_library_by_titles(self->movies, others, &on_disk, error))
			return FALSE;
		for (i = 0; i < on_disk->len; i++) {
			const AxisMovieFacts *film = g_ptr_array_index(on_disk, i);
			AxisRequestsMovieFact *fact = g_new0(AxisRequestsMovieFact, 1);

			fact->on_disk = TRUE;
			g_hash_table_insert(facts->movies, key_of(film->title_id), fact);
		}
	}
	/* The last grab of each managed movie: one that failed, with nothing in
	 * flight and no file, is a download that failed (the engine looks again
	 * by itself). */
	if (movie_ids->len > 0) {
		g_autofree gchar *array = axis_requests_array_text(movie_ids);
		g_autoptr(AxisDbResult) result = NULL;
		const gchar *params[] = { array };

		if (!axis_db_pool_exec(self->pool, "SELECT DISTINCT ON (g.movie_id) g.movie_id,"
		        " coalesce(g.close_reason IN ('failed', 'stalled', 'gone'), false)"
		        " FROM movie_grab g WHERE g.movie_id = ANY ($1::bigint[]) ORDER BY g.movie_id, g.id DESC", 1, params,
		        &result, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
			AxisRequestsMovieFact *fact;
			gint64 movie_id = 0;
			gboolean failed = FALSE;

			axis_db_result_get_int64(result, i, 0, &movie_id);
			axis_db_result_get_boolean(result, i, 1, &failed);
			fact = g_hash_table_lookup(by_movie, &movie_id);
			if (fact != NULL)
				fact->failed = failed && !fact->on_disk && !fact->in_flight && !fact->blocked;
		}
	}
	return TRUE;
}

/*
 * The seasons of the managed series of these titles, their episodes counted
 * by the database: aired (an air date that has come), aired with a file
 * present, in an open grab, with a blocked import, and with a last grab that
 * failed while nothing is in flight and no file is there.
 */
static const gchar series_facts_sql[] =
	"SELECT s.title_id, s.id, ss.number, ss.monitored, coalesce(e.aired, 0), coalesce(e.have, 0),"
	" coalesce(e.in_flight, false), coalesce(e.blocked, false), coalesce(e.failed, false)"
	" FROM series s JOIN series_season ss ON ss.series_id = s.id"
	" LEFT JOIN LATERAL (SELECT count(*) FILTER (WHERE x.aired) AS aired,"
	"   count(*) FILTER (WHERE x.aired AND x.has_file) AS have, bool_or(x.grab) AS in_flight,"
	"   bool_or(x.blocked) AS blocked, bool_or(x.failed AND NOT x.has_file AND NOT x.grab) AS failed"
	"  FROM (SELECT (ep.air_date IS NOT NULL AND ep.air_date <= $2::date) AS aired,"
	"    EXISTS (SELECT 1 FROM series_file_episode sfe JOIN library_file lf ON lf.id = sfe.file_id"
	"     AND lf.state = 'present' WHERE sfe.episode_id = ep.id) AS has_file,"
	"    og.grab_id IS NOT NULL AS grab,"
	"    coalesce((SELECT i.state = 'blocked' FROM import i WHERE i.download_id = gg.download_id"
	"     ORDER BY i.id DESC LIMIT 1), false) AS blocked,"
	"    coalesce((SELECT cg.close_reason IN ('failed', 'stalled', 'gone') FROM series_grab_episode ce"
	"     JOIN series_grab cg ON cg.id = ce.grab_id WHERE ce.episode_id = ep.id ORDER BY cg.id DESC LIMIT 1),"
	"     false) AS failed"
	"   FROM series_episode ep"
	"   LEFT JOIN series_grab_episode og ON og.episode_id = ep.id AND og.open"
	"   LEFT JOIN series_grab gg ON gg.id = og.grab_id"
	"   WHERE ep.series_id = s.id AND ep.season_number = ss.number AND ep.gone_at IS NULL) x) e ON true"
	" WHERE s.title_id = ANY ($1::bigint[]) ORDER BY s.title_id, ss.number";

static gboolean
load_series_facts(
	AxisRequests       *self,
	const GArray       *titles,
	AxisRequestsFacts  *facts,
	GError            **error
){
	g_autofree gchar *array = axis_requests_array_text(titles);
	g_autoptr(AxisDbResult) result = NULL;
	gchar today[11];
	const gchar *params[] = { array, today };
	guint i;

	if (self->tv == NULL || titles->len == 0)
		return TRUE;
	axis_requests_today(self, today);
	if (!axis_db_pool_exec(self->pool, series_facts_sql, 2, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisRequestsSeriesFact *series;
		AxisRequestsSeasonFact fact;
		gint64 title_id = 0;
		gint64 series_id = 0;
		gint64 number = 0;
		gint64 aired = 0;
		gint64 have = 0;

		memset(&fact, 0, sizeof(fact));
		axis_db_result_get_int64(result, i, 0, &title_id);
		axis_db_result_get_int64(result, i, 1, &series_id);
		axis_db_result_get_int64(result, i, 2, &number);
		axis_db_result_get_boolean(result, i, 3, &fact.monitored);
		axis_db_result_get_int64(result, i, 4, &aired);
		axis_db_result_get_int64(result, i, 5, &have);
		axis_db_result_get_boolean(result, i, 6, &fact.in_flight);
		axis_db_result_get_boolean(result, i, 7, &fact.blocked);
		axis_db_result_get_boolean(result, i, 8, &fact.failed);
		fact.season = (gint)number;
		fact.aired = (guint)aired;
		fact.have = (guint)have;
		series = g_hash_table_lookup(facts->series, &title_id);
		if (series == NULL) {
			series = g_new0(AxisRequestsSeriesFact, 1);
			series->series_id = series_id;
			series->seasons = g_array_new(FALSE, FALSE, sizeof(AxisRequestsSeasonFact));
			g_hash_table_insert(facts->series, key_of(title_id), series);
		}
		g_array_append_val(series->seasons, fact);
	}
	return TRUE;
}

/* A managed series with no season row yet still is a managed series. */
static gboolean
load_series_ids(
	AxisRequests       *self,
	const GArray       *titles,
	AxisRequestsFacts  *facts,
	GError            **error
){
	g_autofree gchar *array = axis_requests_array_text(titles);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { array };
	guint i;

	if (self->tv == NULL || titles->len == 0)
		return TRUE;
	if (!axis_db_pool_exec(self->pool, "SELECT title_id, id FROM series WHERE title_id = ANY ($1::bigint[])", 1, params,
	        &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 title_id = 0;
		gint64 series_id = 0;

		axis_db_result_get_int64(result, i, 0, &title_id);
		axis_db_result_get_int64(result, i, 1, &series_id);
		if (!g_hash_table_contains(facts->series, &title_id)) {
			AxisRequestsSeriesFact *series = g_new0(AxisRequestsSeriesFact, 1);

			series->series_id = series_id;
			series->seasons = g_array_new(FALSE, FALSE, sizeof(AxisRequestsSeasonFact));
			g_hash_table_insert(facts->series, key_of(title_id), series);
		}
	}
	return TRUE;
}

gboolean
axis_requests_facts_load(
	AxisRequests       *self,
	const GArray       *films,
	const GArray       *series,
	AxisRequestsFacts  *facts,
	GError            **error
){
	facts->movies = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	facts->series = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, series_fact_free);
	if (!load_movie_facts(self, films, facts, error) || !load_series_facts(self, series, facts, error) ||
	    !load_series_ids(self, series, facts, error)) {
		axis_requests_facts_clear(facts);
		return FALSE;
	}
	return TRUE;
}

/* ── A request's state ──────────────────────────────────────────── */

/* An approved request whose title was never added is "not added" once the
 * engine gave a reason, or once the placing has had this long: a daemon that
 * stopped between the decision and the placing leaves no reason, and the
 * request must not read as looked for when nothing looks. */
#define PLACING_GRACE_US (2 * G_TIME_SPAN_MINUTE)

static void
derive_info(
	AxisRequestInfo         *info,
	const AxisRequestsFacts *facts,
	gint64                   now_us
){
	gboolean not_placed = info->added_us == 0 && (info->add_error != NULL || info->decided_us == 0 ||
		now_us - info->decided_us > PLACING_GRACE_US);

	info->cause = AXIS_REQUEST_CAUSE_NONE;
	switch (info->decision) {
	case AXIS_REQUEST_WAITING:
		info->state = AXIS_REQUEST_STATE_REQUESTED;
		break;
	case AXIS_REQUEST_DECLINED:
	case AXIS_REQUEST_REPLACED:
		info->state = AXIS_REQUEST_STATE_DECLINED;
		return;
	case AXIS_REQUEST_WITHDRAWN:
		info->state = AXIS_REQUEST_STATE_WITHDRAWN;
		return;
	case AXIS_REQUEST_REMOVED:
		info->state = AXIS_REQUEST_STATE_REMOVED;
		return;
	default:
		info->state = AXIS_REQUEST_STATE_APPROVED;
		break;
	}
	if (info->kind == AXIS_REQUEST_MOVIE) {
		const AxisRequestsMovieFact *fact = g_hash_table_lookup(facts->movies, &info->title_id);

		if (info->decision == AXIS_REQUEST_WAITING) {
			info->would_look_for = fact != NULL && fact->on_disk ? 0 : 1;
			info->look_known = TRUE;
			return;
		}
		if (fact != NULL && fact->on_disk) {
			info->state = AXIS_REQUEST_STATE_AVAILABLE;
		} else if (info->added_us == 0) {
			/* Approved, and its title was never added: with the engine's
			 * reason, or being placed this moment. */
			if (not_placed) {
				info->state = AXIS_REQUEST_STATE_FAILED;
				info->cause = AXIS_REQUEST_CAUSE_NOT_ADDED;
			}
		} else if (fact == NULL || fact->movie_id == 0) {
			/* Removed between the request's read and the facts' (the
			 * database closes the request itself a moment later). */
			info->state = AXIS_REQUEST_STATE_REMOVED;
		} else if (fact->blocked) {
			info->state = AXIS_REQUEST_STATE_FAILED;
			info->cause = AXIS_REQUEST_CAUSE_NEEDS_ADMIN;
		} else if (fact->in_flight) {
			info->state = AXIS_REQUEST_STATE_DOWNLOADING;
		} else if (fact->failed) {
			info->state = AXIS_REQUEST_STATE_FAILED;
			info->cause = AXIS_REQUEST_CAUSE_DOWNLOAD_FAILED;
		}
		return;
	}
	{
		const AxisRequestsSeriesFact *series = g_hash_table_lookup(facts->series, &info->title_id);
		gboolean in_flight = FALSE;
		gboolean blocked = FALSE;
		gboolean failed = FALSE;
		guint i;

		info->have = 0;
		info->aired = 0;
		for (i = 0; i < info->seasons->len; i++) {
			const AxisRequestsSeasonFact *fact = axis_requests_season_fact(series,
				g_array_index(info->seasons, gint, i));

			if (fact == NULL)
				continue;
			info->aired += fact->aired;
			info->have += fact->have;
			in_flight = in_flight || fact->in_flight;
			blocked = blocked || fact->blocked;
			failed = failed || fact->failed;
		}
		if (info->decision == AXIS_REQUEST_WAITING) {
			/* A managed series: what its seasons still lack.  One Axis does
			 * not have: counted from the title's record by the caller. */
			if (series != NULL) {
				info->would_look_for = info->aired - info->have;
				info->look_known = TRUE;
			}
			return;
		}
		if (info->aired > 0 && info->have >= info->aired) {
			info->state = AXIS_REQUEST_STATE_AVAILABLE;
		} else if (info->added_us == 0) {
			if (not_placed) {
				info->state = AXIS_REQUEST_STATE_FAILED;
				info->cause = AXIS_REQUEST_CAUSE_NOT_ADDED;
			}
		} else if (series == NULL) {
			info->state = AXIS_REQUEST_STATE_REMOVED;
		} else if (blocked) {
			info->state = AXIS_REQUEST_STATE_FAILED;
			info->cause = AXIS_REQUEST_CAUSE_NEEDS_ADMIN;
		} else if (in_flight) {
			info->state = AXIS_REQUEST_STATE_DOWNLOADING;
		} else if (info->have > 0) {
			info->state = AXIS_REQUEST_STATE_PARTLY;
		} else if (failed) {
			info->state = AXIS_REQUEST_STATE_FAILED;
			info->cause = AXIS_REQUEST_CAUSE_DOWNLOAD_FAILED;
		}
	}
}

void
axis_request_info_free(AxisRequestInfo *info)
{
	if (info == NULL)
		return;
	g_free(info->public_id);
	g_free(info->title);
	g_free(info->requested_name);
	g_free(info->approved_how);
	g_free(info->wait_reason);
	g_free(info->decided_by);
	g_free(info->decline_reason);
	g_clear_pointer(&info->seasons, g_array_unref);
	g_free(info->add_error);
	g_free(info);
}

static AxisRequestDecision
decision_from(const gchar *name)
{
	guint i;

	for (i = 0; i <= AXIS_REQUEST_REMOVED; i++) {
		if (g_strcmp0(name, axis_request_decision_name((AxisRequestDecision)i)) == 0)
			return (AxisRequestDecision)i;
	}
	return AXIS_REQUEST_WAITING;
}

#define INFO_SELECT \
	"SELECT r.id, r.public_id, r.title_id, r.kind, t.title, coalesce(t.year, 0), coalesce(r.requested_by, 0)," \
	" r.requested_name, " AXIS_REQUESTS_US("r.created_at") ", " AXIS_REQUESTS_US("r.decided_at") ", r.decision," \
	" r.approved_how, r.wait_reason, r.decided_by, r.decline_reason," \
	" coalesce((SELECT string_agg(s.season::text, ',' ORDER BY s.season) FROM request_season s" \
	"  WHERE s.request_id = r.id), '')," \
	" (SELECT count(*) FROM request_follower f WHERE f.request_id = r.id)," \
	" EXISTS (SELECT 1 FROM request_follower f WHERE f.request_id = r.id AND f.user_id = $1::bigint)," \
	" (SELECT count(*) FROM request d WHERE d.title_id = r.title_id AND d.kind = r.kind" \
	"  AND d.decision IN ('declined', 'replaced') AND d.id < r.id)," \
	" coalesce(r.root_id, 0), coalesce(r.quality_profile_id, 0), coalesce(r.movie_id, 0), coalesce(r.series_id, 0)," \
	" " AXIS_REQUESTS_US("r.added_at") ", r.add_error" \
	" FROM request r JOIN meta_title t ON t.id = r.title_id"

static AxisRequestInfo *
info_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisRequestInfo *info = g_new0(AxisRequestInfo, 1);
	AxisRequestKind kind = AXIS_REQUEST_MOVIE;
	gint64 number = 0;

	axis_db_result_get_int64(result, row, 0, &info->id);
	info->public_id = g_strdup(axis_db_result_get_text(result, row, 1));
	axis_db_result_get_int64(result, row, 2, &info->title_id);
	axis_request_kind_from_name(axis_db_result_get_text(result, row, 3), &kind);
	info->kind = kind;
	info->title = g_strdup(axis_db_result_get_text(result, row, 4));
	axis_db_result_get_int64(result, row, 5, &number);
	info->year = (gint)number;
	axis_db_result_get_int64(result, row, 6, &info->requested_by);
	info->requested_name = g_strdup(axis_db_result_get_text(result, row, 7));
	axis_db_result_get_int64(result, row, 8, &info->created_us);
	axis_db_result_get_int64(result, row, 9, &info->decided_us);
	info->decision = decision_from(axis_db_result_get_text(result, row, 10));
	info->approved_how = g_strdup(axis_db_result_get_text(result, row, 11));
	info->wait_reason = g_strdup(axis_db_result_get_text(result, row, 12));
	info->decided_by = g_strdup(axis_db_result_get_text(result, row, 13));
	info->decline_reason = g_strdup(axis_db_result_get_text(result, row, 14));
	info->seasons = g_array_new(FALSE, FALSE, sizeof(gint));
	axis_requests_parse_list(axis_db_result_get_text(result, row, 15), FALSE, info->seasons);
	number = 0;
	axis_db_result_get_int64(result, row, 16, &number);
	info->n_followers = (guint)number;
	axis_db_result_get_boolean(result, row, 17, &info->follows);
	number = 0;
	axis_db_result_get_int64(result, row, 18, &number);
	info->declined_before = (guint)number;
	axis_db_result_get_int64(result, row, 19, &info->root_id);
	axis_db_result_get_int64(result, row, 20, &info->profile_id);
	axis_db_result_get_int64(result, row, 21, &info->movie_id);
	axis_db_result_get_int64(result, row, 22, &info->series_id);
	axis_db_result_get_int64(result, row, 23, &info->added_us);
	info->add_error = g_strdup(axis_db_result_get_text(result, row, 24));
	return info;
}

/*
 * How many aired episodes an approval of each waiting series request would
 * look for, where Axis does not have the series: counted from the title's
 * record, and known only when the record holds the episodes of every season
 * the request lists.  One statement for the page.
 */
static gboolean
count_would_look_for(
	AxisRequests  *self,
	GPtrArray     *infos,
	GError       **error
){
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *array = NULL;
	gchar today[11];
	const gchar *params[2];
	guint i;
	guint j;

	for (i = 0; i < infos->len; i++) {
		const AxisRequestInfo *info = g_ptr_array_index(infos, i);

		if (info->decision == AXIS_REQUEST_WAITING && info->kind == AXIS_REQUEST_SERIES && !info->look_known)
			g_array_append_val(ids, info->id);
	}
	if (ids->len == 0)
		return TRUE;
	array = axis_requests_array_text(ids);
	axis_requests_today(self, today);
	params[0] = array;
	params[1] = today;
	if (!axis_db_pool_exec(self->pool, "SELECT r.id,"
	        " count(e.title_id) FILTER (WHERE e.air_date IS NOT NULL AND e.air_date <= $2::date),"
	        " coalesce(bool_and(ms.episodes_fetched_at IS NOT NULL), false)"
	        " FROM request r JOIN request_season s ON s.request_id = r.id"
	        " LEFT JOIN meta_season_followed ms ON ms.title_id = r.title_id AND ms.number = s.season"
	        " LEFT JOIN meta_episode_followed e ON e.title_id = r.title_id AND e.season_number = s.season"
	        " WHERE r.id = ANY ($1::bigint[]) GROUP BY r.id", 2, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = 0;
		gint64 count = 0;
		gboolean known = FALSE;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_int64(result, i, 1, &count);
		axis_db_result_get_boolean(result, i, 2, &known);
		for (j = 0; j < infos->len; j++) {
			AxisRequestInfo *info = g_ptr_array_index(infos, j);

			if (info->id == id) {
				info->would_look_for = (guint)count;
				info->look_known = known;
			}
		}
	}
	return TRUE;
}

static gboolean
read_infos(
	AxisRequests  *self,
	const gchar   *sql,
	guint          n_params,
	const gchar  **params,
	GPtrArray    **requests,
	GError       **error
){
	g_autoptr(GPtrArray) infos = g_ptr_array_new_with_free_func((GDestroyNotify)axis_request_info_free);
	g_autoptr(GArray) films = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) series = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisDbResult) result = NULL;
	AxisRequestsFacts facts = { NULL, NULL };
	guint i;

	if (!axis_db_pool_exec(self->pool, sql, n_params, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisRequestInfo *info = info_from_row(result, i);

		g_ptr_array_add(infos, info);
		g_array_append_val(info->kind == AXIS_REQUEST_SERIES ? series : films, info->title_id);
	}
	if (!axis_requests_facts_load(self, films, series, &facts, error))
		return FALSE;
	for (i = 0; i < infos->len; i++)
		derive_info(g_ptr_array_index(infos, i), &facts, axis_requests_now(self));
	axis_requests_facts_clear(&facts);
	if (!count_would_look_for(self, infos, error))
		return FALSE;
	*requests = g_steal_pointer(&infos);
	return TRUE;
}

gboolean
axis_requests_list_all(
	AxisRequests  *self,
	guint          limit,
	GPtrArray    **requests,
	guint         *n_waiting,
	GError       **error
){
	g_autofree gchar *count = axis_requests_int_text(CLAMP(limit, 1, AXIS_REQUESTS_PAGE_MAX));
	const gchar *params[] = { "0", count };

	if (n_waiting != NULL) {
		g_autoptr(AxisDbResult) result = NULL;
		gint64 waiting = 0;

		if (!axis_db_pool_exec(self->pool, "SELECT count(*) FROM request WHERE decision = 'waiting'", 0, NULL, &result,
		        error))
			return FALSE;
		axis_db_result_get_int64(result, 0, 0, &waiting);
		*n_waiting = (guint)waiting;
	}
	/* The waiting ones first, the oldest of them at the top; then what was
	 * decided, the latest first. */
	return read_infos(self, INFO_SELECT " ORDER BY (r.decision = 'waiting') DESC,"
		" CASE WHEN r.decision = 'waiting' THEN r.created_at END ASC,"
		" coalesce(r.decided_at, r.created_at) DESC, r.id DESC LIMIT $2::integer", 2, params, requests, error);
}

gboolean
axis_requests_list_mine(
	AxisRequests  *self,
	gint64         user_id,
	guint          limit,
	GPtrArray    **requests,
	GError       **error
){
	g_autofree gchar *user = axis_requests_int_text(user_id);
	g_autofree gchar *count = axis_requests_int_text(CLAMP(limit, 1, AXIS_REQUESTS_PAGE_MAX));
	const gchar *params[] = { user, count };

	/* What they made and what they follow; a declined request asked for
	 * again is shown as the newer one. */
	return read_infos(self, INFO_SELECT " WHERE r.decision <> 'replaced' AND (r.requested_by = $1::bigint"
		" OR EXISTS (SELECT 1 FROM request_follower m WHERE m.request_id = r.id AND m.user_id = $1::bigint))"
		" ORDER BY r.created_at DESC, r.id DESC LIMIT $2::integer", 2, params, requests, error);
}

gboolean
axis_requests_get(
	AxisRequests     *self,
	const gchar      *public_id,
	gint64            viewer_id,
	gboolean          viewer_admin,
	AxisRequestInfo **info,
	GError          **error
){
	g_autofree gchar *viewer = axis_requests_int_text(viewer_id);
	g_autoptr(GPtrArray) found = NULL;
	const gchar *params[] = { viewer, public_id };
	AxisRequestInfo *one;

	*info = NULL;
	if (public_id == NULL || !g_utf8_validate(public_id, -1, NULL) || strlen(public_id) != 22) {
		g_set_error_literal(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such request");
		return FALSE;
	}
	if (!read_infos(self, INFO_SELECT " WHERE r.public_id = $2", 2, params, &found, error))
		return FALSE;
	one = found->len > 0 ? g_ptr_array_index(found, 0) : NULL;
	/* Another person's request is answered as one that does not exist. */
	if (one == NULL || (!viewer_admin && one->requested_by != viewer_id && !one->follows)) {
		g_set_error_literal(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such request");
		return FALSE;
	}
	*info = g_ptr_array_steal_index(found, 0);
	return TRUE;
}

/* ── A title's state for everyone who can ask ───────────────────── */

void
axis_request_title_state_free(AxisRequestTitleState *state)
{
	if (state == NULL)
		return;
	g_clear_pointer(&state->seasons, g_array_unref);
	g_free(state->request_public_id);
	g_free(state->asked_by);
	g_free(state->decline_reason);
	g_clear_pointer(&state->makers, g_array_unref);
	g_clear_pointer(&state->followers, g_array_unref);
	g_clear_pointer(&state->reason_people, g_array_unref);
	g_free(state);
}

static gboolean
holds(
	const GArray *ids,
	gint64        id
){
	guint i;

	for (i = 0; ids != NULL && i < ids->len; i++) {
		if (g_array_index(ids, gint64, i) == id)
			return TRUE;
	}
	return FALSE;
}

void
axis_request_title_view(
	const AxisRequestTitleState *state,
	gint64                       viewer_id,
	gboolean                     viewer_admin,
	gboolean                     show_who,
	AxisRequestTitleView        *view
){
	memset(view, 0, sizeof(*view));
	view->mine = viewer_id != 0 && holds(state->makers, viewer_id);
	view->follows = viewer_id != 0 && holds(state->followers, viewer_id);
	/* The name is of one request (of a series, the first open one): only its
	 * own maker is told it besides, not whoever asked for another season. */
	if (state->asked_by != NULL && (viewer_admin || show_who || (viewer_id != 0 && state->asked_by_id == viewer_id)))
		view->asked_by = state->asked_by;
	if (state->decline_reason != NULL && (viewer_admin || (viewer_id != 0 && holds(state->reason_people, viewer_id))))
		view->decline_reason = state->decline_reason;
}

/* A request of a title that still counts for its state, as read. */
typedef struct
{
	gint64               id;
	gchar               *public_id;
	gint64               title_id;
	AxisRequestDecision  decision;
	gint64               by;
	gchar               *name;
	gchar               *reason;
	GArray              *seasons;	/* gint */
	GArray              *followers;	/* gint64 */
} Counting;

static void
counting_free(gpointer data)
{
	Counting *row = data;

	g_free(row->public_id);
	g_free(row->name);
	g_free(row->reason);
	g_array_unref(row->seasons);
	g_array_unref(row->followers);
	g_free(row);
}

static gboolean
is_open(const Counting *row)
{
	return row->decision == AXIS_REQUEST_WAITING || row->decision == AXIS_REQUEST_APPROVED;
}

static gboolean
lists_season(
	const Counting *row,
	gint            season
){
	guint i;

	for (i = 0; i < row->seasons->len; i++) {
		if (g_array_index(row->seasons, gint, i) == season)
			return TRUE;
	}
	return FALSE;
}

static void
add_people(
	AxisRequestTitleState *state,
	const Counting        *row
){
	guint i;

	if (row->by != 0 && !holds(state->makers, row->by))
		g_array_append_val(state->makers, row->by);
	for (i = 0; i < row->followers->len; i++) {
		gint64 follower = g_array_index(row->followers, gint64, i);

		if (!holds(state->followers, follower))
			g_array_append_val(state->followers, follower);
	}
}

/* The reason of a declined request, with the only people who may be told. */
static void
keep_reason(
	AxisRequestTitleState *state,
	const Counting        *row
){
	g_free(state->decline_reason);
	state->decline_reason = g_strdup(row->reason);
	g_array_set_size(state->reason_people, 0);
	if (row->by != 0)
		g_array_append_val(state->reason_people, row->by);
	g_array_append_vals(state->reason_people, row->followers->data, row->followers->len);
}

static void
name_request(
	AxisRequestTitleState *state,
	const Counting        *row
){
	g_free(state->request_public_id);
	g_free(state->asked_by);
	state->request_public_id = g_strdup(row->public_id);
	state->asked_by = g_strdup(row->name);
	state->asked_by_id = row->by;
}

static void
movie_title_state(
	AxisRequestTitleState       *state,
	const AxisRequestsMovieFact *fact,
	const GPtrArray             *rows,
	gboolean                     closed
){
	const Counting *open = NULL;
	const Counting *declined = NULL;
	guint i;

	for (i = 0; i < rows->len; i++) {
		const Counting *row = g_ptr_array_index(rows, i);

		if (row->title_id != state->title_id)
			continue;
		if (is_open(row))
			open = row;
		else
			declined = row;	/* by id: the last */
	}
	/* A declined request counts only while nothing was asked since. */
	if (open != NULL && declined != NULL && declined->id < open->id)
		declined = NULL;
	if (open != NULL) {
		add_people(state, open);
		name_request(state, open);
	} else if (declined != NULL) {
		add_people(state, declined);
		name_request(state, declined);
		keep_reason(state, declined);
	}
	if (fact != NULL && fact->on_disk)
		state->state = AXIS_REQUEST_STATE_AVAILABLE;
	else if (fact != NULL && (fact->in_flight || fact->blocked))
		state->state = AXIS_REQUEST_STATE_DOWNLOADING;
	else if (open != NULL)
		state->state = open->decision == AXIS_REQUEST_APPROVED ? AXIS_REQUEST_STATE_APPROVED :
			AXIS_REQUEST_STATE_REQUESTED;
	else if (closed)
		state->state = AXIS_REQUEST_STATE_CLOSED;
	else if (declined != NULL)
		state->state = AXIS_REQUEST_STATE_DECLINED;
	else
		state->state = AXIS_REQUEST_STATE_CAN;
}

static void
series_title_state(
	AxisRequestTitleState        *state,
	const AxisRequestsSeriesFact *fact,
	const GPtrArray              *rows,
	const GArray                 *numbers,
	gboolean                      closed
){
	const Counting *first_open = NULL;
	const Counting *last_declined = NULL;
	guint counted = 0;
	guint n[AXIS_REQUEST_STATE_AVAILABLE + 1];
	guint i;
	guint j;

	memset(n, 0, sizeof(n));
	for (i = 0; i < numbers->len; i++) {
		AxisRequestSeasonState season;
		const AxisRequestsSeasonFact *season_fact;
		const Counting *open = NULL;
		const Counting *declined = NULL;

		memset(&season, 0, sizeof(season));
		season.season = g_array_index(numbers, gint, i);
		season_fact = axis_requests_season_fact(fact, season.season);
		for (j = 0; j < rows->len; j++) {
			const Counting *row = g_ptr_array_index(rows, j);

			if (row->title_id != state->title_id || !lists_season(row, season.season))
				continue;
			if (is_open(row))
				open = row;
			else
				declined = row;
		}
		if (open != NULL && declined != NULL && declined->id < open->id)
			declined = NULL;
		if (open != NULL) {
			add_people(state, open);
			if (first_open == NULL || open->id < first_open->id)
				first_open = open;
		} else if (declined != NULL) {
			add_people(state, declined);
			if (last_declined == NULL || declined->id > last_declined->id)
				last_declined = declined;
		}
		if (season_fact != NULL) {
			season.have = season_fact->have;
			season.aired = season_fact->aired;
		}
		if (axis_requests_season_available(season_fact))
			season.state = AXIS_REQUEST_STATE_AVAILABLE;
		else if (season_fact != NULL && (season_fact->in_flight || season_fact->blocked))
			season.state = AXIS_REQUEST_STATE_DOWNLOADING;
		else if (season_fact != NULL && season_fact->have > 0)
			season.state = AXIS_REQUEST_STATE_PARTLY;
		else if (open != NULL)
			season.state = open->decision == AXIS_REQUEST_APPROVED ? AXIS_REQUEST_STATE_APPROVED :
				AXIS_REQUEST_STATE_REQUESTED;
		else if (closed)
			season.state = AXIS_REQUEST_STATE_CLOSED;
		else if (declined != NULL)
			season.state = AXIS_REQUEST_STATE_DECLINED;
		else
			season.state = AXIS_REQUEST_STATE_CAN;
		g_array_append_val(state->seasons, season);
		/* The specials do not make a series partly available, unless they
		 * are all it has. */
		if (season.season != 0 || numbers->len == 1) {
			n[season.state]++;
			counted++;
		}
	}
	if (first_open != NULL)
		name_request(state, first_open);
	else if (last_declined != NULL)
		name_request(state, last_declined);
	if (last_declined != NULL)
		keep_reason(state, last_declined);
	if (counted > 0 && n[AXIS_REQUEST_STATE_AVAILABLE] == counted)
		state->state = AXIS_REQUEST_STATE_AVAILABLE;
	else if (n[AXIS_REQUEST_STATE_DOWNLOADING] > 0)
		state->state = AXIS_REQUEST_STATE_DOWNLOADING;
	else if (n[AXIS_REQUEST_STATE_AVAILABLE] + n[AXIS_REQUEST_STATE_PARTLY] > 0)
		state->state = AXIS_REQUEST_STATE_PARTLY;
	else if (n[AXIS_REQUEST_STATE_APPROVED] > 0)
		state->state = AXIS_REQUEST_STATE_APPROVED;
	else if (n[AXIS_REQUEST_STATE_REQUESTED] > 0)
		state->state = AXIS_REQUEST_STATE_REQUESTED;
	else if (closed)
		state->state = AXIS_REQUEST_STATE_CLOSED;
	else if (n[AXIS_REQUEST_STATE_DECLINED] > 0)
		state->state = AXIS_REQUEST_STATE_DECLINED;
	else
		state->state = AXIS_REQUEST_STATE_CAN;
}

static gint
compare_ints(
	gconstpointer a,
	gconstpointer b
){
	return *(const gint *)a - *(const gint *)b;
}

gboolean
axis_requests_title_states(
	AxisRequests  *self,
	const GArray  *title_ids,
	GHashTable   **states,
	GError       **error
){
	g_autoptr(GHashTable) out = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)axis_request_title_state_free);
	g_autoptr(GHashTable) closed = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	/* title id -> GArray of gint: the seasons its record has */
	g_autoptr(GHashTable) numbers = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)g_array_unref);
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(counting_free);
	g_autoptr(GArray) films = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) series = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *array = axis_requests_array_text(title_ids);
	g_autoptr(AxisDbResult) titles = NULL;
	g_autoptr(AxisDbResult) requests = NULL;
	AxisRequestsFacts facts = { NULL, NULL };
	const gchar *params[] = { array };
	GHashTableIter iter;
	gpointer value;
	guint i;

	*states = NULL;
	if (title_ids == NULL || title_ids->len == 0) {
		*states = g_steal_pointer(&out);
		return TRUE;
	}
	if (!axis_db_pool_exec(self->pool, "SELECT t.id, t.kind, c.title_id IS NOT NULL FROM meta_title t"
	        " LEFT JOIN request_closed_title c ON c.title_id = t.id WHERE t.id = ANY ($1::bigint[])", 1, params, &titles,
	        error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(titles); i++) {
		AxisRequestTitleState *state;
		const gchar *kind = axis_db_result_get_text(titles, i, 1);
		gboolean is_closed = FALSE;
		gint64 id = 0;

		axis_db_result_get_int64(titles, i, 0, &id);
		axis_db_result_get_boolean(titles, i, 2, &is_closed);
		if (g_strcmp0(kind, "film") != 0 && g_strcmp0(kind, "series") != 0)
			continue;
		state = g_new0(AxisRequestTitleState, 1);
		state->title_id = id;
		state->kind = g_strcmp0(kind, "series") == 0 ? AXIS_REQUEST_SERIES : AXIS_REQUEST_MOVIE;
		state->seasons = g_array_new(FALSE, FALSE, sizeof(AxisRequestSeasonState));
		state->makers = g_array_new(FALSE, FALSE, sizeof(gint64));
		state->followers = g_array_new(FALSE, FALSE, sizeof(gint64));
		state->reason_people = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_hash_table_insert(out, key_of(id), state);
		g_array_append_val(state->kind == AXIS_REQUEST_SERIES ? series : films, id);
		if (is_closed)
			g_hash_table_add(closed, key_of(id));
	}
	/* The requests that count for a title's state: the open ones and the
	 * declined ones that were not asked for again, by id. */
	if (!axis_db_pool_exec(self->pool, "SELECT r.id, r.public_id, r.title_id, r.decision, coalesce(r.requested_by, 0),"
	        " r.requested_name, r.decline_reason,"
	        " coalesce((SELECT string_agg(s.season::text, ',') FROM request_season s WHERE s.request_id = r.id), ''),"
	        " coalesce((SELECT string_agg(f.user_id::text, ',') FROM request_follower f WHERE f.request_id = r.id), '')"
	        " FROM request r WHERE r.title_id = ANY ($1::bigint[]) AND r.decision IN ('waiting', 'approved', 'declined')"
	        " ORDER BY r.id", 1, params, &requests, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(requests); i++) {
		Counting *row = g_new0(Counting, 1);

		axis_db_result_get_int64(requests, i, 0, &row->id);
		row->public_id = g_strdup(axis_db_result_get_text(requests, i, 1));
		axis_db_result_get_int64(requests, i, 2, &row->title_id);
		row->decision = decision_from(axis_db_result_get_text(requests, i, 3));
		axis_db_result_get_int64(requests, i, 4, &row->by);
		row->name = g_strdup(axis_db_result_get_text(requests, i, 5));
		row->reason = g_strdup(axis_db_result_get_text(requests, i, 6));
		row->seasons = g_array_new(FALSE, FALSE, sizeof(gint));
		axis_requests_parse_list(axis_db_result_get_text(requests, i, 7), FALSE, row->seasons);
		row->followers = g_array_new(FALSE, FALSE, sizeof(gint64));
		axis_requests_parse_list(axis_db_result_get_text(requests, i, 8), TRUE, row->followers);
		g_ptr_array_add(rows, row);
	}
	/* The seasons each series' record has. */
	if (series->len > 0) {
		g_autofree gchar *series_array = axis_requests_array_text(series);
		g_autoptr(AxisDbResult) seasons = NULL;
		const gchar *series_params[] = { series_array };

		if (!axis_db_pool_exec(self->pool, "SELECT title_id, number FROM meta_season_followed"
		        " WHERE title_id = ANY ($1::bigint[]) ORDER BY title_id, number", 1, series_params, &seasons, error))
			return FALSE;
		for (i = 0; i < axis_db_result_get_n_rows(seasons); i++) {
			GArray *list;
			gint64 id = 0;
			gint64 number = 0;
			gint season;

			axis_db_result_get_int64(seasons, i, 0, &id);
			axis_db_result_get_int64(seasons, i, 1, &number);
			season = (gint)number;
			list = g_hash_table_lookup(numbers, &id);
			if (list == NULL) {
				list = g_array_new(FALSE, FALSE, sizeof(gint));
				g_hash_table_insert(numbers, key_of(id), list);
			}
			g_array_append_val(list, season);
		}
	}
	if (!axis_requests_facts_load(self, films, series, &facts, error))
		return FALSE;
	g_hash_table_iter_init(&iter, out);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		AxisRequestTitleState *state = value;
		gboolean is_closed = g_hash_table_contains(closed, &state->title_id);

		if (state->kind == AXIS_REQUEST_MOVIE) {
			movie_title_state(state, g_hash_table_lookup(facts.movies, &state->title_id), rows, is_closed);
		} else {
			const AxisRequestsSeriesFact *fact = g_hash_table_lookup(facts.series, &state->title_id);
			GArray *list = g_hash_table_lookup(numbers, &state->title_id);
			g_autoptr(GArray) all = g_array_new(FALSE, FALSE, sizeof(gint));
			guint j;
			guint k;

			if (list != NULL)
				g_array_append_vals(all, list->data, list->len);
			/* A season Axis has that the record lost is still shown. */
			for (j = 0; fact != NULL && j < fact->seasons->len; j++) {
				gint season = g_array_index(fact->seasons, AxisRequestsSeasonFact, j).season;
				gboolean known = FALSE;

				for (k = 0; k < all->len; k++)
					known = known || g_array_index(all, gint, k) == season;
				if (!known)
					g_array_append_val(all, season);
			}
			g_array_sort(all, compare_ints);
			series_title_state(state, fact, rows, all, is_closed);
		}
	}
	axis_requests_facts_clear(&facts);
	*states = g_steal_pointer(&out);
	return TRUE;
}

/* ── The quota, the defaults, the people, the closed titles ─────── */

gboolean
axis_requests_quota_on(
	AxisRequests      *self,
	AxisDbConn        *conn,
	gint64             user_id,
	AxisRequestQuota  *quota,
	gboolean          *found,
	GError           **error
){
	g_autoptr(GTimeZone) local = self->zone != NULL ? g_time_zone_ref(self->zone) : g_time_zone_new_local();
	g_autofree gchar *user = axis_requests_int_text(user_id);
	g_autofree gchar *start = NULL;
	g_autofree gchar *end = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[3];
	gint64 movies_used = 0;
	gint64 seasons_used = 0;
	gint64 limit = 0;
	gboolean admin = FALSE;
	gboolean disabled = FALSE;

	memset(quota, 0, sizeof(*quota));
	*found = FALSE;
	quota->window = axis_requests_window(self);
	axis_request_window_bounds(local, axis_requests_now(self), quota->window, &quota->start_us, &quota->end_us);
	start = axis_requests_int_text(quota->start_us);
	end = axis_requests_int_text(quota->end_us);
	params[0] = user;
	params[1] = start;
	params[2] = end;
	/* Counted from the rows: what the person made in this window and did
	 * not withdraw.  A declined request keeps its count; what they follow
	 * was never theirs. */
	if (!axis_db_conn_exec(conn, "SELECT u.role = 'admin', u.disabled, u.request_movies_limit, u.request_seasons_limit,"
	        " (SELECT count(*) FROM request r WHERE r.requested_by = u.id AND r.kind = 'movie'"
	        "   AND r.decision <> 'withdrawn' AND r.counted_at >= " AXIS_REQUESTS_TS(2)
	        "   AND r.counted_at < " AXIS_REQUESTS_TS(3) "),"
	        " (SELECT count(*) FROM request_season s JOIN request r ON r.id = s.request_id"
	        "   WHERE r.requested_by = u.id AND r.kind = 'series' AND r.decision <> 'withdrawn'"
	        "   AND r.counted_at >= " AXIS_REQUESTS_TS(2) " AND r.counted_at < " AXIS_REQUESTS_TS(3) ")"
	        " FROM app_user u WHERE u.id = $1::bigint", 3, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) == 0)
		return TRUE;
	axis_db_result_get_boolean(result, 0, 0, &admin);
	axis_db_result_get_boolean(result, 0, 1, &disabled);
	if (disabled)
		return TRUE;
	*found = TRUE;
	axis_db_result_get_int64(result, 0, 4, &movies_used);
	axis_db_result_get_int64(result, 0, 5, &seasons_used);
	quota->movies.unlimited = admin;
	quota->seasons.unlimited = admin;
	limit = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_MOVIES, 5);
	if (!axis_db_result_is_null(result, 0, 2))
		axis_db_result_get_int64(result, 0, 2, &limit);
	quota->movies.limit = (guint)CLAMP(limit, 0, AXIS_REQUESTS_MAX_LIMIT);
	limit = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_SEASONS, 5);
	if (!axis_db_result_is_null(result, 0, 3))
		axis_db_result_get_int64(result, 0, 3, &limit);
	quota->seasons.limit = (guint)CLAMP(limit, 0, AXIS_REQUESTS_MAX_LIMIT);
	quota->movies.used = (guint)movies_used;
	quota->seasons.used = (guint)seasons_used;
	quota->movies.left = quota->movies.used < quota->movies.limit ? quota->movies.limit - quota->movies.used : 0;
	quota->seasons.left = quota->seasons.used < quota->seasons.limit ? quota->seasons.limit - quota->seasons.used : 0;
	return TRUE;
}

gboolean
axis_requests_get_quota(
	AxisRequests      *self,
	gint64             user_id,
	AxisRequestQuota  *quota,
	GError           **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean found = FALSE;
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_requests_quota_on(self, conn, user_id, quota, &found, error);
	axis_db_pool_release(self->pool, conn);
	if (ok && !found) {
		g_set_error_literal(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND, "there is no such account");
		return FALSE;
	}
	return ok;
}

gboolean
axis_requests_defaults_read(
	AxisRequests         *self,
	AxisDbConn           *conn,
	AxisRequestDefaults  *defaults,
	GError              **error
){
	g_autofree gchar *movies_root = NULL;
	g_autofree gchar *movies_profile = NULL;
	g_autofree gchar *series_root = NULL;
	g_autofree gchar *series_profile = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[4];

	memset(defaults, 0, sizeof(*defaults));
	defaults->movies_root = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_MOVIES_ROOT, 0);
	defaults->movies_profile = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_MOVIES_PROFILE, 0);
	defaults->series_root = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_SERIES_ROOT, 0);
	defaults->series_profile = axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_SERIES_PROFILE, 0);
	params[0] = movies_root = axis_requests_int_text(defaults->movies_root);
	params[1] = movies_profile = axis_requests_int_text(defaults->movies_profile);
	params[2] = series_root = axis_requests_int_text(defaults->series_root);
	params[3] = series_profile = axis_requests_int_text(defaults->series_profile);
	/* A default that was set and whose folder or profile was removed since
	 * is gone: a request then waits, and its page says why. */
	if (!axis_db_conn_exec(conn, "SELECT"
	        " EXISTS (SELECT 1 FROM library_root WHERE id = $1::bigint AND media_type = 'movies')"
	        "  AND EXISTS (SELECT 1 FROM quality_profile WHERE id = $2::bigint),"
	        " EXISTS (SELECT 1 FROM library_root WHERE id = $3::bigint AND media_type = 'tv')"
	        "  AND EXISTS (SELECT 1 FROM quality_profile WHERE id = $4::bigint)", 4, params, &result, error))
		return FALSE;
	axis_db_result_get_boolean(result, 0, 0, &defaults->movies_ok);
	axis_db_result_get_boolean(result, 0, 1, &defaults->series_ok);
	return TRUE;
}

gboolean
axis_requests_get_defaults(
	AxisRequests         *self,
	AxisRequestDefaults  *defaults,
	GError              **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);
	gboolean ok;

	if (conn == NULL)
		return FALSE;
	ok = axis_requests_defaults_read(self, conn, defaults, error);
	axis_db_pool_release(self->pool, conn);
	return ok;
}

void
axis_request_person_free(AxisRequestPerson *person)
{
	if (person == NULL)
		return;
	g_free(person->username);
	g_free(person);
}

gboolean
axis_requests_list_people(
	AxisRequests  *self,
	GPtrArray    **people,
	GError       **error
){
	g_autoptr(GPtrArray) list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_request_person_free);
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT id, username, role = 'admin', disabled, request_auto_movies,"
	        " request_auto_series, coalesce(request_movies_limit, -1), coalesce(request_seasons_limit, -1)"
	        " FROM app_user ORDER BY username LIMIT 1000", 0, NULL, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisRequestPerson *person = g_new0(AxisRequestPerson, 1);
		gint64 limit = -1;

		axis_db_result_get_int64(result, i, 0, &person->id);
		person->username = g_strdup(axis_db_result_get_text(result, i, 1));
		axis_db_result_get_boolean(result, i, 2, &person->admin);
		axis_db_result_get_boolean(result, i, 3, &person->disabled);
		axis_db_result_get_boolean(result, i, 4, &person->auto_movies);
		axis_db_result_get_boolean(result, i, 5, &person->auto_series);
		axis_db_result_get_int64(result, i, 6, &limit);
		person->movies_limit = (gint)limit;
		limit = -1;
		axis_db_result_get_int64(result, i, 7, &limit);
		person->seasons_limit = (gint)limit;
		g_ptr_array_add(list, person);
	}
	*people = g_steal_pointer(&list);
	return TRUE;
}

gboolean
axis_requests_people_without(
	AxisRequests  *self,
	GArray       **no_movies,
	GArray       **no_series,
	GError       **error
){
	g_autofree gchar *movies = axis_requests_int_text(axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_MOVIES, 5));
	g_autofree gchar *seasons = axis_requests_int_text(axis_requests_setting_int(self, AXIS_REQUESTS_SETTING_SEASONS,
		5));
	g_autoptr(GArray) without_movies = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GArray) without_series = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[] = { movies, seasons };
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT id, coalesce(request_movies_limit, $1::integer) = 0,"
	        " coalesce(request_seasons_limit, $2::integer) = 0 FROM app_user WHERE role <> 'admin'"
	        " AND (coalesce(request_movies_limit, $1::integer) = 0 OR coalesce(request_seasons_limit, $2::integer) = 0)"
	        " ORDER BY id LIMIT 1000", 2, params, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gint64 id = 0;
		gboolean none_movies = FALSE;
		gboolean none_series = FALSE;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_boolean(result, i, 1, &none_movies);
		axis_db_result_get_boolean(result, i, 2, &none_series);
		if (none_movies)
			g_array_append_val(without_movies, id);
		if (none_series)
			g_array_append_val(without_series, id);
	}
	*no_movies = g_steal_pointer(&without_movies);
	*no_series = g_steal_pointer(&without_series);
	return TRUE;
}

void
axis_request_closed_free(AxisRequestClosed *closed)
{
	if (closed == NULL)
		return;
	g_free(closed->title);
	g_free(closed->closed_by);
	g_free(closed);
}

gboolean
axis_requests_list_closed(
	AxisRequests  *self,
	GPtrArray    **closed,
	GError       **error
){
	g_autoptr(GPtrArray) list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_request_closed_free);
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_db_pool_exec(self->pool, "SELECT c.title_id, t.title, coalesce(t.year, 0), c.closed_by, "
	        AXIS_REQUESTS_US("c.closed_at") " FROM request_closed_title c JOIN meta_title t ON t.id = c.title_id"
	        " ORDER BY c.closed_at DESC, c.title_id DESC LIMIT 500", 0, NULL, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisRequestClosed *one = g_new0(AxisRequestClosed, 1);
		gint64 year = 0;

		axis_db_result_get_int64(result, i, 0, &one->title_id);
		one->title = g_strdup(axis_db_result_get_text(result, i, 1));
		axis_db_result_get_int64(result, i, 2, &year);
		one->year = (gint)year;
		one->closed_by = g_strdup(axis_db_result_get_text(result, i, 3));
		axis_db_result_get_int64(result, i, 4, &one->closed_us);
		g_ptr_array_add(list, one);
	}
	*closed = g_steal_pointer(&list);
	return TRUE;
}
