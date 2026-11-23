/*
 * fuzz-arr-parse.c - libFuzzer target for the reading of Radarr's and
 * Sonarr's answers (arr/axis-arr-parse.h), the address rule of a connection
 * and the mapping of the app's paths to library folders
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' a status answer, '1' the list of
 * movies, '2' quality profiles, '3' root folders, '4' a page of the download
 * queue (as either app's), '5' an address as typed, '6' a path to map, '7'
 * Sonarr's list of series, '8' a series' episodes, '9' a series' episode
 * files); the rest is the input.  Of Sonarr's: a series' seasons are each
 * once and in range, its type is one of three words, an episode's place is in
 * range and its day is a day or none, a file has its number.
 * Whatever it is, the parser returns without crashing or allocating without
 * bound, and what it returns is within the limits the store's constraints
 * state: a title cut to its length, a path of valid UTF-8 without a control
 * character and within its bytes, numbers in their ranges, a word of
 * availability Axis knows or none, and a movie called usable only when it has
 * everything a run needs.  A failure returns nothing and a fixed reason.  An
 * accepted address is an http or https origin without credentials, query or
 * fragment, and a prefix of plain segments; a mapped path is relative,
 * without a "." or ".." segment, below one of the mapped folders.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arr/axis-arr-parse.h"
#include "download/axis-download-path.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_text(
	const gchar *text,
	glong        max_chars
){
	if (text == NULL)
		return;
	if (text[0] == '\0' || !g_utf8_validate(text, -1, NULL) || g_utf8_strlen(text, -1) > max_chars)
		abort();
}

static void
check_path(const gchar *path)
{
	const gchar *p;

	if (path == NULL)
		return;
	if (path[0] == '\0' || strlen(path) > AXIS_ARR_PATH_MAX_BYTES || !g_utf8_validate(path, -1, NULL))
		abort();
	for (p = path; *p != '\0'; p = g_utf8_next_char(p)) {
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			abort();
	}
}

static void
check_movie(const AxisArrMovie *movie)
{
	if (movie == NULL)
		abort();
	check_text(movie->title, AXIS_ARR_TITLE_MAX_CHARS);
	check_text(movie->file_quality, 40);
	check_text(movie->file_scene, AXIS_ARR_TITLE_MAX_CHARS);
	check_path(movie->path);
	check_path(movie->root_path);
	check_path(movie->file_path);
	if (movie->arr_id < 0 || movie->arr_id > AXIS_ARR_ID_MAX || movie->tmdb_id < 0 || movie->tmdb_id > AXIS_ARR_ID_MAX ||
	    movie->profile_id < 0 || movie->profile_id > AXIS_ARR_ID_MAX || movie->file_size < -1 ||
	    (movie->year != 0 && (movie->year < 1870 || movie->year > 2200)))
		abort();
	if (movie->min_availability != NULL && strcmp(movie->min_availability, "announced") != 0 &&
	    strcmp(movie->min_availability, "in_cinemas") != 0 && strcmp(movie->min_availability, "released") != 0)
		abort();
	if (movie->imdb_id != NULL) {
		gsize length = strlen(movie->imdb_id);
		gsize i;

		if (length < 9 || length > 12 || movie->imdb_id[0] != 't' || movie->imdb_id[1] != 't')
			abort();
		for (i = 2; i < length; i++) {
			if (!g_ascii_isdigit(movie->imdb_id[i]))
				abort();
		}
	}
	/* Usable means a run has what it needs. */
	if (movie->usable && (movie->arr_id <= 0 || movie->title == NULL || movie->profile_id <= 0 || movie->path == NULL))
		abort();
	/* A file described is a file said. */
	if ((movie->file_path != NULL || movie->file_size >= 0 || movie->file_quality != NULL) && !movie->has_file)
		abort();
}

static void
check_status(
	const gchar *text,
	gsize        length
){
	gchar *version = NULL;
	const gchar *p;

	if (!axis_arr_parse_status(text, length, AXIS_ARR_RADARR, &version)) {
		if (version != NULL)
			abort();
	} else if (version != NULL) {
		if (version[0] == '\0' || strlen(version) > AXIS_ARR_VERSION_MAX_CHARS)
			abort();
		for (p = version; *p != '\0'; p++) {
			if (!g_ascii_isalnum(*p) && *p != '.' && *p != '-')
				abort();
		}
	}
	g_free(version);
	version = NULL;
	/* Never both apps at once. */
	if (axis_arr_parse_status(text, length, AXIS_ARR_SONARR, NULL) &&
	    axis_arr_parse_status(text, length, AXIS_ARR_RADARR, NULL))
		abort();
}

