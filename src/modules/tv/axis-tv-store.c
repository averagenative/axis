/*
 * axis-tv-store.c - the TV module's SQL: the series facts, THE episode
 * facts query, a series' summary, history, and queueing a search
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * axis_tv_store_episodes() is the one statement an episode's state is
 * derived from (axis_tv_episode_derive()): the pages, the scheduler, the
 * search's want and the import all read it.  It reads the downloads layer's
 * table, the job queue's (a search that holds the episode, by the
 * de-duplication keys of axis_tv_search_key()) and the imports'; it writes
 * none of them.
 */

#include "modules/tv/axis-tv-private.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#include "jobs/axis-job-lifecycle.h"

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"
#define LIVE_STATES "('sending', 'queued', 'downloading', 'paused')"
/* AXIS_TV_SEARCH_KEY_PREFIX, as a regular expression. */
#define AXIS_TV_SEARCH_KEY_REGEX "tv\\.search:"

static gchar *
get_text(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	const gchar *text = axis_db_result_get_text(result, row, column);

	return text != NULL ? g_strdup(text) : NULL;
}

static gint64
get_int(
	AxisDbResult *result,
	guint         row,
	guint         column,
	gint64        fallback
){
	gint64 value = fallback;

	if (!axis_db_result_get_int64(result, row, column, &value))
		return fallback;
	return value;
}

static gboolean
get_bool(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gboolean value = FALSE;

	axis_db_result_get_boolean(result, row, column, &value);
	return value;
}

/* ── The series ─────────────────────────────────────────────────── */

enum
{
	S_ID, S_TITLE_ID, S_TITLE, S_ORIGINAL, S_YEAR, S_STATUS, S_RUNTIME, S_POSTER, S_PROVIDER, S_TVDB, S_TMDB, S_IMDB,
	S_PROVIDER_ID, S_PROFILE_ID, S_PROFILE_NAME, S_PROFILE_FORMAT, S_PROFILE_DEFINITION, S_ROOT_ID, S_ROOT_NAME,
	S_VOLUME_ID, S_VOLUME_NAME, S_VOLUME_STATE, S_MONITOR, S_MONITOR_NEW, S_ADDED_BY, S_ADDED, S_SYNCED,
	S_MISMATCHED, S_BY_PLACE, S_OTHER_LIVE, S_PROFILE_SLUG, S_SOURCE, S_CLAIMS, S_SYNCED_RECORD, S_HOLD,
	S_KIND, S_BROUGHT_ABSOLUTE, S_ABSOLUTE_ONLY, S_SEASON_ABSOLUTE
};

/*
 * The series' facts.  What only a series' own page and its searches read is
 * counted or looked up per series (its identifiers at the other providers,
 * the releases its searches ruled out, its episodes without a provider's id,
 * its stray downloads, the claims about its identifiers): the whole of it
 * for one series or a few (SERIES_SELECT), nothing of it for a list of every
 * series (SERIES_SELECT_LIST), which is then one join with no subquery a
 * row.  The columns are the same either way.
 */
#define SERIES_SELECT_OF(ids, counts, claims, absolute) \
	"SELECT s.id, s.title_id, t.title, t.original_title, t.year, t.status, t.runtime_minutes, t.poster_ref," \
	" t.provider," ids \
	" t.provider_id, s.quality_profile_id, qp.name, qp.format, qp.definition, s.root_id, lr.name, lr.volume_id," \
	" v.name, v.state, s.monitor_choice, s.monitor_new, s.added_by, " US("s.added_at") ", " US("s.synced_at") "," \
	counts " qp.slug, s.episode_source," claims " s.record_id, s.search_hold, s.kind, s.brought_absolute::text," \
	absolute \
	" FROM series s JOIN meta_title t ON t.id = s.title_id" \
	" JOIN quality_profile qp ON qp.id = s.quality_profile_id" \
	" LEFT JOIN library_root lr ON lr.id = s.root_id LEFT JOIN volume v ON v.id = lr.volume_id"

static const gchar series_select[] = SERIES_SELECT_OF(
	" (SELECT e.value FROM meta_external_id e WHERE e.title_id = t.id AND e.scheme = 'tvdb' LIMIT 1),"
	" (SELECT e.value FROM meta_external_id e WHERE e.title_id = t.id AND e.scheme = 'tmdb' LIMIT 1),"
	" (SELECT e.value FROM meta_external_id e WHERE e.title_id = t.id AND e.scheme = 'imdb' LIMIT 1),",
	" (SELECT count(DISTINCT c.name) FROM series_candidate c JOIN series_search ss ON ss.id = c.search_id"
	"   WHERE ss.series_id = s.id AND c.mismatched),"
	" (SELECT count(*) FROM series_episode e WHERE e.series_id = s.id AND e.provider_id IS NULL"
	"   AND e.gone_at IS NULL),"
	" (SELECT count(*) FROM download od WHERE od.module = '" AXIS_TV_MODULE_ID "' AND od.title_id = s.title_id"
	"   AND od.state IN " LIVE_STATES " AND NOT EXISTS (SELECT 1 FROM series_grab og WHERE og.download_id = od.id"
	"   AND og.closed_at IS NULL)),",
	/* Claims about identifiers that name another title (meta_id_claim):
	 * provider, scheme, value, whether this title's record makes it, and the
	 * other title's name, parted by control characters stored text lacks. */
	" (SELECT string_agg(c.provider || chr(31) || c.scheme || chr(31) || c.value || chr(31)"
	"   || CASE WHEN c.title_id = t.id THEN 'names' ELSE 'named' END || chr(31)"
	"   || CASE WHEN o.year > 0 THEN o.title || ' (' || o.year || ')' ELSE o.title END, chr(30) ORDER BY c.noted_at)"
	"   FROM meta_id_claim c JOIN meta_title o ON o.id = CASE WHEN c.title_id = t.id THEN c.held_by ELSE c.title_id END"
	"   WHERE c.title_id = t.id OR c.held_by = t.id),",
	/* Releases ruled out for their absolute numbering, by what the matcher
	 * said of each (axis-tv-state.c writes both sentences). */
	" (SELECT count(DISTINCT c.name) FROM series_candidate c JOIN series_search ss ON ss.id = c.search_id"
	"   WHERE ss.series_id = s.id AND c.match = 'none' AND c.match_text LIKE '" AXIS_TV_SAYS_ABSOLUTE_ONLY "%'),"
	" (SELECT count(DISTINCT c.name) FROM series_candidate c JOIN series_search ss ON ss.id = c.search_id"
	"   WHERE ss.series_id = s.id AND c.match = 'none' AND c.match_text LIKE '" AXIS_TV_SAYS_SEASON_ABSOLUTE "%')");

static const gchar series_select_list[] = SERIES_SELECT_OF(" NULL::text, NULL::text, NULL::text,", " 0, 0, 0,",
	" NULL::text,", " 0, 0");

/* What a series takes from its quality profile. */
typedef struct
{
	gboolean    usable;
	AxisQuality cutoff;
	gboolean    upgrades;
	gboolean    upgrade_unknown;
} ProfileFacts;

/* @profiles: profile id (gint64) -> ProfileFacts: a profile's definition is
 * read once for all the series of a result that have it. */
