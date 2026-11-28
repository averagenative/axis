/*
 * test-discover.c - the Discover layer against a real database and a fake
 * TMDb: the refresh job, what it asks and when, recommendations from what
 * you have, the reads with their filters and pages, and Not interested
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (add-discover, discover): A list is fetched once; A stale
 * list is fetched again; A failure keeps what was there; A provider that
 * stops answering; Ranked by agreement; What you have is not recommended;
 * Turned off; By genre and rating; Hiding what you have; Paging; Hiding a
 * title; Showing it again; No key.  (metadata) A list stored; Coming soon in
 * the region.  Task 2.4.
 *
 * "What you have" is a table of the test's own, test_have, registered as a
 * reference of the metadata layer the way the modules register theirs.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-discover.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "discover/axis-discover.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"

typedef struct
{
	AxisTestDb           *db;
	AxisDbPool           *pool;
	gchar                *dir;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisMeta             *meta;
	AxisTestDiscoverTmdb *tmdb;
	AxisDiscover         *discover;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *data_path = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->dir = axis_test_storage_dir_new();
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	axis_test_db_exec(fx->db, "CREATE TABLE test_have (title_id bigint)");
	data_path = g_build_filename(fx->dir, "data", NULL);
	fx->data = axis_data_dir_open(data_path, &error);
	g_assert_no_error(error);
	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->tmdb = axis_test_discover_tmdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->tmdb->info, &error));
	g_assert_true(axis_meta_add_reference(fx->meta, "test", "SELECT title_id FROM test_have"));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_DISCOVER_MODULE_ID, axis_discover_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, AXIS_TEST_DISCOVER_TOKEN, &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
	fx->discover = axis_discover_new(fx->pool, fx->settings, fx->meta);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_discover_unref(fx->discover);
	axis_meta_stop(fx->meta);
	axis_meta_unref(fx->meta);
	axis_settings_unref(fx->settings);
	axis_test_discover_tmdb_free(fx->tmdb);
	axis_data_dir_unref(fx->data);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	axis_test_log_reset();
	while (g_main_context_iteration(NULL, FALSE))
		;
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisDiscoverReport
run(Fixture *fx)
{
	AxisDiscoverReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_discover_run_refresh(fx->discover, NULL, FALSE, &report, &error));
	g_assert_no_error(error);
	return report;
}

/* The first column of the first row of @sql is @expected (NULL: none). */
static void
query_is(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	g_assert_cmpstr(value, ==, expected);
}

/* The store's id of TMDb's title @tmdb of @kind. */
static gint64
title_id(
	Fixture     *fx,
	const gchar *kind,
	const gchar *tmdb
){
	g_autofree gchar *text = axis_test_db_queryf(fx->db, "SELECT title_id FROM meta_external_id WHERE kind = '%s'"
		" AND scheme = 'tmdb' AND value = '%s'", kind, tmdb);

	g_assert_nonnull(text);
	return g_ascii_strtoll(text, NULL, 10);
}

static void
have(
	Fixture     *fx,
	const gchar *kind,
	const gchar *tmdb
){
	g_autofree gchar *sql = g_strdup_printf("INSERT INTO test_have VALUES (%" G_GINT64_FORMAT ")",
		title_id(fx, kind, tmdb));

	axis_test_db_exec(fx->db, sql);
}

static void
query_init(
	AxisDiscoverQuery *query,
	AxisMetaKind       kind,
	AxisMetaList       list
){
	memset(query, 0, sizeof(*query));
	query->kind = kind;
	query->list = list;
	query->limit = 20;
}

static void
read_page(
	Fixture                 *fx,
	const AxisDiscoverQuery *query,
	AxisDiscoverPage        *page
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_discover_read(fx->discover, query, page, &error));
	g_assert_no_error(error);
}

/* The TMDb ids of a page's items, "9001 9002 ...". */
static gchar *
ids_of(const AxisDiscoverPage *page)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < page->items->len; i++) {
		const AxisDiscoverItem *item = g_ptr_array_index(page->items, i);

		g_string_append_printf(out, "%s%s", i > 0 ? " " : "", item->title->provider_id);
	}
	return g_string_free(out, FALSE);
}

static gchar *
ids_with(
	Fixture           *fx,
	AxisDiscoverQuery *query
){
	AxisDiscoverPage page;
	gchar *ids;

	read_page(fx, query, &page);
	ids = ids_of(&page);
	axis_discover_page_clear(&page);
	return ids;
}

