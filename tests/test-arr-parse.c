/*
 * test-arr-parse.c - reading Radarr's answers (arr/axis-arr-parse.h) from the
 * hand-written answers of tests/fixtures/arr, the address rule of a
 * connection, and the mapping of the app's paths to library folders
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (bring-over): An incomplete title; Another mount; Thousands
 * of movies (the parser's part); (design.md section 12) the addresses
 * allowed.  Tasks 1.4, 1.5 (the address rule), 2.3 (the text of the folder
 * mapping).  No database.
 */

#include <glib.h>
#include <string.h>

#include "arr/axis-arr-assumed.h"
#include "arr/axis-arr-parse.h"
#include "download/axis-download-path.h"

#define DIR "tests/fixtures/arr/"

static gchar *
fixture(
	const gchar *name,
	gsize       *length
){
	g_autofree gchar *path = g_strconcat(DIR, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, length, &error));
	g_assert_no_error(error);
	return text;
}

/* ── system/status ──────────────────────────────────────────────── */

static void
test_status(void)
{
	gsize length = 0;
	g_autofree gchar *text = fixture("radarr-status.json", &length);
	g_autofree gchar *version = NULL;
	g_autofree gchar *none = NULL;
	static const gchar sonarr[] = "{\"appName\":\"Sonarr\",\"version\":\"4.0.1\"}";
	static const gchar odd[] = "{\"appName\":\"Radarr\",\"version\":\"5.1 <b>bold</b>\\u0000 and much more text than fits\"}";
	static const gchar array[] = "[{\"appName\":\"Radarr\"}]";
	static const gchar number[] = "{\"appName\":7}";

	g_assert_true(axis_arr_parse_status(text, length, AXIS_ARR_RADARR, &version));
	g_assert_cmpstr(version, ==, "5.2.6.8376");
	/* Another app's answer is not this app's, and gives no version. */
	g_assert_false(axis_arr_parse_status(text, length, AXIS_ARR_SONARR, &none));
	g_assert_null(none);
	g_assert_false(axis_arr_parse_status(sonarr, strlen(sonarr), AXIS_ARR_RADARR, &none));
	g_assert_true(axis_arr_parse_status(sonarr, strlen(sonarr), AXIS_ARR_SONARR, NULL));
	/* Not that shape. */
	g_assert_false(axis_arr_parse_status(array, strlen(array), AXIS_ARR_RADARR, &none));
	g_assert_false(axis_arr_parse_status(number, strlen(number), AXIS_ARR_RADARR, &none));
	g_assert_false(axis_arr_parse_status("<html>Welcome</html>", 20, AXIS_ARR_RADARR, &none));
	g_assert_false(axis_arr_parse_status("", 0, AXIS_ARR_RADARR, &none));
	g_assert_false(axis_arr_parse_status(NULL, 0, AXIS_ARR_RADARR, &none));
	g_assert_null(none);
	/* A version is digits, letters, '.' and '-', and short. */
	g_free(version);
	version = NULL;
	g_assert_true(axis_arr_parse_status(odd, strlen(odd), AXIS_ARR_RADARR, &version));
	g_assert_cmpuint(strlen(version), <=, AXIS_ARR_VERSION_MAX_CHARS);
	g_assert_null(strpbrk(version, "<> /\\"));
	g_assert_true(g_str_has_prefix(version, "5.1bbold"));
}

/* ── The list of movies ─────────────────────────────────────────── */

