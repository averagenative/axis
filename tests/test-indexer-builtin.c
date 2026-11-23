/*
 * test-indexer-builtin.c - the site definitions Axis ships (TorrentProject,
 * LimeTorrents, TheRARBG, Torrents-CSV): written at start, overridden by an
 * administrator, searched and fetched against stand-ins of their sites, told
 * apart when their pages no longer look as expected, and checked step by
 * step, on a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The stand-ins answer from tests/fixtures/builtin (its README: written by
 * hand in the shapes the sites answered on 2026-11-18, with made-up titles,
 * hashes and names).  No test reaches a real site.
 *
 * add-builtin-torrent-indexers, spec scenarios (indexers): Offered (the
 * pages' part is test-web-indexers), An older copy after a restore, The
 * RARBG mirror, A newer override, Not newer, Ignored after an upgrade,
 * Changed layout, Nothing found, Music in a feed.  Tasks 4.1 to 4.3.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-plex.h"
#include "download/axis-download-model.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-cardigann.h"
#include "indexer/axis-indexer-def-check.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-shipped.h"
#include "indexer/def/axis-def.h"
#include "indexer/def/axis-def-engine.h"

#define FIXTURES "tests/fixtures/builtin/"

typedef struct
{
	AxisTestDb   *db;
	AxisDbPool   *pool;
	AxisSettings *settings;
	AxisIndexers *indexers;
	AxisTestPlex *site;
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
	/* What the module's start does. */
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, NULL, &error));
	g_assert_no_error(error);
	fx->site = axis_test_plex_new();
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

	g_assert_cmpuint(g_strv_length(parts), >, 1);
	return g_strjoinv(to, parts);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

/* The listed definition of @def_id. */
static AxisIndexerDefinitionInfo *
definition_of(
	Fixture     *fx,
	const gchar *def_id
){
	g_autoptr(GPtrArray) all = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	g_assert_true(axis_indexers_list_definitions(fx->indexers, &all, &error));
	for (i = 0; i < all->len; i++) {
		AxisIndexerDefinitionInfo *info = g_ptr_array_index(all, i);

		if (g_strcmp0(info->def_id, def_id) == 0)
			return g_ptr_array_steal_index(all, i);
	}
	g_error("no definition %s", def_id);
	return NULL;
}

/* An indexer that runs the built-in @def_id at the stand-in's address. */
static gint64
add_builtin(
	Fixture     *fx,
	const gchar *def_id
){
	g_autoptr(AxisIndexerDefinitionInfo) info = definition_of(fx, def_id);
	AxisIndexerSourceSpec spec;
	g_autofree gchar *base = axis_test_plex_base(fx->site);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_INDEXER_CARDIGANN_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.definition_id = info->id;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
route(
	Fixture     *fx,
	const gchar *path,
	guint        status,
	const gchar *content_type,
	const gchar *body
){
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

	axis_test_plex_route_full(fx->site, path, status, content_type, bytes, NULL);
}

static void
route_fixture(
	Fixture     *fx,
	const gchar *path,
	const gchar *content_type,
	const gchar *name
){
	g_autofree gchar *body = fixture(name);

	route(fx, path, 200, content_type, body);
}

static void
movie(AxisIndexerRequest *request)
{
	axis_indexer_request_init(request, AXIS_INDEXER_QUERY_FILM);
	request->title = "Made Up Film";
	request->year = 2001;
	request->imdb_id = "tt0000123";
}

static AxisIndexerSearchResult *
search_ok(
	Fixture                  *fx,
	gint64                    id,
	const AxisIndexerRequest *request
){
	AxisIndexerSearchResult *result = NULL;
	g_autoptr(GError) error = NULL;

	if (!axis_indexers_search(fx->indexers, id, request, NULL, &result, &error))
		g_error("the search failed: %s", error->message);
	return result;
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

static gboolean
has_release(
	const AxisIndexerSearchResult *result,
	const gchar                   *part
){
	guint i;

	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (strstr(release->name, part) != NULL)
			return TRUE;
	}
	return FALSE;
}

/* The built-in @def_id's file with its version raised to @version (and
 * @from made @to, when given). */
static gchar *
variant(
	const gchar *def_id,
	gint         version,
	const gchar *from,
	const gchar *to
){
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find(def_id);
	g_autofree gchar *was = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *versioned = NULL;

	g_assert_nonnull(shipped);
	was = g_strdup_printf("\nversion: %d\n", shipped->version);
	now = g_strdup_printf("\nversion: %d\n", version);
	versioned = replaced(shipped->body, was, now);
	return from != NULL ? replaced(versioned, from, to) : g_steal_pointer(&versioned);
}

/* ── Built in ───────────────────────────────────────────────────── */

/* Task 4.1, 4.3: each built-in definition is usable, says a version, has
 * the id of its file and builds its requests; they are rows at start */
static void
test_shipped(void)
{
	static const gchar *const ids[] = { "axis-limetorrents", "axis-therarbg", "axis-torrentproject", "axis-torrentscsv" };
	guint i;

	g_assert_cmpuint(axis_indexer_shipped_count(), ==, G_N_ELEMENTS(ids));
	for (i = 0; i < G_N_ELEMENTS(ids); i++) {
		const AxisIndexerShipped *shipped = axis_indexer_shipped_at(i);
		g_autoptr(AxisDef) def = NULL;
		g_autofree gchar *problem = NULL;

		g_assert_cmpstr(shipped->def_id, ==, ids[i]);
		g_assert_true(axis_indexer_shipped_find(ids[i]) == shipped);
		g_assert_cmpint(shipped->version, >=, 1);
		g_assert_cmpuint(strlen(shipped->sha256), ==, 64);
		g_assert_nonnull(shipped->description);
		/* One sentence of what the site is, for the page. */
		g_assert_cmpuint(strlen(shipped->description), <, 300);
		def = axis_def_load(shipped->body, shipped->length, &problem);
		g_assert_nonnull(def);
		g_assert_true(axis_def_usable(def));
		/* Every one tells an empty answer from a changed page. */
		g_assert_nonnull(def->rows_expect);
		g_assert_true(g_str_has_prefix(g_ptr_array_index(def->links, 0), "https://"));
		/* Its own version is not an override of it; a newer one is. */
		g_assert_false(axis_indexer_shipped_check_override(shipped, shipped->body, shipped->length, &problem));
		g_assert_nonnull(strstr(problem, "must say a later version"));
		g_clear_pointer(&problem, g_free);
		{
			g_autofree gchar *newer = variant(ids[i], shipped->version + 1, NULL, NULL);

			g_assert_true(axis_indexer_shipped_check_override(shipped, newer, strlen(newer), &problem));
		}
	}
	g_assert_null(axis_indexer_shipped_find("axis-nothing"));
	g_assert_true(axis_indexer_shipped_reserved("axis-nothing"));
	g_assert_true(axis_indexer_shipped_reserved("AXIS-x"));
	g_assert_false(axis_indexer_shipped_reserved("limetorrents"));
}

/* Spec: An older copy after a restore: the rows are written at start, and
 * an older copy (an earlier build's, a restored database's) is replaced */
static void
test_rows_at_start(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerDefinitionInfo) info = definition_of(fx, "axis-limetorrents");
	g_autoptr(AxisIndexerDefinitionInfo) after = NULL;
	g_autoptr(GError) error = NULL;
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find("axis-limetorrents");
	g_autofree gchar *expected = g_strdup_printf("%s", shipped->sha256);

	assert_q(fx, "4", "SELECT count(*) FROM indexer_definition WHERE def_id LIKE 'axis-%'");
	g_assert_true(info->built_in);
	g_assert_true(info->usable);
	g_assert_cmpint(info->version, ==, shipped->version);
	g_assert_cmpint(info->built_in_version, ==, shipped->version);
	g_assert_false(info->overridden);
	g_assert_false(info->override_ignored);
	g_assert_cmpuint(info->n_indexers, ==, 0);
	/* Written again with nothing changed: nothing is. */
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, NULL, &error));
	assert_q(fx, "4", "SELECT count(*) FROM indexer_definition");
	/* A copy an earlier version left (or a restored backup holds). */
	axis_test_db_exec(fx->db, "UPDATE indexer_definition SET body = replace(body, 'searchrss', 'oldfeed'),"
		" sha256 = repeat('0', 64) WHERE def_id = 'axis-limetorrents'");
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, NULL, &error));
	g_assert_no_error(error);
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM indexer_definition WHERE def_id = "
			"'axis-limetorrents' AND sha256 = '%s' AND body NOT LIKE '%%oldfeed%%'", expected);

		assert_q(fx, "1", sql);
	}
	/* A built-in one is not removed. */
	after = definition_of(fx, "axis-limetorrents");
	g_assert_false(axis_indexers_remove_definition(fx->indexers, after->id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "built into Axis"));
}

