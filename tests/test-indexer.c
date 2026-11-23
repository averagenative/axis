/*
 * test-indexer.c - the indexer layer and its Prowlarr kind, against a fake
 * Prowlarr on loopback and a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two sets of answers (tests/fixtures/prowlarr and its README): recordings
 * of a real Prowlarr 3.0.1, scrubbed (the "recorded" cases and the parser's
 * "recorded" test), and constructed ones that hold what a recording does
 * not (junk, addresses elsewhere, failing indexers).  The stand-in
 * (tests/axis-test-prowlarr.c) behaves as the real one did: a bare 401,
 * link addresses that redirect to their magnet links, downloads of an
 * indexer with Redirect on that redirect to its site.  What is still only
 * believed is listed in the README.
 *
 * Spec scenarios (indexers): Search; Some indexers fail; Address elsewhere;
 * Redirect elsewhere; Not a release file; Key in a fetch address; Repeat;
 * Markup in a name (the layer returns it as text; the page test shows it
 * escaped).  Tasks 4.1 to 4.5.  fix-prowlarr-live: Link address; Wrong
 * torrent; Followed; Switched off; Inside the network; Mixed indexers
 * (tasks 5.1 to 5.5; the HTTP client's own rules are tests/test-http.c's).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-plex.h"
#include "axis-test-prowlarr.h"
#include "download/axis-download-bencode.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-model.h"
#include "indexer/axis-indexer-private.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "indexer/axis-indexer-prowlarr-parse.h"

#define KEY "prowlarr-api-key-7f3a9c21d84e5b06"
#define OTHER_KEY "not-the-key-91c0de2a77b4"

/* Indexers that all take an IMDb identifier: a search for a movie by it is
 * one request (none is asked by the title as well). */
#define ALL_TAKE_IMDB \
	"[{\"id\":3,\"name\":\"NewsOwl\",\"protocol\":\"usenet\",\"enable\":true," \
	"\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}}," \
	"{\"id\":5,\"name\":\"Tracker One\",\"protocol\":\"torrent\",\"enable\":true," \
	"\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}}]"

/* ── Helpers for the parser tests ───────────────────────────────── */