/* ── The job ────────────────────────────────────────────────────── */

static void
test_refresh_lists(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report = run(fx);
	g_autofree gchar *transcript = axis_test_plex_transcript(fx->tmdb->api);
	g_autofree gchar *auth = axis_test_plex_last_header(fx->tmdb->api, "/3/movie/popular", "Authorization");
	g_autofree gchar *expected_auth = g_strconcat("Bearer ", AXIS_TEST_DISCOVER_TOKEN, NULL);
	g_autofree gchar *entries = NULL;
	g_autofree gchar *brief = NULL;
	g_autofree gchar *details = NULL;

	/* Five movie lists and four of series (TMDb has no upcoming series);
	 * popular has two pages, the rest one; the genres of both kinds. */
	g_assert_cmpuint(report.lists, ==, 9);
	g_assert_cmpuint(report.genres, ==, 2);
	g_assert_cmpuint(report.pages, ==, 10);
	g_assert_cmpuint(report.failed, ==, 0);
	g_assert_cmpuint(report.skipped, ==, 0);
	g_assert_cmpuint(report.recommended, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 12);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/popular"), ==, 2);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/tv/upcoming"), ==, 0);
	/* The region only where it decides what the list is. */
	g_assert_nonnull(strstr(transcript, "/3/movie/upcoming?language=en-US&page=1&region=US"));
	g_assert_nonnull(strstr(transcript, "/3/movie/popular?language=en-US&page=2&region=US"));
	g_assert_nonnull(strstr(transcript, "/3/trending/movie/week?language=en-US&page=1"));
	g_assert_null(strstr(transcript, "/3/trending/movie/week?language=en-US&page=1&region"));
	g_assert_nonnull(strstr(transcript, "/3/tv/on_the_air?language=en-US&page=1"));
	g_assert_null(strstr(transcript, "/3/tv/on_the_air?language=en-US&page=1&region"));
	g_assert_nonnull(strstr(transcript, "/3/genre/tv/list?language=en-US"));
	g_assert_cmpstr(auth, ==, expected_auth);
	g_assert_false(axis_test_plex_saw_non_get(fx->tmdb->api));
	/* (The image host is asked for the lists' posters meanwhile, in the
	 * background: /discover/refresh/posters.) */

	/* Page 2 repeats a title of page 1: it is kept once, at its first place. */
	entries = axis_test_db_query(fx->db, "SELECT string_agg(x.value, ' ' ORDER BY e.position) FROM discover_entry e"
		" JOIN discover_list l ON l.id = e.list_id JOIN meta_external_id x ON x.title_id = e.title_id"
		" AND x.scheme = 'tmdb' WHERE l.kind = 'film' AND l.list = 'popular'");
	g_assert_cmpstr(entries, ==, "9001 9002 9003 9005 9004 9008 9009");
	query_is(fx, "SELECT pages FROM discover_list WHERE kind = 'film' AND list = 'popular'", "2");
	/* Titles are stored as brief records, nothing more. */
	brief = axis_test_db_query(fx->db, "SELECT count(*) FROM meta_title WHERE detail_fetched_at IS NULL");
	details = axis_test_db_query(fx->db, "SELECT count(*) FROM meta_title");
	g_assert_cmpstr(brief, ==, details);
	query_is(fx, "SELECT rating FROM discover_entry e JOIN meta_external_id x ON x.title_id = e.title_id"
		" WHERE x.value = '9005' LIMIT 1", NULL);
	query_is(fx, "SELECT count(*) FROM discover_genre WHERE kind = 'film'", "13");
}

/* Waits until the image host was asked @n times, for a while. */
static void
wait_for_images(Fixture *fx, guint n)
{
	gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (axis_test_plex_n_requests(fx->tmdb->images) < n && g_get_monotonic_time() < until)
		g_usleep(20000);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tmdb->images), ==, n);
}

/* A refresh asks for the posters of what the lists hold, by itself, so that
 * they are stored before anyone opens a page (the request portal shows only
 * stored pictures); a stored poster is not asked for again, and a title
 * marked not interested is left out. */