/* ── The sites ──────────────────────────────────────────────────── */

/* Spec: Music in a feed; task 4.3: LimeTorrents' feed, every field; not
 * video left out; the hash out of another site's address, never fetched */
static void
test_limetorrents(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) none = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;
	gint64 id = add_builtin(fx, "axis-limetorrents");

	route_fixture(fx, "/searchrss/Made+Up+Film+2001/", "text/html; charset=UTF-8", "limetorrents-search.xml");
	movie(&request);
	result = search_ok(fx, id, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpuint(result->releases->len, ==, 5);
	release = release_named(result, "Made Up Film 2001 1080p WEB-DL x265-NOGRP");
	g_assert_cmpstr(release->info_hash, ==, "a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7");
	g_assert_cmpint(release->size_bytes, ==, G_GINT64_CONSTANT(2000000001));
	g_assert_cmpint(release->seeders, ==, 501);
	g_assert_cmpint(release->leechers, ==, 51);
	g_assert_cmpint(release->published_us, >, 0);
	g_assert_cmpuint(release->n_categories, ==, 1);
	g_assert_cmpint(release->categories[0], ==, 2000);
	g_assert_true(release->has_magnet);
	/* A name the feed escaped twice is read once. */
	release_named(result, "Made Up Film 2001 2160p UHD BluRay x265 & More-NOGRP");
	g_assert_cmpint(release_named(result, "Made Up Show S01E02 720p WEB x264-NOGRP")->categories[0], ==, 5000);
	g_assert_cmpint(release_named(result, "Made Up Cartoon - Made Up Fansubs [WEBRip 1080p HEVC]")->categories[0], ==,
		5070);
	g_assert_cmpint(release_named(result, "Made Up Film Extras 2001")->seeders, ==, 1234);
	/* The e-book and the music are not video. */
	g_assert_false(has_release(result, "Novel"));
	g_assert_false(has_release(result, "Soundtrack"));

	/* The release is the magnet link built from its hash: nothing more is
	 * asked, of the site or of the feed's other site. */
	release = release_named(result, "Made Up Film 2001 1080p WEB-DL x265-NOGRP");
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, release->info_hash);
	g_assert_null(strstr(fetched.magnet, "itorrents"));
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 1);

	/* Nothing found: the channel is there, with no item. */
	route_fixture(fx, "/searchrss/Nothing+Made+Up/", "text/html", "limetorrents-empty.xml");
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = "Nothing Made Up";
	none = search_ok(fx, id, &request);
	g_assert_cmpuint(none->releases->len, ==, 0);
	assert_q(fx, "ok|", "SELECT state || '|' || coalesce(state_message, '') FROM indexer_source");
	transcript = axis_test_plex_transcript(fx->site);
	g_assert_null(strstr(transcript, "Cookie"));
	g_assert_false(axis_test_plex_saw_non_get(fx->site));
}

/* Task 4.3: TorrentProject's result page (a cut name read from its link);
 * the hash read from the chosen release's own page at grab time, never from
 * the redirector its magnet link goes through */