static gchar *
fixture_file(
	const gchar *name,
	const gchar *origin,
	const gchar *base
){
	g_autofree gchar *path = g_strconcat("tests/fixtures/prowlarr/", name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;
	gchar **parts;
	gchar *joined;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read the fixture %s: %s", path, error->message);
	parts = g_strsplit(text, "@ORIGIN@", -1);
	g_free(text);
	text = g_strjoinv(origin, parts);
	g_strfreev(parts);
	parts = g_strsplit(text, "@BASE@", -1);
	g_free(text);
	text = g_strjoinv(base, parts);
	g_strfreev(parts);
	parts = g_strsplit(text, "@KEY@", -1);
	g_free(text);
	joined = g_strjoinv(KEY, parts);
	g_strfreev(parts);
	return joined;
}

static AxisIndexerFound *
found_named(
	const AxisIndexerOutcome *outcome,
	const gchar              *name
){
	guint i;

	for (i = 0; i < outcome->items->len; i++) {
		AxisIndexerFound *found = g_ptr_array_index(outcome->items, i);

		if (g_strcmp0(found->release->name, name) == 0)
			return found;
	}
	return NULL;
}

static gint64
iso_us(
	gint year,
	gint month,
	gint day,
	gint hour,
	gint minute,
	gint second
){
	GDateTime *when = g_date_time_new_utc(year, month, day, hour, minute, second);
	gint64 us = g_date_time_to_unix(when) * G_USEC_PER_SEC;

	g_date_time_unref(when);
	return us;
}

/* ── The parser ─────────────────────────────────────────────────── */

/* Spec: Search (the mapping of Prowlarr's fields); tasks 4.1 */
static void
test_parse_search_mapping(void)
{
	g_autofree gchar *text = fixture_file("search-movie.json", "http://127.0.0.1:4242", "");
	AxisHttpOrigin origin;
	AxisIndexerOutcome outcome;
	AxisIndexerFound *found;
	AxisIndexerRelease *r;
	g_autofree gchar *problem = NULL;

	g_assert_true(axis_http_origin_parse("http://127.0.0.1:4242", &origin, NULL));
	g_assert_true(axis_prowlarr_parse_search(text, strlen(text), &origin, "", 0, &outcome, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(outcome.items->len, ==, 8);
	g_assert_cmpuint(outcome.n_off_origin, ==, 2);
	g_assert_cmpuint(outcome.n_unusable, ==, 1);
	g_assert_cmpuint(outcome.n_skipped, ==, 3);
	g_assert_false(outcome.truncated);

	/* A Usenet release, every field. */
	found = found_named(&outcome, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_nonnull(found);
	r = found->release;
	g_assert_cmpint(r->size_bytes, ==, G_GINT64_CONSTANT(8923441152));
	g_assert_cmpint(r->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_cmpint(r->indexer_id, ==, 3);
	g_assert_cmpstr(r->indexer_name, ==, "NewsOwl");
	g_assert_cmpint(r->published_us, ==, iso_us(2024, 3, 2, 10, 11, 12));
	g_assert_cmpint(r->seeders, ==, -1);
	g_assert_cmpint(r->leechers, ==, -1);
	g_assert_cmpint(r->grabs, ==, 342);
	g_assert_cmpuint(r->n_categories, ==, 1);
	g_assert_cmpint(r->categories[0], ==, 2040);
	g_assert_cmpstr(r->imdb_id, ==, "tt0600001");
	g_assert_cmpint(r->tmdb_id, ==, 6001);
	g_assert_cmpint(r->tvdb_id, ==, 0);
	g_assert_null(r->info_hash);
	g_assert_false(r->has_magnet);
	g_assert_cmpstr(found->fetch_path, ==, "/3/download?apikey=" KEY "&link=QUJDMTIz&file=hearth-nzb");
	g_assert_null(found->magnet);

	/* A torrent: peers, flags, categories without repeats, a hash in the
	 * case it was given in, a time with an offset, a magnet link. */
	found = found_named(&outcome, "Hearth.1995.2160p.UHD.BluRay.x265-TRK");
	g_assert_nonnull(found);
	r = found->release;
	g_assert_cmpint(r->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpint(r->seeders, ==, 85);
	g_assert_cmpint(r->leechers, ==, 4);
	g_assert_cmpint(r->published_us, ==, iso_us(2024, 10, 12, 20, 0, 0));
	g_assert_nonnull(r->flags);
	g_assert_cmpstr(r->flags[0], ==, "freeleech");
	g_assert_cmpstr(r->flags[1], ==, "internal");
	g_assert_null(r->flags[2]);
	g_assert_cmpuint(r->n_categories, ==, 2);
	g_assert_cmpint(r->categories[0], ==, 2045);
	g_assert_cmpint(r->categories[1], ==, 2000);
	g_assert_cmpstr(r->info_hash, ==, "0123456789abcdef0123456789abcdef01234567");
	g_assert_true(r->has_magnet);
	g_assert_true(g_str_has_prefix(found->magnet, "magnet:?xt=urn:btih:0123456789abcdef"));
	g_assert_nonnull(found->fetch_path);

	/* A torrent whose download address redirects to a magnet link: nothing
	 * known beforehand. */
	found = found_named(&outcome, "Hearth.1995.720p.WEB-DL.x264-WEB");
	g_assert_nonnull(found);
	g_assert_null(found->magnet);
	g_assert_nonnull(found->fetch_path);
	g_assert_null(found->release->info_hash);

	/* Markup is only text; it is kept as it is. */
	found = found_named(&outcome, "<script>alert('x')</script>.1995.720p <b>bold</b> & \"quoted\"");
	g_assert_nonnull(found);
	g_assert_cmpstr(found->release->indexer_name, ==, "NewsOwl <i>Evil</i>");

	/* Every field of the wrong type is left out; the release stays. */
	found = found_named(&outcome, "Hearth.1995.WrongTypes");
	g_assert_nonnull(found);
	r = found->release;
	g_assert_cmpint(r->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_cmpint(r->size_bytes, ==, -1);
	g_assert_cmpint(r->indexer_id, ==, 0);
	g_assert_null(r->indexer_name);
	g_assert_cmpint(r->published_us, ==, 0);
	g_assert_null(r->flags);
	g_assert_cmpuint(r->n_categories, ==, 0);
	g_assert_cmpint(r->seeders, ==, -1);
	g_assert_cmpint(r->grabs, ==, -1);
	g_assert_null(r->imdb_id);
	g_assert_cmpint(r->tmdb_id, ==, 0);

	/* A magnet link that is not one is dropped; the address stays. */
	found = found_named(&outcome, "Hearth.1995.BadMagnet");
	g_assert_nonnull(found);
	g_assert_null(found->magnet);
	g_assert_false(found->release->has_magnet);
	g_assert_nonnull(found->fetch_path);

	/* A magnet link in the place of the address is the magnet link. */
	found = found_named(&outcome, "Hearth.1995.MagnetInDownloadUrl");
	g_assert_nonnull(found);
	g_assert_null(found->fetch_path);
	g_assert_true(g_str_has_prefix(found->magnet, "magnet:?xt=urn:btih:fedcba98"));
	g_assert_cmpstr(found->release->info_hash, ==, "fedcba9876543210fedcba9876543210fedcba98");

	/* What was dropped is not there: the other origin, the other port, no way to fetch. */
	g_assert_null(found_named(&outcome, "Hearth.1995.DVDRip.XviD-EVIL"));
	g_assert_null(found_named(&outcome, "Hearth.1995.DVDRip.XviD-PORT"));
	g_assert_null(found_named(&outcome, "Hearth.1995.NoWayToFetch"));
	g_assert_null(found_named(&outcome, "Hearth.1995.NoProtocol"));

	axis_indexer_outcome_clear(&outcome);
	axis_http_origin_clear(&origin);
}

/* Spec: A release is fetched only from its source (the base path) */
static void
test_parse_search_base_path(void)
{
	AxisHttpOrigin origin;
	AxisIndexerOutcome outcome;
	g_autofree gchar *under = fixture_file("search-movie.json", "http://127.0.0.1:4242", "/prowlarr");
	g_autofree gchar *top = fixture_file("search-movie.json", "http://127.0.0.1:4242", "");

	g_assert_true(axis_http_origin_parse("http://127.0.0.1:4242", &origin, NULL));
	/* Prowlarr served under /prowlarr names its addresses under it. */
	g_assert_true(axis_prowlarr_parse_search(under, strlen(under), &origin, "/prowlarr", 0, &outcome, NULL));
	g_assert_cmpuint(outcome.items->len, ==, 8);
	g_assert_cmpuint(outcome.n_off_origin, ==, 2);
	g_assert_true(g_str_has_prefix(found_named(&outcome, "Hearth.1995.Remux-ONLY")->fetch_path, "/prowlarr/5/download?"));
	axis_indexer_outcome_clear(&outcome);

	/* Addresses that are not under the configured base path are not
	 * fetched from: with a magnet link the result stays, without it goes. */
	g_assert_true(axis_prowlarr_parse_search(top, strlen(top), &origin, "/prowlarr", 0, &outcome, NULL));
	g_assert_cmpuint(outcome.items->len, ==, 2);
	g_assert_nonnull(found_named(&outcome, "Hearth.1995.2160p.UHD.BluRay.x265-TRK"));
	g_assert_null(found_named(&outcome, "Hearth.1995.2160p.UHD.BluRay.x265-TRK")->fetch_path);
	g_assert_nonnull(found_named(&outcome, "Hearth.1995.MagnetInDownloadUrl"));
	g_assert_cmpuint(outcome.n_unusable, ==, 7);
	g_assert_cmpuint(outcome.n_off_origin, ==, 2);
	axis_indexer_outcome_clear(&outcome);
	axis_http_origin_clear(&origin);
}

/* Spec: Address elsewhere; the rules of a fetch reference */
static void
test_fetch_reference_rules(void)
{
	static const struct
	{
		const gchar         *url;
		const gchar         *base;
		AxisIndexerRefResult expected;
		const gchar         *path;
	} cases[] = {
		{ "http://prowlarr.test:9696/3/download?apikey=k&link=x", "", AXIS_INDEXER_REF_OK, "/3/download?apikey=k&link=x" },
		{ "HTTP://PROWLARR.TEST:9696/a", "", AXIS_INDEXER_REF_OK, "/a" },
		{ "http://prowlarr.test:9697/a", "", AXIS_INDEXER_REF_OFF_ORIGIN, NULL },
		{ "http://prowlarr.test/a", "", AXIS_INDEXER_REF_OFF_ORIGIN, NULL },
		{ "https://prowlarr.test:9696/a", "", AXIS_INDEXER_REF_OFF_ORIGIN, NULL },
		{ "http://other.test:9696/a", "", AXIS_INDEXER_REF_OFF_ORIGIN, NULL },
		{ "http://prowlarr.test.evil.test:9696/a", "", AXIS_INDEXER_REF_OFF_ORIGIN, NULL },
		{ "ftp://prowlarr.test:9696/a", "", AXIS_INDEXER_REF_OFF_ORIGIN, NULL },
		{ "http://user:pw@prowlarr.test:9696/a", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a#frag", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a/../b", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a/./b", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a/%2e%2e/b", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a/%2E%2E/b", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a%2fb", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a%5cb", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a%00b", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696//a", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a b", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "/3/download?x", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "//prowlarr.test:9696/a", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "javascript:alert(1)", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "", "", AXIS_INDEXER_REF_INVALID, NULL },
		{ NULL, "", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/prowlarr/3/download?x=1", "/prowlarr", AXIS_INDEXER_REF_OK, "/prowlarr/3/download?x=1" },
		{ "http://prowlarr.test:9696/prowlarr", "/prowlarr", AXIS_INDEXER_REF_OK, "/prowlarr" },
		{ "http://prowlarr.test:9696/prowlarrx/3", "/prowlarr", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/3/download", "/prowlarr", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/prowlarr/../admin", "/prowlarr", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/prowlarr/%2e%2e/admin", "/prowlarr", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/other/prowlarr/3", "/prowlarr", AXIS_INDEXER_REF_INVALID, NULL },
		{ "http://prowlarr.test:9696/a?next=http://evil.test/", "", AXIS_INDEXER_REF_OK, "/a?next=http://evil.test/" }
	};
	AxisHttpOrigin origin;
	AxisHttpOrigin secure;
	g_autofree gchar *long_path = g_strnfill(AXIS_INDEXER_FETCH_PATH_MAX + 1, 'a');
	g_autofree gchar *fits_path = g_strnfill(3900, 'a');
	g_autofree gchar *fits_url = g_strconcat("http://prowlarr.test:9696/", fits_path, NULL);
	g_autofree gchar *long_url = g_strconcat("http://prowlarr.test:9696/", long_path, NULL);
	gchar *path = NULL;
	guint i;

	g_assert_true(axis_http_origin_parse("http://prowlarr.test:9696", &origin, NULL));
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		AxisIndexerRefResult result = axis_indexer_fetch_ref(cases[i].url, &origin, cases[i].base, &path);

		if (result != cases[i].expected)
			g_error("%s (base %s): expected %d, got %d", cases[i].url != NULL ? cases[i].url : "(null)", cases[i].base,
				(gint)cases[i].expected, (gint)result);
		g_assert_cmpstr(path, ==, cases[i].path);
		g_free(path);
		path = NULL;
	}
	g_assert_cmpint(axis_indexer_fetch_ref(long_url, &origin, "", &path), ==, AXIS_INDEXER_REF_INVALID);
	g_assert_null(path);
	/* A long one that fits is taken (the whole address may be 4,096 bytes): a
	 * real Prowlarr's link for a torrent is well past 2,000. */
	g_assert_cmpint(axis_indexer_fetch_ref(fits_url, &origin, "", &path), ==, AXIS_INDEXER_REF_OK);
	g_assert_cmpuint(strlen(path), ==, 3901);
	g_clear_pointer(&path, g_free);
	axis_http_origin_clear(&origin);

	/* The scheme's default port counts as the port. */
	g_assert_true(axis_http_origin_parse("https://Indexers.Test", &secure, NULL));
	g_assert_cmpint(axis_indexer_fetch_ref("https://indexers.test/a", &secure, "", &path), ==, AXIS_INDEXER_REF_OK);
	g_free(path);
	g_assert_cmpint(axis_indexer_fetch_ref("https://INDEXERS.test:443/a", &secure, "", &path), ==, AXIS_INDEXER_REF_OK);
	g_free(path);
	g_assert_cmpint(axis_indexer_fetch_ref("https://indexers.test:444/a", &secure, "", &path), ==,
		AXIS_INDEXER_REF_OFF_ORIGIN);
	g_assert_cmpint(axis_indexer_fetch_ref("http://indexers.test/a", &secure, "", &path), ==,
		AXIS_INDEXER_REF_OFF_ORIGIN);
	axis_http_origin_clear(&secure);
}

/* Task 4.1: lists are capped */
static void
test_parse_search_capped(void)
{
	AxisHttpOrigin origin;
	AxisIndexerOutcome outcome;
	GString *text = g_string_new("[");
	guint i;

	g_assert_true(axis_http_origin_parse("http://127.0.0.1:4242", &origin, NULL));
	for (i = 0; i < 1500; i++)
		g_string_append_printf(text, "%s{\"title\":\"Release %u\",\"size\":%u,\"protocol\":\"usenet\","
			"\"downloadUrl\":\"http://127.0.0.1:4242/3/download?apikey=k&n=%u\"}", i > 0 ? "," : "", i, i, i);
	g_string_append_c(text, ']');
	g_assert_true(axis_prowlarr_parse_search(text->str, text->len, &origin, "", 0, &outcome, NULL));
	g_assert_cmpuint(outcome.items->len, ==, AXIS_INDEXER_MAX_RESULTS);
	g_assert_true(outcome.truncated);
	axis_indexer_outcome_clear(&outcome);

	/* A list of nothing but junk ends too. */
	g_string_truncate(text, 0);
	g_string_append_c(text, '[');
	for (i = 0; i < 7000; i++)
		g_string_append_printf(text, "%s\"x\"", i > 0 ? "," : "");
	g_string_append_c(text, ']');
	g_assert_true(axis_prowlarr_parse_search(text->str, text->len, &origin, "", 0, &outcome, NULL));
	g_assert_cmpuint(outcome.items->len, ==, 0);
	g_assert_cmpuint(outcome.n_skipped, ==, AXIS_INDEXER_MAX_SCAN);
	g_assert_true(outcome.truncated);
	axis_indexer_outcome_clear(&outcome);
	g_string_free(text, TRUE);
	axis_http_origin_clear(&origin);
}

/* Task 4.1: answers that are not lists */
static void
test_parse_broken_answers(void)
{
	static const gchar *const broken[] = {
		"", "not json", "{\"message\":\"x\"}", "\"string\"", "42", "null", "[1,2", "[}", "{\"a\":[1,2,3]}"
	};
	AxisHttpOrigin origin;
	guint i;

	g_assert_true(axis_http_origin_parse("http://127.0.0.1:4242", &origin, NULL));
	for (i = 0; i < G_N_ELEMENTS(broken); i++) {
		AxisIndexerOutcome outcome;
		g_autofree gchar *problem = NULL;
		GPtrArray *indexers = NULL;
		GArray *ids = NULL;

		g_assert_false(axis_prowlarr_parse_search(broken[i], strlen(broken[i]), &origin, "", 0, &outcome, &problem));
		g_assert_nonnull(problem);
		g_assert_null(outcome.items);
		g_clear_pointer(&problem, g_free);
		g_assert_false(axis_prowlarr_parse_indexers(broken[i], strlen(broken[i]), &indexers, &problem));
		g_assert_null(indexers);
		g_clear_pointer(&problem, g_free);
		g_assert_false(axis_prowlarr_parse_failing(broken[i], strlen(broken[i]), 0, &ids, &problem));
		g_assert_null(ids);
	}
	axis_http_origin_clear(&origin);
	g_assert_null(axis_prowlarr_parse_version("[]", 2));
	g_assert_null(axis_prowlarr_parse_version("{\"version\":5}", 13));
	g_assert_null(axis_prowlarr_parse_version(NULL, 0));
}

/* Spec: Some indexers fail (the lists the failures come from) */
static void
test_parse_indexers_and_status(void)
{
	g_autofree gchar *indexers_text = fixture_file("indexer.json", "", "");
	g_autofree gchar *status_text = fixture_file("indexerstatus.json", "", "");
	g_autofree gchar *version_text = fixture_file("system-status.json", "", "");
	g_autofree gchar *version = axis_prowlarr_parse_version(version_text, strlen(version_text));
	GPtrArray *indexers = NULL;
	GArray *ids = NULL;
	AxisIndexerInfo *info;

	g_assert_cmpstr(version, ==, "1.21.2.4649");
	g_assert_true(axis_prowlarr_parse_indexers(indexers_text, strlen(indexers_text), &indexers, NULL));
	/* The protocol nobody knows and the string are left out. */
	g_assert_cmpuint(indexers->len, ==, 4);
	info = g_ptr_array_index(indexers, 0);
	g_assert_cmpint(info->id, ==, 3);
	g_assert_cmpstr(info->name, ==, "NewsOwl");
	g_assert_cmpint(info->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_true(info->enabled);
	g_assert_cmpuint(info->movie_ids, ==, AXIS_INDEXER_IDS_IMDB | AXIS_INDEXER_IDS_TMDB);
	g_assert_cmpuint(info->tv_ids, ==, AXIS_INDEXER_IDS_TVDB | AXIS_INDEXER_IDS_IMDB);
	info = g_ptr_array_index(indexers, 1);
	g_assert_cmpint(info->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_cmpuint(info->tv_ids, ==, 0);
	info = g_ptr_array_index(indexers, 2);
	g_assert_cmpstr(info->name, ==, "Slow <b>Tracker</b> & Co");
	info = g_ptr_array_index(indexers, 3);
	g_assert_false(info->enabled);
	g_ptr_array_unref(indexers);

	/* Failing now: disabled until a time after now; the one disabled until
	 * 2020, and the one without a time, are not. */
	g_assert_true(axis_prowlarr_parse_failing(status_text, strlen(status_text), g_get_real_time(), &ids, NULL));
	g_assert_cmpuint(ids->len, ==, 1);
	g_assert_cmpint(g_array_index(ids, gint64, 0), ==, 5);
	g_array_unref(ids);
}

/* Spec: Search (identifiers where the source takes them; the one place that
 * knows the forms) */
static void
test_search_path_forms(void)
{
	AxisIndexerRequest r;
	AxisIndexerMode modes[3];
	g_autofree gchar *path = NULL;

	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_FILM);
	r.title = "Hearth";
	r.year = 1995;
	r.imdb_id = "tt0600001";
	r.tmdb_id = 6001;
	g_assert_cmpuint(axis_prowlarr_identifiers(&r, modes), ==, 2);
	g_assert_cmpint(modes[0], ==, AXIS_INDEXER_BY_IMDB);
	g_assert_cmpint(modes[1], ==, AXIS_INDEXER_BY_TMDB);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_IMDB);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=%7BImdbId%3Att0600001%7D&type=movie&categories=2000&limit=1000");
	g_free(path);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TMDB);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=%7BTmdbId%3A6001%7D&type=movie&categories=2000&limit=1000");
	g_free(path);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Hearth%201995&type=search&categories=2000&limit=1000");
	g_free(path);
	/* An identifier the request does not have cannot be asked. */
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TVDB);
	g_assert_null(path);

	/* An episode: the series' identifier with the numbers; series by TVDB first. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_EPISODE);
	r.title = "The Show";
	r.tvdb_id = 82001;
	r.imdb_id = "tt0700001";
	r.season = 1;
	r.episode = 2;
	g_assert_cmpuint(axis_prowlarr_identifiers(&r, modes), ==, 2);
	g_assert_cmpint(modes[0], ==, AXIS_INDEXER_BY_TVDB);
	g_assert_cmpint(modes[1], ==, AXIS_INDEXER_BY_IMDB);
	g_free(path);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TVDB);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=%7BTvdbId%3A82001%7D%7BSeason%3A1%7D%7BEpisode%3A2%7D&type=tvsearch"
		"&categories=5000&limit=1000");
	g_free(path);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=The%20Show%20S01E02&type=search&categories=5000&limit=1000");
	g_clear_pointer(&path, g_free);

	/* A season, with categories and indexers the request names. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_SEASON);
	r.title = "The Show";
	r.tvdb_id = 82001;
	r.season = 3;
	r.n_categories = 2;
	r.categories[0] = 5030;
	r.categories[1] = 5040;
	r.n_indexers = 2;
	r.indexers[0] = 3;
	r.indexers[1] = 5;
	g_free(path);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TVDB);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=%7BTvdbId%3A82001%7D%7BSeason%3A3%7D&type=tvsearch"
		"&categories=5030&categories=5040&indexerIds=3&indexerIds=5&limit=1000");
	g_free(path);

	/* Words as typed, whatever is in them, are only ever encoded text: no
	 * "&", "#", space or non-ASCII byte reaches the path as it is. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_TEXT);
	r.title = "a&b=c#d e/f?g \xc3\xa9 {ImdbId:tt1}";
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_nonnull(path);
	g_assert_true(axis_indexer_path_ok(path, ""));
	g_assert_null(strchr(strchr(path, '?') + 1 + strlen("query="), '#'));
	g_assert_nonnull(strstr(path, "query=a%26b%3Dc%23d%20e%2Ff%3Fg%20%C3%A9%20%7BImdbId%3Att1%7D&type=search"));
	g_free(path);
	/* No key in any path. */
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_null(strstr(path, "apikey"));
	g_assert_cmpuint(axis_prowlarr_identifiers(&r, modes), ==, 0);
}

/*
 * add-anime-support, A3.  A request marked as an anime series' is asked in
 * TV/Anime beside the TV categories, for an episode, a season and words; a
 * request not so marked is, byte for byte, the request it was, with the key
 * it had (the text the key is made of is written out here as it was before
 * the field existed).
 * Spec (indexers delta): An anime search includes the anime category; A
 * standard series; No absolute number.
 */
static void
test_request_anime(void)
{
	static const gint own[] = { 5030, 5040 };
	static const gint has[] = { 5070, 5000 };
	AxisIndexerRequest r;
	gint out[AXIS_INDEXER_MAX_CATEGORIES];
	gint full[AXIS_INDEXER_MAX_CATEGORIES];
	g_autofree gchar *path = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *key_anime = NULL;
	g_autofree gchar *was = NULL;
	g_autofree gchar *is = NULL;
	guint i;

	g_assert_cmpuint(axis_indexer_request_tv_categories(NULL, 0, out), ==, 2);
	g_assert_cmpint(out[0], ==, 5000);
	g_assert_cmpint(out[1], ==, 5070);
	g_assert_cmpuint(axis_indexer_request_tv_categories(own, G_N_ELEMENTS(own), out), ==, 3);
	g_assert_cmpint(out[0], ==, 5030);
	g_assert_cmpint(out[1], ==, 5040);
	g_assert_cmpint(out[2], ==, 5070);
	/* Among them already: once. */
	g_assert_cmpuint(axis_indexer_request_tv_categories(has, G_N_ELEMENTS(has), out), ==, 2);
	g_assert_cmpint(out[0], ==, 5070);
	g_assert_cmpint(out[1], ==, 5000);
	/* No room: the indexer's own, and no write past the end. */
	for (i = 0; i < G_N_ELEMENTS(full); i++)
		full[i] = 5010 + (gint)i;
	g_assert_cmpuint(axis_indexer_request_tv_categories(full, G_N_ELEMENTS(full), out), ==, AXIS_INDEXER_MAX_CATEGORIES);
	g_assert_cmpint(out[AXIS_INDEXER_MAX_CATEGORIES - 1], ==, 5010 + AXIS_INDEXER_MAX_CATEGORIES - 1);

	/* An episode. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_EPISODE);
	g_assert_false(r.anime);
	r.title = "Tenkuu Rangers";
	r.tvdb_id = 82001;
	r.season = 6;
	r.episode = 12;
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TVDB);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=%7BTvdbId%3A82001%7D%7BSeason%3A6%7D%7BEpisode%3A12%7D&type=tvsearch"
		"&categories=5000&limit=1000");
	g_clear_pointer(&path, g_free);
	key = axis_indexer_request_key(&r);
	was = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "v1|1||0|82001|tenkuu rangers|0|6|12||", -1);
	g_assert_cmpstr(key, ==, was);
	r.anime = TRUE;
	g_assert_true(axis_indexer_request_check(&r, NULL));
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TVDB);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=%7BTvdbId%3A82001%7D%7BSeason%3A6%7D%7BEpisode%3A12%7D&type=tvsearch"
		"&categories=5000&categories=5070&limit=1000");
	g_clear_pointer(&path, g_free);
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Tenkuu%20Rangers%20S06E12&type=search&categories=5000&categories=5070"
		"&limit=1000");
	g_clear_pointer(&path, g_free);
	key_anime = axis_indexer_request_key(&r);
	is = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "v1|1||0|82001|tenkuu rangers|0|6|12|||anime", -1);
	g_assert_cmpstr(key_anime, ==, is);
	g_assert_cmpstr(key_anime, !=, key);
	/* Categories the request names itself are the ones asked. */
	r.n_categories = 1;
	r.categories[0] = 5040;
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Tenkuu%20Rangers%20S06E12&type=search&categories=5040&limit=1000");
	g_clear_pointer(&path, g_free);

	/* A season. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_SEASON);
	r.title = "Tenkuu Rangers";
	r.season = 6;
	r.anime = TRUE;
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Tenkuu%20Rangers%20S06&type=search&categories=5000&categories=5070"
		"&limit=1000");
	g_clear_pointer(&path, g_free);

	/* The words of the title and an absolute number: a text search; not for
	 * an anime series, in no category, as words always were. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_TEXT);
	r.title = "Tenkuu Rangers 137";
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Tenkuu%20Rangers%20137&type=search&limit=1000");
	g_clear_pointer(&path, g_free);
	r.anime = TRUE;
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Tenkuu%20Rangers%20137&type=search&categories=5000&categories=5070"
		"&limit=1000");
	g_clear_pointer(&path, g_free);

	/* A film is asked for as a film, whatever is set. */
	axis_indexer_request_init(&r, AXIS_INDEXER_QUERY_FILM);
	r.title = "Hearth";
	r.anime = TRUE;
	path = axis_prowlarr_search_path(&r, AXIS_INDEXER_BY_TEXT);
	g_assert_cmpstr(path, ==, "/api/v1/search?query=Hearth&type=search&categories=2000&limit=1000");
}

/* Spec: Repeat (the key of a search) */
static void
test_request_rules(void)
{
	AxisIndexerRequest a;
	AxisIndexerRequest b;
	const gchar *problem = NULL;
	g_autofree gchar *key_a = NULL;
	g_autofree gchar *key_b = NULL;

	axis_indexer_request_init(&a, AXIS_INDEXER_QUERY_FILM);
	a.title = "Hearth";
	a.year = 1995;
	a.n_categories = 3;
	a.categories[0] = 2040;
	a.categories[1] = 2000;
	a.categories[2] = 2040;
	g_assert_true(axis_indexer_request_check(&a, &problem));

	/* The same however it is cased, spaced or ordered. */
	axis_indexer_request_init(&b, AXIS_INDEXER_QUERY_FILM);
	b.title = "  HEARTH ";
	b.year = 1995;
	b.n_categories = 2;
	b.categories[0] = 2000;
	b.categories[1] = 2040;
	key_a = axis_indexer_request_key(&a);
	key_b = axis_indexer_request_key(&b);
	g_assert_cmpuint(strlen(key_a), ==, 64);
	g_assert_cmpstr(key_a, ==, key_b);
	g_assert_null(strstr(key_a, "hearth"));
	b.year = 1996;
	g_free(key_b);
	key_b = axis_indexer_request_key(&b);
	g_assert_cmpstr(key_a, !=, key_b);
	b.year = 1995;
	b.kind = AXIS_INDEXER_QUERY_TEXT;
	g_free(key_b);
	key_b = axis_indexer_request_key(&b);
	g_assert_cmpstr(key_a, !=, key_b);

	/* What cannot be asked. */
	axis_indexer_request_init(&a, AXIS_INDEXER_QUERY_FILM);
	g_assert_false(axis_indexer_request_check(&a, &problem));
	g_assert_nonnull(problem);
	axis_indexer_request_init(&a, AXIS_INDEXER_QUERY_TEXT);
	a.title = "   ";
	g_assert_false(axis_indexer_request_check(&a, NULL));
	axis_indexer_request_init(&a, AXIS_INDEXER_QUERY_EPISODE);
	a.title = "The Show";
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.season = 1;
	a.episode = 0;
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.episode = 2;
	g_assert_true(axis_indexer_request_check(&a, NULL));
	axis_indexer_request_init(&a, AXIS_INDEXER_QUERY_FILM);
	a.imdb_id = "tt12";
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.imdb_id = "tt0600001";
	g_assert_true(axis_indexer_request_check(&a, NULL));
	a.tmdb_id = -4;
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.tmdb_id = 0;
	a.year = 12;
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.year = 0;
	a.n_categories = 17;
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.n_categories = 1;
	a.categories[0] = 0;
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.categories[0] = 2000;
	a.n_indexers = 1;
	a.indexers[0] = 0;
	g_assert_false(axis_indexer_request_check(&a, NULL));
	a.n_indexers = 0;
	{
		g_autofree gchar *long_title = g_strnfill(201, 'x');

		a.title = long_title;
		g_assert_false(axis_indexer_request_check(&a, NULL));
	}
	a.title = "\xff\xfe";
	g_assert_false(axis_indexer_request_check(&a, NULL));
}

/* Spec: Not a release file; task 4.4 */
static void
test_content_kind(void)
{
	static const gchar nzb_plain[] = "<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\"><file/></nzb>";
	static const gchar nzb_bom[] = "\xef\xbb\xbf  \n<?xml version=\"1.0\"?><nzb></nzb>";
	static const gchar nzb_comment[] = "<?xml version=\"1.0\"?><!-- made by a tool --><!DOCTYPE nzb PUBLIC \"x\" \"y\">"
		"\n<nzb\n>";
	static const gchar nzb_selfclosed[] = "<nzb/>        ";
	static const gchar html[] = "<html><body>Please log in</body></html>";
	static const gchar other_xml[] = "<?xml version=\"1.0\"?><rss><channel/></rss>";
	static const gchar nzbx[] = "<?xml version=\"1.0\"?><nzbx></nzbx>";
	static const gchar entities[] = "<?xml version=\"1.0\"?><!DOCTYPE nzb [<!ENTITY a \"b\">]><nzb></nzb>";
	static const gchar late[] = "<?xml version=\"1.0\"?><html><nzb></nzb></html>";
	static const gchar json[] = "{\"error\":\"nope\"}      ";
	g_autoptr(GBytes) torrent = axis_test_download_torrent("Hearth.1995", NULL);
	gsize length = 0;
	const guint8 *data = g_bytes_get_data(torrent, &length);
	g_autofree guint8 *cut = g_memdup2(data, length - 3);
	g_autofree guint8 *extra = g_malloc(length + 1);
	GString *long_comment = g_string_new("<?xml version=\"1.0\"?><!--");

	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)nzb_plain, strlen(nzb_plain)), ==, AXIS_INDEXER_CONTENT_NZB);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)nzb_bom, strlen(nzb_bom)), ==, AXIS_INDEXER_CONTENT_NZB);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)nzb_comment, strlen(nzb_comment)), ==,
		AXIS_INDEXER_CONTENT_NZB);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)nzb_selfclosed, strlen(nzb_selfclosed)), ==,
		AXIS_INDEXER_CONTENT_NZB);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)AXIS_TEST_PROWLARR_NZB, strlen(AXIS_TEST_PROWLARR_NZB)), ==,
		AXIS_INDEXER_CONTENT_NZB);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)html, strlen(html)), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)other_xml, strlen(other_xml)), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)nzbx, strlen(nzbx)), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)entities, strlen(entities)), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)late, strlen(late)), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)json, strlen(json)), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind(NULL, 0), ==, AXIS_INDEXER_CONTENT_NONE);
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)"<nzb", 4), ==, AXIS_INDEXER_CONTENT_NONE);

	g_assert_cmpint(axis_indexer_content_kind(data, length), ==, AXIS_INDEXER_CONTENT_TORRENT);
	g_assert_cmpint(axis_indexer_content_kind(cut, length - 3), ==, AXIS_INDEXER_CONTENT_NONE);
	memcpy(extra, data, length);
	extra[length] = 'x';
	g_assert_cmpint(axis_indexer_content_kind(extra, length + 1), ==, AXIS_INDEXER_CONTENT_NONE);

	/* An unterminated comment longer than the check allows is not an NZB. */
	g_string_append_len(long_comment, "aaaaaaaa", 8);
	while (long_comment->len < 20000)
		g_string_append(long_comment, "aaaaaaaaaaaaaaaa");
	g_string_append(long_comment, "--><nzb></nzb>");
	g_assert_cmpint(axis_indexer_content_kind((const guint8 *)long_comment->str, long_comment->len), ==,
		AXIS_INDEXER_CONTENT_NONE);
	g_string_free(long_comment, TRUE);
}

