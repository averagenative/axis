/*
 * axis-test-tvdb.h - test support: a stand-in TheTVDB on loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An API server that answers as TheTVDB's version 4 documents it (and as
 * src/meta/axis-meta-tvdb-assumed.h assumes): POST /v4/login hands out a
 * new token each time (refused while asked to), every GET needs the latest
 * token as a bearer header (and is refused 401 while asked to expire), and
 * the answers are the hand-written files of tests/fixtures/tvdb; and an
 * image server for the series' poster.  The provider's info is TheTVDB's
 * own, pointed at the two (axis_test_tvdb_info()).  Nothing is recorded from
 * the real service.
 */

#ifndef AXIS_TEST_TVDB_H
#define AXIS_TEST_TVDB_H

#include <glib.h>

#include "axis-test-plex.h"
#include "meta/axis-meta-provider.h"

G_BEGIN_DECLS

/* Obviously fake: the test key and PIN, never a real one. */
#define AXIS_TEST_TVDB_KEY          "fake-tvdb-key-for-tests-0000"
#define AXIS_TEST_TVDB_OTHER_KEY    "fake-tvdb-key-for-tests-1111"
#define AXIS_TEST_TVDB_PIN          "FAKEPIN0000"
/* Every token it hands out starts so. */
#define AXIS_TEST_TVDB_TOKEN_PREFIX "fake.tvdb-token-"

typedef struct _AxisTestTvdb AxisTestTvdb;

AxisTestTvdb *
axis_test_tvdb_new(void);

void
axis_test_tvdb_free(AxisTestTvdb *self);

/* TheTVDB's provider info, its two addresses the stand-ins'; it lives as
 * long as @self. */
const AxisMetaProviderInfo *
axis_test_tvdb_info(AxisTestTvdb *self);

AxisTestPlex *
axis_test_tvdb_api(AxisTestTvdb *self);

AxisTestPlex *
axis_test_tvdb_images(AxisTestTvdb *self);

/* Logins answered with a token, and refused. */
guint
axis_test_tvdb_logins(AxisTestTvdb *self);

/* While @refuse, every login is answered 401. */
void
axis_test_tvdb_refuse(
	AxisTestTvdb *self,
	gboolean      refuse
);

/* The next @n GETs are answered 401 whatever their token. */
void
axis_test_tvdb_expire(
	AxisTestTvdb *self,
	guint         n
);

/* Series 81001's record names @address as its image (%NULL: the
 * fixture's), and @tmdb as its TMDb identifier (%NULL: the fixture's, 7051). */
void
axis_test_tvdb_set_record(
	AxisTestTvdb *self,
	const gchar  *address,
	const gchar  *tmdb
);

/* While @empty, every page of an order says it succeeded with no episode. */
void
axis_test_tvdb_empty_order(
	AxisTestTvdb *self,
	gboolean      empty
);

/* From now on a GET of @path (with exactly @query, %NULL: any query) is
 * answered 200 with @json, before any fixture file; %NULL @json removes the
 * answer.  For a test's own made-up series. */
void
axis_test_tvdb_set_answer(
	AxisTestTvdb *self,
	const gchar  *path,
	const gchar  *query,
	const gchar  *json
);

/* The GETs answered so far (logins aside). */
guint
axis_test_tvdb_gets(AxisTestTvdb *self);

G_END_DECLS

#endif /* AXIS_TEST_TVDB_H */