static AxisTvSeries *
series_from_row(
	AxisDbResult *result,
	guint         row,
	GHashTable   *profiles
){
	AxisTvSeries *s = g_new0(AxisTvSeries, 1);
	const ProfileFacts *facts;
	const gchar *text;

	s->id = get_int(result, row, S_ID, 0);
	s->title_id = get_int(result, row, S_TITLE_ID, 0);
	s->title = get_text(result, row, S_TITLE);
	s->original_title = get_text(result, row, S_ORIGINAL);
	s->year = (gint)get_int(result, row, S_YEAR, 0);
	s->status = axis_meta_status_from_string(axis_db_result_get_text(result, row, S_STATUS));
	s->runtime = (gint)get_int(result, row, S_RUNTIME, 0);
	s->poster_ref = get_text(result, row, S_POSTER);
	s->provider = get_text(result, row, S_PROVIDER);
	s->tvdb_id = get_text(result, row, S_TVDB);
	s->tmdb_id = get_text(result, row, S_TMDB);
	if (s->tmdb_id == NULL && g_strcmp0(s->provider, "tmdb") == 0)
		s->tmdb_id = get_text(result, row, S_PROVIDER_ID);
	s->imdb_id = get_text(result, row, S_IMDB);
	s->profile_id = get_int(result, row, S_PROFILE_ID, 0);
	s->profile_name = get_text(result, row, S_PROFILE_NAME);
	s->profile_slug = get_text(result, row, S_PROFILE_SLUG);
	facts = g_hash_table_lookup(profiles, &s->profile_id);
	if (facts == NULL) {
		ProfileFacts *read = g_new0(ProfileFacts, 1);
		gint64 *key = g_new(gint64, 1);
		AxisQualityProfile parsed;
		g_autofree gchar *unusable = NULL;

		memset(&parsed, 0, sizeof(parsed));
		read->usable = axis_quality_stored_check((gint)get_int(result, row, S_PROFILE_FORMAT, 0),
			axis_db_result_get_text(result, row, S_PROFILE_DEFINITION), &parsed, &unusable);
		read->cutoff = read->usable ? parsed.cutoff : AXIS_QUALITY_UNKNOWN;
		read->upgrades = read->usable && parsed.upgrades;
		read->upgrade_unknown = read->usable && parsed.upgrade_unknown;
		*key = s->profile_id;
		g_hash_table_insert(profiles, key, read);
		facts = read;
	}
	s->profile_usable = facts->usable;
	s->profile_cutoff = facts->cutoff;
	s->profile_upgrades = facts->upgrades;
	s->profile_upgrade_unknown = facts->upgrade_unknown;
	s->root_id = get_int(result, row, S_ROOT_ID, 0);
	s->root_name = get_text(result, row, S_ROOT_NAME);
	s->volume_id = get_int(result, row, S_VOLUME_ID, 0);
	s->volume_name = get_text(result, row, S_VOLUME_NAME);
	s->volume_state = AXIS_VOLUME_OFFLINE;
	text = axis_db_result_get_text(result, row, S_VOLUME_STATE);
	if (text != NULL)
		axis_volume_state_from_string(text, &s->volume_state);
	s->monitor_choice = AXIS_TV_MONITOR_ALL;
	axis_tv_monitor_from_name(axis_db_result_get_text(result, row, S_MONITOR), &s->monitor_choice);
	s->monitor_new = get_bool(result, row, S_MONITOR_NEW);
	s->added_by = get_text(result, row, S_ADDED_BY);
	s->added_us = get_int(result, row, S_ADDED, 0);
	s->synced_us = get_int(result, row, S_SYNCED, 0);
	s->n_mismatched = (guint)get_int(result, row, S_MISMATCHED, 0);
	s->n_by_place = (guint)get_int(result, row, S_BY_PLACE, 0);
	s->other_live = (guint)get_int(result, row, S_OTHER_LIVE, 0);
	s->episode_source = get_text(result, row, S_SOURCE);
	if (s->episode_source == NULL)
		s->episode_source = g_strdup(s->provider);
	s->claims = get_text(result, row, S_CLAIMS);
	s->record_id = get_text(result, row, S_PROVIDER_ID);
	s->synced_record_id = get_text(result, row, S_SYNCED_RECORD);
	s->search_hold = get_text(result, row, S_HOLD);
	s->anime = g_strcmp0(axis_db_result_get_text(result, row, S_KIND), AXIS_TV_KIND_ANIME) == 0;
	s->brought_absolute = get_text(result, row, S_BROUGHT_ABSOLUTE);
	s->n_absolute_only = (guint)get_int(result, row, S_ABSOLUTE_ONLY, 0);
	s->n_season_absolute = (guint)get_int(result, row, S_SEASON_ABSOLUTE, 0);
	return s;
}

gboolean
axis_tv_store_series(
	AxisDbConn          *conn,
	AxisTvSeriesFilter   filter,
	const gchar         *param,
	GPtrArray          **series,
	GError             **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GHashTable) profiles = NULL;
	g_autofree gchar *sql = NULL;
	const gchar *params[1];
	guint i;

	switch (filter) {
	case AXIS_TV_SERIES_ONE:
		sql = g_strconcat(series_select, " WHERE s.id = $1::bigint", NULL);
		break;
	case AXIS_TV_SERIES_TITLES:
		sql = g_strconcat(series_select_list, " WHERE s.title_id = ANY ($1::bigint[])", NULL);
		break;
	case AXIS_TV_SERIES_LIST:
		sql = g_strconcat(series_select_list, " WHERE ($1::bigint[] IS NULL OR s.id = ANY ($1::bigint[]))"
			" ORDER BY lower(t.title), t.year, s.id", NULL);
		break;
	default:
		sql = g_strconcat(series_select, " ORDER BY lower(t.title), t.year, s.id", NULL);
		break;
	}
	params[0] = param;
	if (!axis_db_conn_exec(conn, sql, filter == AXIS_TV_SERIES_ALL ? 0 : 1, params, &result, error))
		return FALSE;
	profiles = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	*series = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_series_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*series, series_from_row(result, i, profiles));
	return TRUE;
}

gboolean
axis_tv_store_series_one(
	AxisDbConn     *conn,
	gint64          series_id,
	AxisTvSeries  **series,
	GError        **error
){
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autoptr(GPtrArray) all = NULL;

	*series = NULL;
	if (!axis_tv_store_series(conn, AXIS_TV_SERIES_ONE, id, &all, error))
		return FALSE;
	if (all->len > 0)
		*series = g_ptr_array_steal_index(all, 0);
	return TRUE;
}

gboolean
axis_tv_store_lock(
	AxisDbConn  *conn,
	gint64       series_id,
	gboolean    *found,
	GError     **error
){
	g_autofree gchar *id = axis_tv_int_text(series_id);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];

	params[0] = id;
	if (!axis_db_conn_exec(conn, "SELECT id FROM series WHERE id = $1::bigint FOR UPDATE", 1, params, &result, error))
		return FALSE;
	*found = axis_db_result_get_n_rows(result) > 0;
	return TRUE;
}

/* ── THE episode facts ──────────────────────────────────────────── */

enum
{
	E_ID, E_SERIES, E_PROVIDER_ID, E_SEASON, E_NUMBER, E_ABSOLUTE, E_TITLE, E_AIR, E_RUNTIME, E_MONITORED,
	E_SEASON_MONITORED, E_GONE, E_LAST_SEARCH, E_NEXT_SEARCH, E_FLOOR, E_JOB,
	E_GRAB_ID, E_GRAB_RELEASE, E_GRAB_HOLDS, E_GRAB_COMPLETED, E_DOWNLOAD_ID, E_D_STATE, E_D_SIZE, E_D_DONE,
	E_D_CLIENT, E_REMOVAL_PENDING, E_IMPORT_ID, E_IMPORT_STATE, E_IMPORT_TEXT, E_FILES, E_FILE_QUALITIES,
	E_FILE_PATH, E_SEARCH_ID, E_SEARCH_OUTCOME, E_SEARCH_TEXT, E_FILES_SIZE, E_SOURCE, E_KEPT_BY, E_HAND_HOW,
	E_HAND_FILE, E_HOLD, E_KEPT_BROUGHT
};

/*
 * The searches in flight, read once: the queued and running jobs whose key is
 * a search of a series, a season or an episode (axis_tv_search_key(); never a
 * manual search's list), each key once (job_dedupe_key_idx, the partial index
 * over the queued and running jobs, which stays small however many finished
 * jobs the table keeps).  An episode is held by its own search, its season's
 * and its series': three joins on what the keys say, not three lookups a row.
 */