static void
test_names_and_file_names(void)
{
	g_autofree gchar *a = axis_indexer_file_name("Hearth.1995.1080p.BluRay.x264-GRP", "nzb");
	g_autofree gchar *b = axis_indexer_file_name("../../etc/passwd", "nzb");
	g_autofree gchar *c = axis_indexer_file_name("..hidden", "torrent");
	g_autofree gchar *d = axis_indexer_file_name("", "nzb");
	g_autofree gchar *e = axis_indexer_file_name("\xc3\xa9t\xc3\xa9 <b>x</b>", "nzb");
	g_autofree gchar *long_name = g_strnfill(400, 'a');
	g_autofree gchar *f = axis_indexer_file_name(long_name, "nzb");
	g_autofree gchar *text = axis_indexer_text_clean("  a \t b\n\x01 c ", 10);

	g_assert_cmpstr(a, ==, "Hearth.1995.1080p.BluRay.x264-GRP.nzb");
	g_assert_null(strchr(b, '/'));
	g_assert_cmpstr(c, ==, "hidden.torrent");
	g_assert_cmpstr(d, ==, "release.nzb");
	g_assert_null(strchr(e, '<'));
	g_assert_cmpuint(strlen(f), ==, 120 + 4);
	g_assert_cmpstr(text, ==, "a b c");
}

/* Spec: Release names are never markup (kinds' words and states) */
static void
test_words(void)
{
	AxisIndexerSourceState state;
	AxisIndexerMode mode;
	AxisIndexerQueryKind kind;
	guint i;

	for (i = 0; i <= AXIS_INDEXER_SOURCE_FAILING; i++) {
		g_assert_true(axis_indexer_source_state_from_string(axis_indexer_source_state_to_string(i), &state));
		g_assert_cmpuint(state, ==, i);
		g_assert_nonnull(axis_indexer_source_state_label(i));
	}
	g_assert_false(axis_indexer_source_state_from_string("nonsense", &state));
	for (i = 0; i <= AXIS_INDEXER_BY_TVDB; i++) {
		g_assert_true(axis_indexer_mode_from_string(axis_indexer_mode_to_string(i), &mode));
		g_assert_cmpuint(mode, ==, i);
	}
	for (i = 0; i <= AXIS_INDEXER_QUERY_TEXT; i++) {
		g_assert_true(axis_indexer_query_kind_from_string(axis_indexer_query_kind_to_string(i), &kind));
		g_assert_cmpuint(kind, ==, i);
	}
	g_assert_false(axis_indexer_query_kind_from_string(NULL, &kind));
	{
		g_autofree gchar *none = axis_indexer_ids_label(0);
		g_autofree gchar *two = axis_indexer_ids_label(AXIS_INDEXER_IDS_IMDB | AXIS_INDEXER_IDS_TVDB);

		g_assert_cmpstr(none, ==, "none");
		g_assert_cmpstr(two, ==, "IMDb, TVDB");
	}
}

/* ── The fixture ────────────────────────────────────────────────── */

typedef struct
{
	AxisTestDb       *db;
	AxisDbPool       *pool;
	AxisSettings     *settings;
	AxisIndexers     *indexers;
	AxisTestProwlarr *fake;
	gint64            source_id;
} Fixture;

static AxisIndexers *
layer_new(Fixture *fx)
{
	AxisIndexers *indexers = axis_indexers_new(fx->pool, fx->settings);
	AxisIndexerTuning tuning;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_add_kind(indexers, axis_indexer_prowlarr_kind(), &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.wait_ms = 5000;
	axis_indexers_set_tuning(indexers, &tuning);
	return indexers;
}

static gint64
add_source_at(
	Fixture     *fx,
	const gchar *name,
	const gchar *address,
	const gchar *base_path,
	const gchar *key
){
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = AXIS_INDEXER_PROWLARR_NAME;
	spec.address = address;
	spec.base_path = base_path;
	spec.enabled = TRUE;
	spec.secret = key;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 12);
	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_INDEXER_MODULE_ID, axis_indexers_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_no_error(error);
	fx->indexers = layer_new(fx);
	/* 1: served under /prowlarr; 2: the recorded answers of a real one. */
	fx->fake = GPOINTER_TO_UINT(data) == 2 ? axis_test_prowlarr_new_recorded(KEY, "") :
		axis_test_prowlarr_new(KEY, GPOINTER_TO_UINT(data) == 1 ? "/prowlarr" : "");
	base = axis_test_prowlarr_base(fx->fake);
	fx->source_id = add_source_at(fx, "Prowlarr", base, GPOINTER_TO_UINT(data) == 1 ? "/prowlarr" : "", KEY);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_http_set_offsite_testing(FALSE, NULL);
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_indexers_unref(fx->indexers);
	axis_test_prowlarr_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

static gchar *
q(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->db, sql);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = q(fx, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static void
film_request(AxisIndexerRequest *request)
{
	axis_indexer_request_init(request, AXIS_INDEXER_QUERY_FILM);
	request->title = "Hearth";
	request->year = 1995;
	request->imdb_id = "tt0600001";
	request->tmdb_id = 6001;
}

static AxisIndexerSearchResult *
search_ok(
	Fixture                  *fx,
	const AxisIndexerRequest *request
){
	AxisIndexerSearchResult *result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_search(fx->indexers, 0, request, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static AxisIndexerRelease *
release_named(
	const AxisIndexerSearchResult *result,
	const gchar                   *name
){
	guint i;

	for (i = 0; i < result->releases->len; i++) {
		AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (g_strcmp0(release->name, name) == 0)
			return release;
	}
	return NULL;
}

/* A search that finds exactly the one release of @json. */
static AxisIndexerSearchResult *
search_one(
	Fixture     *fx,
	const gchar *json
){
	AxisIndexerRequest request;

	axis_test_prowlarr_set_search(fx->fake, NULL, json);
	film_request(&request);
	return search_ok(fx, &request);
}

static void
assert_no_secret_in(const gchar *text)
{
	g_assert_nonnull(text);
	g_assert_null(strstr(text, KEY));
	g_assert_null(strstr(text, "apikey"));
	g_assert_null(strstr(text, "download?"));
	g_assert_null(strstr(text, "pk-secret"));
}

static void
assert_error_is_clean(const GError *error)
{
	g_assert_nonnull(error);
	assert_no_secret_in(error->message);
	g_assert_null(strstr(error->message, "link="));
	g_assert_null(strstr(error->message, "file=hearth"));
}

/* ── Sources ────────────────────────────────────────────────────── */

/* Spec: Indexers page (the state); task 2.1 */
static void
test_source_test_reads_version_and_indexers(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerInfo *info;

	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_UNCHECKED);
	g_assert_true(source->key_set);
	g_assert_cmpuint(source->indexers->len, ==, 0);
	g_clear_pointer(&source, axis_indexer_source_info_free);

	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_null(source->state_message);
	g_assert_cmpstr(source->version, ==, "1.21.2.4649");
	g_assert_cmpint(source->last_contact_us, >, 0);
	g_assert_cmpint(source->indexers_us, >, 0);
	g_assert_cmpuint(source->indexers->len, ==, 4);
	/* By name: NewsOwl, Slow <b>Tracker</b> & Co, Switched Off, Tracker One. */
	info = g_ptr_array_index(source->indexers, 0);
	g_assert_cmpstr(info->name, ==, "NewsOwl");
	g_assert_false(info->failing);
	info = g_ptr_array_index(source->indexers, 3);
	g_assert_cmpstr(info->name, ==, "Tracker One");
	g_assert_true(info->failing);

	/* Every request carried the key in the header and none in its address,
	 * and nothing but GET was sent. */
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 3);
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->fake), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(axis_test_prowlarr_server(fx->fake)));
}

/* Spec: Key in a fetch address; task 4.3 (the key goes to the source, in a header) */
static void
test_the_key_is_sent_in_a_header_to_the_source_only(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *transcript = NULL;
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *header = NULL;
	g_autofree gchar *query = NULL;

	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 8);
	header = axis_test_plex_last_header(axis_test_prowlarr_server(fx->fake), "/api/v1/search", "X-Api-Key");
	g_assert_cmpstr(header, ==, KEY);
	query = axis_test_prowlarr_search_query(fx->fake, 0);
	g_assert_null(strstr(query, KEY));
	transcript = axis_test_plex_transcript(axis_test_prowlarr_server(fx->fake));
	g_assert_nonnull(strstr(transcript, KEY));
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->fake), ==, 0);
	/* Nothing reached anywhere else. */
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	axis_test_plex_free(other);
}

/* Spec: Key in a fetch address (a refused key is a state, not a message with the key) */
static void
test_a_wrong_key_is_refused_without_naming_it(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *result = NULL;

	axis_test_prowlarr_set_key(fx->fake, OTHER_KEY);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_REFUSED);
	g_assert_nonnull(source->state_message);
	assert_no_secret_in(source->state_message);
	g_assert_null(strstr(source->state_message, OTHER_KEY));

	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_null(result);
	g_assert_error(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_AUTH);
	assert_error_is_clean(failure);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_search");
}

static void
test_a_source_that_does_not_answer_is_a_state(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *result = NULL;
	AxisIndexerTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 700;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_DOWN);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_UNREACHABLE);
	g_assert_nonnull(source->state_message);
	assert_no_secret_in(source->state_message);

	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_true(g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREACHABLE) ||
	              g_error_matches(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED));
	assert_error_is_clean(failure);
}