static void
test_torrentproject(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *query = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;
	gint64 id = add_builtin(fx, "axis-torrentproject");

	route_fixture(fx, "/", "text/html; charset=UTF-8", "torrentproject-search.html");
	route_fixture(fx, "/t3-1000001/Made-Up-Film-2001-1080p-BluRay-x264-NOGRP-torrent.html", "text/html",
		"torrentproject-details.html");
	movie(&request);
	result = search_ok(fx, id, &request);
	query = axis_test_plex_transcript(fx->site);
	g_assert_nonnull(strstr(query, "GET /?t=Made+Up+Film+2001&orderby=seeders"));
	/* Five rows; the script's text that looks like one is none. */
	g_assert_cmpuint(result->releases->len, ==, 5);
	release = release_named(result, "Made.Up.Film.2001.1080p.BluRay.x264-NOGRP");
	g_assert_null(release->info_hash);
	g_assert_false(release->has_magnet);
	g_assert_cmpint(release->seeders, ==, 301);
	g_assert_cmpint(release->leechers, ==, 31);
	g_assert_cmpint(release->size_bytes, ==, (gint64)(2.25 * 1024 * 1024 * 1024));
	g_assert_cmpint(release->published_us, >, 0);
	g_assert_cmpint(release_named(result, "Made.Up.Film.2001.iTA-ENG.WEBDL.1080p.x264-NOGRP.mkv")->seeders, ==, 1500);
	/* The list cut this one's name: its address has it whole. */
	release_named(result, "Made Up Film 2001 1080p MA WEB DL x265 10bit EAC3 Atmos 5 1 NOGRP");
	g_assert_cmpint(release_named(result, "Made.Up.Film.2001.Extras-NOGRP")->published_us, >, 0);
	/* Only the release fetched has its page asked. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 1);
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(fetched.info_hash, ==, "c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8");
	g_assert_nonnull(fetched.magnet);
	g_assert_null(strstr(fetched.magnet, "redirector"));
	g_assert_nonnull(strstr(fetched.magnet, "dn=Made.Up.Film.2001.1080p.BluRay.x264-NOGRP"));
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 2);
	g_assert_cmpuint(axis_test_plex_count(fx->site, "/t3-1000001/Made-Up-Film-2001-1080p-BluRay-x264-NOGRP-torrent.html"),
		==, 1);

	/* A release page without its hash: the fetch fails, saying so, and
	 * nothing is guessed. */
	{
		const AxisIndexerRelease *other = release_named(result, "Made.Up.Film.2001.GERMAN.720p.BluRay.H264.AAC-NOGRP");

		route(fx, "/t4/Made-Up-Film-2001-GERMAN-720p-BluRay-H264-AAC-NOGRP-torrent.html", 200, "text/html",
			"<html><body><h1>Gone</h1></body></html>");
		memset(&fetched, 0, sizeof(fetched));
		g_assert_false(axis_indexers_fetch(fx->indexers, other->id, NULL, &fetched, &error));
		g_assert_nonnull(strstr(error->message, "no download the definition can read"));
		g_assert_null(fetched.magnet);
	}
}

/* Task 4.3: TheRARBG's JSON listing, in its movie category; a name without
 * every word left out (andmatch), music and games not video */
static void
test_therarbg(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) none = NULL;
	AxisIndexerRequest request;
	const AxisIndexerRelease *release;
	gint64 id = add_builtin(fx, "axis-therarbg");

	route_fixture(fx, "/get-posts/order:-se:category:Movies:keywords:Made+Up+Film+2001:paginate_by:100:format:json/",
		"application/json", "therarbg-search.json");
	movie(&request);
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 3);
	release = release_named(result, "Made Up Film 2001 1080p WEB DL HEVC x265 5 1 NOGRP");
	g_assert_cmpstr(release->info_hash, ==, "a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9a9");
	g_assert_cmpint(release->size_bytes, ==, G_GINT64_CONSTANT(2100000001));
	g_assert_cmpint(release->seeders, ==, 601);
	g_assert_cmpint(release->leechers, ==, 61);
	g_assert_cmpint(release->published_us, ==, G_GINT64_CONSTANT(1730000101) * G_USEC_PER_SEC);
	g_assert_cmpstr(release->imdb_id, ==, "tt0000123");
	g_assert_cmpint(release->categories[0], ==, 2000);
	release_named(result, "Made Up Film 2001 Fan Edit 720p");
	g_assert_false(has_release(result, "Returns"));
	g_assert_false(has_release(result, "Soundtrack"));
	g_assert_false(has_release(result, "The Game"));

	/* An episode: in its TV categories. */
	route_fixture(fx, "/get-posts/order:-se:category:TV:category:Anime:category:Documentaries:keywords:Made+Up+Show+"
		"S01E02:paginate_by:100:format:json/", "application/json", "therarbg-empty.json");
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "Made Up Show";
	request.season = 1;
	request.episode = 2;
	none = search_ok(fx, id, &request);
	g_assert_cmpuint(none->releases->len, ==, 0);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
}

/* Spec: The RARBG mirror: a 403 that is a browser challenge says so plainly,
 * and searches pass the indexer over for a while */
static void
test_therarbg_mirror_challenge(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 id = add_builtin(fx, "axis-therarbg");
	guint before;

	route(fx, "/get-posts/order:-se:category:Movies:keywords:Made+Up+Film+2001:paginate_by:100:format:json/", 403,
		"text/html; charset=UTF-8", "<!DOCTYPE html><html lang=\"en-US\"><head><title>Just a moment...</title>"
		"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\"></head><body></body></html>");
	movie(&request);
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_nonnull(strstr(error->message, "with a browser challenge in front of its pages, which Axis cannot pass"));
	g_clear_error(&error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_FAILING);
	g_assert_nonnull(strstr(source->state_message, "browser challenge"));
	g_assert_cmpint(source->held_until_us, >, g_get_real_time());
	before = axis_test_plex_n_requests(fx->site);
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "answers with a browser challenge Axis cannot pass"));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before);
}

/* Review item 3: what says it is a challenge is one, whatever the page
 * holds: a 503 with "cf-mitigated: challenge"; an ordinary page with
 * Cloudflare's detection script (both forms, in the TorrentProject
 * fixtures) is read as the page it is */
