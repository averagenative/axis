/*
 * test-meta.c - the metadata layer against the fixture provider and its two
 * fake servers (API and images) on loopback, with a real database and a data
 * directory of the test's own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (metadata):
 *   One record; Fetched again; Malformed answer
 *   Provider unreachable; Search while the provider is down
 *   Rate limit; Told to wait; Unreferenced titles
 *   Key stays put; What is sent
 *   Not an image; Cache bounded
 * and the rest of the layer's promises: negative answers are kept for a
 * while, each freshness rule, questions asked twice at once are asked once,
 * artwork is read back by its hash only.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-storage.h"
#include "axis-version.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "meta/axis-meta.h"
#include "web/axis-web-meta.h"

#define KEY AXIS_TEST_META_KEY
#define WAIT 5000

typedef struct
{
	gdouble rate;
	guint   burst;
} Budget;

typedef struct
{
	AxisTestDb          *db;
	AxisDbPool          *pool;
	AxisSettings        *settings;
	gchar               *dir;
	gchar               *data_path;
	gchar               *art_path;
	AxisDataDir         *data;
	AxisTestMetaFixture *fa;	/* "fixture" */
	AxisTestMetaFixture *fb;	/* "fixtureb" */
	AxisMeta            *meta;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	const Budget *budget = data;
	g_autoptr(GError) error = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->data_path = g_build_filename(fx->dir, "data", NULL);
	fx->art_path = g_build_filename(fx->data_path, "artwork", NULL);
	fx->data = axis_data_dir_open(fx->data_path, &error);
	g_assert_no_error(error);
	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", budget != NULL ? budget->rate : 50.0,
		budget != NULL ? budget->burst : 10);
	fx->fb = axis_test_meta_fixture_new("fixtureb", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fb->info, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixtureb_key", KEY "-b", &error));
	/* The settings table is made once per set of provider names, with the
	 * first one's default rate: a test with its own budget sets it. */
	if (budget != NULL) {
		gchar rate[G_ASCII_DTOSTR_BUF_SIZE];

		g_ascii_formatd(rate, sizeof(rate), "%.2f", budget->rate);
		g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", rate, &error));
	}
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
	axis_test_meta_fixture_standard(fx->fa);
	axis_test_meta_fixture_standard(fx->fb);
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
	/* The providers' info outlived the layer. */
	axis_test_meta_fixture_free(fx->fa);
	axis_test_meta_fixture_free(fx->fb);
	if (fx->data != NULL)
		axis_data_dir_unref(fx->data);
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_free(fx->data_path);
	g_free(fx->art_path);
	axis_test_storage_dir_free(fx->dir);
	axis_test_log_reset();
	/* The settings store keeps its tables' validators; nothing else lingers. */
	while (g_main_context_iteration(NULL, FALSE))
		;
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static void
search_ok(
	Fixture              *fx,
	const gchar          *provider,
	AxisMetaKind          kind,
	const gchar          *text,
	guint                 wait_ms,
	AxisMetaSearchAnswer *answer
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_search(fx->meta, provider, kind, text, 0, wait_ms, answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer->titles);
}

static void
lookup_ok(
	Fixture             *fx,
	AxisMetaKind         kind,
	const gchar         *id,
	guint                wait_ms,
	AxisMetaTitleAnswer *answer
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_lookup(fx->meta, "fixture", kind, id, wait_ms, answer, &error));
	g_assert_no_error(error);
}

static gint64
query_int(
	Fixture     *fx,
	const gchar *sql
){
	g_autofree gchar *text = axis_test_db_query(fx->db, sql);

	return text != NULL ? g_ascii_strtoll(text, NULL, 10) : -1;
}

static void
route_text(
	AxisTestPlex *plex,
	const gchar  *path,
	guint         status,
	const gchar  *text
){
	axis_test_plex_route(plex, path, -1, AXIS_TEST_PLEX_NORMAL, status, text, 0);
}

static void
assert_key_absent(const gchar *text)
{
	if (text != NULL && strstr(text, KEY) != NULL)
		g_error("the key appears in: %s", text);
}

/* ── One record; fetched again ──────────────────────────────────── */

/* Spec: One record */
static void
test_one_record(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	AxisMetaSearchAnswer find;
	AxisMetaSearchAnswer again;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;
	gint64 id;

	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "The Mazework", WAIT, &search);
	g_assert_false(search.saved);
	g_assert_false(search.stale);
	g_assert_cmpint(search.fetched_us, >, 0);
	g_assert_cmpuint(search.titles->len, ==, 3);
	g_assert_true(search.provider == &fx->fa->info);
	id = ((AxisMetaTitle *)g_ptr_array_index(search.titles, 0))->id;
	g_assert_cmpint(id, >, 0);
	g_assert_cmpstr(((AxisMetaTitle *)g_ptr_array_index(search.titles, 0))->title, ==, "The Mazework");

	g_assert_true(axis_meta_find(fx->meta, "fixture", AXIS_META_KIND_FILM, "imdb", "tt0600003", WAIT, &find, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(find.titles->len, ==, 1);
	g_assert_cmpint(((AxisMetaTitle *)g_ptr_array_index(find.titles, 0))->id, ==, id);

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &title);
	g_assert_nonnull(title.title);
	g_assert_cmpint(title.title->id, ==, id);
	g_assert_true(title.title->detail);
	g_assert_cmpint(title.title->runtime, ==, 136);
	g_assert_cmpuint(title.title->alt_titles->len, ==, 2);
	g_assert_cmpuint(title.title->genres->len, ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 3);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_external_id WHERE scheme = 'imdb'"), ==, 2);

	/* The same search, typed differently, is answered from the store. */
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "  The   MAZEWORK ", WAIT, &again);
	g_assert_cmpuint(again.titles->len, ==, 3);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);

	axis_meta_search_answer_clear(&search);
	axis_meta_search_answer_clear(&find);
	axis_meta_search_answer_clear(&again);
	axis_meta_title_answer_clear(&title);
}

/*
 * fix-identify-hunt-findings, item B: a search's answer never replaces a
 * stored full record's year and dates (a search may give a date of another
 * region); before a full record is stored, it gives them.  Made-up titles.
 */
static void
test_search_keeps_full_year(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer full;
	AxisMetaTitleAnswer again;
	AxisMetaSearchAnswer search;
	AxisMetaSearchAnswer second;
	g_autoptr(GError) error = NULL;

	route_text(fx->fa->api, "/search/film", 200, "{\"results\": [{\"id\": \"901\", \"kind\": \"film\", \"title\":"
		" \"Quarry Lights\", \"year\": 2015, \"release_date\": \"2015-04-01\"}, {\"id\": \"902\", \"kind\": \"film\","
		" \"title\": \"Quarry Lights\", \"year\": 1988, \"release_date\": \"1988-02-02\"}]}");
	route_text(fx->fa->api, "/title/film/901", 200, "{\"id\": \"901\", \"kind\": \"film\", \"title\": \"Quarry Lights\","
		" \"year\": 2014, \"release_date\": \"2014-11-20\", \"runtime\": 99, \"status\": \"released\"}");

	lookup_ok(fx, AXIS_META_KIND_FILM, "901", WAIT, &full);
	g_assert_cmpint(full.title->year, ==, 2014);
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "Quarry Lights", WAIT, &search);
	g_assert_cmpuint(search.titles->len, ==, 2);
	/* Stored: the full record's year and date stand. */
	g_assert_cmpint(query_int(fx, "SELECT year FROM meta_title WHERE provider_id = '901'"), ==, 2014);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE provider_id = '901'"
		" AND release_date = '2014-11-20'"), ==, 1);
	lookup_ok(fx, AXIS_META_KIND_FILM, "901", WAIT, &again);
	g_assert_cmpint(again.title->year, ==, 2014);
	g_assert_cmpstr(again.title->release_date, ==, "2014-11-20");
	/* A title with no full record takes the search's. */
	g_assert_cmpint(query_int(fx, "SELECT year FROM meta_title WHERE provider_id = '902'"), ==, 1988);

	/* The second page is a lookup of its own, asked for as page 2. */
	g_assert_true(axis_meta_search_page(fx->meta, "fixture", AXIS_META_KIND_FILM, "Quarry Lights", 0, 2, WAIT, &second,
		&error));
	g_assert_no_error(error);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search WHERE lookup = 'search2'"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search WHERE lookup = 'search'"), ==, 1);
	axis_meta_search_answer_clear(&second);
	g_assert_false(axis_meta_search_page(fx->meta, "fixture", AXIS_META_KIND_FILM, "Quarry Lights", 0, 3, WAIT,
		&second, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_INVALID);

	axis_meta_title_answer_clear(&full);
	axis_meta_title_answer_clear(&again);
	axis_meta_search_answer_clear(&search);
	axis_meta_search_answer_clear(&second);
}