static void
test_refresh_posters(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *wanted = NULL;
	g_autofree gchar *stored = NULL;
	gint64 until;
	guint n;

	/* The first refresh fills the lists and asks for their posters; one
	 * title is then marked not interested, and its poster forgotten. */
	run(fx);
	wanted = axis_test_db_query(fx->db, "SELECT count(DISTINCT (t.provider, t.poster_ref)) FROM discover_entry e"
		" JOIN meta_title t ON t.id = e.title_id WHERE t.poster_ref IS NOT NULL");
	n = (guint)g_ascii_strtoull(wanted, NULL, 10);
	g_assert_cmpuint(n, >=, 2);
	wait_for_images(fx, n);
	until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	do {
		g_free(stored);
		stored = axis_test_db_query(fx->db, "SELECT count(*) FROM meta_artwork WHERE sha256 IS NOT NULL");
		if (g_strcmp0(stored, wanted) != 0)
			g_usleep(20000);
	} while (g_strcmp0(stored, wanted) != 0 && g_get_monotonic_time() < until);
	g_assert_cmpstr(stored, ==, wanted);

	/* Stored: the next refresh asks the image host for nothing. */
	g_assert_true(axis_discover_run_refresh(fx->discover, NULL, TRUE, NULL, NULL));
	g_usleep(300000);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tmdb->images), ==, n);
}

static void
test_refresh_fresh_not_asked(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;

	g_autoptr(GError) error = NULL;

	run(fx);
	axis_test_plex_reset_requests(fx->tmdb->api);
	report = run(fx);
	g_assert_cmpuint(report.pages, ==, 0);
	g_assert_cmpuint(report.lists, ==, 0);
	g_assert_cmpuint(report.genres, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);

	/* A person asked: every list again, fresh or not (the genres keep their
	 * week). */
	g_assert_true(axis_discover_run_refresh(fx->discover, NULL, TRUE, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.lists, ==, 9);
	g_assert_cmpuint(report.genres, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 10);
}

static void
test_refresh_stale_asked_again(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;
	g_autoptr(GError) error = NULL;

	run(fx);
	axis_test_plex_reset_requests(fx->tmdb->api);
	axis_test_db_exec(fx->db, "UPDATE discover_list SET fetched_at = now() - interval '13 hours'"
		" WHERE kind = 'film' AND list = 'popular'");
	report = run(fx);
	g_assert_cmpuint(report.lists, ==, 1);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 2);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/popular"), ==, 2);

	/* One page kept, by the setting: one asked. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_DISCOVER_SETTING_PAGES, "1", &error));
	g_assert_no_error(error);
	axis_test_plex_reset_requests(fx->tmdb->api);
	axis_test_db_exec(fx->db, "UPDATE discover_list SET fetched_at = now() - interval '13 hours'"
		" WHERE kind = 'film' AND list = 'popular'");
	run(fx);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/popular"), ==, 1);
	query_is(fx, "SELECT count(*) FROM discover_entry e JOIN discover_list l ON l.id = e.list_id"
		" WHERE l.kind = 'film' AND l.list = 'popular'", "5");

	/* Longer before a list is old. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_DISCOVER_SETTING_REFRESH_AFTER, "2d", &error));
	axis_test_plex_reset_requests(fx->tmdb->api);
	axis_test_db_exec(fx->db, "UPDATE discover_list SET fetched_at = now() - interval '13 hours'"
		" WHERE kind = 'film' AND list = 'popular'");
	run(fx);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);
}

static void
test_refresh_failure_keeps_snapshot(Fixture *fx, gconstpointer data)
{
	AxisDiscoverQuery query;
	AxisDiscoverReport report;
	AxisDiscoverPage page;

	run(fx);
	axis_test_db_exec(fx->db, "UPDATE discover_list SET fetched_at = now() - interval '13 hours'"
		" WHERE kind = 'film' AND list = 'popular'");
	axis_test_discover_tmdb_set_status(fx->tmdb, "/3/movie/popular", 2, 404);
	report = run(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_cmpuint(report.lists, ==, 0);

	query_init(&query, AXIS_META_KIND_FILM, AXIS_META_LIST_POPULAR);
	read_page(fx, &query, &page);
	/* The snapshot shown is the one before, with when and why. */
	g_assert_true(page.stored);
	g_assert_cmpuint(page.total, ==, 7);
	g_assert_cmpuint(page.size, ==, 7);
	g_assert_nonnull(page.problem);
	g_assert_nonnull(strstr(page.problem, "TMDb"));
	g_assert_cmpint(page.tried_us, >, page.fetched_us);
	axis_discover_page_clear(&page);

	/* Answered again: replaced, and the problem gone. */
	axis_test_discover_tmdb_set_status(fx->tmdb, "/3/movie/popular", 2, 0);
	report = run(fx);
	g_assert_cmpuint(report.lists, ==, 1);
	read_page(fx, &query, &page);
	g_assert_null(page.problem);
	axis_discover_page_clear(&page);
}

