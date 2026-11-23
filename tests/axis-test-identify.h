/*
 * axis-test-identify.h - test support for identification: the films the
 * metadata test provider knows, and the handler of its fake server that
 * answers searches, finds and full records from them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Install with axis_test_plex_set_handler(fixture->api,
 * axis_test_identify_provider, NULL).  Searches find every film whose title
 * holds the words asked for (ignoring case), finds go by "tmdb" (the film's
 * id) or "imdb", and /title/film/<id> is the full record with its runtime.
 * Every film has a poster reference "/p<id>.png";
 * axis_test_identify_posters() serves them on the image server.
 */

#ifndef AXIS_TEST_IDENTIFY_H
#define AXIS_TEST_IDENTIFY_H

#include <glib.h>

#include "axis-test-meta-fixture.h"

G_BEGIN_DECLS

typedef struct
{
	const gchar *id;
	const gchar *title;
	gint         year;
	gint         runtime;
	const gchar *imdb;
} AxisTestFilm;

/* The films, ended by an entry whose id is %NULL. */
const AxisTestFilm *
axis_test_identify_films(void);

/*
 * The made-up series the provider knows too, ended by an entry whose id is
 * %NULL.  Searches by "/search/series", finds by tmdb, tvdb or imdb, full
 * records at /title/series/<id> with their seasons, and each season at
 * /title/series/<id>/season/<n>: episode e of season s is named
 * "Chapter <e>", runs @runtime minutes (the specials and a daily show's
 * too), has the id "<id>-<s>-<e>" and aired @first_aired plus a week for
 * each episode before it in the series (a daily show: a day; @two_on_day
 * puts its last two episodes on one day).  A season listed with no
 * episodes is not served (the provider has not fetched it).  A @runtime of
 * 0 is none given (for the series and its episodes); @other_name, when set,
 * is one of its other names, found by a search and listed in its full
 * record only (as a real provider's search records carry none).
 *
 * Besides the table, "Shelf Series <n>" (n from 1 to 99999, the scale
 * tests' made-up library) is a series of four seasons of ten episodes, id
 * "shelf<n>", year 1960 + n % 60, no runtime, episode e of season s aired
 * on 2001-01-01 plus s * 100 + e * 7 days; a search finds it by that name
 * exactly.
 */
typedef struct
{
	const gchar *id;
	const gchar *title;
	gint         year;
	gint         runtime;
	const gchar *tvdb;
	const gchar *imdb;
	const gchar *first_aired;	/* "YYYY-MM-DD" */
	gint         seasons[4];	/* episodes of seasons 1 to 4 (0: none) */
	gint         specials;		/* episodes of season 0 */
	gboolean     daily;
	gboolean     two_on_day;
	const gchar *other_name;	/* (nullable) */
} AxisTestSeries;

const AxisTestSeries *
axis_test_identify_series(void);

/* How many requests the provider answered for seasons (any series). */
guint
axis_test_identify_season_requests(void);

void
axis_test_identify_provider(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
);

/* Each film's poster at the card size, on @fixture's image server. */
void
axis_test_identify_posters(AxisTestMetaFixture *fixture);

G_END_DECLS

#endif /* AXIS_TEST_IDENTIFY_H */