/*
 * fix-identify-regional-years: migration 0054 drops TMDb's stored answers of
 * film searches, which were asked with region=US and gave the region's
 * year; the next search asks again, and its answer gives the year of a
 * title with no full record stored.  Other stored lookups stay.  (The
 * migration's statement is run as the file holds it; made-up titles.)
 */
static void
test_regional_searches_dropped(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *insert = NULL;

	route_text(fx->fa->api, "/search/film", 200, "{\"results\": [{\"id\": \"921\", \"kind\": \"film\", \"title\":"
		" \"Paper Lantern Sky\", \"year\": 2005, \"release_date\": \"2005-03-04\"}]}");
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "Paper Lantern Sky", WAIT, &search);
	axis_meta_search_answer_clear(&search);
	g_assert_cmpint(query_int(fx, "SELECT year FROM meta_title WHERE provider_id = '921'"), ==, 2005);
	/* TMDb's stored lookups, as searches with region=US left them. */
	id = axis_test_db_query(fx->db, "SELECT id FROM meta_title WHERE provider_id = '921'");
	insert = g_strdup_printf("INSERT INTO meta_search (provider, kind, lookup, query, year, result_ids) VALUES"
		" ('tmdb', 'film', 'search', 'paper lantern sky', 0, ARRAY[%s]::bigint[]),"
		" ('tmdb', 'film', 'search2', 'paper lantern sky', 0, '{}'),"
		" ('tmdb', 'film', 'find', 'imdb:tt7000092', 0, ARRAY[%s]::bigint[]),"
		" ('tmdb', 'series', 'search', 'paper lantern sky', 0, '{}')", id, id);
	axis_test_db_exec(fx->db, insert);
	g_assert_true(g_file_get_contents("data/migrations/0054_identify_regional_years.sql", &sql, NULL, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->db, sql);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search WHERE provider = 'tmdb' AND kind = 'film'"
		" AND lookup IN ('search', 'search2')"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search WHERE provider = 'tmdb'"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search WHERE provider = 'fixture'"), ==, 1);

	/* With its stored answer gone, the next search asks again, and the year
	 * it gives replaces the region's on the title with no full record. */
	axis_test_db_exec(fx->db, "DELETE FROM meta_search WHERE provider = 'fixture'");
	axis_test_plex_clear_routes(fx->fa->api);
	route_text(fx->fa->api, "/search/film", 200, "{\"results\": [{\"id\": \"921\", \"kind\": \"film\", \"title\":"
		" \"Paper Lantern Sky\", \"year\": 1988, \"release_date\": \"1988-04-16\"}]}");
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "Paper Lantern Sky", WAIT, &search);
	axis_meta_search_answer_clear(&search);
	g_assert_cmpint(query_int(fx, "SELECT year FROM meta_title WHERE provider_id = '921'"), ==, 1988);
}

/*
 * Review of fix-identify-hunt-findings, item 9: the name keys the migration
 * empties are made again before anything reads them (the daemon's start
 * calls this after the migrations), each with the letters now folded.
 */
static void
test_fill_every_name_key(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	g_autoptr(GError) error = NULL;

	route_text(fx->fa->api, "/search/film", 200, "{\"results\": [{\"id\": \"911\", \"kind\": \"film\", \"title\":"
		" \"S\\u00f8ren's Winter\", \"year\": 2012}, {\"id\": \"912\", \"kind\": \"film\", \"title\": \"\\u00c6r\\u00f8"
		" Tide\", \"year\": 2013}]}");
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "Winter", WAIT, &search);
	axis_meta_search_answer_clear(&search);
	/* As the migration leaves them. */
	axis_test_db_exec(fx->db, "UPDATE meta_title SET name_keys = NULL");
	g_assert_true(axis_meta_fill_every_name_key(fx->pool, &error));
	g_assert_no_error(error);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE name_keys IS NULL"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE name_keys = 'sorenswinter'"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE name_keys = 'aerotide'"), ==, 1);
	/* Nothing to do: still fine. */
	g_assert_true(axis_meta_fill_every_name_key(fx->pool, &error));
	g_assert_no_error(error);
}

/* Spec: Fetched again */
static void
test_fetched_again_in_place(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer first;
	AxisMetaTitleAnswer cached;
	AxisMetaTitleAnswer again;
	gint64 id;

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &first);
	id = first.title->id;
	route_text(fx->fa->api, "/title/film/101", 200, "{\"id\": \"101\", \"kind\": \"film\", \"title\": \"The Mazework\","
		" \"runtime\": 137, \"status\": \"released\", \"ids\": {\"imdb\": \"tt0600003\"}}");

	/* Fresh: the store answers. */
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &cached);
	g_assert_cmpint(cached.title->runtime, ==, 136);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/101"), ==, 1);

	/* Stale: fetched again, the same record updated; what the new answer
	 * lacks (the overview, the other names) is kept. */
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '40 days'");
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &again);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/101"), ==, 2);
	g_assert_cmpint(again.title->id, ==, id);
	g_assert_cmpint(again.title->runtime, ==, 137);
	g_assert_nonnull(again.title->overview);
	g_assert_cmpuint(again.title->alt_titles->len, ==, 2);
	g_assert_false(again.stale);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);

	axis_meta_title_answer_clear(&first);
	axis_meta_title_answer_clear(&cached);
	axis_meta_title_answer_clear(&again);
}

/* Two records that turn out to be one title become one. */
static void
test_two_records_merge(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	AxisMetaSearchAnswer find;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;
	gint64 first;
	gint64 second;

	/* A search that knows 101 without its IMDb id, and a find that knows
	 * the IMDb id under another of the provider's ids. */
	route_text(fx->fa->api, "/search/film", 200, "{\"results\": [{\"id\": \"101\", \"title\": \"The Mazework\"}]}");
	route_text(fx->fa->api, "/find/film", 200, "{\"results\": [{\"id\": \"107\", \"title\": \"The Mazework\","
		" \"ids\": {\"imdb\": \"tt0600003\"}}]}");
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", WAIT, &search);
	g_assert_true(axis_meta_find(fx->meta, "fixture", AXIS_META_KIND_FILM, "imdb", "tt0600003", WAIT, &find, &error));
	first = ((AxisMetaTitle *)g_ptr_array_index(search.titles, 0))->id;
	second = ((AxisMetaTitle *)g_ptr_array_index(find.titles, 0))->id;
	g_assert_cmpint(first, !=, second);

	/* What two connections brought over: the first only the record that is
	 * merged away, the second both. */
	{
		g_autofree gchar *brought = g_strdup_printf("INSERT INTO arr_brought (source_id, title_id, label) SELECT s.id, t.id, 'Made Up Film' FROM arr_source s,"
			" meta_title t WHERE (s.name = 'One' AND t.id = %" G_GINT64_FORMAT ") OR (s.name = 'Two' AND t.id IN (%"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT "))", MAX(first, second), first, second);

		axis_test_db_exec(fx->db, "INSERT INTO arr_source (app, name, origin) VALUES ('radarr', 'One', "
			"'http://radarr-one:7878'), ('radarr', 'Two', 'http://radarr-two:7878')");
		axis_test_db_exec(fx->db, brought);
	}

	/* The full record of 101 names the IMDb id: both are the one title. */
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &title);
	if (title.saved)
		g_error("not stored: %s", title.problem);
	g_assert_cmpint(title.title->id, ==, MIN(first, second));
	/* The first connection's record moved to the title that is left; the
	 * second's is one, not two. */
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM arr_brought b JOIN meta_title t ON t.id = b.title_id"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(DISTINCT source_id) FROM arr_brought"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_external_id WHERE scheme = 'imdb'"), ==, 1);
	/* The saved lookups point at the record that is left. */
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search WHERE result_ids <> '{}'"
		" AND NOT result_ids <@ ARRAY(SELECT id FROM meta_title)"), ==, 0);

	axis_meta_search_answer_clear(&search);
	axis_meta_search_answer_clear(&find);
	axis_meta_title_answer_clear(&title);
}