static void
check_movies(
	const gchar *text,
	gsize        length
){
	GPtrArray *movies = NULL;
	const gchar *problem = NULL;
	guint i;

	/* A small limit too: the count is refused, never cut. */
	if (!axis_arr_parse_movies(text, length, 64, &movies, &problem)) {
		if (movies != NULL || problem == NULL)
			abort();
		return;
	}
	if (movies == NULL || movies->len > 64 || problem != NULL)
		abort();
	for (i = 0; i < movies->len; i++)
		check_movie(g_ptr_array_index(movies, i));
	g_ptr_array_unref(movies);
}

static void
check_lists(
	gint         which,
	const gchar *text,
	gsize        length
){
	GPtrArray *list = NULL;
	const gchar *problem = NULL;
	guint i;

	if (which == 2) {
		if (!axis_arr_parse_profiles(text, length, &list, &problem)) {
			if (list != NULL || problem == NULL)
				abort();
			return;
		}
		if (list == NULL || list->len > AXIS_ARR_MAX_PROFILES)
			abort();
		for (i = 0; i < list->len; i++) {
			const AxisArrProfile *profile = g_ptr_array_index(list, i);

			if (profile->id <= 0 || profile->name == NULL)
				abort();
			check_text(profile->name, AXIS_ARR_NAME_MAX_CHARS);
		}
	} else {
		if (!axis_arr_parse_folders(text, length, &list, &problem)) {
			if (list != NULL || problem == NULL)
				abort();
			return;
		}
		if (list == NULL || list->len > AXIS_ARR_MAX_FOLDERS)
			abort();
		for (i = 0; i < list->len; i++) {
			const AxisArrFolder *folder = g_ptr_array_index(list, i);

			if (folder->path == NULL || folder->path[0] != '/' || strstr(folder->path, "/../") != NULL ||
			    g_str_has_suffix(folder->path, "/..") || g_str_has_suffix(folder->path, "/"))
				abort();
			check_path(folder->path);
		}
	}
	g_ptr_array_unref(list);
}

static void
check_series(
	const gchar *text,
	gsize        length
){
	GPtrArray *list = NULL;
	const gchar *problem = NULL;
	guint i;
	guint k;
	guint m;

	if (!axis_arr_parse_series(text, length, 64, &list, &problem)) {
		if (list != NULL || problem == NULL)
			abort();
		return;
	}
	if (list == NULL || list->len > 64 || problem != NULL)
		abort();
	for (i = 0; i < list->len; i++) {
		const AxisArrSeries *series = g_ptr_array_index(list, i);

		if (series == NULL || series->seasons == NULL)
			abort();
		check_text(series->title, AXIS_ARR_TITLE_MAX_CHARS);
		check_path(series->path);
		check_path(series->root_path);
		if (series->arr_id < 0 || series->arr_id > AXIS_ARR_ID_MAX || series->tvdb_id < 0 ||
		    series->tvdb_id > AXIS_ARR_ID_MAX || series->profile_id < 0 || series->profile_id > AXIS_ARR_ID_MAX ||
		    (series->year != 0 && (series->year < 1870 || series->year > 2200)) || series->monitor_new < -1 ||
		    series->monitor_new > 1)
			abort();
		/* One of the three words the store's constraint takes, or none (a word
		 * Axis does not know). */
		if (series->series_type != NULL && strcmp(series->series_type, "standard") != 0 &&
		    strcmp(series->series_type, "daily") != 0 && strcmp(series->series_type, "anime") != 0)
			abort();
		if (series->seasons->len > AXIS_ARR_MAX_SEASONS)
			abort();
		/* Each season once, within its range. */
		for (k = 0; k < series->seasons->len; k++) {
			gint number = g_array_index(series->seasons, AxisArrSeason, k).number;

			if (number < 0 || number > AXIS_ARR_SEASON_MAX)
				abort();
			for (m = 0; m < k; m++) {
				if (g_array_index(series->seasons, AxisArrSeason, m).number == number)
					abort();
			}
		}
		/* Usable means the preview has what it needs. */
		if (series->usable && (series->arr_id <= 0 || series->title == NULL || series->profile_id <= 0 ||
		    series->path == NULL))
			abort();
	}
	g_ptr_array_unref(list);
}