static void
test_challenge_header(Fixture *fx, gconstpointer data)
{
	static const gchar *const mitigated[] = { "cf-mitigated", "challenge", NULL };
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GBytes) body = g_bytes_new_static("<html><body><p>One moment.</p></body></html>", 44);
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	gint64 id = add_builtin(fx, "axis-therarbg");

	axis_test_plex_route_full(fx->site, "/get-posts/order:-se:category:Movies:keywords:Made+Up+Film+2001:"
		"paginate_by:100:format:json/", 503, "text/html", body, mitigated);
	movie(&request);
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_nonnull(strstr(error->message, "with a browser challenge in front of its pages, which Axis cannot pass"));
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_nonnull(strstr(source->state_message, "browser challenge"));
}

/* Review item 5: a site that asks Axis to wait (429, Retry-After) is held
 * in those words; a test and a check honour it, saying until when, and
 * lift nothing; nor does a new version of its definition; a search says
 * the site asked to wait */
static void
test_site_asked_to_wait(Fixture *fx, gconstpointer data)
{
	static const gchar *const wait[] = { "Retry-After", "600", NULL };
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find("axis-torrentscsv");
	g_autofree gchar *newer = variant("axis-torrentscsv", shipped->version + 1, "size: 100", "size: 99");
	g_autoptr(GBytes) body = g_bytes_new_static("Slow down.", 10);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(AxisDefTrace) trace = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	AxisIndexerDefinitionInstalled installed;
	AxisIndexerRequest request;
	gint64 id = add_builtin(fx, "axis-torrentscsv");
	gint64 later = g_get_real_time() + G_GINT64_CONSTANT(500) * G_USEC_PER_SEC;
	guint before;

	axis_test_plex_route_full(fx->site, "/service/search", 429, "text/plain", body, wait);
	movie(&request);
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_clear_error(&error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->held_until_us, >, later);
	g_assert_cmpstr(source->hold_message, ==, "Torrents-CSV" AXIS_INDEXER_HOLD_WAIT_WORDS);
	g_assert_cmpint(axis_indexer_hold_cause(source, g_get_real_time()), ==, AXIS_INDEXER_HOLD_WAIT);
	g_clear_pointer(&source, axis_indexer_source_info_free);
	before = axis_test_plex_n_requests(fx->site);

	/* The test: not sent, and the hold stays. */
	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before);
	/* The check: not sent, saying until when. */
	g_assert_true(axis_indexers_check_source(fx->indexers, id, "Made Up Film", FALSE, NULL, &trace, &error));
	g_assert_no_error(error);
	text = axis_def_trace_to_text(trace);
	g_assert_nonnull(strstr(text, "Torrents-CSV asked Axis to wait, so it is not asked, by a test or a check either, "
		"until "));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before);
	/* A new version of its definition does not lift it either. */
	g_assert_true(axis_indexers_install_definition(fx->indexers, newer, strlen(newer), &installed, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->held_until_us, >, later);
	g_assert_cmpint(axis_indexer_hold_cause(source, g_get_real_time()), ==, AXIS_INDEXER_HOLD_WAIT);
	g_clear_pointer(&source, axis_indexer_source_info_free);

	/* A search: passed over, in the site's words. */
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "Torrents-CSV asked Axis to wait, so it is not asked again until "));
	g_assert_null(strstr(error->message, "a test or a check that passes ends that"));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before);
}

/* Task 4.3: Torrents-CSV's API; an entry whose hash is not one is lost, a
 * count it leaves empty unstated */
static void
test_torrentscsv(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *transcript = NULL;
	AxisIndexerRequest request;
	const AxisIndexerRelease *release;
	gint64 id = add_builtin(fx, "axis-torrentscsv");

	route_fixture(fx, "/service/search", "application/json", "torrentscsv-search.json");
	movie(&request);
	result = search_ok(fx, id, &request);
	transcript = axis_test_plex_transcript(fx->site);
	g_assert_nonnull(strstr(transcript, "GET /service/search?q=Made+Up+Film+2001&size=100"));
	g_assert_cmpuint(result->releases->len, ==, 3);
	release = release_named(result, "Made Up Film 2001 1080p WEB-DL HEVC x265 5.1 NOGRP.mkv");
	g_assert_cmpstr(release->info_hash, ==, "d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6d6");
	g_assert_cmpint(release->size_bytes, ==, G_GINT64_CONSTANT(1900000001));
	g_assert_cmpint(release->seeders, ==, 701);
	g_assert_cmpint(release->leechers, ==, 71);
	g_assert_cmpint(release->grabs, ==, 30001);
	g_assert_cmpint(release->published_us, ==, G_GINT64_CONSTANT(1740000001) * G_USEC_PER_SEC);
	release = release_named(result, "Made Up Film (2001)");
	g_assert_cmpint(release->seeders, ==, -1);
	g_assert_cmpint(release->leechers, ==, -1);
	g_assert_false(has_release(result, "Broken"));
}

/* Review item 2: a movie known only by an IMDb identifier, which none of
 * the four is searched by, gives no words: nothing is asked, and the search
 * says why, instead of the word a test asks for ("1080p") bringing back a
 * hundred releases of other films as this one's.  The stand-ins would
 * answer that word with their fixtures. */