static void
test_refresh_rate_limited(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;
	g_autofree gchar *problem = NULL;

	axis_test_discover_tmdb_set_status(fx->tmdb, "/3/movie/popular", 0, 429);
	report = run(fx);
	/* The movie genres, then popular: TMDb asked Axis to wait, so nothing more
	 * is asked in this run, of movies or of series. */
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_cmpuint(report.lists, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 2);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/trending/movie/week"), ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/tv/popular"), ==, 0);
	problem = axis_test_db_query(fx->db, "SELECT problem FROM discover_list WHERE kind = 'film' AND list = 'popular'");
	g_assert_nonnull(problem);
	g_assert_nonnull(strstr(problem, "429"));
	query_is(fx, "SELECT fetched_at IS NULL FROM discover_list WHERE kind = 'film' AND list = 'popular'", "t");
	/* Series were not asked at all (their genres were first and TMDb was
	 * waited for): the attempt is recorded on their first list, so its page
	 * says why and does not queue another refresh by itself. */
	query_is(fx, "SELECT problem LIKE '%asked Axis to wait%' FROM discover_list WHERE kind = 'series'"
		" AND list = 'popular'", "t");
}

static void
test_refresh_unreadable(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;

	axis_test_discover_tmdb_set_body(fx->tmdb, "/3/tv/top_rated", 0, "{\"page\": 1}");
	report = run(fx);
	/* An answer that cannot be read fails that list only. */
	g_assert_cmpuint(report.failed, ==, 1);
	g_assert_cmpuint(report.lists, ==, 8);
	query_is(fx, "SELECT count(*) FROM discover_list l JOIN discover_entry e ON e.list_id = l.id"
		" WHERE l.kind = 'series' AND l.list = 'top_rated'", "0");
	query_is(fx, "SELECT problem IS NOT NULL FROM discover_list WHERE kind = 'series' AND list = 'top_rated'", "t");
}

static void
test_refresh_no_key(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;
	AxisDiscoverQuery query;
	AxisDiscoverPage page;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TMDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	report = run(fx);
	g_assert_cmpuint(report.skipped, ==, 2);
	g_assert_cmpuint(report.pages, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);
	query_is(fx, "SELECT count(*) FROM discover_list", "0");

	query_init(&query, AXIS_META_KIND_FILM, AXIS_META_LIST_POPULAR);
	read_page(fx, &query, &page);
	g_assert_true(page.provider == &fx->tmdb->info);
	g_assert_false(page.ready);
	g_assert_false(page.stored);
	g_assert_false(page.tried_ever);
	g_assert_cmpuint(page.items->len, ==, 0);
	axis_discover_page_clear(&page);
}

/* ── Recommendations ────────────────────────────────────────────── */