/* Spec: An incomplete title; the odd entries of the fixture. */
static void
test_movies(void)
{
	gsize length = 0;
	g_autofree gchar *text = fixture("radarr-movies.json", &length);
	g_autoptr(GPtrArray) movies = NULL;
	const gchar *problem = "unset";
	const AxisArrMovie *movie;

	g_assert_true(axis_arr_parse_movies(text, length, AXIS_ARR_MAX_TITLES, &movies, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(movies->len, ==, 12);

	/* 1: everything Axis keeps, and nothing else. */
	movie = g_ptr_array_index(movies, 0);
	g_assert_true(movie->usable);
	g_assert_cmpint(movie->arr_id, ==, 1);
	g_assert_cmpstr(movie->title, ==, "Paper Lanterns");
	g_assert_cmpint(movie->year, ==, 2011);
	g_assert_cmpint(movie->tmdb_id, ==, 6001);
	g_assert_cmpstr(movie->imdb_id, ==, "tt0000601");
	g_assert_true(movie->monitored);
	g_assert_cmpstr(movie->min_availability, ==, "released");
	g_assert_cmpint(movie->profile_id, ==, 4);
	g_assert_cmpstr(movie->path, ==, "/movies/Paper Lanterns (2011)");
	g_assert_cmpstr(movie->root_path, ==, "/movies");
	g_assert_true(movie->has_file);
	g_assert_cmpstr(movie->file_path, ==, "/movies/Paper Lanterns (2011)/Paper Lanterns (2011).mkv");
	g_assert_cmpint(movie->file_size, ==, 1500000000);
	g_assert_cmpstr(movie->file_quality, ==, "Bluray-1080p");
	g_assert_cmpstr(movie->file_scene, ==, "Paper.Lanterns.2011.1080p.BluRay.x264-MADEUP");

	/* 2: not monitored, no file; A7's word for its availability. */
	movie = g_ptr_array_index(movies, 1);
	g_assert_true(movie->usable);
	g_assert_false(movie->monitored);
	g_assert_false(movie->has_file);
	g_assert_null(movie->file_path);
	g_assert_cmpint(movie->file_size, ==, -1);
	g_assert_cmpstr(movie->min_availability, ==, "in_cinemas");

	/* 3: no TMDb identifier is a movie that can be read, without one. */
	movie = g_ptr_array_index(movies, 2);
	g_assert_true(movie->usable);
	g_assert_cmpint(movie->tmdb_id, ==, 0);
	g_assert_cmpstr(movie->min_availability, ==, "announced");

	/* 4: no id.  5: an identifier of another type.  Both incomplete, with
	 * what could be read for the list. */
	movie = g_ptr_array_index(movies, 3);
	g_assert_false(movie->usable);
	g_assert_cmpstr(movie->title, ==, "Glass Harbour");
	g_assert_cmpint(movie->arr_id, ==, 0);
	movie = g_ptr_array_index(movies, 4);
	g_assert_false(movie->usable);
	g_assert_cmpstr(movie->title, ==, "Nine Winters");
	g_assert_cmpint(movie->tmdb_id, ==, 0);

	/* 6 and 7: not a record at all. */
	movie = g_ptr_array_index(movies, 5);
	g_assert_false(movie->usable);
	g_assert_null(movie->title);
	movie = g_ptr_array_index(movies, 6);
	g_assert_false(movie->usable);
	g_assert_null(movie->title);

	/* 8: the text cleaned, the numbers checked, the words known or dropped,
	 * a file said and not described. */
	movie = g_ptr_array_index(movies, 7);
	g_assert_true(movie->usable);
	g_assert_cmpstr(movie->title, ==, "Ash and Ember");
	g_assert_cmpint(movie->year, ==, 0);
	g_assert_null(movie->min_availability);
	g_assert_null(movie->imdb_id);
	g_assert_true(movie->has_file);
	g_assert_null(movie->file_path);
	g_assert_cmpint(movie->file_size, ==, -1);
	g_assert_null(movie->file_quality);

	/* 9 and 10: paths are kept byte for byte; mapping them is refused later. */
	movie = g_ptr_array_index(movies, 8);
	g_assert_true(movie->usable);
	g_assert_cmpstr(movie->path, ==, "/movies/../etc/Tin Sparrow (2001)");
	movie = g_ptr_array_index(movies, 9);
	g_assert_true(movie->usable);
	g_assert_cmpstr(movie->path, ==, "C:\\Movies\\Low Tide Radio (2015)");
	g_assert_cmpstr(movie->min_availability, ==, "released");
	g_assert_cmpstr(movie->file_quality, ==, "Some-Custom-Quality");

	/* 11: an empty title.  12: a flag that is not one. */
	movie = g_ptr_array_index(movies, 10);
	g_assert_false(movie->usable);
	movie = g_ptr_array_index(movies, 11);
	g_assert_false(movie->usable);
	g_assert_cmpstr(movie->title, ==, "Hollow Compass");
}

static void
assert_refused(
	const gchar *text,
	const gchar *expected
){
	GPtrArray *movies = (GPtrArray *)0x1;
	const gchar *problem = NULL;

	g_assert_false(axis_arr_parse_movies(text, text != NULL ? strlen(text) : 0, AXIS_ARR_MAX_TITLES, &movies, &problem));
	g_assert_null(movies);
	g_assert_cmpstr(problem, ==, expected);
}

static void
test_movies_refused(void)
{
	g_autoptr(GPtrArray) movies = NULL;
	const gchar *problem = NULL;
	static const gchar many[] = "[1,2,3,4]";
	static const gchar tricky[] = " [ {\"title\":\"a ] , [ \\\" } b\",\"id\":1} , [1,[2,3]] ,\n{} ] \n";
	static const gchar empty_slots[] = "[,]";

	assert_refused(NULL, "is empty");
	assert_refused("", "is not a list");
	assert_refused("{\"movies\":[]}", "is not a list");
	assert_refused("<html>", "is not a list");
	assert_refused("[{\"id\":1}", "is cut short");
	assert_refused("[\"never closed", "is cut short");
	assert_refused("[1]}", "is not a list");
	assert_refused("[1] 2", "is not a list");
	assert_refused("[}", "is not a list");

	/* More than the limit fails as a whole. */
	g_assert_false(axis_arr_parse_movies(many, strlen(many), 3, &movies, &problem));
	g_assert_null(movies);
	g_assert_cmpstr(problem, ==, "holds more titles than Axis reads");
	g_assert_true(axis_arr_parse_movies(many, strlen(many), 4, &movies, &problem));
	g_assert_cmpuint(movies->len, ==, 4);
	g_clear_pointer(&movies, g_ptr_array_unref);

	/* An empty list is a list. */
	g_assert_true(axis_arr_parse_movies(" [ ] ", 5, 10, &movies, &problem));
	g_assert_cmpuint(movies->len, ==, 0);
	g_clear_pointer(&movies, g_ptr_array_unref);

	/* Brackets, commas and quotes inside a string do not cut an element. */
	g_assert_true(axis_arr_parse_movies(tricky, strlen(tricky), 10, &movies, &problem));
	g_assert_cmpuint(movies->len, ==, 3);
	g_assert_cmpstr(((const AxisArrMovie *)g_ptr_array_index(movies, 0))->title, ==, "a ] , [ \" } b");
	g_assert_cmpint(((const AxisArrMovie *)g_ptr_array_index(movies, 0))->arr_id, ==, 1);
	g_clear_pointer(&movies, g_ptr_array_unref);

	/* An element that is nothing is an incomplete one, not a crash. */
	g_assert_true(axis_arr_parse_movies(empty_slots, strlen(empty_slots), 10, &movies, &problem));
	g_assert_cmpuint(movies->len, ==, 2);
	g_assert_false(((const AxisArrMovie *)g_ptr_array_index(movies, 0))->usable);
}

/* Spec: Thousands of movies: far more elements than the JSON reader's own
 * limit of values holds are read, because each is read alone; and one
 * element beyond its size limit is incomplete alone. */
static void
test_movies_many_and_large(void)
{
	g_autoptr(GString) text = g_string_new("[");
	g_autoptr(GPtrArray) movies = NULL;
	const gchar *problem = NULL;
	guint n = 8000;
	guint i;

	for (i = 0; i < n; i++) {
		g_string_append_printf(text, "%s{\"id\":%u,\"title\":\"Made Up Film %u\",\"year\":2001,\"tmdbId\":%u,"
			"\"monitored\":true,\"qualityProfileId\":4,\"path\":\"/movies/Made Up Film %u (2001)\","
			"\"rootFolderPath\":\"/movies\",\"hasFile\":false,\"images\":[{\"a\":1},{\"a\":2},{\"a\":3}],"
			"\"ratings\":{\"x\":{\"votes\":1,\"value\":2},\"y\":{\"votes\":1,\"value\":2}},"
			"\"alternateTitles\":[{\"t\":\"a\"},{\"t\":\"b\"},{\"t\":\"c\"},{\"t\":\"d\"},{\"t\":\"e\"},{\"t\":\"f\"}],"
			"\"genres\":[\"a\",\"b\",\"c\"],\"tags\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20]}",
			i > 0 ? "," : "", i + 1, i + 1, i + 1, i + 1);
	}
	/* One record far larger than a movie's is. */
	g_string_append(text, ",{\"id\":9000,\"title\":\"Too Large\",\"filler\":\"");
	for (i = 0; i < AXIS_ARR_MAX_ELEMENT_BYTES / 64 + 1; i++)
		g_string_append(text, "0123456789012345678901234567890123456789012345678901234567890123");
	g_string_append(text, "\"},{\"id\":9001,\"title\":\"After It\",\"tmdbId\":77,\"monitored\":false,"
		"\"qualityProfileId\":1,\"path\":\"/movies/After It\"}]");

	g_assert_true(axis_arr_parse_movies(text->str, text->len, AXIS_ARR_MAX_TITLES, &movies, &problem));
	g_assert_cmpuint(movies->len, ==, n + 2);
	for (i = 0; i < n; i++)
		g_assert_true(((const AxisArrMovie *)g_ptr_array_index(movies, i))->usable);
	g_assert_cmpstr(((const AxisArrMovie *)g_ptr_array_index(movies, n - 1))->title, ==, "Made Up Film 8000");
	g_assert_false(((const AxisArrMovie *)g_ptr_array_index(movies, n))->usable);
	g_assert_null(((const AxisArrMovie *)g_ptr_array_index(movies, n))->title);
	g_assert_true(((const AxisArrMovie *)g_ptr_array_index(movies, n + 1))->usable);
	g_assert_cmpstr(((const AxisArrMovie *)g_ptr_array_index(movies, n + 1))->title, ==, "After It");
}

/* ── The short lists ────────────────────────────────────────────── */

static void
test_profiles_folders_queue(void)
{
	gsize length = 0;
	g_autofree gchar *profiles_text = fixture("radarr-qualityprofile.json", &length);
	g_autoptr(GPtrArray) profiles = NULL;
	g_autoptr(GPtrArray) folders = NULL;
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *folders_text = NULL;
	g_autofree gchar *queue_text = NULL;
	const gchar *problem = NULL;
	guint total = 99;
	guint n = 99;

	g_assert_true(axis_arr_parse_profiles(profiles_text, length, &profiles, &problem));
	g_assert_cmpuint(profiles->len, ==, 2);
	g_assert_cmpint(((const AxisArrProfile *)g_ptr_array_index(profiles, 0))->id, ==, 4);
	g_assert_cmpstr(((const AxisArrProfile *)g_ptr_array_index(profiles, 0))->name, ==, "HD-1080p");
	g_assert_cmpstr(((const AxisArrProfile *)g_ptr_array_index(profiles, 1))->name, ==, "Ultra-HD");
	g_clear_pointer(&profiles, g_ptr_array_unref);
	g_assert_false(axis_arr_parse_profiles("{}", 2, &profiles, &problem));
	g_assert_null(profiles);
	g_assert_cmpstr(problem, ==, "is not a list");

	folders_text = fixture("radarr-rootfolder.json", &length);
	g_assert_true(axis_arr_parse_folders(folders_text, length, &folders, &problem));
	/* The relative one, the one with "..", and the one without a path are
	 * left out; a trailing slash is not part of a path. */
	g_assert_cmpuint(folders->len, ==, 2);
	g_assert_cmpstr(((const AxisArrFolder *)g_ptr_array_index(folders, 0))->path, ==, "/movies");
	g_assert_true(((const AxisArrFolder *)g_ptr_array_index(folders, 0))->accessible);
	g_assert_cmpstr(((const AxisArrFolder *)g_ptr_array_index(folders, 1))->path, ==, "/movies-4k");
	g_assert_false(((const AxisArrFolder *)g_ptr_array_index(folders, 1))->accessible);

	queue_text = fixture("radarr-queue.json", &length);
	g_assert_true(axis_arr_parse_queue_page(queue_text, length, AXIS_ARR_RADARR, ids, &total, &n, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(total, ==, 3);
	g_assert_cmpuint(n, ==, 3);
	g_assert_cmpuint(ids->len, ==, 1);
	g_assert_cmpint(g_array_index(ids, gint64, 0), ==, 2);
	/* Not a page. */
	g_assert_false(axis_arr_parse_queue_page("[]", 2, AXIS_ARR_RADARR, ids, &total, &n, &problem));
	g_assert_nonnull(problem);
	g_assert_false(axis_arr_parse_queue_page("{\"records\":[]}", 14, AXIS_ARR_RADARR, ids, &total, &n, &problem));
	g_assert_false(axis_arr_parse_queue_page("{\"totalRecords\":-1,\"records\":[]}", 32, AXIS_ARR_RADARR, ids, &total,
		&n, &problem));
	g_assert_cmpuint(ids->len, ==, 1);
	/* Radarr's queue read as a Sonarr's names no series: a record without
	 * the member of the app asked for is passed over. */
	g_assert_true(axis_arr_parse_queue_page(queue_text, length, AXIS_ARR_SONARR, ids, &total, &n, &problem));
	g_assert_cmpuint(n, ==, 3);
	g_assert_cmpuint(ids->len, ==, 1);
}

/* ── Sonarr ─────────────────────────────────────────────────────── */

/* Spec: An incomplete title (a series); the odd entries of the fixture. */
static void
test_series(void)
{
	gsize length = 0;
	g_autofree gchar *text = fixture("sonarr-series.json", &length);
	g_autoptr(GPtrArray) list = NULL;
	const gchar *problem = "unset";
	const AxisArrSeries *series;
	guint i;

	g_assert_true(axis_arr_parse_series(text, length, AXIS_ARR_MAX_TITLES, &list, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(list->len, ==, 11);

	/* 1: everything Axis keeps: the specials off, season 1 on, season 2 off. */
	series = g_ptr_array_index(list, 0);
	g_assert_true(series->usable);
	g_assert_cmpint(series->arr_id, ==, 1);
	g_assert_cmpstr(series->title, ==, "Gullwing Ferry");
	g_assert_cmpint(series->year, ==, 2025);
	g_assert_cmpint(series->tvdb_id, ==, 81061);
	g_assert_cmpstr(series->imdb_id, ==, "tt0000861");
	g_assert_true(series->monitored);
	g_assert_cmpstr(series->series_type, ==, "standard");
	g_assert_cmpint(series->monitor_new, ==, 1);
	g_assert_cmpint(series->profile_id, ==, 4);
	g_assert_cmpstr(series->path, ==, "/tv/Gullwing Ferry");
	g_assert_cmpstr(series->root_path, ==, "/tv");
	g_assert_cmpuint(series->seasons->len, ==, 3);
	g_assert_cmpint(g_array_index(series->seasons, AxisArrSeason, 0).number, ==, 0);
	g_assert_false(g_array_index(series->seasons, AxisArrSeason, 0).monitored);
	g_assert_cmpint(g_array_index(series->seasons, AxisArrSeason, 1).number, ==, 1);
	g_assert_true(g_array_index(series->seasons, AxisArrSeason, 1).monitored);
	g_assert_false(g_array_index(series->seasons, AxisArrSeason, 2).monitored);

	/* 2: not monitored; a daily show; Sonarr says it monitors no new season. */
	series = g_ptr_array_index(list, 1);
	g_assert_true(series->usable);
	g_assert_false(series->monitored);
	g_assert_cmpstr(series->series_type, ==, "daily");
	g_assert_cmpint(series->monitor_new, ==, 0);

	/* 3: anime; what Sonarr 3 leaves out is not said (A13). */
	series = g_ptr_array_index(list, 2);
	g_assert_true(series->usable);
	g_assert_cmpstr(series->series_type, ==, "anime");
	g_assert_cmpint(series->monitor_new, ==, -1);

	/* 4: no TheTVDB identifier (0), 5: none (null): series that can be read,
	 * without one.  A type Axis does not know ("miniseries") is not said
	 * (NULL), not a standard series; none given at all is a standard one. */
	series = g_ptr_array_index(list, 3);
	g_assert_true(series->usable);
	g_assert_cmpint(series->tvdb_id, ==, 0);
	g_assert_null(series->series_type);
	series = g_ptr_array_index(list, 4);
	g_assert_true(series->usable);
	g_assert_cmpint(series->tvdb_id, ==, 0);
	g_assert_cmpstr(series->series_type, ==, "standard");

	/* 6: no id.  7: an identifier of another type.  8: no seasons.  9: a
	 * season out of shape.  10: a season named twice.  All incomplete, with
	 * what could be read for the list, and no season kept. */
	for (i = 5; i < 10; i++) {
		series = g_ptr_array_index(list, i);
		g_assert_false(series->usable);
		g_assert_nonnull(series->title);
		g_assert_cmpuint(series->seasons->len, ==, i < 7 ? 1 : 0);
	}
	g_assert_cmpint(((const AxisArrSeries *)g_ptr_array_index(list, 5))->arr_id, ==, 0);
	g_assert_cmpint(((const AxisArrSeries *)g_ptr_array_index(list, 6))->tvdb_id, ==, 0);

	/* 11: not a record at all. */
	series = g_ptr_array_index(list, 10);
	g_assert_false(series->usable);
	g_assert_null(series->title);

	g_clear_pointer(&list, g_ptr_array_unref);
	g_assert_false(axis_arr_parse_series("{\"series\":[]}", 13, 10, &list, &problem));
	g_assert_null(list);
	g_assert_cmpstr(problem, ==, "is not a list");
	g_assert_false(axis_arr_parse_series("[1,2,3]", 7, 2, &list, &problem));
	g_assert_cmpstr(problem, ==, "holds more titles than Axis reads");

	/* Sonarr's download queue: one record names a series by number; one names
	 * none and one names it as text, and both are passed over. */
	{
		g_autofree gchar *queue_text = fixture("sonarr-queue.json", &length);
		g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		guint total = 0;
		guint n = 0;

		g_assert_true(axis_arr_parse_queue_page(queue_text, length, AXIS_ARR_SONARR, ids, &total, &n, &problem));
		g_assert_cmpuint(total, ==, 3);
		g_assert_cmpuint(n, ==, 3);
		g_assert_cmpuint(ids->len, ==, 1);
		g_assert_cmpint(g_array_index(ids, gint64, 0), ==, 2);
		/* Read as a Radarr's it names no movie. */
		g_array_set_size(ids, 0);
		g_assert_true(axis_arr_parse_queue_page(queue_text, length, AXIS_ARR_RADARR, ids, &total, &n, &problem));
		g_assert_cmpuint(ids->len, ==, 0);
	}
}

/* A series' episodes and its files; what is odd in them is left out and
 * counted, and the rest is read. */
static void
test_episodes_and_files(void)
{
	gsize length = 0;
	g_autofree gchar *episodes_text = fixture("sonarr-episode.json", &length);
	g_autoptr(GArray) episodes = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autofree gchar *files_text = NULL;
	g_autoptr(GString) many = g_string_new("[");
	const gchar *problem = "unset";
	const AxisArrEpisode *episode;
	const AxisArrEpisodeFile *file;
	guint odd = 99;
	guint i;

	g_assert_true(axis_arr_parse_episodes(episodes_text, length, AXIS_ARR_MAX_SERIES_EPISODES, &episodes, &odd,
		&problem));
	g_assert_null(problem);
	/* Nine entries: six read, three left out (no season, a number of another
	 * type, not a record). */
	g_assert_cmpuint(episodes->len, ==, 6);
	g_assert_cmpuint(odd, ==, 3);
	episode = &g_array_index(episodes, AxisArrEpisode, 0);
	g_assert_cmpint(episode->arr_id, ==, 101);
	g_assert_cmpint(episode->tvdb_id, ==, 81061001);
	g_assert_cmpint(episode->season, ==, 1);
	g_assert_cmpint(episode->number, ==, 1);
	g_assert_cmpstr(episode->air_date, ==, "2025-01-06");
	g_assert_true(episode->monitored);
	g_assert_cmpint(episode->file_id, ==, 501);
	/* A double episode: the same file on two episodes. */
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 1).file_id, ==, 502);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 2).file_id, ==, 502);
	/* Not monitored, no file. */
	episode = &g_array_index(episodes, AxisArrEpisode, 3);
	g_assert_false(episode->monitored);
	g_assert_cmpint(episode->file_id, ==, 0);
	/* A special, without a day and without TheTVDB's id. */
	episode = &g_array_index(episodes, AxisArrEpisode, 4);
	g_assert_cmpint(episode->season, ==, 0);
	g_assert_cmpstr(episode->air_date, ==, "");
	g_assert_cmpint(episode->tvdb_id, ==, 0);
	/* A day that is none is no day; the episode is kept. */
	episode = &g_array_index(episodes, AxisArrEpisode, 5);
	g_assert_cmpint(episode->number, ==, 5);
	g_assert_cmpstr(episode->air_date, ==, "");
	/* A20, Sonarr's own absolute number: a whole number from 1 is kept; 0,
	 * absent, a string, null and one out of range are none, and each of those
	 * entries is read all the same. */
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 0).absolute, ==, 1);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 1).absolute, ==, 0);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 2).absolute, ==, 0);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 3).absolute, ==, 0);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 3).number, ==, 4);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 4).absolute, ==, 0);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 5).absolute, ==, 0);
	{
		/* The largest kept, a fraction and a negative number. */
		static const gchar numbers[] = "[{\"seasonNumber\":9,\"episodeNumber\":1,\"monitored\":true,"
			"\"absoluteEpisodeNumber\":1000000},{\"seasonNumber\":9,\"episodeNumber\":2,\"monitored\":true,"
			"\"absoluteEpisodeNumber\":12.5},{\"seasonNumber\":9,\"episodeNumber\":3,\"monitored\":true,"
			"\"absoluteEpisodeNumber\":-3}]";
		g_autoptr(GArray) read = NULL;

		g_assert_true(axis_arr_parse_episodes(numbers, strlen(numbers), 10, &read, &odd, &problem));
		g_assert_cmpuint(read->len, ==, 3);
		g_assert_cmpuint(odd, ==, 0);
		g_assert_cmpint(g_array_index(read, AxisArrEpisode, 0).absolute, ==, 1000000);
		g_assert_cmpint(g_array_index(read, AxisArrEpisode, 1).absolute, ==, 0);
		g_assert_cmpint(g_array_index(read, AxisArrEpisode, 2).absolute, ==, 0);
		odd = 3;
	}

	files_text = fixture("sonarr-episodefile.json", &length);
	g_assert_true(axis_arr_parse_episode_files(files_text, length, AXIS_ARR_MAX_SERIES_EPISODES, &files, &odd, &problem));
	g_assert_cmpuint(files->len, ==, 3);
	g_assert_cmpuint(odd, ==, 2);
	file = g_ptr_array_index(files, 0);
	g_assert_cmpint(file->arr_id, ==, 501);
	g_assert_cmpstr(file->path, ==, "/tv/Gullwing Ferry/Season 1/Gullwing Ferry - S01E01 - Low Water.mkv");
	g_assert_cmpint(file->size, ==, 900000000);
	g_assert_cmpstr(file->quality, ==, "WEBDL-1080p");
	g_assert_cmpstr(file->scene, ==, "Gullwing.Ferry.S01E01.1080p.WEB-DL.H.264-MADEUP");
	/* A file Sonarr gives no path or size for is kept, without them. */
	file = g_ptr_array_index(files, 2);
	g_assert_cmpint(file->arr_id, ==, 503);
	g_assert_null(file->path);
	g_assert_cmpint(file->size, ==, -1);

	/* Not a list; and more than the limit fails as a whole. */
	g_clear_pointer(&episodes, g_array_unref);
	/* A place that can be read with a monitoring that is no yes or no: the
	 * place alone is kept, marked, and nothing else of the entry. */
	{
		static const gchar place[] = "[{\"id\":7,\"tvdbId\":8,\"seasonNumber\":1,\"episodeNumber\":3,"
			"\"monitored\":\"yes\",\"episodeFileId\":9}]";
		g_autoptr(GArray) kept = NULL;

		g_assert_true(axis_arr_parse_episodes(place, strlen(place), 10, &kept, &odd, &problem));
		g_assert_cmpuint(kept->len, ==, 1);
		g_assert_cmpuint(odd, ==, 0);
		episode = &g_array_index(kept, AxisArrEpisode, 0);
		g_assert_true(episode->odd);
		g_assert_false(episode->monitored);
		g_assert_cmpint(episode->season, ==, 1);
		g_assert_cmpint(episode->number, ==, 3);
		g_assert_cmpint(episode->arr_id + episode->tvdb_id + episode->file_id, ==, 0);
	}
	g_assert_false(axis_arr_parse_episodes("{}", 2, 10, &episodes, &odd, &problem));
	g_assert_null(episodes);
	g_assert_cmpstr(problem, ==, "is not a list");
	g_assert_false(axis_arr_parse_episodes("[1,2,3]", 7, 2, &episodes, &odd, &problem));
	g_assert_cmpstr(problem, ==, "holds more entries than Axis reads");
	g_clear_pointer(&files, g_ptr_array_unref);
	g_assert_false(axis_arr_parse_episode_files("<html>", 6, 10, &files, &odd, &problem));
	g_assert_null(files);

	/* A long series: far more values than the JSON reader holds in one
	 * document, read because each entry is read alone. */
	for (i = 0; i < 12000; i++)
		g_string_append_printf(many, "%s{\"id\":%u,\"seriesId\":7,\"tvdbId\":%u,\"seasonNumber\":%u,\"episodeNumber\":%u,"
			"\"airDate\":\"2020-02-29\",\"airDateUtc\":\"2020-02-29T20:00:00Z\",\"title\":\"Made Up Episode %u\","
			"\"overview\":\"Nothing happens, at length.\",\"monitored\":true,\"hasFile\":true,\"episodeFileId\":%u,"
			"\"images\":[{\"a\":1},{\"a\":2}],\"unverifiedSceneNumbering\":false,\"runtime\":24,\"grabbed\":false}",
			i > 0 ? "," : "", i + 1, 900000 + i, i / 200 + 1, i % 200 + 1, i + 1, 5000 + i);
	g_string_append_c(many, ']');
	g_assert_true(axis_arr_parse_episodes(many->str, many->len, AXIS_ARR_MAX_SERIES_EPISODES, &episodes, &odd,
		&problem));
	g_assert_cmpuint(episodes->len, ==, 12000);
	g_assert_cmpuint(odd, ==, 0);
	g_assert_cmpint(g_array_index(episodes, AxisArrEpisode, 11999).season, ==, 60);
	g_assert_cmpstr(g_array_index(episodes, AxisArrEpisode, 11999).air_date, ==, "2020-02-29");
}