static void
test_an_answer_that_is_not_a_list_is_unreadable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *result = NULL;
	g_autofree gchar *body = axis_test_prowlarr_fixture(fx->fake, "search-not-a-list.json");

	axis_test_prowlarr_set_search(fx->fake, NULL, body);
	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_error(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE);
	g_assert_null(strstr(failure->message, "Value cannot be null"));
	assert_error_is_clean(failure);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_FAILING);
	g_clear_error(&failure);

	axis_test_prowlarr_set_search(fx->fake, NULL, "<html><body>Login</body></html>");
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_error(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_search");
}

/* Task 2.4: a 429 pauses the source */
static void
test_a_429_pauses_the_source(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *result = NULL;
	guint before;

	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_TOO_MANY);
	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_error(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 1);
	g_clear_error(&failure);

	/* It is not asked again until the time it stated, whatever it says now. */
	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_NORMAL);
	before = axis_test_prowlarr_n_requests(fx->fake);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_error(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_RATE_LIMITED);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, before);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, NULL));
	/* A source that asked to be left alone is not failing. */
	g_assert_cmpint(source->state, !=, AXIS_INDEXER_SOURCE_FAILING);
}

static void
test_a_server_error_is_failing(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) failure = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *result = NULL;

	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_BROKEN);
	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &failure));
	g_assert_error(failure, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED);
	assert_error_is_clean(failure);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_FAILING);
}

static void
test_saving_sources(Fixture *fx, gconstpointer data)
{
	AxisIndexerSourceSpec spec;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	gint64 id = 0;
	g_autofree gchar *address_with_path = g_strconcat(base, "/typedpath7", NULL);

	memset(&spec, 0, sizeof(spec));
	spec.name = "Second";
	spec.kind = AXIS_INDEXER_PROWLARR_NAME;
	spec.address = base;
	spec.enabled = TRUE;

	/* A name in use; a kind there is none of; an address with a path; a
	 * base path that is not one; a key that cannot be sent: each refused
	 * without the value in the message. */
	spec.name = "Prowlarr";
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_EXISTS);
	g_clear_error(&error);
	spec.name = "Second";
	spec.kind = "nonesuch";
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_clear_error(&error);
	spec.kind = AXIS_INDEXER_PROWLARR_NAME;
	spec.address = address_with_path;
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_null(strstr(error->message, "typedpath7"));
	g_clear_error(&error);
	spec.address = base;
	spec.base_path = "../x";
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_null(strstr(error->message, "../x"));
	g_clear_error(&error);
	spec.base_path = "/a/b/";
	spec.secret = "bad\nkey-with-a-line-break";
	g_assert_false(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_assert_null(strstr(error->message, "line-break"));
	g_clear_error(&error);

	/* A base path is stored without its trailing slash. */
	spec.secret = "another-key-value";
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpstr(source->base_path, ==, "/a/b");
	g_assert_true(source->plain_http);
	g_assert_true(source->key_set);
	g_assert_true(source->enabled);

	/* An edit without a key keeps the key; with one replaces it. */
	spec.base_path = "";
	spec.secret = NULL;
	spec.name = "Second renamed";
	g_assert_true(axis_indexers_save_source(fx->indexers, id, &spec, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&source, axis_indexer_source_info_free);
	g_assert_true(axis_indexers_get_source(fx->indexers, id, &source, &error));
	g_assert_cmpstr(source->name, ==, "Second renamed");
	g_assert_cmpstr(source->base_path, ==, "");
	g_assert_true(source->key_set);
	assert_q(fx, "another-key-value", "SELECT secret FROM indexer_source_secret ORDER BY source_id DESC LIMIT 1");

	/* Nothing else of a source is in a listing. */
	g_clear_pointer(&source, axis_indexer_source_info_free);
	g_assert_false(axis_indexers_get_source(fx->indexers, 9999, &source, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_indexers_save_source(fx->indexers, 9999, &spec, NULL, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_indexers_delete_source(fx->indexers, 9999, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);
}

/* A source moved elsewhere, or given another key, has other searches. */
static void
test_changing_a_source_forgets_its_searches(Fixture *fx, gconstpointer data)
{
	AxisIndexerSourceSpec spec;
	AxisIndexerRequest request;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);

	film_request(&request);
	result = search_ok(fx, &request);
	assert_q(fx, "1", "SELECT count(*) FROM indexer_search");
	assert_q(fx, "4", "SELECT count(*) FROM indexer_source_indexer");

	/* A rename alone keeps them. */
	memset(&spec, 0, sizeof(spec));
	spec.name = "Prowlarr renamed";
	spec.address = base;
	spec.enabled = TRUE;
	g_assert_true(axis_indexers_save_source(fx->indexers, fx->source_id, &spec, NULL, &error));
	assert_q(fx, "1", "SELECT count(*) FROM indexer_search");

	/* A new key forgets them, and what was learned of the source. */
	spec.secret = KEY;
	g_assert_true(axis_indexers_save_source(fx->indexers, fx->source_id, &spec, NULL, &error));
	assert_q(fx, "0", "SELECT count(*) FROM indexer_search");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release_secret");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source_indexer");
	assert_q(fx, "unchecked", "SELECT state FROM indexer_source");

	/* Removing the source takes everything of it. */
	g_clear_pointer(&result, axis_indexer_search_result_free);
	result = search_ok(fx, &request);
	g_assert_true(axis_indexers_delete_source(fx->indexers, fx->source_id, &error));
	assert_q(fx, "0", "SELECT count(*) FROM indexer_search");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release_secret");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source_secret");
}

/* ── Searching ──────────────────────────────────────────────────── */

/* Spec: Search; Some indexers fail; tasks 2.2, 4.1 */
static void
test_search_returns_releases_and_failures(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autofree gchar *query = NULL;
	AxisIndexerRelease *release;
	guint i;

	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_false(result->cached);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_false(result->fell_back);
	g_assert_cmpuint(result->releases->len, ==, 8);
	g_assert_cmpuint(result->n_off_origin, ==, 2);
	g_assert_cmpuint(result->n_unusable, ==, 1);
	g_assert_cmpuint(result->n_skipped, ==, 3);
	g_assert_false(result->truncated);
	g_assert_cmpint(result->source_id, ==, fx->source_id);
	g_assert_cmpint(result->expires_us - result->created_us, ==, G_GINT64_CONSTANT(900) * G_USEC_PER_SEC);

	/* Three indexers are on at the source and one of them is failing: the
	 * releases of the others are returned and the failure is stated. */
	g_assert_cmpint(result->n_indexers, ==, 3);
	g_assert_nonnull(result->failed);
	g_assert_cmpstr(result->failed[0], ==, "Tracker One");
	g_assert_null(result->failed[1]);

	/* Every release has its stored identifier and the fields as stated. */
	for (i = 0; i < result->releases->len; i++) {
		release = g_ptr_array_index(result->releases, i);
		g_assert_cmpint(release->id, >, 0);
	}
	release = release_named(result, "Hearth.1995.1080p.BluRay.x264-GRP");
	g_assert_nonnull(release);
	g_assert_cmpint(release->size_bytes, ==, G_GINT64_CONSTANT(8923441152));
	g_assert_cmpint(release->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_cmpstr(release->indexer_name, ==, "NewsOwl");
	g_assert_cmpstr(release->imdb_id, ==, "tt0600001");
	release = release_named(result, "Hearth.1995.2160p.UHD.BluRay.x265-TRK");
	g_assert_nonnull(release);
	g_assert_cmpint(release->seeders, ==, 85);
	g_assert_true(release->has_magnet);
	g_assert_cmpstr(release->info_hash, ==, "0123456789abcdef0123456789abcdef01234567");
	g_assert_cmpstr(release->flags[0], ==, "freeleech");

	/* What reached the source: the indexers, read for the first time and
	 * before the search; a search by the IMDb identifier; and, for the one
	 * indexer that is on and takes no IMDb identifier for a movie (Slow
	 * Tracker, 7: TMDb only), a search by the title of that indexer alone.
	 * It found nothing more. */
	g_assert_true(result->also_text);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 2);
	query = axis_test_prowlarr_search_query(fx->fake, 0);
	g_assert_cmpstr(query, ==, "query=%7BImdbId%3Att0600001%7D&type=movie&categories=2000&limit=1000");
	g_clear_pointer(&query, g_free);
	query = axis_test_prowlarr_search_query(fx->fake, 1);
	g_assert_cmpstr(query, ==, "query=Hearth%201995&type=search&categories=2000&indexerIds=7&limit=1000");
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 4);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, NULL));
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);
	g_assert_cmpuint(source->indexers->len, ==, 4);
	g_assert_cmpuint(source->n_searches, ==, 1);

	/* A search that is not the same asks again, and the indexers (read a
	 * moment ago) are not read again. */
	request.year = 1996;
	g_clear_pointer(&result, axis_indexer_search_result_free);
	result = search_ok(fx, &request);
	g_assert_false(result->cached);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 6);
	g_assert_cmpint(result->n_indexers, ==, 3);
	g_assert_cmpstr(result->failed[0], ==, "Tracker One");
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->fake), ==, 0);
}

/* Spec: Repeat; task 4.5 */
static void
test_a_repeated_search_asks_once(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	AxisIndexerRequest same;
	g_autoptr(AxisIndexerSearchResult) first = NULL;
	g_autoptr(AxisIndexerSearchResult) second = NULL;
	g_autoptr(AxisIndexerSearchResult) third = NULL;
	g_autoptr(GError) error = NULL;
	guint requests;
	guint i;

	film_request(&request);
	first = search_ok(fx, &request);
	requests = axis_test_prowlarr_n_requests(fx->fake);
	g_assert_false(first->cached);

	/* The same request, however the title is cased or spaced. */
	film_request(&same);
	same.title = "  hEARTH ";
	second = search_ok(fx, &same);
	g_assert_true(second->cached);
	g_assert_cmpint(second->search_id, ==, first->search_id);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, requests);
	g_assert_cmpuint(second->releases->len, ==, first->releases->len);
	for (i = 0; i < first->releases->len; i++) {
		const AxisIndexerRelease *a = g_ptr_array_index(first->releases, i);
		const AxisIndexerRelease *b = g_ptr_array_index(second->releases, i);

		g_assert_cmpint(a->id, ==, b->id);
		g_assert_cmpstr(a->name, ==, b->name);
		g_assert_cmpint(a->size_bytes, ==, b->size_bytes);
		g_assert_cmpint(a->has_magnet, ==, b->has_magnet);
	}
	g_assert_cmpint(second->n_indexers, ==, first->n_indexers);
	g_assert_cmpstr(second->failed[0], ==, first->failed[0]);
	g_assert_cmpuint(second->n_off_origin, ==, first->n_off_origin);
	g_assert_cmpint(second->mode, ==, first->mode);

	/* Past its life it asks again; the old search and its releases are gone. */
	axis_test_db_exec(fx->db, "UPDATE indexer_search SET expires_at = now() - interval '1 minute'");
	third = search_ok(fx, &request);
	g_assert_false(third->cached);
	g_assert_cmpint(third->search_id, !=, first->search_id);
	/* By the identifier and, for the indexer that takes none, by the title:
	 * twice each. */
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 4);
	g_assert_cmpint(second->also_text, ==, first->also_text);
	assert_q(fx, "1", "SELECT count(*) FROM indexer_search");
	assert_q(fx, "8", "SELECT count(*) FROM indexer_release");
	assert_q(fx, "8", "SELECT count(*) FROM indexer_release_secret");
}

/* The life of a search is a setting. */
static void
test_the_cache_life_is_a_setting(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_CACHE_LIFE, "2m", &error));
	g_assert_no_error(error);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpint(result->expires_us - result->created_us, ==, G_GINT64_CONSTANT(120) * G_USEC_PER_SEC);
}

/* Spec: Release names are never markup (names are limited and cleaned); task 4.1 */
static void
test_names_are_cleaned_and_cut(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *long_name = g_strnfill(5000, 'x');
	g_autofree gchar *json = NULL;
	AxisIndexerRelease *release;
	AxisIndexerRequest request;
	gchar *invalid_utf8;

	/* A name of five thousand characters, with control characters, a run of
	 * spaces and a byte that is not UTF-8. */
	json = g_strdup_printf("[{\"title\":\"%s\",\"protocol\":\"torrent\",\"magnetUrl\":"
		"\"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567\"},"
		"{\"title\":\"A\\u0001B\\u0007C   D\\n\\tE\",\"protocol\":\"torrent\",\"magnetUrl\":"
		"\"magnet:?xt=urn:btih:f5f6f7f8f5f6f7f8f5f6f7f8f5f6f7f8f5f6f7f8\"},"
		"{\"title\":\"bad \xff\xfe byte\",\"protocol\":\"torrent\",\"magnetUrl\":"
		"\"magnet:?xt=urn:btih:a5a6a7a8a5a6a7a8a5a6a7a8a5a6a7a8a5a6a7a8\"}]", long_name);
	axis_test_prowlarr_set_search(fx->fake, NULL, json);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 3);
	release = g_ptr_array_index(result->releases, 0);
	g_assert_cmpuint(g_utf8_strlen(release->name, -1), ==, AXIS_INDEXER_NAME_MAX_CHARS);
	release = g_ptr_array_index(result->releases, 1);
	g_assert_cmpstr(release->name, ==, "A B C D E");
	release = g_ptr_array_index(result->releases, 2);
	invalid_utf8 = release->name;
	g_assert_true(g_utf8_validate(invalid_utf8, -1, NULL));
	g_assert_nonnull(strstr(invalid_utf8, "bad "));
	/* Stored the same. */
	assert_q(fx, "300", "SELECT max(char_length(name)) FROM indexer_release");
}

/* Task 4.1: result lists are capped */
static void
test_results_are_capped_at_a_thousand(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerRequest request;
	GString *json = g_string_new("[");
	g_autofree gchar *origin = axis_test_prowlarr_base(fx->fake);
	guint i;

	for (i = 0; i < 1300; i++)
		g_string_append_printf(json, "%s{\"title\":\"Release %u\",\"size\":%u,\"protocol\":\"torrent\","
			"\"magnetUrl\":\"magnet:?xt=urn:btih:%040x\",\"seeders\":%u}", i > 0 ? "," : "", i, i + 1, i, i % 50);
	g_string_append_c(json, ']');
	axis_test_prowlarr_set_search(fx->fake, NULL, json->str);
	g_string_free(json, TRUE);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, AXIS_INDEXER_MAX_RESULTS);
	g_assert_true(result->truncated);
	assert_q(fx, "1000", "SELECT count(*) FROM indexer_release");
	/* In the order the source gave them. */
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 0))->name, ==, "Release 0");
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 999))->name, ==, "Release 999");
}

/* Spec: Search (identifiers where supported, words otherwise); task 2.2.
 * Which indexers take which identifier is not known (the source reports
 * none): when the identifier finds nothing, the title is searched instead. */
static void
test_search_falls_back_to_the_title(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autoptr(AxisIndexerSearchResult) bare = NULL;

	/* The indexers take no identifier: the identifier search finds nothing,
	 * the title search finds the releases, and the result says so. */
	axis_test_prowlarr_set_indexers(fx->fake, "[]");
	axis_test_prowlarr_set_search(fx->fake, "movie", "[]");
	{
		g_autofree gchar *body = axis_test_prowlarr_fixture(fx->fake, "search-movie.json");

		axis_test_prowlarr_set_search(fx->fake, "search", body);
	}
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 8);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_TEXT);
	g_assert_true(result->fell_back);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 2);
	first = axis_test_prowlarr_search_query(fx->fake, 0);
	second = axis_test_prowlarr_search_query(fx->fake, 1);
	g_assert_true(g_str_has_prefix(first, "query=%7BImdbId%3Att0600001%7D&type=movie"));
	g_assert_true(g_str_has_prefix(second, "query=Hearth%201995&type=search"));
	/* The same when it is asked again. */
	g_clear_pointer(&result, axis_indexer_search_result_free);
	result = search_ok(fx, &request);
	g_assert_true(result->cached);
	g_assert_true(result->fell_back);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_TEXT);

	/* Without a title there are no words to fall back to: what was found
	 * (nothing) is the answer, by the identifier. */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_FILM);
	request.imdb_id = "tt9999999";
	bare = search_ok(fx, &request);
	g_assert_cmpuint(bare->releases->len, ==, 0);
	g_assert_false(bare->fell_back);
	g_assert_cmpint(bare->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 3);
}

