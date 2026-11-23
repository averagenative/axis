/*
 * test-meta-tmdb.c - the TMDb provider: its answers read into the model
 * (from hand-written fixtures in tests/fixtures/tmdb, in TMDb's published
 * version 3 format), and the provider run through the metadata layer against
 * a fake TMDb on loopback, with a real database and a data directory
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (metadata):
 *   Search; One record across lookups; Address in a response; Unreadable
 *   fields; Not in a URL; Images; Notice
 * and the rest of the change's promises: the token is a bearer header to
 * the data address only, the short key is refused, images are fetched only
 * for a strict file name, an unknown status stays unknown, a film's date is
 * taken for the configured region, and a 429 pauses the provider.
 *
 * The fixtures are written by hand from TMDb's documentation, not recorded
 * from the real service: what is proved here is that Axis reads that format,
 * not that TMDb answers in it today.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-plex.h"
#include "axis-test-storage.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb-parse.h"
#include "meta/axis-meta-tmdb.h"

#define TOKEN "eyJhbGciOiJIUzI1NiJ9.dGVzdC10b2tlbi1ub3QtcmVhbA.c2lnbmF0dXJlLW5vdC1yZWFs"
#define SHORT_KEY "0123456789abcdef0123456789ABCDEF"
#define WAIT 5000
#define DIR "tests/fixtures/tmdb/"

/* ── Parsing, without a network ─────────────────────────────────── */

static gchar *
read_fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(DIR, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, &error))
		g_error("cannot read %s: %s", path, error->message);
	return text;
}

static GPtrArray *
search_of(
	AxisMetaKind kind,
	const gchar *name,
	gboolean     adult
){
	g_autofree gchar *text = read_fixture(name);
	g_autoptr(GError) error = NULL;
	GPtrArray *titles = NULL;

	g_assert_true(axis_meta_tmdb_parse_search(kind, text, strlen(text), adult, &titles, &error));
	g_assert_no_error(error);
	g_assert_nonnull(titles);
	return titles;
}

static AxisMetaTitle *
title_of(
	AxisMetaKind kind,
	const gchar *name,
	const gchar *region
){
	g_autofree gchar *text = read_fixture(name);
	g_autoptr(GError) error = NULL;
	AxisMetaTitle *title = NULL;
	g_autofree gchar *problem = NULL;

	g_assert_true(axis_meta_tmdb_parse_title(kind, region, text, strlen(text), &title, &error));
	g_assert_no_error(error);
	g_assert_nonnull(title);
	g_assert_true(axis_meta_title_check(title, "tmdb", &problem));
	g_assert_null(problem);
	return title;
}

static void
assert_titles_checked(GPtrArray *titles)
{
	guint i;

	for (i = 0; i < titles->len; i++) {
		g_autofree gchar *problem = NULL;

		g_assert_true(axis_meta_title_check(g_ptr_array_index(titles, i), "tmdb", &problem));
	}
}

static void
assert_ids(
	GPtrArray   *titles,
	const gchar *expected
){
	g_autoptr(GString) ids = g_string_new(NULL);
	guint i;

	for (i = 0; i < titles->len; i++)
		g_string_append_printf(ids, "%s%s", i > 0 ? "," : "", ((AxisMetaTitle *)g_ptr_array_index(titles, i))->provider_id);
	g_assert_cmpstr(ids->str, ==, expected);
}

static void
test_parse_search_film(void)
{
	g_autoptr(GPtrArray) titles = search_of(AXIS_META_KIND_FILM, "search-movie.json", FALSE);
	g_autoptr(GPtrArray) with_adult = search_of(AXIS_META_KIND_FILM, "search-movie.json", TRUE);
	const AxisMetaTitle *mazework;
	const AxisMetaTitle *revolutions;
	const AxisMetaTitle *named;

	/* The adult entry, the ones without a usable identifier, the series and
	 * the repeat are left out. */
	assert_ids(titles, "6003,6017,6018,607");
	assert_ids(with_adult, "6003,6017,6018,910001,607");
	assert_titles_checked(titles);
	assert_titles_checked(with_adult);

	mazework = g_ptr_array_index(titles, 0);
	g_assert_cmpstr(mazework->title, ==, "The Mazework");
	g_assert_cmpstr(mazework->original_title, ==, "The Mazework");
	g_assert_cmpint(mazework->year, ==, 1999);
	g_assert_cmpstr(mazework->release_date, ==, "1999-03-30");
	g_assert_cmpstr(mazework->original_language, ==, "en");
	g_assert_cmpstr(mazework->poster_ref, ==, "/madeUpPoster000000000006003.jpg");
	g_assert_cmpstr(mazework->backdrop_ref, ==, "/madeUpBackdrop0000000006003.jpg");
	g_assert_nonnull(mazework->overview);
	g_assert_cmpint(mazework->status, ==, AXIS_META_STATUS_UNKNOWN);
	g_assert_false(mazework->detail);
	g_assert_null(mazework->genres);
	g_assert_null(mazework->alt_titles);
	g_assert_cmpstr(axis_meta_title_get_id(mazework, "tmdb"), ==, "6003");

	/* An address instead of a file name is no image, and the base address
	 * a response states is nowhere used. */
	revolutions = g_ptr_array_index(titles, 2);
	g_assert_cmpstr(revolutions->title, ==, "The Mazework Revolutions");
	g_assert_null(revolutions->poster_ref);
	g_assert_null(revolutions->backdrop_ref);

	/* A title of the wrong type: the original name serves; an empty date is
	 * no date. */
	named = g_ptr_array_index(titles, 3);
	g_assert_cmpstr(named->title, ==, "Mazework Original Name");
	g_assert_cmpint(named->year, ==, 0);
	g_assert_null(named->release_date);
	g_assert_null(named->poster_ref);
}

static void
test_parse_search_series(void)
{
	g_autoptr(GPtrArray) titles = search_of(AXIS_META_KIND_SERIES, "search-tv.json", FALSE);
	const AxisMetaTitle *bad;
	const AxisMetaTitle *other;

	assert_ids(titles, "7101,7103");
	assert_titles_checked(titles);
	bad = g_ptr_array_index(titles, 0);
	g_assert_cmpstr(bad->title, ==, "Cracking Slate");
	g_assert_cmpstr(bad->first_air_date, ==, "2008-01-20");
	g_assert_cmpint(bad->year, ==, 2008);
	g_assert_null(bad->release_date);
	g_assert_cmpstr(bad->poster_ref, ==, "/madeUpPoster000000000007101.jpg");
	other = g_ptr_array_index(titles, 1);
	g_assert_cmpstr(other->poster_ref, ==, "/ab12CD34ef56GH78ij90KL12mn34.webp");
}

static void
test_parse_film_details(void)
{
	g_autoptr(AxisMetaTitle) title = title_of(AXIS_META_KIND_FILM, "movie-6003.json", "US");
	const AxisMetaAltTitle *alt;
	guint i;

	g_assert_true(title->detail);
	g_assert_cmpstr(title->title, ==, "The Mazework");
	g_assert_cmpint(title->year, ==, 1999);
	g_assert_cmpint(title->runtime, ==, 136);
	g_assert_cmpint(title->status, ==, AXIS_META_STATUS_RELEASED);
	g_assert_cmpstr(title->original_language, ==, "en");
	g_assert_cmpstr(title->poster_ref, ==, "/madeUpPoster000000000006003.jpg");
	g_assert_null(title->first_air_date);
	g_assert_null(title->seasons);

	g_assert_nonnull(title->genres);
	g_assert_cmpuint(title->genres->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(title->genres, 0), ==, "Action");
	g_assert_cmpstr(g_ptr_array_index(title->genres, 1), ==, "Science Fiction");

	/* Identifiers: TMDb's, and IMDb's (named twice, kept once). */
	g_assert_cmpuint(title->ids->len, ==, 2);
	g_assert_cmpstr(axis_meta_title_get_id(title, "tmdb"), ==, "6003");
	g_assert_cmpstr(axis_meta_title_get_id(title, "imdb"), ==, "tt0600003");

	/* Other names: the repeat and the unreadable one are left out; a wrong
	 * country code leaves the country empty, not the name. */
	g_assert_nonnull(title->alt_titles);
	g_assert_cmpuint(title->alt_titles->len, ==, 5);
	alt = g_ptr_array_index(title->alt_titles, 0);
	g_assert_cmpstr(alt->title, ==, "Mazework");
	g_assert_cmpstr(alt->country, ==, "DE");
	alt = g_ptr_array_index(title->alt_titles, 2);
	g_assert_cmpstr(alt->title, ==, "メイズワーク");
	alt = g_ptr_array_index(title->alt_titles, 4);
	g_assert_cmpstr(alt->title, ==, "Wrong country code");
	g_assert_null(alt->country);
	for (i = 0; i < title->alt_titles->len; i++)
		g_assert_null(((AxisMetaAltTitle *)g_ptr_array_index(title->alt_titles, i))->language);
}