/* A8, A7: the tables. */
static void
test_tables(void)
{
	g_assert_cmpstr(axis_arr_quality_to_axis("Bluray-1080p"), ==, "bluray_1080p");
	g_assert_cmpstr(axis_arr_quality_to_axis("WEBDL-2160p"), ==, "webdl_2160p");
	g_assert_cmpstr(axis_arr_quality_to_axis("Remux-2160p"), ==, "remux_2160p");
	g_assert_cmpstr(axis_arr_quality_to_axis("SDTV"), ==, "sdtv");
	g_assert_cmpstr(axis_arr_quality_to_axis("Bluray-576p"), ==, "bluray_480p");
	g_assert_null(axis_arr_quality_to_axis("bluray-1080p"));
	/* Every name a running Radarr 6.4.4 gave: in the table, but for the two
	 * that give no quality (the file is kept, its quality read from its name). */
	{
		static const gchar *const seen[] = { "BR-DISK", "Bluray-480p", "Bluray-576p", "Bluray-720p", "Bluray-1080p",
			"Bluray-2160p", "CAM", "DVD", "HDTV-1080p", "Remux-1080p", "Remux-2160p", "SDTV", "TELESYNC",
			"WEBDL-480p", "WEBDL-720p", "WEBDL-1080p", "WEBDL-2160p", "WEBRip-480p", "WEBRip-720p", "WEBRip-1080p",
			"WEBRip-2160p" };
		guint i;

		for (i = 0; i < G_N_ELEMENTS(seen); i++)
			g_assert_nonnull(axis_arr_quality_to_axis(seen[i]));
	}
	g_assert_null(axis_arr_quality_to_axis("DVD-R"));
	g_assert_null(axis_arr_quality_to_axis("Unknown"));
	g_assert_null(axis_arr_quality_to_axis("Some-Custom-Quality"));
	g_assert_null(axis_arr_quality_to_axis(NULL));
	g_assert_cmpstr(axis_arr_app_label(AXIS_ARR_RADARR), ==, AXIS_ARR_APP_NAME_RADARR);
	g_assert_cmpstr(axis_arr_app_to_string(AXIS_ARR_SONARR), ==, "sonarr");
}