static void
check_parts(
	gint         which,
	const gchar *text,
	gsize        length
){
	const gchar *problem = NULL;
	guint odd = 7;
	guint i;

	if (which == 8) {
		GArray *episodes = NULL;

		if (!axis_arr_parse_episodes(text, length, 64, &episodes, &odd, &problem)) {
			if (episodes != NULL || problem == NULL || odd != 0)
				abort();
			return;
		}
		if (episodes == NULL || problem != NULL || episodes->len + odd > 64)
			abort();
		for (i = 0; i < episodes->len; i++) {
			const AxisArrEpisode *episode = &g_array_index(episodes, AxisArrEpisode, i);

			if (episode->season < 0 || episode->season > AXIS_ARR_SEASON_MAX || episode->number < 0 ||
			    episode->number > AXIS_ARR_EPISODE_MAX || episode->arr_id < 0 || episode->arr_id > AXIS_ARR_ID_MAX ||
			    episode->tvdb_id < 0 || episode->tvdb_id > AXIS_ARR_ID_MAX || episode->file_id < 0 ||
			    episode->file_id > AXIS_ARR_ID_MAX)
				abort();
			/* Sonarr's absolute number (A20): none, or one the plan's column
			 * takes; and never anything on a place that is all that was read. */
			if (episode->absolute < 0 || episode->absolute > AXIS_ARR_ABSOLUTE_MAX ||
			    (episode->odd && episode->absolute != 0))
				abort();
			/* No day, or a day there is: the store takes it as a date. */
			if (episode->air_date[10] != '\0')
				abort();
			if (episode->air_date[0] != '\0') {
				const gchar *d = episode->air_date;
				guint year;
				guint month;
				guint day;
				guint k;

				if (strlen(d) != 10 || d[4] != '-' || d[7] != '-')
					abort();
				for (k = 0; k < 10; k++) {
					if (k != 4 && k != 7 && !g_ascii_isdigit(d[k]))
						abort();
				}
				year = (guint)g_ascii_strtoull(d, NULL, 10);
				month = (guint)g_ascii_strtoull(d + 5, NULL, 10);
				day = (guint)g_ascii_strtoull(d + 8, NULL, 10);
				if (year < 1800 || year > 2200 || !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
					abort();
			}
		}
		g_array_unref(episodes);
	} else {
		GPtrArray *files = NULL;

		if (!axis_arr_parse_episode_files(text, length, 64, &files, &odd, &problem)) {
			if (files != NULL || problem == NULL || odd != 0)
				abort();
			return;
		}
		if (files == NULL || problem != NULL || files->len + odd > 64)
			abort();
		for (i = 0; i < files->len; i++) {
			const AxisArrEpisodeFile *file = g_ptr_array_index(files, i);

			if (file == NULL || file->arr_id <= 0 || file->arr_id > AXIS_ARR_ID_MAX || file->size < -1)
				abort();
			check_path(file->path);
			check_text(file->quality, 40);
			check_text(file->scene, AXIS_ARR_TITLE_MAX_CHARS);
		}
		g_ptr_array_unref(files);
	}
}

static void
check_queue(
	const gchar *text,
	gsize        length,
	AxisArrApp   app
){
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	const gchar *problem = NULL;
	guint total = 0;
	guint n = 0;
	guint i;

	if (!axis_arr_parse_queue_page(text, length, app, ids, &total, &n, &problem)) {
		if (ids->len != 0 || problem == NULL || total != 0 || n != 0)
			abort();
	} else {
		if (ids->len > AXIS_ARR_MAX_PAGE_RECORDS || ids->len > n || problem != NULL)
			abort();
		for (i = 0; i < ids->len; i++) {
			if (g_array_index(ids, gint64, i) <= 0 || g_array_index(ids, gint64, i) > AXIS_ARR_ID_MAX)
				abort();
		}
	}
	g_array_unref(ids);
}

static void
check_address(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar *origin = NULL;
	gchar *prefix = NULL;
	const gchar *problem = NULL;
	const gchar *p;

	if (!axis_arr_address_split(copy, &origin, &prefix, &problem)) {
		if (origin != NULL || prefix != NULL || problem == NULL)
			abort();
		/* The reason never repeats what was typed. */
		if (strlen(copy) >= 12 && strstr(problem, copy) != NULL)
			abort();
		g_free(copy);
		return;
	}
	if (origin == NULL || prefix == NULL || problem != NULL)
		abort();
	if (!g_str_has_prefix(origin, "http://") && !g_str_has_prefix(origin, "https://"))
		abort();
	p = strstr(origin, "://") + 3;
	if (*p == '\0' || strlen(origin) > 255)
		abort();
	for (; *p != '\0'; p++) {
		if (*p == '@' || *p == '?' || *p == '#' || *p == '/' || *p == '\\' || (guchar)*p <= 0x20)
			abort();
	}
	if (prefix[0] != '\0') {
		if (prefix[0] != '/' || g_str_has_suffix(prefix, "/") || strstr(prefix, "//") != NULL ||
		    strstr(prefix, "/../") != NULL || g_str_has_suffix(prefix, "/..") || strstr(prefix, "/./") != NULL ||
		    g_str_has_suffix(prefix, "/.") || strlen(prefix) > 128)
			abort();
		for (p = prefix; *p != '\0'; p++) {
			if (!g_ascii_isalnum(*p) && strchr("/._~-", *p) == NULL)
				abort();
		}
	}
	g_free(origin);
	g_free(prefix);
	g_free(copy);
}

static void
check_map(
	const gchar *text,
	gsize        length
){
	GPtrArray *maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);
	gchar *copy = g_strndup(text, length);
	gchar *stored = NULL;
	gint64 root = -1;
	AxisArrPathResult result;

	g_ptr_array_add(maps, axis_download_path_map_new("/movies", 11, ""));
	g_ptr_array_add(maps, axis_download_path_map_new("/movies/kids", 12, "young"));
	g_ptr_array_add(maps, axis_download_path_map_new("/m", 13, "a/b"));
	result = axis_arr_map_path(maps, copy, &root, &stored);
	if (result != AXIS_ARR_PATH_MAPPED) {
		if (stored != NULL || root != 0)
			abort();
	} else {
		gchar **segments;
		guint i;

		if (stored == NULL || stored[0] == '\0' || stored[0] == '/' || (root != 11 && root != 12 && root != 13) ||
		    !g_utf8_validate(stored, -1, NULL) || copy[0] != '/')
			abort();
		segments = g_strsplit(stored, "/", -1);
		for (i = 0; segments[i] != NULL; i++) {
			if (segments[i][0] == '\0' || strcmp(segments[i], ".") == 0 || strcmp(segments[i], "..") == 0)
				abort();
		}
		g_strfreev(segments);
		/* Below the folder's own place (or that place itself, for a path
		 * that is the mapped folder). */
		if ((root == 12 && strcmp(stored, "young") != 0 && !g_str_has_prefix(stored, "young/")) ||
		    (root == 13 && strcmp(stored, "a/b") != 0 && !g_str_has_prefix(stored, "a/b/")))
			abort();
	}
	g_free(stored);
	g_free(copy);
	g_ptr_array_unref(maps);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text;
	gsize length;

	if (size < 1 || size > 1 << 20)
		return 0;
	text = (const gchar *)data + 1;
	length = size - 1;
	switch (data[0]) {
	case '0':
		check_status(text, length);
		break;
	case '1':
		check_movies(text, length);
		break;
	case '2':
	case '3':
		check_lists(data[0] - '0', text, length);
		break;
	case '4':
		check_queue(text, length, AXIS_ARR_RADARR);
		check_queue(text, length, AXIS_ARR_SONARR);
		break;
	case '7':
		check_series(text, length);
		break;
	case '8':
	case '9':
		check_parts(data[0] - '0', text, length);
		break;
	case '5':
		check_address(text, length);
		break;
	case '6':
		check_map(text, length);
		break;
	default:
		break;
	}
	return 0;
}