/* Spec: Malformed answer */
static void
test_malformed_answer_changes_nothing(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad[] = {
		"{not json",
		"{\"id\": \"101\", \"kind\": \"film\", \"title\": \"\"}",
		"{\"id\": \"999\", \"kind\": \"film\", \"title\": \"Another title\"}",
		"[]",
		NULL
	};
	AxisMetaTitleAnswer first;
	AxisMetaSearchAnswer search;
	g_autofree gchar *before = NULL;
	g_autofree gchar *search_before = NULL;
	guint i;

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &first);
	axis_meta_title_answer_clear(&first);
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", WAIT, &search);
	axis_meta_search_answer_clear(&search);
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '40 days'");
	axis_test_db_exec(fx->db, "UPDATE meta_search SET fetched_at = now() - interval '2 days'");
	before = axis_test_db_query(fx->db, "SELECT string_agg(to_jsonb(t)::text, ',' ORDER BY id) FROM"
		" (SELECT id, kind, title, overview, runtime_minutes, genres, poster_ref, provider_id, detail_fetched_at,"
		" fetched_at FROM meta_title) t");
	search_before = axis_test_db_query(fx->db, "SELECT string_agg(to_jsonb(s)::text, ',') FROM meta_search s");

	for (i = 0; bad[i] != NULL; i++) {
		AxisMetaTitleAnswer answer;
		g_autofree gchar *after = NULL;

		route_text(fx->fa->api, "/title/film/101", 200, bad[i]);
		lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
		/* What was stored is shown, with why the new answer was not used. */
		g_assert_nonnull(answer.title);
		g_assert_true(answer.saved);
		g_assert_true(answer.stale);
		g_assert_nonnull(answer.problem);
		g_assert_nonnull(strstr(answer.problem, "cannot be"));
		after = axis_test_db_query(fx->db, "SELECT string_agg(to_jsonb(t)::text, ',' ORDER BY id) FROM"
			" (SELECT id, kind, title, overview, runtime_minutes, genres, poster_ref, provider_id, detail_fetched_at,"
			" fetched_at FROM meta_title) t");
		g_assert_cmpstr(after, ==, before);
		axis_meta_title_answer_clear(&answer);
	}

	route_text(fx->fa->api, "/search/film", 200, "{\"results\": 7}");
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", WAIT, &search);
	g_assert_true(search.saved);
	g_assert_cmpuint(search.titles->len, ==, 3);
	{
		g_autofree gchar *search_after = axis_test_db_query(fx->db,
			"SELECT string_agg(to_jsonb(s)::text, ',') FROM meta_search s");

		g_assert_cmpstr(search_after, ==, search_before);
	}
	axis_meta_search_answer_clear(&search);
}

/* ── A provider that does not answer ────────────────────────────── */

/* Spec: Provider unreachable */
static void
test_provider_down_title_with_age(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer first;
	AxisMetaTitleAnswer answer;
	gint64 fetched;

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &first);
	axis_meta_title_answer_clear(&first);
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '40 days'");
	fetched = query_int(fx, "SELECT (extract(epoch FROM detail_fetched_at) * 1000000)::bigint FROM meta_title");
	axis_test_plex_route(fx->fa->api, "/title/film/101", -1, AXIS_TEST_PLEX_STATUS, 500, NULL, 500);

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
	g_assert_nonnull(answer.title);
	g_assert_cmpstr(answer.title->title, ==, "The Mazework");
	g_assert_true(answer.saved);
	g_assert_true(answer.stale);
	g_assert_cmpint(answer.fetched_us, ==, fetched);
	g_assert_nonnull(strstr(answer.problem, "500"));
	axis_meta_title_answer_clear(&answer);
}

/* Spec: Search while the provider is down */
static void
test_provider_down_search_saved(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer first;
	AxisMetaSearchAnswer answer;
	AxisMetaSearchAnswer nothing;
	gint64 fetched;
	gint64 start;

	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", WAIT, &first);
	axis_meta_search_answer_clear(&first);
	axis_test_db_exec(fx->db, "UPDATE meta_search SET fetched_at = now() - interval '2 days'");
	fetched = query_int(fx, "SELECT (extract(epoch FROM fetched_at) * 1000000)::bigint FROM meta_search");
	axis_test_plex_route(fx->fa->api, "/search/film", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);

	start = g_get_monotonic_time();
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", 500, &answer);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 2 * G_USEC_PER_SEC);
	g_assert_true(answer.saved);
	g_assert_true(answer.stale);
	g_assert_cmpuint(answer.titles->len, ==, 3);
	g_assert_cmpint(answer.fetched_us, ==, fetched);
	g_assert_nonnull(strstr(answer.problem, "did not answer"));

	/* Nothing saved for this one: an empty answer, still not an error. */
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "something else", 300, &nothing);
	g_assert_true(nothing.saved);
	g_assert_cmpuint(nothing.titles->len, ==, 0);
	g_assert_cmpint(nothing.fetched_us, ==, 0);
	axis_meta_search_answer_clear(&answer);
	axis_meta_search_answer_clear(&nothing);
}

/* A question asked twice at once is asked once. */
typedef struct
{
	Fixture             *fx;
	AxisMetaSearchAnswer answer;
	const gchar         *text;
	guint                wait_ms;
} Asker;

static gpointer
ask_in_thread(gpointer data)
{
	Asker *asker = data;
	g_autoptr(GError) error = NULL;

	if (!axis_meta_search(asker->fx->meta, "fixture", AXIS_META_KIND_FILM, asker->text, 0, asker->wait_ms,
	        &asker->answer, &error))
		g_error("search failed: %s", error->message);
	return NULL;
}

static void
test_asked_once(Fixture *fx, gconstpointer data)
{
	Asker askers[4];
	GThread *threads[4];
	guint i;

	for (i = 0; i < 4; i++) {
		memset(&askers[i], 0, sizeof(askers[i]));
		askers[i].fx = fx;
		askers[i].text = i % 2 == 0 ? "Mazework" : "mazework";
		askers[i].wait_ms = WAIT;
		threads[i] = g_thread_new("asker", ask_in_thread, &askers[i]);
	}
	for (i = 0; i < 4; i++) {
		g_thread_join(threads[i]);
		g_assert_cmpuint(askers[i].answer.titles->len, ==, 3);
		axis_meta_search_answer_clear(&askers[i].answer);
	}
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);
}