#define JOBS_IN_FLIGHT \
	"j AS MATERIALIZED (SELECT split_part(k.dedupe_key, ':', 2)::bigint AS series_id," \
	"   split_part(k.dedupe_key, ':', 3) AS scope," \
	"   CASE WHEN split_part(k.dedupe_key, ':', 4) <> '' THEN split_part(k.dedupe_key, ':', 4)::bigint END AS item," \
	"   k.running FROM (SELECT jj.dedupe_key, bool_or(jj.state = 'running') AS running FROM job jj" \
	"   WHERE jj.dedupe_key IS NOT NULL AND jj.state IN ('queued', 'running')" \
	"   AND jj.dedupe_key ~ '^" AXIS_TV_SEARCH_KEY_REGEX "[0-9]{1,18}:(series|season:[0-9]{1,9}|episode:[0-9]{1,18})$'" \
	"   GROUP BY jj.dedupe_key) k)"

#define JOBS_JOINS(e) \
	" LEFT JOIN j je ON je.scope = 'episode' AND je.series_id = " e ".series_id AND je.item = " e ".id" \
	" LEFT JOIN j jn ON jn.scope = 'season' AND jn.series_id = " e ".series_id AND jn.item = " e ".season_number" \
	" LEFT JOIN j jr ON jr.scope = 'series' AND jr.series_id = " e ".series_id"

/* 'running', 'queued' or NULL: the running one first, as the pages say it. */
#define JOBS_STATE \
	"CASE WHEN coalesce(je.running, false) OR coalesce(jn.running, false) OR coalesce(jr.running, false)" \
	" THEN 'running' WHEN je.series_id IS NOT NULL OR jn.series_id IS NOT NULL OR jr.series_id IS NOT NULL" \
	" THEN 'queued' END"

/* The episodes with a download Axis gave up on that is still at its client,
 * read once: such grabs are few. */
#define REMOVALS_PENDING \
	"rp AS MATERIALIZED (SELECT DISTINCT ge2.episode_id FROM series_grab g2" \
	"   JOIN series_grab_episode ge2 ON ge2.grab_id = g2.id JOIN download d2 ON d2.id = g2.download_id" \
	"   WHERE g2.closed_at IS NOT NULL AND g2.removal IN ('pending', 'failed') AND d2.state IN " LIVE_STATES ")"

static const gchar episodes_select[] =
	"WITH " JOBS_IN_FLIGHT ", " REMOVALS_PENDING
	" SELECT e.id, e.series_id, e.provider_id, e.season_number, e.number, coalesce(e.absolute_number, 0), e.title,"
	" e.air_date::text, coalesce(e.runtime_minutes, 0), e.monitored, coalesce(ss.monitored, true),"
	" e.gone_at IS NOT NULL, " US("e.last_search_at") ", " US("e.next_search_at") ", e.search_floor_s,"
	" " JOBS_STATE ","
	" g.id, g.release_name, g.holds, g.completed_at IS NOT NULL, g.download_id, d.state, d.size_bytes, d.done_bytes,"
	" d.client_name, rp.episode_id IS NOT NULL,"
	" imp.id, imp.state, imp.reason_text, coalesce(f.n, 0), f.qualities, f.path, ls.id, ls.outcome, ls.outcome_text,"
	/* Held apart only while it has no file: one that got a file another way
	 * is not held, for every reader of these facts (AXIS_TV_EPISODE_HELD_SQL
	 * says the same in SQL). */
	" coalesce(f.size, 0), e.source, f.kept_by, f.hand_how, f.hand_file,"
	" CASE WHEN NOT coalesce(f.settled, false) THEN e.search_hold END, coalesce(f.kept_brought, false)"
	" FROM series_episode e"
	" LEFT JOIN series_season ss ON ss.series_id = e.series_id AND ss.number = e.season_number"
	JOBS_JOINS("e")
	" LEFT JOIN rp ON rp.episode_id = e.id"
	/* The grab it is in: an episode is in at most one open grab
	 * (series_grab_episode_one_open). */
	" LEFT JOIN series_grab_episode ge ON ge.episode_id = e.id AND ge.open"
	" LEFT JOIN series_grab g ON g.id = ge.grab_id"
	" LEFT JOIN download d ON d.id = g.download_id"
	" LEFT JOIN LATERAL (SELECT ii.id, ii.state, ii.reason_text FROM import ii WHERE g.download_id IS NOT NULL"
	"   AND ii.download_id = g.download_id ORDER BY ii.id DESC LIMIT 1) imp ON true"
	" LEFT JOIN LATERAL (SELECT count(*) AS n,"
	/* quality:conversion:resolution:release, the conversion "" or "c" and the
	 * quality of the file a done replacement was made from (a converted file
	 * counts as that), the resolution of a file of an unknown source. */
	"   string_agg(coalesce(sf.quality, '') || ':' || coalesce((SELECT 'c' || coalesce(c.source_quality, '')"
	"    FROM conversion c WHERE c.file_id = lf.id AND c.state = 'done' AND c.outcome = 'replace'"
	"    ORDER BY c.id DESC LIMIT 1), '') || ':' || coalesce(sf.resolution, '') || ':' || coalesce(sf.release_name, ''),"
	"    E'\\x1f') AS qualities, coalesce(sum(lf.size), 0) AS size,"
	/* Kept by hand, and made by a person (add-force-grab-and-assign). */
	"   min(coalesce(sf.hand_by, 'a person')) FILTER (WHERE sf.kept) AS kept_by, min(sf.hand_how) AS hand_how,"
	/* A file that is kept, or linked by a person or an import, ends an
	 * episode's hold; one only identification linked and nobody kept does not
	 * (AXIS_TV_EPISODE_HELD_SQL). */
	"   bool_or(sf.kept OR sf.hand_at IS NOT NULL OR sf.import_id IS NOT NULL) AS settled,"
	/* Every kept file of it is kept because it was brought over, by no
	 * person's choice (0073_arr_brought_kept.sql). */
	"   bool_and(sf.kept_how IS NOT DISTINCT FROM '" AXIS_TV_KEPT_HOW_BROUGHT "'"
	"    AND sf.hand_how IS NOT DISTINCT FROM 'brought') FILTER (WHERE sf.kept)"
	"    AS kept_brought,"
	"   string_agg(lf.id || ':' || sf.kept::int, ',' ORDER BY lf.id) FILTER (WHERE sf.kept OR sf.hand_how IS NOT NULL)"
	"    AS hand_file,"
	"   max(lf.rel_path) AS path FROM series_file_episode sfe JOIN series_file sf ON sf.file_id = sfe.file_id"
	"   JOIN library_file lf ON lf.id = sf.file_id WHERE sfe.episode_id = e.id AND lf.state = 'present') f ON true"
	" LEFT JOIN LATERAL (SELECT s2.id, s2.outcome, s2.outcome_text FROM series_search s2"
	"   WHERE s2.episode_id = e.id AND s2.finished_at IS NOT NULL AND s2.cause <> 'list'"
	"   ORDER BY s2.id DESC LIMIT 1) ls ON true";

/* How many episodes' facts were read since the process started
 * (axis_tv_episodes_read()). */
static gint episodes_read;

/* Which episodes: a series' (or several'), the ones named, or those that air
 * between two days; $2: the gone ones too. */
static const gchar *const episodes_where[] = {
	" WHERE e.series_id = ANY ($1::bigint[])",
	" WHERE e.id = ANY ($1::bigint[])",
	" WHERE e.air_date BETWEEN ($1::date[])[1] AND ($1::date[])[2]"
};

#define EPISODES_ORDER \
	" AND ($2::boolean OR e.gone_at IS NULL)" \
	" ORDER BY e.series_id, e.gone_at IS NOT NULL, e.season_number, e.number, e.id"