/* Spec: release dates taken for the configured region. */
static void
test_parse_film_regional_dates(void)
{
	static const struct
	{
		const gchar *region;
		const gchar *date;
	} cases[] = {
		/* US: a premiere (24 March) and a television date (1998) are not
		 * counted; the earliest of cinema (31 March), disc and digital is. */
		{ "US", "1999-03-31" },
		{ "GB", "1999-06-11" },
		{ "DE", "1999-06-17" },
		/* JP: one digital date and one that is not a date. */
		{ "JP", "2001-01-01" },
		/* A country with no entry, an invalid region and none: TMDb's own. */
		{ "FR", "1999-03-30" },
		{ "us", "1999-03-30" },
		{ NULL, "1999-03-30" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(AxisMetaTitle) title = title_of(AXIS_META_KIND_FILM, "movie-6003.json", cases[i].region);

		g_assert_cmpstr(title->release_date, ==, cases[i].date);
		/* Its year is TMDb's own, wherever it is released. */
		g_assert_cmpint(title->year, ==, 1999);
	}
}

static void
test_parse_series_details(void)
{
	g_autoptr(AxisMetaTitle) title = title_of(AXIS_META_KIND_SERIES, "tv-7101.json", "US");
	const AxisMetaSeason *season;

	g_assert_true(title->detail);
	g_assert_cmpstr(title->title, ==, "Cracking Slate");
	g_assert_cmpint(title->status, ==, AXIS_META_STATUS_ENDED);
	g_assert_cmpint(title->runtime, ==, 45);
	g_assert_cmpint(title->year, ==, 2008);
	g_assert_cmpstr(title->first_air_date, ==, "2008-01-20");
	g_assert_cmpstr(title->last_air_date, ==, "2013-09-29");
	g_assert_null(title->release_date);
	g_assert_cmpuint(title->genres->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(title->genres, 1), ==, "Crime");
	g_assert_cmpuint(title->ids->len, ==, 3);
	g_assert_cmpstr(axis_meta_title_get_id(title, "tmdb"), ==, "7101");
	g_assert_cmpstr(axis_meta_title_get_id(title, "imdb"), ==, "tt0700001");
	g_assert_cmpstr(axis_meta_title_get_id(title, "tvdb"), ==, "82001");
	g_assert_cmpuint(title->alt_titles->len, ==, 3);

	/* Seasons, specials among them; one without a number is skipped, an
	 * unreadable count is unknown (-1). */
	g_assert_cmpuint(title->seasons->len, ==, 4);
	season = g_ptr_array_index(title->seasons, 0);
	g_assert_cmpint(season->number, ==, 0);
	g_assert_cmpstr(season->name, ==, "Specials");
	g_assert_cmpint(season->episode_count, ==, 11);
	g_assert_cmpstr(season->air_date, ==, "2009-02-17");
	g_assert_null(season->episodes);
	season = g_ptr_array_index(title->seasons, 1);
	g_assert_cmpint(season->number, ==, 1);
	g_assert_cmpint(season->episode_count, ==, 7);
	season = g_ptr_array_index(title->seasons, 3);
	g_assert_cmpint(season->number, ==, 3);
	g_assert_cmpint(season->episode_count, ==, -1);
	g_assert_null(season->air_date);
}

/* Spec: Unreadable fields. */
static void
test_parse_wrong_typed_fields(void)
{
	g_autoptr(AxisMetaTitle) title = title_of(AXIS_META_KIND_FILM, "movie-broken.json", "US");

	g_assert_cmpstr(title->title, ==, "The Mazework");
	g_assert_null(title->original_title);
	g_assert_null(title->overview);
	g_assert_cmpint(title->runtime, ==, 0);
	g_assert_cmpint(title->status, ==, AXIS_META_STATUS_UNKNOWN);
	g_assert_null(title->release_date);
	g_assert_cmpint(title->year, ==, 0);
	/* An address, or a path that climbs out, is no image. */
	g_assert_null(title->poster_ref);
	g_assert_null(title->backdrop_ref);
	g_assert_null(title->genres);
	g_assert_null(title->original_language);
	g_assert_null(title->alt_titles);
	g_assert_cmpuint(title->ids->len, ==, 1);
	g_assert_cmpstr(axis_meta_title_get_id(title, "tmdb"), ==, "6003");
}

/* Spec: unknown status stays unknown. */
static void
test_parse_status(void)
{
	static const struct
	{
		AxisMetaKind     kind;
		const gchar     *status;
		AxisMetaStatus   expected;
	} cases[] = {
		{ AXIS_META_KIND_FILM, "Released", AXIS_META_STATUS_RELEASED },
		{ AXIS_META_KIND_FILM, "Rumored", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_FILM, "Planned", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_FILM, "In Production", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_FILM, "Post Production", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_FILM, "Canceled", AXIS_META_STATUS_UNKNOWN },
		{ AXIS_META_KIND_FILM, "released", AXIS_META_STATUS_UNKNOWN },
		{ AXIS_META_KIND_FILM, "Returning Series", AXIS_META_STATUS_UNKNOWN },
		{ AXIS_META_KIND_FILM, "", AXIS_META_STATUS_UNKNOWN },
		{ AXIS_META_KIND_SERIES, "Returning Series", AXIS_META_STATUS_CONTINUING },
		{ AXIS_META_KIND_SERIES, "Ended", AXIS_META_STATUS_ENDED },
		{ AXIS_META_KIND_SERIES, "Canceled", AXIS_META_STATUS_ENDED },
		{ AXIS_META_KIND_SERIES, "Planned", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_SERIES, "In Production", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_SERIES, "Pilot", AXIS_META_STATUS_UPCOMING },
		{ AXIS_META_KIND_SERIES, "Released", AXIS_META_STATUS_UNKNOWN },
		{ AXIS_META_KIND_SERIES, "Suspended Indefinitely", AXIS_META_STATUS_UNKNOWN }
	};
	g_autoptr(AxisMetaTitle) series = title_of(AXIS_META_KIND_SERIES, "tv-status-unknown.json", "US");
	guint i;

	g_assert_cmpint(series->status, ==, AXIS_META_STATUS_UNKNOWN);
	/* Its identifiers: IMDb's, and no TVDB one for a null. */
	g_assert_cmpstr(axis_meta_title_get_id(series, "imdb"), ==, "tt1234567");
	g_assert_null(axis_meta_title_get_id(series, "tvdb"));
	g_assert_cmpuint(series->seasons->len, ==, 1);

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *text = g_strdup_printf("{\"id\": 5, \"%s\": \"x\", \"status\": \"%s\"}",
			cases[i].kind == AXIS_META_KIND_FILM ? "title" : "name", cases[i].status);
		g_autoptr(AxisMetaTitle) title = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_meta_tmdb_parse_title(cases[i].kind, "US", text, strlen(text), &title, &error));
		if (title->status != cases[i].expected)
			g_error("%s: status %d, expected %d", cases[i].status, title->status, cases[i].expected);
	}
	{
		g_autoptr(AxisMetaTitle) none = NULL;
		g_autoptr(GError) error = NULL;
		const gchar *text = "{\"id\": 5, \"title\": \"x\"}";

		g_assert_true(axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", text, strlen(text), &none, &error));
		g_assert_cmpint(none->status, ==, AXIS_META_STATUS_UNKNOWN);
	}
}

static void
test_parse_seasons(void)
{
	g_autofree gchar *one_text = read_fixture("season-7101-1.json");
	g_autofree gchar *zero_text = read_fixture("season-7101-0.json");
	g_autoptr(AxisMetaSeason) one = NULL;
	g_autoptr(AxisMetaSeason) zero = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *problem = NULL;
	const AxisMetaEpisode *episode;

	g_assert_true(axis_meta_tmdb_parse_season(1, one_text, strlen(one_text), &one, &error));
	g_assert_no_error(error);
	g_assert_true(axis_meta_season_check(one, &problem));
	g_assert_cmpint(one->number, ==, 1);
	g_assert_cmpstr(one->name, ==, "Season 1");
	/* Episodes without a number, with a number as text, and a number twice
	 * are skipped: 1 to 5 and 7 remain. */
	g_assert_cmpuint(one->episodes->len, ==, 6);
	g_assert_cmpint(one->episode_count, ==, 6);
	episode = g_ptr_array_index(one->episodes, 0);
	g_assert_cmpint(episode->number, ==, 1);
	g_assert_cmpint(episode->season_number, ==, 1);
	g_assert_cmpstr(episode->name, ==, "Pilot");
	g_assert_cmpstr(episode->air_date, ==, "2008-01-20");
	g_assert_cmpint(episode->runtime, ==, 58);
	/* TMDb's own id of the episode, which stays when it is numbered anew. */
	g_assert_cmpstr(episode->provider_id, ==, "970005");
	/* TMDb has no absolute numbers. */
	g_assert_cmpint(episode->absolute_number, ==, 0);
	episode = g_ptr_array_index(one->episodes, 2);
	g_assert_cmpint(episode->runtime, ==, 0);
	episode = g_ptr_array_index(one->episodes, 3);
	g_assert_null(episode->air_date);
	episode = g_ptr_array_index(one->episodes, 4);
	g_assert_cmpint(episode->number, ==, 5);
	g_assert_null(episode->name);
	episode = g_ptr_array_index(one->episodes, 5);
	g_assert_cmpint(episode->number, ==, 7);

	/* Season zero: the specials. */
	g_assert_true(axis_meta_tmdb_parse_season(0, zero_text, strlen(zero_text), &zero, &error));
	g_assert_true(axis_meta_season_check(zero, NULL));
	g_assert_cmpint(zero->number, ==, 0);
	g_assert_cmpstr(zero->name, ==, "Specials");
	g_assert_cmpuint(zero->episodes->len, ==, 2);
	g_assert_cmpint(((AxisMetaEpisode *)g_ptr_array_index(zero->episodes, 1))->season_number, ==, 0);
}

