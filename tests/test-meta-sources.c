/*
 * test-meta-sources.c - a series' seasons and episodes kept per source of
 * numbering (add-metadata-tvdb, V2): TMDb's seasons and TheTVDB's aired
 * order of one title side by side, through the metadata layer against a
 * stand-in TMDb and the stand-in TheTVDB (tests/axis-test-tvdb.h), with a
 * real database; and the migration that made the store so, on a database
 * that held a series before it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (metadata): One title, two sources; The other provider's
 * answer; Identifiers that disagree; Every episode of an order (stored
 * apart, no TMDb season changed).
 *
 * The series is Wobbly Lighthouse (made up): TMDb 7051, whose season 1 holds
 * five episodes, four without a title or a date, and whose season 2 is three
 * short clips; TheTVDB 81001, whose aired order has a season 2 of half-hour
 * episodes.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-plex.h"
#include "axis-test-storage.h"
#include "axis-test-tvdb.h"
#include "db/axis-db-migrate.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"
#include "meta/axis-meta-tvdb.h"

#define TMDB_TOKEN "eyJhbGciOiJIUzI1NiJ9.ZmFrZS1zb3VyY2VzLXRva2Vu.bm90LWEtcmVhbC1zaWduYXR1cmU"
#define WAIT 5000

/* ── The stand-in TMDb ──────────────────────────────────────────── */

typedef struct
{
	GMutex   lock;
	gchar   *tvdb;	/* the TheTVDB id 7051's record names */
	gboolean empty;	/* season 1 answers with no episode */
} TmdbStand;

static void
tmdb_answers(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	TmdbStand *stand = user_data;
	gchar *json = NULL;

	answer->content_type = g_strdup("application/json");
	if (strcmp(request->path, "/3/tv/7051") == 0) {
		g_mutex_lock(&stand->lock);
		json = g_strdup_printf("{\"id\": 7051, \"name\": \"Wobbly Lighthouse\", \"original_name\": \"Wobbly Lighthouse\","
			" \"first_air_date\": \"2023-09-01\", \"last_air_date\": \"2024-06-15\", \"status\": \"Returning Series\","
			" \"overview\": \"TMDb's own words about the lighthouse.\", \"original_language\": \"en\","
			" \"poster_path\": \"/madeUpPoster000000000007051.jpg\", \"episode_run_time\": [24],"
			" \"seasons\": [{\"season_number\": 1, \"episode_count\": 5, \"name\": \"Season 1\"},"
			" {\"season_number\": 2, \"episode_count\": 3, \"name\": \"Clips\"}],"
			" \"external_ids\": {\"tvdb_id\": %s, \"imdb_id\": \"tt9000081\"},"
			" \"alternative_titles\": {\"results\": [{\"iso_3166_1\": \"GB\", \"title\": \"The Leaning Light\"}]}}",
			stand->tvdb);
		g_mutex_unlock(&stand->lock);
	} else if (strcmp(request->path, "/3/tv/7052") == 0) {
		/* Another series, whose own record names TheTVDB 81001. */
		json = g_strdup("{\"id\": 7052, \"name\": \"Harbour Bell\", \"first_air_date\": \"2021-05-01\","
			" \"status\": \"Ended\", \"external_ids\": {\"tvdb_id\": 81001, \"imdb_id\": \"tt9000082\"}}");
	} else if (strcmp(request->path, "/3/tv/7051/season/1") == 0 && stand->empty) {
		json = g_strdup("{\"season_number\": 1, \"name\": \"Season 1\", \"episodes\": []}");
	} else if (strcmp(request->path, "/3/tv/7051/season/1") == 0) {
		json = g_strdup("{\"season_number\": 1, \"name\": \"Season 1\", \"episodes\": ["
			"{\"id\": 970701, \"episode_number\": 1, \"name\": \"The First Beam\", \"air_date\": \"2023-09-01\","
			" \"runtime\": 24},"
			"{\"id\": 970702, \"episode_number\": 2}, {\"id\": 970703, \"episode_number\": 3},"
			"{\"id\": 970704, \"episode_number\": 4}, {\"id\": 970705, \"episode_number\": 5}]}");
	} else if (strcmp(request->path, "/3/tv/7051/season/2") == 0) {
		json = g_strdup("{\"season_number\": 2, \"name\": \"Clips\", \"episodes\": ["
			"{\"id\": 970711, \"episode_number\": 1, \"name\": \"Clip One\", \"air_date\": \"2024-03-01\", \"runtime\": 2},"
			"{\"id\": 970712, \"episode_number\": 2, \"name\": \"Clip Two\", \"air_date\": \"2024-03-01\", \"runtime\": 2},"
			"{\"id\": 970713, \"episode_number\": 3, \"name\": \"Clip Three\", \"air_date\": \"2024-03-02\","
			" \"runtime\": 2}]}");
	} else {
		answer->status = 404;
		return;
	}
	answer->status = 200;
	answer->body = g_bytes_new_take(json, strlen(json));
}

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestDb           *db;
	AxisDbPool           *pool;
	AxisSettings         *settings;
	gchar                *dir;
	gchar                *data_path;
	AxisDataDir          *data;
	AxisMeta             *meta;
	AxisMetaProviderInfo  tmdb;	/* the real one, pointed at its stand-in */
	AxisTestPlex         *tmdb_api;
	gchar                *tmdb_origin;
	TmdbStand             stand;
	AxisTestTvdb         *tvdb;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	g_mutex_init(&fx->stand.lock);
	fx->stand.tvdb = g_strdup("81001");
	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->data_path = g_build_filename(fx->dir, "data", NULL);
	fx->data = axis_data_dir_open(fx->data_path, &error);
	g_assert_no_error(error);
	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);

	/* Both as the daemon registers them, TMDb first, but for their
	 * addresses. */
	fx->tmdb_api = axis_test_plex_new();
	fx->tmdb_origin = axis_test_plex_base(fx->tmdb_api);
	fx->tmdb = *axis_meta_tmdb_get_info();
	fx->tmdb.api_origin = fx->tmdb_origin;
	fx->tmdb.image_origin = fx->tmdb_origin;
	axis_test_plex_set_handler(fx->tmdb_api, tmdb_answers, &fx->stand);
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->tmdb, &error));
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, TMDB_TOKEN, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->meta != NULL) {
		axis_meta_stop(fx->meta);
		axis_meta_unref(fx->meta);
	}
	if (fx->settings != NULL)
		axis_settings_unref(fx->settings);
	axis_test_plex_free(fx->tmdb_api);
	g_free(fx->tmdb_origin);
	axis_test_tvdb_free(fx->tvdb);
	g_free(fx->stand.tvdb);
	g_mutex_clear(&fx->stand.lock);
	if (fx->data != NULL)
		axis_data_dir_unref(fx->data);
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_free(fx->data_path);
	axis_test_storage_dir_free(fx->dir);
	axis_test_log_reset();
	while (g_main_context_iteration(NULL, FALSE))
		;
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