static void
test_negative_answer_kept(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer answer;

	axis_test_plex_route(fx->fa->api, "/title/film/999", -1, AXIS_TEST_PLEX_STATUS, 404, NULL, 404);
	lookup_ok(fx, AXIS_META_KIND_FILM, "999", WAIT, &answer);
	g_assert_true(answer.not_found);
	g_assert_null(answer.title);
	axis_meta_title_answer_clear(&answer);
	lookup_ok(fx, AXIS_META_KIND_FILM, "999", WAIT, &answer);
	g_assert_true(answer.not_found);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/999"), ==, 1);
	/* After its day, asked again. */
	axis_test_db_exec(fx->db, "UPDATE meta_search SET fetched_at = now() - interval '2 days'");
	lookup_ok(fx, AXIS_META_KIND_FILM, "999", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/999"), ==, 2);
}

static void
test_seasons(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer series;
	AxisMetaSeasonAnswer season;
	g_autoptr(GError) error = NULL;
	gint64 id;

	lookup_ok(fx, AXIS_META_KIND_SERIES, "201", WAIT, &series);
	g_assert_nonnull(series.title);
	g_assert_cmpuint(series.title->seasons->len, ==, 2);
	id = series.title->id;
	axis_meta_title_answer_clear(&series);

	g_assert_true(axis_meta_get_season(fx->meta, id, 2, WAIT, &season, &error));
	g_assert_no_error(error);
	g_assert_nonnull(season.season);
	g_assert_cmpuint(season.season->episodes->len, ==, 2);
	g_assert_cmpint(((AxisMetaEpisode *)g_ptr_array_index(season.season->episodes, 0))->absolute_number, ==, 4);
	axis_meta_season_answer_clear(&season);
	g_assert_true(axis_meta_get_season(fx->meta, id, 2, WAIT, &season, &error));
	axis_meta_season_answer_clear(&season);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201/season/2"), ==, 1);

	/* A film has no seasons. */
	g_assert_false(axis_meta_get_season(fx->meta, 999999, 1, WAIT, &season, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND);
}

static const AxisMetaEpisode *
episode_numbered(
	const AxisMetaSeason *season,
	gint                  number
){
	guint i;

	for (i = 0; i < season->episodes->len; i++) {
		const AxisMetaEpisode *episode = g_ptr_array_index(season->episodes, i);

		if (episode->number == number)
			return episode;
	}
	return NULL;
}

/* An episode keeps the provider's own id when the provider numbers it anew;
 * a provider that gives no ids still works. */
static void
test_episode_ids(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer series;
	AxisMetaSeasonAnswer season;
	g_autoptr(GError) error = NULL;
	gint64 id;

	axis_test_plex_clear_routes(fx->fa->api);
	axis_test_plex_route_file(fx->fa->api, "/title/series/201", -1, "tests/fixtures/meta/title-series-201.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/201/season/1", -1, "tests/fixtures/meta/season-201-1.json");
	route_text(fx->fa->api, "/title/series/201/season/2", 200, "{\"number\": 2, \"name\": \"Season 2\","
		" \"episodes\": [{\"number\": 1, \"id\": \"e9001\", \"name\": \"Back Again\", \"air_date\": \"2026-09-04\"},"
		" {\"number\": 2, \"id\": 9002, \"name\": \"Next Door\", \"air_date\": \"2026-09-11\"},"
		" {\"number\": 3, \"id\": \"e9001\", \"name\": \"An id listed twice is kept by the first\"},"
		" {\"number\": 4, \"id\": \"not valid!\", \"name\": \"Not an id\"}]}");
	lookup_ok(fx, AXIS_META_KIND_SERIES, "201", WAIT, &series);
	id = series.title->id;
	axis_meta_title_answer_clear(&series);

	g_assert_true(axis_meta_get_season(fx->meta, id, 2, WAIT, &season, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(season.season->episodes->len, ==, 4);
	g_assert_cmpstr(episode_numbered(season.season, 1)->provider_id, ==, "e9001");
	g_assert_cmpstr(episode_numbered(season.season, 2)->provider_id, ==, "9002");
	g_assert_null(episode_numbered(season.season, 3)->provider_id);
	g_assert_null(episode_numbered(season.season, 4)->provider_id);
	axis_meta_season_answer_clear(&season);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_episode WHERE provider_id = 'e9001'"), ==, 1);

	/* Numbered anew: the ids go with the episodes. */
	axis_test_plex_clear_routes(fx->fa->api);
	axis_test_plex_route_file(fx->fa->api, "/title/series/201", -1, "tests/fixtures/meta/title-series-201.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/201/season/1", -1, "tests/fixtures/meta/season-201-1.json");
	route_text(fx->fa->api, "/title/series/201/season/2", 200, "{\"number\": 2, \"name\": \"Season 2\","
		" \"episodes\": [{\"number\": 1, \"id\": 9002, \"name\": \"Next Door\", \"air_date\": \"2026-09-11\"},"
		" {\"number\": 3, \"id\": \"e9001\", \"name\": \"Back Again\", \"air_date\": \"2026-09-04\"}]}");
	axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'");
	g_assert_true(axis_meta_get_season(fx->meta, id, 2, WAIT, &season, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(season.season->episodes->len, ==, 2);
	g_assert_cmpstr(episode_numbered(season.season, 1)->provider_id, ==, "9002");
	g_assert_cmpstr(episode_numbered(season.season, 1)->name, ==, "Next Door");
	g_assert_cmpstr(episode_numbered(season.season, 3)->provider_id, ==, "e9001");
	g_assert_null(episode_numbered(season.season, 2));
	axis_meta_season_answer_clear(&season);
	g_assert_cmpint(query_int(fx, "SELECT number FROM meta_episode WHERE provider_id = 'e9001'"), ==, 3);

	/* A provider that gives no ids (the standard season 1 has none). */
	g_assert_true(axis_meta_get_season(fx->meta, id, 1, WAIT, &season, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(season.season->episodes->len, ==, 3);
	g_assert_null(episode_numbered(season.season, 1)->provider_id);
	g_assert_cmpstr(episode_numbered(season.season, 2)->name, ==, "Number Two");
	axis_meta_season_answer_clear(&season);
}

/* ── Refresh ────────────────────────────────────────────────────── */

static gint64
title_id(
	Fixture     *fx,
	const gchar *provider_id
){
	g_autofree gchar *sql = g_strdup_printf("SELECT id FROM meta_title WHERE provider_id = '%s'", provider_id);

	return query_int(fx, sql);
}

static void
run_refresh(
	Fixture               *fx,
	AxisMetaRefreshReport *report
){
	g_autoptr(GError) error = NULL;

	axis_test_plex_reset_requests(fx->fa->api);
	g_assert_true(axis_meta_run_refresh(fx->meta, NULL, report, &error));
	g_assert_no_error(error);
}

/* Spec: Unreferenced titles, and each freshness rule */
static void
test_refresh_referenced_only(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer answer;
	AxisMetaSearchAnswer search;
	AxisMetaSeasonAnswer season;
	AxisMetaRefreshReport report;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *wanted = NULL;

	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", WAIT, &search);
	axis_meta_search_answer_clear(&search);
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	lookup_ok(fx, AXIS_META_KIND_FILM, "102", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	lookup_ok(fx, AXIS_META_KIND_SERIES, "201", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);

	/* 101 was viewed; 102 is on a module's list; 201 and 103 neither. */
	axis_test_db_exec(fx->db, "CREATE TABLE test_wanted (title_id bigint)");
	wanted = g_strdup_printf("INSERT INTO test_wanted VALUES (%" G_GINT64_FORMAT ")", title_id(fx, "102"));
	axis_test_db_exec(fx->db, wanted);
	g_assert_true(axis_meta_add_reference(fx->meta, "test", "SELECT title_id FROM test_wanted"));
	axis_test_db_exec(fx->db, "UPDATE meta_title SET viewed_at = NULL WHERE provider_id <> '101'");
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '100 days'"
		" WHERE detail_fetched_at IS NOT NULL");

	run_refresh(fx, &report);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/101"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/102"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201"), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/103"), ==, 0);
	g_assert_cmpuint(report.titles, ==, 2);

	/* Each rule: a released film is fresh for 30 days, a film not yet out
	 * for a day, a running series for a day, an ended one for 90. */
	axis_test_db_exec(fx->db, "UPDATE meta_title SET viewed_at = now()");
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '20 days',"
		" status = 'released' WHERE provider_id = '101'");
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '2 days',"
		" status = 'upcoming' WHERE provider_id = '102'");
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '2 days',"
		" status = 'continuing' WHERE provider_id = '201'");
	run_refresh(fx, &report);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/101"), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/102"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201"), ==, 1);
	/* 103, known only from a search and now viewed, gets its full record. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/103"), ==, 1);

	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '40 days',"
		" status = 'ended' WHERE provider_id = '201'");
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() WHERE provider_id <> '201'");
	axis_test_plex_route_file(fx->fa->api, "/title/series/201", -1, "tests/fixtures/meta/title-series-201.json");
	run_refresh(fx, &report);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201"), ==, 0);

	/* Seasons: the current one of a running series daily, the others as
	 * a released film. */
	axis_test_db_exec(fx->db, "UPDATE meta_title SET status = 'continuing', detail_fetched_at = now()"
		" WHERE provider_id = '201'");
	g_assert_true(axis_meta_get_season(fx->meta, title_id(fx, "201"), 1, WAIT, &season, &error));
	axis_meta_season_answer_clear(&season);
	g_assert_true(axis_meta_get_season(fx->meta, title_id(fx, "201"), 2, WAIT, &season, &error));
	axis_meta_season_answer_clear(&season);
	axis_test_db_exec(fx->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '2 days'");
	run_refresh(fx, &report);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201/season/2"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201/season/1"), ==, 0);
	g_assert_cmpuint(report.seasons, ==, 1);
}

/* A provider without a key, or one that asked to wait, is left alone. */
static void
test_refresh_skips_held_providers(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer answer;
	AxisMetaRefreshReport report;
	g_autoptr(GError) error = NULL;

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '100 days'");
	g_assert_true(axis_settings_clear(fx->settings, "metadata.fixture_key", &error));
	run_refresh(fx, &report);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, 0);
	g_assert_cmpuint(report.providers_skipped, ==, 1);
	/* fixtureb has nothing stale: asked nothing either. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fb->api), ==, 0);

	/* With its key back but asked to wait, it is still left alone. */
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", KEY, &error));
	{
		const gchar *const wait[] = { "Retry-After", "30", NULL };
		AxisMetaSearchAnswer search;

		axis_test_plex_route_full(fx->fa->api, "/search/series", 429, NULL, NULL, wait);
		search_ok(fx, "fixture", AXIS_META_KIND_SERIES, "street", WAIT, &search);
		axis_meta_search_answer_clear(&search);
	}
	run_refresh(fx, &report);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, 0);
	g_assert_cmpuint(report.providers_skipped, ==, 1);
}