/* The resolution of a stored name ("1080p"), NONE for anything else. */
static AxisReleaseResolution
resolution_of(const gchar *name)
{
	AxisReleaseResolution r;

	for (r = AXIS_RELEASE_RESOLUTION_480P; name != NULL && name[0] != '\0' && r <= AXIS_RELEASE_RESOLUTION_2160P; r++) {
		if (g_strcmp0(axis_release_resolution_name(r), name) == 0)
			return r;
	}
	return AXIS_RELEASE_RESOLUTION_NONE;
}

/* The best and the worst of the files' "quality:conversion:resolution:
 * release" fields (a converted file counts as its source's quality; a file
 * of an unknown source as that source at its resolution, below every known
 * source of it, as on a film's page). */
static void
files_quality(
	const gchar    *list,
	AxisTvEpisode  *e
){
	g_auto(GStrv) pairs = list != NULL ? g_strsplit(list, "\x1f", 64) : NULL;
	gboolean first = TRUE;
	guint i;

	e->file_quality = AXIS_QUALITY_UNKNOWN;
	e->file_resolution = AXIS_RELEASE_RESOLUTION_NONE;
	e->worst_quality = AXIS_QUALITY_UNKNOWN;
	e->worst_resolution = AXIS_RELEASE_RESOLUTION_NONE;
	e->worst_converted = FALSE;
	for (i = 0; pairs != NULL && pairs[i] != NULL; i++) {
		gchar *colon = strchr(pairs[i], ':');
		gchar *release = NULL;
		gchar *resolution = NULL;
		const gchar *converted = NULL;
		AxisQuality q = AXIS_QUALITY_UNKNOWN;
		AxisReleaseResolution r;

		if (colon != NULL) {
			*colon = '\0';
			resolution = strchr(colon + 1, ':');
			if (resolution != NULL) {
				*resolution = '\0';
				resolution++;
				release = strchr(resolution, ':');
				if (release != NULL) {
					*release = '\0';
					release++;
				}
			}
			if (colon[1] == 'c')
				converted = colon + 2;
		}
		if (converted != NULL) {
			if (converted[0] == '\0' || !axis_quality_from_name(converted, &q))
				q = AXIS_QUALITY_UNKNOWN;
		} else if (pairs[i][0] == '\0' || !axis_quality_from_name(pairs[i], &q)) {
			q = AXIS_QUALITY_UNKNOWN;
		}
		r = q == AXIS_QUALITY_UNKNOWN && converted == NULL ? resolution_of(resolution) : AXIS_RELEASE_RESOLUTION_NONE;
		if (first || axis_quality_compare_on_disk(q, r, e->file_quality, e->file_resolution) > 0) {
			e->file_quality = q;
			e->file_resolution = r;
		}
		if (first || axis_quality_compare_on_disk(q, r, e->worst_quality, e->worst_resolution) < 0) {
			e->worst_quality = q;
			e->worst_resolution = r;
			e->worst_converted = converted != NULL;
			g_free(e->worst_release);
			e->worst_release = release != NULL && release[0] != '\0' ? g_strdup(release) : NULL;
		}
		first = FALSE;
	}
}

static AxisTvEpisode *
episode_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisTvEpisode *e = g_new0(AxisTvEpisode, 1);
	const gchar *text;

	e->id = get_int(result, row, E_ID, 0);
	e->series_id = get_int(result, row, E_SERIES, 0);
	e->provider_id = get_text(result, row, E_PROVIDER_ID);
	e->source = get_text(result, row, E_SOURCE);
	e->season = (gint)get_int(result, row, E_SEASON, 0);
	e->number = (gint)get_int(result, row, E_NUMBER, 0);
	e->absolute = (gint)get_int(result, row, E_ABSOLUTE, 0);
	e->title = get_text(result, row, E_TITLE);
	e->air_date = get_text(result, row, E_AIR);
	e->runtime = (gint)get_int(result, row, E_RUNTIME, 0);
	e->monitored = get_bool(result, row, E_MONITORED);
	e->season_monitored = get_bool(result, row, E_SEASON_MONITORED);
	e->gone = get_bool(result, row, E_GONE);
	e->last_search_us = get_int(result, row, E_LAST_SEARCH, 0);
	e->next_search_us = get_int(result, row, E_NEXT_SEARCH, 0);
	e->floor_s = get_int(result, row, E_FLOOR, 21600);
	text = axis_db_result_get_text(result, row, E_JOB);
	e->search_running = g_strcmp0(text, "running") == 0;
	e->search_queued = g_strcmp0(text, "queued") == 0;
	e->grab_id = get_int(result, row, E_GRAB_ID, 0);
	e->grab_release = get_text(result, row, E_GRAB_RELEASE);
	e->grab_holds = get_text(result, row, E_GRAB_HOLDS);
	e->grab_completed = get_bool(result, row, E_GRAB_COMPLETED);
	e->download_id = get_int(result, row, E_DOWNLOAD_ID, 0);
	text = axis_db_result_get_text(result, row, E_D_STATE);
	e->download_known = text != NULL && axis_download_state_from_string(text, &e->download_state);
	e->download_size = get_int(result, row, E_D_SIZE, -1);
	e->download_done = get_int(result, row, E_D_DONE, -1);
	e->client_name = get_text(result, row, E_D_CLIENT);
	e->removal_pending = get_bool(result, row, E_REMOVAL_PENDING);
	e->import_id = get_int(result, row, E_IMPORT_ID, 0);
	e->import_state = get_text(result, row, E_IMPORT_STATE);
	e->import_text = get_text(result, row, E_IMPORT_TEXT);
	e->n_files = (guint)get_int(result, row, E_FILES, 0);
	files_quality(axis_db_result_get_text(result, row, E_FILE_QUALITIES), e);
	e->file_path = get_text(result, row, E_FILE_PATH);
	e->files_size = get_int(result, row, E_FILES_SIZE, 0);
	e->kept_by = get_text(result, row, E_KEPT_BY);
	e->kept_brought = e->kept_by != NULL && get_bool(result, row, E_KEPT_BROUGHT);
	e->hand_how = get_text(result, row, E_HAND_HOW);
	e->hand_files = get_text(result, row, E_HAND_FILE);
	e->search_id = get_int(result, row, E_SEARCH_ID, 0);
	e->search_outcome = get_text(result, row, E_SEARCH_OUTCOME);
	e->search_text = get_text(result, row, E_SEARCH_TEXT);
	e->search_hold = get_text(result, row, E_HOLD);
	return e;
}

gboolean
axis_tv_store_episodes_of(
	AxisDbConn         *conn,
	AxisTvEpisodesOf    which,
	const gchar        *param,
	gboolean            with_gone,
	GPtrArray         **episodes,
	GError            **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = NULL;
	const gchar *params[2];
	guint i;

	g_return_val_if_fail((guint)which < G_N_ELEMENTS(episodes_where), FALSE);

	sql = g_strconcat(episodes_select, episodes_where[which], EPISODES_ORDER, NULL);
	params[0] = param;
	params[1] = with_gone ? "true" : "false";
	if (!axis_db_conn_exec(conn, sql, 2, params, &result, error))
		return FALSE;
	*episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_episode_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(*episodes, episode_from_row(result, i));
	g_atomic_int_add(&episodes_read, (gint)(*episodes)->len);
	return TRUE;
}

guint
axis_tv_episodes_read(void)
{
	return (guint)g_atomic_int_get(&episodes_read);
}

gboolean
axis_tv_store_episodes(
	AxisDbConn   *conn,
	const gchar  *series_ids,
	gboolean      with_gone,
	GPtrArray   **episodes,
	GError      **error
){
	return axis_tv_store_episodes_of(conn, AXIS_TV_EPISODES_OF_SERIES, series_ids, with_gone, episodes, error);
}