static gchar *
query(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->db, sql);
}

static gint64
query_int(
	Fixture     *fx,
	const gchar *sql
){
	g_autofree gchar *text = query(fx, sql);

	return text != NULL ? g_ascii_strtoll(text, NULL, 10) : -1;
}

static void
assert_db(
	AxisTestDb  *db,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_query(db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected, value != NULL ? value : "NULL");
}

/* The provider's record of its series @id, stored; its store id. */
static gint64
lookup(
	Fixture     *fx,
	const gchar *provider,
	const gchar *id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 stored;

	g_assert_true(axis_meta_lookup(fx->meta, provider, AXIS_META_KIND_SERIES, id, WAIT, &answer, &error));
	g_assert_no_error(error);
	if (answer.title == NULL)
		g_error("%s %s was not stored: %s", provider, id, answer.problem != NULL ? answer.problem : "(no reason)");
	stored = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return stored;
}

static AxisMetaSeason *
season_of(
	Fixture     *fx,
	gint64       title_id,
	const gchar *source,
	gint         number
){
	AxisMetaSeasonAnswer answer;
	g_autoptr(GError) error = NULL;
	AxisMetaSeason *season;

	g_assert_true(axis_meta_get_source_season(fx->meta, title_id, source, number, WAIT, &answer, &error));
	g_assert_no_error(error);
	if (answer.season == NULL)
		g_error("season %d of %s was not stored: %s", number, source != NULL ? source : "the record",
			answer.problem != NULL ? answer.problem : "(no reason)");
	season = g_steal_pointer(&answer.season);
	axis_meta_season_answer_clear(&answer);
	return season;
}