/* ── Limits ─────────────────────────────────────────────────────── */

static const Budget slow_budget = { 5.0, 2 };

/* Spec: Rate limit */
static void
test_rate_limit_under_a_burst(Fixture *fx, gconstpointer data)
{
	enum { N = 12 };
	static const gchar *const texts[N] = {
		"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve"
	};
	Asker askers[N];
	GThread *threads[N];
	g_autoptr(GArray) times = NULL;
	gint64 start = g_get_monotonic_time();
	guint i;
	guint j;

	for (i = 0; i < N; i++) {
		memset(&askers[i], 0, sizeof(askers[i]));
		askers[i].fx = fx;
		askers[i].text = texts[i];
		askers[i].wait_ms = 20000;
		threads[i] = g_thread_new("burst", ask_in_thread, &askers[i]);
	}
	for (i = 0; i < N; i++) {
		g_thread_join(threads[i]);
		g_assert_false(askers[i].answer.saved);
		axis_meta_search_answer_clear(&askers[i].answer);
	}
	times = axis_test_plex_request_times(fx->fa->api);
	g_assert_cmpuint(times->len, ==, N);
	/* In any stretch, at most burst + rate * its length started (one more
	 * for the clock's grain between the client and the server). */
	for (i = 0; i < times->len; i++) {
		for (j = i + 1; j < times->len; j++) {
			gint64 span = g_array_index(times, gint64, j) - g_array_index(times, gint64, i);
			gdouble allowed = slow_budget.burst + slow_budget.rate * (gdouble)span / G_USEC_PER_SEC + 1.0;

			if ((gdouble)(j - i + 1) > allowed)
				g_error("%u requests in %" G_GINT64_FORMAT " us", j - i + 1, span);
		}
	}
	g_assert_cmpint(g_get_monotonic_time() - start, >=, (gint64)((N - slow_budget.burst) / slow_budget.rate * 0.9 *
		G_USEC_PER_SEC));
}

/* Spec: Told to wait */
static void
test_told_to_wait_pauses_one_provider(Fixture *fx, gconstpointer data)
{
	const gchar *const wait[] = { "Retry-After", "2", NULL };
	AxisMetaSearchAnswer answer;
	AxisMetaProviderStatus status;
	gint64 start;
	gint64 told;

	axis_test_plex_route_full(fx->fa->api, "/search/film", 429, "application/json", NULL, wait);
	told = g_get_monotonic_time();
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "one", WAIT, &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "wait"));
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_cmpint(status.limit.paused_until_us, >, g_get_real_time());
	axis_meta_provider_status_clear(&status);

	/* Not asked again until the stated time; the answer comes at once. */
	axis_test_plex_route_file(fx->fa->api, "/search/film", -1, "tests/fixtures/meta/search-film.json");
	start = g_get_monotonic_time();
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "two", 1000, &answer);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 500000);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "asked Axis to wait"));
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);

	/* The other provider is not held up. */
	start = g_get_monotonic_time();
	search_ok(fx, "fixtureb", AXIS_META_KIND_FILM, "mazework", WAIT, &answer);
	g_assert_false(answer.saved);
	g_assert_cmpuint(answer.titles->len, ==, 3);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 1500000);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->fb->api, "/search/film"), ==, 1);

	/* A waiter that may wait that long is answered once the pause ends;
	 * the question queued meanwhile ("two") is sent then too, not before. */
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "three", WAIT, &answer);
	g_assert_false(answer.saved);
	g_assert_cmpuint(answer.titles->len, ==, 3);
	axis_meta_search_answer_clear(&answer);
	{
		g_autoptr(GArray) times = axis_test_plex_request_times(fx->fa->api);
		guint i;

		g_assert_cmpuint(times->len, ==, 3);
		for (i = 1; i < times->len; i++)
			g_assert_cmpint(g_array_index(times, gint64, i) - told, >=, 2 * G_USEC_PER_SEC - 100000);
	}
}

/* ── What leaves the machine ────────────────────────────────────── */

static void
assert_requests_plain(
	AxisTestPlex *plex,
	gboolean      api
){
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(plex);
	const gchar *user = g_get_user_name();
	guint i;

	g_assert_cmpuint(requests->len, >, 0);
	for (i = 0; i < requests->len; i++) {
		g_auto(GStrv) lines = g_strsplit(g_ptr_array_index(requests, i), "\n", -1);
		const gchar *query;
		gboolean key = FALSE;
		gboolean agent = FALSE;
		guint j;

		g_assert_true(g_str_has_prefix(lines[0], "GET /"));
		/* Only the search text and identifiers are in the query. */
		query = strchr(lines[0], '?');
		if (query != NULL) {
			g_auto(GStrv) pairs = g_strsplit(query + 1, "&", -1);

			g_assert_true(api);
			for (j = 0; pairs[j] != NULL; j++) {
				if (!g_str_has_prefix(pairs[j], "q=") && !g_str_has_prefix(pairs[j], "year=") &&
				    !g_str_has_prefix(pairs[j], "scheme=") && !g_str_has_prefix(pairs[j], "value="))
					g_error("an unexpected parameter was sent: %s", pairs[j]);
			}
		}
		for (j = 1; lines[j] != NULL && lines[j][0] != '\0'; j++) {
			const gchar *colon = strchr(lines[j], ':');
			g_autofree gchar *name = g_strndup(lines[j], (gsize)(colon - lines[j]));
			const gchar *value = colon + 2;

			if (g_ascii_strcasecmp(name, "Host") == 0 || g_ascii_strcasecmp(name, "Accept") == 0 ||
			    g_ascii_strcasecmp(name, "Connection") == 0)
				continue;
			if (g_ascii_strcasecmp(name, "User-Agent") == 0) {
				g_assert_cmpstr(value, ==, "Axis/" AXIS_VERSION);
				agent = TRUE;
				continue;
			}
			if (g_ascii_strcasecmp(name, AXIS_TEST_META_KEY_HEADER) == 0) {
				/* The key goes to the API's origin only. */
				g_assert_true(api);
				g_assert_cmpstr(value, ==, KEY);
				key = TRUE;
				continue;
			}
			g_error("an unexpected header was sent: %s", lines[j]);
		}
		g_assert_true(agent);
		g_assert_true(key == api);
		/* Nothing about the user. */
		if (user != NULL && strlen(user) >= 3)
			g_assert_null(strstr(g_ptr_array_index(requests, i), user));
	}
}

