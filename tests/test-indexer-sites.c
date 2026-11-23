/*
 * test-indexer-sites.c - the public torrent sites (YTS, EZTV, The Pirate Bay)
 * as native indexers: their requests, the mapping of their answers (missing,
 * wrong-typed, capped, hostile), the magnet links built from them, and the
 * kinds against stand-ins on loopback, on a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The stand-ins (tests/axis-test-sites.c) answer from hand-written fixtures
 * (tests/fixtures/sites and its README: written from the sites' documented
 * and public answer shapes; no site was asked).  No test reaches a real site,
 * Prowlarr or any other service.
 *
 * add-indexer-sites, spec scenarios (indexers): Movie from YTS; Site moved.
 * Tasks 1.1 to 1.4 (the pages' part is test-web-indexers), 2.1 to 2.4; 2.5 is
 * tests/fuzz/fuzz-indexer-sites.c.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-sites.h"
#include "download/axis-download-model.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-sites.h"
#include "indexer/axis-indexer-sites-parse.h"
#include "release/axis-release.h"

#define NOW_US (G_GINT64_CONSTANT(1700000000) * G_USEC_PER_SEC)

/* Hearth's torrents at YTS, lower-cased by the parser. */
#define H_720  "aaaa1111bbbb2222cccc3333dddd4444eeee5555"
#define H_1080 "aaaa1111bbbb2222cccc3333dddd4444eeee6666"
#define H_2160 "aaaa1111bbbb2222cccc3333dddd4444eeee7777"

/* ── Helpers ────────────────────────────────────────────────────── */

/* The value of parameter @name of a magnet link (first), decoded. */
static gchar *
magnet_param(
	const gchar *magnet,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("&%s=", name);
	const gchar *at = strstr(magnet, needle);
	const gchar *end;
	g_autofree gchar *raw = NULL;

	if (at == NULL)
		return NULL;
	at += strlen(needle);
	end = strchr(at, '&');
	raw = end != NULL ? g_strndup(at, (gsize)(end - at)) : g_strdup(at);
	return g_uri_unescape_string(raw, NULL);
}

static guint
count_of(
	const gchar *text,
	const gchar *needle
){
	guint n = 0;
	const gchar *at = text;

	while ((at = strstr(at, needle)) != NULL) {
		n++;
		at += strlen(needle);
	}
	return n;
}

static guint
n_trackers(void)
{
	const gchar *const *trackers = axis_sites_trackers();
	guint n;

	for (n = 0; trackers[n] != NULL; n++)
		;
	return n;
}

static void
request_for(
	AxisIndexerRequest   *request,
	AxisIndexerQueryKind  kind
){
	axis_indexer_request_init(request, kind);
	request->title = "Hearth";
	request->year = 1995;
	request->imdb_id = "tt0600001";
}

static const AxisIndexerFound *
found_at(
	const AxisIndexerOutcome *outcome,
	guint                     index
){
	g_assert_nonnull(outcome->items);
	g_assert_cmpuint(outcome->items->len, >, index);
	return g_ptr_array_index(outcome->items, index);
}