static void
test_recommendations(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;
	AxisDiscoverQuery query;
	AxisDiscoverPage page;
	const AxisDiscoverItem *first;
	g_autofree gchar *ids = NULL;
	g_autoptr(GError) error = NULL;

	run(fx);
	have(fx, "film", "9001");
	have(fx, "film", "9002");
	report = run(fx);
	g_assert_cmpuint(report.recommended, ==, 2);
	g_assert_cmpuint(report.lists, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9001/recommendations"), ==, 1);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9002/recommendations"), ==, 1);

	query_init(&query, AXIS_META_KIND_FILM, AXIS_META_LIST_RECOMMENDED);
	read_page(fx, &query, &page);
	/* Recommended by both first; never one you have. */
	ids = ids_of(&page);
	g_assert_cmpstr(ids, ==, "9022 9021 9024 9023");
	g_assert_cmpuint(page.have, ==, 2);
	g_assert_cmpuint(page.seeds, ==, 2);
	g_assert_cmpuint(page.size, ==, 4);
	g_assert_true(page.stored);
	first = g_ptr_array_index(page.items, 0);
	g_assert_cmpuint(first->because_n, ==, 2);
	g_assert_cmpuint(first->because->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(first->because, 0), ==, "Nine Lanterns South");
	g_assert_cmpstr(g_ptr_array_index(first->because, 1), ==, "The Brass Orchard");
	g_assert_cmpint(first->rating, ==, 65);
	axis_discover_page_clear(&page);

	/* Kept a week: nothing asked again. */
	axis_test_plex_reset_requests(fx->tmdb->api);
	report = run(fx);
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);

	/* Not interested: left out. */
	g_assert_true(axis_discover_dismiss(fx->discover, title_id(fx, "film", "9021"), "root", &error));
	g_assert_no_error(error);
	g_clear_pointer(&ids, g_free);
	ids = ids_with(fx, &query);
	g_assert_cmpstr(ids, ==, "9022 9024 9023");

	/* A title no longer had: its recommendations go. */
	axis_test_db_exec(fx->db, "DELETE FROM test_have WHERE title_id = (SELECT title_id FROM meta_external_id"
		" WHERE kind = 'film' AND scheme = 'tmdb' AND value = '9002')");
	report = run(fx);
	g_assert_cmpuint(report.dropped, ==, 1);
	read_page(fx, &query, &page);
	g_clear_pointer(&ids, g_free);
	ids = ids_of(&page);
	g_assert_cmpstr(ids, ==, "9022 9002 9023");
	g_assert_cmpuint(((const AxisDiscoverItem *)g_ptr_array_index(page.items, 0))->because_n, ==, 1);
	g_assert_cmpuint(page.have, ==, 1);
	axis_discover_page_clear(&page);

	/* Filters apply to recommendations too. */
	query.genre = 9648;
	g_clear_pointer(&ids, g_free);
	ids = ids_with(fx, &query);
	g_assert_cmpstr(ids, ==, "9022");
	query.genre = 0;
	query.min_rating = 60;
	g_clear_pointer(&ids, g_free);
	ids = ids_with(fx, &query);
	g_assert_cmpstr(ids, ==, "9022 9002");
}

static void
test_recommendations_off(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, AXIS_DISCOVER_SETTING_RECOMMEND, "0", &error));
	g_assert_no_error(error);
	run(fx);
	have(fx, "film", "9001");
	report = run(fx);
	g_assert_cmpuint(report.recommended, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9001/recommendations"), ==, 0);
	g_assert_cmpuint(axis_discover_recommend_per_run(fx->discover), ==, 0);
}

static void
test_recommendations_per_run(Fixture *fx, gconstpointer data)
{
	AxisDiscoverReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, AXIS_DISCOVER_SETTING_RECOMMEND, "1", &error));
	run(fx);
	have(fx, "film", "9001");
	have(fx, "film", "9002");
	/* One a run: the second waits for the next. */
	report = run(fx);
	g_assert_cmpuint(report.recommended, ==, 1);
	report = run(fx);
	g_assert_cmpuint(report.recommended, ==, 1);
	report = run(fx);
	g_assert_cmpuint(report.recommended, ==, 0);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9001/recommendations"), ==, 1);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9002/recommendations"), ==, 1);

	/* A title TMDb has no recommendations of (a 404) is not asked again
	 * every hour: it waits a week like the others. */
	axis_test_db_exec(fx->db, "UPDATE discover_list SET tried_at = now() - interval '8 days'"
		" WHERE list = 'recommended' AND seed_title_id = (SELECT title_id FROM meta_external_id"
		" WHERE kind = 'film' AND scheme = 'tmdb' AND value = '9001')");
	axis_test_discover_tmdb_set_status(fx->tmdb, "/3/movie/9001/recommendations", 0, 404);
	axis_test_plex_reset_requests(fx->tmdb->api);
	report = run(fx);
	g_assert_cmpuint(report.failed, ==, 1);
	report = run(fx);
	report = run(fx);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9001/recommendations"), ==, 1);
}

/* ── Reads ──────────────────────────────────────────────────────── */