/* Spec: What is sent */
static void
test_only_the_lookup_leaves(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	AxisMetaTitleAnswer title;
	AxisMetaSeasonAnswer season;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *transcript = NULL;

	g_assert_true(axis_meta_search(fx->meta, "fixture", AXIS_META_KIND_FILM, "the mazework", 1999, WAIT, &search,
		&error));
	axis_meta_search_answer_clear(&search);
	g_assert_true(axis_meta_find(fx->meta, "fixture", AXIS_META_KIND_FILM, "imdb", "tt0600003", WAIT, &search,
		&error));
	axis_meta_search_answer_clear(&search);
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &title);
	axis_meta_title_answer_clear(&title);
	lookup_ok(fx, AXIS_META_KIND_SERIES, "201", WAIT, &title);
	g_assert_true(axis_meta_get_season(fx->meta, title.title->id, 1, WAIT, &season, &error));
	axis_meta_season_answer_clear(&season);
	axis_meta_title_answer_clear(&title);
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &hash, &error));
	g_assert_no_error(error);

	assert_requests_plain(fx->fa->api, TRUE);
	assert_requests_plain(fx->fa->images, FALSE);
	transcript = axis_test_plex_transcript(fx->fa->api);
	g_assert_nonnull(strstr(transcript, "GET /search/film?q=the%20mazework&year=1999\n"));
	g_assert_nonnull(strstr(transcript, "GET /find/film?scheme=imdb&value=tt0600003\n"));
	g_assert_null(strstr(transcript, fx->data_path));
}

/* Spec: Key stays put */
static void
test_key_stays_put(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *images = axis_test_plex_base(fx->fa->images);
	g_autofree gchar *elsewhere = g_strconcat(images, "/title/film/101", NULL);
	g_autofree gchar *echo = g_strdup_printf("{\"error\": \"bad key %s\"}", KEY);
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *second_hop = NULL;
	AxisMetaTitleAnswer answer;
	AxisMetaSearchAnswer search;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;

	/* (Two failures in a row start a back-off; each failure here is
	 * followed by a request that succeeds.) */

	/* A redirect to another origin is refused, and nothing is sent there. */
	axis_test_plex_route(fx->fa->api, "/title/film/101", -1, AXIS_TEST_PLEX_REDIRECT, 302, elsewhere, 0);
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "different address"));
	assert_key_absent(answer.problem);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->images), ==, 0);

	/* A redirect within the origin is followed, the key with it. */
	axis_test_plex_route(fx->fa->api, "/title/film/102", -1, AXIS_TEST_PLEX_REDIRECT, 302, "/moved/102", 0);
	axis_test_plex_route_file(fx->fa->api, "/moved/102", -1, "tests/fixtures/meta/title-film-102.json");
	lookup_ok(fx, AXIS_META_KIND_FILM, "102", WAIT, &answer);
	g_assert_false(answer.saved);
	axis_meta_title_answer_clear(&answer);
	second_hop = axis_test_plex_last_header(fx->fa->api, "/moved/102", AXIS_TEST_META_KEY_HEADER);
	g_assert_cmpstr(second_hop, ==, KEY);

	/* An error that echoes the key. */
	route_text(fx->fa->api, "/search/film", 500, echo);
	search_ok(fx, "fixture", AXIS_META_KIND_FILM, "mazework", WAIT, &search);
	g_assert_true(search.saved);
	assert_key_absent(search.problem);
	axis_meta_search_answer_clear(&search);

	/* Proxy variables are not followed. */
	g_setenv("http_proxy", "http://127.0.0.1:1", TRUE);
	g_setenv("HTTP_PROXY", "http://127.0.0.1:1", TRUE);
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p102.png", AXIS_META_ART_POSTER_SMALL, &hash, &error));
	g_assert_no_error(error);
	g_unsetenv("http_proxy");
	g_unsetenv("HTTP_PROXY");

	/* A refused key. */
	axis_test_plex_route(fx->fa->api, "/search/series", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	search_ok(fx, "fixture", AXIS_META_KIND_SERIES, "street", WAIT, &search);
	g_assert_nonnull(strstr(search.problem, "key was refused"));
	assert_key_absent(search.problem);
	axis_meta_search_answer_clear(&search);
	g_clear_pointer(&hash, g_free);
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &hash, &error));

	/* A too-large answer. */
	axis_test_plex_route(fx->fa->api, "/find/film", -1, AXIS_TEST_PLEX_OVERSIZE, 200, NULL, 3 * 1024 * 1024);
	g_assert_true(axis_meta_find(fx->meta, "fixture", AXIS_META_KIND_FILM, "imdb", "tt0600003", WAIT, &search,
		&error));
	g_assert_nonnull(strstr(search.problem, "larger than the limit"));
	axis_meta_search_answer_clear(&search);

	/* The image host never saw the key; no log line holds it. */
	transcript = axis_test_plex_transcript(fx->fa->images);
	assert_key_absent(transcript);
	g_assert_null(strstr(transcript, AXIS_TEST_META_KEY_HEADER));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" KEY "*"), ==, 0);
	/* Nor does the settings table, beyond the secret's own row. */
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search s WHERE to_jsonb(s)::text LIKE '%" KEY "%'"),
		==, 0);
}

static gboolean
job_finished(gpointer data)
{
	Fixture *fx = data;
	g_autofree gchar *state = axis_test_db_query(fx->db,
		"SELECT state FROM job WHERE kind = '" AXIS_META_REFRESH_KIND "' ORDER BY id DESC LIMIT 1");

	return g_strcmp0(state, "succeeded") == 0 || g_strcmp0(state, "failed") == 0;
}

/* Spec: Key stays put (the job's row), on the real job queue. */
static void
test_key_not_in_job_rows(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(AxisWebMeta) web = axis_web_meta_new(fx->meta);
	g_autofree gchar *echo = g_strdup_printf("{\"error\": \"bad key %s\"}", KEY);
	const AxisModuleInfo *module = axis_web_meta_get_module(web);
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	AxisJobQueue *queue;
	gint64 job = 0;
	guint i;

	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	axis_test_db_exec(fx->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '100 days'");
	route_text(fx->fa->api, "/title/film/101", 500, echo);

	for (i = 0; module->job_handlers[i].kind != NULL; i++) {
		g_assert_cmpstr(module->job_handlers[i].queue, ==, AXIS_META_QUEUE);
		g_assert_true(axis_job_registry_add(registry, &module->job_handlers[i], web, &error));
	}
	queue = axis_job_queue_new(fx->pool, registry);
	axis_job_queue_set_poll_interval(queue, 50);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	g_assert_true(axis_meta_request_refresh(fx->meta, &job, NULL, &error));
	g_assert_cmpint(job, >, 0);
	axis_job_queue_wake(queue);
	g_assert_true(axis_test_pump_until(job_finished, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_job_queue_stop(queue, 5000));
	g_object_unref(queue);

	{
		g_autofree gchar *state = axis_test_db_query(fx->db, "SELECT state || ' ' || coalesce(last_error, '')"
			" FROM job WHERE kind = '" AXIS_META_REFRESH_KIND "'");

		g_assert_cmpstr(state, ==, "succeeded ");
	}
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/film/101"), ==, 2);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM job WHERE to_jsonb(job)::text LIKE '%" KEY "%'"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM job_schedule WHERE to_jsonb(job_schedule)::text LIKE '%"
		KEY "%'"), ==, 0);
}