/* An answer in which everything was dropped is not "found nothing": words
 * would drop the same. */
static void
test_search_does_not_fall_back_over_dropped_results(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *json = g_strdup("[{\"title\":\"Hearth.EVIL\",\"protocol\":\"usenet\","
		"\"downloadUrl\":\"http://evil.example.test/3/download?apikey=x\"}]");

	axis_test_prowlarr_set_search(fx->fake, NULL, json);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 0);
	g_assert_cmpuint(result->n_off_origin, ==, 1);
	g_assert_false(result->fell_back);
	/* The second search is the title one of the indexer that takes no
	 * identifier (it found nothing), not a fall-back. */
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 2);
	g_assert_true(result->also_text);
}

/*
 * Spec: An identifier search does not lose indexers that only take text
 * (Mixed indexers); task 5.5, against the constructed answers.  The indexer
 * that takes no IMDb identifier (Slow Tracker, 7) is asked by the title,
 * alone; what it gives that the identifier search gave too is there once,
 * after the identifier's.  When every indexer that is on takes the
 * identifier, only the identifier is asked.
 */
static void
test_search_by_identifier_and_title(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) only_ids = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	g_autofree gchar *by_id = g_strdup_printf("["
		"{\"guid\":\"g-1\",\"title\":\"Hearth.1995.1080p.BY-ID\",\"indexerId\":3,\"protocol\":\"usenet\","
		"\"downloadUrl\":\"%s/3/download?apikey=%s&file=hearth-nzb\"},"
		"{\"guid\":\"g-7\",\"title\":\"Hearth.1995.720p.BOTH\",\"indexerId\":7,\"protocol\":\"torrent\","
		"\"downloadUrl\":\"%s/7/download?apikey=%s&file=hearth-torrent\"}]", base, KEY, base, KEY);
	g_autofree gchar *by_title = g_strdup_printf("["
		"{\"guid\":\"g-7\",\"title\":\"Hearth.1995.720p.BOTH\",\"indexerId\":7,\"protocol\":\"torrent\","
		"\"downloadUrl\":\"%s/7/download?apikey=%s&file=hearth-torrent\"},"
		"{\"guid\":\"g-8\",\"title\":\"Hearth.1995.2160p.BY-TITLE\",\"indexerId\":7,\"protocol\":\"torrent\","
		"\"downloadUrl\":\"%s/7/download?apikey=%s&file=hearth-torrent\"},"
		"{\"guid\":\"g-9\",\"title\":\"Hearth.1995.NOT-ASKED\",\"indexerId\":3,\"protocol\":\"usenet\","
		"\"downloadUrl\":\"%s/3/download?apikey=%s&file=hearth-nzb\"}]", base, KEY, base, KEY, base, KEY);
	g_autofree gchar *second = NULL;

	axis_test_prowlarr_set_search(fx->fake, "movie", by_id);
	axis_test_prowlarr_set_search(fx->fake, "search", by_title);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_true(result->also_text);
	g_assert_false(result->fell_back);
	g_assert_cmpuint(result->releases->len, ==, 3);
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 0))->name, ==, "Hearth.1995.1080p.BY-ID");
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 1))->name, ==, "Hearth.1995.720p.BOTH");
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 2))->name, ==,
		"Hearth.1995.2160p.BY-TITLE");
	g_assert_null(release_named(result, "Hearth.1995.NOT-ASKED"));
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 2);
	second = axis_test_prowlarr_search_query(fx->fake, 1);
	g_assert_cmpstr(second, ==, "query=Hearth%201995&type=search&categories=2000&indexerIds=7&limit=1000");
	assert_q(fx, "t", "SELECT also_text FROM indexer_search");

	/* Every indexer that is on takes the IMDb identifier: it alone is asked. */
	axis_test_prowlarr_set_indexers(fx->fake, "[{\"id\":3,\"name\":\"NewsOwl\",\"protocol\":\"usenet\","
		"\"enable\":true,\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}},"
		"{\"id\":7,\"name\":\"Slow\",\"protocol\":\"torrent\",\"enable\":true,"
		"\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}},"
		"{\"id\":9,\"name\":\"Off\",\"protocol\":\"usenet\",\"enable\":false,"
		"\"capabilities\":{\"movieSearchParams\":[\"q\"]}}]");
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, NULL));
	request.year = 1996;
	only_ids = search_ok(fx, &request);
	g_assert_false(only_ids->also_text);
	g_assert_cmpuint(only_ids->releases->len, ==, 2);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 3);
}

/* Task 2.2: episodes and seasons */
static void
test_search_for_episodes_and_seasons(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) season = NULL;
	g_autoptr(AxisIndexerSearchResult) words = NULL;
	g_autofree gchar *body = axis_test_prowlarr_fixture(fx->fake, "search-tv.json");
	g_autofree gchar *q0 = NULL;
	g_autofree gchar *q1 = NULL;
	g_autofree gchar *q2 = NULL;
	g_autofree gchar *q3 = NULL;
	g_autofree gchar *q4 = NULL;

	/* By TVDB; and by the title, for the one indexer that takes no TVDB
	 * identifier for an episode (Tracker One, 5), which found nothing. */
	axis_test_prowlarr_set_search(fx->fake, NULL, body);
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_EPISODE);
	request.title = "The Show";
	request.tvdb_id = 82001;
	request.season = 1;
	request.episode = 2;
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 2);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_TVDB);
	g_assert_true(result->also_text);
	q0 = axis_test_prowlarr_search_query(fx->fake, 0);
	g_assert_cmpstr(q0, ==, "query=%7BTvdbId%3A82001%7D%7BSeason%3A1%7D%7BEpisode%3A2%7D&type=tvsearch&categories=5000&limit=1000");
	q1 = axis_test_prowlarr_search_query(fx->fake, 1);
	g_assert_cmpstr(q1, ==, "query=The%20Show%20S01E02&type=search&categories=5000&indexerIds=5&limit=1000");

	request.kind = AXIS_INDEXER_QUERY_SEASON;
	request.episode = -1;
	season = search_ok(fx, &request);
	g_assert_cmpint(season->mode, ==, AXIS_INDEXER_BY_TVDB);
	q2 = axis_test_prowlarr_search_query(fx->fake, 2);
	g_assert_cmpstr(q2, ==, "query=%7BTvdbId%3A82001%7D%7BSeason%3A1%7D&type=tvsearch&categories=5000&limit=1000");
	q3 = axis_test_prowlarr_search_query(fx->fake, 3);
	g_assert_cmpstr(q3, ==, "query=The%20Show%20S01&type=search&categories=5000&indexerIds=5&limit=1000");

	/* No identifier: the series' title with the numbers as a person writes them. */
	request.kind = AXIS_INDEXER_QUERY_EPISODE;
	request.episode = 2;
	request.tvdb_id = 0;
	words = search_ok(fx, &request);
	g_assert_cmpint(words->mode, ==, AXIS_INDEXER_BY_TEXT);
	g_assert_false(words->fell_back);
	g_assert_false(words->also_text);
	q4 = axis_test_prowlarr_search_query(fx->fake, 4);
	g_assert_cmpstr(q4, ==, "query=The%20Show%20S01E02&type=search&categories=5000&limit=1000");
}

static void
test_invalid_and_missing_sources(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(GError) error = NULL;
	AxisIndexerSearchResult *result = NULL;
	AxisIndexerSourceSpec spec;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);

	/* A request that cannot be made is refused, with nothing sent. */
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_FILM);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 0);

	/* No such source. */
	film_request(&request);
	g_assert_false(axis_indexers_search(fx->indexers, 4242, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A source that is switched off is not asked, and with none on there is nothing to ask. */
	memset(&spec, 0, sizeof(spec));
	spec.name = "Prowlarr";
	spec.address = base;
	spec.enabled = FALSE;
	g_assert_true(axis_indexers_save_source(fx->indexers, fx->source_id, &spec, NULL, &error));
	g_assert_false(axis_indexers_search(fx->indexers, fx->source_id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE);
	g_clear_error(&error);
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 0);
}

/* ── Fetching ───────────────────────────────────────────────────── */

static gint64
only_release_id(const AxisIndexerSearchResult *result)
{
	g_assert_cmpuint(result->releases->len, ==, 1);
	return ((AxisIndexerRelease *)g_ptr_array_index(result->releases, 0))->id;
}

/* Spec: A release is fetched only from its source; tasks 2.3, 4.2 */
static void
test_fetch_an_nzb(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.1080p.BluRay.x264-GRP", "usenet",
		"hearth-nzb", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	gsize length = 0;
	const gchar *bytes;
	g_autofree gchar *header = NULL;

	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_cmpint(fetched.protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_null(fetched.magnet);
	g_assert_nonnull(fetched.file);
	bytes = g_bytes_get_data(fetched.file, &length);
	g_assert_cmpmem(bytes, length, AXIS_TEST_PROWLARR_NZB, strlen(AXIS_TEST_PROWLARR_NZB));
	g_assert_cmpstr(fetched.file_name, ==, "Hearth.1995.1080p.BluRay.x264-GRP.nzb");
	g_assert_null(fetched.info_hash);
	axis_indexer_fetched_clear(&fetched);

	/* One request, to the source's own address, with the key in the header
	 * (and as the address's parameter, as Prowlarr wrote it). */
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 1);
	header = axis_test_plex_last_header(axis_test_prowlarr_server(fx->fake), "/3/download", "X-Api-Key");
	g_assert_cmpstr(header, ==, KEY);
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->fake), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(axis_test_prowlarr_server(fx->fake)));
}

static void
test_fetch_a_torrent_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.Remux", "torrent", "hearth-torrent", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autoptr(GBytes) expected = axis_test_download_torrent("Hearth.1995", &hash);

	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_cmpint(fetched.protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_null(fetched.magnet);
	g_assert_true(g_bytes_equal(fetched.file, expected));
	g_assert_cmpstr(fetched.info_hash, ==, hash);
	g_assert_cmpstr(fetched.file_name, ==, "Hearth.1995.Remux.torrent");
	axis_indexer_fetched_clear(&fetched);
}

/* A magnet link the search gave is handed over without asking the source. */
static void
test_fetch_a_stored_magnet_link(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.UHD", "torrent", "hearth-torrent",
		"magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Hearth&tr=http%3A%2F%2Ftracker.test%2Fa%3Fpasskey%3Dpk-secret-4f2a");
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	guint before = axis_test_prowlarr_n_requests(fx->fake);

	g_assert_true(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 0))->has_magnet);
	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_null(fetched.file);
	g_assert_true(g_str_has_prefix(fetched.magnet, "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Hearth"));
	g_assert_nonnull(strstr(fetched.magnet, "pk-secret-4f2a"));
	g_assert_cmpstr(fetched.info_hash, ==, "0123456789abcdef0123456789abcdef01234567");
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, before);
	axis_indexer_fetched_clear(&fetched);
}

/* Spec: Redirect elsewhere (a magnet link); task 4.2 */
static void
test_a_redirect_to_a_magnet_link_is_read_and_not_followed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.720p", "torrent", "hearth-magnet", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_null(fetched.file);
	g_assert_cmpstr(fetched.magnet, ==, AXIS_TEST_PROWLARR_REDIRECT_MAGNET);
	g_assert_cmpstr(fetched.info_hash, ==, AXIS_TEST_PROWLARR_REDIRECT_HASH);
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 1);
	axis_indexer_fetched_clear(&fetched);
}

/* A redirect to a "magnet link" that is not one is refused. */
static void
test_a_redirect_to_a_bad_magnet_link_is_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.720p", "torrent", "hearth-bad-magnet",
		NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;

	g_assert_false(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
	assert_error_is_clean(error);
	g_assert_null(fetched.file);
	g_assert_null(fetched.magnet);

	/* And a Usenet release is never a magnet link. */
	g_clear_error(&error);
	{
		g_autofree gchar *usenet = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.USENET", "usenet",
			"hearth-magnet", NULL);
		AxisIndexerRequest request;
		g_autoptr(AxisIndexerSearchResult) other = NULL;

		axis_test_prowlarr_set_search(fx->fake, NULL, usenet);
		film_request(&request);
		request.year = 1996;
		other = search_ok(fx, &request);
		g_assert_false(axis_indexers_fetch(fx->indexers, only_release_id(other), NULL, &fetched, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
	}
}

/* Spec: Redirect elsewhere; task 4.2 */
static void
test_a_redirect_elsewhere_is_refused_and_nothing_is_sent_there(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *other_base = axis_test_plex_base(other);
	g_autofree gchar *target = g_strconcat(other_base, "/stolen?x=1", NULL);
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.1080p", "usenet", "hearth-elsewhere",
		NULL);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;

	axis_test_prowlarr_set_elsewhere(fx->fake, target);
	result = search_one(fx, json);
	g_assert_false(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT);
	assert_error_is_clean(error);
	g_assert_null(strstr(error->message, other_base));
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 1);
	g_assert_null(fetched.file);
	axis_test_plex_free(other);
}

/* A redirect within the origin is followed, within the limit. */
static void
test_a_redirect_within_the_origin_is_followed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.1080p", "usenet", "hearth-moved", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 2);
	axis_indexer_fetched_clear(&fetched);
}

/* Spec: Not a release file; task 4.4 */
static void
test_what_is_not_a_release_file_is_refused(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	static const struct
	{
		const gchar *protocol;
		const gchar *file;
		gint         code;
	} cases[] = {
		{ "usenet", "hearth-html", AXIS_INDEXER_ERROR_REFUSED },
		{ "torrent", "hearth-html", AXIS_INDEXER_ERROR_REFUSED },
		{ "torrent", "hearth-nzb", AXIS_INDEXER_ERROR_REFUSED },
		{ "usenet", "hearth-torrent", AXIS_INDEXER_ERROR_REFUSED },
		{ "usenet", "hearth-huge", AXIS_INDEXER_ERROR_TOO_LARGE },
		{ "usenet", "hearth-gone", AXIS_INDEXER_ERROR_REFUSED }
	};
	guint i;

	film_request(&request);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995", cases[i].protocol,
			cases[i].file, NULL);
		g_autoptr(AxisIndexerSearchResult) result = NULL;

		axis_test_prowlarr_set_search(fx->fake, NULL, json);
		request.year = (gint)(1990 + i);
		result = search_ok(fx, &request);
		g_assert_false(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
		if (!g_error_matches(error, AXIS_INDEXER_ERROR, cases[i].code))
			g_error("%s %s: expected code %d, got %s", cases[i].protocol, cases[i].file, cases[i].code,
				error != NULL ? error->message : "success");
		assert_error_is_clean(error);
		g_assert_null(fetched.file);
		g_assert_null(fetched.magnet);
		g_clear_error(&error);
	}
	/* A refused file is not the source failing. */
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
}