static void
test_parse_find(void)
{
	g_autofree gchar *movie = read_fixture("find-imdb-movie.json");
	g_autofree gchar *series = read_fixture("find-tvdb-series.json");
	g_autofree gchar *none = read_fixture("find-none.json");
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GError) error = NULL;
	const AxisMetaTitle *title;

	g_assert_true(axis_meta_tmdb_parse_find(AXIS_META_KIND_FILM, "imdb", "tt0600003", movie, strlen(movie), FALSE,
		&titles, &error));
	assert_ids(titles, "6003");
	assert_titles_checked(titles);
	title = g_ptr_array_index(titles, 0);
	g_assert_cmpstr(axis_meta_title_get_id(title, "imdb"), ==, "tt0600003");
	g_assert_cmpint(title->year, ==, 1999);
	g_clear_pointer(&titles, g_ptr_array_unref);

	/* Series results are not films. */
	g_assert_true(axis_meta_tmdb_parse_find(AXIS_META_KIND_SERIES, "imdb", "tt0600003", movie, strlen(movie), FALSE,
		&titles, &error));
	g_assert_cmpuint(titles->len, ==, 0);
	g_clear_pointer(&titles, g_ptr_array_unref);

	g_assert_true(axis_meta_tmdb_parse_find(AXIS_META_KIND_SERIES, "tvdb", "82001", series, strlen(series), FALSE,
		&titles, &error));
	assert_ids(titles, "7101");
	title = g_ptr_array_index(titles, 0);
	g_assert_cmpstr(axis_meta_title_get_id(title, "tvdb"), ==, "82001");
	g_assert_cmpstr(title->first_air_date, ==, "2008-01-20");
	g_clear_pointer(&titles, g_ptr_array_unref);

	g_assert_true(axis_meta_tmdb_parse_find(AXIS_META_KIND_FILM, "imdb", "tt0000001", none, strlen(none), FALSE,
		&titles, &error));
	g_assert_cmpuint(titles->len, ==, 0);
	g_clear_pointer(&titles, g_ptr_array_unref);

	/* Several results: the identifier asked for is not put on any of them. */
	{
		const gchar *two = "{\"movie_results\": [{\"id\": 1, \"title\": \"a\"}, {\"id\": 2, \"title\": \"b\"}]}";

		g_assert_true(axis_meta_tmdb_parse_find(AXIS_META_KIND_FILM, "imdb", "tt0600003", two, strlen(two), FALSE,
			&titles, &error));
		g_assert_cmpuint(titles->len, ==, 2);
		g_assert_null(axis_meta_title_get_id(g_ptr_array_index(titles, 0), "imdb"));
		g_assert_null(axis_meta_title_get_id(g_ptr_array_index(titles, 1), "imdb"));
	}
}

static void
assert_unreadable(
	gboolean  ok,
	GError   *error
){
	g_assert_false(ok);
	g_assert_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE);
	g_error_free(error);
}

static void
test_parse_refused(void)
{
	static const gchar *const not_titles[] = {
		"", "not json", "{", "[]", "null", "42", "\"text\"", "{}",
		"{\"id\": \"6003\", \"title\": \"x\"}",
		"{\"id\": null, \"title\": \"x\"}",
		"{\"id\": 6003.5, \"title\": \"x\"}",
		"{\"id\": 0, \"title\": \"x\"}",
		"{\"id\": -3, \"title\": \"x\"}",
		"{\"id\": 1e3, \"title\": \"x\"}",
		"{\"id\": 1234567890123, \"title\": \"x\"}",
		"{\"id\": 6003, \"title\": \"\"}",
		"{\"id\": 6003, \"title\": 7}",
		"{\"id\": 6003, \"title\": \"x\", \"media_type\": \"tv\"}"
	};
	g_autofree gchar *array = read_fixture("not-an-object.json");
	g_autofree gchar *no_title = read_fixture("movie-no-title.json");
	g_autofree gchar *no_list = read_fixture("search-no-list.json");
	g_autofree gchar *tv = read_fixture("tv-7101.json");
	g_autofree gchar *film = read_fixture("movie-6003.json");
	g_autofree gchar *season = read_fixture("season-wrong-number.json");
	g_autofree gchar *no_episodes = read_fixture("season-no-episodes.json");
	g_autoptr(GString) deep = g_string_new(NULL);
	AxisMetaTitle *title = NULL;
	AxisMetaSeason *out_season = NULL;
	GPtrArray *titles = NULL;
	GError *error = NULL;
	gboolean ok;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(not_titles); i++) {
		error = NULL;
		ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", not_titles[i], strlen(not_titles[i]), &title, &error);
		g_assert_null(title);
		assert_unreadable(ok, error);
	}
	error = NULL;
	ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", NULL, 0, &title, &error);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", array, strlen(array), &title, &error);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", no_title, strlen(no_title), &title, &error);
	assert_unreadable(ok, error);
	/* A series is not a film, nor a film a series. */
	error = NULL;
	ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", tv, strlen(tv), &title, &error);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_SERIES, "US", film, strlen(film), &title, &error);
	assert_unreadable(ok, error);

	error = NULL;
	ok = axis_meta_tmdb_parse_search(AXIS_META_KIND_FILM, array, strlen(array), FALSE, &titles, &error);
	g_assert_null(titles);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_search(AXIS_META_KIND_FILM, no_list, strlen(no_list), FALSE, &titles, &error);
	g_assert_null(titles);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_search(AXIS_META_KIND_FILM, "", 0, FALSE, &titles, &error);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_find(AXIS_META_KIND_FILM, "imdb", "tt0600003", no_list, strlen(no_list), FALSE, &titles,
		&error);
	g_assert_null(titles);
	assert_unreadable(ok, error);

	error = NULL;
	ok = axis_meta_tmdb_parse_season(1, season, strlen(season), &out_season, &error);
	g_assert_null(out_season);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_season(1, no_episodes, strlen(no_episodes), &out_season, &error);
	g_assert_null(out_season);
	assert_unreadable(ok, error);
	error = NULL;
	ok = axis_meta_tmdb_parse_season(1, array, strlen(array), &out_season, &error);
	assert_unreadable(ok, error);

	/* Nesting beyond what the reader takes. */
	for (i = 0; i < 200; i++)
		g_string_append(deep, "{\"a\":");
	g_string_append(deep, "1");
	for (i = 0; i < 200; i++)
		g_string_append_c(deep, '}');
	error = NULL;
	ok = axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", deep->str, deep->len, &title, &error);
	assert_unreadable(ok, error);
}

/* Lists far beyond any real answer are cut, and what remains is checked. */
static void
test_parse_huge_lists(void)
{
	g_autoptr(GString) text = g_string_new(NULL);
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(AxisMetaTitle) title = NULL;
	g_autoptr(AxisMetaSeason) season = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *long_name = g_strnfill(3000, 'a');
	guint i;

	g_string_append(text, "{\"results\": [");
	for (i = 1; i <= 120; i++)
		g_string_append_printf(text, "%s{\"id\": %u, \"title\": \"Title %u\"}", i > 1 ? "," : "", i, i);
	g_string_append(text, "]}");
	g_assert_true(axis_meta_tmdb_parse_search(AXIS_META_KIND_FILM, text->str, text->len, FALSE, &titles, &error));
	g_assert_cmpuint(titles->len, ==, AXIS_META_MAX_RESULTS);
	assert_titles_checked(titles);

	/* Other names, genres and seasons. */
	g_string_truncate(text, 0);
	g_string_append_printf(text, "{\"id\": 9, \"name\": \"%s\", \"genres\": [", long_name);
	for (i = 0; i < 60; i++)
		g_string_append_printf(text, "%s{\"id\": %u, \"name\": \"Genre %u\"}", i > 0 ? "," : "", i, i);
	g_string_append(text, "], \"alternative_titles\": {\"results\": [");
	for (i = 0; i < 400; i++)
		g_string_append_printf(text, "%s{\"iso_3166_1\": \"DE\", \"title\": \"Name %u\"}", i > 0 ? "," : "", i);
	g_string_append(text, "]}, \"seasons\": [");
	for (i = 0; i < 300; i++)
		g_string_append_printf(text, "%s{\"season_number\": %u, \"episode_count\": 1}", i > 0 ? "," : "", i);
	g_string_append(text, "]}");
	g_assert_true(axis_meta_tmdb_parse_title(AXIS_META_KIND_SERIES, "US", text->str, text->len, &title, &error));
	g_assert_true(axis_meta_title_check(title, "tmdb", &problem));
	g_assert_cmpuint(title->genres->len, ==, AXIS_META_MAX_GENRES);
	g_assert_cmpuint(title->alt_titles->len, ==, AXIS_META_MAX_ALT_TITLES);
	g_assert_cmpuint(title->seasons->len, ==, AXIS_META_MAX_SEASONS);
	g_assert_cmpuint(g_utf8_strlen(title->title, -1), ==, AXIS_META_TITLE_MAX_CHARS);

	g_string_truncate(text, 0);
	g_string_append(text, "{\"season_number\": 1, \"episodes\": [");
	for (i = 1; i <= 2500; i++)
		g_string_append_printf(text, "%s{\"episode_number\": %u, \"name\": \"E%u\"}", i > 1 ? "," : "", i, i);
	g_string_append(text, "]}");
	g_assert_true(axis_meta_tmdb_parse_season(1, text->str, text->len, &season, &error));
	g_assert_true(axis_meta_season_check(season, &problem));
	g_assert_cmpuint(season->episodes->len, ==, AXIS_META_MAX_EPISODES);
}

/* Control and direction-changing characters in a name become spaces. */
static void
test_parse_cleans_text(void)
{
	const gchar *text = "{\"id\": 3, \"title\": \"Evil\\u202eTitle\\u0007\\u0000!\", \"overview\": \"a\\n\\n  b\"}";
	g_autoptr(AxisMetaTitle) title = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", text, strlen(text), &title, &error));
	g_assert_true(axis_meta_title_check(title, "tmdb", NULL));
	g_assert_true(g_utf8_validate(title->title, -1, NULL));
	g_assert_null(strstr(title->title, "\xe2\x80\xae"));
	g_assert_cmpstr(title->overview, ==, "a b");
}