/* What is stored of @source's seasons and episodes, but for when. */
static gchar *
snapshot(
	Fixture     *fx,
	const gchar *source
){
	g_autofree gchar *sql = g_strdup_printf("SELECT coalesce((SELECT string_agg(s.number || ':' || coalesce(s.name, '')"
		" || ':' || coalesce(s.episode_count, -1), ',' ORDER BY s.number) FROM meta_season s WHERE s.source = '%s'), '')"
		" || '|' || coalesce((SELECT string_agg(e.season_number || 'x' || e.number || ':' || coalesce(e.name, '') || ':'"
		" || coalesce(e.air_date::text, '') || ':' || coalesce(e.provider_id, ''), ',' ORDER BY e.season_number, e.number)"
		" FROM meta_episode e WHERE e.source = '%s'), '')", source, source);

	return query(fx, sql);
}

/* Spec: One title, two sources: TMDb's record names TheTVDB 81001, and
 * TheTVDB's record of 81001 is fetched. */
static void
test_one_title_two_sources(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisMetaSeason) tmdb_one = NULL;
	g_autoptr(AxisMetaSeason) tvdb_two = NULL;
	g_autoptr(AxisMetaSeason) record_two = NULL;
	gint64 title;
	gint64 again;

	title = lookup(fx, "tmdb", "7051");
	again = lookup(fx, "tvdb", "81001");
	g_assert_cmpint(again, ==, title);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(DISTINCT title_id) FROM meta_external_id"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_external_id WHERE (scheme, value) IN"
		" (('tmdb', '7051'), ('tvdb', '81001'), ('imdb', 'tt9000081'))"), ==, 3);

	/* Each source's seasons, apart. */
	tmdb_one = season_of(fx, title, NULL, 1);
	g_assert_cmpuint(tmdb_one->episodes->len, ==, 5);
	tvdb_two = season_of(fx, title, "tvdb", 2);
	g_assert_cmpuint(tvdb_two->episodes->len, ==, 3);
	g_assert_cmpstr(((AxisMetaEpisode *)g_ptr_array_index(tvdb_two->episodes, 2))->name, ==, "Storm Glass");
	g_assert_cmpint(((AxisMetaEpisode *)g_ptr_array_index(tvdb_two->episodes, 2))->runtime, ==, 24);
	/* TMDb's season 2 is its own: the clips. */
	record_two = season_of(fx, title, "tmdb", 2);
	g_assert_cmpuint(record_two->episodes->len, ==, 3);
	g_assert_cmpstr(((AxisMetaEpisode *)g_ptr_array_index(record_two->episodes, 0))->name, ==, "Clip One");
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season WHERE source = 'tmdb'"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season WHERE source = 'tvdb'"), ==, 3);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_episode WHERE source = 'tvdb'"), ==, 9);
	/* What nothing names the source of reads the record's own. */
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_episode_followed"), ==, 8);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season_followed WHERE source <> 'tmdb'"), ==, 0);
}

/* Spec: The other provider's answer: TheTVDB's record of a series TMDb's
 * made leaves its name, overview, year and poster; it adds its other names. */
static void
test_record_provider_keeps(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 title;

	title = lookup(fx, "tmdb", "7051");
	before = query(fx, "SELECT title || '|' || coalesce(original_title, '') || '|' || year || '|' || overview || '|'"
		" || poster_ref || '|' || provider || '|' || provider_id || '|' || runtime_minutes || '|' || status"
		" FROM meta_title");
	g_assert_cmpint(lookup(fx, "tvdb", "81001"), ==, title);
	after = query(fx, "SELECT title || '|' || coalesce(original_title, '') || '|' || year || '|' || overview || '|'"
		" || poster_ref || '|' || provider || '|' || provider_id || '|' || runtime_minutes || '|' || status"
		" FROM meta_title");
	g_assert_cmpstr(after, ==, before);
	g_assert_nonnull(strstr(after, "TMDb's own words"));
	g_assert_nonnull(strstr(after, "/madeUpPoster000000000007051.jpg"));
	/* Its other names join TMDb's; TMDb's stay. */
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_alt_title WHERE title = 'Le Phare Bancal'"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_alt_title WHERE title = 'The Leaning Light'"), ==, 1);

	/* Asked again within its time, TheTVDB is not asked again (its own
	 * answer's time counts, not the record's). */
	axis_test_plex_reset_requests(axis_test_tvdb_api(fx->tvdb));
	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &answer, &error));
	g_assert_false(answer.stale);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(axis_test_tvdb_api(fx->tvdb), "/v4/series/81001/extended"), ==, 0);

	/* TMDb's record fetched again keeps its own other names only. */
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '400 days'");
	g_assert_cmpint(lookup(fx, "tmdb", "7051"), ==, title);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_alt_title WHERE title = 'The Leaning Light'"), ==, 1);
}