/* ── Every series' episodes, counted ────────────────────────────── */

/*
 * The episodes of the series $1 (bigint[]; NULL: every series) that are not
 * gone, by what their state is derived from, counted: one row for every
 * distinct set of those facts within a series (a "class": a series has a
 * handful, whatever its length), never a row an episode.  $2 is the last day
 * whose episodes have aired (axis_tv_episode_aired(), as a day); $3 is whose
 * place in their series is wanted beside the count ('wanted', 'upgrade', or
 * neither), a superset of the episodes that can be in those states by the
 * facts alone.
 *
 * Nothing is looked up once an episode: the files, the open grabs, their
 * imports, the downloads left at a client and the searches in flight are
 * each read once and joined.  Only the last search of an episode is looked
 * up by itself (series_search_episode), and only for an episode that can be
 * wanted: aired, monitored, without a file and in no grab.
 *
 * The state is not worked out here: axis_tv_episode_derive_attention() is
 * given each class as an episode (class_episode()), so the list, the
 * dashboard and Wanted say what a series' own page says of every episode.
 */
enum
{
	C_SERIES, C_MONITORED, C_AIR, C_GRAB, C_GRAB_COMPLETED, C_DOWNLOAD, C_IMPORT_STATE, C_REMOVAL, C_JOB, C_FILES,
	C_KEPT, C_KEPT_BROUGHT, C_QUALITIES, C_OUTCOME, C_PERSON_ONLY, C_COUNT, C_MEMBERS
};

static const gchar classes_select[] =
	"WITH " JOBS_IN_FLIGHT ", " REMOVALS_PENDING ","
	" og AS MATERIALIZED (SELECT ge.episode_id, gg.completed_at IS NOT NULL AS completed, gg.download_id"
	"   FROM series_grab_episode ge JOIN series_grab gg ON gg.id = ge.grab_id WHERE ge.open),"
	" imp AS MATERIALIZED (SELECT DISTINCT ON (ii.download_id) ii.download_id, ii.state FROM import ii"
	"   WHERE ii.download_id IN (SELECT og.download_id FROM og WHERE og.download_id IS NOT NULL)"
	"   ORDER BY ii.download_id, ii.id DESC),"
	" conv AS (SELECT DISTINCT ON (c.file_id) c.file_id, 'c' || coalesce(c.source_quality, '') AS made FROM conversion c"
	"   WHERE c.state = 'done' AND c.outcome = 'replace' ORDER BY c.file_id, c.id DESC),"
	/* As the episode facts' own: the fields files_quality() reads, without
	 * the release's name, which no state depends on. */
	" f AS (SELECT sfe.episode_id, bool_or(sf.kept) AS kept,"
	"   coalesce(bool_and(sf.kept_how IS NOT DISTINCT FROM '" AXIS_TV_KEPT_HOW_BROUGHT "'"
	"    AND sf.hand_how IS NOT DISTINCT FROM 'brought') FILTER (WHERE sf.kept), false) AS kept_brought,"
	"   string_agg(coalesce(sf.quality, '') || ':' || coalesce(conv.made, '') || ':' || coalesce(sf.resolution, '')"
	"    || ':', E'\\x1f' ORDER BY sf.quality, conv.made, sf.resolution) AS qualities"
	"   FROM series_file_episode sfe JOIN series_file sf ON sf.file_id = sfe.file_id"
	"   JOIN library_file lf ON lf.id = sf.file_id AND lf.state = 'present'"
	"   LEFT JOIN conv ON conv.file_id = lf.id"
	"   WHERE ($1::bigint[] IS NULL OR sf.series_id = ANY ($1::bigint[])) GROUP BY sfe.episode_id),"
	" x AS (SELECT e.id, e.series_id, e.season_number, e.number, e.monitored, e.air_date,"
	"   CASE WHEN e.air_date IS NULL THEN 0 WHEN e.air_date <= DATE '1970-01-01' THEN 1"
	"    WHEN e.air_date <= $2::date THEN 2 ELSE 3 END AS air,"
	"   og.episode_id IS NOT NULL AS grab, coalesce(og.completed, false) AS grab_completed,"
	"   og.download_id IS NOT NULL AS download, imp.state AS import_state, rp.episode_id IS NOT NULL AS removal,"
	"   " JOBS_STATE " AS job, f.episode_id IS NOT NULL AS files, coalesce(f.kept, false) AS kept,"
	"   coalesce(f.kept_brought, false) AS kept_brought, f.qualities, ls.outcome, coalesce(ls.person_only, false)"
	"    AS person_only"
	"   FROM series_episode e"
	JOBS_JOINS("e")
	"   LEFT JOIN rp ON rp.episode_id = e.id"
	"   LEFT JOIN og ON og.episode_id = e.id"
	"   LEFT JOIN imp ON imp.download_id = og.download_id"
	"   LEFT JOIN f ON f.episode_id = e.id"
	"   LEFT JOIN LATERAL (SELECT s2.outcome, s2.n_person_only > 0 AS person_only FROM series_search s2"
	"    WHERE s2.episode_id = e.id AND s2.finished_at IS NOT NULL AND s2.cause <> 'list'"
	"    AND e.monitored AND e.air_date <= $2::date AND f.episode_id IS NULL AND og.episode_id IS NULL"
	"    ORDER BY s2.id DESC LIMIT 1) ls ON true"
	"   WHERE e.gone_at IS NULL AND ($1::bigint[] IS NULL OR e.series_id = ANY ($1::bigint[])))"
	" SELECT x.series_id, x.monitored, min(x.air_date)::text, x.grab, x.grab_completed, x.download, x.import_state,"
	"  x.removal, x.job, x.files, x.kept, x.kept_brought, x.qualities, x.outcome, x.person_only, count(*),"
	"  string_agg(x.season_number || ':' || x.number || ':' || x.id, ',') FILTER (WHERE"
	"   ($3::text = 'wanted' AND (x.removal OR x.job IS NOT NULL OR x.grab"
	"    OR (NOT x.files AND x.monitored AND x.air = 2)))"
	"   OR ($3::text = 'upgrade' AND x.files AND x.monitored AND NOT x.kept))"
	" FROM x GROUP BY x.series_id, x.monitored, x.air, CASE WHEN x.air = 1 THEN x.air_date END, x.grab,"
	"  x.grab_completed, x.download, x.import_state, x.removal, x.job, x.files, x.kept, x.kept_brought, x.qualities,"
	"  x.outcome, x.person_only"
	" ORDER BY x.series_id";

/* A class as the episode its state is derived from.  It borrows from
 * @result and owns nothing but what files_quality() gives it. */
static void
class_episode(
	AxisDbResult  *result,
	guint          row,
	AxisTvEpisode *e
){
	const gchar *job = axis_db_result_get_text(result, row, C_JOB);

	memset(e, 0, sizeof(*e));
	e->series_id = get_int(result, row, C_SERIES, 0);
	e->monitored = get_bool(result, row, C_MONITORED);
	e->season_monitored = TRUE;
	e->air_date = (gchar *)axis_db_result_get_text(result, row, C_AIR);
	e->grab_id = get_bool(result, row, C_GRAB) ? 1 : 0;
	e->grab_completed = get_bool(result, row, C_GRAB_COMPLETED);
	e->download_id = get_bool(result, row, C_DOWNLOAD) ? 1 : 0;
	e->download_size = -1;
	e->download_done = -1;
	e->import_state = (gchar *)axis_db_result_get_text(result, row, C_IMPORT_STATE);
	e->removal_pending = get_bool(result, row, C_REMOVAL);
	e->search_running = g_strcmp0(job, "running") == 0;
	e->search_queued = g_strcmp0(job, "queued") == 0;
	e->n_files = get_bool(result, row, C_FILES) ? 1 : 0;
	files_quality(axis_db_result_get_text(result, row, C_QUALITIES), e);
	e->kept_by = get_bool(result, row, C_KEPT) ? (gchar *)"a person" : NULL;
	e->kept_brought = e->kept_by != NULL && get_bool(result, row, C_KEPT_BROUGHT);
	e->search_outcome = (gchar *)axis_db_result_get_text(result, row, C_OUTCOME);
	e->floor_s = 21600;
}

