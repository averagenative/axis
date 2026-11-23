/*
 * axis-test-prowlarr.h - test support: a stand-in for Prowlarr on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * It behaves as a real Prowlarr 3.0.1 did when Axis was first run against
 * one (see tests/fixtures/prowlarr/README.md: what was recorded, and what is
 * still only believed), and it records every request (through
 * axis-test-plex.h) so that a test can assert which requests arrived, with
 * which headers.  Two sets of answers:
 *
 *  - axis_test_prowlarr_new_recorded(): the recorded answers of
 *    tests/fixtures/prowlarr/recorded, eleven real indexers, two of them
 *    Usenet ones with Prowlarr's "Redirect" on;
 *  - axis_test_prowlarr_new(): the constructed answers of
 *    tests/fixtures/prowlarr, which hold what a recording does not (junk,
 *    addresses elsewhere, failing indexers, every field of the wrong type).
 *
 *   GET /api/v1/system/status     system-status.json
 *   GET /api/v1/indexer           indexer.json
 *   GET /api/v1/indexerstatus     indexerstatus.json
 *   GET /api/v1/search?...        a body the test set, by the type asked
 *                                 ("search", "movie" or "tvsearch"), else
 *                                 the fixture of that type (recorded: by
 *                                 IMDb, by words, by TVDB; constructed:
 *                                 search-movie.json); recorded answers keep
 *                                 only the indexers "indexerIds" names, as
 *                                 Prowlarr asks only those
 *   GET /N/download?apikey=..&link=..&file=..
 *                                 a release, as Prowlarr answers it: for an
 *                                 indexer whose downloads redirect, 301 to
 *                                 the indexer's site
 *                                 (axis_test_prowlarr_set_indexer_site());
 *                                 for a torrent known only by its link
 *                                 address, 301 to its magnet link; otherwise
 *                                 the file.  Constructed releases answer by
 *                                 "file": see the table in
 *                                 axis-test-prowlarr.c
 *
 * Everything needs the key, in the X-Api-Key header (a download address
 * carries it as a parameter, as Prowlarr's do): otherwise a bare 401, with
 * no body and no content type, as Prowlarr answers.  With a prefix
 * ("/prowlarr") only paths under it are served, as behind a reverse proxy
 * that strips it.
 *
 * The recordings' origin (http://prowlarr.test:9696) and key are replaced,
 * as the fixtures' @ORIGIN@, @BASE@ and @KEY@ are, by the stand-in's own.
 */

#ifndef AXIS_TEST_PROWLARR_H
#define AXIS_TEST_PROWLARR_H

#include <glib.h>

#include "axis-test-plex.h"

G_BEGIN_DECLS

#define AXIS_TEST_PROWLARR_NZB \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n" \
	"<!DOCTYPE nzb PUBLIC \"-//newzBin//DTD NZB 1.1//EN\" \"http://www.newzbin.com/DTD/nzb/nzb-1.1.dtd\">\r\n" \
	"<nzb xmlns=\"http://www.newzbin.com/DTD/2003/nzb\"><file poster=\"a@b.test\" date=\"1\" subject=\"x\">" \
	"<groups><group>alt.binaries.test</group></groups>" \
	"<segments><segment bytes=\"100\" number=\"1\">id1@x.test</segment></segments></file></nzb>\r\n"

/* The magnet link the fake's "hearth-magnet" redirect points at, and the hash. */
#define AXIS_TEST_PROWLARR_REDIRECT_MAGNET \
	"magnet:?xt=urn:btih:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&dn=Hearth.1995.720p&tr=http%3A%2F%2Ftracker.test%2Fa"
#define AXIS_TEST_PROWLARR_REDIRECT_HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* The hash every link redirect names when told to name the wrong one. */
#define AXIS_TEST_PROWLARR_WRONG_HASH "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

/* What the recordings were scrubbed to: their origin and their key. */
#define AXIS_TEST_PROWLARR_RECORDED_ORIGIN "http://prowlarr.test:9696"
#define AXIS_TEST_PROWLARR_RECORDED_KEY    "0123456789abcdef0123456789abcdef"

/* The indexer's own key, in the address a redirecting download points at:
 * Axis must never store, log or show it. */
#define AXIS_TEST_PROWLARR_INDEXER_KEY     "indexer-own-key-6b2e91d0c4f7"

/* The path the indexer's site serves a release at. */
#define AXIS_TEST_PROWLARR_SITE_PATH       "/api"

typedef struct _AxisTestProwlarr AxisTestProwlarr;

typedef enum
{
	AXIS_TEST_PROWLARR_NORMAL,
	AXIS_TEST_PROWLARR_DOWN,	/* the head never comes */
	AXIS_TEST_PROWLARR_TOO_MANY,	/* 429 with Retry-After 30 */
	AXIS_TEST_PROWLARR_BROKEN	/* 500 */
} AxisTestProwlarrMode;

