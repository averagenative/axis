/*
 * axis-arr-parse.c - reading Radarr's and Sonarr's answers within limits
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "arr/axis-arr-parse.h"

#include <string.h>

#include "arr/axis-arr-assumed.h"
#include "download/axis-download-model.h"
#include "download/axis-download-path.h"
#include "library/axis-library-name.h"
#include "meta/axis-meta-model.h"
#include "probe/axis-probe-json.h"
#include "storage/axis-storage-path.h"

/* ── The apps ───────────────────────────────────────────────────── */

const gchar *
axis_arr_app_to_string(AxisArrApp app)
{
	return app == AXIS_ARR_SONARR ? "sonarr" : "radarr";
}

gboolean
axis_arr_app_from_string(
	const gchar *text,
	AxisArrApp  *app
){
	if (g_strcmp0(text, "radarr") == 0)
		*app = AXIS_ARR_RADARR;
	else if (g_strcmp0(text, "sonarr") == 0)
		*app = AXIS_ARR_SONARR;
	else
		return FALSE;
	return TRUE;
}

const gchar *
axis_arr_app_label(AxisArrApp app)
{
	return app == AXIS_ARR_SONARR ? AXIS_ARR_APP_NAME_SONARR : AXIS_ARR_APP_NAME_RADARR;
}

const gchar *
axis_arr_app_noun(
	AxisArrApp app,
	gboolean   many
){
	if (app == AXIS_ARR_SONARR)
		return "series";
	return many ? "movies" : "movie";
}

/* ── Members ────────────────────────────────────────────────────── */

/* A string member, cleaned and cut; %NULL when missing, of another type or
 * empty once cleaned. */
static gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	const AxisJsonNode *node = axis_json_member(object, name);

	if (axis_json_type(node) != AXIS_JSON_STRING)
		return NULL;
	return axis_meta_clean_text(axis_json_string(node), max_chars);
}

/* A number member that is a whole number within the range.  Only a number
 * counts: a string that holds one does not. */
static gboolean
int_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              minimum,
	gint64              maximum,
	gint64             *value
){
	const AxisJsonNode *node = axis_json_member(object, name);

	if (axis_json_type(node) != AXIS_JSON_NUMBER)
		return FALSE;
	return axis_json_get_integer(node, minimum, maximum, value);
}

static gboolean
bool_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gboolean           *value
){
	const AxisJsonNode *node = axis_json_member(object, name);

	if (axis_json_type(node) != AXIS_JSON_BOOLEAN)
		return FALSE;
	return axis_json_get_boolean(node, value);
}

/* A path member, byte for byte: a string of 1 to %AXIS_ARR_PATH_MAX_BYTES
 * bytes of valid UTF-8 without a control character.  Anything else is no
 * path (a cleaned or cut path would name another place). */
static gchar *
path_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const AxisJsonNode *node = axis_json_member(object, name);
	const gchar *text;
	const gchar *p;
	gsize length;

	if (axis_json_type(node) != AXIS_JSON_STRING)
		return NULL;
	text = axis_json_string(node);
	length = text != NULL ? strlen(text) : 0;
	/* The reader keeps a string's first AXIS_JSON_MAX_STRING bytes: one that
	 * long may have been cut, and is no path either. */
	if (length == 0 || length > AXIS_ARR_PATH_MAX_BYTES || length >= AXIS_JSON_MAX_STRING ||
	    !g_utf8_validate(text, (gssize)length, NULL))
		return NULL;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		/* U+FFFD is what the reader makes of bytes that were not text. */
		if (g_unichar_iscntrl(c) || c == 0xFFFD)
			return NULL;
	}
	return g_strdup(text);
}

/* ── system/status ──────────────────────────────────────────────── */

gboolean
axis_arr_parse_status(
	const gchar  *data,
	gsize         length,
	AxisArrApp    app,
	gchar       **version
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	const gchar *name;

	if (version != NULL)
		*version = NULL;
	if (data == NULL || length == 0)
		return FALSE;
	doc = axis_json_parse(data, length, NULL);
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	if (axis_json_type(root) != AXIS_JSON_OBJECT)
		return FALSE;
	name = axis_json_string(axis_json_member(root, AXIS_ARR_STATUS_APP));
	if (name == NULL || strcmp(name, axis_arr_app_label(app)) != 0)
		return FALSE;
	if (version != NULL) {
		const gchar *text = axis_json_string(axis_json_member(root, AXIS_ARR_STATUS_VERSION));
		GString *clean = g_string_new(NULL);
		const gchar *p;

		for (p = text; p != NULL && *p != '\0' && clean->len < AXIS_ARR_VERSION_MAX_CHARS; p++) {
			if (g_ascii_isalnum(*p) || *p == '.' || *p == '-')
				g_string_append_c(clean, *p);
		}
		if (clean->len > 0)
			*version = g_string_free(clean, FALSE);
		else
			g_string_free(clean, TRUE);
	}
	return TRUE;
}

