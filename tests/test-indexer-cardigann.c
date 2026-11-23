/*
 * test-indexer-cardigann.c - indexers that run an installed definition:
 * installing, listing and removing definitions, adding an indexer from
 * one, its searches and fetches against stand-in sites on loopback, and
 * that nothing a definition or a site's page says makes Axis ask another
 * address, on a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The definitions and pages are the hand-written fixtures of
 * tests/fixtures/definitions (its README: none is copied, none is a real
 * site).  The stand-ins are tests/axis-test-plex.c servers on loopback; no
 * test reaches a real site.
 *
 * add-indexer-definitions, spec scenarios (indexers): Supported definition,
 * Unsupported feature, Elsewhere.  Tasks 1.5, 1.6, 4.1 (with hand-written
 * fixtures), 4.2, 4.3.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-plex.h"
#include "download/axis-download-model.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-cardigann.h"
#include "indexer/axis-indexer-model.h"

#define FIXTURES "tests/fixtures/definitions/"

typedef struct
{
	AxisTestDb   *db;
	AxisDbPool   *pool;
	AxisSettings *settings;
	AxisIndexers *indexers;
	AxisTestPlex *site;	/* the site's own address */
	AxisTestPlex *other;	/* another address, never to be asked */
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisIndexerTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 16);
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_SITE_RATE, "20", &error));
	g_assert_no_error(error);
	fx->indexers = axis_indexers_new(fx->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_cardigann_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.wait_ms = 10000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	fx->site = axis_test_plex_new();
	fx->other = axis_test_plex_new();
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_indexers_unref(fx->indexers);
	axis_test_plex_free(fx->site);
	axis_test_plex_free(fx->other);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	gchar *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
	return data;
}

static gchar *
replaced(
	const gchar *text,
	const gchar *from,
	const gchar *to
){
	g_auto(GStrv) parts = g_strsplit(text, from, -1);

	return g_strjoinv(to, parts);
}

static gint64
install(
	Fixture     *fx,
	const gchar *text
){
	AxisIndexerDefinitionInstalled installed;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_install_definition(fx->indexers, text, strlen(text), &installed, &error));
	g_assert_no_error(error);
	g_assert_cmpint(installed.id, >, 0);
	return installed.id;
}

/* An indexer that runs @definition at @stand_in's address. */
static gint64
add_indexer(
	Fixture      *fx,
	gint64        definition,
	AxisTestPlex *stand_in,
	const gchar  *path
){
	AxisIndexerSourceSpec spec;
	g_autofree gchar *base = axis_test_plex_base(stand_in);
	g_autofree gchar *address = g_strconcat(base, path != NULL ? path : "", NULL);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_INDEXER_CARDIGANN_NAME;
	spec.address = address;
	spec.enabled = TRUE;
	spec.definition_id = definition;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
route(
	AxisTestPlex *stand_in,
	const gchar  *path,
	const gchar  *content_type,
	const gchar  *body
){
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

	axis_test_plex_route_full(stand_in, path, 200, content_type, bytes, NULL);
}

static void
movie(
	AxisIndexerRequest *request,
	const gchar        *title,
	gint                year
){
	axis_indexer_request_init(request, AXIS_INDEXER_QUERY_FILM);
	request->title = title;
	request->year = year;
}

static const AxisIndexerRelease *
release_named(
	const AxisIndexerSearchResult *result,
	const gchar                   *name
){
	guint i;

	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (g_strcmp0(release->name, name) == 0)
			return release;
	}
	g_error("no release named %s", name);
	return NULL;
}

/* ── Installing ─────────────────────────────────────────────────── */