/* What an episode's state adds to its series' summary (as
 * axis_tv_summary_new() counts one episode, @n times). */
static void
summary_count(
	AxisTvSummary       *summary,
	const AxisTvEpisode *e,
	const AxisTvRules   *rules,
	AxisTvState          state,
	guint                n,
	gboolean            *any
){
	summary->counts[state] += n;
	summary->episodes += n;
	if (e->monitored && axis_tv_episode_aired(e, rules, NULL)) {
		summary->aired_monitored += n;
		if (e->n_files > 0)
			summary->in_library += n;
	}
	if (e->monitored && e->air_date != NULL && !axis_tv_episode_aired(e, rules, NULL) && e->n_files == 0 &&
	    (summary->next_air_date == NULL || strcmp(e->air_date, summary->next_air_date) < 0)) {
		g_free(summary->next_air_date);
		summary->next_air_date = g_strdup(e->air_date);
	}
	if (!*any || axis_tv_state_severity(state) > axis_tv_state_severity(summary->state)) {
		summary->state = state;
		*any = TRUE;
	}
}

/* The summary's sentence, from its counts (and its episodes that need
 * attention, when they were read). */
static void
summary_say(AxisTvSummary *summary)
{
	g_clear_pointer(&summary->reason, g_free);
	if (summary->episodes == 0) {
		summary->state = AXIS_TV_NOT_AIRED;
		summary->reason = g_strdup("No episodes are known yet. Select Update details now on the series page to fetch them.");
	} else if (summary->attention->len > 0) {
		summary->reason = axis_tv_attention_sentence(summary->attention);
	} else if (summary->counts[AXIS_TV_ATTENTION] > 0) {
		/* Counted, and its episodes not read: the caller reads them
		 * (axis_tv_store_overview_attention()) before it shows the series. */
		summary->reason = NULL;
	} else if (summary->aired_monitored == 0) {
		summary->reason = summary->next_air_date != NULL ? g_strdup_printf("Next episode on %s.",
			summary->next_air_date) : g_strdup("No monitored episode has aired.");
	} else {
		/* What is under way (the share in the library is shown beside it). */
		g_autoptr(GString) text = g_string_new(NULL);
		guint wanted = summary->counts[AXIS_TV_WANTED] + summary->counts[AXIS_TV_SEARCHING];

		if (wanted > 0)
			g_string_append_printf(text, wanted == 1 ? "%u episode wanted" : "%u episodes wanted", wanted);
		if (summary->counts[AXIS_TV_DOWNLOADING] > 0)
			g_string_append_printf(text, "%s%u downloading", text->len > 0 ? ", " : "",
				summary->counts[AXIS_TV_DOWNLOADING]);
		if (summary->counts[AXIS_TV_TO_IMPORT] > 0)
			g_string_append_printf(text, "%s%u to import", text->len > 0 ? ", " : "",
				summary->counts[AXIS_TV_TO_IMPORT]);
		if (summary->counts[AXIS_TV_UPGRADE_WANTED] > 0)
			g_string_append_printf(text, "%s%u wanted for an upgrade", text->len > 0 ? ", " : "",
				summary->counts[AXIS_TV_UPGRADE_WANTED]);
		if (text->len == 0)
			g_string_append(text, "Every aired episode is in the library");
		if (summary->next_air_date != NULL)
			g_string_append_printf(text, "; the next episode airs on %s", summary->next_air_date);
		g_string_append_c(text, '.');
		summary->reason = g_string_free(g_steal_pointer(&text), FALSE);
	}
}

gchar *
axis_tv_store_statement(const gchar *name)
{
	if (g_strcmp0(name, "classes") == 0)
		return g_strdup(classes_select);
	if (g_strcmp0(name, "episodes") == 0)
		return g_strconcat(episodes_select, episodes_where[AXIS_TV_EPISODES_OF_SERIES], EPISODES_ORDER, NULL);
	if (g_strcmp0(name, "series-list") == 0)
		return g_strconcat(series_select_list, " WHERE ($1::bigint[] IS NULL OR s.id = ANY ($1::bigint[]))"
			" ORDER BY lower(t.title), t.year, s.id", NULL);
	if (g_strcmp0(name, "series-all") == 0)
		return g_strconcat(series_select, " ORDER BY lower(t.title), t.year, s.id", NULL);
	return NULL;
}

static gint
compare_members(
	gconstpointer a,
	gconstpointer b
){
	const AxisTvMember *x = a;
	const AxisTvMember *y = b;

	if (x->series_id != y->series_id)
		return x->series_id < y->series_id ? -1 : 1;
	if (x->season != y->season)
		return x->season < y->season ? -1 : 1;
	if (x->number != y->number)
		return x->number < y->number ? -1 : 1;
	return x->id < y->id ? -1 : x->id > y->id;
}

gboolean
axis_tv_store_overview(
	AxisTv           *self,
	AxisDbConn       *conn,
	const gchar      *series_ids,
	AxisTvMembersOf   members_of,
	GPtrArray       **summaries,
	AxisTvTotals     *totals,
	GArray          **members,
	GError          **error
){
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GHashTable) by_id = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GArray) any = NULL;
	AxisTvRules rules;
	gchar aired[11];
	const gchar *params[3];
	guint n;
	guint i;

	if (totals != NULL)
		memset(totals, 0, sizeof(*totals));
	if (members != NULL)
		*members = NULL;
	axis_tv_get_rules(self, &rules);
	/* An episode has aired once the start of its day and the wait after it
	 * are past (axis_tv_episode_aired()): its day is no later than the day
	 * that wait ago. */
	axis_tv_today(rules.now_us - rules.air_delay_s * G_USEC_PER_SEC, aired);
	params[0] = series_ids;
	params[1] = aired;
	params[2] = members == NULL ? "none" : members_of == AXIS_TV_MEMBERS_OF_WANTED ? "wanted" :
		members_of == AXIS_TV_MEMBERS_OF_UPGRADES ? "upgrade" : "none";
	if (!axis_tv_store_series(conn, AXIS_TV_SERIES_LIST, series_ids, &series, error))
		return FALSE;
	/*
	 * (Without the server's just-in-time compilation, as every statement of
	 * a pooled session is, axis-db-pool.c: the planner prices the look-up of
	 * an episode's last search as if every episode had one made, which puts
	 * a large library's statement over the price at which it compiles the
	 * plan first, and that compilation takes longer than the statement.)
	 */
	if (!axis_db_conn_exec(conn, classes_select, 3, params, &result, error))
		return FALSE;

	*summaries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_summary_free);
	any = g_array_sized_new(FALSE, TRUE, sizeof(gboolean), series->len);
	g_array_set_size(any, series->len);
	while (series->len > 0) {
		AxisTvSummary *summary = g_new0(AxisTvSummary, 1);

		summary->series = g_ptr_array_steal_index(series, 0);
		summary->state = AXIS_TV_IN_LIBRARY;
		summary->attention = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_attention_item_free);
		g_ptr_array_add(*summaries, summary);
	}
	/* (The summaries are not moved from here on: the table borrows them.) */
	for (i = 0; i < (*summaries)->len; i++) {
		AxisTvSummary *summary = g_ptr_array_index(*summaries, i);

		summary->overview_index = i;
		g_hash_table_insert(by_id, &summary->series->id, summary);
	}
	if (members != NULL)
		*members = g_array_new(FALSE, TRUE, sizeof(AxisTvMember));

	n = axis_db_result_get_n_rows(result);
	for (i = 0; i < n; i++) {
		AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;
		AxisTvSummary *summary;
		AxisTvEpisode e;
		AxisTvState state;
		const gchar *listed;
		gboolean held;
		guint count;

		class_episode(result, i, &e);
		summary = g_hash_table_lookup(by_id, &e.series_id);
		/* A series added between the two statements: the next view has it. */
		if (summary == NULL) {
			g_free(e.worst_release);
			continue;
		}
		count = (guint)get_int(result, i, C_COUNT, 0);
		state = axis_tv_episode_derive_attention(&e, summary->series, &rules, NULL, &kind);
		summary_count(summary, &e, &rules, state, count, &g_array_index(any, gboolean, summary->overview_index));
		held = state == AXIS_TV_ATTENTION && kind == AXIS_ACQUIRE_ATTENTION_REJECTED &&
			get_bool(result, i, C_PERSON_ONLY);
		if (held)
			summary->held += count;
		if (totals != NULL) {
			totals->counts[state] += count;
			if ((guint)kind < G_N_ELEMENTS(totals->kinds))
				totals->kinds[kind] += count;
		}
		listed = members != NULL ? axis_db_result_get_text(result, i, C_MEMBERS) : NULL;
		if (listed != NULL && listed[0] != '\0') {
			g_auto(GStrv) each = g_strsplit(listed, ",", -1);
			guint k;

			for (k = 0; each[k] != NULL; k++) {
				AxisTvMember member;
				gint season = 0;
				gint number = 0;
				gint64 id = 0;

				if (sscanf(each[k], "%d:%d:%" G_GINT64_FORMAT, &season, &number, &id) != 3)
					continue;
				memset(&member, 0, sizeof(member));
				member.series_id = e.series_id;
				member.id = id;
				member.season = season;
				member.number = number;
				member.state = state;
				member.held = held;
				g_array_append_val(*members, member);
			}
		}
		g_free(e.worst_release);
	}
	for (i = 0; i < (*summaries)->len; i++)
		summary_say(g_ptr_array_index(*summaries, i));
	if (members != NULL)
		g_array_sort(*members, compare_members);
	return TRUE;
}