/* ── One movie ──────────────────────────────────────────────────── */

void
axis_arr_movie_free(AxisArrMovie *movie)
{
	if (movie == NULL)
		return;
	g_free(movie->title);
	g_free(movie->imdb_id);
	g_free(movie->min_availability);
	g_free(movie->path);
	g_free(movie->root_path);
	g_free(movie->file_path);
	g_free(movie->file_quality);
	g_free(movie->file_scene);
	g_free(movie);
}

static const gchar *
availability_of(const gchar *word)
{
#define X(arr, axis) if (strcmp(word, arr) == 0) return axis;
	AXIS_ARR_AVAILABILITY_TABLE(X)
#undef X
	return NULL;
}

const gchar *
axis_arr_quality_to_axis(const gchar *arr_name)
{
	if (arr_name == NULL)
		return NULL;
#define X(arr, axis) if (strcmp(arr_name, arr) == 0) return axis;
	AXIS_ARR_QUALITY_TABLE(X)
#undef X
	return NULL;
}

static gboolean
imdb_valid(const gchar *text)
{
	gsize length = strlen(text);
	gsize i;

	if (length < 9 || length > 12 || text[0] != 't' || text[1] != 't')
		return FALSE;
	for (i = 2; i < length; i++) {
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	}
	return TRUE;
}

/* What is kept of a file's record, a movie's or an episode file's (A4, A14):
 * its path, its size (left as it is when not given), its quality's name and
 * the release it came from. */
static void
read_file(
	const AxisJsonNode  *file,
	gchar              **path,
	gint64              *file_size,
	gchar              **quality_name,
	gchar              **scene
){
	const AxisJsonNode *quality;
	gint64 size = 0;

	if (axis_json_type(file) != AXIS_JSON_OBJECT)
		return;
	*path = path_member(file, AXIS_ARR_FILE_PATH);
	if (int_member(file, AXIS_ARR_FILE_SIZE, 0, G_MAXINT64 / 2, &size))
		*file_size = size;
	quality = axis_json_member(axis_json_member(file, AXIS_ARR_FILE_QUALITY), AXIS_ARR_FILE_QUALITY_INNER);
	*quality_name = text_member(quality, AXIS_ARR_FILE_QUALITY_NAME, 40);
	*scene = text_member(file, AXIS_ARR_FILE_SCENE_NAME, AXIS_ARR_TITLE_MAX_CHARS);
}

/* @object (nullable: an element that could not be read) as a movie. */
static AxisArrMovie *
read_movie(const AxisJsonNode *object)
{
	AxisArrMovie *movie = g_new0(AxisArrMovie, 1);
	const AxisJsonNode *tmdb;
	g_autofree gchar *availability = NULL;
	g_autofree gchar *imdb = NULL;
	gboolean complete = TRUE;
	gint64 number = 0;

	movie->file_size = -1;
	if (axis_json_type(object) != AXIS_JSON_OBJECT)
		return movie;
	if (int_member(object, AXIS_ARR_MOVIE_ID, 1, AXIS_ARR_ID_MAX, &number))
		movie->arr_id = number;
	else
		complete = FALSE;
	movie->title = text_member(object, AXIS_ARR_MOVIE_TITLE, AXIS_ARR_TITLE_MAX_CHARS);
	if (movie->title == NULL)
		complete = FALSE;
	if (int_member(object, AXIS_ARR_MOVIE_YEAR, 1870, 2200, &number))
		movie->year = (gint)number;
	/* No identifier (absent, null or 0) is a movie Radarr itself could not
	 * name; one of another form is a record that cannot be trusted. */
	tmdb = axis_json_member(object, AXIS_ARR_MOVIE_TMDB);
	if (axis_json_type(tmdb) != AXIS_JSON_NULL) {
		if (int_member(object, AXIS_ARR_MOVIE_TMDB, 0, AXIS_ARR_ID_MAX, &number))
			movie->tmdb_id = number;
		else
			complete = FALSE;
	}
	imdb = text_member(object, AXIS_ARR_MOVIE_IMDB, 16);
	if (imdb != NULL && imdb_valid(imdb))
		movie->imdb_id = g_steal_pointer(&imdb);
	if (!bool_member(object, AXIS_ARR_MOVIE_MONITORED, &movie->monitored))
		complete = FALSE;
	availability = text_member(object, AXIS_ARR_MOVIE_AVAILABILITY, 32);
	if (availability != NULL)
		movie->min_availability = g_strdup(availability_of(availability));
	if (int_member(object, AXIS_ARR_MOVIE_PROFILE, 1, AXIS_ARR_ID_MAX, &number))
		movie->profile_id = number;
	else
		complete = FALSE;
	movie->path = path_member(object, AXIS_ARR_MOVIE_PATH);
	if (movie->path == NULL)
		complete = FALSE;
	movie->root_path = path_member(object, AXIS_ARR_MOVIE_ROOT);
	/* A file is said by the flag or by the record itself. */
	bool_member(object, AXIS_ARR_MOVIE_HAS_FILE, &movie->has_file);
	if (axis_json_type(axis_json_member(object, AXIS_ARR_MOVIE_FILE)) == AXIS_JSON_OBJECT) {
		movie->has_file = TRUE;
		read_file(axis_json_member(object, AXIS_ARR_MOVIE_FILE), &movie->file_path, &movie->file_size,
			&movie->file_quality, &movie->file_scene);
	}
	movie->usable = complete;
	return movie;
}