/*
 * The review of fix-identify-hunt-findings, item 1: only a theatrical date
 * in the region (types 2 and 3) within two years of the first release is a
 * year of the film; a disc (5) or a stream (4) is not.  The stored release
 * date stays the earliest of the four kinds, as the movies module reads it.
 * Made-up titles.
 */
static void
test_parse_regional_year(void)
{
	static const struct
	{
		const gchar *dates;
		gint         regional;
		const gchar *release;
	} cases[] = {
		/* A disc decades later. */
		{ "{\"type\": 5, \"release_date\": \"2009-05-05T00:00:00.000Z\"}", 0, "2009-05-05" },
		/* A stream a year later. */
		{ "{\"type\": 4, \"release_date\": \"1976-03-01T00:00:00.000Z\"}", 0, "1976-03-01" },
		/* In cinemas a year later. */
		{ "{\"type\": 3, \"release_date\": \"1976-02-01T00:00:00.000Z\"}", 1976, "1976-02-01" },
		/* In cinemas two years later, in a limited release. */
		{ "{\"type\": 2, \"release_date\": \"1977-02-01T00:00:00.000Z\"}", 1977, "1977-02-01" },
		/* In cinemas three years later: too far to be a year of it. */
		{ "{\"type\": 3, \"release_date\": \"1978-02-01T00:00:00.000Z\"}", 0, "1978-02-01" },
		/* The same year: nothing to add. */
		{ "{\"type\": 3, \"release_date\": \"1975-12-01T00:00:00.000Z\"}", 0, "1975-12-01" },
		/* A disc first, then cinemas: the date is the disc's, the year the
		 * cinemas'. */
		{ "{\"type\": 5, \"release_date\": \"1976-01-01T00:00:00.000Z\"}, {\"type\": 3, \"release_date\":"
		  " \"1976-09-01T00:00:00.000Z\"}", 1976, "1976-01-01" },
		{ NULL, 0, NULL }
	};
	guint i;

	for (i = 0; cases[i].dates != NULL; i++) {
		g_autofree gchar *text = g_strdup_printf("{\"id\": 7, \"title\": \"Iron Orchard\", \"release_date\":"
			" \"1975-06-01\", \"runtime\": 104, \"release_dates\": {\"results\": [{\"iso_3166_1\": \"US\","
			" \"release_dates\": [%s]}]}}", cases[i].dates);
		g_autoptr(AxisMetaTitle) title = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_meta_tmdb_parse_title(AXIS_META_KIND_FILM, "US", text, strlen(text), &title, &error));
		g_assert_no_error(error);
		g_assert_true(axis_meta_title_check(title, "tmdb", NULL));
		g_assert_cmpint(title->year, ==, 1975);
		g_assert_cmpint(title->regional_year, ==, cases[i].regional);
		g_assert_cmpstr(title->release_date, ==, cases[i].release);
	}
}

/* ── TMDb's own forms ───────────────────────────────────────────── */

/* Spec: An image path that does not match the pattern is not used. */
static void
test_image_paths(void)
{
	static const gchar *const bad[] = {
		NULL, "", "/", "x.jpg", "/.jpg", "/abc", "/abc.gif", "/abc.JPG", "/abc.jpeg", "/abc.jpg.exe", "/abc.jpg?x=1",
		"/abc.jpg#f", "/a/b.jpg", "/a_b.jpg", "/a-b.jpg", "/a b.jpg", "/../abc.jpg", "//abc.jpg", "/abc..jpg",
		"https://evil.example/abc.jpg", "//evil.example/abc.jpg", "/abc.jpg/", "/abc\n.jpg", "/\xc3\xa9.jpg"
	};
	g_autofree gchar *hundred = g_strnfill(100, 'a');
	g_autofree gchar *hundred_one = g_strnfill(101, 'a');
	g_autofree gchar *longest = g_strdup_printf("/%s.png", hundred);
	g_autofree gchar *too_long = g_strdup_printf("/%s.png", hundred_one);
	g_autofree gchar *path = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_assert_false(axis_meta_tmdb_image_ref_valid(bad[i]));
		g_assert_null(axis_meta_tmdb_image_path(bad[i], AXIS_META_ART_POSTER));
	}
	g_assert_false(axis_meta_tmdb_image_ref_valid(too_long));
	g_assert_true(axis_meta_tmdb_image_ref_valid(longest));
	g_assert_true(axis_meta_tmdb_image_ref_valid("/madeUpPoster000000000006003.jpg"));
	g_assert_true(axis_meta_tmdb_image_ref_valid("/abc123.png"));
	g_assert_true(axis_meta_tmdb_image_ref_valid("/abc123.webp"));

	path = axis_meta_tmdb_image_path("/abc123.jpg", AXIS_META_ART_POSTER_SMALL);
	g_assert_cmpstr(path, ==, "/t/p/w342/abc123.jpg");
	g_free(path);
	path = axis_meta_tmdb_image_path("/abc123.jpg", AXIS_META_ART_POSTER);
	g_assert_cmpstr(path, ==, "/t/p/w780/abc123.jpg");
	g_free(path);
	path = axis_meta_tmdb_image_path("/abc123.webp", AXIS_META_ART_BACKDROP);
	g_assert_cmpstr(path, ==, "/t/p/w1280/abc123.webp");
	g_assert_null(axis_meta_tmdb_image_path("/abc123.jpg", (AxisMetaArtSize)3));
	g_assert_null(axis_meta_tmdb_image_path("/abc123.jpg", (AxisMetaArtSize)-1));
}

static void
test_key_forms(void)
{
	static const gchar *const accepted[] = {
		TOKEN, "eyJ", "0123456789abcdef0123456789abcde", "0123456789abcdef0123456789abcdef0",
		"0123456789abcdef0123456789abcdeg", "x"
	};
	static const gchar *const short_keys[] = {
		"0123456789abcdef0123456789abcdef", "0123456789ABCDEF0123456789ABCDEF", SHORT_KEY, "00000000000000000000000000000000"
	};
	static const gchar *const unusable[] = {
		"two words", "tab\there", "new\nline", "caf\xc3\xa9", "del\x7f"
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(accepted); i++) {
		g_autofree gchar *reason = NULL;

		g_assert_true(axis_meta_tmdb_key_valid(accepted[i], &reason));
		g_assert_null(reason);
	}
	for (i = 0; i < G_N_ELEMENTS(short_keys); i++) {
		g_autofree gchar *reason = NULL;

		g_assert_false(axis_meta_tmdb_key_valid(short_keys[i], &reason));
		g_assert_nonnull(strstr(reason, "API Read Access Token"));
		g_assert_nonnull(strstr(reason, "short"));
		g_assert_null(strstr(reason, short_keys[i]));
	}
	for (i = 0; i < G_N_ELEMENTS(unusable); i++) {
		g_autofree gchar *reason = NULL;

		g_assert_false(axis_meta_tmdb_key_valid(unusable[i], &reason));
		g_assert_nonnull(reason);
	}
}

static void
test_language_and_region_forms(void)
{
	static const gchar *const languages[] = { "en", "en-US", "de-DE", "pt-BR", "fil", "fil-PH", NULL };
	static const gchar *const not_languages[] = {
		"", "e", "EN", "en-us", "en_US", "en-U", "en-USA", "english", "en US", "e1", "en-", "-US", "1234", "en-US ", NULL
	};
	static const gchar *const regions[] = { "US", "GB", "DE", NULL };
	static const gchar *const not_regions[] = { "", "U", "us", "USA", "U1", "U ", NULL };
	guint i;

	for (i = 0; languages[i] != NULL; i++)
		g_assert_true(axis_meta_tmdb_language_valid(languages[i]));
	for (i = 0; not_languages[i] != NULL; i++)
		g_assert_false(axis_meta_tmdb_language_valid(not_languages[i]));
	for (i = 0; regions[i] != NULL; i++)
		g_assert_true(axis_meta_tmdb_region_valid(regions[i]));
	for (i = 0; not_regions[i] != NULL; i++)
		g_assert_false(axis_meta_tmdb_region_valid(not_regions[i]));
	g_assert_false(axis_meta_tmdb_language_valid(NULL));
	g_assert_false(axis_meta_tmdb_region_valid(NULL));
}

/* What the provider declares: its fixed origins, its header and the notice. */
static void
test_provider_declaration(void)
{
	const AxisMetaProviderInfo *info = axis_meta_tmdb_get_info();

	g_assert_cmpstr(info->name, ==, "tmdb");
	g_assert_cmpstr(info->label, ==, "TMDb");
	g_assert_cmpstr(info->api_origin, ==, "https://api.themoviedb.org");
	g_assert_cmpstr(info->image_origin, ==, "https://image.tmdb.org");
	g_assert_cmpstr(info->key_header, ==, "Authorization");
	g_assert_cmpstr(info->key_prefix, ==, "Bearer ");
	g_assert_cmpfloat(info->default_rate, ==, 20.0);
	g_assert_true((info->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_FILM)) != 0);
	g_assert_true((info->kinds & AXIS_META_KIND_BIT(AXIS_META_KIND_SERIES)) != 0);
	/* Spec: Notice (the Metadata page shows every provider's attribution). */
	g_assert_nonnull(strstr(info->attribution, "uses the TMDb API"));
	g_assert_nonnull(strstr(info->attribution, "not endorsed or certified by TMDb"));
	g_assert_nonnull(strstr(info->key_help, "API Read Access Token"));
	g_assert_true(axis_meta_provider_name_valid(info->name));
}

