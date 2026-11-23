/*
 * axis-test-sonarr.h - test support: a stand-in for a person's Sonarr, on
 * loopback
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * It answers the version 3 API as src/arr/axis-arr-assumed.h assumes it
 * (nobody recorded a real Sonarr for this, and nobody has yet seen one
 * answer): GET system/status, qualityprofile, rootfolder, queue (paged),
 * series, and for one series episode?seriesId=N and episodefile?seriesId=N,
 * below an optional URL base, to a request that carries the API key in
 * X-Api-Key.  A request without the key, or with another, gets 401.  A
 * request of any method but GET is counted and answered 405: Axis only reads
 * from Sonarr, and a test fails when that count is not 0
 * (axis_test_sonarr_assert_read_only()).
 *
 * It runs on the fake server of axis-test-plex.h (a thread of its own), so
 * the code under test may block on it, and it records every request.
 */

#ifndef AXIS_TEST_SONARR_H
#define AXIS_TEST_SONARR_H

#include <glib.h>

#include "axis-test-plex.h"

G_BEGIN_DECLS

typedef struct _AxisTestSonarr AxisTestSonarr;

/* A Sonarr with the answers of tests/fixtures/arr (one series' episodes and
 * files, series 1) and the key @key. */
AxisTestSonarr *
axis_test_sonarr_new(const gchar *key);

void
axis_test_sonarr_free(AxisTestSonarr *self);

/* "http://127.0.0.1:PORT" */
gchar *
axis_test_sonarr_base(AxisTestSonarr *self);

/* The server under it: for a route that misbehaves, which answers before the
 * stand-in. */
AxisTestPlex *
axis_test_sonarr_server(AxisTestSonarr *self);

/* Serves the API below @prefix ("/sonarr"); %NULL or "" for the top. */
void
axis_test_sonarr_set_prefix(
	AxisTestSonarr *self,
	const gchar    *prefix
);

/* What system/status names itself ("Sonarr" by default). */
void
axis_test_sonarr_set_app(
	AxisTestSonarr *self,
	const gchar    *app_name
);

/* The body of GET series, qualityprofile and rootfolder; copied. */
void
axis_test_sonarr_set_series(
	AxisTestSonarr *self,
	const gchar    *json
);

void
axis_test_sonarr_set_profiles(
	AxisTestSonarr *self,
	const gchar    *json
);

void
axis_test_sonarr_set_folders(
	AxisTestSonarr *self,
	const gchar    *json
);

/* The body of GET episode?seriesId=@series_id and of
 * episodefile?seriesId=@series_id; copied.  A series nothing was set for
 * answers an empty list. */
void
axis_test_sonarr_set_episodes(
	AxisTestSonarr *self,
	gint64          series_id,
	const gchar    *json
);

void
axis_test_sonarr_set_episode_files(
	AxisTestSonarr *self,
	gint64          series_id,
	const gchar    *json
);

/*
 * A generated library: GET series lists @n_series series ("Made Up Series N",
 * Sonarr's number N, TheTVDB identifier 700000 + N, under /tv, every tenth
 * not monitored, two seasons of which the second is not monitored for every
 * third), and every one of them answers @per_series episodes (twenty a
 * season, each with TheTVDB's id and a file of its own, every fifth not
 * monitored) and as many episode files
 * ("/tv/Made Up Series N/Season S/Made Up Series N - SssEee.mkv", of 1000 +
 * the episode's place bytes).  The answers are made as they are asked for.
 */
void
axis_test_sonarr_generate(
	AxisTestSonarr *self,
	guint           n_series,
	guint           per_series
);

/* The two lists of series @series_id answer @status (say 503) the next @times
 * times they are asked for (-1: always; 0: no more). */
void
axis_test_sonarr_fail_series(
	AxisTestSonarr *self,
	gint64          series_id,
	guint           status,
	gint            times
);

/* The series the download queue names, served page by page as asked. */
void
axis_test_sonarr_set_queue(
	AxisTestSonarr *self,
	const gint64   *series_ids,
	guint           n
);

/* How many requests were not a GET. */
guint
axis_test_sonarr_n_non_get(AxisTestSonarr *self);

/* How many requests were for @path (below the prefix, without the query),
 * such as "/api/v3/episode". */
guint
axis_test_sonarr_count(
	AxisTestSonarr *self,
	const gchar    *path
);

/*
 * Fails the test unless every request received was a GET, none carried the
 * key anywhere but in its X-Api-Key header (never in a path or a query), and
 * none asked with "apikey" in its query.
 */
void
axis_test_sonarr_assert_read_only(AxisTestSonarr *self);

G_END_DECLS

#endif /* AXIS_TEST_SONARR_H */