/* TheTVDB asked first (a series TMDb is not asked about yet) keeps nothing
 * (review of V1/V2, blocker 1); TMDb's record makes the title, and
 * TheTVDB's answer then joins it. */
static void
test_record_moves_to_tmdb(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *record = NULL;
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 first;

	g_assert_true(axis_meta_lookup(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "81001", WAIT, &answer, &error));
	g_assert_null(answer.title);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 0);
	first = lookup(fx, "tmdb", "7051");
	g_assert_cmpint(lookup(fx, "tvdb", "81001"), ==, first);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);
	record = query(fx, "SELECT provider || ':' || provider_id || ':' || poster_ref || ':' || overview FROM meta_title");
	g_assert_cmpstr(record, ==, "tmdb:7051:/madeUpPoster000000000007051.jpg:TMDb's own words about the lighthouse.");
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_id_claim"), ==, 0);
}

/* TMDb takes over a record of TheTVDB's alone (kept from before: none is
 * made now), while naming another TheTVDB series: the record keeps
 * TheTVDB's own identifier of it, and nothing else is joined. */
static void
test_record_moves_keeps_own_id(Fixture *fx, gconstpointer data)
{
	gint64 first;

	g_mutex_lock(&fx->stand.lock);
	g_free(fx->stand.tvdb);
	fx->stand.tvdb = g_strdup("81002");
	g_mutex_unlock(&fx->stand.lock);
	axis_test_db_exec(fx->db, "INSERT INTO meta_title (kind, title, provider, provider_id) VALUES ('series',"
		" 'Wobbly Lighthouse', 'tvdb', '81001')");
	axis_test_db_exec(fx->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series', s,"
		" v FROM meta_title, (VALUES ('tvdb', '81001'), ('tmdb', '7051')) AS u (s, v)");
	first = query_int(fx, "SELECT id FROM meta_title");
	g_assert_cmpint(lookup(fx, "tmdb", "7051"), ==, first);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);
	assert_db(fx->db, "SELECT provider || ':' || string_agg(scheme || ':' || value, ',' ORDER BY scheme)"
		" FROM meta_title t JOIN meta_external_id x ON x.title_id = t.id GROUP BY provider",
		"tmdb:imdb:tt9000081,tmdb:7051,tvdb:81001");
}

/* Spec: Every episode of an order: three pages read and stored as that
 * order's seasons and episodes, and no stored TMDb season changes. */