static void
test_identifier_only(Fixture *fx, gconstpointer data)
{
	static const gchar *const builtins[] = {
		"axis-limetorrents", "axis-torrentproject", "axis-therarbg", "axis-torrentscsv", NULL
	};
	AxisIndexerRequest request;
	guint i;

	route_fixture(fx, "/searchrss/1080p/", "text/xml", "limetorrents-search.xml");
	route_fixture(fx, "/", "text/html", "torrentproject-search.html");
	route_fixture(fx, "/get-posts/order:-se:category:Movies:keywords:1080p:paginate_by:100:format:json/",
		"application/json", "therarbg-search.json");
	route_fixture(fx, "/get-posts/order:-se:category:TV:category:Anime:category:Documentaries:keywords:1080p:"
		"paginate_by:100:format:json/", "application/json", "therarbg-search.json");
	route_fixture(fx, "/service/search", "application/json", "torrentscsv-search.json");
	for (i = 0; builtins[i] != NULL; i++) {
		g_autoptr(AxisIndexerSearchResult) result = NULL;
		gint64 id = add_builtin(fx, builtins[i]);

		axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_FILM);
		request.imdb_id = "tt0000123";
		result = search_ok(fx, id, &request);
		g_assert_cmpuint(result->releases->len, ==, 0);
		g_assert_nonnull(result->not_asked);
		g_assert_nonnull(strstr(result->not_asked, "searched by words, and this search has none"));
		g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 0);

		/* An episode of a series known only by its identifier, the same. */
		if (strcmp(builtins[i], "axis-therarbg") == 0) {
			g_autoptr(AxisIndexerSearchResult) episode = NULL;

			axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_SEASON);
			request.imdb_id = "tt0000456";
			request.season = 1;
			episode = search_ok(fx, id, &request);
			g_assert_cmpuint(episode->releases->len, ==, 0);
			g_assert_nonnull(episode->not_asked);
			g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, 0);
		}
	}
}

/* ── Drift ──────────────────────────────────────────────────────── */

/* Spec: Changed layout; Nothing found: a page without the part every answer
 * has is not "no results": failing in plain words, passed over for an hour,
 * a check still asks it and, once its pages are right again, lifts that */