/* ── The list, cut into its elements ────────────────────────────── */

static gboolean
is_space(gchar c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

#define TOO_MANY_TITLES  "holds more titles than Axis reads"
#define TOO_MANY_ENTRIES "holds more entries than Axis reads"

typedef void (*ElementFunc)(
	const gchar *element,
	gsize        length,
	gpointer     user_data
);

/*
 * Calls @func for every element of the array @data holds, in order, with
 * the element's own text.  Only strings and nesting are followed: what an
 * element is, is the reader's to say.  Fails when @data is not one array,
 * or holds more than @max elements (@too_many says so).
 */
static gboolean
split_array(
	const gchar  *data,
	gsize         length,
	guint         max,
	const gchar  *too_many,
	ElementFunc   func,
	gpointer      user_data,
	const gchar **problem
){
	gsize pos = 0;
	gsize start;
	guint depth = 0;
	guint count = 0;
	gboolean in_string = FALSE;
	gboolean closed = FALSE;

	while (pos < length && is_space(data[pos]))
		pos++;
	if (pos >= length || data[pos] != '[') {
		*problem = "is not a list";
		return FALSE;
	}
	pos++;
	while (pos < length && is_space(data[pos]))
		pos++;
	if (pos < length && data[pos] == ']') {
		closed = TRUE;
		pos++;
	}
	start = pos;
	while (!closed && pos < length) {
		gchar c = data[pos];

		if (in_string) {
			if (c == '\\')
				pos++;
			else if (c == '"')
				in_string = FALSE;
			pos++;
			continue;
		}
		if (c == '"') {
			in_string = TRUE;
		} else if (c == '{' || c == '[') {
			depth++;
		} else if ((c == '}' || c == ']') && depth > 0) {
			depth--;
		} else if (c == '}') {
			*problem = "is not a list";
			return FALSE;
		} else if (c == ',' || c == ']') {
			if (depth == 0) {
				gsize from = start;
				gsize to = pos;

				while (from < to && is_space(data[from]))
					from++;
				while (to > from && is_space(data[to - 1]))
					to--;
				if (++count > max) {
					*problem = too_many;
					return FALSE;
				}
				func(data + from, to - from, user_data);
				start = pos + 1;
				if (c == ']')
					closed = TRUE;
			}
		}
		pos++;
	}
	if (!closed) {
		*problem = "is cut short";
		return FALSE;
	}
	while (pos < length && is_space(data[pos]))
		pos++;
	if (pos != length) {
		*problem = "is not a list";
		return FALSE;
	}
	return TRUE;
}

static void
movie_element(
	const gchar *element,
	gsize        length,
	gpointer     user_data
){
	GPtrArray *movies = user_data;
	AxisJsonDoc *doc = NULL;

	if (length > 0 && length <= AXIS_ARR_MAX_ELEMENT_BYTES)
		doc = axis_json_parse(element, length, NULL);
	g_ptr_array_add(movies, read_movie(doc != NULL ? axis_json_doc_get_root(doc) : NULL));
	axis_json_doc_free(doc);
}

gboolean
axis_arr_parse_movies(
	const gchar  *data,
	gsize         length,
	guint         max,
	GPtrArray   **movies,
	const gchar **problem
){
	GPtrArray *out;
	const gchar *why = NULL;

	*movies = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (data == NULL) {
		if (problem != NULL)
			*problem = "is empty";
		return FALSE;
	}
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_arr_movie_free);
	if (!split_array(data, length, MIN(max, AXIS_ARR_MAX_TITLES), TOO_MANY_TITLES, movie_element, out, &why)) {
		g_ptr_array_unref(out);
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	*movies = out;
	return TRUE;
}

/* ── Sonarr: one series ─────────────────────────────────────────── */

void
axis_arr_series_free(AxisArrSeries *series)
{
	if (series == NULL)
		return;
	g_free(series->title);
	g_free(series->imdb_id);
	g_free(series->path);
	g_free(series->root_path);
	g_clear_pointer(&series->seasons, g_array_unref);
	g_free(series);
}

/* The seasons of a series, each once.  %FALSE when the list is missing, too
 * long, has an entry out of shape or names a season twice: its monitoring is
 * then not known whole, and nothing of it is guessed. */
static gboolean
read_seasons(
	const AxisJsonNode *list,
	GArray             *seasons
){
	guint i;
	guint k;

	if (axis_json_type(list) != AXIS_JSON_ARRAY || axis_json_length(list) > AXIS_ARR_MAX_SEASONS)
		return FALSE;
	for (i = 0; i < axis_json_length(list); i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		AxisArrSeason season;
		gint64 number = 0;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT ||
		    !int_member(entry, AXIS_ARR_SEASON_NUMBER, 0, AXIS_ARR_SEASON_MAX, &number) ||
		    !bool_member(entry, AXIS_ARR_SEASON_MONITORED, &season.monitored))
			return FALSE;
		season.number = (gint)number;
		for (k = 0; k < seasons->len; k++) {
			if (g_array_index(seasons, AxisArrSeason, k).number == season.number)
				return FALSE;
		}
		g_array_append_val(seasons, season);
	}
	return TRUE;
}