/* A fake on 127.0.0.1 that takes @key, serving under @prefix ("" for the top). */
AxisTestProwlarr *
axis_test_prowlarr_new(
	const gchar *key,
	const gchar *prefix
);

/* The same, answering with the recordings of tests/fixtures/prowlarr/recorded. */
AxisTestProwlarr *
axis_test_prowlarr_new_recorded(
	const gchar *key,
	const gchar *prefix
);

void
axis_test_prowlarr_free(AxisTestProwlarr *self);

/**
 * axis_test_prowlarr_set_indexer_site:
 * @base: the indexer's site ("https://127.0.0.1:PORT"), or %NULL for an
 *   address that does not resolve
 *
 * Where a redirecting download points: @base, %AXIS_TEST_PROWLARR_SITE_PATH
 * and a query with the release and %AXIS_TEST_PROWLARR_INDEXER_KEY.
 */
void
axis_test_prowlarr_set_indexer_site(
	AxisTestProwlarr *self,
	const gchar      *base
);

/* The answer of GET /api/v1/indexer from now on (copied); %NULL for the
 * fixture's again. */
void
axis_test_prowlarr_set_indexers(
	AxisTestProwlarr *self,
	const gchar      *body
);

/* From now on every link redirect names %AXIS_TEST_PROWLARR_WRONG_HASH. */
void
axis_test_prowlarr_set_wrong_magnets(
	AxisTestProwlarr *self,
	gboolean          wrong
);

/* How many downloads were answered with a redirect to the indexer's site. */
guint
axis_test_prowlarr_n_site_redirects(AxisTestProwlarr *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_prowlarr_base(AxisTestProwlarr *self);

AxisTestPlex *
axis_test_prowlarr_server(AxisTestProwlarr *self);

void
axis_test_prowlarr_set_mode(
	AxisTestProwlarr     *self,
	AxisTestProwlarrMode  mode
);

/* The key it takes from now on. */
void
axis_test_prowlarr_set_key(
	AxisTestProwlarr *self,
	const gchar      *key
);

/* Where a redirect "elsewhere" points (another fake's base, or an address). */
void
axis_test_prowlarr_set_elsewhere(
	AxisTestProwlarr *self,
	const gchar      *base
);

/* A search answer takes this long (the server is busy meanwhile). */
void
axis_test_prowlarr_set_search_delay(
	AxisTestProwlarr *self,
	guint             milliseconds
);

/**
 * axis_test_prowlarr_fixture:
 * @name: a file of tests/fixtures/prowlarr
 *
 * Its text, with @ORIGIN@ the fake's origin, @BASE@ its prefix and @KEY@ its
 * key put in.
 */
gchar *
axis_test_prowlarr_fixture(
	AxisTestProwlarr *self,
	const gchar      *name
);

/**
 * axis_test_prowlarr_set_search:
 * @type: "search", "movie" or "tvsearch" for the searches of that type, or
 *   %NULL for every type
 * @body: the answer's text (copied)
 */
void
axis_test_prowlarr_set_search(
	AxisTestProwlarr *self,
	const gchar      *type,
	const gchar      *body
);

/**
 * axis_test_prowlarr_release_json:
 * @name, @protocol: "usenet" or "torrent"
 * @file: what its download address asks for ("hearth-nzb", "hearth-torrent",
 *   "hearth-magnet", "hearth-long-magnet" (a magnet link of about 1.1 kB, as
 *   Prowlarr's are), "hearth-indexer" (a redirect to the indexer's site),
 *   "hearth-elsewhere", "hearth-html", "hearth-huge", "hearth-moved", "hearth-gone",
 *   "hearth-bad-magnet", "hearth-slow"), or %NULL for no address
 * @magnet: (nullable): its magnetUrl
 *
 * A one-release answer, its download address on the fake.
 */
gchar *
axis_test_prowlarr_release_json(
	AxisTestProwlarr *self,
	const gchar      *name,
	const gchar      *protocol,
	const gchar      *file,
	const gchar      *magnet
);

/* ── What it received ───────────────────────────────────────────── */

/* How many searches (requests for /api/v1/search) arrived. */
guint
axis_test_prowlarr_n_searches(AxisTestProwlarr *self);

/* The query of the @index-th search (raw, as received), or %NULL. */
gchar *
axis_test_prowlarr_search_query(
	AxisTestProwlarr *self,
	guint             index
);

/* How many requests of any kind arrived. */
guint
axis_test_prowlarr_n_requests(AxisTestProwlarr *self);

/* How many requests for /N/download arrived. */
guint
axis_test_prowlarr_n_downloads(AxisTestProwlarr *self);

/* How many requests carried a wrong or no key, were not under the prefix,
 * or put the key in the query of a request that is not a download. */
guint
axis_test_prowlarr_n_bad(AxisTestProwlarr *self);

G_END_DECLS

#endif /* AXIS_TEST_PROWLARR_H */