static void
test_drift(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(AxisDefTrace) trace = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *account = NULL;
	AxisIndexerRequest request;
	const AxisDefTraceAnswer *answer;
	gint64 id = add_builtin(fx, "axis-torrentproject");
	guint before;

	/* Nothing found is an answer like any. */
	route_fixture(fx, "/", "text/html", "torrentproject-empty.html");
	movie(&request);
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 0);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	g_clear_pointer(&result, axis_indexer_search_result_free);

	/* Another business at the address, or a new layout. */
	axis_test_plex_clear_routes(fx->site);
	route(fx, "/", 200, "text/html", "<html><body><h1>This domain is for sale</h1><div><span><a href=\"/t1/x-torrent"
		".html\">x</a></span></div></body></html>");
	request.year = 2002;
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE);
	g_assert_nonnull(strstr(error->message, "TorrentProject's pages no longer look as expected (the part every answer "
		"of the site has is missing)"));
	g_assert_nonnull(strstr(error->message, "Check this indexer"));
	g_clear_error(&error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_FAILING);
	g_assert_nonnull(strstr(source->state_message, "pages no longer look as expected"));
	g_assert_cmpint(source->held_until_us, >, g_get_real_time() + G_GINT64_CONSTANT(3500) * G_USEC_PER_SEC);
	g_assert_cmpstr(source->hold_message, ==, "TorrentProject's pages no longer look as expected");
	g_clear_pointer(&source, axis_indexer_source_info_free);

	/* Passed over, asking nothing, with a search of them all too. */
	before = axis_test_plex_n_requests(fx->site);
	request.year = 2003;
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_clear_error(&error);
	/* (It is the only indexer: a search of them all has none to ask.) */
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &error));
	g_assert_nonnull(strstr(error->message, "pages no longer look as expected, so searches pass it over until"));
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before);

	/* A check still asks, and shows what was wrong. */
	g_assert_true(axis_indexers_check_source(fx->indexers, id, "made up film", TRUE, NULL, &trace, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->site), ==, before + 1);
	g_assert_cmpuint(trace->answers->len, ==, 1);
	answer = g_ptr_array_index(trace->answers, 0);
	g_assert_false(answer->expect_found);
	g_assert_cmpuint(answer->rows_matched, ==, 0);
	g_assert_cmpuint(answer->status, ==, 200);
	g_assert_nonnull(answer->excerpt);
	g_assert_nonnull(strstr(answer->excerpt, "This domain is for sale"));
	g_assert_nonnull(strstr(trace->outcome, "pages no longer look as expected"));
	account = axis_def_trace_to_text(trace);
	g_assert_nonnull(strstr(account, "Every answer has: #similarfiles: NOT FOUND"));
	g_clear_pointer(&trace, axis_def_trace_free);
	g_clear_pointer(&account, g_free);
	/* Still held: a failing check lifts nothing. */
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->held_until_us, >, g_get_real_time());
	g_clear_pointer(&source, axis_indexer_source_info_free);

	/* The site is right again: a check that reads it ends the hold. */
	axis_test_plex_clear_routes(fx->site);
	route_fixture(fx, "/", "text/html", "torrentproject-search.html");
	g_assert_true(axis_indexers_check_source(fx->indexers, id, "made up film", FALSE, NULL, &trace, &error));
	g_assert_cmpstr(trace->outcome, ==, "5 releases found");
	answer = g_ptr_array_index(trace->answers, 0);
	g_assert_true(answer->expect_found);
	g_assert_cmpuint(answer->rows_matched, ==, 5);
	g_assert_cmpuint(answer->kept, ==, 5);
	g_assert_null(answer->excerpt);
	g_assert_cmpuint(answer->rows->len, ==, 5);
	g_assert_null(((AxisDefTraceRow *)g_ptr_array_index(answer->rows, 0))->excerpt);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_cmpint(source->held_until_us, ==, 0);
	request.year = 2004;
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 5);

	/* A check of an indexer that runs no definition, or of no words, is
	 * refused. */
	g_clear_pointer(&trace, axis_def_trace_free);
	g_assert_false(axis_indexers_check_source(fx->indexers, id, "   ", FALSE, NULL, &trace, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_null(trace);
}

/* ── Overrides ──────────────────────────────────────────────────── */

/* Spec: A newer override; Not newer; Ignored after an upgrade; task 4.2 */
static void
test_overrides(Fixture *fx, gconstpointer data)
{
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find("axis-limetorrents");
	g_autofree gchar *newer = variant("axis-limetorrents", shipped->version + 1, "searchrss/", "searchfeed/");
	g_autofree gchar *same = variant("axis-limetorrents", shipped->version, "searchrss/", "searchfeed/");
	g_autofree gchar *unusable = variant("axis-limetorrents", shipped->version + 2, "\nsearch:\n",
		"\nlogin:\n  method: form\n  path: /login\nsearch:\n");
	g_autofree gchar *foreign = replaced(newer, "id: axis-limetorrents", "id: axis-unknown");
	g_autoptr(AxisIndexerDefinitionInfo) info = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerDefinitionInstalled installed;
	AxisIndexerRequest request;
	gint64 id = add_builtin(fx, "axis-limetorrents");
	gint64 builtin_id;

	info = definition_of(fx, "axis-limetorrents");
	builtin_id = info->id;
	g_clear_pointer(&info, axis_indexer_definition_info_free);
	route_fixture(fx, "/searchrss/Made+Up+Film+2001/", "text/xml", "limetorrents-search.xml");
	route_fixture(fx, "/searchfeed/Made+Up+Film+2001/", "text/xml", "limetorrents-empty.xml");

	/* Not newer, not usable, not a built-in one: refused, saying why. */
	g_assert_false(axis_indexers_install_definition(fx->indexers, same, strlen(same), &installed, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "an override must say a later version"));
	g_clear_error(&error);
	g_assert_false(axis_indexers_install_definition(fx->indexers, unusable, strlen(unusable), &installed, &error));
	g_assert_nonnull(strstr(error->message, "cannot run it"));
	g_clear_error(&error);
	g_assert_false(axis_indexers_install_definition(fx->indexers, foreign, strlen(foreign), &installed, &error));
	g_assert_nonnull(strstr(error->message, "Axis's own built-in definitions"));
	g_clear_error(&error);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_definition WHERE def_id LIKE '%.override'");

	/* Newer: kept apart, and the indexer runs it. */
	g_assert_true(axis_indexers_install_definition(fx->indexers, newer, strlen(newer), &installed, &error));
	g_assert_no_error(error);
	g_assert_true(installed.override);
	g_assert_cmpint(installed.id, ==, builtin_id);
	g_assert_false(installed.unchanged);
	info = definition_of(fx, "axis-limetorrents");
	g_assert_true(info->overridden);
	g_assert_false(info->override_ignored);
	g_assert_cmpint(info->version, ==, shipped->version + 1);
	g_assert_cmpint(info->override_version, ==, shipped->version + 1);
	g_assert_cmpint(info->built_in_version, ==, shipped->version);
	g_assert_cmpuint(info->n_indexers, ==, 1);
	g_clear_pointer(&info, axis_indexer_definition_info_free);
	movie(&request);
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->site, "/searchfeed/Made+Up+Film+2001/"), ==, 1);
	g_clear_pointer(&result, axis_indexer_search_result_free);
	/* The start writes the built-in copy, and never touches the override. */
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, NULL, &error));
	info = definition_of(fx, "axis-limetorrents");
	g_assert_true(info->overridden);
	g_clear_pointer(&info, axis_indexer_definition_info_free);

	/* After an upgrade whose built-in copy is as new: ignored, said so, the
	 * built-in copy run. */
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE indexer_definition SET body = replace(body, 'version: %d', "
			"'version: %d') WHERE def_id = 'axis-limetorrents.override'", shipped->version + 1, shipped->version);

		axis_test_db_exec(fx->db, sql);
	}
	info = definition_of(fx, "axis-limetorrents");
	g_assert_false(info->overridden);
	g_assert_true(info->override_ignored);
	g_assert_cmpint(info->version, ==, shipped->version);
	g_assert_nonnull(strstr(info->override_note, "as new or newer"));
	g_clear_pointer(&info, axis_indexer_definition_info_free);
	request.year = 2002;
	route_fixture(fx, "/searchrss/Made+Up+Film+2002/", "text/xml", "limetorrents-search.xml");
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 5);
	g_clear_pointer(&result, axis_indexer_search_result_free);

	/* Removing it returns to the built-in copy; there is nothing to remove
	 * then. */
	g_assert_true(axis_indexers_remove_definition_override(fx->indexers, builtin_id, &error));
	g_assert_no_error(error);
	info = definition_of(fx, "axis-limetorrents");
	g_assert_false(info->overridden);
	g_assert_false(info->override_ignored);
	g_clear_pointer(&info, axis_indexer_definition_info_free);
	g_assert_false(axis_indexers_remove_definition_override(fx->indexers, builtin_id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
}

/* Task 4.2: an override placed in the data directory is taken at start
 * when it passes the checks; one that does not is logged and changes
 * nothing */
static void
test_override_from_the_data_directory(Fixture *fx, gconstpointer data)
{
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find("axis-torrentscsv");
	g_autofree gchar *dir = g_dir_make_tmp("axis-definitions-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "axis-torrentscsv.yml", NULL);
	g_autofree gchar *newer = variant("axis-torrentscsv", shipped->version + 1, "service/search", "api/search");
	g_autofree gchar *older = variant("axis-torrentscsv", shipped->version, "service/search", "api/search");
	g_autoptr(AxisIndexerDefinitionInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_nonnull(dir);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the override*is not taken*");
	g_assert_true(g_file_set_contents(path, older, -1, NULL));
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, dir, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*must say a later version*"), ==, 1);
	info = definition_of(fx, "axis-torrentscsv");
	g_assert_false(info->overridden);
	g_assert_false(info->override_ignored);
	g_clear_pointer(&info, axis_indexer_definition_info_free);

	g_assert_true(g_file_set_contents(path, newer, -1, NULL));
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, dir, &error));
	info = definition_of(fx, "axis-torrentscsv");
	g_assert_true(info->overridden);
	g_assert_cmpint(info->version, ==, shipped->version + 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*override*axis-torrentscsv.yml*is in use*"), ==, 1);
	/* The same file at the next start: nothing new. */
	g_assert_true(axis_indexers_sync_shipped(fx->indexers, dir, &error));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*override*axis-torrentscsv.yml*is in use*"), ==, 1);
	g_clear_pointer(&info, axis_indexer_definition_info_free);

	/* Review item 4: a newer version pasted on the page is kept at the next
	 * start, the older file left (said in the log); a file newer than it is
	 * taken. */
	{
		g_autofree gchar *pasted = variant("axis-torrentscsv", shipped->version + 2, "service/search", "api/search");
		g_autofree gchar *newest = variant("axis-torrentscsv", shipped->version + 3, "service/search", "api/search");
		AxisIndexerDefinitionInstalled installed;

		g_assert_true(axis_indexers_install_definition(fx->indexers, pasted, strlen(pasted), &installed, &error));
		g_assert_no_error(error);
		g_assert_true(axis_indexers_sync_shipped(fx->indexers, dir, &error));
		info = definition_of(fx, "axis-torrentscsv");
		g_assert_true(info->overridden);
		g_assert_cmpint(info->version, ==, shipped->version + 2);
		g_clear_pointer(&info, axis_indexer_definition_info_free);
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*axis-torrentscsv.yml*is not taken: the override "
			"stored*is as new or newer*"), ==, 1);

		g_assert_true(g_file_set_contents(path, newest, -1, NULL));
		g_assert_true(axis_indexers_sync_shipped(fx->indexers, dir, &error));
		info = definition_of(fx, "axis-torrentscsv");
		g_assert_cmpint(info->version, ==, shipped->version + 3);
		g_clear_pointer(&info, axis_indexer_definition_info_free);
	}

	/* Review item 8: a file larger than a definition may be is refused
	 * before it is read; the version in use stays. */
	{
		g_autofree gchar *huge = g_strnfill(AXIS_INDEXER_DEFINITION_MAX_BYTES + 1, '#');

		g_assert_true(g_file_set_contents(path, huge, -1, NULL));
		g_assert_true(axis_indexers_sync_shipped(fx->indexers, dir, &error));
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*axis-torrentscsv.yml*is larger than*"), ==, 1);
		info = definition_of(fx, "axis-torrentscsv");
		g_assert_cmpint(info->version, ==, shipped->version + 3);
	}
	g_unlink(path);
	g_rmdir(dir);
}