/* ── Through the metadata layer, against a fake TMDb ─────────────── */

typedef struct
{
	AxisTestDb           *db;
	AxisDbPool           *pool;
	AxisSettings         *settings;
	gchar                *dir;
	gchar                *data_path;
	AxisDataDir          *data;
	AxisMeta             *meta;
	AxisMetaProviderInfo  info;	/* the real one, pointed at the fake servers */
	AxisTestPlex         *api;
	AxisTestPlex         *images;
	gchar                *api_origin;
	gchar                *image_origin;
} Fixture;

static void
routes(Fixture *fx)
{
	static const struct
	{
		const gchar *path;
		const gchar *file;
	} files[] = {
		{ "/3/search/movie", DIR "search-movie.json" },
		{ "/3/search/tv", DIR "search-tv.json" },
		{ "/3/movie/6003", DIR "movie-6003.json" },
		{ "/3/tv/7101", DIR "tv-7101.json" },
		{ "/3/tv/7101/season/0", DIR "season-7101-0.json" },
		{ "/3/tv/7101/season/1", DIR "season-7101-1.json" },
		{ "/3/find/tt0600003", DIR "find-imdb-movie.json" },
		{ "/3/find/82001", DIR "find-tvdb-series.json" },
		{ NULL, NULL }
	};
	const gchar *const images[] = { "/t/p/w342/madeUpPoster000000000006003.jpg",
		"/t/p/w780/madeUpPoster000000000006003.jpg", "/t/p/w1280/madeUpBackdrop0000000006003.jpg",
		"/t/p/w342/madeUpPoster000000000007101.jpg", NULL };
	g_autoptr(GBytes) png = axis_test_meta_png();
	guint i;

	for (i = 0; files[i].path != NULL; i++)
		axis_test_plex_route_file(fx->api, files[i].path, -1, files[i].file);
	for (i = 0; images[i] != NULL; i++)
		axis_test_plex_route_full(fx->images, images[i], 200, "image/png", png, NULL);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->data_path = g_build_filename(fx->dir, "data", NULL);
	fx->data = axis_data_dir_open(fx->data_path, &error);
	g_assert_no_error(error);
	fx->settings = axis_settings_new();
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);

	/* The provider as the daemon registers it, but for the two addresses,
	 * which are the only thing a test changes (on a copy). */
	fx->api = axis_test_plex_new();
	fx->images = axis_test_plex_new();
	g_assert_nonnull(fx->api);
	g_assert_nonnull(fx->images);
	fx->api_origin = axis_test_plex_base(fx->api);
	fx->image_origin = axis_test_plex_base(fx->images);
	fx->info = *axis_meta_tmdb_get_info();
	fx->info.api_origin = fx->api_origin;
	fx->info.image_origin = fx->image_origin;
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->info, &error));
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_META_MODULE_ID, axis_meta_get_setting_table(fx->meta),
		&error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, TOKEN, &error));
	g_assert_no_error(error);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
	routes(fx);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->meta != NULL) {
		axis_meta_stop(fx->meta);
		axis_meta_unref(fx->meta);
	}
	if (fx->settings != NULL)
		axis_settings_unref(fx->settings);
	axis_test_plex_free(fx->api);
	axis_test_plex_free(fx->images);
	g_free(fx->api_origin);
	g_free(fx->image_origin);
	if (fx->data != NULL)
		axis_data_dir_unref(fx->data);
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_free(fx->data_path);
	axis_test_storage_dir_free(fx->dir);
	axis_test_log_reset();
	while (g_main_context_iteration(NULL, FALSE))
		;
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

static void
search_ok(
	Fixture              *fx,
	AxisMetaKind          kind,
	const gchar          *text,
	gint                  year,
	AxisMetaSearchAnswer *answer
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_search(fx->meta, "tmdb", kind, text, year, WAIT, answer, &error));
	g_assert_no_error(error);
	g_assert_nonnull(answer->titles);
}

static void
lookup_ok(
	Fixture             *fx,
	AxisMetaKind         kind,
	const gchar         *id,
	AxisMetaTitleAnswer *answer
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_meta_lookup(fx->meta, "tmdb", kind, id, WAIT, answer, &error));
	g_assert_no_error(error);
}

static gint64
query_int(
	Fixture     *fx,
	const gchar *sql
){
	g_autofree gchar *text = axis_test_db_query(fx->db, sql);

	return text != NULL ? g_ascii_strtoll(text, NULL, 10) : -1;
}

static void
assert_token_absent(const gchar *text)
{
	if (text != NULL && strstr(text, TOKEN) != NULL)
		g_error("the token appears in: %s", text);
}

static gboolean
param_allowed(const gchar *pair)
{
	static const gchar *const names[] = { "query=", "include_adult=", "language=", "page=", "region=", "year=",
		"first_air_date_year=", "append_to_response=", "external_source=", NULL };
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		if (g_str_has_prefix(pair, names[i]))
			return TRUE;
	}
	return FALSE;
}

/* Every request: a GET, the token only as a bearer header, no secret or
 * unexpected parameter in the URL, nothing but the fixed headers. */
static void
assert_api_requests_plain(AxisTestPlex *plex)
{
	g_autoptr(GPtrArray) requests = axis_test_plex_requests(plex);
	guint i;

	g_assert_cmpuint(requests->len, >, 0);
	for (i = 0; i < requests->len; i++) {
		g_auto(GStrv) lines = g_strsplit(g_ptr_array_index(requests, i), "\n", -1);
		const gchar *query;
		gboolean bearer = FALSE;
		guint j;

		g_assert_true(g_str_has_prefix(lines[0], "GET /3/"));
		assert_token_absent(lines[0]);
		g_assert_null(strstr(lines[0], "api_key"));
		g_assert_null(strstr(lines[0], "token"));
		query = strchr(lines[0], '?');
		if (query != NULL) {
			g_auto(GStrv) pairs = g_strsplit(query + 1, "&", -1);

			for (j = 0; pairs[j] != NULL; j++) {
				if (!param_allowed(pairs[j]))
					g_error("an unexpected parameter was sent: %s", pairs[j]);
			}
		}
		for (j = 1; lines[j] != NULL && lines[j][0] != '\0'; j++) {
			const gchar *colon = strchr(lines[j], ':');
			g_autofree gchar *name = g_strndup(lines[j], (gsize)(colon - lines[j]));
			const gchar *value = colon + 2;

			if (g_ascii_strcasecmp(name, "Host") == 0 || g_ascii_strcasecmp(name, "Accept") == 0 ||
			    g_ascii_strcasecmp(name, "Connection") == 0 || g_ascii_strcasecmp(name, "User-Agent") == 0)
				continue;
			if (g_ascii_strcasecmp(name, "Authorization") == 0) {
				g_assert_cmpstr(value, ==, "Bearer " TOKEN);
				bearer = TRUE;
				continue;
			}
			g_error("an unexpected header was sent: %s", lines[j]);
		}
		g_assert_true(bearer);
	}
}

/* Spec: Search */
static void
test_search_films(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaSearchAnswer with_year;
	g_autofree gchar *transcript = NULL;
	const AxisMetaTitle *mazework;

	search_ok(fx, AXIS_META_KIND_FILM, "The Mazework", 0, &answer);
	g_assert_false(answer.saved);
	g_assert_true(answer.provider == &fx->info);
	g_assert_cmpuint(answer.titles->len, ==, 4);
	mazework = g_ptr_array_index(answer.titles, 0);
	g_assert_cmpstr(mazework->title, ==, "The Mazework");
	g_assert_cmpint(mazework->year, ==, 1999);
	g_assert_nonnull(mazework->overview);
	g_assert_cmpstr(mazework->poster_ref, ==, "/madeUpPoster000000000006003.jpg");
	g_assert_cmpstr(axis_meta_title_get_id(mazework, "tmdb"), ==, "6003");
	axis_meta_search_answer_clear(&answer);

	search_ok(fx, AXIS_META_KIND_FILM, "The Mazework", 1999, &with_year);
	g_assert_cmpuint(with_year.titles->len, ==, 4);
	axis_meta_search_answer_clear(&with_year);

	/* The second page (fix-identify-hunt-findings, item G): page=2. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_meta_search_page(fx->meta, "tmdb", AXIS_META_KIND_FILM, "The Mazework", 0, 2, WAIT,
			&with_year, &error));
		g_assert_no_error(error);
		axis_meta_search_answer_clear(&with_year);
	}

	/* Adults are off by default, and the request says so.  No region: with
	 * it, TMDb's answer gives the region's release date, not the first
	 * release (fix-identify-regional-years). */
	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript,
		"GET /3/search/movie?query=The%20Mazework&include_adult=false&language=en-US&page=1\n"));
	g_assert_nonnull(strstr(transcript,
		"GET /3/search/movie?query=The%20Mazework&include_adult=false&language=en-US&page=1&year=1999\n"));
	g_assert_nonnull(strstr(transcript,
		"GET /3/search/movie?query=The%20Mazework&include_adult=false&language=en-US&page=2\n"));
	g_assert_null(strstr(transcript, "region="));
	assert_api_requests_plain(fx->api);
}

/*
 * A stand-in for TMDb answering as its documentation says "region" acts on
 * a search: a filter of the release date shown, so that with region=US the
 * answer's release_date is the date in the United States.  A made-up film
 * first released in Japan in 1988 and in the United States in 2005.
 */