/* Task 1.6: installing, judging, replacing, listing and removing. */
static void
test_installing(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *login = replaced(html, "\nsearch:\n", "\nlogin:\n  method: form\n  path: /login\nsearch:\n");
	g_autofree gchar *renamed = replaced(html, "name: Example HTML", "name: Example HTML Two");
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerDefinitionInstalled installed;
	const AxisIndexerDefinitionInfo *info;
	gint64 id;

	id = install(fx, html);
	g_assert_true(axis_indexers_list_definitions(fx->indexers, &all, &error));
	g_assert_cmpuint(all->len, ==, 1);
	info = g_ptr_array_index(all, 0);
	g_assert_cmpint(info->id, ==, id);
	g_assert_cmpstr(info->def_id, ==, "example-html");
	g_assert_cmpstr(info->name, ==, "Example HTML");
	g_assert_true(info->usable);
	g_assert_null(info->problem);
	g_assert_true(info->movies);
	g_assert_true(info->tv);
	g_assert_cmpstr(info->links[0], ==, "https://example-html.invalid/");
	g_assert_cmpstr(info->links[1], ==, "https://old.example-html.invalid/");
	g_assert_null(info->links[2]);
	g_assert_cmpuint(strlen(info->sha256), ==, 64);
	g_assert_cmpint(info->size_bytes, ==, (gint64)strlen(html));
	g_clear_pointer(&all, g_ptr_array_unref);

	/* The same file again changes nothing; another of the same id
	 * replaces it, judged again. */
	g_assert_true(axis_indexers_install_definition(fx->indexers, html, strlen(html), &installed, &error));
	g_assert_true(installed.replaced && installed.unchanged && installed.was_usable);
	g_assert_cmpint(installed.id, ==, id);
	g_assert_true(axis_indexers_install_definition(fx->indexers, login, strlen(login), &installed, &error));
	g_assert_true(installed.replaced && !installed.unchanged && installed.was_usable);
	{
		g_autoptr(AxisIndexerDefinitionInfo) now = NULL;

		g_assert_true(axis_indexers_get_definition(fx->indexers, id, &now, &error));
		g_assert_false(now->usable);
		g_assert_nonnull(strstr(now->problem, "a login"));
	}
	g_assert_true(axis_indexers_install_definition(fx->indexers, renamed, strlen(renamed), &installed, &error));
	g_assert_true(axis_indexers_get_definition(fx->indexers, id, (AxisIndexerDefinitionInfo **)&info, &error));
	g_assert_cmpstr(info->name, ==, "Example HTML Two");
	axis_indexer_definition_info_free((AxisIndexerDefinitionInfo *)info);

	/* What is not a definition is not stored. */
	g_assert_false(axis_indexers_install_definition(fx->indexers, "a: &x 1\n", 8, &installed, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "anchors"));
	g_clear_error(&error);
	g_assert_false(axis_indexers_install_definition(fx->indexers, "", 0, &installed, &error));
	g_clear_error(&error);
	g_assert_true(axis_indexers_list_definitions(fx->indexers, &all, &error));
	g_assert_cmpuint(all->len, ==, 1);
	g_clear_pointer(&all, g_ptr_array_unref);

	/* One that an indexer runs cannot be removed; then it can. */
	{
		gint64 source = add_indexer(fx, id, fx->site, NULL);

		g_assert_false(axis_indexers_remove_definition(fx->indexers, id, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
		g_clear_error(&error);
		g_assert_true(axis_indexers_delete_source(fx->indexers, source, &error));
		g_assert_true(axis_indexers_remove_definition(fx->indexers, id, &error));
		g_assert_no_error(error);
		g_assert_false(axis_indexers_remove_definition(fx->indexers, id, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	}
}

/* An indexer is added only from a usable definition, at an http or https
 * address, its path kept as the base path; its name and address default to
 * the definition's. */
static void
test_adding(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *post = replaced(html, "    - path: \"{{", "    - method: post\n      path: \"{{");
	gint64 usable = install(fx, html);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerSourceSpec spec;
	gint64 id = 0;
	gint64 not_usable;

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_INDEXER_CARDIGANN_NAME;
	spec.enabled = TRUE;
	spec.definition_id = usable;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpstr(source->name, ==, "Example HTML");
	g_assert_cmpstr(source->address, ==, "https://example-html.invalid:443");
	g_assert_cmpstr(source->base_path, ==, "");
	g_assert_cmpint(source->definition_id, ==, usable);
	g_assert_cmpstr(source->definition_name, ==, "Example HTML");
	g_assert_true(source->native);
	g_assert_cmpint(source->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_false(source->key_set);
	g_clear_pointer(&source, axis_indexer_source_info_free);

	/* A typed address with a path: the path is its base path. */
	spec.name = "Under a path";
	spec.address = "https://mirror.example.invalid/forum/";
	spec.secret = "a key is never kept";
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpstr(source->address, ==, "https://mirror.example.invalid:443");
	g_assert_cmpstr(source->base_path, ==, "/forum");
	g_assert_false(source->key_set);

	/* Not usable, not installed, not given, a query: refused. */
	not_usable = install(fx, post);
	g_assert_cmpint(not_usable, ==, usable);
	spec.name = "Posting";
	spec.address = NULL;
	spec.secret = NULL;
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "not usable yet"));
	g_assert_nonnull(strstr(error->message, "POST"));
	g_clear_error(&error);
	spec.definition_id = 999999;
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_nonnull(strstr(error->message, "not installed"));
	g_clear_error(&error);
	spec.definition_id = 0;
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_nonnull(strstr(error->message, "Choose the definition"));
	g_clear_error(&error);
	install(fx, html);
	spec.definition_id = usable;
	spec.address = "https://example.invalid/?q=1";
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_nonnull(strstr(error->message, "without a query"));
}

/* ── Searching and fetching ─────────────────────────────────────── */

/* Spec: Supported definition: searching returns that site's releases;
 * tasks 1.5, 4.1 */
static void
test_search_and_fetch(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = fixture("example-html-search.html");
	g_autofree gchar *details = fixture("example-html-details.html");
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *base = axis_test_plex_base(fx->site);
	g_autofree gchar *referer = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *expected_referer = g_strconcat(base, "/", NULL);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const AxisIndexerRelease *release;
	AxisIndexerFetched fetched;
	AxisIndexerRequest request;
	gint64 definition = install(fx, html);
	gint64 id = add_indexer(fx, definition, fx->site, NULL);

	route(fx->site, "/search/hearth+1995/added/1/", "text/html; charset=utf-8", page);
	route(fx->site, "/torrent/101/hearth-1995", "text/html", details);
	movie(&request, "Hearth", 1995);
	g_assert_true(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(result->releases->len, ==, 2);
	g_assert_cmpuint(result->n_skipped, ==, 1);
	release = release_named(result, "Hearth 1995 1080p BluRay x264-GROUP");
	g_assert_cmpint(release->size_bytes, ==, G_GINT64_CONSTANT(9126805504));
	g_assert_cmpint(release->seeders, ==, 1234);
	g_assert_cmpint(release->categories[0], ==, 2040);
	g_assert_cmpstr(release->indexer_name, ==, "Example HTML");
	g_assert_false(release->has_magnet);
	/* What was asked: the path, the query, its Referer; nothing else. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 1);
	g_assert_false(axis_test_plex_saw_non_get(fx->site));
	transcript = axis_test_plex_transcript(fx->site);
	g_assert_nonnull(strstr(transcript, "GET /search/hearth+1995/added/1/?cat=1%2C2"));
	referer = axis_test_plex_last_header(fx->site, "/search/hearth+1995/added/1/", "Referer");
	g_assert_cmpstr(referer, ==, expected_referer);

	/* The grab reads the release's page for its magnet link; the torrent
	 * file at another site is never asked. */
	memset(&fetched, 0, sizeof(fetched));
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, "0123456789abcdef0123456789abcdef01234567");
	g_assert_cmpuint(axis_test_plex_count(fx->site, "/torrent/101/hearth-1995"), ==, 1);
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);

	/* The same search again is answered from what was kept. */
	{
		g_autoptr(AxisIndexerSearchResult) again = NULL;

		g_assert_true(axis_indexers_search(fx->indexers, id, &request, NULL, &again, &error));
		g_assert_true(again->cached);
		g_assert_cmpuint(axis_test_plex_count(fx->site, "/search/hearth+1995/added/1/"), ==, 1);
	}
	/* A test asks for its newest page. */
	route(fx->site, "/latest/", "text/html", page);
	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	{
		g_autoptr(AxisIndexerSourceInfo) source = NULL;

		g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
		g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
		g_assert_nonnull(strstr(source->version, "by its definition"));
		g_assert_true((source->caps.modes & AXIS_INDEXER_CAN_MOVIE) != 0);
	}
}

/* A site whose release is a torrent file on its own origin; one whose
 * definition is for TV only is not asked for a movie. */
static void
test_files_and_coverage(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *rss_text = fixture("example-rss.yml");
	g_autofree gchar *feed = fixture("example-rss.xml");
	g_autofree gchar *base = axis_test_plex_base(fx->site);
	g_autofree gchar *own = replaced(feed, "https://feed.example-rss.invalid", base);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) movies = NULL;
	g_autoptr(GError) error = NULL;
	static const guint8 torrent[] = "d4:infod6:lengthi1e4:name1:a12:piece lengthi16384e6:pieces20:"
		"aaaaaaaaaaaaaaaaaaaaee";
	g_autoptr(GBytes) file = g_bytes_new(torrent, sizeof(torrent) - 1);
	const AxisIndexerRelease *release;
	AxisIndexerFetched fetched;
	AxisIndexerRequest request;
	gint64 id = add_indexer(fx, install(fx, rss_text), fx->site, NULL);

	route(fx->site, "/rss/search.xml", "application/rss+xml", own);
	axis_test_plex_route_full(fx->site, "/get/1.torrent", 200, "application/x-bittorrent", file, NULL);
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.season = 1;
	request.episode = 2;
	g_assert_true(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(result->releases->len, ==, 2);
	g_assert_cmpuint(result->n_off_origin, ==, 1);
	release = release_named(result, "The Show S01E02 720p HDTV x264 & more");
	memset(&fetched, 0, sizeof(fetched));
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	g_assert_null(fetched.magnet);
	g_assert_cmpuint(axis_test_plex_count(fx->site, "/get/1.torrent"), ==, 1);
	axis_indexer_fetched_clear(&fetched);

	movie(&request, "Hearth", 1995);
	axis_test_plex_reset_requests(fx->site);
	g_assert_true(axis_indexers_search(fx->indexers, id, &request, NULL, &movies, &error));
	g_assert_cmpuint(movies->releases->len, ==, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 0);
}

/* ── Elsewhere ──────────────────────────────────────────────────── */

/* Spec: Elsewhere: a definition or a page that would lead to another
 * address makes no request there; task 4.3 */
static void
test_never_elsewhere(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *other = axis_test_plex_base(fx->other);
	g_autofree gchar *self_base = axis_test_plex_base(fx->site);
	g_autofree gchar *hostile_text = fixture("hostile-path.yml");
	g_autofree gchar *hostile = replaced(hostile_text, "@OTHER@", other);
	g_autofree gchar *links_page = fixture("hostile-links-search.html");
	g_autofree gchar *page_other = replaced(links_page, "@OTHER@", other);
	g_autofree gchar *page = replaced(page_other, "@SELF@", self_base);
	g_autofree gchar *release_text = fixture("hostile-links-release.html");
	g_autofree gchar *release_page = replaced(release_text, "@OTHER@", other);
	g_autofree gchar *links_text = fixture("hostile-links.yml");
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerFetched fetched;
	AxisIndexerRequest request;
	gint64 path_id = add_indexer(fx, install(fx, hostile), fx->site, NULL);
	gint64 links_id = add_indexer(fx, install(fx, links_text), fx->site, NULL);
	guint i;

	movie(&request, "Hearth", 1995);
	/* A path that leads elsewhere: refused, nothing asked anywhere. */
	g_assert_false(axis_indexers_search(fx->indexers, path_id, &request, NULL, &result, &error));
	g_assert_nonnull(strstr(error->message, "not its site's"));
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 0);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);

	/* Pages that point elsewhere: those releases are left out; the page of
	 * a kept one gives a file only elsewhere: refused. */
	route(fx->site, "/search", "text/html", page);
	route(fx->site, "/release/1", "text/html", release_page);
	route(fx->site, "/release/2", "text/html", release_page);
	g_assert_true(axis_indexers_search(fx->indexers, links_id, &request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(result->releases->len, ==, 2);
	g_assert_cmpuint(result->n_off_origin, ==, 2);
	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		memset(&fetched, 0, sizeof(fetched));
		g_assert_false(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
		g_assert_nonnull(strstr(error->message, "another address"));
		g_clear_error(&error);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);
	g_clear_pointer(&result, axis_indexer_search_result_free);

	/* A site that redirects its search elsewhere is not followed. */
	{
		g_autofree gchar *location = g_strconcat(other, "/search", NULL);
		g_autoptr(AxisIndexerSearchResult) redirected = NULL;
		AxisIndexerRequest words;

		axis_test_plex_clear_routes(fx->site);
		axis_test_plex_route(fx->site, "/search", -1, AXIS_TEST_PLEX_REDIRECT, 302, location, 0);
		axis_indexer_request_init(&words, AXIS_INDEXER_QUERY_TEXT);
		words.title = "something else";
		g_assert_false(axis_indexers_search(fx->indexers, links_id, &words, NULL, &redirected, &error));
		g_assert_nonnull(strstr(error->message, "does not follow"));
		g_clear_error(&error);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(fx->site));
}

/* Spec: Elsewhere: a release's download address that redirects off the
 * origin is refused and nothing is asked there, though the other address
 * would answer, with following download redirects left on for the indexer
 * as for any new one (a definition's indexer never follows one); one that
 * redirects to a magnet link gives that link; task 4.3 */
static void
test_download_redirect(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *rss_text = fixture("example-rss.yml");
	g_autofree gchar *feed = fixture("example-rss.xml");
	g_autofree gchar *base = axis_test_plex_base(fx->site);
	g_autofree gchar *other = axis_test_plex_base(fx->other);
	g_autofree gchar *own = replaced(feed, "https://feed.example-rss.invalid", base);
	g_autofree gchar *elsewhere = g_strconcat(other, "/get/1.torrent", NULL);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	static const gchar magnet[] = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=The+Show";
	static const guint8 torrent[] = "d4:infod6:lengthi1e4:name1:a12:piece lengthi16384e6:pieces20:"
		"aaaaaaaaaaaaaaaaaaaaee";
	g_autoptr(GBytes) file = g_bytes_new(torrent, sizeof(torrent) - 1);
	const AxisIndexerRelease *release;
	AxisIndexerFetched fetched;
	AxisIndexerRequest request;
	gint64 id = add_indexer(fx, install(fx, rss_text), fx->site, NULL);

	route(fx->site, "/rss/search.xml", "application/rss+xml", own);
	axis_test_plex_route(fx->site, "/get/1.torrent", -1, AXIS_TEST_PLEX_REDIRECT, 302, elsewhere, 0);
	axis_test_plex_route(fx->site, "/get/3.torrent", -1, AXIS_TEST_PLEX_REDIRECT, 302, magnet, 0);
	axis_test_plex_route_full(fx->other, "/get/1.torrent", 200, "application/x-bittorrent", file, NULL);
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.season = 1;
	request.episode = 2;
	g_assert_true(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(result->releases->len, ==, 2);

	/* Off the origin: refused, nothing sent there. */
	release = release_named(result, "The Show S01E02 720p HDTV x264 & more");
	memset(&fetched, 0, sizeof(fetched));
	g_assert_false(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_nonnull(strstr(error->message, "does not follow"));
	g_assert_nonnull(strstr(error->message, "nothing was sent there"));
	g_assert_null(fetched.file);
	g_assert_null(fetched.magnet);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_plex_count(fx->site, "/get/1.torrent"), ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);

	/* To a magnet link: read, never followed. */
	release = release_named(result, "The Show S01E02 SD");
	memset(&fetched, 0, sizeof(fetched));
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_null(fetched.file);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, "0123456789abcdef0123456789abcdef01234567");
	axis_indexer_fetched_clear(&fetched);

	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(fx->site));
}

/* Spec: Unsupported feature: an indexer whose definition was replaced by
 * one this engine cannot run fails, saying what it needs; task 4.2 */
static void
test_replaced_by_unusable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *dates = replaced(html, "    filters:\n      - name: andmatch\n",
		"    filters:\n      - name: andmatch\n    dateheaders:\n      selector: td\n");
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 definition = install(fx, html);
	gint64 id = add_indexer(fx, definition, fx->site, NULL);

	install(fx, dates);
	movie(&request, "Hearth", 1995);
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_nonnull(strstr(error->message, "not usable yet"));
	g_assert_nonnull(strstr(error->message, "dateheaders"));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 0);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_FAILING);
	g_assert_nonnull(strstr(source->state_message, "dateheaders"));
}