/* ── The command line ───────────────────────────────────────────── */

/* Spec: A saved page: 6.1, axisd --check-definition against saved pages,
 * asking nothing */
static void
test_check_command(void)
{
	AxisDefCheckOptions options;
	g_autoptr(GString) out = g_string_new(NULL);
	g_autofree gchar *saved = g_dir_make_tmp("axis-check-XXXXXX", NULL);
	g_autofree gchar *definition = NULL;
	g_autofree gchar *path = NULL;
	gint status;

	/* A built-in one by its id, a saved feed, the rows' excerpts. */
	memset(&options, 0, sizeof(options));
	options.definition = "axis-limetorrents";
	options.page = FIXTURES "limetorrents-search.xml";
	options.words = "made up film";
	options.excerpts = TRUE;
	status = axis_indexer_def_check_run(&options, out);
	g_assert_cmpint(status, ==, 0);
	g_assert_nonnull(strstr(out->str, "Built in: axis-limetorrents, version "));
	g_assert_nonnull(strstr(out->str, "Request: /searchrss/made+up+film/\n"));
	g_assert_nonnull(strstr(out->str, "Every answer has: rss > channel: found\n"));
	g_assert_nonnull(strstr(out->str, "Rows: rss > channel > item: matched 7\n"));
	g_assert_nonnull(strstr(out->str, "5 kept, 0 lost for a field every result must have, 2 left out on purpose "
		"(2 not video)"));
	g_assert_nonnull(strstr(out->str, "Row 5: left out: its category is not video (Books)"));
	g_assert_nonnull(strstr(out->str, "Excerpt of the row:"));
	g_assert_nonnull(strstr(out->str, "Outcome: 5 releases found"));

	/* A file, a changed page: what was wrong, and a status that says so. */
	g_string_truncate(out, 0);
	definition = g_build_filename(saved, "mine.yml", NULL);
	path = g_build_filename(saved, "page.html", NULL);
	g_assert_true(g_file_set_contents(definition, axis_indexer_shipped_find("axis-torrentproject")->body, -1, NULL));
	g_assert_true(g_file_set_contents(path, "<html><body><h1>New look</h1></body></html>", -1, NULL));
	options.definition = definition;
	options.page = path;
	options.excerpts = TRUE;
	status = axis_indexer_def_check_run(&options, out);
	g_assert_cmpint(status, ==, 1);
	g_assert_nonnull(strstr(out->str, "File: "));
	g_assert_nonnull(strstr(out->str, "Every answer has: #similarfiles: NOT FOUND"));
	g_assert_nonnull(strstr(out->str, "Verdict: its pages no longer look as expected"));
	g_assert_nonnull(strstr(out->str, "Excerpt of the page:"));
	g_assert_nonnull(strstr(out->str, "<h1>New look</h1>"));

	/* A release page: its download, read as at grab time. */
	g_string_truncate(out, 0);
	options.definition = "axis-torrentproject";
	options.page = NULL;
	options.words = NULL;
	options.release_page = FIXTURES "torrentproject-details.html";
	status = axis_indexer_def_check_run(&options, out);
	g_assert_cmpint(status, ==, 0);
	g_assert_nonnull(strstr(out->str, "Download: a magnet link of the info-hash "
		"c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8c8"));

	/* Review item 8: a saved release page larger than an answer may be is
	 * refused before it is read. */
	{
		g_autofree gchar *huge = g_strnfill(AXIS_CARDIGANN_ANSWER_BYTES + 1, ' ');

		g_string_truncate(out, 0);
		g_assert_true(g_file_set_contents(path, huge, -1, NULL));
		options.release_page = path;
		g_assert_cmpint(axis_indexer_def_check_run(&options, out), ==, 2);
		g_assert_nonnull(strstr(out->str, "The release page cannot be read: "));
		g_assert_nonnull(strstr(out->str, "is larger than 4096 KiB"));
	}

	/* What is not a definition, or not there. */
	g_string_truncate(out, 0);
	options.definition = "axis-nothing";
	options.release_page = NULL;
	g_assert_cmpint(axis_indexer_def_check_run(&options, out), ==, 2);
	g_assert_nonnull(strstr(out->str, "no built-in definition of that id"));
	g_unlink(definition);
	g_unlink(path);
	g_rmdir(saved);
}