static void
regional_answers(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	gboolean regional = request->query != NULL && strstr(request->query, "region=US") != NULL;
	gchar *json;

	answer->content_type = g_strdup("application/json");
	if (strcmp(request->path, "/3/search/movie") == 0) {
		json = g_strdup_printf("{\"page\":1,\"total_pages\":1,\"total_results\":1,\"results\":[{\"id\":9101,"
			"\"title\":\"Paper Lantern Sky\",\"original_title\":\"Paper Lantern Sky\",\"original_language\":\"ja\","
			"\"release_date\":\"%s\",\"adult\":false}]}", regional ? "2005-03-04" : "1988-04-16");
	} else if (strcmp(request->path, "/3/movie/9101") == 0) {
		json = g_strdup("{\"id\":9101,\"title\":\"Paper Lantern Sky\",\"original_title\":\"Paper Lantern Sky\","
			"\"original_language\":\"ja\",\"release_date\":\"1988-04-16\",\"runtime\":86,\"status\":\"Released\","
			"\"release_dates\":{\"results\":[{\"iso_3166_1\":\"JP\",\"release_dates\":[{\"release_date\":"
			"\"1988-04-16T00:00:00.000Z\",\"type\":3}]},{\"iso_3166_1\":\"US\",\"release_dates\":[{\"release_date\":"
			"\"2005-03-04T00:00:00.000Z\",\"type\":3}]}]}}");
	} else {
		answer->status = 404;
		return;
	}
	answer->status = 200;
	answer->body = g_bytes_new_take(json, strlen(json));
}

/*
 * fix-identify-regional-years, A: a film first released abroad.  The
 * search sent region=US, and TMDb's answer then gives the date in the
 * region: the year stored from the search was 2005, so identification
 * left the folder "Paper Lantern Sky (1988)" unknown before ever reading
 * the full record.  The search sends no region now, and the year stored
 * from it is the first release's; the full record still takes its date in
 * the region from its release dates.
 */
static void
test_search_first_release_year(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer record;
	g_autofree gchar *transcript = NULL;
	const AxisMetaTitle *film;

	axis_test_plex_clear_routes(fx->api);
	axis_test_plex_set_handler(fx->api, regional_answers, NULL);
	search_ok(fx, AXIS_META_KIND_FILM, "Paper Lantern Sky", 0, &answer);
	g_assert_cmpuint(answer.titles->len, ==, 1);
	film = g_ptr_array_index(answer.titles, 0);
	g_assert_cmpint(film->year, ==, 1988);
	g_assert_cmpstr(film->release_date, ==, "1988-04-16");
	axis_meta_search_answer_clear(&answer);

	lookup_ok(fx, AXIS_META_KIND_FILM, "9101", &record);
	g_assert_cmpint(record.title->year, ==, 1988);
	g_assert_cmpstr(record.title->release_date, ==, "2005-03-04");
	g_assert_cmpint(record.title->regional_year, ==, 0);
	axis_meta_title_answer_clear(&record);

	transcript = axis_test_plex_transcript(fx->api);
	g_assert_null(strstr(transcript, "region="));
	axis_test_plex_set_handler(fx->api, NULL, NULL);
}

static void
test_search_series(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	g_autofree gchar *transcript = NULL;
	const AxisMetaTitle *bad;

	search_ok(fx, AXIS_META_KIND_SERIES, "cracking slate", 2008, &answer);
	g_assert_cmpuint(answer.titles->len, ==, 2);
	bad = g_ptr_array_index(answer.titles, 0);
	g_assert_cmpstr(bad->title, ==, "Cracking Slate");
	g_assert_cmpint(bad->year, ==, 2008);
	g_assert_cmpstr(bad->first_air_date, ==, "2008-01-20");
	axis_meta_search_answer_clear(&answer);

	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript,
		"GET /3/search/tv?query=cracking%20slate&include_adult=false&language=en-US&page=1&first_air_date_year=2008\n"));
	assert_api_requests_plain(fx->api);
}

/* Adult titles are off until the setting says otherwise; language and
 * region follow their settings. */
static void
test_search_settings(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer answer;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *language = axis_settings_get_string(fx->settings, AXIS_META_TMDB_SETTING_LANGUAGE);
	g_autofree gchar *region = axis_settings_get_string(fx->settings, AXIS_META_TMDB_SETTING_REGION);

	g_assert_false(axis_settings_get_boolean(fx->settings, AXIS_META_TMDB_SETTING_ADULT));
	g_assert_cmpstr(language, ==, "en-US");
	g_assert_cmpstr(region, ==, "US");
	g_assert_cmpfloat(axis_settings_get_decimal(fx->settings, AXIS_META_TMDB_SETTING_RATE), ==, 20.0);

	search_ok(fx, AXIS_META_KIND_FILM, "mazework", 0, &answer);
	g_assert_cmpuint(answer.titles->len, ==, 4);
	axis_meta_search_answer_clear(&answer);

	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_ADULT, "true", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_LANGUAGE, "de-DE", &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_REGION, "GB", &error));
	g_assert_no_error(error);
	search_ok(fx, AXIS_META_KIND_FILM, "mazework again", 0, &answer);
	g_assert_cmpuint(answer.titles->len, ==, 5);
	axis_meta_search_answer_clear(&answer);

	/* The region is not sent with a search (it stays for the full record's
	 * dates: /meta/tmdb/film/region-setting). */
	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript,
		"GET /3/search/movie?query=mazework%20again&include_adult=true&language=de-DE&page=1\n"));
	g_assert_null(strstr(transcript, "region="));
}

/* Spec: Settings: a short key is refused with a reason, and so is anything
 * that is not a language or a region. */
static void
test_settings_refuse(Fixture *fx, gconstpointer data)
{
	AxisSettingProblem *problem = NULL;
	g_autoptr(GError) error = NULL;
	const AxisSettingInfo *key = axis_settings_lookup(fx->settings, AXIS_META_TMDB_SETTING_KEY, NULL);

	g_assert_nonnull(key);
	g_assert_true((key->flags & AXIS_SETTING_SECRET) != 0);
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_KEY, SHORT_KEY, &problem));
	g_assert_nonnull(problem);
	g_assert_nonnull(strstr(problem->reason, "API Read Access Token"));
	g_assert_null(strstr(problem->message, SHORT_KEY));
	g_assert_null(strstr(problem->reason, SHORT_KEY));
	axis_setting_problem_free(problem);

	/* Saving it is refused, and the token already saved stays. */
	g_assert_false(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, SHORT_KEY, &error));
	g_assert_nonnull(error);
	g_assert_null(strstr(error->message, SHORT_KEY));
	g_clear_error(&error);
	g_assert_true(axis_settings_is_set(fx->settings, AXIS_META_TMDB_SETTING_KEY));
	g_assert_null(axis_settings_format(fx->settings, AXIS_META_TMDB_SETTING_KEY));
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_KEY, "two words", NULL));
	g_assert_true(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_KEY, TOKEN, NULL));

	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_LANGUAGE, "english", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_LANGUAGE, "en-us", NULL));
	g_assert_true(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_LANGUAGE, "pt-BR", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_REGION, "usa", NULL));
	g_assert_false(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_REGION, "U", NULL));
	g_assert_true(axis_settings_check(fx->settings, AXIS_META_TMDB_SETTING_REGION, "GB", NULL));
}

/* Spec: Search (details); alternative titles, identifiers, regional dates. */
static void
test_film_details(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer answer;
	g_autofree gchar *transcript = NULL;

	lookup_ok(fx, AXIS_META_KIND_FILM, "6003", &answer);
	g_assert_false(answer.saved);
	g_assert_nonnull(answer.title);
	g_assert_cmpstr(answer.title->title, ==, "The Mazework");
	g_assert_cmpint(answer.title->runtime, ==, 136);
	g_assert_cmpint(answer.title->status, ==, AXIS_META_STATUS_RELEASED);
	g_assert_cmpstr(answer.title->release_date, ==, "1999-03-31");
	g_assert_cmpuint(answer.title->genres->len, ==, 2);
	g_assert_cmpuint(answer.title->alt_titles->len, ==, 5);
	g_assert_cmpstr(axis_meta_title_get_id(answer.title, "imdb"), ==, "tt0600003");
	axis_meta_title_answer_clear(&answer);

	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript, "GET /3/movie/6003?language=en-US&append_to_response="
		"alternative_titles,external_ids,release_dates\n"));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 1);
	assert_api_requests_plain(fx->api);
}

/* Spec: release dates are those of the configured region. */
static void
test_film_region_setting(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_REGION, "GB", &error));
	g_assert_no_error(error);
	lookup_ok(fx, AXIS_META_KIND_FILM, "6003", &answer);
	g_assert_cmpstr(answer.title->release_date, ==, "1999-06-11");
	g_assert_cmpint(answer.title->year, ==, 1999);
	axis_meta_title_answer_clear(&answer);
}