/* @object (nullable: an element that could not be read) as a series. */
static AxisArrSeries *
read_series(const AxisJsonNode *object)
{
	AxisArrSeries *series = g_new0(AxisArrSeries, 1);
	const AxisJsonNode *tvdb;
	g_autofree gchar *type = NULL;
	g_autofree gchar *imdb = NULL;
	g_autofree gchar *later = NULL;
	gboolean complete = TRUE;
	gint64 number = 0;

	series->seasons = g_array_new(FALSE, FALSE, sizeof(AxisArrSeason));
	series->series_type = AXIS_ARR_SERIES_TYPE_STANDARD;
	series->monitor_new = -1;
	if (axis_json_type(object) != AXIS_JSON_OBJECT)
		return series;
	if (int_member(object, AXIS_ARR_SERIES_ID, 1, AXIS_ARR_ID_MAX, &number))
		series->arr_id = number;
	else
		complete = FALSE;
	series->title = text_member(object, AXIS_ARR_SERIES_TITLE, AXIS_ARR_TITLE_MAX_CHARS);
	if (series->title == NULL)
		complete = FALSE;
	if (int_member(object, AXIS_ARR_SERIES_YEAR, 1870, 2200, &number))
		series->year = (gint)number;
	/* As a movie's: no identifier is a series Sonarr could not name; one of
	 * another form is a record that cannot be trusted. */
	tvdb = axis_json_member(object, AXIS_ARR_SERIES_TVDB);
	if (axis_json_type(tvdb) != AXIS_JSON_NULL) {
		if (int_member(object, AXIS_ARR_SERIES_TVDB, 0, AXIS_ARR_ID_MAX, &number))
			series->tvdb_id = number;
		else
			complete = FALSE;
	}
	imdb = text_member(object, AXIS_ARR_SERIES_IMDB, 16);
	if (imdb != NULL && imdb_valid(imdb))
		series->imdb_id = g_steal_pointer(&imdb);
	if (!bool_member(object, AXIS_ARR_SERIES_MONITORED, &series->monitored))
		complete = FALSE;
	/* Not said (absent or null) is a standard series; a word Axis does not know
	 * is stored as not said (NULL) and the series is skipped, not guessed. */
	if (axis_json_type(axis_json_member(object, AXIS_ARR_SERIES_TYPE)) != AXIS_JSON_NULL) {
		type = text_member(object, AXIS_ARR_SERIES_TYPE, 32);
		if (type != NULL && g_ascii_strcasecmp(type, AXIS_ARR_SERIES_TYPE_ANIME) == 0)
			series->series_type = AXIS_ARR_SERIES_TYPE_ANIME;
		else if (type != NULL && g_ascii_strcasecmp(type, AXIS_ARR_SERIES_TYPE_DAILY) == 0)
			series->series_type = AXIS_ARR_SERIES_TYPE_DAILY;
		else if (type != NULL && g_ascii_strcasecmp(type, AXIS_ARR_SERIES_TYPE_STANDARD) == 0)
			series->series_type = AXIS_ARR_SERIES_TYPE_STANDARD;
		else
			series->series_type = NULL;
	}
	later = text_member(object, AXIS_ARR_SERIES_MONITOR_NEW, 32);
	if (later != NULL && strcmp(later, AXIS_ARR_MONITOR_NEW_ALL) == 0)
		series->monitor_new = 1;
	else if (later != NULL && strcmp(later, AXIS_ARR_MONITOR_NEW_NONE) == 0)
		series->monitor_new = 0;
	if (int_member(object, AXIS_ARR_SERIES_PROFILE, 1, AXIS_ARR_ID_MAX, &number))
		series->profile_id = number;
	else
		complete = FALSE;
	series->path = path_member(object, AXIS_ARR_SERIES_PATH_MEMBER);
	if (series->path == NULL)
		complete = FALSE;
	series->root_path = path_member(object, AXIS_ARR_SERIES_ROOT);
	if (!read_seasons(axis_json_member(object, AXIS_ARR_SERIES_SEASONS), series->seasons)) {
		g_array_set_size(series->seasons, 0);
		complete = FALSE;
	}
	series->usable = complete;
	return series;
}