static void
test_read_filters(Fixture *fx, gconstpointer data)
{
	AxisDiscoverQuery query;
	AxisDiscoverPage page;
	g_autofree gchar *ids = NULL;
	g_autoptr(GError) error = NULL;

	run(fx);
	query_init(&query, AXIS_META_KIND_FILM, AXIS_META_LIST_POPULAR);
	read_page(fx, &query, &page);
	ids = ids_of(&page);
	g_assert_cmpstr(ids, ==, "9001 9002 9003 9005 9004 9008 9009");
	g_assert_cmpuint(page.total, ==, 7);
	g_assert_true(page.ready);
	g_assert_true(page.tried_ever);
	g_assert_cmpint(((const AxisDiscoverItem *)g_ptr_array_index(page.items, 0))->rating, ==, 74);
	g_assert_cmpint(((const AxisDiscoverItem *)g_ptr_array_index(page.items, 3))->rating, ==, -1);
	axis_discover_page_clear(&page);

#define EXPECT(expected) do { \
		g_clear_pointer(&ids, g_free); \
		ids = ids_with(fx, &query); \
		g_assert_cmpstr(ids, ==, (expected)); \
	} while (0)

	query.genre = 18;
	EXPECT("9001 9005 9004");
	query.genre = 0;
	query.year_from = 2024;
	EXPECT("9001 9003 9005 9009");
	query.year_from = 0;
	query.year_to = 2022;
	EXPECT("9004 9008");
	query.year_from = 2021;
	EXPECT("9004 9008");
	query.year_from = 0;
	query.year_to = 0;
	/* No rating is not "7 and up". */
	query.min_rating = 70;
	EXPECT("9001 9003 9008 9009");
	query.min_rating = 0;

	have(fx, "film", "9003");
	EXPECT("9001 9002 9003 9005 9004 9008 9009");
	query.hide_have = TRUE;
	EXPECT("9001 9002 9005 9004 9008 9009");
	query.hide_have = FALSE;
	g_assert_true(axis_discover_dismiss(fx->discover, title_id(fx, "film", "9002"), "root", &error));
	EXPECT("9001 9003 9005 9004 9008 9009");

	/* Pages of the matching entries, with their count even beyond the end. */
	query.limit = 4;
	EXPECT("9001 9003 9005 9004");
	query.offset = 4;
	EXPECT("9008 9009");
	query.offset = 40;
	read_page(fx, &query, &page);
	g_assert_cmpuint(page.items->len, ==, 0);
	g_assert_cmpuint(page.total, ==, 6);
	g_assert_cmpuint(page.size, ==, 7);
	axis_discover_page_clear(&page);

	/* Series have their own lists. */
	query_init(&query, AXIS_META_KIND_SERIES, AXIS_META_LIST_POPULAR);
	EXPECT("9601 9501 9502");
	query.list = AXIS_META_LIST_NOW;
	EXPECT("9504");
#undef EXPECT
}

static void
test_read_refused(Fixture *fx, gconstpointer data)
{
	AxisDiscoverQuery query;
	AxisDiscoverPage page;
	g_autoptr(GError) error = NULL;

	query_init(&query, AXIS_META_KIND_SERIES, AXIS_META_LIST_UPCOMING);
	g_assert_false(axis_discover_read(fx->discover, &query, &page, &error));
	g_assert_error(error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_INVALID);
	axis_discover_page_clear(&page);
	g_clear_error(&error);
	query_init(&query, AXIS_META_KIND_FILM, AXIS_META_LIST_POPULAR);
	query.limit = 0;
	g_assert_false(axis_discover_read(fx->discover, &query, &page, &error));
	g_assert_error(error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_INVALID);
	axis_discover_page_clear(&page);
	g_clear_error(&error);
	query.limit = 20;
	query.min_rating = 101;
	g_assert_false(axis_discover_read(fx->discover, &query, &page, &error));
	axis_discover_page_clear(&page);
	g_clear_error(&error);
	query.min_rating = 0;
	query.year_from = 12;
	g_assert_false(axis_discover_read(fx->discover, &query, &page, &error));
	axis_discover_page_clear(&page);

	/* Nothing stored yet: an empty page, not an error. */
	g_clear_error(&error);
	query.year_from = 0;
	read_page(fx, &query, &page);
	g_assert_false(page.stored);
	g_assert_false(page.tried_ever);
	g_assert_cmpuint(page.total, ==, 0);
	axis_discover_page_clear(&page);
}

