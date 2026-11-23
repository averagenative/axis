/*
 * axis-test-radarr.h - test support: a stand-in for a person's Radarr, on
 * loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * It answers the version 3 API as src/arr/axis-arr-assumed.h assumes it
 * (nobody recorded a real Radarr for this): GET system/status,
 * qualityprofile, rootfolder, queue (paged) and movie, below an optional URL
 * base, to a request that carries the API key in X-Api-Key.  A request
 * without the key, or with another, gets 401.  A request of any method but
 * GET is counted and answered 405: Axis only reads from Radarr, and a test
 * fails when that count is not 0 (axis_test_radarr_assert_read_only()).
 *
 * It runs on the fake server of axis-test-plex.h (a thread of its own), so
 * the code under test may block on it, and it records every request.
 */

#ifndef AXIS_TEST_RADARR_H
#define AXIS_TEST_RADARR_H

#include <glib.h>

#include "axis-test-plex.h"

G_BEGIN_DECLS

typedef struct _AxisTestRadarr AxisTestRadarr;

/* A Radarr with the answers of tests/fixtures/arr and the key @key. */
AxisTestRadarr *
axis_test_radarr_new(const gchar *key);

void
axis_test_radarr_free(AxisTestRadarr *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_radarr_base(AxisTestRadarr *self);

/* The server under it: for a route that misbehaves (a redirect, an answer
 * too large, one that never comes), which answers before the stand-in. */
AxisTestPlex *
axis_test_radarr_server(AxisTestRadarr *self);

/* Serves the API below @prefix ("/radarr"); %NULL or "" for the top. */
void
axis_test_radarr_set_prefix(
	AxisTestRadarr *self,
	const gchar    *prefix
);

/* What system/status names itself ("Radarr" by default). */
void
axis_test_radarr_set_app(
	AxisTestRadarr *self,
	const gchar    *app_name
);

/* The body of GET movie, qualityprofile and rootfolder; copied. */
void
axis_test_radarr_set_movies(
	AxisTestRadarr *self,
	const gchar    *json
);

void
axis_test_radarr_set_profiles(
	AxisTestRadarr *self,
	const gchar    *json
);

void
axis_test_radarr_set_folders(
	AxisTestRadarr *self,
	const gchar    *json
);

/* The movies in the download queue, served page by page as asked. */
void
axis_test_radarr_set_queue(
	AxisTestRadarr *self,
	const gint64   *movie_ids,
	guint           n
);

/* How many requests were not a GET. */
guint
axis_test_radarr_n_non_get(AxisTestRadarr *self);

/* How many requests were for @path (below the prefix, without the query),
 * such as "/api/v3/movie". */
guint
axis_test_radarr_count(
	AxisTestRadarr *self,
	const gchar    *path
);

/*
 * Fails the test unless every request received was a GET, none carried the
 * key anywhere but in its X-Api-Key header (never in a path or a query), and
 * none asked with "apikey" in its query.
 */
void
axis_test_radarr_assert_read_only(AxisTestRadarr *self);

G_END_DECLS

#endif /* AXIS_TEST_RADARR_H */