static void
test_order_stored_apart(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisMetaSeason) one = NULL;
	g_autoptr(AxisMetaSeason) two = NULL;
	g_autoptr(AxisMetaSeason) aired = NULL;
	g_autofree gchar *tmdb_before = NULL;
	g_autofree gchar *tmdb_after = NULL;
	g_autofree gchar *times_before = NULL;
	g_autofree gchar *times_after = NULL;
	AxisTestPlex *api = axis_test_tvdb_api(fx->tvdb);
	gint64 title;

	title = lookup(fx, "tmdb", "7051");
	one = season_of(fx, title, NULL, 1);
	two = season_of(fx, title, NULL, 2);
	tmdb_before = snapshot(fx, "tmdb");
	times_before = query(fx, "SELECT string_agg(fetched_at::text || episodes_fetched_at::text, ',' ORDER BY number)"
		" FROM meta_season WHERE source = 'tmdb'");
	g_assert_cmpstr(tmdb_before, !=, "|");

	/* The aired order of a TMDb series: asked of TheTVDB by its TVDB id. */
	aired = season_of(fx, title, "tvdb", 1);
	g_assert_cmpuint(aired->episodes->len, ==, 5);
	g_assert_cmpstr(((AxisMetaEpisode *)g_ptr_array_index(aired->episodes, 3))->name, ==, "The Fog Bell");
	g_assert_cmpuint(axis_test_plex_count(api, "/v4/series/81001/episodes/official/eng"), ==, 3);
	{
		g_autofree gchar *tvdb = snapshot(fx, "tvdb");

		/* (TheTVDB's record, which names its seasons, was not asked for.) */
		g_assert_cmpstr(tvdb, ==, "0::1,1::5,2::3|0x1:Behind the Lamp:2023-12-20:8100100,"
			"1x1:The First Beam:2023-09-01:8100101,1x2:Keeper of Gulls:2023-09-08:8100102,1x3:Salt on the Glass:"
			"2023-09-15:8100103,1x4:The Fog Bell:2024-02-10:8100104,1x5:Low Tide:2024-02-17:8100105,2x1:Second Light:"
			"2024-05-04:8100106,2x2:The Leaning Season:2024-05-11:8100107,2x3:Storm Glass:2024-05-18:8100108");
	}
	tmdb_after = snapshot(fx, "tmdb");
	times_after = query(fx, "SELECT string_agg(fetched_at::text || episodes_fetched_at::text, ',' ORDER BY number)"
		" FROM meta_season WHERE source = 'tmdb'");
	g_assert_cmpstr(tmdb_after, ==, tmdb_before);
	g_assert_cmpstr(times_after, ==, times_before);

	/* The other seasons of the order were stored with it: no second fetch. */
	g_clear_pointer(&aired, axis_meta_season_free);
	aired = season_of(fx, title, "tvdb", 2);
	g_assert_cmpuint(axis_test_plex_count(api, "/v4/series/81001/episodes/official/eng"), ==, 3);
	/* A season the order lacks: not found, and nothing else asked. */
	{
		AxisMetaSeasonAnswer answer;
		g_autoptr(GError) error = NULL;

		axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'"
			" WHERE source = 'tvdb'");
		g_assert_true(axis_meta_get_source_season(fx->meta, title, "tvdb", 7, WAIT, &answer, &error));
		g_assert_true(answer.not_found);
		g_assert_null(answer.season);
		axis_meta_season_answer_clear(&answer);
		g_assert_cmpuint(axis_test_plex_count(api, "/v4/series/81001/episodes/official/eng"), ==, 6);
		g_assert_false(axis_meta_get_source_season(fx->meta, title, "Not a source", 1, WAIT, &answer, &error));
		g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID);
	}
	{
		g_autofree gchar *tmdb_end = snapshot(fx, "tmdb");

		g_assert_cmpstr(tmdb_end, ==, tmdb_before);
	}
}

/* Spec: Identifiers that disagree: TheTVDB's record names TMDb 7051, whose
 * own record names another TheTVDB series (81002): nothing merged, the claim
 * kept apart. */
static void
test_identifiers_disagree(Fixture *fx, gconstpointer data)
{
	gint64 tmdb_title;
	gint64 tvdb_title;
	g_autofree gchar *claims = NULL;

	g_mutex_lock(&fx->stand.lock);
	g_free(fx->stand.tvdb);
	fx->stand.tvdb = g_strdup("81002");
	g_mutex_unlock(&fx->stand.lock);
	/* TMDb 7051 names TheTVDB 81002; TMDb 7052 names TheTVDB 81001, whose
	 * record names TMDb 7051. */
	tmdb_title = lookup(fx, "tmdb", "7051");
	tvdb_title = lookup(fx, "tmdb", "7052");
	g_assert_cmpint(lookup(fx, "tvdb", "81001"), ==, tvdb_title);
	g_assert_cmpint(tvdb_title, !=, tmdb_title);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 2);
	/* Each keeps its own provider's identifiers. */
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT string_agg(scheme || ':' || value, ',' ORDER BY scheme) FROM"
			" meta_external_id WHERE title_id = %" G_GINT64_FORMAT, tmdb_title);
		g_autofree gchar *ids = query(fx, sql);

		g_assert_cmpstr(ids, ==, "imdb:tt9000081,tmdb:7051,tvdb:81002");
	}
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT string_agg(scheme || ':' || value, ',' ORDER BY scheme) FROM"
			" meta_external_id WHERE title_id = %" G_GINT64_FORMAT, tvdb_title);
		g_autofree gchar *ids = query(fx, sql);

		g_assert_cmpstr(ids, ==, "imdb:tt9000082,tmdb:7052,tvdb:81001");
	}
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT string_agg(provider || ':' || scheme || ':' || value || ':' ||"
			" (held_by = %" G_GINT64_FORMAT ")::text, ',' ORDER BY scheme) FROM meta_id_claim WHERE title_id = %"
			G_GINT64_FORMAT, tmdb_title, tvdb_title);

		claims = query(fx, sql);
	}
	g_assert_cmpstr(claims, ==, "tvdb:imdb:tt9000081:true,tvdb:tmdb:7051:true");

	/* Fetched again, the claim is the same one. */
	axis_test_db_exec(fx->db, "DELETE FROM meta_search WHERE provider = 'tvdb'");
	g_assert_cmpint(lookup(fx, "tvdb", "81001"), ==, tvdb_title);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_id_claim"), ==, 2);
	/* A brief record (a search) leaves the claims as they are. */
	{
		AxisMetaSearchAnswer search;
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_meta_search(fx->meta, "tvdb", AXIS_META_KIND_SERIES, "Wobbly Lighthouse", 0, WAIT, &search,
			&error));
		axis_meta_search_answer_clear(&search);
	}
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_id_claim"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE provider = 'tmdb'"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 2);
}