/* ── The address ────────────────────────────────────────────────── */

static void
assert_address(
	const gchar *typed,
	const gchar *origin,
	const gchar *prefix
){
	g_autofree gchar *got_origin = NULL;
	g_autofree gchar *got_prefix = NULL;
	const gchar *problem = "unset";

	if (!axis_arr_address_split(typed, &got_origin, &got_prefix, &problem))
		g_error("'%s' was refused: %s", typed, problem);
	g_assert_null(problem);
	g_assert_cmpstr(got_origin, ==, origin);
	g_assert_cmpstr(got_prefix, ==, prefix);
}

static void
assert_address_refused(const gchar *typed)
{
	gchar *origin = (gchar *)0x1;
	gchar *prefix = (gchar *)0x1;
	const gchar *problem = NULL;

	if (axis_arr_address_split(typed, &origin, &prefix, &problem))
		g_error("'%s' was accepted", typed);
	g_assert_null(origin);
	g_assert_null(prefix);
	g_assert_nonnull(problem);
	/* The reason is fixed words: it never repeats what was typed. */
	if (typed != NULL && strlen(typed) >= 8)
		g_assert_null(strstr(problem, typed));
}

/* Spec: An address on the home network; design.md section 12. */
static void
test_address(void)
{
	assert_address("http://radarr:7878", "http://radarr:7878", "");
	assert_address("  http://radarr:7878/  ", "http://radarr:7878", "");
	assert_address("HTTPS://Media.Example/radarr/", "https://media.example", "/radarr");
	assert_address("http://media.example/apps/radarr", "http://media.example", "/apps/radarr");
	/* Private, loopback and link-local addresses are where these apps live. */
	assert_address("http://192.168.0.0:7878", "http://192.168.0.0:7878", "");
	assert_address("http://127.0.0.1:7878", "http://127.0.0.1:7878", "");
	assert_address("http://localhost:7878/radarr", "http://localhost:7878", "/radarr");
	assert_address("http://[::1]:7878", "http://[::1]:7878", "");
	assert_address("http://169.254.0.0:7878", "http://169.254.0.0:7878", "");

	assert_address_refused(NULL);
	assert_address_refused("");
	assert_address_refused("radarr:7878");
	assert_address_refused("ftp://radarr.example:7878");
	assert_address_refused("file:///etc/passwd");
	assert_address_refused("http://");
	assert_address_refused("http:///radarr");
	assert_address_refused("http://user:secret-word@radarr.example:7878");
	assert_address_refused("http://radarr.example:7878/?apikey=0123456789abcdef");
	assert_address_refused("http://radarr.example:7878/radarr?x=1");
	assert_address_refused("http://radarr.example:7878/radarr#top");
	assert_address_refused("http://radarr.example:7878/../admin");
	assert_address_refused("http://radarr.example:7878/a/./b");
	assert_address_refused("http://radarr.example:7878/a b");
	assert_address_refused("http://radarr.example:7878/a%2e%2e/b");
	assert_address_refused("http://radarr.example\\@other.example/");
	assert_address_refused("http://radarr .example:7878");
}