/* Spec: Address elsewhere; task 4.2 */
static void
test_an_address_elsewhere_is_never_stored_or_fetched(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerRequest request;
	AxisTestPlex *other = axis_test_plex_new();
	g_autofree gchar *other_base = axis_test_plex_base(other);
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	g_autofree gchar *fixed = g_strdup_printf("[{\"title\":\"Hearth.1995.ELSEWHERE\",\"protocol\":\"usenet\","
		"\"downloadUrl\":\"%s/3/download?apikey=%s&file=hearth-nzb\"},"
		"{\"title\":\"Hearth.1995.HERE\",\"protocol\":\"usenet\",\"downloadUrl\":\"%s/3/download?file=hearth-nzb\"}]",
		other_base, KEY, base);

	axis_test_prowlarr_set_search(fx->fake, NULL, fixed);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 1);
	g_assert_cmpuint(result->n_off_origin, ==, 1);
	g_assert_cmpstr(((AxisIndexerRelease *)g_ptr_array_index(result->releases, 0))->name, ==, "Hearth.1995.HERE");
	/* The address that was elsewhere is nowhere in the database. */
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release_secret WHERE fetch_ref LIKE '%stolen%' "
		"OR fetch_ref LIKE 'http%'");
	assert_q(fx, "1", "SELECT count(*) FROM indexer_release_secret");
	g_assert_cmpuint(axis_test_plex_n_requests(other), ==, 0);
	axis_test_plex_free(other);
}

static void
test_a_stored_release_can_be_fetched_until_its_search_expires(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.1080p", "usenet", "hearth-nzb", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	gint64 id = only_release_id(result);
	AxisIndexerFetched fetched;
	AxisIndexerRelease *release = NULL;
	g_autoptr(GError) error = NULL;
	guint n = 0;

	g_assert_true(axis_indexers_get_release(fx->indexers, id, &release, &error));
	g_assert_cmpstr(release->name, ==, "Hearth.1995.1080p");
	axis_indexer_release_free(release);

	/* Not there, or past its life. */
	g_assert_false(axis_indexers_fetch(fx->indexers, id + 1000, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	axis_test_db_exec(fx->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(axis_indexers_get_release(fx->indexers, id, &release, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 0);

	/* The job's work forgets the search, its releases and their secrets. */
	assert_q(fx, "1", "SELECT count(*) FROM indexer_release_secret");
	g_assert_true(axis_indexers_expire(fx->indexers, &n, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(n, ==, 1);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_search");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release_secret");
	g_assert_true(axis_indexers_expire(fx->indexers, &n, &error));
	g_assert_cmpuint(n, ==, 0);
}

static void
test_a_release_of_a_source_switched_off_is_not_fetched(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.1080p", "usenet", "hearth-nzb", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerSourceSpec spec;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);

	memset(&spec, 0, sizeof(spec));
	spec.name = "Prowlarr";
	spec.address = base;
	spec.enabled = FALSE;
	g_assert_true(axis_indexers_save_source(fx->indexers, fx->source_id, &spec, NULL, &error));
	g_assert_false(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_NO_SOURCE);
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 0);
}

/* A magnet link of about 1.1 kB, as Prowlarr's redirects give, is read
 * from the redirect whole. */
static void
test_a_long_magnet_link_is_read(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995.720p", "torrent", "hearth-long-magnet",
		NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.magnet);
	g_assert_cmpuint(strlen(fetched.magnet), >, 1000);
	g_assert_cmpstr(fetched.info_hash, ==, AXIS_TEST_PROWLARR_REDIRECT_HASH);
	axis_indexer_fetched_clear(&fetched);
}

/* ── The recorded answers of a real Prowlarr ────────────────────── */

/* Spec: Search (the mapping of Prowlarr's fields), against what a real
 * Prowlarr 3.0.1 answered; tasks 1.1, 1.3, 2.1. */
static void
test_parse_recorded(void)
{
	g_autofree gchar *text = fixture_file("recorded/search-movie-text.json", "", "");
	g_autofree gchar *tv = fixture_file("recorded/search-tv-tvdb.json", "", "");
	g_autofree gchar *list = fixture_file("recorded/indexer.json", "", "");
	g_autofree gchar *status = fixture_file("recorded/indexerstatus.json", "", "");
	g_autofree gchar *system = fixture_file("recorded/system-status.json", "", "");
	g_autofree gchar *version = NULL;
	g_autoptr(GPtrArray) indexers = NULL;
	g_autoptr(GArray) failing = NULL;
	g_autoptr(GHashTable) keys = g_hash_table_new(g_str_hash, g_str_equal);
	AxisHttpOrigin origin;
	AxisIndexerOutcome outcome;
	AxisIndexerFound *found;
	guint torrents = 0;
	guint i;

	g_assert_true(axis_http_origin_parse(AXIS_TEST_PROWLARR_RECORDED_ORIGIN, &origin, NULL));
	g_assert_true(axis_prowlarr_parse_search(text, strlen(text), &origin, "", 0, &outcome, NULL));
	/* Every result is kept: nothing is dropped for being a link address. */
	g_assert_cmpuint(outcome.items->len, ==, 10);
	g_assert_cmpuint(outcome.n_unusable, ==, 0);
	g_assert_cmpuint(outcome.n_off_origin, ==, 0);
	g_assert_cmpuint(outcome.n_skipped, ==, 0);
	for (i = 0; i < outcome.items->len; i++) {
		found = g_ptr_array_index(outcome.items, i);
		g_assert_nonnull(found->fetch_path);
		g_assert_nonnull(found->merge_key);
		g_assert_false(g_hash_table_contains(keys, found->merge_key));
		g_hash_table_add(keys, found->merge_key);
		torrents += found->release->protocol == AXIS_DOWNLOAD_TORRENT;
	}
	g_assert_cmpuint(torrents, ==, 6);

	/* Known only by its link address (magnetUrl): the address is kept, with
	 * the hash the search stated, in lower case. */
	found = found_named(&outcome, "Hearth (1995) 1080p BrRip x264 -YIFY");
	g_assert_nonnull(found);
	g_assert_cmpint(found->release->protocol, ==, AXIS_DOWNLOAD_TORRENT);
	g_assert_true(g_str_has_prefix(found->fetch_path, "/3/download?apikey=" AXIS_TEST_PROWLARR_RECORDED_KEY "&link="));
	g_assert_nonnull(strstr(found->fetch_path, "&file=Hearth+(1995)+1080p+BrRip+x264+-YIFY"));
	/* The link wraps the whole magnet: the recording keeps its real length,
	 * past the 2,048 bytes that were once the limit (and dropped every such
	 * release on a real Prowlarr). */
	g_assert_cmpuint(strlen(found->fetch_path), >, 2048);
	g_assert_cmpuint(strlen(found->fetch_path), <=, AXIS_INDEXER_FETCH_PATH_MAX);
	g_assert_null(found->magnet);
	g_assert_false(found->release->has_magnet);
	g_assert_cmpstr(found->release->info_hash, ==, "f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4");
	g_assert_cmpint(found->release->seeders, >=, 0);

	/* Both addresses: the file's is kept. */
	found = found_named(&outcome, "Hearth (1995) 720p BRRip x264 -YTS");
	g_assert_nonnull(found);
	g_assert_true(g_str_has_prefix(found->fetch_path, "/32/download?"));
	g_assert_cmpstr(found->release->info_hash, ==, "e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4e1e2e3e4");
	g_assert_nonnull(found->release->flags);
	g_assert_cmpstr(found->release->flags[0], ==, "freeleech");
	g_assert_cmpstr(found->release->imdb_id, ==, "tt0600001");
	/* tmdbId 0 is none. */
	g_assert_cmpint(found->release->tmdb_id, ==, 0);

	/* A Usenet release, as NewsOwl gives it through Prowlarr. */
	found = found_named(&outcome, "Hearth.1995.2160p.UHD.BluRay.DTS-HD.MA.5.1.HDR10.x265-W4NK3R.DUAL-AFLA");
	g_assert_nonnull(found);
	g_assert_cmpint(found->release->protocol, ==, AXIS_DOWNLOAD_USENET);
	g_assert_cmpint(found->release->size_bytes, ==, G_GINT64_CONSTANT(43503367000));
	g_assert_cmpint(found->release->grabs, ==, 223);
	g_assert_cmpint(found->release->indexer_id, ==, 27);
	g_assert_cmpuint(found->release->n_categories, ==, 4);
	g_assert_cmpint(found->release->categories[0], ==, 2000);
	g_assert_null(found->release->flags);
	axis_indexer_outcome_clear(&outcome);

	/* An episode: imdbId 0 is none, and the same name from two indexers is
	 * two results. */
	g_assert_true(axis_prowlarr_parse_search(tv, strlen(tv), &origin, "", 0, &outcome, NULL));
	g_assert_cmpuint(outcome.items->len, ==, 4);
	found = g_ptr_array_index(outcome.items, 0);
	g_assert_null(found->release->imdb_id);
	g_assert_cmpint(found->release->tvdb_id, ==, 82001);
	found = g_ptr_array_index(outcome.items, 2);
	g_assert_cmpstr(found->release->imdb_id, ==, "tt0700001");
	g_assert_cmpstr(found->release->name, ==, ((AxisIndexerFound *)g_ptr_array_index(outcome.items, 0))->release->name);
	g_assert_cmpstr(found->merge_key, !=, ((AxisIndexerFound *)g_ptr_array_index(outcome.items, 0))->merge_key);
	axis_indexer_outcome_clear(&outcome);

	/* The indexers: what each takes, and which redirect their downloads. */
	g_assert_true(axis_prowlarr_parse_indexers(list, strlen(list), &indexers, NULL));
	g_assert_cmpuint(indexers->len, ==, 11);
	for (i = 0; i < indexers->len; i++) {
		const AxisIndexerInfo *info = g_ptr_array_index(indexers, i);

		g_assert_cmpint(info->redirect, ==, info->id == 27 || info->id == 29);
		if (info->id == 29) {
			g_assert_cmpuint(info->movie_ids, ==, AXIS_INDEXER_IDS_IMDB);
			g_assert_cmpuint(info->tv_ids, ==, AXIS_INDEXER_IDS_IMDB | AXIS_INDEXER_IDS_TMDB | AXIS_INDEXER_IDS_TVDB);
			g_assert_cmpint(info->protocol, ==, AXIS_DOWNLOAD_USENET);
		}
		if (info->id == 3)
			g_assert_cmpuint(info->movie_ids, ==, 0);
		if (info->id == 41)
			g_assert_false(info->enabled);
	}
	g_assert_true(axis_prowlarr_parse_failing(status, strlen(status), g_get_real_time(), &failing, NULL));
	g_assert_cmpuint(failing->len, ==, 0);
	version = axis_prowlarr_parse_version(system, strlen(system));
	g_assert_cmpstr(version, ==, "3.0.1.5752");
	axis_http_origin_clear(&origin);
}

/* Spec: An identifier search does not lose indexers that only take text
 * (Mixed indexers), against the recorded answers: the four indexers that
 * are on and take no IMDb identifier are asked by the title, and only they;
 * the answers are merged, each release once; task 5.5. */
static void
test_recorded_identifier_and_title(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;

	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpint(result->mode, ==, AXIS_INDEXER_BY_IMDB);
	g_assert_true(result->also_text);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 2);
	first = axis_test_prowlarr_search_query(fx->fake, 0);
	g_assert_cmpstr(first, ==, "query=%7BImdbId%3Att0600001%7D&type=movie&categories=2000&limit=1000");
	second = axis_test_prowlarr_search_query(fx->fake, 1);
	g_assert_cmpstr(second, ==, "query=Hearth%201995&type=search&categories=2000&indexerIds=36&indexerIds=42"
		"&indexerIds=37&indexerIds=3&limit=1000");
	/* Six by the identifier, then four the title found (The Pirate Bay's
	 * and OpenShelf's), each once; and one of The Pirate Bay's is the torrent
	 * YTS gave already (the same info-hash): add-indexer-newznab lists it
	 * once, as YTS's, with The Pirate Bay's as its alternative. */
	g_assert_cmpuint(result->releases->len, ==, 9);
	g_assert_cmpuint(result->n_merged, ==, 1);
	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);
		gchar *key = g_strdup_printf("%" G_GINT64_FORMAT "|%s", release->indexer_id, release->name);

		g_assert_false(g_hash_table_contains(seen, key));
		g_hash_table_add(seen, key);
		if (i < 6)
			g_assert_true(release->indexer_id == 27 || release->indexer_id == 29 || release->indexer_id == 32);
		else
			g_assert_true(release->indexer_id == 3 || release->indexer_id == 36);
		if (g_strcmp0(release->info_hash, "f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4f1f2f3f4") == 0) {
			g_assert_cmpint(release->indexer_id, ==, 32);
			g_assert_cmpuint(release->n_alternatives, ==, 1);
			g_assert_nonnull(release->also_at);
			g_assert_cmpstr(release->also_at[0], ==, "The Pirate Bay through Prowlarr");
		} else {
			g_assert_cmpuint(release->n_alternatives, ==, 0);
		}
	}
	assert_q(fx, "2", "SELECT count(*) FROM indexer_source_indexer WHERE redirect");
	assert_q(fx, "t", "SELECT also_text FROM indexer_search");
}

/* Spec: A release offered as a link is kept (Link address; Wrong torrent);
 * task 5.1: every torrent of a recorded search by words that has a link
 * address is kept, fetches to its magnet link (checked against the hash
 * the search stated), and one whose redirect names another torrent is
 * refused. */
static void
test_recorded_links_fetch_to_magnets(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(GArray) links = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint files = 0;
	guint i;

	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = "Hearth 1995";
	result = search_ok(fx, &request);
	/* Ten, less YTS's 1080p torrent, which is The Pirate Bay's YIFY one (the
	 * same info-hash) and listed once, as The Pirate Bay's. */
	g_assert_cmpuint(result->releases->len, ==, 9);
	g_assert_cmpuint(result->n_merged, ==, 1);
	g_assert_cmpuint(result->n_unusable, ==, 0);
	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);
		AxisIndexerFetched fetched;
		g_autoptr(GError) error = NULL;

		if (release->protocol != AXIS_DOWNLOAD_TORRENT)
			continue;
		g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
		g_assert_no_error(error);
		if (release->indexer_id == 3) {
			/* The Pirate Bay's: a link address, read as its magnet link. */
			g_assert_null(fetched.file);
			g_assert_nonnull(fetched.magnet);
			g_assert_cmpuint(strlen(fetched.magnet), >, 1000);
			g_assert_cmpstr(fetched.info_hash, ==, release->info_hash);
			g_array_append_val(links, release->id);
		} else {
			/* YTS's and OpenShelf's: the file. */
			g_assert_nonnull(fetched.file);
			g_assert_null(fetched.magnet);
			files++;
		}
		axis_indexer_fetched_clear(&fetched);
	}
	g_assert_cmpuint(links->len, ==, 2);
	g_assert_cmpuint(files, ==, 3);

	/* The redirect names another torrent than the search said: refused.
	 * The one YTS found too (the same info-hash) is fetched from YTS
	 * instead (add-indexer-newznab: an alternative, tried before the
	 * release fails). */
	axis_test_prowlarr_set_wrong_magnets(fx->fake, TRUE);
	for (i = 0; i < links->len; i++) {
		AxisIndexerFetched fetched;
		g_autoptr(GError) error = NULL;
		gint64 id = g_array_index(links, gint64, i);
		AxisIndexerRelease *release = NULL;
		guint k;

		for (k = 0; k < result->releases->len; k++) {
			if (((AxisIndexerRelease *)g_ptr_array_index(result->releases, k))->id == id)
				release = g_ptr_array_index(result->releases, k);
		}
		g_assert_nonnull(release);
		if (release->n_alternatives > 0) {
			g_assert_true(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
			g_assert_no_error(error);
			g_assert_cmpint(fetched.release_id, !=, id);
			g_assert_nonnull(fetched.file);
			axis_indexer_fetched_clear(&fetched);
			continue;
		}
		g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
		g_assert_nonnull(strstr(error->message, "another torrent"));
		assert_error_is_clean(error);
		g_assert_null(fetched.magnet);
	}
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
}