static void
series_element(
	const gchar *element,
	gsize        length,
	gpointer     user_data
){
	GPtrArray *list = user_data;
	AxisJsonDoc *doc = NULL;

	if (length > 0 && length <= AXIS_ARR_MAX_ELEMENT_BYTES)
		doc = axis_json_parse(element, length, NULL);
	g_ptr_array_add(list, read_series(doc != NULL ? axis_json_doc_get_root(doc) : NULL));
	axis_json_doc_free(doc);
}

gboolean
axis_arr_parse_series(
	const gchar  *data,
	gsize         length,
	guint         max,
	GPtrArray   **series,
	const gchar **problem
){
	GPtrArray *out;
	const gchar *why = NULL;

	*series = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (data == NULL) {
		if (problem != NULL)
			*problem = "is empty";
		return FALSE;
	}
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_arr_series_free);
	if (!split_array(data, length, MIN(max, AXIS_ARR_MAX_TITLES), TOO_MANY_TITLES, series_element, out, &why)) {
		g_ptr_array_unref(out);
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	*series = out;
	return TRUE;
}

/* ── Sonarr: a series' episodes and its files ───────────────────── */

void
axis_arr_episode_file_free(AxisArrEpisodeFile *file)
{
	if (file == NULL)
		return;
	g_free(file->path);
	g_free(file->quality);
	g_free(file->scene);
	g_free(file);
}

/* "YYYY-MM-DD" of a day there is, between 1800 and 2200. */
static gboolean
date_valid(const gchar *text)
{
	guint year;
	guint month;
	guint day;
	guint i;

	if (strlen(text) != 10 || text[4] != '-' || text[7] != '-')
		return FALSE;
	for (i = 0; i < 10; i++) {
		if (i != 4 && i != 7 && !g_ascii_isdigit(text[i]))
			return FALSE;
	}
	year = (guint)(text[0] - '0') * 1000 + (guint)(text[1] - '0') * 100 + (guint)(text[2] - '0') * 10 +
		(guint)(text[3] - '0');
	month = (guint)(text[5] - '0') * 10 + (guint)(text[6] - '0');
	day = (guint)(text[8] - '0') * 10 + (guint)(text[9] - '0');
	return year >= 1800 && year <= 2200 && g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year);
}

typedef struct
{
	GArray    *episodes;	/* AxisArrEpisode, or NULL */
	GPtrArray *files;	/* AxisArrEpisodeFile, or NULL */
	guint      odd;
} Parts;

/* An identifier that is 0, null or absent is none; one of another form is
 * none too (the entry is kept: its place is what it is known by). */
static gint64
id_or_none(
	const AxisJsonNode *object,
	const gchar        *name
){
	gint64 value = 0;

	if (!int_member(object, name, 0, AXIS_ARR_ID_MAX, &value))
		return 0;
	return value;
}