/* Review of V1/V2, fault 3: an answer that says it succeeded with no episode
 * for a series whose source had some is a fault of the provider: refused,
 * nothing stored, nothing removed; TheTVDB's order and TMDb's season alike. */
static void
test_empty_answers_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisMetaSeason) one = NULL;
	g_autoptr(AxisMetaSeason) aired = NULL;
	g_autofree gchar *tvdb_before = NULL;
	g_autofree gchar *tmdb_before = NULL;
	AxisMetaSeasonAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 title;

	title = lookup(fx, "tmdb", "7051");
	one = season_of(fx, title, NULL, 1);
	aired = season_of(fx, title, "tvdb", 1);
	tvdb_before = snapshot(fx, "tvdb");
	tmdb_before = snapshot(fx, "tmdb");
	axis_test_tvdb_empty_order(fx->tvdb, TRUE);
	g_mutex_lock(&fx->stand.lock);
	fx->stand.empty = TRUE;
	g_mutex_unlock(&fx->stand.lock);
	axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'");

	g_assert_true(axis_meta_get_source_season(fx->meta, title, "tvdb", 1, WAIT, &answer, &error));
	g_assert_no_error(error);
	g_assert_true(answer.saved);
	g_assert_nonnull(answer.season);
	g_assert_cmpuint(answer.season->episodes->len, ==, 5);
	axis_meta_season_answer_clear(&answer);
	g_assert_true(axis_meta_get_source_season(fx->meta, title, NULL, 1, WAIT, &answer, &error));
	g_assert_true(answer.saved);
	g_assert_cmpuint(answer.season->episodes->len, ==, 5);
	axis_meta_season_answer_clear(&answer);
	{
		g_autofree gchar *tvdb_after = snapshot(fx, "tvdb");
		g_autofree gchar *tmdb_after = snapshot(fx, "tmdb");

		g_assert_cmpstr(tvdb_after, ==, tvdb_before);
		g_assert_cmpstr(tmdb_after, ==, tmdb_before);
	}
}

/* An empty season refused moves its fetch time (it does not stay first in
 * every refresh's batch), and the same empty answer given again on separate
 * refreshes spread over a day is taken as a real removal. */
static void
test_empty_season_taken_in_time(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisMetaSeason) one = NULL;
	AxisMetaSeasonAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 title;
	guint round;

	title = lookup(fx, "tmdb", "7051");
	one = season_of(fx, title, NULL, 1);
	g_mutex_lock(&fx->stand.lock);
	fx->stand.empty = TRUE;
	g_mutex_unlock(&fx->stand.lock);
	for (round = 1; round <= 2; round++) {
		axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'"
			" WHERE source = 'tmdb' AND number = 1");
		g_assert_true(axis_meta_get_source_season(fx->meta, title, NULL, 1, WAIT, &answer, &error));
		g_assert_true(answer.saved);
		g_assert_cmpuint(answer.season->episodes->len, ==, 5);
		axis_meta_season_answer_clear(&answer);
		g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season WHERE source = 'tmdb' AND number = 1"
			" AND episodes_fetched_at > now() - interval '1 hour'"), ==, 1);
		g_assert_cmpint(query_int(fx, "SELECT empty_answers FROM meta_season WHERE source = 'tmdb' AND number = 1"),
			==, round);
	}
	/* The third, a day after the first: the season is empty now. */
	axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days',"
		" empty_since = now() - interval '2 days' WHERE source = 'tmdb' AND number = 1");
	g_assert_true(axis_meta_get_source_season(fx->meta, title, NULL, 1, WAIT, &answer, &error));
	g_assert_false(answer.saved);
	g_assert_cmpuint(answer.season->episodes->len, ==, 0);
	axis_meta_season_answer_clear(&answer);
	g_assert_cmpint(query_int(fx, "SELECT empty_answers FROM meta_season WHERE source = 'tmdb' AND number = 1"), ==, 0);
}