/* ── Artwork ────────────────────────────────────────────────────── */

static gchar *
file_in_artwork(
	Fixture     *fx,
	const gchar *name
){
	return g_build_filename(fx->art_path, name, NULL);
}

static guint
artwork_files(Fixture *fx)
{
	GDir *dir = g_dir_open(fx->art_path, 0, NULL);
	guint n = 0;

	g_assert_nonnull(dir);
	while (g_dir_read_name(dir) != NULL)
		n++;
	g_dir_close(dir);
	return n;
}

static void
assert_kept(
	Fixture     *fx,
	const gchar *ref,
	GBytes      *bytes,
	const gchar *type
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *expected = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
	g_autofree gchar *path = NULL;
	g_autofree gchar *content = NULL;
	g_autofree gchar *stored_type = NULL;
	g_autoptr(GBytes) read = NULL;
	gsize length = 0;

	if (!axis_meta_artwork_fetch(fx->meta, "fixture", ref, AXIS_META_ART_POSTER_SMALL, &hash, &error))
		g_error("%s was not kept: %s", ref, error->message);
	g_assert_cmpstr(hash, ==, expected);
	path = file_in_artwork(fx, hash);
	g_assert_true(g_file_get_contents(path, &content, &length, NULL));
	g_assert_cmpuint(length, ==, g_bytes_get_size(bytes));
	g_assert_cmpmem(content, length, g_bytes_get_data(bytes, NULL), length);
	g_assert_true(axis_meta_artwork_read(fx->meta, hash, &read, &stored_type, &error));
	g_assert_cmpstr(stored_type, ==, type);
	g_assert_true(g_bytes_equal(read, bytes));
}

static void
assert_refused(
	Fixture     *fx,
	const gchar *ref
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *sql = g_strdup_printf("SELECT problem FROM meta_artwork WHERE source_ref = '%s'", ref);

	g_assert_false(axis_meta_artwork_fetch(fx->meta, "fixture", ref, AXIS_META_ART_POSTER_SMALL, &hash, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_REFUSED);
	g_assert_null(hash);
	problem = axis_test_db_query(fx->db, sql);
	g_assert_nonnull(problem);
}

/* Spec: Not an image, and the types that are kept */
static void
test_artwork_checks(Fixture *fx, gconstpointer data)
{
	static const gchar html[] = "<!DOCTYPE html><html><body>not an image</body></html>";
	g_autoptr(GBytes) png = axis_test_meta_png();
	g_autoptr(GBytes) jpeg = axis_test_meta_jpeg(500, 750, 3000);
	g_autoptr(GBytes) webp = axis_test_meta_webp(342, 513, 900);
	g_autoptr(GBytes) page = g_bytes_new_static(html, sizeof(html) - 1);
	g_autoptr(GBytes) huge = axis_test_meta_jpeg(500, 750, AXIS_META_ART_MAX_BYTES + 1);
	g_autoptr(GBytes) wide = axis_test_meta_jpeg(20000, 100, 3000);
	g_autoptr(GBytes) many = axis_test_meta_jpeg(9000, 9000, 3000);
	g_autoptr(GError) error = NULL;
	guint kept;

	axis_test_plex_route_full(fx->fa->images, "/img/w185/a.png", 200, "image/png", png, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/b.jpg", 200, "image/jpeg", jpeg, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/c.webp", 200, "image/webp", webp, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/d.png", 200, NULL, png, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/page.html", 200, "text/html", page, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/page.jpg", 200, "image/jpeg", page, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/huge.jpg", 200, "image/jpeg", huge, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/wide.jpg", 200, "image/jpeg", wide, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/many.jpg", 200, "image/jpeg", many, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/lies.jpg", 200, "image/jpeg", png, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/lies2.png", 200, "image/png", jpeg, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/svg.png", 200, "image/svg+xml", png, NULL);

	assert_kept(fx, "/a.png", png, "image/png");
	assert_kept(fx, "/b.jpg", jpeg, "image/jpeg");
	assert_kept(fx, "/c.webp", webp, "image/webp");
	/* Sent without a type, it is what its bytes show. */
	assert_kept(fx, "/d.png", png, "image/png");
	kept = artwork_files(fx);
	g_assert_cmpuint(kept, ==, 3);

	assert_refused(fx, "/page.html");
	assert_refused(fx, "/page.jpg");
	assert_refused(fx, "/huge.jpg");
	assert_refused(fx, "/wide.jpg");
	assert_refused(fx, "/many.jpg");
	assert_refused(fx, "/lies.jpg");
	assert_refused(fx, "/lies2.png");
	assert_refused(fx, "/svg.png");
	assert_refused(fx, "/not-there.png");
	g_assert_cmpuint(artwork_files(fx), ==, kept);

	/* A refusal is believed for a while: the image is not asked for again. */
	axis_test_plex_reset_requests(fx->fa->images);
	g_assert_null(axis_meta_artwork_find(fx->meta, "fixture", "/page.html", AXIS_META_ART_POSTER_SMALL, TRUE));
	axis_test_pump_for(200);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->images), ==, 0);
}

/* Read back by its hash and nothing else. */
static void
test_artwork_read_by_hash_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) read = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *upper = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *outside = g_build_filename(fx->dir, "outside.png", NULL);
	g_autofree gchar *found = NULL;

	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &hash, &error));
	g_assert_no_error(error);
	/* A request for it fetches nothing new. */
	found = axis_meta_artwork_find(fx->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, TRUE);
	g_assert_cmpstr(found, ==, hash);

	upper = g_ascii_strup(hash, -1);
	g_assert_false(axis_meta_artwork_read(fx->meta, upper, &read, &type, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_meta_artwork_read(fx->meta, "../../etc/passwd", &read, &type, &error));
	g_clear_error(&error);
	g_assert_false(axis_meta_artwork_read(fx->meta, "0000000000000000000000000000000000000000000000000000000000000000",
		&read, &type, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* The file replaced by a link to somewhere else is not followed. */
	path = file_in_artwork(fx, hash);
	axis_test_storage_write(outside, "outside");
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(symlink(outside, path), ==, 0);
	g_assert_false(axis_meta_artwork_read(fx->meta, hash, &read, &type, &error));
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_NOT_FOUND);
}

static void
set_mtime(
	const gchar *path,
	gint64       seconds_ago
){
	struct timespec times[2];

	times[0].tv_sec = (time_t)(g_get_real_time() / G_USEC_PER_SEC - seconds_ago);
	times[0].tv_nsec = 0;
	times[1] = times[0];
	g_assert_cmpint(utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW), ==, 0);
}