static void
episode_element(
	const gchar *element,
	gsize        length,
	gpointer     user_data
){
	Parts *parts = user_data;
	AxisJsonDoc *doc = NULL;
	const AxisJsonNode *object;
	g_autofree gchar *aired = NULL;
	AxisArrEpisode episode;
	gint64 season = 0;
	gint64 number = 0;
	gint64 absolute = 0;

	memset(&episode, 0, sizeof(episode));
	if (length > 0 && length <= AXIS_ARR_MAX_ELEMENT_BYTES)
		doc = axis_json_parse(element, length, NULL);
	object = doc != NULL ? axis_json_doc_get_root(doc) : NULL;
	if (axis_json_type(object) != AXIS_JSON_OBJECT ||
	    !int_member(object, AXIS_ARR_EPISODE_SEASON, 0, AXIS_ARR_SEASON_MAX, &season) ||
	    !int_member(object, AXIS_ARR_EPISODE_NUMBER, 0, AXIS_ARR_EPISODE_MAX, &number)) {
		parts->odd++;
		axis_json_doc_free(doc);
		return;
	}
	episode.season = (gint)season;
	episode.number = (gint)number;
	if (!bool_member(object, AXIS_ARR_EPISODE_MONITORED, &episode.monitored)) {
		/* Its place is known and the rest cannot be trusted: the place is
		 * kept, so that nothing is searched for there (Sonarr may hold a
		 * file for it), and nothing else of the entry is. */
		episode.odd = TRUE;
		episode.monitored = FALSE;
		g_array_append_val(parts->episodes, episode);
		axis_json_doc_free(doc);
		return;
	}
	episode.arr_id = id_or_none(object, AXIS_ARR_EPISODE_ID);
	episode.tvdb_id = id_or_none(object, AXIS_ARR_EPISODE_TVDB);
	episode.file_id = id_or_none(object, AXIS_ARR_EPISODE_FILE);
	/* Sonarr's own absolute number (A20): a whole number in range, else none.
	 * It is only ever compared with TheTVDB's, so one that cannot be read
	 * costs the entry nothing. */
	if (int_member(object, AXIS_ARR_EPISODE_ABSOLUTE, 1, AXIS_ARR_ABSOLUTE_MAX, &absolute))
		episode.absolute = (gint)absolute;
	aired = text_member(object, AXIS_ARR_EPISODE_AIR_DATE, 16);
	if (aired != NULL && date_valid(aired))
		memcpy(episode.air_date, aired, 10);
	g_array_append_val(parts->episodes, episode);
	axis_json_doc_free(doc);
}

static void
episode_file_element(
	const gchar *element,
	gsize        length,
	gpointer     user_data
){
	Parts *parts = user_data;
	AxisJsonDoc *doc = NULL;
	const AxisJsonNode *object;
	AxisArrEpisodeFile *file;
	gint64 id = 0;

	if (length > 0 && length <= AXIS_ARR_MAX_ELEMENT_BYTES)
		doc = axis_json_parse(element, length, NULL);
	object = doc != NULL ? axis_json_doc_get_root(doc) : NULL;
	if (axis_json_type(object) != AXIS_JSON_OBJECT ||
	    !int_member(object, AXIS_ARR_EPISODE_FILE_ID, 1, AXIS_ARR_ID_MAX, &id)) {
		parts->odd++;
		axis_json_doc_free(doc);
		return;
	}
	file = g_new0(AxisArrEpisodeFile, 1);
	file->arr_id = id;
	file->size = -1;
	read_file(object, &file->path, &file->size, &file->quality, &file->scene);
	g_ptr_array_add(parts->files, file);
	axis_json_doc_free(doc);
}