/* What every release of every site is, whatever the answer was. */
static void
assert_release_clean(const AxisIndexerFound *found)
{
	const AxisIndexerRelease *r = found->release;
	g_autofree gchar *hash = NULL;

	g_assert_nonnull(r->name);
	g_assert_true(g_utf8_validate(r->name, -1, NULL));
	g_assert_cmpuint(g_utf8_strlen(r->name, -1), <=, AXIS_INDEXER_NAME_MAX_CHARS);
	g_assert_cmpint(r->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_true(axis_download_hash_valid(r->info_hash));
	g_assert_null(found->fetch_path);
	g_assert_nonnull(found->magnet);
	g_assert_true(r->has_magnet);
	g_assert_true(axis_download_magnet_parse(found->magnet, &hash, NULL));
	g_assert_cmpstr(hash, ==, r->info_hash);
	g_assert_cmpuint(strlen(found->merge_key), ==, 64);
	g_assert_cmpint(r->size_bytes, >=, -1);
	g_assert_cmpint(r->seeders, >=, -1);
	g_assert_cmpint(r->leechers, >=, -1);
	g_assert_cmpint(r->published_us, >=, 0);
}

typedef gboolean (*ParseFunc)(const gchar *, gsize, gint64, GHashTable *, AxisIndexerOutcome *, AxisSitesRead *,
	gchar **);

/* Parses @text with @parse into a fresh @outcome; the problem in @problem. */
static gboolean
parse_text(
	ParseFunc            parse,
	const gchar         *text,
	AxisIndexerOutcome  *outcome,
	AxisSitesRead       *read,
	gchar              **problem
){
	axis_indexer_outcome_init(outcome);
	return parse(text, strlen(text), NOW_US, NULL, outcome, read, problem);
}

/* ── Requests ───────────────────────────────────────────────────── */

/* Task 1.1 to 1.3: what each site is asked */
static void
test_paths(void)
{
	AxisIndexerRequest request;
	g_autofree gchar *path = NULL;

	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	/* YTS: by the identifier when there is one, else by the title. */
	path = axis_sites_yts_path(&request, 1);
	g_assert_cmpstr(path, ==, "/api/v2/list_movies.json?query_term=tt0600001&limit=50&page=1");
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_clear_pointer(&path, g_free);
	request.imdb_id = NULL;
	request.title = "Hearth & Dust";
	path = axis_sites_yts_path(&request, 3);
	g_assert_cmpstr(path, ==, "/api/v2/list_movies.json?query_term=Hearth%20%26%20Dust&limit=50&page=3");
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_clear_pointer(&path, g_free);
	request.title = NULL;
	g_assert_null(axis_sites_yts_path(&request, 1));
	request_for(&request, AXIS_INDEXER_QUERY_EPISODE);
	g_assert_null(axis_sites_yts_path(&request, 1));
	request_for(&request, AXIS_INDEXER_QUERY_TEXT);
	request.imdb_id = NULL;
	path = axis_sites_yts_path(&request, 1);
	g_assert_cmpstr(path, ==, "/api/v2/list_movies.json?query_term=Hearth&limit=50&page=1");
	g_clear_pointer(&path, g_free);

	/* EZTV: the identifier as digits, without "tt" and leading zeros; never
	 * for a movie, never without one. */
	request_for(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.imdb_id = "tt0700001";
	request.season = 1;
	request.episode = 2;
	path = axis_sites_eztv_path(&request, 1);
	g_assert_cmpstr(path, ==, "/api/get-torrents?imdb_id=700001&limit=100&page=1");
	g_clear_pointer(&path, g_free);
	path = axis_sites_eztv_path(&request, 4);
	g_assert_cmpstr(path, ==, "/api/get-torrents?imdb_id=700001&limit=100&page=4");
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_clear_pointer(&path, g_free);
	request.kind = AXIS_INDEXER_QUERY_SEASON;
	path = axis_sites_eztv_path(&request, 1);
	g_assert_nonnull(path);
	g_clear_pointer(&path, g_free);
	request.imdb_id = "tt1234567890";
	path = axis_sites_eztv_path(&request, 1);
	g_assert_cmpstr(path, ==, "/api/get-torrents?imdb_id=1234567890&limit=100&page=1");
	g_clear_pointer(&path, g_free);
	request.imdb_id = "tt0000000";
	g_assert_null(axis_sites_eztv_path(&request, 1));
	request.imdb_id = NULL;
	g_assert_null(axis_sites_eztv_path(&request, 1));
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	g_assert_null(axis_sites_eztv_path(&request, 1));

	/* The Pirate Bay: words, in the categories of what is looked for (the
	 * UHD, DVD and 3D ones too); a movie by its identifier first. */
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	path = axis_sites_tpb_path(&request);
	g_assert_cmpstr(path, ==, "/q.php?q=Hearth%201995&cat=201%2C202%2C207%2C209%2C211");
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_clear_pointer(&path, g_free);
	path = axis_sites_tpb_imdb_path(&request);
	g_assert_cmpstr(path, ==, "/q.php?q=tt0600001&cat=201%2C202%2C207%2C209%2C211");
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_clear_pointer(&path, g_free);
	request.imdb_id = "tt12";
	g_assert_null(axis_sites_tpb_imdb_path(&request));
	request.imdb_id = NULL;
	g_assert_null(axis_sites_tpb_imdb_path(&request));
	request.kind = AXIS_INDEXER_QUERY_EPISODE;
	request.imdb_id = "tt0700001";
	request.title = "The Show";
	request.season = 1;
	request.episode = 2;
	/* Never an episode by its series' identifier: every episode would come. */
	g_assert_null(axis_sites_tpb_imdb_path(&request));
	path = axis_sites_tpb_path(&request);
	g_assert_cmpstr(path, ==, "/q.php?q=The%20Show%20S01E02&cat=205%2C208%2C212");
	g_clear_pointer(&path, g_free);
	request.kind = AXIS_INDEXER_QUERY_SEASON;
	path = axis_sites_tpb_path(&request);
	g_assert_cmpstr(path, ==, "/q.php?q=The%20Show%20S01&cat=205%2C208%2C212");
	g_clear_pointer(&path, g_free);
	request.kind = AXIS_INDEXER_QUERY_TEXT;
	request.title = "hearth & dust";
	path = axis_sites_tpb_path(&request);
	g_assert_cmpstr(path, ==, "/q.php?q=hearth%20%26%20dust&cat=200");
	g_clear_pointer(&path, g_free);
	request.title = NULL;
	g_assert_null(axis_sites_tpb_path(&request));
	/* A title is escaped whatever it holds. */
	request.title = "a/b?c=d&e#f%g+h\"i<j>";
	path = axis_sites_tpb_path(&request);
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_assert_nonnull(strstr(path, "q=a%2Fb%3Fc%3Dd%26e%23f%25g%2Bh%22i%3Cj%3E&cat=200"));
	g_clear_pointer(&path, g_free);
}

/* add-builtin-torrent-indexers 2.2: The Pirate Bay's categories as Newznab's;
 * what is plainly not video */
static void
test_tpb_categories(void)
{
	static const struct
	{
		const gchar *site;
		gint         newznab;
		gboolean     not_video;
	} cases[] = {
		{ "201", 2000, FALSE }, { "202", 2070, FALSE }, { "207", 2040, FALSE }, { "209", 2060, FALSE },
		{ "210", 2030, FALSE }, { "211", 2045, FALSE }, { "205", 5000, FALSE }, { "208", 5040, FALSE },
		{ "212", 5045, FALSE }, { "200", 2000, FALSE }, { "299", 2999, FALSE }, { "203", 3020, TRUE },
		{ "101", 3000, TRUE }, { "104", 3000, TRUE }, { "301", 4000, TRUE }, { "401", 1000, TRUE },
		{ "501", 6000, FALSE }, { "601", 7020, TRUE }, { "602", 7030, TRUE }, { "699", 8000, FALSE },
		{ "", 0, FALSE }, { "2O1", 0, FALSE }, { "2010", 0, FALSE }, { "-201", 0, FALSE }, { "701", 0, FALSE },
		{ NULL, 0, FALSE }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		gint category = axis_sites_tpb_category(cases[i].site);

		g_assert_cmpint(category, ==, cases[i].newznab);
		g_assert_true(axis_sites_category_not_video(category) == cases[i].not_video);
	}
	g_assert_false(axis_sites_category_not_video(8000));
	g_assert_false(axis_sites_category_not_video(0));
}

/* ── Magnet links ───────────────────────────────────────────────── */

/* Task 2.3: a link carries the hash and the name, names the trackers kept in
 * one place, and is one the downloads layer accepts */
static void
test_magnets(void)
{
	const gchar *const *trackers = axis_sites_trackers();
	g_autofree gchar *magnet = axis_sites_magnet(H_1080, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	g_autofree gchar *hash = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *long_name = NULL;
	g_autofree gchar *long_magnet = NULL;
	g_autofree gchar *hostile = NULL;
	g_autofree gchar *bare = NULL;
	guint n;
	guint i;

	g_assert_nonnull(magnet);
	g_assert_true(g_str_has_prefix(magnet, "magnet:?xt=urn:btih:" H_1080 "&dn=Hearth%20%281995%29%20%5B1080p%5D"));
	g_assert_true(axis_download_magnet_parse(magnet, &hash, NULL));
	g_assert_cmpstr(hash, ==, H_1080);
	name = magnet_param(magnet, "dn");
	g_assert_cmpstr(name, ==, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	/* The trackers: the list, in order, each once, escaped. */
	for (n = 0; trackers[n] != NULL; n++)
		;
	g_assert_cmpuint(n, >=, 4);
	g_assert_cmpuint(n, <=, 10);
	g_assert_cmpuint(count_of(magnet, "&tr="), ==, n);
	for (i = 0; i < n; i++) {
		g_autofree gchar *escaped = g_uri_escape_string(trackers[i], NULL, FALSE);
		g_autofree gchar *piece = g_strdup_printf("&tr=%s", escaped);

		g_assert_true(g_str_has_prefix(trackers[i], "udp://") || g_str_has_prefix(trackers[i], "http"));
		g_assert_nonnull(strstr(magnet, piece));
	}
	/* A name with everything in it is escaped into one parameter. */
	hostile = axis_sites_magnet(H_720, "A&B=C#D e%20f \xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\xac");
	g_assert_nonnull(hostile);
	g_assert_true(axis_download_magnet_parse(hostile, NULL, NULL));
	g_free(name);
	name = magnet_param(hostile, "dn");
	g_assert_cmpstr(name, ==, "A&B=C#D e%20f \xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\xac");
	/* No name, no dn; too long a name is cut, and the link still holds. */
	bare = axis_sites_magnet(H_720, NULL);
	g_assert_nonnull(bare);
	g_assert_null(strstr(bare, "&dn="));
	g_assert_true(axis_download_magnet_parse(bare, NULL, NULL));
	long_name = g_strnfill(300, 'x');
	long_magnet = axis_sites_magnet(H_720, long_name);
	g_assert_nonnull(long_magnet);
	g_assert_cmpuint(strlen(long_magnet), <=, AXIS_DOWNLOAD_MAGNET_MAX);
	g_assert_true(axis_download_magnet_parse(long_magnet, NULL, NULL));
	/* Only a hash is a hash. */
	g_assert_null(axis_sites_magnet("short", "x"));
	g_assert_null(axis_sites_magnet(NULL, "x"));
	g_assert_null(axis_sites_magnet("AAAA1111BBBB2222CCCC3333DDDD4444EEEE5555", "x"));
	g_assert_null(axis_sites_magnet("zzzz1111bbbb2222cccc3333dddd4444eeee5555", "x"));

	/* What a site says is a hash: 40 hex digits, lower-cased, not zeros. */
	g_clear_pointer(&hash, g_free);
	g_assert_true(axis_sites_hash_read("AAAA1111BBBB2222CCCC3333DDDD4444EEEE5555", &hash));
	g_assert_cmpstr(hash, ==, H_720);
	g_assert_false(axis_sites_hash_read("0000000000000000000000000000000000000000", NULL));
	g_assert_false(axis_sites_hash_read("AAAA1111BBBB2222CCCC3333DDDD4444EEEE555", NULL));
	g_assert_false(axis_sites_hash_read("AAAA1111BBBB2222CCCC3333DDDD4444EEEE55555", NULL));
	g_assert_false(axis_sites_hash_read("AAAA1111BBBB2222CCCC3333DDDD4444EEEE555G", NULL));
	g_assert_false(axis_sites_hash_read(" AAA1111BBBB2222CCCC3333DDDD4444EEEE5555", NULL));
	g_assert_false(axis_sites_hash_read("", NULL));
	g_assert_false(axis_sites_hash_read(NULL, NULL));
}

/* ── The answers ────────────────────────────────────────────────── */

/* Tasks 1.1, 2.2, 2.3: YTS's list becomes one release for each torrent, named
 * from its fields so that the release parser reads them */
static void
test_parse_yts(void)
{
	g_autofree gchar *text = axis_test_sites_fixture("yts-list.json");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *problem = NULL;
	const AxisIndexerFound *first;
	const AxisIndexerFound *web;
	g_autofree gchar *name = NULL;
	g_autofree gchar *dn = NULL;
	AxisRelease *parsed;
	guint i;

	g_assert_true(parse_text(axis_sites_parse_yts, text, &outcome, &read, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(outcome.items->len, ==, 4);
	g_assert_cmpuint(outcome.n_skipped, ==, 0);
	g_assert_cmpuint(read.n_entries, ==, 2);
	g_assert_cmpint(read.total, ==, 2);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	first = found_at(&outcome, 0);
	g_assert_cmpstr(first->release->name, ==, "Hearth (1995) [720p] [BluRay] [x264] [YTS]");
	g_assert_cmpstr(first->release->info_hash, ==, H_720);
	g_assert_cmpint(first->release->size_bytes, ==, 1181116006);
	g_assert_cmpint(first->release->seeders, ==, 120);
	g_assert_cmpint(first->release->leechers, ==, 10);
	g_assert_cmpint(first->release->published_us, ==, G_GINT64_CONSTANT(1577872800) * G_USEC_PER_SEC);
	g_assert_cmpstr(first->release->imdb_id, ==, "tt0600001");
	g_assert_cmpuint(first->release->n_categories, ==, 1);
	g_assert_cmpint(first->release->categories[0], ==, 2000);
	/* The link holds the hash and the name as built. */
	dn = magnet_param(first->magnet, "dn");
	g_assert_cmpstr(dn, ==, first->release->name);
	g_assert_cmpstr(found_at(&outcome, 1)->release->name, ==, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	g_assert_cmpstr(found_at(&outcome, 1)->release->info_hash, ==, H_1080);
	web = found_at(&outcome, 2);
	g_assert_cmpstr(web->release->name, ==, "Hearth (1995) [2160p] [WEB] [x265] [YTS]");
	g_assert_cmpstr(web->release->info_hash, ==, H_2160);
	g_assert_cmpstr(found_at(&outcome, 3)->release->name, ==, "Hearth (2011) [1080p] [BluRay] [x264] [YTS]");
	g_assert_cmpstr(found_at(&outcome, 3)->release->imdb_id, ==, "tt0500002");

	/* The release parser reads what was built: the title, the year, the
	 * resolution, the source and the codec. */
	parsed = axis_release_parse(first->release->name);
	g_assert_cmpstr(parsed->title, ==, "Hearth");
	g_assert_cmpint(parsed->year, ==, 1995);
	g_assert_cmpint(parsed->resolution, ==, AXIS_RELEASE_RESOLUTION_720P);
	g_assert_cmpint(parsed->source, ==, AXIS_RELEASE_SOURCE_BLURAY);
	g_assert_cmpint(parsed->codec, ==, AXIS_RELEASE_CODEC_H264);
	axis_release_free(parsed);
	parsed = axis_release_parse(web->release->name);
	g_assert_cmpstr(parsed->title, ==, "Hearth");
	g_assert_cmpint(parsed->year, ==, 1995);
	g_assert_cmpint(parsed->resolution, ==, AXIS_RELEASE_RESOLUTION_2160P);
	g_assert_cmpint(parsed->source, ==, AXIS_RELEASE_SOURCE_WEBDL);
	g_assert_cmpint(parsed->codec, ==, AXIS_RELEASE_CODEC_H265);
	axis_release_free(parsed);
	axis_indexer_outcome_clear(&outcome);

	/* Nothing found: no "movies", and no release. */
	g_free(text);
	text = axis_test_sites_fixture("yts-empty.json");
	g_assert_true(parse_text(axis_sites_parse_yts, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpint(read.total, ==, 0);
	axis_indexer_outcome_clear(&outcome);
}

/* The name from its fields alone */
static void
test_yts_names(void)
{
	g_autofree gchar *a = axis_sites_yts_name("Hearth", 1995, "1080p", "bluray", "x264");
	g_autofree gchar *b = axis_sites_yts_name("Hearth", 0, NULL, NULL, NULL);
	g_autofree gchar *c = axis_sites_yts_name("Hearth", 1995, "10 80p", "blu ray", "x 264");
	g_autofree gchar *d = axis_sites_yts_name("Hearth", 1995, "720p", "web", "h264");
	g_autofree gchar *e = axis_sites_yts_name("Hearth", 1995, "720p", "WEBRip", "x265");

	g_assert_cmpstr(a, ==, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	g_assert_cmpstr(b, ==, "Hearth [YTS]");
	g_assert_cmpstr(c, ==, "Hearth (1995) [YTS]");
	g_assert_cmpstr(d, ==, "Hearth (1995) [720p] [WEB] [h264] [YTS]");
	g_assert_cmpstr(e, ==, "Hearth (1995) [720p] [WEBRip] [x265] [YTS]");
}

/* Task 2.2: missing and wrong-typed fields, invalid hashes, a movie without
 * its torrents, an item that is no object */
static void
test_parse_yts_hostile(void)
{
	g_autofree gchar *text = axis_test_sites_fixture("yts-hostile.json");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *problem = NULL;
	const AxisIndexerFound *wrong;
	const AxisIndexerFound *long_only;
	guint i;

	g_assert_true(parse_text(axis_sites_parse_yts, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 2);
	/* The hash that is short, the one that is not hex, the one of zeros, and
	 * three torrents that are no objects; and the movie that is none. */
	g_assert_cmpuint(outcome.n_skipped, ==, 7);
	g_assert_cmpint(read.total, ==, -1);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	/* A year and a size that arrive as text are not numbers here; a quality
	 * that is not a word, a type that is a list, are left out of the name;
	 * what is out of range is not stated. */
	wrong = found_at(&outcome, 0);
	g_assert_cmpstr(wrong->release->name, ==, "Wrong Types [x264] [YTS]");
	g_assert_cmpstr(wrong->release->info_hash, ==, "cccc1111bbbb2222cccc3333dddd4444eeee5555");
	g_assert_cmpint(wrong->release->size_bytes, ==, -1);
	g_assert_cmpint(wrong->release->seeders, ==, -1);
	g_assert_cmpint(wrong->release->leechers, ==, -1);
	g_assert_cmpint(wrong->release->published_us, ==, 0);
	g_assert_null(wrong->release->imdb_id);
	/* The long title carries its year. */
	long_only = found_at(&outcome, 1);
	g_assert_cmpstr(long_only->release->name, ==, "Long Only (2005) [480p] [dvd] [xvid] [YTS]");
	g_assert_cmpstr(long_only->release->imdb_id, ==, "tt0123456");
	g_assert_cmpint(long_only->release->seeders, ==, 7);
	g_assert_cmpint(long_only->release->size_bytes, ==, 734003200);
	g_assert_cmpint(long_only->release->published_us, ==, G_GINT64_CONSTANT(1100000000) * G_USEC_PER_SEC);
	axis_indexer_outcome_clear(&outcome);
}

/* Task 2.2: an answer that is not the site's is refused, nothing of it taken */
static void
test_parse_refuses(void)
{
	static const struct
	{
		ParseFunc    parse;
		const gchar *text;
	} cases[] = {
		{ axis_sites_parse_yts, "" },
		{ axis_sites_parse_yts, "[]" },
		{ axis_sites_parse_yts, "\"ok\"" },
		{ axis_sites_parse_yts, "null" },
		{ axis_sites_parse_yts, "12" },
		{ axis_sites_parse_yts, "{}" },
		{ axis_sites_parse_yts, "{\"torrents\":[]}" },
		{ axis_sites_parse_yts, "{\"status\":\"error\",\"status_message\":\"no\",\"data\":[]}" },
		{ axis_sites_parse_yts, "<html><body>parked</body></html>" },
		{ axis_sites_parse_yts, "{\"status\":\"ok\"" },
		{ axis_sites_parse_eztv, "" },
		{ axis_sites_parse_eztv, "[]" },
		{ axis_sites_parse_eztv, "\"torrents\"" },
		{ axis_sites_parse_eztv, "{}" },
		{ axis_sites_parse_eztv, "{\"status\":\"ok\",\"data\":{}}" },
		{ axis_sites_parse_eztv, "<html></html>" },
		{ axis_sites_parse_eztv, "{\"torrents\":[" },
		{ axis_sites_parse_tpb, "" },
		{ axis_sites_parse_tpb, "{}" },
		{ axis_sites_parse_tpb, "{\"torrents\":[]}" },
		{ axis_sites_parse_tpb, "\"[]\"" },
		{ axis_sites_parse_tpb, "null" },
		{ axis_sites_parse_tpb, "<html></html>" },
		{ axis_sites_parse_tpb, "[{\"name\":" },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; cases[i].parse != NULL; i++) {
		AxisIndexerOutcome outcome;
		AxisSitesRead read;
		gchar *problem = NULL;

		if (parse_text(cases[i].parse, cases[i].text, &outcome, &read, &problem))
			g_error("accepted: %s", cases[i].text);
		g_assert_nonnull(problem);
		g_assert_true(g_utf8_validate(problem, -1, NULL));
		/* The reason holds none of what was given. */
		g_assert_null(strstr(problem, "parked"));
		g_assert_null(outcome.items);
		g_free(problem);
	}
	/* The same with no text at all. */
	{
		AxisIndexerOutcome outcome;
		gchar *problem = NULL;

		axis_indexer_outcome_init(&outcome);
		g_assert_false(axis_sites_parse_tpb(NULL, 0, NOW_US, NULL, &outcome, NULL, &problem));
		g_assert_nonnull(problem);
		g_free(problem);
	}
}

/* Tasks 1.2, 2.2, 2.3: EZTV's list: names from the title, else the file's
 * name; numbers that arrive as text */
static void
test_parse_eztv(void)
{
	g_autofree gchar *text = axis_test_sites_fixture("eztv-list.json");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *dn = NULL;
	const AxisIndexerFound *first;
	guint i;

	g_assert_true(parse_text(axis_sites_parse_eztv, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 4);
	g_assert_cmpuint(read.n_entries, ==, 4);
	g_assert_cmpint(read.total, ==, 4);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	first = found_at(&outcome, 0);
	g_assert_cmpstr(first->release->name, ==, "Cracking Slate S01E01 720p HDTV x264-GRP EZTV");
	g_assert_cmpstr(first->release->info_hash, ==, "e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1e1");
	g_assert_cmpint(first->release->size_bytes, ==, 912345678);
	g_assert_cmpint(first->release->seeders, ==, 88);
	g_assert_cmpint(first->release->leechers, ==, 12);
	g_assert_cmpint(first->release->published_us, ==, G_GINT64_CONSTANT(1200000000) * G_USEC_PER_SEC);
	g_assert_cmpstr(first->release->imdb_id, ==, "tt0700001");
	g_assert_cmpint(first->release->categories[0], ==, 5000);
	/* Without a title: the file's name, without its extension. */
	g_assert_cmpstr(found_at(&outcome, 2)->release->name, ==, "Cracking.Slate.S01E03.1080p.WEB.h264-GRP[eztv]");
	g_assert_cmpstr(found_at(&outcome, 3)->release->name, ==, "Cracking Slate S01E04");
	g_assert_cmpint(found_at(&outcome, 3)->release->size_bytes, ==, 0);
	/* The magnet link is built here, not taken from the one the site gives. */
	dn = magnet_param(first->magnet, "dn");
	g_assert_cmpstr(dn, ==, first->release->name);
	g_assert_null(strstr(first->magnet, "zoink"));
	g_assert_null(strstr(first->magnet, "S01E01.720p"));
	axis_indexer_outcome_clear(&outcome);

	g_free(text);
	text = axis_test_sites_fixture("eztv-empty.json");
	g_assert_true(parse_text(axis_sites_parse_eztv, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(read.n_entries, ==, 0);
	g_assert_cmpint(read.total, ==, 0);
	axis_indexer_outcome_clear(&outcome);
}

static void
test_parse_eztv_hostile(void)
{
	g_autofree gchar *text = axis_test_sites_fixture("eztv-hostile.json");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *problem = NULL;
	const AxisIndexerFound *odd;
	const AxisIndexerFound *plain;
	guint i;

	g_assert_true(parse_text(axis_sites_parse_eztv, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 2);
	/* The hash that is short, the one with no title or file, none at all, and
	 * four that are no objects; the same hash twice is one. */
	g_assert_cmpuint(outcome.n_skipped, ==, 7);
	g_assert_cmpint(read.total, ==, -1);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	odd = found_at(&outcome, 0);
	g_assert_cmpstr(odd->release->name, ==, "Odd Values S01E01");
	g_assert_cmpint(odd->release->seeders, ==, -1);
	g_assert_cmpint(odd->release->leechers, ==, -1);
	g_assert_cmpint(odd->release->size_bytes, ==, -1);
	g_assert_cmpint(odd->release->published_us, ==, G_GINT64_CONSTANT(1200000000) * G_USEC_PER_SEC);
	g_assert_cmpstr(odd->release->imdb_id, ==, "tt0700001");
	plain = found_at(&outcome, 1);
	g_assert_cmpstr(plain->release->name, ==, "No.Title.S02E02");
	g_assert_null(plain->release->imdb_id);
	g_assert_cmpint(plain->release->seeders, ==, 5);
	g_assert_cmpint(plain->release->size_bytes, ==, 1500);
	/* A date before there were any is not one. */
	g_assert_cmpint(plain->release->published_us, ==, 0);
	axis_indexer_outcome_clear(&outcome);
}

/* Tasks 1.3, 2.2: apibay's numbers are text; its "no results" is one item of
 * zeros, which is empty; what an uploader typed is not believed */
static void
test_parse_tpb(void)
{
	g_autofree gchar *text = axis_test_sites_fixture("tpb-list.json");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *problem = NULL;
	const AxisIndexerFound *first;
	guint i;

	g_assert_true(parse_text(axis_sites_parse_tpb, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 3);
	g_assert_cmpuint(outcome.n_skipped, ==, 0);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	first = found_at(&outcome, 0);
	g_assert_cmpstr(first->release->name, ==, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_cmpstr(first->release->info_hash, ==, "3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a");
	g_assert_cmpint(first->release->size_bytes, ==, G_GINT64_CONSTANT(8912345678));
	g_assert_cmpint(first->release->seeders, ==, 61);
	g_assert_cmpint(first->release->leechers, ==, 4);
	g_assert_cmpint(first->release->published_us, ==, G_GINT64_CONSTANT(1600000000) * G_USEC_PER_SEC);
	/* Its IMDb identifier (one of them wrong, as they can be) is nothing to
	 * go by; its category says movie or TV, its uploader's status a flag. */
	g_assert_null(first->release->imdb_id);
	g_assert_cmpuint(first->release->n_categories, ==, 1);
	g_assert_cmpint(first->release->categories[0], ==, 2040);
	g_assert_nonnull(first->release->flags);
	g_assert_cmpstr(first->release->flags[0], ==, "VIP");
	g_assert_null(first->release->flags[1]);
	g_assert_null(found_at(&outcome, 1)->release->imdb_id);
	g_assert_cmpint(found_at(&outcome, 1)->release->categories[0], ==, 2000);
	g_assert_null(found_at(&outcome, 1)->release->flags);
	g_assert_cmpstr(found_at(&outcome, 2)->release->info_hash, ==, "3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c");
	g_assert_cmpstr(found_at(&outcome, 2)->release->flags[0], ==, "Trusted");
	g_assert_cmpuint(read.n_not_video, ==, 0);
	axis_indexer_outcome_clear(&outcome);

	/* Results in categories that are not video are left out and counted. */
	g_free(text);
	text = axis_test_sites_fixture("tpb-categories.json");
	g_assert_true(parse_text(axis_sites_parse_tpb, text, &outcome, &read, &problem));
	g_assert_cmpuint(read.n_entries, ==, 8);
	g_assert_cmpuint(read.n_not_video, ==, 4);
	g_assert_cmpuint(outcome.n_skipped, ==, 0);
	g_assert_cmpuint(outcome.items->len, ==, 4);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	g_assert_cmpstr(found_at(&outcome, 0)->release->name, ==, "Made Up Film 2001 2160p UHD BluRay x265-NOGRP");
	g_assert_cmpint(found_at(&outcome, 0)->release->categories[0], ==, 2045);
	g_assert_cmpint(found_at(&outcome, 1)->release->categories[0], ==, 5045);
	/* Adult is video; a category that is not one of the site's is none. */
	g_assert_cmpint(found_at(&outcome, 2)->release->categories[0], ==, 6000);
	g_assert_cmpuint(found_at(&outcome, 3)->release->n_categories, ==, 0);
	axis_indexer_outcome_clear(&outcome);

	/* Nothing found: one item whose name says so and whose hash is zeros. */
	g_free(text);
	text = axis_test_sites_fixture("tpb-none.json");
	g_assert_true(parse_text(axis_sites_parse_tpb, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(outcome.n_skipped, ==, 0);
	g_assert_cmpuint(outcome.n_unusable, ==, 0);
	axis_indexer_outcome_clear(&outcome);
	/* A list that is empty is empty too. */
	g_assert_true(parse_text(axis_sites_parse_tpb, "[]", &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 0);
	axis_indexer_outcome_clear(&outcome);
}

static void
test_parse_tpb_hostile(void)
{
	g_autofree gchar *text = axis_test_sites_fixture("tpb-hostile.json");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *problem = NULL;
	const AxisIndexerFound *text_numbers;
	const AxisIndexerFound *numbers;
	guint i;

	g_assert_true(parse_text(axis_sites_parse_tpb, text, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(outcome.n_skipped, ==, 8);
	for (i = 0; i < outcome.items->len; i++)
		assert_release_clean(found_at(&outcome, i));
	text_numbers = found_at(&outcome, 0);
	g_assert_cmpstr(text_numbers->release->name, ==, "Numbers As Text");
	g_assert_cmpint(text_numbers->release->leechers, ==, -1);
	g_assert_cmpint(text_numbers->release->seeders, ==, -1);
	g_assert_cmpint(text_numbers->release->size_bytes, ==, 1000);
	g_assert_cmpint(text_numbers->release->published_us, ==, 0);
	g_assert_null(text_numbers->release->imdb_id);
	numbers = found_at(&outcome, 1);
	g_assert_cmpstr(numbers->release->name, ==, "Numbers As Numbers");
	g_assert_cmpint(numbers->release->seeders, ==, 6);
	g_assert_cmpint(numbers->release->leechers, ==, 5);
	g_assert_cmpint(numbers->release->size_bytes, ==, 7000);
	axis_indexer_outcome_clear(&outcome);
}

/* Task 2.2: lists are capped, a list of nothing but junk ends, a name is cut */
static void
test_parse_caps(void)
{
	GString *text = g_string_new("[");
	AxisIndexerOutcome outcome;
	AxisSitesRead read;
	g_autofree gchar *name = g_strnfill(2000, 'N');
	g_autofree gchar *wide = g_strdup("\xc3\xa9");
	g_autofree gchar *problem = NULL;
	guint i;

	/* A thousand and more releases: a thousand are kept. */
	for (i = 1; i <= AXIS_INDEXER_MAX_RESULTS + 50; i++)
		g_string_append_printf(text, "%s{\"name\":\"Release %u\",\"info_hash\":\"%040x\"}", i > 1 ? "," : "", i, i);
	g_string_append_c(text, ']');
	g_assert_true(parse_text(axis_sites_parse_tpb, text->str, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, AXIS_INDEXER_MAX_RESULTS);
	g_assert_true(outcome.truncated);
	g_assert_cmpuint(read.n_entries, ==, AXIS_INDEXER_MAX_RESULTS + 50);
	axis_indexer_outcome_clear(&outcome);
	g_string_free(text, TRUE);

	/* Junk without end: the scan stops. */
	text = g_string_new("[");
	for (i = 0; i < AXIS_INDEXER_MAX_SCAN + 500; i++)
		g_string_append(text, i > 0 ? ",1" : "1");
	g_string_append_c(text, ']');
	g_assert_true(parse_text(axis_sites_parse_tpb, text->str, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(outcome.n_skipped, ==, AXIS_INDEXER_MAX_SCAN);
	g_assert_true(outcome.truncated);
	axis_indexer_outcome_clear(&outcome);
	g_string_free(text, TRUE);

	/* A name of 2000 characters is cut to the most; one that is not UTF-8 is
	 * text still. */
	text = g_string_new(NULL);
	g_string_append_printf(text, "[{\"name\":\"%s\",\"info_hash\":\"%040x\"},{\"name\":\"bad \xff\xfe bytes %s\","
		"\"info_hash\":\"%040x\"}]", name, 1, wide, 2);
	g_assert_true(parse_text(axis_sites_parse_tpb, text->str, &outcome, &read, &problem));
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_cmpuint(g_utf8_strlen(found_at(&outcome, 0)->release->name, -1), ==, AXIS_INDEXER_NAME_MAX_CHARS);
	g_assert_true(g_utf8_validate(found_at(&outcome, 1)->release->name, -1, NULL));
	assert_release_clean(found_at(&outcome, 0));
	assert_release_clean(found_at(&outcome, 1));
	axis_indexer_outcome_clear(&outcome);
	g_string_free(text, TRUE);

	/* A name built from long fields stays within a name's length. */
	{
		g_autofree gchar *long_title = g_strnfill(400, 'T');
		g_autofree gchar *yts = g_strdup_printf("{\"status\":\"ok\",\"data\":{\"movies\":[{\"title\":\"%s\",\"year\":1995,"
			"\"torrents\":[{\"hash\":\"%040x\",\"quality\":\"1234567890123456\",\"type\":\"1234567890123456\","
			"\"video_codec\":\"1234567890123456\"}]}]}}", long_title, 7);

		g_assert_true(parse_text(axis_sites_parse_yts, yts, &outcome, &read, &problem));
		g_assert_cmpuint(outcome.items->len, ==, 1);
		g_assert_cmpuint(g_utf8_strlen(found_at(&outcome, 0)->release->name, -1), <=, AXIS_INDEXER_NAME_MAX_CHARS);
		g_assert_true(g_str_has_suffix(found_at(&outcome, 0)->release->name, " [YTS]"));
		assert_release_clean(found_at(&outcome, 0));
		axis_indexer_outcome_clear(&outcome);
	}

	/* A hash seen in an earlier answer (EZTV's pages) is not listed again. */
	{
		GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		const gchar *one = "[{\"name\":\"One\",\"info_hash\":\"00AA00AA00AA00AA00AA00AA00AA00AA00AA00AA\"}]";

		axis_indexer_outcome_init(&outcome);
		g_assert_true(axis_sites_parse_tpb(one, strlen(one), NOW_US, seen, &outcome, NULL, NULL));
		g_assert_true(axis_sites_parse_tpb(one, strlen(one), NOW_US, seen, &outcome, NULL, NULL));
		g_assert_cmpuint(outcome.items->len, ==, 1);
		axis_indexer_outcome_clear(&outcome);
		g_hash_table_unref(seen);
	}
}

/* ── With a database ────────────────────────────────────────────── */

typedef struct
{
	AxisTestDb      *db;
	AxisDbPool      *pool;
	AxisSettings    *settings;
	AxisIndexers    *indexers;
	AxisTestSites   *yts;
	AxisTestSites   *eztv;
	AxisTestSites   *tpb;
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
	g_assert_no_error(error);
	/* Fast, unless the test is about the rate. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_SITE_RATE, "20", &error));
	g_assert_no_error(error);
	fx->indexers = axis_indexers_new(fx->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_newznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_torznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_yts_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_eztv_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_piratebay_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.wait_ms = 10000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	fx->yts = axis_test_sites_new(AXIS_TEST_SITE_YTS);
	fx->eztv = axis_test_sites_new(AXIS_TEST_SITE_EZTV);
	fx->tpb = axis_test_sites_new(AXIS_TEST_SITE_TPB);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_indexers_unref(fx->indexers);
	axis_test_sites_free(fx->yts);
	axis_test_sites_free(fx->eztv);
	axis_test_sites_free(fx->tpb);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

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

/* A site at @stand_in's address (or its kind's own when @stand_in is %NULL). */
static gint64
add_site(
	Fixture       *fx,
	const gchar   *kind,
	const gchar   *name,
	AxisTestSites *stand_in,
	gint           priority
){
	AxisIndexerSourceSpec spec;
	g_autofree gchar *base = stand_in != NULL ? axis_test_sites_base(stand_in) : NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = kind;
	spec.address = base;
	spec.enabled = TRUE;
	spec.priority = priority;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static AxisIndexerSearchResult *
search_ok(
	Fixture                  *fx,
	gint64                    source_id,
	const AxisIndexerRequest *request
){
	AxisIndexerSearchResult *result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_search(fx->indexers, source_id, request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
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
	return NULL;
}

static const AxisIndexerSearchPart *
part_named(
	const AxisIndexerSearchResult *result,
	const gchar                   *name
){
	guint i;

	for (i = 0; result->parts != NULL && i < result->parts->len; i++) {
		const AxisIndexerSearchPart *part = g_ptr_array_index(result->parts, i);

		if (g_strcmp0(part->name, name) == 0)
			return part;
	}
	return NULL;
}

/* A page of EZTV's list of @count torrents numbered from @first. */
static gchar *
eztv_page(
	guint first,
	guint count,
	guint total
){
	GString *text = g_string_new(NULL);
	guint i;

	g_string_append_printf(text, "{\"imdb_id\":\"700001\",\"torrents_count\":%u,\"limit\":100,\"page\":1,\"torrents\":[", total);
	for (i = 0; i < count; i++)
		g_string_append_printf(text, "%s{\"hash\":\"%040x\",\"title\":\"Show S01E%02u 720p\",\"imdb_id\":\"700001\","
			"\"seeds\":1,\"peers\":1,\"size_bytes\":\"1000\",\"date_released_unix\":1200000000}", i > 0 ? "," : "",
			first + i, (first + i) % 100);
	g_string_append(text, "]}");
	return g_string_free(text, FALSE);
}

/* Spec: Movie from YTS: its releases are returned with their info-hashes;
 * tasks 1.1, 1.4, 2.1, 2.3 */
static void
test_yts_a_movie(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_sites_base(fx->yts);
	g_autofree gchar *query = NULL;
	g_autofree gchar *dn = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;
	gint64 id = add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 0);
	guint before;

	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_true(source->native);
	g_assert_cmpint(source->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_null(source->state_message);
	g_assert_cmpstr(source->version, ==, "YTS's list of movies");
	g_assert_cmpstr(source->address, ==, base);
	g_assert_false(source->key_set);
	g_assert_cmpint(source->last_contact_us, >, 0);
	/* It reports itself, with the identifier it takes. */
	g_assert_cmpuint(source->indexers->len, ==, 1);
	g_assert_cmpuint(((AxisIndexerInfo *)g_ptr_array_index(source->indexers, 0))->movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
	g_assert_cmpuint(((AxisIndexerInfo *)g_ptr_array_index(source->indexers, 0))->tv_ids, ==, 0);
	g_assert_cmpint(source->caps.read_us, >, 0);
	g_assert_cmpuint(source->caps.modes, ==, AXIS_INDEXER_CAN_MOVIE);
	/* The test asked for the newest, once. */
	g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, 1);
	query = axis_test_sites_query(fx->yts, 0);
	g_assert_cmpstr(query, ==, "limit=1&page=1");

	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	result = search_ok(fx, id, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_cmpuint(result->releases->len, ==, 4);
	release = release_named(result, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	g_assert_nonnull(release);
	g_assert_cmpstr(release->info_hash, ==, H_1080);
	g_assert_cmpstr(release->indexer_name, ==, "YTS");
	g_assert_true(release->has_magnet);
	g_assert_cmpint(release->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpint(release->seeders, ==, 300);
	g_clear_pointer(&query, g_free);
	query = axis_test_sites_query(fx->yts, 1);
	g_assert_cmpstr(query, ==, "query_term=tt0600001&limit=50&page=1");

	/* A fetch is the stored link, and asks the site for nothing. */
	before = axis_test_sites_n_requests(fx->yts);
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, before);
	g_assert_null(fetched.file);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, H_1080);
	g_assert_true(axis_download_magnet_parse(fetched.magnet, NULL, NULL));
	dn = magnet_param(fetched.magnet, "dn");
	g_assert_cmpstr(dn, ==, release->name);
	g_assert_cmpuint(count_of(fetched.magnet, "&tr="), ==, n_trackers());
	axis_indexer_fetched_clear(&fetched);

	/* No key, no header, nothing but a GET of its own path. */
	g_assert_cmpuint(axis_test_sites_n_bad(fx->yts), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(axis_test_sites_server(fx->yts)));
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source_secret");
	assert_q(fx, "yts|torrent", "SELECT kind || '|' || protocol FROM indexer_source");
}

/* A movie without an identifier is asked for by its title, and a TV search
 * finds nothing at YTS without asking it */
static void
test_yts_by_title_and_not_for_tv(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) none = NULL;
	g_autofree gchar *query = NULL;
	AxisIndexerRequest request;
	gint64 id = add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 0);

	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	request.imdb_id = NULL;
	result = search_ok(fx, id, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpuint(result->releases->len, ==, 4);
	query = axis_test_sites_query(fx->yts, 0);
	g_assert_cmpstr(query, ==, "query_term=Hearth&limit=50&page=1");

	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.season = 1;
	request.episode = 2;
	request.imdb_id = "tt0700001";
	none = search_ok(fx, id, &request);
	g_assert_cmpuint(none->releases->len, ==, 0);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, 1);
}

/* Nothing found is an answer, not a failure */
static void
test_yts_nothing_found(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *empty = axis_test_sites_fixture("yts-empty.json");
	AxisIndexerRequest request;
	gint64 id = add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 0);

	axis_test_sites_set_body(fx->yts, NULL, 0, empty);
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 0);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
}

/* Task 1.2, 2.1: EZTV by the identifier, paged to its cap, names from the
 * answers; a search without an identifier says so and asks nothing */
static void
test_eztv_by_identifier_paged(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) three = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page1 = eztv_page(1, 100, 250);
	g_autofree gchar *page2 = eztv_page(101, 100, 250);
	g_autofree gchar *page3 = eztv_page(201, 50, 250);
	g_autofree gchar *query = NULL;
	g_autofree gchar *forever = eztv_page(1000, 100, 100000);
	AxisIndexerRequest request;
	AxisIndexerSearchResult *refused = NULL;
	gint64 id = add_site(fx, AXIS_INDEXER_EZTV_NAME, "EZTV", fx->eztv, 0);
	guint i;

	axis_test_sites_set_body(fx->eztv, NULL, 1, page1);
	axis_test_sites_set_body(fx->eztv, NULL, 2, page2);
	axis_test_sites_set_body(fx->eztv, NULL, 3, page3);
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "Show";
	request.imdb_id = "tt0700001";
	request.season = 1;
	request.episode = 2;
	result = search_ok(fx, id, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_cmpuint(result->releases->len, ==, 250);
	g_assert_false(result->truncated);
	/* Three pages, then it stopped: the last had fewer than a page. */
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 3);
	for (i = 0; i < 3; i++) {
		g_autofree gchar *expected = g_strdup_printf("imdb_id=700001&limit=100&page=%u", i + 1);
		g_autofree gchar *got = axis_test_sites_query(fx->eztv, i);

		g_assert_cmpstr(got, ==, expected);
	}
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 0))->indexer_name, ==, "EZTV");

	/* The cap: a site that never ends is read five pages. */
	axis_test_sites_set_body(fx->eztv, NULL, 0, forever);
	axis_test_sites_set_body(fx->eztv, NULL, 1, forever);
	axis_test_sites_set_body(fx->eztv, NULL, 2, forever);
	axis_test_sites_set_body(fx->eztv, NULL, 3, forever);
	request.season = 2;
	three = search_ok(fx, id, &request);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 3 + AXIS_SITES_EZTV_MAX_PAGES);
	g_assert_true(three->truncated);
	/* The same hashes on every page are listed once. */
	g_assert_cmpuint(three->releases->len, ==, 100);

	/* A search with no IMDb identifier asks nothing and says why. */
	request.imdb_id = NULL;
	request.tvdb_id = 82002;
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &refused, &error));
	g_assert_null(refused);
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "IMDb identifier only"));
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 3 + AXIS_SITES_EZTV_MAX_PAGES);
	/* That is the search's, not the site's: it is not shown as failing. */
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	/* A movie is none of EZTV's. */
	g_clear_error(&error);
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	{
		g_autoptr(AxisIndexerSearchResult) film = search_ok(fx, id, &request);

		g_assert_cmpuint(film->releases->len, ==, 0);
	}
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 3 + AXIS_SITES_EZTV_MAX_PAGES);
	g_assert_cmpuint(axis_test_sites_n_bad(fx->eztv), ==, 0);
}

/* A page after the first that cannot be had ends the list there */
static void
test_eztv_a_later_page_fails(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *page1 = eztv_page(1, 100, 400);
	AxisIndexerRequest request;
	gint64 id = add_site(fx, AXIS_INDEXER_EZTV_NAME, "EZTV", fx->eztv, 0);

	axis_test_sites_set_body(fx->eztv, NULL, 1, page1);
	axis_test_sites_set_body(fx->eztv, NULL, 2, "<html>sorry</html>");
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_SEASON);
	request.title = "Show";
	request.imdb_id = "tt0700001";
	request.season = 1;
	result = search_ok(fx, id, &request);
	g_assert_cmpuint(result->releases->len, ==, 100);
	g_assert_true(result->truncated);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 2);
}

/* Tasks 1.3, 2.1: The Pirate Bay by words; "no results" is empty; a fetch is
 * the link */
static void
test_tpb_by_words(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) none = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *nothing = axis_test_sites_fixture("tpb-none.json");
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *release;
	gint64 id = add_site(fx, AXIS_INDEXER_PIRATEBAY_NAME, "The Pirate Bay", fx->tpb, 0);

	/* A movie with its identifier: asked by it, once, when it finds some. */
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	result = search_ok(fx, id, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_false(result->fell_back);
	g_assert_cmpuint(result->releases->len, ==, 3);
	query = axis_test_sites_query(fx->tpb, 0);
	g_assert_cmpstr(query, ==, "q=tt0600001&cat=201%2C202%2C207%2C209%2C211");
	g_assert_cmpuint(axis_test_sites_n_requests(fx->tpb), ==, 1);
	release = release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_nonnull(release);
	g_assert_cmpstr(release->info_hash, ==, "3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a3a");
	g_assert_cmpstr(release->indexer_name, ==, "The Pirate Bay");
	g_assert_null(release->imdb_id);
	/* Its category and its uploader's flag are kept with it. */
	g_assert_cmpuint(release->n_categories, ==, 1);
	g_assert_cmpint(release->categories[0], ==, 2040);
	g_assert_nonnull(release->flags);
	g_assert_cmpstr(release->flags[0], ==, "VIP");
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpstr(fetched.info_hash, ==, release->info_hash);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->tpb), ==, 1);
	axis_indexer_fetched_clear(&fetched);

	/* The identifier finds nothing (an upload names none): the title and
	 * year are asked, and the result says it fell back. */
	axis_test_sites_set_body(fx->tpb, "tt0500003", 0, nothing);
	request.imdb_id = "tt0500003";
	{
		g_autoptr(AxisIndexerSearchResult) words = search_ok(fx, id, &request);
		g_autofree gchar *first = axis_test_sites_query(fx->tpb, 1);
		g_autofree gchar *second = axis_test_sites_query(fx->tpb, 2);

		g_assert_cmpint(words->mode, ==, AXIS_INDEXER_BY_TEXT);
		g_assert_true(words->fell_back);
		g_assert_cmpuint(words->releases->len, ==, 3);
		g_assert_cmpstr(first, ==, "q=tt0500003&cat=201%2C202%2C207%2C209%2C211");
		g_assert_cmpstr(second, ==, "q=Hearth%201995&cat=201%2C202%2C207%2C209%2C211");
	}

	/* Words alone, when there is no identifier. */
	axis_test_sites_set_body(fx->tpb, NULL, 0, nothing);
	request.imdb_id = NULL;
	request.year = 1996;
	none = search_ok(fx, id, &request);
	g_assert_cmpuint(none->releases->len, ==, 0);
	g_assert_false(none->fell_back);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	g_assert_cmpuint(axis_test_sites_n_bad(fx->tpb), ==, 0);
}

/* Spec: Site moved; task 2.4: a site that has gone from its address, answers
 * with an error or with another business's page is shown as failing and
 * named so, and says what to do; the others are asked as before */
static void
test_a_site_that_moved_is_failing_and_the_others_are_not(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		AxisTestSitesMode mode;
		const gchar      *state;
		const gchar      *says;
	} cases[] = {
		{ AXIS_TEST_SITES_GONE, "failing", "change its address" },
		{ AXIS_TEST_SITES_FORBIDDEN, "failing", "change its address" },
		{ AXIS_TEST_SITES_MOVED, "failing", "redirects to another address" },
		{ AXIS_TEST_SITES_PARKED, "failing", "cannot be read" },
		{ AXIS_TEST_SITES_BROKEN, "failing", NULL },
		{ AXIS_TEST_SITES_DOWN, "unreachable", NULL }
	};
	AxisIndexerTuning tuning;
	guint i;

	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	tuning.wait_ms = 10000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(AxisIndexerSearchResult) result = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *sql = NULL;
		const AxisIndexerSearchPart *down;
		const AxisIndexerSearchPart *up;
		AxisIndexerRequest request;
		AxisTestSites *good = axis_test_sites_new(AXIS_TEST_SITE_TPB);
		AxisTestSites *bad = axis_test_sites_new(AXIS_TEST_SITE_YTS);
		gint64 bad_id;

		axis_test_db_exec(fx->db, "DELETE FROM indexer_source");
		add_site(fx, AXIS_INDEXER_PIRATEBAY_NAME, "The Pirate Bay", good, 0);
		bad_id = add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", bad, 0);
		axis_test_sites_set_mode(bad, cases[i].mode);
		/* The test says so... */
		g_assert_true(axis_indexers_test_source(fx->indexers, bad_id, NULL, &error));
		sql = g_strdup_printf("SELECT state FROM indexer_source WHERE id = %" G_GINT64_FORMAT, bad_id);
		assert_q(fx, cases[i].state, sql);
		if (cases[i].says != NULL) {
			g_autofree gchar *message_sql = g_strdup_printf("SELECT state_message FROM indexer_source WHERE id = %"
				G_GINT64_FORMAT, bad_id);
			g_autofree gchar *message = axis_test_db_query(fx->db, message_sql);

			g_assert_nonnull(message);
			if (strstr(message, cases[i].says) == NULL)
				g_error("%s does not say '%s'", message, cases[i].says);
			g_assert_null(strstr(message, "?"));
		}
		/* ...and so does a search of them all, which still has the other's. */
		request_for(&request, AXIS_INDEXER_QUERY_FILM);
		result = search_ok(fx, 0, &request);
		down = part_named(result, "YTS");
		up = part_named(result, "The Pirate Bay");
		g_assert_nonnull(down);
		g_assert_nonnull(up);
		g_assert_cmpuint(result->releases->len, ==, 3);
		g_assert_null(up->problem);
		g_assert_cmpuint(up->n_releases, ==, 3);
		/* The failing one is asked at most once more (it may be held). */
		g_assert_true(down->problem != NULL || down->n_releases == 0);
		assert_q(fx, "ok", "SELECT state FROM indexer_source WHERE name = 'The Pirate Bay'");
		axis_test_sites_free(good);
		axis_test_sites_free(bad);
	}
}

/* Task 2.4 and the rate: after a failure the site is left alone for a while,
 * and a site is asked one request in two seconds by default */
static void
test_back_off_and_the_default_rate(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) first = NULL;
	g_autoptr(AxisIndexerSearchResult) second = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) times = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *none = NULL;
	AxisIndexerTuning tuning;
	gint64 id = add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 0);

	/* The default: half a request a second. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_SITE_RATE, "0.50", &error));
	g_assert_no_error(error);
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	first = search_ok(fx, id, &request);
	request.imdb_id = NULL;
	second = search_ok(fx, id, &request);
	times = axis_test_plex_request_times(axis_test_sites_server(fx->yts));
	g_assert_cmpuint(times->len, ==, 2);
	g_assert_cmpint(g_array_index(times, gint64, 1) - g_array_index(times, gint64, 0), >=, G_GINT64_CONSTANT(1900000));

	/* Failures: after the second, the site is left alone for a while (the
	 * limiter's back-off), and the search says until when without asking. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.wait_ms = 4000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	axis_test_sites_set_mode(fx->yts, AXIS_TEST_SITES_BROKEN);
	request.year = 1996;
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &none, &error));
	g_assert_null(none);
	g_clear_error(&error);
	request.year = 1997;
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &none, &error));
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, 4);
	request.year = 1998;
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &none, &error));
	g_assert_nonnull(strstr(error->message, "not asked again until"));
	g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, 4);
	assert_q(fx, "unreachable", "SELECT state FROM indexer_source");
}

/* A release found at two sites by its info-hash is listed once, the other's
 * name kept as its alternative */
static void
test_the_same_hash_at_two_sites(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	const AxisIndexerRelease *at_yts;
	const gchar *body = "[{\"name\":\"Hearth 1995 1080p BluRay x264 YTS\",\"info_hash\":\"AAAA1111BBBB2222CCCC3333DDDD4444EEEE6666\","
		"\"seeders\":\"9\",\"leechers\":\"1\",\"size\":\"2576980378\",\"added\":\"1600000000\"},"
		"{\"name\":\"Hearth 1995 Other\",\"info_hash\":\"9999999999999999999999999999999999999999\",\"seeders\":\"1\"}]";

	add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 10);
	add_site(fx, AXIS_INDEXER_PIRATEBAY_NAME, "The Pirate Bay", fx->tpb, 20);
	axis_test_sites_set_body(fx->tpb, NULL, 0, body);
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	result = search_ok(fx, 0, &request);
	/* YTS's four and The Pirate Bay's other one. */
	g_assert_cmpuint(result->releases->len, ==, 5);
	g_assert_cmpuint(result->n_merged, ==, 1);
	at_yts = release_named(result, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	g_assert_nonnull(at_yts);
	g_assert_cmpuint(at_yts->n_alternatives, ==, 1);
	g_assert_nonnull(at_yts->also_at);
	g_assert_cmpstr(at_yts->also_at[0], ==, "The Pirate Bay");
	g_assert_true(axis_indexers_fetch(fx->indexers, at_yts->id, NULL, &fetched, &error));
	g_assert_cmpstr(fetched.info_hash, ==, H_1080);
	axis_indexer_fetched_clear(&fetched);
}

/* A search of all three: each says what it did, and one that cannot answer
 * the kind of search says so without failing */
static void
test_a_search_of_all_three(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) tv = NULL;
	g_autoptr(AxisIndexerSearchResult) film = NULL;
	AxisIndexerRequest request;
	const AxisIndexerSearchPart *part;
	gint64 eztv_id;

	add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 10);
	eztv_id = add_site(fx, AXIS_INDEXER_EZTV_NAME, "EZTV", fx->eztv, 20);
	add_site(fx, AXIS_INDEXER_PIRATEBAY_NAME, "The Pirate Bay", fx->tpb, 30);
	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	film = search_ok(fx, 0, &request);
	g_assert_cmpuint(film->parts->len, ==, 3);
	g_assert_cmpuint(part_named(film, "YTS")->n_releases, ==, 4);
	g_assert_cmpuint(part_named(film, "EZTV")->n_releases, ==, 0);
	g_assert_null(part_named(film, "EZTV")->problem);
	/* Nothing was asked of EZTV: it is shown as not asked, not as an answer. */
	g_assert_false(part_named(film, "EZTV")->asked);
	g_assert_cmpstr(part_named(film, "EZTV")->not_asked, ==, "it has no movies");
	g_assert_true(part_named(film, "YTS")->asked);
	g_assert_null(part_named(film, "YTS")->not_asked);
	g_assert_cmpuint(part_named(film, "The Pirate Bay")->n_releases, ==, 3);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 0);
	{
		g_autoptr(AxisIndexerSearchResult) again = search_ok(fx, 0, &request);
		g_autoptr(AxisIndexerSearchResult) alone = search_ok(fx, eztv_id, &request);

		/* Never stored, so never read back as an answer. */
		g_assert_cmpstr(part_named(again, "EZTV")->not_asked, ==, "it has no movies");
		g_assert_false(part_named(again, "EZTV")->cached);
		g_assert_cmpstr(alone->not_asked, ==, "it has no movies");
		g_assert_cmpuint(alone->releases->len, ==, 0);
		g_assert_false(alone->cached);
		assert_q(fx, "0", "SELECT count(*) FROM indexer_search s JOIN indexer_source o ON o.id = s.source_id WHERE "
			"o.name = 'EZTV'");
	}

	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "Show";
	request.season = 1;
	request.episode = 2;
	tv = search_ok(fx, 0, &request);
	/* No identifier: EZTV says so, as its own problem; YTS finds nothing. */
	part = part_named(tv, "EZTV");
	g_assert_nonnull(part->problem);
	g_assert_nonnull(strstr(part->problem, "IMDb identifier only"));
	g_assert_cmpuint(part_named(tv, "YTS")->n_releases, ==, 0);
	g_assert_null(part_named(tv, "YTS")->problem);
	g_assert_false(part_named(tv, "YTS")->asked);
	g_assert_cmpstr(part_named(tv, "YTS")->not_asked, ==, "it has no TV");
	g_assert_cmpuint(part_named(tv, "The Pirate Bay")->n_releases, ==, 3);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, 1);
	g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 0);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source WHERE state IN ('failing', 'unreachable', 'refused')");

	/* Words as typed (refine-indexer-followups 5.1): EZTV cannot be asked by
	 * words and finds nothing, quietly, as YTS does for TV; the others
	 * answer. */
	{
		g_autoptr(AxisIndexerSearchResult) words = NULL;
		g_autoptr(AxisIndexerSearchResult) alone = NULL;
		g_autoptr(GError) error = NULL;

		request_for(&request, AXIS_INDEXER_QUERY_TEXT);
		request.imdb_id = NULL;
		words = search_ok(fx, 0, &request);
		part = part_named(words, "EZTV");
		g_assert_false(part->asked);
		g_assert_null(part->problem);
		g_assert_cmpstr(part->not_asked, ==, "it cannot be searched by words");
		g_assert_cmpuint(part->n_releases, ==, 0);
		g_assert_null(part_named(words, "YTS")->problem);
		g_assert_null(part_named(words, "The Pirate Bay")->problem);
		g_assert_cmpuint(part_named(words, "The Pirate Bay")->n_releases, ==, 3);
		g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 0);
		/* Searched on its own, it says why. */
		g_assert_false(axis_indexers_search(fx->indexers, eztv_id, &request, NULL, &alone, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
		g_assert_nonnull(strstr(error->message, "IMDb identifier only"));
	}

	/* add-anime-support, A3: the words a search asks for an anime series (its
	 * name and an absolute number) are a series' words.  YTS, which has no
	 * TV, is not asked for them (words not so marked it answers, above); EZTV
	 * takes no words and is shown as not asked; The Pirate Bay answers.
	 * Spec (indexers delta): an indexer that takes no words is shown as not
	 * asked for that request. */
	{
		g_autoptr(AxisIndexerSearchResult) anime = NULL;
		guint yts_before = axis_test_sites_n_requests(fx->yts);

		request_for(&request, AXIS_INDEXER_QUERY_TEXT);
		request.imdb_id = NULL;
		request.anime = TRUE;
		anime = search_ok(fx, 0, &request);
		g_assert_false(part_named(anime, "YTS")->asked);
		g_assert_null(part_named(anime, "YTS")->problem);
		g_assert_cmpstr(part_named(anime, "YTS")->not_asked, ==, "it has no TV");
		g_assert_cmpuint(axis_test_sites_n_requests(fx->yts), ==, yts_before);
		part = part_named(anime, "EZTV");
		g_assert_false(part->asked);
		g_assert_null(part->problem);
		g_assert_cmpstr(part->not_asked, ==, "it cannot be searched by words");
		g_assert_cmpuint(axis_test_sites_n_requests(fx->eztv), ==, 0);
		g_assert_true(part_named(anime, "The Pirate Bay")->asked);
		g_assert_null(part_named(anime, "The Pirate Bay")->problem);
		g_assert_cmpuint(part_named(anime, "The Pirate Bay")->n_releases, ==, 3);
	}
}

/* Task 1.4: the address is a setting with a default; a site has no key */
static void
test_saving_a_site(Fixture *fx, gconstpointer data)
{
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autofree gchar *base = axis_test_sites_base(fx->yts);
	gint64 id = 0;

	/* No address typed: the kind's own. */
	memset(&spec, 0, sizeof(spec));
	spec.name = "YTS";
	spec.kind = AXIS_INDEXER_YTS_NAME;
	spec.enabled = TRUE;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	/* As an address is stored: with its port. */
	assert_q(fx, AXIS_INDEXER_YTS_ADDRESS ":443|yts|torrent", "SELECT address || '|' || kind || '|' || protocol FROM "
		"indexer_source");
	/* A key sent for it is not kept. */
	spec.name = "EZTV";
	spec.kind = AXIS_INDEXER_EZTV_NAME;
	spec.secret = "a-key-that-must-not-be-kept-1234";
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source_secret");
	assert_q(fx, AXIS_INDEXER_EZTV_ADDRESS ":443", "SELECT address FROM indexer_source WHERE name = 'EZTV'");
	spec.name = "The Pirate Bay";
	spec.kind = AXIS_INDEXER_PIRATEBAY_NAME;
	spec.secret = NULL;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	assert_q(fx, AXIS_INDEXER_PIRATEBAY_ADDRESS ":443", "SELECT address FROM indexer_source WHERE name = 'The Pirate "
		"Bay'");
	/* Changed: a site moved. */
	spec.address = base;
	g_assert_true(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpstr(source->address, ==, base);
	g_assert_cmpstr(source->state_message, ==, NULL);
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_UNCHECKED);
	/* A site is at its address, not under a path, nor has it files elsewhere. */
	spec.base_path = "/x";
	spec.files_origin = "https://files.example";
	g_assert_true(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	assert_q(fx, "|", "SELECT base_path || '|' || coalesce(files_origin, '') FROM indexer_source WHERE name = 'The Pirate Bay'");
	/* An address that is not one is refused, as for every kind. */
	spec.address = "yts.example";
	g_assert_false(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_clear_error(&error);
	/* The kinds say what they are. */
	g_assert_true(axis_indexers_find_kind(fx->indexers, AXIS_INDEXER_YTS_NAME)->site);
	g_assert_cmpstr(axis_indexers_find_kind(fx->indexers, AXIS_INDEXER_EZTV_NAME)->default_address, ==,
		AXIS_INDEXER_EZTV_ADDRESS);
	g_assert_false(axis_indexers_find_kind(fx->indexers, AXIS_INDEXER_NEWZNAB_NAME)->site);
}

/* fix-first-contact 4: when a kind's built-in address changes after an
 * indexer was added at the old one, nothing changes the stored address: it is
 * the indexer's own, saved again as it is, and listed as it is */
static void
test_a_stored_old_default_address_is_kept(Fixture *fx, gconstpointer data)
{
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	gint64 id = 0;

	/* Stored as the kind's default was at the time. */
	memset(&spec, 0, sizeof(spec));
	spec.name = "YTS";
	spec.kind = AXIS_INDEXER_YTS_NAME;
	spec.address = "https://old-default.yts.test";
	spec.enabled = TRUE;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	assert_q(fx, "https://old-default.yts.test:443", "SELECT address FROM indexer_source");

	/* Read, listed, and saved again with only its name changed: the address
	 * is still the stored one, not the kind's present default. */
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpstr(source->address, ==, "https://old-default.yts.test:443");
	g_assert_cmpstr(source->address, !=, AXIS_INDEXER_YTS_ADDRESS ":443");
	g_assert_cmpstr(AXIS_INDEXER_YTS_ADDRESS, !=, "https://old-default.yts.test");
	g_assert_true(axis_indexers_list_sources(fx->indexers, &sources, &error));
	g_assert_cmpuint(sources->len, ==, 1);
	g_assert_cmpstr(((const AxisIndexerSourceInfo *)g_ptr_array_index(sources, 0))->address, ==,
		"https://old-default.yts.test:443");
	spec.name = "YTS renamed";
	spec.address = source->address;
	g_assert_true(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	g_assert_no_error(error);
	assert_q(fx, "https://old-default.yts.test:443|YTS renamed", "SELECT address || '|' || name FROM indexer_source");
}

static gboolean
nothing(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	gchar                    **version,
	GError                   **error
){
	return TRUE;
}

static gboolean
nothing_listed(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	GPtrArray                **indexers,
	GError                   **error
){
	return TRUE;
}

static gboolean
nothing_searched(
	const AxisIndexerKindInfo *self,
	AxisIndexerConn           *conn,
	const AxisIndexerRequest  *request,
	AxisIndexerOutcome        *outcome,
	GError                   **error
){
	return TRUE;
}

static gboolean
nothing_fetched(
	const AxisIndexerKindInfo     *self,
	AxisIndexerConn               *conn,
	const AxisIndexerFetchRequest *request,
	AxisIndexerFetched            *fetched,
	GError                       **error
){
	return TRUE;
}

/* Only a public site's kind may have no key */
static void
test_only_a_site_may_have_no_key(Fixture *fx, gconstpointer data)
{
	static const AxisIndexerKindInfo keyless = {
		"keyless", "Keyless", "", NULL, NULL, nothing, nothing_listed, nothing_searched, nothing_fetched, NULL,
		TRUE, AXIS_DOWNLOAD_TORRENT, NULL, NULL, FALSE, NULL, NULL, FALSE
	};
	static const AxisIndexerKindInfo site_without_address = {
		"nowhere", "Nowhere", "", NULL, NULL, nothing, nothing_listed, nothing_searched, nothing_fetched, NULL,
		TRUE, AXIS_DOWNLOAD_TORRENT, NULL, NULL, TRUE, NULL, NULL, FALSE
	};
	static const AxisIndexerKindInfo site_with_key = {
		"withkey", "With key", "API key", NULL, NULL, nothing, nothing_listed, nothing_searched, nothing_fetched, NULL,
		TRUE, AXIS_DOWNLOAD_TORRENT, "apikey", NULL, TRUE, "https://a.example", NULL, FALSE
	};
	static const AxisIndexerKindInfo not_native = {
		"notnative", "Not native", "", NULL, NULL, nothing, nothing_listed, nothing_searched, nothing_fetched, NULL,
		FALSE, AXIS_DOWNLOAD_TORRENT, NULL, NULL, TRUE, "https://a.example", NULL, FALSE
	};
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_indexers_add_kind(fx->indexers, &keyless, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_indexers_add_kind(fx->indexers, &site_without_address, &error));
	g_clear_error(&error);
	g_assert_false(axis_indexers_add_kind(fx->indexers, &site_with_key, &error));
	g_clear_error(&error);
	g_assert_false(axis_indexers_add_kind(fx->indexers, &not_native, &error));
	g_clear_error(&error);
}

/* A site's answers are nowhere in a log or a stored message beyond what they
 * are: no address of a result, no query */
static void
test_nothing_of_a_request_is_logged(Fixture *fx, gconstpointer data)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	static const gchar *const needles[] = { "*Hearth*", "*tt0600001*", "*query_term*", "*0600001*", "*magnet:*", NULL };
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerSearchResult *none = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	gint64 id = add_site(fx, AXIS_INDEXER_YTS_NAME, "YTS", fx->yts, 0);
	guint i;
	guint j;

	request_for(&request, AXIS_INDEXER_QUERY_FILM);
	result = search_ok(fx, id, &request);
	g_assert_true(axis_indexers_fetch(fx->indexers, release_named(result, "Hearth (1995) [720p] [BluRay] [x264] [YTS]")->id,
		NULL, &fetched, &error));
	axis_indexer_fetched_clear(&fetched);
	axis_test_sites_set_mode(fx->yts, AXIS_TEST_SITES_BROKEN);
	request.year = 1996;
	request.title = "Hearth Again";
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &none, &error));
	g_assert_null(strstr(error->message, "query_term"));
	g_assert_null(strstr(error->message, "tt0600001"));
	g_assert_null(strstr(error->message, "Hearth Again"));
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source WHERE coalesce(state_message, '') LIKE '%query_term%'"
		" OR coalesce(state_message, '') LIKE '%tt0600001%' OR coalesce(state_message, '') LIKE '%Hearth%'");
	for (i = 0; needles[i] != NULL; i++) {
		for (j = 0; j < G_N_ELEMENTS(levels); j++) {
			if (axis_test_log_count(levels[j], needles[i]) > 0) {
				g_autofree gchar *line = axis_test_log_find(levels[j], needles[i]);

				g_error("a log line at level %d matches %s: %s", (gint)levels[j], needles[i], line);
			}
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sites/paths", test_paths);
	g_test_add_func("/sites/tpb-categories", test_tpb_categories);
	g_test_add_func("/sites/magnets", test_magnets);
	g_test_add_func("/sites/parse/yts", test_parse_yts);
	g_test_add_func("/sites/parse/yts-names", test_yts_names);
	g_test_add_func("/sites/parse/yts-hostile", test_parse_yts_hostile);
	g_test_add_func("/sites/parse/refuses", test_parse_refuses);
	g_test_add_func("/sites/parse/eztv", test_parse_eztv);
	g_test_add_func("/sites/parse/eztv-hostile", test_parse_eztv_hostile);
	g_test_add_func("/sites/parse/tpb", test_parse_tpb);
	g_test_add_func("/sites/parse/tpb-hostile", test_parse_tpb_hostile);
	g_test_add_func("/sites/parse/caps", test_parse_caps);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/sites/" path, Fixture, NULL, func)
	ADD("yts/movie", test_yts_a_movie);
	ADD("yts/title-not-tv", test_yts_by_title_and_not_for_tv);
	ADD("yts/nothing", test_yts_nothing_found);
	ADD("eztv/paged", test_eztv_by_identifier_paged);
	ADD("eztv/later-page", test_eztv_a_later_page_fails);
	ADD("tpb/words", test_tpb_by_words);
	ADD("moved", test_a_site_that_moved_is_failing_and_the_others_are_not);
	ADD("back-off-rate", test_back_off_and_the_default_rate);
	ADD("same-hash", test_the_same_hash_at_two_sites);
	ADD("all-three", test_a_search_of_all_three);
	ADD("save", test_saving_a_site);
	ADD("save/old-default-kept", test_a_stored_old_default_address_is_kept);
	ADD("no-key-kinds", test_only_a_site_may_have_no_key);
	ADD("log", test_nothing_of_a_request_is_logged);
#undef ADD
	return g_test_run();
}