gboolean
axis_tv_store_overview_attention(
	AxisTv      *self,
	AxisDbConn  *conn,
	GPtrArray   *summaries,
	guint        first,
	guint        n,
	GError     **error
){
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GString) ids = g_string_new("{");
	AxisTvRules rules;
	guint i;

	for (i = first; i < summaries->len && i < first + n; i++) {
		const AxisTvSummary *summary = g_ptr_array_index(summaries, i);

		if (summary->counts[AXIS_TV_ATTENTION] > 0 && summary->attention->len == 0)
			g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, ids->len > 1 ? "," : "", summary->series->id);
	}
	if (ids->len == 1)
		return TRUE;
	g_string_append_c(ids, '}');
	if (!axis_tv_store_episodes(conn, ids->str, FALSE, &episodes, error))
		return FALSE;
	axis_tv_get_rules(self, &rules);
	for (i = first; i < summaries->len && i < first + n; i++) {
		AxisTvSummary *summary = g_ptr_array_index(summaries, i);
		AxisTvSummary *whole;

		if (summary->counts[AXIS_TV_ATTENTION] == 0 || summary->attention->len > 0)
			continue;
		/* Counted again from its episodes, as its own page counts it: what
		 * changed between the two statements is as the later one says. */
		whole = axis_tv_summary_new(g_steal_pointer(&summary->series), episodes, &rules);
		whole->overview_index = summary->overview_index;
		whole->held = summary->held;
		axis_tv_summary_free(summary);
		summaries->pdata[i] = whole;
	}
	return TRUE;
}

/* ── Summaries ──────────────────────────────────────────────────── */

AxisTvSummary *
axis_tv_summary_new(
	AxisTvSeries      *series,
	const GPtrArray   *episodes,
	const AxisTvRules *rules
){
	AxisTvSummary *summary = g_new0(AxisTvSummary, 1);
	gboolean any = FALSE;
	guint i;

	summary->series = series;
	summary->state = AXIS_TV_IN_LIBRARY;
	summary->attention = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_attention_item_free);
	for (i = 0; episodes != NULL && i < episodes->len; i++) {
		const AxisTvEpisode *e = g_ptr_array_index(episodes, i);
		g_autofree gchar *why = NULL;
		AxisAcquireAttention kind = AXIS_ACQUIRE_ATTENTION_NONE;
		AxisTvState state;

		if (e->series_id != series->id || e->gone)
			continue;
		state = axis_tv_episode_derive_attention(e, series, rules, &why, &kind);
		if (state == AXIS_TV_ATTENTION) {
			AxisTvAttentionItem *item = g_new0(AxisTvAttentionItem, 1);

			item->episode_id = e->id;
			item->season = e->season;
			item->number = e->number;
			item->kind = kind;
			item->reason = g_strdup(why);
			item->search_id = e->search_id;
			item->import_id = kind == AXIS_ACQUIRE_ATTENTION_IMPORT ? e->import_id : 0;
			g_ptr_array_add(summary->attention, item);
		}
		summary_count(summary, e, rules, state, 1, &any);
	}
	summary_say(summary);
	return summary;
}

gboolean
axis_tv_store_summaries(
	AxisTv              *self,
	AxisDbConn          *conn,
	AxisTvSeriesFilter   filter,
	const gchar         *param,
	GPtrArray          **summaries,
	GError             **error
){
	g_autoptr(GPtrArray) series = NULL;
	g_autoptr(GPtrArray) episodes = NULL;
	g_autoptr(GString) ids = g_string_new("{");
	AxisTvRules rules;
	guint i;

	if (!axis_tv_store_series(conn, filter, param, &series, error))
		return FALSE;
	for (i = 0; i < series->len; i++)
		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "",
			((AxisTvSeries *)g_ptr_array_index(series, i))->id);
	g_string_append_c(ids, '}');
	if (!axis_tv_store_episodes(conn, ids->str, FALSE, &episodes, error))
		return FALSE;
	axis_tv_get_rules(self, &rules);
	*summaries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_summary_free);
	while (series->len > 0)
		g_ptr_array_add(*summaries, axis_tv_summary_new(g_ptr_array_steal_index(series, 0), episodes, &rules));
	return TRUE;
}

/* ── History ────────────────────────────────────────────────────── */

gboolean
axis_tv_store_event(
	AxisDbConn   *conn,
	gint64        series_id,
	gint64        title_id,
	const gchar  *title_label,
	gint64        episode_id,
	const gchar  *kind,
	const gchar  *text,
	const gchar  *user,
	gint64        search_id,
	gint64        grab_id,
	GError      **error
){
	g_autofree gchar *series = series_id > 0 ? axis_tv_int_text(series_id) : NULL;
	g_autofree gchar *title = title_id > 0 ? axis_tv_int_text(title_id) : NULL;
	g_autofree gchar *episode = episode_id > 0 ? axis_tv_int_text(episode_id) : NULL;
	g_autofree gchar *search = search_id > 0 ? axis_tv_int_text(search_id) : NULL;
	g_autofree gchar *grab = grab_id > 0 ? axis_tv_int_text(grab_id) : NULL;
	g_autofree gchar *label = axis_tv_text_cut(title_label, 600);
	g_autofree gchar *body = axis_tv_text_cut(text, 4000);
	g_autofree gchar *by = axis_tv_text_cut(user, 200);
	const gchar *params[9];

	params[0] = series;
	params[1] = title;
	params[2] = label != NULL ? label : "Untitled";
	params[3] = episode;
	params[4] = kind;
	params[5] = body != NULL ? body : kind;
	params[6] = by;
	params[7] = search;
	params[8] = grab;
	return axis_db_conn_exec(conn, "INSERT INTO series_event (series_id, title_id, title_label, episode_id, kind, text,"
		" by_user, search_id, grab_id) VALUES ($1::bigint, $2::bigint, $3, $4::bigint, $5, $6, $7, $8::bigint,"
		" $9::bigint)", 9, params, NULL, error);
}