gboolean
axis_arr_parse_episodes(
	const gchar  *data,
	gsize         length,
	guint         max,
	GArray      **episodes,
	guint        *n_odd,
	const gchar **problem
){
	const gchar *why = NULL;
	Parts parts;

	*episodes = NULL;
	*n_odd = 0;
	if (problem != NULL)
		*problem = NULL;
	if (data == NULL) {
		if (problem != NULL)
			*problem = "is empty";
		return FALSE;
	}
	memset(&parts, 0, sizeof(parts));
	parts.episodes = g_array_new(FALSE, TRUE, sizeof(AxisArrEpisode));
	if (!split_array(data, length, MIN(max, AXIS_ARR_MAX_SERIES_EPISODES), TOO_MANY_ENTRIES, episode_element, &parts,
	        &why)) {
		g_array_unref(parts.episodes);
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	*episodes = parts.episodes;
	*n_odd = parts.odd;
	return TRUE;
}

gboolean
axis_arr_parse_episode_files(
	const gchar  *data,
	gsize         length,
	guint         max,
	GPtrArray   **files,
	guint        *n_odd,
	const gchar **problem
){
	const gchar *why = NULL;
	Parts parts;

	*files = NULL;
	*n_odd = 0;
	if (problem != NULL)
		*problem = NULL;
	if (data == NULL) {
		if (problem != NULL)
			*problem = "is empty";
		return FALSE;
	}
	memset(&parts, 0, sizeof(parts));
	parts.files = g_ptr_array_new_with_free_func((GDestroyNotify)axis_arr_episode_file_free);
	if (!split_array(data, length, MIN(max, AXIS_ARR_MAX_SERIES_EPISODES), TOO_MANY_ENTRIES, episode_file_element,
	        &parts, &why)) {
		g_ptr_array_unref(parts.files);
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	*files = parts.files;
	*n_odd = parts.odd;
	return TRUE;
}

/* ── The short lists ────────────────────────────────────────────── */

void
axis_arr_profile_free(AxisArrProfile *profile)
{
	if (profile == NULL)
		return;
	g_free(profile->name);
	g_free(profile);
}

void
axis_arr_folder_free(AxisArrFolder *folder)
{
	if (folder == NULL)
		return;
	g_free(folder->path);
	g_free(folder);
}

/* The answer as a document whose root is an array. */
static AxisJsonDoc *
parse_array(
	const gchar  *data,
	gsize         length,
	const gchar **problem
){
	AxisJsonDoc *doc = data != NULL && length > 0 ? axis_json_parse(data, length, NULL) : NULL;

	if (doc == NULL || axis_json_type(axis_json_doc_get_root(doc)) != AXIS_JSON_ARRAY) {
		axis_json_doc_free(doc);
		if (problem != NULL)
			*problem = "is not a list";
		return NULL;
	}
	return doc;
}

gboolean
axis_arr_parse_profiles(
	const gchar  *data,
	gsize         length,
	GPtrArray   **profiles,
	const gchar **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	guint i;

	*profiles = NULL;
	doc = parse_array(data, length, problem);
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	*profiles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_arr_profile_free);
	for (i = 0; i < axis_json_length(root) && (*profiles)->len < AXIS_ARR_MAX_PROFILES; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		AxisArrProfile *profile;
		gchar *name;
		gint64 id = 0;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT ||
		    !int_member(entry, AXIS_ARR_PROFILE_ID, 1, AXIS_ARR_ID_MAX, &id))
			continue;
		name = text_member(entry, AXIS_ARR_PROFILE_NAME, AXIS_ARR_NAME_MAX_CHARS);
		if (name == NULL)
			continue;
		profile = g_new0(AxisArrProfile, 1);
		profile->id = id;
		profile->name = name;
		g_ptr_array_add(*profiles, profile);
	}
	return TRUE;
}

gboolean
axis_arr_parse_folders(
	const gchar  *data,
	gsize         length,
	GPtrArray   **folders,
	const gchar **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const AxisJsonNode *root;
	guint i;

	*folders = NULL;
	doc = parse_array(data, length, problem);
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	*folders = g_ptr_array_new_with_free_func((GDestroyNotify)axis_arr_folder_free);
	for (i = 0; i < axis_json_length(root) && (*folders)->len < AXIS_ARR_MAX_FOLDERS; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		g_autofree gchar *path = NULL;
		gchar *normal = NULL;
		AxisArrFolder *folder;

		if (axis_json_type(entry) != AXIS_JSON_OBJECT)
			continue;
		path = path_member(entry, AXIS_ARR_FOLDER_PATH);
		if (path == NULL || !axis_storage_path_normalize_exact(path, TRUE, &normal, NULL))
			continue;
		folder = g_new0(AxisArrFolder, 1);
		folder->path = normal;
		/* Not said: taken as reachable (it only words a note). */
		folder->accessible = TRUE;
		bool_member(entry, AXIS_ARR_FOLDER_ACCESSIBLE, &folder->accessible);
		g_ptr_array_add(*folders, folder);
	}
	return TRUE;
}

gboolean
axis_arr_parse_queue_page(
	const gchar  *data,
	gsize         length,
	AxisArrApp    app,
	GArray       *title_ids,
	guint        *total,
	guint        *n_records,
	const gchar **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	const gchar *member = app == AXIS_ARR_SONARR ? AXIS_ARR_QUEUE_SERIES : AXIS_ARR_QUEUE_MOVIE;
	const AxisJsonNode *root;
	const AxisJsonNode *records;
	gint64 count = 0;
	guint i;

	*total = 0;
	*n_records = 0;
	if (problem != NULL)
		*problem = "is not a page of a list";
	doc = data != NULL && length > 0 ? axis_json_parse(data, length, NULL) : NULL;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	records = axis_json_member(root, AXIS_ARR_PAGE_RECORDS);
	if (axis_json_type(root) != AXIS_JSON_OBJECT || axis_json_type(records) != AXIS_JSON_ARRAY ||
	    !int_member(root, AXIS_ARR_PAGE_TOTAL, 0, G_MAXINT32, &count))
		return FALSE;
	*total = (guint)count;
	*n_records = axis_json_length(records);
	for (i = 0; i < axis_json_length(records) && i < AXIS_ARR_MAX_PAGE_RECORDS; i++) {
		const AxisJsonNode *entry = axis_json_element(records, i);
		gint64 id = 0;

		if (axis_json_type(entry) == AXIS_JSON_OBJECT &&
		    int_member(entry, member, 1, AXIS_ARR_ID_MAX, &id))
			g_array_append_val(title_ids, id);
	}
	if (problem != NULL)
		*problem = NULL;
	return TRUE;
}