/* ── Paths ──────────────────────────────────────────────────────── */

static GPtrArray *
maps_of(void)
{
	GPtrArray *maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);

	g_ptr_array_add(maps, axis_download_path_map_new("/movies", 11, ""));
	g_ptr_array_add(maps, axis_download_path_map_new("/movies/kids", 12, "young"));
	g_ptr_array_add(maps, axis_download_path_map_new("/data/media/films", 13, ""));
	return maps;
}

static void
assert_mapped(
	GPtrArray   *maps,
	const gchar *path,
	gint64       root,
	const gchar *stored
){
	g_autofree gchar *got = NULL;
	gint64 got_root = 0;

	g_assert_cmpint(axis_arr_map_path(maps, path, &got_root, &got), ==, AXIS_ARR_PATH_MAPPED);
	g_assert_cmpint(got_root, ==, root);
	g_assert_cmpstr(got, ==, stored);
}

static void
assert_not_mapped(
	GPtrArray         *maps,
	const gchar       *path,
	AxisArrPathResult  expected
){
	gchar *got = (gchar *)0x1;
	gint64 root = 7;

	g_assert_cmpint(axis_arr_map_path(maps, path, &root, &got), ==, expected);
	g_assert_null(got);
	g_assert_cmpint(root, ==, 0);
}