gboolean
axis_tv_store_eventf(
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	gint64               episode_id,
	const gchar         *kind,
	const gchar         *user,
	GError             **error,
	const gchar         *format,
	...
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *label = axis_tv_series_label(series->title, series->year);
	va_list ap;

	va_start(ap, format);
	text = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_tv_store_event(conn, series->id, series->title_id, label, episode_id, kind, text, user, 0, 0, error);
}

void
axis_tv_event_free(AxisTvEvent *event)
{
	if (event == NULL)
		return;
	g_free(event->title_label);
	g_free(event->kind);
	g_free(event->text);
	g_free(event->by_user);
	g_free(event);
}

gboolean
axis_tv_list_events(
	AxisTv      *self,
	gint64       series_id,
	gint64       before_id,
	guint        limit,
	GPtrArray  **events,
	GError     **error
){
	g_autofree gchar *series = axis_tv_int_text(series_id);
	g_autofree gchar *before = axis_tv_int_text(before_id > 0 ? before_id : G_MAXINT64);
	g_autofree gchar *count = axis_tv_int_text(CLAMP(limit, 1, 500));
	g_autoptr(AxisDbResult) rows = NULL;
	const gchar *params[3];
	guint i;

	params[0] = series;
	params[1] = before;
	params[2] = count;
	if (!axis_db_pool_exec(self->pool, "SELECT id, series_id, title_id, title_label, episode_id, kind, text, by_user, "
	        US("created_at") " FROM series_event WHERE ($1::bigint = 0 OR series_id = $1::bigint) AND id < $2::bigint"
	        " ORDER BY id DESC LIMIT $3::integer", 3, params, &rows, error))
		return FALSE;
	*events = g_ptr_array_new_with_free_func((GDestroyNotify)axis_tv_event_free);
	for (i = 0; i < axis_db_result_get_n_rows(rows); i++) {
		AxisTvEvent *event = g_new0(AxisTvEvent, 1);

		event->id = get_int(rows, i, 0, 0);
		event->series_id = get_int(rows, i, 1, 0);
		event->title_id = get_int(rows, i, 2, 0);
		event->title_label = get_text(rows, i, 3);
		event->episode_id = get_int(rows, i, 4, 0);
		event->kind = get_text(rows, i, 5);
		event->text = get_text(rows, i, 6);
		event->by_user = get_text(rows, i, 7);
		event->created_us = get_int(rows, i, 8, 0);
		g_ptr_array_add(*events, event);
	}
	return TRUE;
}

/* ── The search job ─────────────────────────────────────────────── */

gchar *
axis_tv_search_key(
	gint64       series_id,
	AxisTvScope  scope,
	gint64       item
){
	if (scope == AXIS_TV_SCOPE_SERIES)
		return g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":series", series_id);
	return g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":%s:%" G_GINT64_FORMAT, series_id,
		axis_tv_scope_name(scope), item);
}

gchar *
axis_tv_manual_key(
	gint64       series_id,
	AxisTvScope  scope,
	gint64       item
){
	if (scope == AXIS_TV_SCOPE_SERIES)
		return g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":list:series", series_id);
	return g_strdup_printf(AXIS_TV_SEARCH_KEY_PREFIX "%" G_GINT64_FORMAT ":list:%s:%" G_GINT64_FORMAT, series_id,
		axis_tv_scope_name(scope), item);
}

gboolean
axis_tv_store_enqueue_search(
	AxisDbConn   *conn,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *cause,
	const gchar  *user,
	gboolean     *created,
	GError      **error
){
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *key = g_strcmp0(cause, "list") == 0 ? axis_tv_manual_key(series_id, scope, item) :
		axis_tv_search_key(series_id, scope, item);
	gboolean made = FALSE;
	AxisJobSpec spec;

	/*
	 * The gate of a series brought over (import-from-arr, slice S2): every
	 * search of a series is queued here, whoever asks (the scheduler, a new
	 * profile, the fallback of a season's search, the retry after a failed
	 * grab), and none is queued while the series is held, nor an episode's own
	 * search while that episode is.  A person's own search (Search now, a
	 * manual search) passes by its cause, and ends nothing of the series'
	 * hold: what it may take is decided where it is asked for (an episode
	 * held apart only as the one episode a person named) and where it runs
	 * (load() in axis-tv-search.c).
	 */
	if (!axis_tv_cause_is_person(cause)) {
		g_autoptr(AxisDbResult) held = NULL;
		g_autofree gchar *series_text = axis_tv_int_text(series_id);
		g_autofree gchar *item_text = axis_tv_int_text(scope == AXIS_TV_SCOPE_EPISODE ? item : 0);
		const gchar *params[2];

		params[0] = series_text;
		params[1] = item_text;
		if (!axis_db_conn_exec(conn, "SELECT 1 FROM series s WHERE s.id = $1::bigint AND (s.search_hold IS NOT NULL"
		        " OR EXISTS (SELECT 1 FROM series_episode e WHERE e.id = $2::bigint AND e.series_id = s.id"
		        " AND " AXIS_TV_EPISODE_HELD_SQL("e") "))", 2, params, &held, error))
			return FALSE;
		if (axis_db_result_get_n_rows(held) > 0) {
			if (created != NULL)
				*created = FALSE;
			/* The search for the next best after a grab failed is Axis's own,
			 * whoever asked for the search that grabbed (it never takes that
			 * person's cause): refused as any automatic search.  That one
			 * refusal is said, since a person who searched a held series
			 * themselves would otherwise wait for a retry that never comes. */
			if (g_strcmp0(cause, "retry") == 0) {
				g_autoptr(AxisTvSeries) series = NULL;

				if (!axis_tv_store_series_one(conn, series_id, &series, error))
					return FALSE;
				if (series != NULL)
					return axis_tv_store_eventf(conn, series, scope == AXIS_TV_SCOPE_EPISODE ? item : 0, "skipped", NULL,
						error, "%s", axis_tv_hold_stops(series->search_hold) ? "A grab failed, and it is not searched for again: "
						"the series is held until you start searching. Search for it yourself to look again." : "A grab "
						"failed, and it is not searched for again: Sonarr has a file for the episode that Axis does not "
						"see, and it is held. Search for it yourself to look again.");
			}
			return TRUE;
		}
	}
	json_object_set_int_member(payload, "series", series_id);
	json_object_set_string_member(payload, "scope", axis_tv_scope_name(scope));
	json_object_set_int_member(payload, "item", item);
	json_object_set_string_member(payload, "cause", cause);
	if (user != NULL)
		json_object_set_string_member(payload, "user", user);
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_TV_SEARCH_KIND;
	spec.queue = AXIS_ACQUIRE_SEARCH_QUEUE;
	spec.payload = payload;
	spec.priority = g_strcmp0(cause, "scheduled") == 0 ? 0 : 10;
	spec.dedupe_key = key;
	spec.max_attempts = 2;
	if (!axis_job_enqueue(conn, &spec, NULL, &made, error))
		return FALSE;
	if (created != NULL)
		*created = made;
	/* The same search queued already, by the scheduler (a click between a
	 * look and this, a fallback): raised to this one's priority, not left
	 * behind the scheduler's backlog. */
	if (!made && spec.priority > 0) {
		g_autofree gchar *priority = g_strdup_printf("%d", spec.priority);
		const gchar *params[2];

		params[0] = key;
		params[1] = priority;
		return axis_db_conn_exec(conn, "UPDATE job SET priority = $2::integer WHERE dedupe_key = $1"
			" AND state = 'queued' AND priority < $2::integer", 2, params, NULL, error);
	}
	return TRUE;
}