/* Series: seasons on demand, specials, and only the ones asked for. */
static void
test_series_and_seasons(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer title;
	AxisMetaSeasonAnswer season;
	AxisMetaSeasonAnswer missing;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	const AxisMetaEpisode *episode;

	lookup_ok(fx, AXIS_META_KIND_SERIES, "7101", &title);
	g_assert_nonnull(title.title);
	g_assert_cmpint(title.title->status, ==, AXIS_META_STATUS_ENDED);
	g_assert_cmpint(title.title->runtime, ==, 45);
	g_assert_cmpstr(axis_meta_title_get_id(title.title, "imdb"), ==, "tt0700001");
	g_assert_cmpstr(axis_meta_title_get_id(title.title, "tvdb"), ==, "82001");
	g_assert_cmpuint(title.title->alt_titles->len, ==, 3);
	g_assert_cmpuint(title.title->seasons->len, ==, 4);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 1);

	g_assert_true(axis_meta_get_season(fx->meta, title.title->id, 1, WAIT, &season, &error));
	g_assert_no_error(error);
	g_assert_nonnull(season.season);
	g_assert_cmpuint(season.season->episodes->len, ==, 6);
	episode = g_ptr_array_index(season.season->episodes, 0);
	g_assert_cmpstr(episode->name, ==, "Pilot");
	g_assert_cmpint(episode->absolute_number, ==, 0);
	axis_meta_season_answer_clear(&season);

	g_assert_true(axis_meta_get_season(fx->meta, title.title->id, 0, WAIT, &season, &error));
	g_assert_nonnull(season.season);
	g_assert_cmpstr(season.season->name, ==, "Specials");
	g_assert_cmpuint(season.season->episodes->len, ==, 2);
	axis_meta_season_answer_clear(&season);

	/* A season TMDb does not have. */
	g_assert_true(axis_meta_get_season(fx->meta, title.title->id, 5, WAIT, &missing, &error));
	g_assert_true(missing.not_found);
	g_assert_null(missing.season);
	axis_meta_season_answer_clear(&missing);

	/* Only the seasons asked for were requested. */
	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript, "GET /3/tv/7101?language=en-US&append_to_response=alternative_titles,"
		"external_ids\n"));
	g_assert_nonnull(strstr(transcript, "GET /3/tv/7101/season/1?language=en-US\n"));
	g_assert_nonnull(strstr(transcript, "GET /3/tv/7101/season/0?language=en-US\n"));
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/3/tv/7101/season/2"), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/3/tv/7101/season/3"), ==, 0);
	axis_meta_title_answer_clear(&title);
	assert_api_requests_plain(fx->api);
}

/* Spec: One record across lookups: searched, fetched, then found by IMDb
 * identifier. */
static void
test_one_record_across_lookups(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	AxisMetaSearchAnswer find;
	AxisMetaSearchAnswer by_tvdb;
	AxisMetaSearchAnswer none;
	AxisMetaTitleAnswer title;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *transcript = NULL;
	gint64 id;

	search_ok(fx, AXIS_META_KIND_FILM, "mazework", 0, &search);
	id = ((AxisMetaTitle *)g_ptr_array_index(search.titles, 0))->id;
	g_assert_cmpint(id, >, 0);
	axis_meta_search_answer_clear(&search);
	lookup_ok(fx, AXIS_META_KIND_FILM, "6003", &title);
	g_assert_cmpint(title.title->id, ==, id);
	axis_meta_title_answer_clear(&title);

	g_assert_true(axis_meta_find(fx->meta, "tmdb", AXIS_META_KIND_FILM, "imdb", "tt0600003", WAIT, &find, &error));
	g_assert_no_error(error);
	g_assert_false(find.saved);
	g_assert_cmpuint(find.titles->len, ==, 1);
	g_assert_cmpint(((AxisMetaTitle *)g_ptr_array_index(find.titles, 0))->id, ==, id);
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(find.titles, 0), "tmdb"), ==, "6003");
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(find.titles, 0), "imdb"), ==, "tt0600003");
	axis_meta_search_answer_clear(&find);

	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE kind = 'film' AND title = 'The Mazework'"), ==, 1);
	g_assert_cmpint(query_int(fx, "SELECT count(DISTINCT title_id) FROM meta_external_id WHERE kind = 'film' AND "
		"value IN ('6003', 'tt0600003')"), ==, 1);

	/* A series by its TVDB number; a film is not looked up that way (TVDB
	 * numbers series), and TMDb's own number is not another scheme. */
	g_assert_true(axis_meta_find(fx->meta, "tmdb", AXIS_META_KIND_SERIES, "tvdb", "82001", WAIT, &by_tvdb, &error));
	g_assert_cmpuint(by_tvdb.titles->len, ==, 1);
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(by_tvdb.titles, 0), "tvdb"), ==, "82001");
	g_assert_cmpstr(axis_meta_title_get_id(g_ptr_array_index(by_tvdb.titles, 0), "tmdb"), ==, "7101");
	axis_meta_search_answer_clear(&by_tvdb);
	g_assert_true(axis_meta_find(fx->meta, "tmdb", AXIS_META_KIND_FILM, "tvdb", "82001", WAIT, &none, &error));
	g_assert_cmpuint(none.titles->len, ==, 0);
	axis_meta_search_answer_clear(&none);
	g_assert_true(axis_meta_find(fx->meta, "tmdb", AXIS_META_KIND_FILM, "tmdb", "6003", WAIT, &none, &error));
	g_assert_true(none.saved);
	g_assert_nonnull(strstr(none.problem, "cannot look a title up"));
	axis_meta_search_answer_clear(&none);

	transcript = axis_test_plex_transcript(fx->api);
	g_assert_nonnull(strstr(transcript, "GET /3/find/tt0600003?external_source=imdb_id&language=en-US\n"));
	g_assert_nonnull(strstr(transcript, "GET /3/find/82001?external_source=tvdb_id&language=en-US\n"));
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/3/find/82001"), ==, 1);
	assert_api_requests_plain(fx->api);
}

/* Spec: Not in a URL; Images. */
static void
test_token_goes_to_the_data_address_only(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	AxisMetaTitleAnswer title;
	AxisMetaSeasonAnswer season;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash_small = NULL;
	g_autofree gchar *hash_poster = NULL;
	g_autofree gchar *hash_back = NULL;
	g_autofree gchar *images = NULL;
	g_autofree gchar *auth = NULL;
	g_autoptr(GPtrArray) image_requests = NULL;
	guint i;

	search_ok(fx, AXIS_META_KIND_FILM, "mazework", 0, &search);
	axis_meta_search_answer_clear(&search);
	lookup_ok(fx, AXIS_META_KIND_SERIES, "7101", &title);
	g_assert_true(axis_meta_get_season(fx->meta, title.title->id, 1, WAIT, &season, &error));
	axis_meta_season_answer_clear(&season);
	axis_meta_title_answer_clear(&title);
	g_assert_true(axis_meta_find(fx->meta, "tmdb", AXIS_META_KIND_FILM, "imdb", "tt0600003", WAIT, &search, &error));
	axis_meta_search_answer_clear(&search);

	g_assert_true(axis_meta_artwork_fetch(fx->meta, "tmdb", "/madeUpPoster000000000006003.jpg",
		AXIS_META_ART_POSTER_SMALL, &hash_small, &error));
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "tmdb", "/madeUpPoster000000000006003.jpg", AXIS_META_ART_POSTER,
		&hash_poster, &error));
	g_assert_true(axis_meta_artwork_fetch(fx->meta, "tmdb", "/madeUpBackdrop0000000006003.jpg", AXIS_META_ART_BACKDROP,
		&hash_back, &error));
	g_assert_no_error(error);

	/* The data address got a bearer header on every request. */
	assert_api_requests_plain(fx->api);
	auth = axis_test_plex_last_header(fx->api, "/3/search/movie", "Authorization");
	g_assert_cmpstr(auth, ==, "Bearer " TOKEN);

	/* The image address got three requests, at the three sizes, and no
	 * credential of any kind. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->images), ==, 3);
	g_assert_cmpuint(axis_test_plex_count(fx->images, "/t/p/w342/madeUpPoster000000000006003.jpg"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->images, "/t/p/w780/madeUpPoster000000000006003.jpg"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->images, "/t/p/w1280/madeUpBackdrop0000000006003.jpg"), ==, 1);
	images = axis_test_plex_transcript(fx->images);
	assert_token_absent(images);
	g_assert_null(strstr(images, "Authorization"));
	g_assert_null(strstr(images, "Bearer"));
	image_requests = axis_test_plex_requests(fx->images);
	for (i = 0; i < image_requests->len; i++)
		g_assert_true(g_str_has_prefix(g_ptr_array_index(image_requests, i), "GET /t/p/w"));
	g_assert_false(axis_test_plex_saw_non_get(fx->images));

	/* Nowhere else: no log line, no stored row. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" TOKEN "*"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_search s WHERE to_jsonb(s)::text LIKE '%" TOKEN "%'"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_artwork a WHERE to_jsonb(a)::text LIKE '%" TOKEN "%'"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM job WHERE to_jsonb(job)::text LIKE '%" TOKEN "%'"), ==, 0);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM job_schedule WHERE to_jsonb(job_schedule)::text LIKE '%"
		TOKEN "%'"), ==, 0);
}

/* Spec: an image path that does not match the pattern is not fetched. */
static void
test_odd_image_paths_are_not_fetched(Fixture *fx, gconstpointer data)
{
	static const gchar *const refs[] = {
		"/x.gif", "/a/b.jpg", "/ab_cd.jpg", "/ab-cd.jpg", "/abc.JPG", "/.jpg", "/abc.jpg.exe", "//evil.example/x.jpg",
		"https://evil.example/x.jpg", "/abc", "/../x.jpg", NULL
	};
	AxisMetaSearchAnswer search;
	const AxisMetaTitle *revolutions;
	guint i;

	for (i = 0; refs[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_meta_artwork_fetch(fx->meta, "tmdb", refs[i], AXIS_META_ART_POSTER, NULL, &error));
		g_assert_nonnull(error);
	}
	g_assert_cmpuint(axis_test_plex_n_requests(fx->images), ==, 0);

	/* A response that names a full address (and its own image base) gives
	 * the title no poster, and nothing is requested from that address. */
	search_ok(fx, AXIS_META_KIND_FILM, "mazework", 0, &search);
	revolutions = g_ptr_array_index(search.titles, 2);
	g_assert_cmpstr(revolutions->provider_id, ==, "6018");
	g_assert_null(revolutions->poster_ref);
	axis_meta_search_answer_clear(&search);
	g_assert_null(axis_meta_artwork_find(fx->meta, "tmdb", "https://evil.example/t/p/original/x.jpg",
		AXIS_META_ART_POSTER_SMALL, TRUE));
	g_assert_cmpuint(axis_test_plex_n_requests(fx->images), ==, 0);
}