static void
test_genres(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) genres = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_discover_genres(fx->discover, AXIS_META_KIND_SERIES, &genres, &error));
	g_assert_cmpuint(genres->len, ==, 0);
	g_clear_pointer(&genres, g_ptr_array_unref);
	run(fx);
	g_assert_true(axis_discover_genres(fx->discover, AXIS_META_KIND_SERIES, &genres, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(genres->len, ==, 5);
	/* By name. */
	g_assert_cmpstr(((const AxisMetaGenre *)g_ptr_array_index(genres, 0))->name, ==, "Action & Adventure");
	g_assert_cmpstr(((const AxisMetaGenre *)g_ptr_array_index(genres, 4))->name, ==, "Sci-Fi & Fantasy");
}

/* ── Not interested ─────────────────────────────────────────────── */

static void
test_dismissed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) items = NULL;
	g_autoptr(GError) error = NULL;
	const AxisDiscoverDismissed *item;
	gboolean removed = FALSE;
	gint64 orchard;
	guint total = 0;

	run(fx);
	orchard = title_id(fx, "film", "9001");
	g_assert_true(axis_discover_dismiss(fx->discover, orchard, "root", &error));
	/* Twice is once. */
	g_assert_true(axis_discover_dismiss(fx->discover, orchard, "someone-else", &error));
	g_assert_true(axis_discover_list_dismissed(fx->discover, 0, 20, &items, &total, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(total, ==, 1);
	g_assert_cmpuint(items->len, ==, 1);
	item = g_ptr_array_index(items, 0);
	g_assert_cmpstr(item->title->title, ==, "The Brass Orchard");
	g_assert_cmpstr(item->by, ==, "root");
	g_assert_cmpint(item->when_us, >, 0);

	g_assert_true(axis_discover_undismiss(fx->discover, orchard, &removed, &error));
	g_assert_true(removed);
	g_assert_true(axis_discover_undismiss(fx->discover, orchard, &removed, &error));
	g_assert_false(removed);
	g_assert_false(axis_discover_dismiss(fx->discover, 987654321, "root", &error));
	g_assert_error(error, AXIS_DISCOVER_ERROR, AXIS_DISCOVER_ERROR_NOT_FOUND);
}

/* Two records of one title merge: the mark follows the one that is kept. */
static void
test_dismissed_follows_merge(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *kept = NULL;
	g_autofree gchar *sql = NULL;
	gint64 orchard;

	run(fx);
	orchard = title_id(fx, "film", "9001");
	g_assert_true(axis_discover_dismiss(fx->discover, orchard, "root", &error));
	kept = axis_test_db_query(fx->db, "INSERT INTO meta_title (kind, title, provider, provider_id)"
		" VALUES ('film', 'The Brass Orchard', 'tmdb', '9001') RETURNING id");
	/* What the store's merge does: the identifiers move, the other record goes. */
	sql = g_strdup_printf("UPDATE meta_external_id SET title_id = %s WHERE title_id = %" G_GINT64_FORMAT, kept,
		orchard);
	axis_test_db_exec(fx->db, sql);
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("DELETE FROM meta_title WHERE id = %" G_GINT64_FORMAT, orchard);
	axis_test_db_exec(fx->db, sql);
	query_is(fx, "SELECT title_id::text FROM discover_dismissed", kept);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/discover/refresh/lists", Fixture, NULL, test_refresh_lists);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/fresh-not-asked", Fixture, NULL, test_refresh_fresh_not_asked);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/posters", Fixture, NULL, test_refresh_posters);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/stale-asked-again", Fixture, NULL, test_refresh_stale_asked_again);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/failure-keeps-snapshot", Fixture, NULL,
		test_refresh_failure_keeps_snapshot);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/rate-limited", Fixture, NULL, test_refresh_rate_limited);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/unreadable", Fixture, NULL, test_refresh_unreadable);
	AXIS_TEST_ADD_DB_CASE("/discover/refresh/no-key", Fixture, NULL, test_refresh_no_key);
	AXIS_TEST_ADD_DB_CASE("/discover/recommended/ranked", Fixture, NULL, test_recommendations);
	AXIS_TEST_ADD_DB_CASE("/discover/recommended/off", Fixture, NULL, test_recommendations_off);
	AXIS_TEST_ADD_DB_CASE("/discover/recommended/per-run", Fixture, NULL, test_recommendations_per_run);
	AXIS_TEST_ADD_DB_CASE("/discover/read/filters", Fixture, NULL, test_read_filters);
	AXIS_TEST_ADD_DB_CASE("/discover/read/refused", Fixture, NULL, test_read_refused);
	AXIS_TEST_ADD_DB_CASE("/discover/read/genres", Fixture, NULL, test_genres);
	AXIS_TEST_ADD_DB_CASE("/discover/dismissed/marked", Fixture, NULL, test_dismissed);
	AXIS_TEST_ADD_DB_CASE("/discover/dismissed/follows-merge", Fixture, NULL, test_dismissed_follows_merge);
	return g_test_run();
}