/* ── The address ────────────────────────────────────────────────── */

gboolean
axis_arr_address_split(
	const gchar  *typed,
	gchar       **origin,
	gchar       **prefix,
	const gchar **problem
){
	g_autofree gchar *text = typed != NULL ? g_strstrip(g_strdup(typed)) : NULL;
	g_autofree gchar *head = NULL;
	g_autofree gchar *normal = NULL;
	const gchar *why = NULL;
	const gchar *authority = NULL;
	const gchar *slash = NULL;
	const gchar *p;

	if (origin != NULL)
		*origin = NULL;
	if (prefix != NULL)
		*prefix = NULL;
	if (problem != NULL)
		*problem = NULL;
	if (text == NULL || text[0] == '\0') {
		why = "The address is missing.";
	} else if (strlen(text) > 512 || !g_utf8_validate(text, -1, NULL)) {
		why = "The address is too long.";
	} else if (g_ascii_strncasecmp(text, "http://", 7) != 0 && g_ascii_strncasecmp(text, "https://", 8) != 0) {
		why = "The address must start with http:// or https://";
	}
	if (why == NULL) {
		authority = strstr(text, "://") + 3;
		for (p = authority; *p != '\0' && *p != '/'; p++) {
			if (*p == '@') {
				why = "The address must not hold a user name or a password.";
				break;
			}
			if (*p == '?' || *p == '#' || (guchar)*p <= 0x20 || *p == '\\') {
				why = "The address must be a host with an optional port and path, without a query.";
				break;
			}
		}
	}
	if (why == NULL) {
		slash = strchr(authority, '/');
		if (authority[0] == '\0' || slash == authority)
			why = "The address has no host.";
		else if (slash != NULL && (strchr(slash, '?') != NULL || strchr(slash, '#') != NULL))
			why = "The address must not hold a query: only the host, an optional port and an optional path.";
		else if (!axis_download_base_path_normalize(slash != NULL ? slash : "", &normal, NULL))
			why = "The path after the host may hold only letters, digits, '.', '_', '~' and '-' between slashes.";
	}
	if (why == NULL) {
		head = slash != NULL ? g_strndup(text, (gsize)(slash - text)) : g_strdup(text);
		if (strlen(head) > 255)
			why = "The address is too long.";
	}
	if (why != NULL) {
		if (problem != NULL)
			*problem = why;
		return FALSE;
	}
	if (origin != NULL)
		*origin = g_ascii_strdown(head, -1);
	if (prefix != NULL)
		*prefix = g_strdup(normal != NULL && strcmp(normal, "/") != 0 ? normal : "");
	return TRUE;
}

/* ── Paths ──────────────────────────────────────────────────────── */

AxisArrPathResult
axis_arr_map_path(
	const GPtrArray  *maps,
	const gchar      *arr_path,
	gint64           *root_id,
	gchar           **stored
){
	g_autofree gchar *rel = NULL;
	AxisDownloadPathResult result;
	GString *out;

	*root_id = 0;
	*stored = NULL;
	if (arr_path == NULL || strlen(arr_path) > AXIS_ARR_PATH_MAX_BYTES)
		return AXIS_ARR_PATH_INVALID;
	result = axis_download_path_apply(maps, arr_path, root_id, &rel, NULL);
	if (result == AXIS_DOWNLOAD_PATH_UNMAPPED)
		return AXIS_ARR_PATH_UNMAPPED;
	if (result != AXIS_DOWNLOAD_PATH_MAPPED || rel == NULL) {
		*root_id = 0;
		return AXIS_ARR_PATH_INVALID;
	}
	out = g_string_new(NULL);
	axis_library_name_encode(out, rel, strlen(rel));
	*stored = g_string_free(out, FALSE);
	return AXIS_ARR_PATH_MAPPED;
}