/* Spec: Unreadable fields, and an answer that is not usable changes
 * nothing. */
static void
test_broken_answers(Fixture *fx, gconstpointer data)
{
	AxisMetaTitleAnswer title;
	AxisMetaSearchAnswer search;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route_file(fx->api, "/3/movie/6003", -1, DIR "movie-broken.json");
	lookup_ok(fx, AXIS_META_KIND_FILM, "6003", &title);
	g_assert_false(title.saved);
	g_assert_nonnull(title.title);
	g_assert_cmpstr(title.title->title, ==, "The Mazework");
	g_assert_cmpint(title.title->status, ==, AXIS_META_STATUS_UNKNOWN);
	g_assert_null(title.title->poster_ref);
	g_assert_null(title.title->release_date);
	axis_meta_title_answer_clear(&title);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title WHERE status IS NULL AND poster_ref IS NULL"), ==, 1);

	axis_test_plex_route_file(fx->api, "/3/search/movie", -1, DIR "not-an-object.json");
	g_assert_true(axis_meta_search(fx->meta, "tmdb", AXIS_META_KIND_FILM, "anything", 0, WAIT, &search, &error));
	g_assert_true(search.saved);
	g_assert_nonnull(strstr(search.problem, "cannot be read"));
	g_assert_true(search.titles == NULL || search.titles->len == 0);
	axis_meta_search_answer_clear(&search);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);

	axis_test_plex_route_file(fx->api, "/3/search/movie", -1, DIR "search-no-list.json");
	g_assert_true(axis_meta_search(fx->meta, "tmdb", AXIS_META_KIND_FILM, "anything else", 0, WAIT, &search, &error));
	g_assert_true(search.saved);
	axis_meta_search_answer_clear(&search);
	g_assert_cmpint(query_int(fx, "SELECT count(*) FROM meta_title"), ==, 1);
}

/* Spec: a title TMDb does not have; a refused token; no token; an error that
 * echoes the token; a redirect to another address. */
static void
test_failures_keep_the_token_private(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *echo = g_strdup_printf("{\"status_message\": \"Invalid API key: %s\", \"success\": false}", TOKEN);
	g_autofree gchar *elsewhere = g_strconcat(fx->image_origin, "/3/movie/6003", NULL);
	AxisMetaTitleAnswer title;
	AxisMetaSearchAnswer search;
	g_autoptr(GError) error = NULL;

	/* No such title: TMDb's 404 (unrouted here). */
	g_assert_true(axis_meta_lookup(fx->meta, "tmdb", AXIS_META_KIND_FILM, "999999", WAIT, &title, &error));
	g_assert_true(title.not_found);
	g_assert_null(title.title);
	axis_meta_title_answer_clear(&title);

	/* A refused token. */
	axis_test_plex_route(fx->api, "/3/search/tv", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	search_ok(fx, AXIS_META_KIND_SERIES, "street", 0, &search);
	g_assert_true(search.saved);
	assert_token_absent(search.problem);
	axis_meta_search_answer_clear(&search);
	/* The next question gets an answer (a failure is not yet a back-off). */
	search_ok(fx, AXIS_META_KIND_FILM, "mazework", 0, &search);
	g_assert_false(search.saved);
	axis_meta_search_answer_clear(&search);

	/* An error that echoes it. */
	axis_test_plex_route(fx->api, "/3/search/movie", -1, AXIS_TEST_PLEX_NORMAL, 500, echo, 0);
	search_ok(fx, AXIS_META_KIND_FILM, "echo", 0, &search);
	g_assert_true(search.saved);
	assert_token_absent(search.problem);
	axis_meta_search_answer_clear(&search);
	axis_test_plex_route_file(fx->api, "/3/search/movie", -1, DIR "search-movie.json");

	/* A redirect to the image address is refused, and nothing is sent. */
	axis_test_plex_clear_routes(fx->api);
	axis_test_plex_route(fx->api, "/3/movie/6003", -1, AXIS_TEST_PLEX_REDIRECT, 302, elsewhere, 0);
	g_assert_true(axis_meta_lookup(fx->meta, "tmdb", AXIS_META_KIND_FILM, "6003", WAIT, &title, &error));
	g_assert_true(title.saved);
	assert_token_absent(title.problem);
	axis_meta_title_answer_clear(&title);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->images), ==, 0);

	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" TOKEN "*"), ==, 0);
}

static void
test_no_token_no_request(Fixture *fx, gconstpointer data)
{
	AxisMetaSearchAnswer search;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TMDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	search_ok(fx, AXIS_META_KIND_FILM, "mazework", 0, &search);
	g_assert_true(search.saved);
	g_assert_nonnull(strstr(search.problem, "no key is set"));
	axis_meta_search_answer_clear(&search);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 0);
}

/* Spec: 429 with Retry-After pauses the provider (the layer does it). */
static void
test_told_to_wait(Fixture *fx, gconstpointer data)
{
	const gchar *const wait[] = { "Retry-After", "2", NULL };
	AxisMetaSearchAnswer answer;
	AxisMetaTitleAnswer title;
	AxisMetaProviderStatus status;
	g_autoptr(GError) error = NULL;

	axis_test_plex_route_full(fx->api, "/3/search/movie", 429, "application/json", NULL, wait);
	search_ok(fx, AXIS_META_KIND_FILM, "one", 0, &answer);
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "wait"));
	assert_token_absent(answer.problem);
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/3/search/movie"), ==, 1);
	axis_meta_provider_status(fx->meta, 0, &status);
	g_assert_cmpint(status.limit.paused_until_us, >, g_get_real_time());
	axis_meta_provider_status_clear(&status);

	/* Not asked again until the stated time, series searches and titles
	 * too, since it is the provider that was asked to wait: a caller that
	 * will not wait that long is answered at once from what is saved. */
	axis_test_plex_route_file(fx->api, "/3/search/movie", -1, DIR "search-movie.json");
	g_assert_true(axis_meta_search(fx->meta, "tmdb", AXIS_META_KIND_FILM, "two", 0, 500, &answer, &error));
	g_assert_true(answer.saved);
	g_assert_nonnull(strstr(answer.problem, "asked Axis to wait"));
	axis_meta_search_answer_clear(&answer);
	g_assert_cmpuint(axis_test_plex_count(fx->api, "/3/search/movie"), ==, 1);
	g_assert_true(axis_meta_lookup(fx->meta, "tmdb", AXIS_META_KIND_FILM, "6003", 500, &title, &error));
	g_assert_true(title.saved);
	axis_meta_title_answer_clear(&title);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->api), ==, 1);

	/* A caller that may wait is answered when the pause ends. */
	search_ok(fx, AXIS_META_KIND_FILM, "three", 0, &answer);
	g_assert_false(answer.saved);
	g_assert_cmpuint(answer.titles->len, ==, 4);
	axis_meta_search_answer_clear(&answer);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/meta/tmdb/parse/search-film", test_parse_search_film);
	g_test_add_func("/meta/tmdb/parse/search-series", test_parse_search_series);
	g_test_add_func("/meta/tmdb/parse/film-details", test_parse_film_details);
	g_test_add_func("/meta/tmdb/parse/film-regional-dates", test_parse_film_regional_dates);
	g_test_add_func("/meta/tmdb/parse/series-details", test_parse_series_details);
	g_test_add_func("/meta/tmdb/parse/wrong-typed-fields", test_parse_wrong_typed_fields);
	g_test_add_func("/meta/tmdb/parse/status", test_parse_status);
	g_test_add_func("/meta/tmdb/parse/seasons", test_parse_seasons);
	g_test_add_func("/meta/tmdb/parse/find", test_parse_find);
	g_test_add_func("/meta/tmdb/parse/refused", test_parse_refused);
	g_test_add_func("/meta/tmdb/parse/huge-lists", test_parse_huge_lists);
	g_test_add_func("/meta/tmdb/parse/cleans-text", test_parse_cleans_text);
	g_test_add_func("/meta/tmdb/parse/regional-year", test_parse_regional_year);
	g_test_add_func("/meta/tmdb/forms/image-paths", test_image_paths);
	g_test_add_func("/meta/tmdb/forms/key", test_key_forms);
	g_test_add_func("/meta/tmdb/forms/language-and-region", test_language_and_region_forms);
	g_test_add_func("/meta/tmdb/declaration", test_provider_declaration);

	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/search/films", Fixture, NULL, test_search_films);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/search/first-release-year", Fixture, NULL, test_search_first_release_year);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/search/series", Fixture, NULL, test_search_series);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/search/settings", Fixture, NULL, test_search_settings);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/settings/refuse", Fixture, NULL, test_settings_refuse);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/film/details", Fixture, NULL, test_film_details);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/film/region-setting", Fixture, NULL, test_film_region_setting);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/series/seasons", Fixture, NULL, test_series_and_seasons);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/one-record", Fixture, NULL, test_one_record_across_lookups);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/token/data-address-only", Fixture, NULL, test_token_goes_to_the_data_address_only);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/images/odd-paths", Fixture, NULL, test_odd_image_paths_are_not_fetched);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/answers/broken", Fixture, NULL, test_broken_answers);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/token/failures", Fixture, NULL, test_failures_keep_the_token_private);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/token/none", Fixture, NULL, test_no_token_no_request);
	AXIS_TEST_ADD_DB_CASE("/meta/tmdb/limit/told-to-wait", Fixture, NULL, test_told_to_wait);
	return g_test_run();
}
