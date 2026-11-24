/*
 * axis-test-requests.h - test support for requests: the layers a request
 * needs (metadata with made-up titles, the movies and the TV module), a few
 * accounts, and files and grabs written straight into the engine's records
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Shared by tests/test-requests.c (the model) and tests/test-web-requests.c
 * (the pages).  Every database, directory and port is the test's own.
 */

#ifndef AXIS_TEST_REQUESTS_H
#define AXIS_TEST_REQUESTS_H

#include <glib.h>

#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "core/axis-data-dir.h"
#include "requests/axis-requests.h"

G_BEGIN_DECLS

/* The made-up titles, by the fixture provider's identifiers. */
#define AXIS_TEST_REQUESTS_ORCHARD   "301"	/* The Lantern Orchard (2019) */
#define AXIS_TEST_REQUESTS_LEDGER    "302"	/* Saltmarsh Ledger (2015) */
#define AXIS_TEST_REQUESTS_KILN      "303"	/* Paper Kiln (2011) */
#define AXIS_TEST_REQUESTS_BELLS     "304"	/* Quarry Bells (2008) */
#define AXIS_TEST_REQUESTS_COMPASS   "305"	/* Tin Compass (2021) */
#define AXIS_TEST_REQUESTS_ALMANAC   "306"	/* Low Tide Almanac (2017) */
#define AXIS_TEST_REQUESTS_FILMS     6
/* Velvet Annex (2014): a film the provider marks as adult. */
#define AXIS_TEST_REQUESTS_ANNEX     "307"
/* Harbour of Glass (2020): seasons 0 (one special), 1, 2 and 3, two aired
 * episodes each. */
#define AXIS_TEST_REQUESTS_HARBOUR   "351"
/* Nine Winters (2012): seasons 1 to 9, one aired episode each. */
#define AXIS_TEST_REQUESTS_WINTERS   "352"
/* Tidal Clockwork (2018): seasons 1 and 2, eight aired episodes each. */
#define AXIS_TEST_REQUESTS_CLOCKWORK "353"

/* The moment the tests' clocks stand at: noon on a Tuesday, UTC
 * (2026-11-24 12:00:00), in microseconds since 1970. */
#define AXIS_TEST_REQUESTS_NOW_US G_GINT64_CONSTANT(1795521600000000)

typedef struct
{
	AxisTestLibrary     *t;
	gchar               *disk;
	gint64               volume_id;
	gint64               films_root;
	gint64               series_root;
	AxisDataDir         *data;
	AxisSettings        *settings;
	AxisTestMetaFixture *fa;
	AxisMeta            *meta;
	AxisIndexers        *indexers;
	AxisDownloads       *downloads;
	AxisQualityStore    *quality;
	AxisMovies          *movies;
	AxisTv              *tv;
	AxisRequests        *requests;
	gint64               profile_id;
	gint64               admin;	/* the account "root" */
	gint64               robin;
	gint64               sam;
} AxisTestRequests;

/*
 * The layers, with the titles above answered by the fixture provider, the
 * clocks of the TV module and of the requests set to
 * %AXIS_TEST_REQUESTS_NOW_US and the calendar read in UTC.  The settings
 * store is made and not loaded: the caller declares the settings (as tables,
 * or by registering modules) and calls axis_test_requests_ready().  %NULL
 * when database tests are skipped.
 */
AxisTestRequests *
axis_test_requests_new(void);

/* Declares every layer's settings as tables (a test without a module
 * registry). */
void
axis_test_requests_declare(AxisTestRequests *r);

/* Loads the settings, gives the fixture provider its key, reads the default
 * quality profile and makes the accounts root (an administrator), robin and
 * sam, in that order. */
void
axis_test_requests_ready(AxisTestRequests *r);

void
axis_test_requests_free(AxisTestRequests *r);

/* The stored title of a made-up film or series (asked of the provider). */
gint64
axis_test_requests_title(
	AxisTestRequests *r,
	AxisMetaKind      kind,
	const gchar      *provider_id
);

/* A further account; its id. */
gint64
axis_test_requests_person(
	AxisTestRequests *r,
	const gchar      *username,
	gboolean          admin
);

/* The placement defaults: the fixture's two folders and its profile. */
void
axis_test_requests_set_defaults(AxisTestRequests *r);

/* The first column of the first row of a statement. */
gchar *
axis_test_requests_q(
	AxisTestRequests *r,
	const gchar      *format,
	...
) G_GNUC_PRINTF(2, 3);

gint64
axis_test_requests_n(
	AxisTestRequests *r,
	const gchar      *format,
	...
) G_GNUC_PRINTF(2, 3);

/* Adds the film to Movies as an administrator would (no search); its id. */
gint64
axis_test_requests_add_movie(
	AxisTestRequests *r,
	gint64            title_id
);

/* Adds the series to TV with nothing monitored; its id. */
gint64
axis_test_requests_add_series(
	AxisTestRequests *r,
	gint64            title_id
);

/* A video file in the library linked to the movie. */
void
axis_test_requests_movie_file(
	AxisTestRequests *r,
	gint64            movie_id
);

/* Files in the library for the first @n episodes of a season. */
void
axis_test_requests_episode_files(
	AxisTestRequests *r,
	gint64            series_id,
	gint              season,
	guint             n
);

/* An open grab of the movie, as a search that took a release leaves it; its
 * id.  @close_reason: (nullable) the grab is closed with it instead. */
gint64
axis_test_requests_movie_grab(
	AxisTestRequests *r,
	gint64            movie_id,
	const gchar      *close_reason
);

/* The open grab @grab_id has arrived, and its import is blocked: a person
 * must decide. */
void
axis_test_requests_import_blocked(
	AxisTestRequests *r,
	gint64            movie_id,
	gint64            grab_id
);

G_END_DECLS

#endif /* AXIS_TEST_REQUESTS_H */