/* ── An indexer's download redirect ─────────────────────────────── */

/* The indexer's site: TLS with the tests' authority, serving @body (an
 * NZB unless said) at its path; %NULL when TLS is not available here. */
static AxisTestPlex *
site_new(GTlsDatabase **database)
{
	AxisTestPlex *site = axis_test_plex_new_tls("tests/fixtures/compat/tls/cert.pem",
		"tests/fixtures/compat/tls/key.pem");
	g_autofree gchar *anchors = g_canonicalize_filename("tests/fixtures/compat/tls/cert.pem", NULL);
	g_autoptr(GError) error = NULL;

	*database = NULL;
	if (site == NULL)
		return NULL;
	*database = g_tls_file_database_new(anchors, &error);
	g_assert_no_error(error);
	return site;
}

static void
site_serves(
	AxisTestPlex *site,
	const gchar  *type,
	const gchar  *body
){
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

	axis_test_plex_clear_routes(site);
	axis_test_plex_route_full(site, AXIS_TEST_PROWLARR_SITE_PATH, 200, type, bytes, NULL);
}

/* The first release of @result from NewsOwl, whose downloads redirect. */
static gint64
redirecting_release(const AxisIndexerSearchResult *result)
{
	guint i;

	for (i = 0; i < result->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(result->releases, i);

		if (release->indexer_id == 27)
			return release->id;
	}
	g_assert_not_reached();
	return 0;
}

/* The indexer's key is nowhere Axis keeps or says anything. */
static void
assert_indexer_key_nowhere(Fixture *fx)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO, G_LOG_LEVEL_DEBUG
	};
	guint i;

	assert_q(fx, "0", "SELECT count(*) FROM indexer_release_secret WHERE coalesce(fetch_ref, '') LIKE '%"
		AXIS_TEST_PROWLARR_INDEXER_KEY "%' OR coalesce(magnet, '') LIKE '%" AXIS_TEST_PROWLARR_INDEXER_KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source WHERE coalesce(state_message, '') LIKE '%"
		AXIS_TEST_PROWLARR_INDEXER_KEY "%' OR coalesce(state_message, '') LIKE '%" AXIS_TEST_PROWLARR_SITE_PATH "?%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release WHERE name LIKE '%" AXIS_TEST_PROWLARR_INDEXER_KEY "%'");
	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		g_assert_cmpuint(axis_test_log_count(levels[i], "*" AXIS_TEST_PROWLARR_INDEXER_KEY "*"), ==, 0);
}

/*
 * Spec: An indexer's download redirect is followed once, with nothing of
 * Axis's (Followed); task 5.2.  Following is on by default: a Usenet release
 * of NewsOwl, whose downloads redirect, is fetched from the indexer's site,
 * once, with nothing of the source's; the address is kept nowhere.
 */
static void
test_redirect_followed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GTlsDatabase) database = NULL;
	AxisTestPlex *site = site_new(&database);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *transcript = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	gsize length = 0;

	if (site == NULL) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	base = axis_test_plex_base(site);
	site_serves(site, "application/x-nzb", AXIS_TEST_PROWLARR_NZB);
	axis_test_prowlarr_set_indexer_site(fx->fake, base);
	axis_http_set_offsite_testing(TRUE, database);

	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_true(source->follow_redirects);
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_true(axis_indexers_fetch(fx->indexers, redirecting_release(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	g_assert_cmpmem(g_bytes_get_data(fetched.file, &length), length, AXIS_TEST_PROWLARR_NZB,
		strlen(AXIS_TEST_PROWLARR_NZB));
	g_assert_true(g_str_has_suffix(fetched.file_name, ".nzb"));
	axis_indexer_fetched_clear(&fetched);

	/* One redirect from the source, one request at the site, with the
	 * indexer's own address and nothing of the source's. */
	g_assert_cmpuint(axis_test_prowlarr_n_site_redirects(fx->fake), ==, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(site), ==, 1);
	transcript = axis_test_plex_transcript(site);
	g_assert_nonnull(strstr(transcript, AXIS_TEST_PROWLARR_INDEXER_KEY));
	g_assert_null(strstr(transcript, KEY));
	g_assert_null(strstr(transcript, "X-Api-Key"));
	g_assert_null(strstr(transcript, "Cookie"));
	g_assert_null(strstr(transcript, "Accept:"));
	assert_indexer_key_nowhere(fx);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	axis_test_plex_free(site);
}

/* Spec: Switched off; task 5.4.  With following switched off for the source
 * the fetch fails saying why and how to resolve it, and nothing is sent to
 * the indexer's site; the source is not counted as failing. */
static void
test_redirect_switched_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GTlsDatabase) database = NULL;
	AxisTestPlex *site = site_new(&database);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *address = axis_test_prowlarr_base(fx->fake);
	AxisIndexerSourceSpec spec;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;

	if (site == NULL) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	base = axis_test_plex_base(site);
	site_serves(site, "application/x-nzb", AXIS_TEST_PROWLARR_NZB);
	axis_test_prowlarr_set_indexer_site(fx->fake, base);
	axis_http_set_offsite_testing(TRUE, database);
	memset(&spec, 0, sizeof(spec));
	spec.name = "Prowlarr";
	spec.address = address;
	spec.enabled = TRUE;
	spec.refuse_redirects = TRUE;
	g_assert_true(axis_indexers_save_source(fx->indexers, fx->source_id, &spec, NULL, &error));
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_false(source->follow_redirects);

	film_request(&request);
	result = search_ok(fx, &request);
	g_clear_pointer(&source, axis_indexer_source_info_free);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_true(axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_USENET));
	g_assert_true(axis_indexer_source_all_redirects_refused(source, AXIS_DOWNLOAD_USENET));
	g_assert_false(axis_indexer_source_redirects_refused(source, AXIS_DOWNLOAD_TORRENT));

	g_assert_false(axis_indexers_fetch(fx->indexers, redirecting_release(result), NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT);
	g_assert_nonnull(strstr(error->message, "redirects this release's download to the indexer's own site"));
	g_assert_nonnull(strstr(error->message, "turn off Redirect for that indexer in Prowlarr"));
	g_assert_nonnull(strstr(error->message, "allow following download redirects"));
	assert_error_is_clean(error);
	g_assert_null(strstr(error->message, AXIS_TEST_PROWLARR_INDEXER_KEY));
	g_assert_null(fetched.file);
	g_assert_cmpuint(axis_test_plex_n_requests(site), ==, 0);
	assert_indexer_key_nowhere(fx);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	axis_test_plex_free(site);
}

/* Spec: Inside the network; task 5.2.  With the tests' allowance off, a
 * redirect to the indexer's "site" on loopback, by its address or by a name
 * that resolves to it, is refused with nothing sent there. */
static void
test_redirect_inside_the_network(Fixture *fx, gconstpointer data)
{
	g_autoptr(GTlsDatabase) database = NULL;
	AxisTestPlex *site = site_new(&database);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerRequest request;
	const gchar *hosts[] = { "127.0.0.1", "localhost", NULL };
	guint i;

	if (site == NULL) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	site_serves(site, "application/x-nzb", AXIS_TEST_PROWLARR_NZB);
	axis_http_set_offsite_testing(FALSE, database);
	film_request(&request);
	result = search_ok(fx, &request);
	for (i = 0; hosts[i] != NULL; i++) {
		g_autofree gchar *base = g_strdup_printf("https://%s:%u", hosts[i], axis_test_plex_port(site));
		AxisIndexerFetched fetched;
		g_autoptr(GError) error = NULL;

		axis_test_prowlarr_set_indexer_site(fx->fake, base);
		g_assert_false(axis_indexers_fetch(fx->indexers, redirecting_release(result), NULL, &fetched, &error));
		g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT);
		g_assert_nonnull(strstr(error->message, "not at a public address"));
		assert_error_is_clean(error);
		g_assert_null(strstr(error->message, AXIS_TEST_PROWLARR_INDEXER_KEY));
	}
	g_assert_cmpuint(axis_test_plex_n_requests(site), ==, 0);
	assert_indexer_key_nowhere(fx);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	axis_test_plex_free(site);
}

/* Task 5.3: at the indexer's site, a second redirect, an answer beyond the
 * cap, one that is not a release file and a refusal are each the fetch's
 * failure, and none counts against the source. */
static void
test_redirect_limits(Fixture *fx, gconstpointer data)
{
	g_autoptr(GTlsDatabase) database = NULL;
	AxisTestPlex *site = site_new(&database);
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *again = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	gint64 id;

	if (site == NULL) {
		g_test_skip("the TLS test server could not start");
		return;
	}
	base = axis_test_plex_base(site);
	axis_test_prowlarr_set_indexer_site(fx->fake, base);
	axis_http_set_offsite_testing(TRUE, database);
	film_request(&request);
	result = search_ok(fx, &request);
	id = redirecting_release(result);

	/* Redirected again. */
	again = g_strconcat(base, "/elsewhere", NULL);
	axis_test_plex_clear_routes(site);
	axis_test_plex_route(site, AXIS_TEST_PROWLARR_SITE_PATH, -1, AXIS_TEST_PLEX_REDIRECT, 302, again, 0);
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REDIRECT);
	assert_error_is_clean(error);
	g_assert_cmpuint(axis_test_plex_count(site, "/elsewhere"), ==, 0);
	g_clear_error(&error);

	/* Larger than a release may be. */
	axis_test_plex_clear_routes(site);
	axis_test_plex_route(site, AXIS_TEST_PROWLARR_SITE_PATH, -1, AXIS_TEST_PLEX_OVERSIZE, 200, NULL,
		AXIS_INDEXER_MAX_FILE_BYTES + 1);
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_TOO_LARGE);
	assert_error_is_clean(error);
	g_clear_error(&error);

	/* Not a release file (a login page). */
	site_serves(site, "application/x-nzb", "<html><body>Please log in</body></html>");
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "not an NZB"));
	g_clear_error(&error);

	/* The site's refusal is not the source refusing its key. */
	axis_test_plex_clear_routes(site);
	axis_test_plex_route(site, AXIS_TEST_PROWLARR_SITE_PATH, -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "indexer's site"));
	assert_error_is_clean(error);
	g_clear_error(&error);

	assert_indexer_key_nowhere(fx);
	assert_q(fx, "ok", "SELECT state FROM indexer_source");
	axis_test_plex_free(site);
}

/* ── Secrets ────────────────────────────────────────────────────── */

/* Spec: Key in a fetch address; task 4.3 */
static void
test_fetch_references_are_secrets_and_expire_with_the_search(Fixture *fx, gconstpointer data)
{
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSearchResult) again = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *shown = NULL;
	guint i;

	film_request(&request);
	result = search_ok(fx, &request);
	/* Every kept release has a way to fetch it, held in the secrets table. */
	assert_q(fx, "8", "SELECT count(*) FROM indexer_release_secret");
	assert_q(fx, "7", "SELECT count(*) FROM indexer_release_secret WHERE fetch_ref LIKE '%apikey=" KEY "%'");
	assert_q(fx, "2", "SELECT count(*) FROM indexer_release_secret WHERE magnet IS NOT NULL");
	/* Nothing else in the database holds one. */
	assert_q(fx, "0", "SELECT count(*) FROM indexer_release WHERE name LIKE '%" KEY "%' OR indexer_name LIKE '%" KEY "%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_search WHERE failed_names::text LIKE '%apikey%'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source WHERE state_message LIKE '%" KEY "%'");

	/* What a listing gives back (a fresh search, then the stored one) names
	 * no address and no link: its releases hold their words and figures. */
	again = search_ok(fx, &request);
	g_assert_true(again->cached);
	for (i = 0; i < again->releases->len; i++) {
		const AxisIndexerRelease *release = g_ptr_array_index(again->releases, i);

		shown = g_strdup_printf("%s %s %s %s", release->name, release->indexer_name != NULL ? release->indexer_name : "",
			release->imdb_id != NULL ? release->imdb_id : "", release->info_hash != NULL ? release->info_hash : "");
		g_assert_null(strstr(shown, "apikey"));
		g_assert_null(strstr(shown, KEY));
		g_assert_null(strstr(shown, "pk-secret"));
		g_clear_pointer(&shown, g_free);
	}
}

/* Task 4.3: no log line holds a key, an address of a release, or what was
 * searched for */
static void
test_no_log_line_holds_a_secret_or_the_search_words(Fixture *fx, gconstpointer data)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	static const gchar *const needles[] = {
		"*" KEY "*", "*apikey*", "*/download?*", "*pk-secret*", "*link=*", "*Hearth*", "*tt0600001*", "*Remux*", NULL
	};
	AxisIndexerRequest request;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerSearchResult *failed_result = NULL;
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	guint i;
	guint j;

	/* Searches, a fetch, failures of each kind. */
	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_true(axis_indexers_fetch(fx->indexers,
		((AxisIndexerRelease *)release_named(result, "Hearth.1995.Remux-ONLY"))->id, NULL, &fetched, &error));
	axis_indexer_fetched_clear(&fetched);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));
	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_BROKEN);
	request.year = 1996;
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &failed_result, &error));
	g_clear_error(&error);
	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_NORMAL);
	axis_test_prowlarr_set_key(fx->fake, OTHER_KEY);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));

	/* This layer makes no job; whatever the queue holds holds no key. */
	assert_q(fx, "0", "SELECT count(*) FROM job WHERE payload::text LIKE '%" KEY "%' OR payload::text LIKE '%apikey%'"
		" OR coalesce(last_error, '') LIKE '%" KEY "%' OR coalesce(progress_message, '') LIKE '%" KEY "%'");
	for (i = 0; needles[i] != NULL; i++) {
		for (j = 0; j < G_N_ELEMENTS(levels); j++) {
			if (axis_test_log_count(levels[j], needles[i]) > 0) {
				g_autofree gchar *line = axis_test_log_find(levels[j], needles[i]);

				g_error("a log line at level %d matches %s: %s", (gint)levels[j], needles[i], line);
			}
		}
	}
}

/* Spec: Key in a fetch address (the error messages of a failed fetch) */
static void
test_failed_fetches_say_nothing_of_the_key_or_the_address(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *json = axis_test_prowlarr_release_json(fx->fake, "Hearth.1995", "usenet", "hearth-nzb", NULL);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;
	gint64 id = only_release_id(result);

	/* The source stops answering, then refuses the key, then breaks. */
	axis_test_prowlarr_set_key(fx->fake, OTHER_KEY);
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_AUTH);
	assert_error_is_clean(error);
	g_clear_error(&error);
	axis_test_prowlarr_set_key(fx->fake, KEY);
	axis_test_prowlarr_set_mode(fx->fake, AXIS_TEST_PROWLARR_BROKEN);
	g_assert_false(axis_indexers_fetch(fx->indexers, id, NULL, &fetched, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_FAILED);
	assert_error_is_clean(error);
	g_clear_error(&error);
	assert_q(fx, "failing", "SELECT state FROM indexer_source");
	{
		g_autofree gchar *message = q(fx, "SELECT state_message FROM indexer_source");

		assert_no_secret_in(message);
	}
}

/* ── Politeness ─────────────────────────────────────────────────── */

typedef struct
{
	Fixture *fx;
	gint     year;
	AxisIndexerSearchResult *result;
	GError  *error;
} Job;

static gpointer
search_thread(gpointer data)
{
	Job *job = data;
	AxisIndexerRequest request;

	film_request(&request);
	request.year = job->year;
	axis_indexers_search(job->fx->indexers, 0, &request, NULL, &job->result, &job->error);
	return NULL;
}

