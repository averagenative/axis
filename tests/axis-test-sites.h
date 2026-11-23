/*
 * axis-test-sites.h - test support: a stand-in for one of three public
 * torrent sites (YTS, EZTV, The Pirate Bay) on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * It answers as the sites' documented answers say they do, from the
 * hand-written files of tests/fixtures/sites (their README lists everything
 * believed and not verified: no site was asked), and records every request
 * (through axis-test-plex.h).  It holds no knowledge of what a fixture says:
 * a recorded answer put in its place is served the same way.
 *
 *   YTS      GET /api/v2/list_movies.json?query_term=..&limit=..&page=..
 *   EZTV     GET /api/get-torrents?imdb_id=..&limit=..&page=..
 *   TPB      GET /q.php?q=..&cat=..
 *
 * The body of a request is the one the test set for it (by the value of
 * query_term, imdb_id or q, or for "" every one; EZTV by page too), else the
 * site's list fixture.  Nothing is ever sent but a GET of those paths with no
 * credentials, no cookie and no key in the query: anything else is counted as
 * bad.
 */

#ifndef AXIS_TEST_SITES_H
#define AXIS_TEST_SITES_H

#include <glib.h>

#include "axis-test-plex.h"

G_BEGIN_DECLS

typedef struct _AxisTestSites AxisTestSites;

typedef enum
{
	AXIS_TEST_SITE_YTS,
	AXIS_TEST_SITE_EZTV,
	AXIS_TEST_SITE_TPB
} AxisTestSite;

typedef enum
{
	AXIS_TEST_SITES_NORMAL,
	AXIS_TEST_SITES_DOWN,		/* the head never comes */
	AXIS_TEST_SITES_BROKEN,		/* 500 */
	AXIS_TEST_SITES_GONE,		/* 404: the address is not its API's any more */
	AXIS_TEST_SITES_FORBIDDEN,	/* 403 */
	AXIS_TEST_SITES_MOVED,		/* 301 to another address */
	AXIS_TEST_SITES_PARKED,		/* 200, an HTML page: another business at the address */
	AXIS_TEST_SITES_LIMITED		/* 429, asking to wait a few seconds */
} AxisTestSitesMode;

AxisTestSites *
axis_test_sites_new(AxisTestSite site);

void
axis_test_sites_free(AxisTestSites *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_sites_base(AxisTestSites *self);

AxisTestPlex *
axis_test_sites_server(AxisTestSites *self);

void
axis_test_sites_set_mode(
	AxisTestSites     *self,
	AxisTestSitesMode  mode
);

/**
 * axis_test_sites_set_body:
 * @term: the value of query_term (YTS), imdb_id (EZTV) or q (TPB) this is for
 *   (as sent, percent-encoded), or %NULL for every one
 * @page: EZTV's page, or 0 for every page
 * @body: the answer (copied)
 */
void
axis_test_sites_set_body(
	AxisTestSites *self,
	const gchar   *term,
	guint          page,
	const gchar   *body
);

/* A search answer takes this long. */
void
axis_test_sites_set_delay(
	AxisTestSites *self,
	guint          milliseconds
);

/* A fixture of tests/fixtures/sites. */
gchar *
axis_test_sites_fixture(const gchar *name);

/* ── What it received ───────────────────────────────────────────── */

guint
axis_test_sites_n_requests(AxisTestSites *self);

/* The query of the @index-th request (raw), or %NULL. */
gchar *
axis_test_sites_query(
	AxisTestSites *self,
	guint          index
);

/* The path of the @index-th request, or %NULL. */
gchar *
axis_test_sites_path(
	AxisTestSites *self,
	guint          index
);

/* Requests that carried a credential, a cookie or a key in the query, used
 * another method than GET, or asked for another path than the site's own. */
guint
axis_test_sites_n_bad(AxisTestSites *self);

G_END_DECLS

#endif /* AXIS_TEST_SITES_H */