/* Each source is refreshed apart, from its own provider: a stale order is
 * fetched once, whole; TMDb's fresh seasons are not asked again. */
static void
test_refresh_per_source(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisMetaSeason) one = NULL;
	g_autoptr(AxisMetaSeason) aired = NULL;
	g_autoptr(GError) error = NULL;
	AxisMetaRefreshReport report;
	AxisTestPlex *api = axis_test_tvdb_api(fx->tvdb);
	gint64 title;

	title = lookup(fx, "tmdb", "7051");
	one = season_of(fx, title, NULL, 1);
	aired = season_of(fx, title, "tvdb", 1);
	axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'"
		" WHERE source = 'tvdb'");
	axis_test_db_exec(fx->db, "UPDATE meta_title SET viewed_at = now()");
	axis_test_plex_reset_requests(api);
	axis_test_plex_reset_requests(fx->tmdb_api);
	g_assert_true(axis_meta_run_refresh(fx->meta, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.failed, ==, 0);
	g_assert_cmpuint(axis_test_plex_count(api, "/v4/series/81001/episodes/official/eng"), ==, 3);
	g_assert_cmpuint(axis_test_plex_count(fx->tmdb_api, "/3/tv/7051/season/1"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_season WHERE source = 'tvdb'"
		" AND episodes_fetched_at > now() - interval '1 hour'"), ==, 3);
}

/* ── The migration, on a database that held a series before it ──── */

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static GPtrArray *
migrations_up_to(guint last)
{
	g_autoptr(GDir) dir = g_dir_open(AXIS_MIGRATION_DIR, 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	const gchar *entry;
	guint i;

	g_assert_nonnull(dir);
	while ((entry = g_dir_read_name(dir)) != NULL) {
		guint version = 0;

		if (axis_db_migration_parse_filename(entry, &version, NULL) && version <= last)
			g_ptr_array_add(names, g_strdup(entry));
	}
	g_ptr_array_sort(names, compare_names);
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename(AXIS_MIGRATION_DIR, name, NULL);
		AxisDbMigration *m = g_new0(AxisDbMigration, 1);
		gchar *sql = NULL;
		gchar *base = NULL;

		g_assert_true(g_file_get_contents(path, &sql, NULL, NULL));
		g_assert_true(axis_db_migration_parse_filename(name, &m->version, &base));
		m->name = base;
		m->sql = sql;
		m->no_transaction = axis_db_migration_has_no_transaction_marker(sql);
		g_ptr_array_add(out, m);
	}
	return out;
}

static void
migrations_free(GPtrArray *list)
{
	guint i;

	for (i = 0; i < list->len; i++) {
		AxisDbMigration *m = g_ptr_array_index(list, i);

		g_free((gchar *)m->name);
		g_free((gchar *)m->sql);
	}
	g_ptr_array_unref(list);
}

static void
migrate_to(
	AxisDbPool *pool,
	guint       last
){
	GPtrArray *list = migrations_up_to(last);
	g_autoptr(GArray) flat = g_array_new(FALSE, FALSE, sizeof(AxisDbMigration));
	g_autoptr(GError) error = NULL;
	guint i;

	for (i = 0; i < list->len; i++)
		g_array_append_vals(flat, g_ptr_array_index(list, i), 1);
	g_assert_cmpuint(flat->len, ==, last);
	g_assert_true(axis_db_migrate_with(pool, (const AxisDbMigration *)flat->data, flat->len, &error));
	g_assert_no_error(error);
	migrations_free(list);
}

/* Every series existing before the migration follows TMDb and shows
 * exactly what it did; its rows are known as TMDb's and remembered. */