/* Spec: Cache bounded, and what no title shows is removed */
static void
test_artwork_cleanup_and_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(GBytes) one = axis_test_meta_jpeg(500, 750, 600 * 1024);
	g_autoptr(GBytes) two = axis_test_meta_jpeg(501, 750, 600 * 1024);
	g_autoptr(GBytes) three = axis_test_meta_jpeg(1280, 720, 600 * 1024);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *h1 = NULL;
	g_autofree gchar *h2 = NULL;
	g_autofree gchar *h3 = NULL;
	g_autofree gchar *stray_old = NULL;
	g_autofree gchar *stray_new = NULL;
	g_autofree gchar *temporary = NULL;
	g_autofree gchar *notes = NULL;
	g_autofree gchar *gone = NULL;
	AxisMetaTitleAnswer answer;
	AxisMetaCleanupReport report;

	axis_test_plex_route_full(fx->fa->images, "/img/w185/p101.png", 200, "image/jpeg", one, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w185/p102.png", 200, "image/jpeg", two, NULL);
	axis_test_plex_route_full(fx->fa->images, "/img/w1280/b101.png", 200, "image/jpeg", three, NULL);
	lookup_ok(fx, AXIS_META_KIND_FILM, "101", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	lookup_ok(fx, AXIS_META_KIND_FILM, "102", WAIT, &answer);
	axis_meta_title_answer_clear(&answer);
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &h1, &error));
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p102.png", AXIS_META_ART_POSTER_SMALL, &h2, &error));
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/b101.png", AXIS_META_ART_BACKDROP, &h3, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(artwork_files(fx), ==, 3);

	/* Nothing to do yet. */
	g_assert_true(axis_meta_run_cleanup(fx->meta, NULL, &report, &error));
	g_assert_cmpuint(report.unreferenced + report.over_cap + report.missing + report.strays_removed, ==, 0);

	/* A title gone: its poster goes. */
	axis_test_db_exec(fx->db, "DELETE FROM meta_title WHERE provider_id = '102'");
	g_assert_true(axis_meta_run_cleanup(fx->meta, NULL, &report, &error));
	g_assert_cmpuint(report.unreferenced, ==, 1);
	g_assert_cmpuint(report.files_removed, ==, 1);
	gone = file_in_artwork(fx, h2);
	g_assert_false(g_file_test(gone, G_FILE_TEST_EXISTS));

	/* Over the cap: the least recently used goes first. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_SETTING_ARTWORK_CAP, "1MiB", &error));
	axis_test_db_exec(fx->db, "UPDATE meta_artwork SET last_used_at = now() - interval '2 days' WHERE source_ref = '/p101.png'");
	g_assert_true(axis_meta_run_cleanup(fx->meta, NULL, &report, &error));
	g_assert_cmpuint(report.over_cap, ==, 1);
	g_clear_pointer(&gone, g_free);
	gone = file_in_artwork(fx, h1);
	g_assert_false(g_file_test(gone, G_FILE_TEST_EXISTS));
	g_clear_pointer(&gone, g_free);
	gone = file_in_artwork(fx, h3);
	g_assert_true(g_file_test(gone, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(report.bytes_kept, ==, g_bytes_get_size(three));

	/* A file gone from the disk: its row goes. */
	g_assert_cmpint(g_unlink(gone), ==, 0);
	g_assert_true(axis_meta_run_cleanup(fx->meta, NULL, &report, &error));
	g_assert_cmpuint(report.missing, ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_artwork WHERE sha256 IS NOT NULL"), ==, 0);

	/* Leftovers nothing knows: old ones go, a new one and a file not of
	 * the artwork's form stay. */
	stray_old = file_in_artwork(fx, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	stray_new = file_in_artwork(fx, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	temporary = file_in_artwork(fx, ".tmp-0123456789abcdef");
	notes = file_in_artwork(fx, "notes.txt");
	axis_test_storage_write(stray_old, "x");
	axis_test_storage_write(stray_new, "x");
	axis_test_storage_write(temporary, "x");
	axis_test_storage_write(notes, "x");
	set_mtime(stray_old, 7200);
	set_mtime(temporary, 7200);
	set_mtime(notes, 7200);
	g_assert_true(axis_meta_run_cleanup(fx->meta, NULL, &report, &error));
	g_assert_cmpuint(report.strays_removed, ==, 2);
	g_assert_false(g_file_test(stray_old, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(temporary, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(stray_new, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(notes, G_FILE_TEST_EXISTS));
}

/* The settings of the module: each provider's key (a secret) and rate. */
static void
test_settings_table(Fixture *fx, gconstpointer data)
{
	const AxisSettingInfo *table = axis_meta_get_setting_table(fx->meta);
	const AxisSettingInfo *key = axis_settings_lookup(fx->settings, "metadata.fixture_key", NULL);
	const AxisSettingInfo *rate = axis_settings_lookup(fx->settings, "metadata.fixtureb_rate", NULL);
	g_autofree gchar *shown = NULL;

	g_assert_nonnull(table);
	g_assert_nonnull(key);
	g_assert_true((key->flags & AXIS_SETTING_SECRET) != 0);
	g_assert_nonnull(rate);
	g_assert_cmpint(rate->type, ==, AXIS_SETTING_DECIMAL);
	shown = axis_settings_format(fx->settings, "metadata.fixture_key");
	g_assert_null(shown);
	g_assert_true(axis_settings_is_set(fx->settings, "metadata.fixture_key"));
	/* The same providers give the same table. */
	g_assert_true(axis_meta_get_setting_table(fx->meta) == table);
	/* A key that could not go in a header is refused. */
	g_assert_false(axis_settings_check(fx->settings, "metadata.fixture_key", "two words", NULL));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/meta/store/one-record", Fixture, NULL, test_one_record);
	AXIS_TEST_ADD_DB_CASE("/meta/store/fetched-again", Fixture, NULL, test_fetched_again_in_place);
	AXIS_TEST_ADD_DB_CASE("/meta/store/search-keeps-full-year", Fixture, NULL, test_search_keeps_full_year);
	AXIS_TEST_ADD_DB_CASE("/meta/store/regional-searches-dropped", Fixture, NULL, test_regional_searches_dropped);
	AXIS_TEST_ADD_DB_CASE("/meta/store/fill-every-name-key", Fixture, NULL, test_fill_every_name_key);
	AXIS_TEST_ADD_DB_CASE("/meta/store/merge", Fixture, NULL, test_two_records_merge);
	AXIS_TEST_ADD_DB_CASE("/meta/store/malformed", Fixture, NULL, test_malformed_answer_changes_nothing);
	AXIS_TEST_ADD_DB_CASE("/meta/down/title", Fixture, NULL, test_provider_down_title_with_age);
	AXIS_TEST_ADD_DB_CASE("/meta/down/search", Fixture, NULL, test_provider_down_search_saved);
	AXIS_TEST_ADD_DB_CASE("/meta/asked-once", Fixture, NULL, test_asked_once);
	AXIS_TEST_ADD_DB_CASE("/meta/negative", Fixture, NULL, test_negative_answer_kept);
	AXIS_TEST_ADD_DB_CASE("/meta/seasons", Fixture, NULL, test_seasons);
	AXIS_TEST_ADD_DB_CASE("/meta/episode-ids", Fixture, NULL, test_episode_ids);
	AXIS_TEST_ADD_DB_CASE("/meta/refresh/referenced-only", Fixture, NULL, test_refresh_referenced_only);
	AXIS_TEST_ADD_DB_CASE("/meta/refresh/held-providers", Fixture, NULL, test_refresh_skips_held_providers);
	AXIS_TEST_ADD_DB_CASE("/meta/limit/burst", Fixture, &slow_budget, test_rate_limit_under_a_burst);
	AXIS_TEST_ADD_DB_CASE("/meta/limit/told-to-wait", Fixture, NULL, test_told_to_wait_pauses_one_provider);
	AXIS_TEST_ADD_DB_CASE("/meta/sent/only-the-lookup", Fixture, NULL, test_only_the_lookup_leaves);
	AXIS_TEST_ADD_DB_CASE("/meta/sent/key-stays-put", Fixture, NULL, test_key_stays_put);
	AXIS_TEST_ADD_DB_CASE("/meta/sent/key-not-in-jobs", Fixture, NULL, test_key_not_in_job_rows);
	AXIS_TEST_ADD_DB_CASE("/meta/artwork/checks", Fixture, NULL, test_artwork_checks);
	AXIS_TEST_ADD_DB_CASE("/meta/artwork/read-by-hash", Fixture, NULL, test_artwork_read_by_hash_only);
	AXIS_TEST_ADD_DB_CASE("/meta/artwork/cleanup-and-cap", Fixture, NULL, test_artwork_cleanup_and_cap);
	AXIS_TEST_ADD_DB_CASE("/meta/settings", Fixture, NULL, test_settings_table);
	return g_test_run();
}
