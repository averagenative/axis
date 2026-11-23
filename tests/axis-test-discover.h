/*
 * axis-test-discover.h - test support: a fake TMDb for the Discover tests
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real TMDb provider (meta/axis-meta-tmdb.h), copied with its two
 * addresses pointed at fake servers on loopback.  The API server answers
 * every GET from the files of tests/fixtures/discover by its path:
 * "/3/movie/popular" from movie-popular.json, its page=2 from
 * movie-popular-page2.json, "/3/trending/tv/week" from trending-tv-week.json;
 * a path without a file is a 404.  A test can make a path answer with a
 * status of its own, or with a body it builds.  The image server answers
 * every path with a small PNG.
 */

#ifndef AXIS_TEST_DISCOVER_H
#define AXIS_TEST_DISCOVER_H

#include <glib.h>

#include "axis-test-plex.h"
#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

/* A key in the form the TMDb key setting takes (no real key). */
#define AXIS_TEST_DISCOVER_TOKEN "eyJfake.discover-test-token.not-a-real-key-7f3a9c"

typedef struct _AxisTestDiscoverTmdb AxisTestDiscoverTmdb;

struct _AxisTestDiscoverTmdb
{
	AxisMetaProviderInfo  info;	/* first: a pointer to this is the provider */
	AxisTestPlex         *api;
	AxisTestPlex         *images;
	gchar                *api_origin;
	gchar                *image_origin;
	GMutex                lock;
	GHashTable           *status;	/* (lock) "path" or "path?page=N" -> status */
	GHashTable           *bodies;	/* (lock) "path" or "path?page=N" -> GBytes */
	GBytes               *png;
};

AxisTestDiscoverTmdb *
axis_test_discover_tmdb_new(void);

void
axis_test_discover_tmdb_free(AxisTestDiscoverTmdb *self);

/* From now on @path (and @page, or every page when 0) answers @status with
 * an empty body (0: the files again). */
void
axis_test_discover_tmdb_set_status(
	AxisTestDiscoverTmdb *self,
	const gchar          *path,
	guint                 page,
	guint                 status
);

/* From now on @path (and @page, or every page when 0) answers @json. */
void
axis_test_discover_tmdb_set_body(
	AxisTestDiscoverTmdb *self,
	const gchar          *path,
	guint                 page,
	const gchar          *json
);

/* How many requests the API server received (any path). */
guint
axis_test_discover_tmdb_requests(AxisTestDiscoverTmdb *self);

/* How many requests for @path (without the query). */
guint
axis_test_discover_tmdb_count(
	AxisTestDiscoverTmdb *self,
	const gchar          *path
);

G_END_DECLS

#endif /* AXIS_TEST_DISCOVER_H */