static void
test_migration_keeps_series(void)
{
	g_autoptr(AxisTestDb) db = axis_test_db_new();
	g_autoptr(GError) error = NULL;
	AxisDbPool *pool;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	if (db == NULL)
		return;
	pool = axis_db_pool_new(axis_test_db_get_url(db), 2, &error);
	g_assert_no_error(error);
	migrate_to(pool, 56);
	axis_test_db_exec(db, "INSERT INTO meta_title (kind, title, provider, provider_id, year) VALUES"
		" ('series', 'Wobbly Lighthouse', 'tmdb', '7051', 2023)");
	axis_test_db_exec(db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series', 'tmdb',"
		" '7051' FROM meta_title");
	axis_test_db_exec(db, "INSERT INTO meta_season (title_id, number, episode_count, episodes_fetched_at)"
		" SELECT id, n, 2, now() FROM meta_title, generate_series(1, 2) AS n");
	axis_test_db_exec(db, "INSERT INTO meta_episode (title_id, season_number, number, name, provider_id)"
		" SELECT s.title_id, s.number, e, 'Episode ' || e, '9707' || s.number || e FROM meta_season s,"
		" generate_series(1, 2) AS e");
	/* The built-in quality profiles are the migrations' own. */
	axis_test_db_exec(db, "INSERT INTO series (title_id, quality_profile_id) SELECT t.id, (SELECT min(id) FROM"
		" quality_profile) FROM meta_title t");
	axis_test_db_exec(db, "INSERT INTO series_episode (series_id, provider_id, season_number, number, monitored)"
		" SELECT s.id, e.provider_id, e.season_number, e.number, true FROM series s JOIN meta_episode e"
		" ON e.title_id = s.title_id");
	assert_db(db, "SELECT count(*) FROM series_episode", "4");
	before = axis_test_db_query(db, "SELECT string_agg(season_number || 'x' || number || ':' || name, ','"
		" ORDER BY season_number, number) FROM meta_episode");

	migrate_to(pool, axis_db_migrate_latest_version());
	after = axis_test_db_query(db, "SELECT string_agg(season_number || 'x' || number || ':' || name, ','"
		" ORDER BY season_number, number) FROM meta_episode_followed");
	g_assert_cmpstr(after, ==, before);
	assert_db(db, "SELECT string_agg(DISTINCT source, ',') FROM meta_season", "tmdb");
	assert_db(db, "SELECT string_agg(DISTINCT source, ',') FROM meta_episode", "tmdb");
	assert_db(db, "SELECT count(*) || ':' || count(*) FILTER (WHERE episode_source = 'tmdb') FROM series", "1:1");
	assert_db(db, "SELECT (SELECT count(*) FROM series_episode WHERE source = 'tmdb') || ':' ||"
		" (SELECT count(*) FROM series_episode_identity WHERE source = 'tmdb')", "4:4");
	/* A season and an episode row added as before (naming no source) are
	 * their record provider's and their series'. */
	axis_test_db_exec(db, "INSERT INTO meta_season (title_id, number) SELECT id, 3 FROM meta_title");
	assert_db(db, "SELECT source FROM meta_season WHERE number = 3", "tmdb");
	axis_test_db_exec(db, "INSERT INTO series_episode (series_id, season_number, number, monitored)"
		" SELECT id, 3, 1, true FROM series");
	assert_db(db, "SELECT source FROM series_episode WHERE season_number = 3", "tmdb");
	axis_db_pool_free(pool);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/meta/sources/one-title-two-sources", Fixture, NULL, test_one_title_two_sources);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/record-provider-keeps", Fixture, NULL, test_record_provider_keeps);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/record-moves-to-tmdb", Fixture, NULL, test_record_moves_to_tmdb);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/record-moves-keeps-own-id", Fixture, NULL, test_record_moves_keeps_own_id);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/order-stored-apart", Fixture, NULL, test_order_stored_apart);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/identifiers-disagree", Fixture, NULL, test_identifiers_disagree);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/refresh-per-source", Fixture, NULL, test_refresh_per_source);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/empty-answers-refused", Fixture, NULL, test_empty_answers_refused);
	AXIS_TEST_ADD_DB_CASE("/meta/sources/empty-season-taken-in-time", Fixture, NULL, test_empty_season_taken_in_time);
	g_test_add_func("/meta/sources/migration-keeps-series", test_migration_keeps_series);
	return g_test_run();
}