/* Task 4.5: the rate limit holds under a burst */
static void
test_the_rate_limit_holds_under_a_burst(Fixture *fx, gconstpointer data)
{
	GThread *threads[6];
	Job jobs[6];
	AxisIndexerTuning tuning;
	g_autoptr(GArray) times = NULL;
	g_autoptr(GError) error = NULL;
	gint64 span;
	guint i;

	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_RATE, "2", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_BURST, "2", &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 5000;
	tuning.wait_ms = 30000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	/* The indexers are read first, so that the burst is searches only, one
	 * request each. */
	axis_test_prowlarr_set_indexers(fx->fake, ALL_TAKE_IMDB);
	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 3);
	for (i = 0; i < G_N_ELEMENTS(jobs); i++) {
		memset(&jobs[i], 0, sizeof(jobs[i]));
		jobs[i].fx = fx;
		jobs[i].year = (gint)(1990 + i);
		threads[i] = g_thread_new("search", search_thread, &jobs[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(jobs); i++) {
		g_thread_join(threads[i]);
		g_assert_no_error(jobs[i].error);
		g_assert_nonnull(jobs[i].result);
		axis_indexer_search_result_free(jobs[i].result);
	}
	/* Six searches after the three requests that read the indexers: of
	 * the six, two may go at once and then two a second, so the last is
	 * at least (6 - 2) / 2 = 2 s after the first, less what the bucket
	 * refilled in the meantime. */
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 9);
	times = axis_test_plex_request_times(axis_test_prowlarr_server(fx->fake));
	g_assert_cmpuint(times->len, ==, 9);
	span = g_array_index(times, gint64, 8) - g_array_index(times, gint64, 3);
	g_assert_cmpint(span, >=, G_GINT64_CONSTANT(1400000));
	/* The requests were not all at once. */
	g_assert_cmpint(g_array_index(times, gint64, 8) - g_array_index(times, gint64, 6), >=, G_GINT64_CONSTANT(700000));
}

/* Task 2.4: two identical searches at once ask once */
static void
test_identical_searches_at_once_ask_the_source_once(Fixture *fx, gconstpointer data)
{
	GThread *threads[4];
	Job jobs[4];
	guint cached = 0;
	guint i;

	axis_test_prowlarr_set_indexers(fx->fake, ALL_TAKE_IMDB);
	axis_test_prowlarr_set_search_delay(fx->fake, 700);
	for (i = 0; i < G_N_ELEMENTS(jobs); i++) {
		memset(&jobs[i], 0, sizeof(jobs[i]));
		jobs[i].fx = fx;
		jobs[i].year = 1995;
		threads[i] = g_thread_new("search", search_thread, &jobs[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(jobs); i++) {
		g_thread_join(threads[i]);
		g_assert_no_error(jobs[i].error);
		g_assert_nonnull(jobs[i].result);
		g_assert_cmpuint(jobs[i].result->releases->len, ==, 8);
		if (jobs[i].result->cached)
			cached++;
		axis_indexer_search_result_free(jobs[i].result);
	}
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 1);
	g_assert_cmpuint(cached, ==, 3);
}

/* Task 2.4: a cap on searches at once */
static void
test_calls_at_a_source_at_once_are_capped(Fixture *fx, gconstpointer data)
{
	AxisIndexerTuning tuning;
	g_autoptr(GError) error = NULL;
	AxisIndexerRequest request;
	AxisIndexerSearchResult *result = NULL;
	GThread *thread;
	Job job;

	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_MAX_SIMULTANEOUS, "1", &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 5000;
	tuning.wait_ms = 400;
	axis_indexers_set_tuning(fx->indexers, &tuning);

	/* One search is in progress at the source (it answers slowly); another
	 * does not get a place in the time it may wait, and sends nothing. */
	axis_test_prowlarr_set_indexers(fx->fake, ALL_TAKE_IMDB);
	axis_test_prowlarr_set_search_delay(fx->fake, 1800);
	memset(&job, 0, sizeof(job));
	job.fx = fx;
	job.year = 1995;
	thread = g_thread_new("search", search_thread, &job);
	while (axis_test_prowlarr_n_searches(fx->fake) == 0)
		g_usleep(10000);
	film_request(&request);
	request.year = 1996;
	g_assert_false(axis_indexers_search(fx->indexers, 0, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_BUSY);
	g_assert_cmpuint(axis_test_prowlarr_n_searches(fx->fake), ==, 1);
	g_thread_join(thread);
	g_assert_no_error(job.error);
	axis_indexer_search_result_free(job.result);
}

/* ── The base path ──────────────────────────────────────────────── */

/* Spec: base path as download clients have it; tasks 2.1 and 2.3 */
static void
test_a_source_served_under_a_base_path(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	g_autoptr(AxisIndexerSourceInfo) source = NULL;
	AxisIndexerRequest request;
	AxisIndexerFetched fetched;
	AxisIndexerRelease *release;

	g_assert_true(axis_indexers_test_source(fx->indexers, fx->source_id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_indexers_get_source(fx->indexers, fx->source_id, &source, &error));
	g_assert_cmpstr(source->base_path, ==, "/prowlarr");
	g_assert_cmpint(source->state, ==, AXIS_INDEXER_SOURCE_OK);

	film_request(&request);
	result = search_ok(fx, &request);
	g_assert_cmpuint(result->releases->len, ==, 8);
	release = release_named(result, "Hearth.1995.Remux-ONLY");
	g_assert_nonnull(release);
	g_assert_true(axis_indexers_fetch(fx->indexers, release->id, NULL, &fetched, &error));
	g_assert_no_error(error);
	g_assert_nonnull(fetched.file);
	axis_indexer_fetched_clear(&fetched);
	/* Every request was under the prefix, and carried the key. */
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->fake), ==, 0);
	assert_q(fx, "8", "SELECT count(*) FROM indexer_release_secret WHERE fetch_ref LIKE '/prowlarr/%' OR magnet IS NOT NULL");
}

/* An address of the source's own origin but outside its base path is not a fetch address. */
static void
test_an_address_outside_the_base_path_is_not_fetched(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	g_autofree gchar *json = g_strdup_printf("[{\"title\":\"Hearth.1995.OUTSIDE\",\"protocol\":\"usenet\","
		"\"downloadUrl\":\"%s/admin/delete?apikey=%s\"},{\"title\":\"Hearth.1995.TRAVERSAL\",\"protocol\":\"usenet\","
		"\"downloadUrl\":\"%s/prowlarr/../admin/delete?apikey=%s\"},{\"title\":\"Hearth.1995.INSIDE\",\"protocol\":"
		"\"usenet\",\"downloadUrl\":\"%s/prowlarr/3/download?apikey=%s&file=hearth-nzb\"}]", base, KEY, base, KEY, base,
		KEY);
	g_autoptr(AxisIndexerSearchResult) result = search_one(fx, json);
	AxisIndexerFetched fetched;
	g_autoptr(GError) error = NULL;

	/* Only the one under the base path is kept, and it fetches. */
	g_assert_cmpuint(result->releases->len, ==, 1);
	g_assert_cmpuint(result->n_unusable, ==, 2);
	g_assert_cmpuint(result->n_off_origin, ==, 0);
	g_assert_true(axis_indexers_fetch(fx->indexers, only_release_id(result), NULL, &fetched, &error));
	g_assert_no_error(error);
	axis_indexer_fetched_clear(&fetched);
	g_assert_cmpuint(axis_test_prowlarr_n_downloads(fx->fake), ==, 1);
	g_assert_cmpuint(axis_test_prowlarr_n_bad(fx->fake), ==, 0);
}

/* ── What the source files may name ─────────────────────────────── */

static gchar *
read_file(const gchar *path)
{
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static void
collect_sources(
	const gchar *dir,
	GPtrArray   *found
){
	g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
	const gchar *name;

	g_assert_nonnull(handle);
	while ((name = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);

		if (g_file_test(path, G_FILE_TEST_IS_DIR))
			collect_sources(path, found);
		else if (g_str_has_suffix(name, ".c") || g_str_has_suffix(name, ".h"))
			g_ptr_array_add(found, g_steal_pointer(&path));
	}
}

static guint
count_in(
	const gchar *text,
	const gchar *needle
){
	guint n = 0;
	const gchar *p = text;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/* Spec: Key in a fetch address (the secrets are read in one place); read-only
 * HTTP */
static void
test_secrets_are_read_in_one_place_and_only_get_is_used(void)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	guint fetch_ref_reads = 0;
	guint magnet_reads = 0;
	guint secret_table_files = 0;
	guint i;

	collect_sources("src", files);
	g_assert_cmpuint(files->len, >, 100);
	for (i = 0; i < files->len; i++) {
		const gchar *path = g_ptr_array_index(files, i);
		g_autofree gchar *text = read_file(path);
		gboolean mine = g_str_has_prefix(path, "src/indexer/") || g_str_has_prefix(path, "src/web/axis-web-indexers");

		if (strstr(text, "indexer_release_secret") != NULL) {
			secret_table_files++;
			g_assert_cmpstr(path, ==, "src/indexer/axis-indexer-store.c");
		}
		fetch_ref_reads += count_in(text, "k.fetch_ref");
		magnet_reads += count_in(text, "k.magnet");
		/* The indexer layer reads: no session, no POST, no download purpose. */
		if (mine) {
			g_assert_null(strstr(text, "axis_http_session_"));
			g_assert_null(strstr(text, "AXIS_HTTP_PURPOSE"));
			g_assert_null(strstr(text, "SOUP_METHOD_POST"));
			/* An indexer is asked through its connection only.  The one other
			 * reader is the fetch of definitions from Jackett's repository
			 * (add-definition-fetch): GitHub's two fixed hosts, no key. */
			if (strstr(text, "axis_http_get") != NULL && strcmp(path, "src/indexer/axis-indexer-jackett.c") != 0)
				g_assert_cmpstr(path, ==, "src/indexer/axis-indexer-conn.c");
			/* And does not call the release parser. */
			g_assert_null(strstr(text, "#include \"release/"));
			g_assert_null(strstr(text, "axis_release_parse"));
		}
	}
	g_assert_cmpuint(secret_table_files, ==, 1);
	/* The fetch reference is selected once (the fetch), the magnet link
	 * twice (whether there is one, and the fetch). */
	g_assert_cmpuint(fetch_ref_reads, ==, 1);
	g_assert_cmpuint(magnet_reads, ==, 2);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/indexer/words", test_words);
	g_test_add_func("/indexer/parse/search-mapping", test_parse_search_mapping);
	g_test_add_func("/indexer/parse/search-base-path", test_parse_search_base_path);
	g_test_add_func("/indexer/parse/search-capped", test_parse_search_capped);
	g_test_add_func("/indexer/parse/broken-answers", test_parse_broken_answers);
	g_test_add_func("/indexer/parse/indexers-and-status", test_parse_indexers_and_status);
	g_test_add_func("/indexer/parse/recorded", test_parse_recorded);
	g_test_add_func("/indexer/fetch-reference/rules", test_fetch_reference_rules);
	g_test_add_func("/indexer/search-path/forms", test_search_path_forms);
	g_test_add_func("/indexer/request/rules", test_request_rules);
	g_test_add_func("/indexer/request/anime", test_request_anime);
	g_test_add_func("/indexer/content-kind", test_content_kind);
	g_test_add_func("/indexer/names/file-names", test_names_and_file_names);
	g_test_add_func("/indexer/secrets/one-place", test_secrets_are_read_in_one_place_and_only_get_is_used);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/indexer/" path, Fixture, NULL, func)
#define ADD_BASE(path, func) AXIS_TEST_ADD_DB_CASE("/indexer/" path, Fixture, GUINT_TO_POINTER(1), func)
#define ADD_RECORDED(path, func) AXIS_TEST_ADD_DB_CASE("/indexer/" path, Fixture, GUINT_TO_POINTER(2), func)
	ADD("search/identifier-and-title", test_search_by_identifier_and_title);
	ADD("fetch/long-magnet", test_a_long_magnet_link_is_read);
	ADD_RECORDED("recorded/identifier-and-title", test_recorded_identifier_and_title);
	ADD_RECORDED("recorded/links", test_recorded_links_fetch_to_magnets);
	ADD_RECORDED("redirect/followed", test_redirect_followed);
	ADD_RECORDED("redirect/switched-off", test_redirect_switched_off);
	ADD_RECORDED("redirect/inside-the-network", test_redirect_inside_the_network);
	ADD_RECORDED("redirect/limits", test_redirect_limits);
	ADD("source/test", test_source_test_reads_version_and_indexers);
	ADD("source/key-in-header", test_the_key_is_sent_in_a_header_to_the_source_only);
	ADD("source/wrong-key", test_a_wrong_key_is_refused_without_naming_it);
	ADD("source/down", test_a_source_that_does_not_answer_is_a_state);
	ADD("source/unreadable", test_an_answer_that_is_not_a_list_is_unreadable);
	ADD("source/429", test_a_429_pauses_the_source);
	ADD("source/500", test_a_server_error_is_failing);
	ADD("source/save", test_saving_sources);
	ADD("source/change-forgets", test_changing_a_source_forgets_its_searches);
	ADD("search/releases-and-failures", test_search_returns_releases_and_failures);
	ADD("search/repeat", test_a_repeated_search_asks_once);
	ADD("search/cache-life", test_the_cache_life_is_a_setting);
	ADD("search/names", test_names_are_cleaned_and_cut);
	ADD("search/capped", test_results_are_capped_at_a_thousand);
	ADD("search/fallback", test_search_falls_back_to_the_title);
	ADD("search/no-fallback-over-dropped", test_search_does_not_fall_back_over_dropped_results);
	ADD("search/episodes", test_search_for_episodes_and_seasons);
	ADD("search/invalid-and-missing", test_invalid_and_missing_sources);
	ADD("fetch/nzb", test_fetch_an_nzb);
	ADD("fetch/torrent", test_fetch_a_torrent_file);
	ADD("fetch/stored-magnet", test_fetch_a_stored_magnet_link);
	ADD("fetch/magnet-redirect", test_a_redirect_to_a_magnet_link_is_read_and_not_followed);
	ADD("fetch/bad-magnet-redirect", test_a_redirect_to_a_bad_magnet_link_is_refused);
	ADD("fetch/redirect-elsewhere", test_a_redirect_elsewhere_is_refused_and_nothing_is_sent_there);
	ADD("fetch/redirect-same-origin", test_a_redirect_within_the_origin_is_followed);
	ADD("fetch/not-a-release", test_what_is_not_a_release_file_is_refused);
	ADD("fetch/address-elsewhere", test_an_address_elsewhere_is_never_stored_or_fetched);
	ADD("fetch/expiry", test_a_stored_release_can_be_fetched_until_its_search_expires);
	ADD("fetch/source-off", test_a_release_of_a_source_switched_off_is_not_fetched);
	ADD("secrets/stored-and-expire", test_fetch_references_are_secrets_and_expire_with_the_search);
	ADD("secrets/logs", test_no_log_line_holds_a_secret_or_the_search_words);
	ADD("secrets/errors", test_failed_fetches_say_nothing_of_the_key_or_the_address);
	ADD("politeness/rate", test_the_rate_limit_holds_under_a_burst);
	ADD("politeness/identical", test_identical_searches_at_once_ask_the_source_once);
	ADD("politeness/cap", test_calls_at_a_source_at_once_are_capped);
	ADD_BASE("base-path/served-under", test_a_source_served_under_a_base_path);
	ADD_BASE("base-path/outside", test_an_address_outside_the_base_path_is_not_fetched);
#undef ADD
#undef ADD_BASE
	return g_test_run();
}