/* A site behind a browser challenge shows as not usable yet, needing a
 * browser helper (stage 3, not built): its challenge page, or the 503 a
 * site its definition says is known to challenge answers with. */
static void
test_challenge(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *page = fixture("example-html-search.html");
	g_autofree gchar *hinted = replaced(html, "settings:\n", "settings:\n  - name: info_flaresolverr\n"
		"    type: info_flaresolverr\n");
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 id = add_indexer(fx, install(fx, hinted), fx->site, NULL);

	movie(&request, "Hearth", 1995);
	route(fx->site, "/search/hearth+1995/added/1/", "text/html", "<html><head><title>Just a moment...</title></head>"
		"<body><script>window._cf_chl_opt={cvId:'3'}</script></body></html>");
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_nonnull(strstr(error->message, "needs a browser helper"));
	g_clear_error(&error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_FAILING);
	g_assert_nonnull(strstr(source->state_message, "browser helper"));
	/* Searches pass it over for a while (add-builtin-torrent-indexers),
	 * asking nothing. */
	g_assert_cmpint(source->held_until_us, >, g_get_real_time());
	g_assert_nonnull(strstr(source->hold_message, "browser challenge"));
	g_clear_pointer(&source, axis_indexer_source_info_free);
	{
		guint before = axis_test_plex_n_requests(fx->site);

		g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
		g_assert_nonnull(strstr(error->message, "searches pass it over until"));
		g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before);
		g_clear_error(&error);
	}

	/* A test still asks it: the 503 of a site known to challenge. */
	axis_test_plex_clear_routes(fx->site);
	axis_test_plex_route(fx->site, "/latest/", -1, AXIS_TEST_PLEX_STATUS, 200, NULL, 503);
	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_nonnull(strstr(source->state_message, "answered 503"));
	g_assert_nonnull(strstr(source->state_message, "needs a browser helper"));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->other), ==, 0);

	/* (add-attention-notices, after the N2 review.)  The hold's hour passes:
	 * its words stay in the row, which is how the page "Needs attention"
	 * knows the indexer has not answered properly since.  The contact that
	 * works forgets the hold. */
	g_clear_pointer(&source, axis_indexer_source_info_free);
	axis_test_db_exec(fx->db, "UPDATE indexer_source SET held_until = now() - interval '1 minute'");
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_nonnull(strstr(source->hold_message, "browser challenge"));
	g_clear_pointer(&source, axis_indexer_source_info_free);
	axis_test_plex_clear_routes(fx->site);
	route(fx->site, "/search/hearth+1995/added/1/", "text/html; charset=utf-8", page);
	g_assert_true(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_null(source->hold_message);
	g_assert_cmpint(source->held_until_us, ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/cardigann/" path, Fixture, NULL, func)
	ADD("installing", test_installing);
	ADD("adding", test_adding);
	ADD("search-and-fetch", test_search_and_fetch);
	ADD("files-and-coverage", test_files_and_coverage);
	ADD("never-elsewhere", test_never_elsewhere);
	ADD("download-redirect", test_download_redirect);
	ADD("replaced-by-unusable", test_replaced_by_unusable);
	ADD("challenge", test_challenge);
#undef ADD
	return g_test_run();
}