/* Spec: Another mount; a path with ".." refused (task 2.5). */
static void
test_paths(void)
{
	g_autoptr(GPtrArray) maps = maps_of();
	g_autoptr(GPtrArray) none = g_ptr_array_new();
	g_autoptr(GString) long_path = g_string_new("/movies");
	guint i;

	assert_mapped(maps, "/movies/Paper Lanterns (2011)/Paper Lanterns (2011).mkv", 11,
		"Paper Lanterns (2011)/Paper Lanterns (2011).mkv");
	/* The longest prefix wins, by whole segments, and its path below the
	 * library folder comes first. */
	assert_mapped(maps, "/movies/kids/Tin Sparrow (2001)/Tin Sparrow (2001).mkv", 12,
		"young/Tin Sparrow (2001)/Tin Sparrow (2001).mkv");
	assert_mapped(maps, "/movies/kidsroom/a.mkv", 11, "kidsroom/a.mkv");
	assert_mapped(maps, "/data/media/films//Salt Meridian (2019)/a.mkv", 13, "Salt Meridian (2019)/a.mkv");
	/* A name is stored as the index stores it. */
	assert_mapped(maps, "/movies/Aur\xc3\xa9lie (2014)/Aur\xc3\xa9lie.mkv", 11, "Aur\xc3\xa9lie (2014)/Aur\xc3\xa9lie.mkv");
	assert_mapped(maps, "/movies/Back\\slash (2000)/a.mkv", 11, "Back\\\\slash (2000)/a.mkv");

	assert_not_mapped(maps, "/moviesfolder/a.mkv", AXIS_ARR_PATH_UNMAPPED);
	assert_not_mapped(maps, "/data/media/a.mkv", AXIS_ARR_PATH_UNMAPPED);
	assert_not_mapped(none, "/movies/a.mkv", AXIS_ARR_PATH_UNMAPPED);
	assert_not_mapped(NULL, "/movies/a.mkv", AXIS_ARR_PATH_UNMAPPED);
	/* Refused, never resolved. */
	assert_not_mapped(maps, "/movies/../etc/passwd", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, "/movies/./a.mkv", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, "movies/a.mkv", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, "C:\\Movies\\Low Tide Radio (2015)\\a.mkv", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, "/movies/a\nb.mkv", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, "/movies/a.mkv ", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, "", AXIS_ARR_PATH_INVALID);
	assert_not_mapped(maps, NULL, AXIS_ARR_PATH_INVALID);
	/* The folder itself is not a file's place. */
	assert_not_mapped(maps, "/movies", AXIS_ARR_PATH_INVALID);
	for (i = 0; i < 600; i++)
		g_string_append(long_path, "/segment");
	assert_not_mapped(maps, long_path->str, AXIS_ARR_PATH_INVALID);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/arr/parse/status", test_status);
	g_test_add_func("/arr/parse/movies", test_movies);
	g_test_add_func("/arr/parse/movies-refused", test_movies_refused);
	g_test_add_func("/arr/parse/movies-many-and-large", test_movies_many_and_large);
	g_test_add_func("/arr/parse/profiles-folders-queue", test_profiles_folders_queue);
	g_test_add_func("/arr/parse/series", test_series);
	g_test_add_func("/arr/parse/episodes-and-files", test_episodes_and_files);
	g_test_add_func("/arr/parse/tables", test_tables);
	g_test_add_func("/arr/address", test_address);
	g_test_add_func("/arr/paths", test_paths);
	return g_test_run();
}