/* 6.1: the daemon's own option, with no configuration, no database */
static void
test_check_option(void)
{
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	gint wait_status = 0;
	const gchar *argv[] = {
		AXISD_PATH, "--check-definition", "axis-therarbg", "--page", FIXTURES "therarbg-search.json", "--words",
		"made up film 2001", NULL
	};

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, &out, &err, &wait_status,
		NULL));
	g_assert_true(g_spawn_check_wait_status(wait_status, NULL));
	g_assert_nonnull(strstr(out, "Rows: results: matched 6\n"));
	/* andmatch, music and the game. */
	g_assert_nonnull(strstr(out, "3 kept"));
	g_assert_nonnull(strstr(out, "left out: its name lacks one of the words searched for (andmatch)"));
	g_assert_nonnull(strstr(out, "Outcome: 3 releases found"));
}

/* ── The fixtures are made up ───────────────────────────────────── */

/* Whether @hash (40 hexadecimal digits) is one of the made-up patterns the
 * fixtures use: a unit of at most 16 digits repeated ("5a5a5a...",
 * "0123456789abcdef0123..."), or ten runs of four of one digit
 * ("aaaa1111bbbb2222...").  An info-hash of a real torrent is neither. */
static gboolean
made_up_hash(const gchar *hash)
{
	gchar folded[40];
	guint period;
	guint i;

	for (i = 0; i < 40; i++)
		folded[i] = g_ascii_tolower(hash[i]);
	for (period = 1; period <= 16; period++) {
		for (i = period; i < 40 && folded[i] == folded[i - period]; i++)
			;
		if (i == 40)
			return TRUE;
	}
	for (i = 0; i < 40; i++) {
		if (folded[i] != folded[i - i % 4])
			return FALSE;
	}
	return TRUE;
}

/* Every run of exactly 40 hexadecimal digits in @path (a file, or a folder
 * read through), and one after a percent-encoded colon ("btih%3A..."),
 * that is not made up, added to @found. */
static void
scan_for_hashes(
	const gchar *path,
	GString     *found
){
	g_autofree gchar *data = NULL;
	gsize length = 0;
	gsize i = 0;

	if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
		g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
		const gchar *name;

		g_assert_nonnull(dir);
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree gchar *child = g_build_filename(path, name, NULL);

			scan_for_hashes(child, found);
		}
		return;
	}
	g_assert_true(g_file_get_contents(path, &data, &length, NULL));
	while (i < length) {
		gsize start = i;
		const gchar *run;

		while (i < length && g_ascii_isxdigit(data[i]))
			i++;
		run = data + start;
		if (i - start == 42 && run[0] == '3' && g_ascii_tolower(run[1]) == 'a')
			run += 2;
		else if (i - start != 40)
			run = NULL;
		if (run != NULL && !made_up_hash(run))
			g_string_append_printf(found, "%s: %.40s\n", path, run);
		if (i == start)
			i++;
	}
}

/* A fixture, a seed or a test of this change holds no info-hash of a real
 * torrent (40 hexadecimal digits identify one, and so its film): only the
 * made-up patterns made_up_hash() accepts.  tests/tools/scrub-fixture.py
 * writes those. */
static void
test_fixtures_made_up(void)
{
	static const gchar *const paths[] = {
		"tests/fixtures/builtin", "tests/fixtures/sites", "tests/fixtures/definitions",
		"tests/fuzz/corpus/fuzz-def-trace", "tests/fuzz/corpus/fuzz-indexer-sites", "tests/fuzz/fuzz-def-trace.dict",
		"tests/fuzz/fuzz-def-trace.c", "tests/test-indexer-builtin.c", "data/definitions", NULL
	};
	g_autoptr(GString) found = g_string_new(NULL);
	guint i;

	/* The rule itself: the patterns in use pass, a hash like a real one
	 * does not, nor one with its last digits overwritten (written in two
	 * pieces, so that this file holds no such run itself). */
	g_assert_true(made_up_hash("5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A"));
	g_assert_true(made_up_hash("0123456789abcdef0123456789ABCDEF01234567"));
	g_assert_true(made_up_hash("aaaa1111bbbb2222cccc3333dddd4444eeee5555"));
	g_assert_false(made_up_hash("d41c8a9b7e2f6013" "5a4c9e8b7d6f5a4c3b2e1d0f"));
	g_assert_false(made_up_hash("d41c8a9b7e2f6013" "5a4c9e8b7d6f5a4c3b2e0000"));
	g_assert_false(made_up_hash("a1b2c3d4e5f60718" "293a4b5c6d7e8f9012345678"));

	for (i = 0; paths[i] != NULL; i++)
		scan_for_hashes(paths[i], found);
	g_assert_cmpstr(found->str, ==, "");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/builtin/fixtures-made-up", test_fixtures_made_up);
	g_test_add_func("/builtin/shipped", test_shipped);
	g_test_add_func("/builtin/check-command", test_check_command);
	g_test_add_func("/builtin/check-option", test_check_option);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/builtin/" path, Fixture, NULL, func)
	ADD("rows-at-start", test_rows_at_start);
	ADD("limetorrents", test_limetorrents);
	ADD("torrentproject", test_torrentproject);
	ADD("therarbg", test_therarbg);
	ADD("therarbg-mirror-challenge", test_therarbg_mirror_challenge);
	ADD("challenge-header", test_challenge_header);
	ADD("site-asked-to-wait", test_site_asked_to_wait);
	ADD("torrentscsv", test_torrentscsv);
	ADD("identifier-only", test_identifier_only);
	ADD("drift", test_drift);
	ADD("overrides", test_overrides);
	ADD("override-from-the-data-directory", test_override_from_the_data_directory);
#undef ADD
	return g_test_run();
}
