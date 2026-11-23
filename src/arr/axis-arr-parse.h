/*
 * axis-arr-parse.h - reading what Radarr and Sonarr answer, and the text
 * rules of a connection: its address, and its paths mapped to Axis's library
 * folders
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An answer of an arr app is outside input.  It is read here and nowhere
 * else, within stated limits:
 *
 *  - The list of movies is one array of every movie.  It is cut into its
 *    elements by a scanner that keeps no more than a position and a depth,
 *    and each movie is then read alone by the bounded JSON reader
 *    (probe/axis-probe-json.h), so the memory taken follows one movie's
 *    record and what is kept of each, never the size of the list.
 *  - A movie whose record cannot be read, is larger than
 *    %AXIS_ARR_MAX_ELEMENT_BYTES, or lacks a member Axis needs is kept as
 *    one that is not usable ("Radarr's answer for it is incomplete"); the
 *    others are read.  Only an answer that is not an array at all, or holds
 *    more than the limit, fails as a whole.
 *  - Every text is cleaned and cut to its limit; a path is kept byte for
 *    byte or not at all (a cleaned path would be another path); every number
 *    is checked for its range; a member of another type than expected counts
 *    as missing; unknown members are ignored.  Overviews, images, ratings,
 *    tags and the rest are never copied.
 *
 * Every member name and every value word is taken from axis-arr-assumed.h.
 *
 * GLib only, no I/O: fuzzed by tests/fuzz/fuzz-arr-parse.c.
 */

#ifndef AXIS_ARR_PARSE_H
#define AXIS_ARR_PARSE_H

#include <glib.h>

G_BEGIN_DECLS

/* A plan holds at most this many titles (design.md, section 11). */
#define AXIS_ARR_MAX_TITLES         100000

/* One title's record in the list, at most. */
#define AXIS_ARR_MAX_ELEMENT_BYTES  (2 * 1024 * 1024)

#define AXIS_ARR_MAX_PROFILES       500
#define AXIS_ARR_MAX_FOLDERS        500

/* Records of one page of a paged list, at most (Axis asks for 250). */
#define AXIS_ARR_MAX_PAGE_RECORDS   1000

#define AXIS_ARR_TITLE_MAX_CHARS    300
#define AXIS_ARR_NAME_MAX_CHARS     120
#define AXIS_ARR_VERSION_MAX_CHARS  32
#define AXIS_ARR_PATH_MAX_BYTES     4096
#define AXIS_ARR_ID_MAX             G_GINT64_CONSTANT(999999999999)

typedef enum
{
	AXIS_ARR_RADARR,
	AXIS_ARR_SONARR
} AxisArrApp;

/* "radarr", "sonarr": what the database stores. */
const gchar *
axis_arr_app_to_string(AxisArrApp app);

gboolean
axis_arr_app_from_string(
	const gchar *text,
	AxisArrApp  *app
);

/* "Radarr", "Sonarr": the app's name on the pages, and what its status
 * answer names itself. */
const gchar *
axis_arr_app_label(AxisArrApp app);

/* What the app holds, in words: "movie" or "movies" (Radarr), "series"
 * (Sonarr, one and many). */
const gchar *
axis_arr_app_noun(
	AxisArrApp app,
	gboolean   many
);

/**
 * axis_arr_parse_status:
 * @app: the app the connection is for
 * @version: (out) (transfer full) (optional): its version, digits, letters,
 *   '.' and '-' only, when the answer is that app's
 *
 * Returns: %TRUE only when the answer is an object that names @app
 */
gboolean
axis_arr_parse_status(
	const gchar  *data,
	gsize         length,
	AxisArrApp    app,
	gchar       **version
);

/**
 * AxisArrMovie:
 * @usable: the record held everything Axis needs; %FALSE: only what could be
 *   read is set (perhaps nothing), and the movie is listed as incomplete
 * @arr_id: Radarr's number of the movie, 0 when not read
 * @title: (nullable): cleaned
 * @year: 0 for none
 * @tmdb_id: 0 for none
 * @imdb_id: (nullable): "tt" and digits
 * @min_availability: (nullable): Axis's word for it (A7)
 * @profile_id: Radarr's quality profile
 * @path: (nullable): the movie's folder as Radarr sees it, byte for byte
 * @root_path: (nullable): its root folder
 * @has_file: Radarr says it has a file
 * @file_path: (nullable): the file's path as Radarr sees it
 * @file_size: its size in bytes, -1 when not given
 * @file_quality: (nullable): Radarr's name of its quality (A8)
 * @file_scene: (nullable): the release name it came from
 */
typedef struct
{
	gboolean  usable;
	gint64    arr_id;
	gchar    *title;
	gint      year;
	gint64    tmdb_id;
	gchar    *imdb_id;
	gboolean  monitored;
	gchar    *min_availability;
	gint64    profile_id;
	gchar    *path;
	gchar    *root_path;
	gboolean  has_file;
	gchar    *file_path;
	gint64    file_size;
	gchar    *file_quality;
	gchar    *file_scene;
} AxisArrMovie;

void
axis_arr_movie_free(AxisArrMovie *movie);

/**
 * axis_arr_parse_movies:
 * @max: the most movies read (%AXIS_ARR_MAX_TITLES)
 * @movies: (out) (transfer full) (element-type AxisArrMovie): every element
 *   of the list, in its order, the unusable ones too
 * @problem: (out) (optional) (transfer none): why the whole answer cannot be
 *   used, a fixed sentence fragment
 *
 * Returns: %TRUE when the answer is an array of at most @max elements
 */
gboolean
axis_arr_parse_movies(
	const gchar  *data,
	gsize         length,
	guint         max,
	GPtrArray   **movies,
	const gchar **problem
);

/* A quality profile of the app: its number and name. */
typedef struct
{
	gint64  id;
	gchar  *name;
} AxisArrProfile;

void
axis_arr_profile_free(AxisArrProfile *profile);

/* (element-type AxisArrProfile); entries out of shape are left out. */
gboolean
axis_arr_parse_profiles(
	const gchar  *data,
	gsize         length,
	GPtrArray   **profiles,
	const gchar **problem
);

/* A root folder of the app: its path in its one form, and whether the app
 * says it can reach it. */
typedef struct
{
	gchar    *path;
	gboolean  accessible;
} AxisArrFolder;

void
axis_arr_folder_free(AxisArrFolder *folder);

/* (element-type AxisArrFolder); a folder whose path is not an absolute path
 * Axis can map is left out. */
gboolean
axis_arr_parse_folders(
	const gchar  *data,
	gsize         length,
	GPtrArray   **folders,
	const gchar **problem
);

/**
 * axis_arr_parse_queue_page:
 * @app: whose queue it is: a record names a movie (Radarr) or a series
 *   (Sonarr)
 * @title_ids: (element-type gint64): the movies or series this page's records
 *   name are appended
 * @total: (out): how many records the app says the whole list has
 * @n_records: (out): how many records this page held (read or not)
 *
 * Returns: %TRUE when the answer is a page of the paged shape (A9)
 */
gboolean
axis_arr_parse_queue_page(
	const gchar  *data,
	gsize         length,
	AxisArrApp    app,
	GArray       *title_ids,
	guint        *total,
	guint        *n_records,
	const gchar **problem
);

/* ── Sonarr: series, their episodes and their files ─────────────── */

/* Seasons of one series, episodes and episode files of one series, at most. */
#define AXIS_ARR_MAX_SEASONS          2000
#define AXIS_ARR_MAX_SERIES_EPISODES  20000

#define AXIS_ARR_SEASON_MAX           10000
#define AXIS_ARR_EPISODE_MAX          100000
/* An absolute episode number, at most: what the metadata store keeps too. */
#define AXIS_ARR_ABSOLUTE_MAX         1000000

/* A season as Sonarr monitors it; 0 is the specials. */
typedef struct
{
	gint     number;
	gboolean monitored;
} AxisArrSeason;

/**
 * AxisArrSeries:
 * @usable: the record held everything Axis needs; %FALSE: only what could be
 *   read is set, and the series is listed as incomplete
 * @arr_id: Sonarr's number of the series, 0 when not read
 * @tvdb_id: 0 for none
 * @series_type: "standard", "daily" or "anime" (A11; compared without case;
 *   none given is "standard", a word that is none of them is %NULL: not said)
 * @monitor_new: 1 when Sonarr says it monitors seasons that come later, 0
 *   when it says it does not, -1 when it does not say (A13)
 * @seasons: (element-type AxisArrSeason): each season once, in Sonarr's order
 *
 * A series is usable only with its seasons: one whose list of seasons is
 * missing, has an entry out of shape or names a season twice is incomplete,
 * since its monitoring would have to be guessed.
 */
typedef struct
{
	gboolean     usable;
	gint64       arr_id;
	gchar       *title;
	gint         year;
	gint64       tvdb_id;
	gchar       *imdb_id;
	gboolean     monitored;
	const gchar *series_type;
	gint         monitor_new;
	gint64       profile_id;
	gchar       *path;
	gchar       *root_path;
	GArray      *seasons;
} AxisArrSeries;

void
axis_arr_series_free(AxisArrSeries *series);

/* As axis_arr_parse_movies(), for Sonarr's list of every series
 * (element-type AxisArrSeries). */
gboolean
axis_arr_parse_series(
	const gchar  *data,
	gsize         length,
	guint         max,
	GPtrArray   **series,
	const gchar **problem
);

/**
 * AxisArrEpisode:
 * @arr_id: Sonarr's number of the episode, 0 when not given
 * @tvdb_id: TheTVDB's id of it, 0 for none
 * @season, @number: its place (A15)
 * @air_date: "YYYY-MM-DD", or "" for none (or one that is no day)
 * @file_id: Sonarr's number of the file that holds it, 0 for none
 * @absolute: Sonarr's own absolute number of it (A20), 0 for none (absent,
 *   null, 0, not a whole number, or outside 1 to %AXIS_ARR_ABSOLUTE_MAX).
 *   Compared with TheTVDB's and reported; never used to match or link
 * @odd: its place could be read and its monitoring could not: only @season
 *   and @number are set.  It is no episode to bring: its place is kept so
 *   that the episode there is not monitored and not searched for.
 */
typedef struct
{
	gint64   arr_id;
	gint64   tvdb_id;
	gint     season;
	gint     number;
	gchar    air_date[11];
	gboolean monitored;
	gint64   file_id;
	gint     absolute;
	gboolean odd;
} AxisArrEpisode;

/**
 * axis_arr_parse_episodes:
 * @max: the most entries read (%AXIS_ARR_MAX_SERIES_EPISODES)
 * @episodes: (out) (transfer full) (element-type AxisArrEpisode): the
 *   episodes that could be read, in the answer's order
 * @n_odd: (out): entries left out: not a record, or without a season or a
 *   number.  (One with a place and without its monitoring is kept, marked
 *   odd, and not counted here.)
 *
 * The answer is cut into its entries as the list of movies is, and each is
 * read alone, so the memory taken follows one entry and the few numbers kept
 * of each.
 *
 * Returns: %TRUE when the answer is an array of at most @max entries
 */
gboolean
axis_arr_parse_episodes(
	const gchar  *data,
	gsize         length,
	guint         max,
	GArray      **episodes,
	guint        *n_odd,
	const gchar **problem
);

/**
 * AxisArrEpisodeFile:
 * @arr_id: Sonarr's number of the file (never 0)
 * @path: (nullable): its path as Sonarr sees it, byte for byte
 * @size: its size in bytes, -1 when not given
 * @quality: (nullable): Sonarr's name of its quality (A8)
 * @scene: (nullable): the release name it came from
 */
typedef struct
{
	gint64  arr_id;
	gchar  *path;
	gint64  size;
	gchar  *quality;
	gchar  *scene;
} AxisArrEpisodeFile;

void
axis_arr_episode_file_free(AxisArrEpisodeFile *file);

/* As axis_arr_parse_episodes(), for a series' episode files (element-type
 * AxisArrEpisodeFile); an entry without its number is left out and counted in
 * @n_odd, since no episode could name it. */
gboolean
axis_arr_parse_episode_files(
	const gchar  *data,
	gsize         length,
	guint         max,
	GPtrArray   **files,
	guint        *n_odd,
	const gchar **problem
);

/* The quality of Axis's ladder an arr quality name is taken as (A8), or
 * %NULL. */
const gchar *
axis_arr_quality_to_axis(const gchar *arr_name);

/* ── The address of a connection ────────────────────────────────── */

/**
 * axis_arr_address_split:
 * @typed: (nullable): what the administrator typed: "http://radarr:7878",
 *   "https://media.example/radarr/"
 * @origin: (out) (transfer full) (optional): scheme, host and port
 * @prefix: (out) (transfer full) (optional): the path the app is served
 *   under, "" for none, else "/" and plain segments without a trailing "/"
 * @problem: (out) (transfer none) (optional): why it is refused, a fixed
 *   sentence that holds none of @typed
 *
 * An http or https origin (a host name or an address literal, an optional
 * port), with an optional path of plain segments; no user information, no
 * query, no fragment.  Any host is accepted, private, loopback and
 * link-local ones included (design.md, section 12).
 *
 * Returns: %TRUE if @typed is acceptable
 */
gboolean
axis_arr_address_split(
	const gchar  *typed,
	gchar       **origin,
	gchar       **prefix,
	const gchar **problem
);

/* ── Paths of the app, in Axis ──────────────────────────────────── */

typedef enum
{
	AXIS_ARR_PATH_MAPPED,
	AXIS_ARR_PATH_UNMAPPED,		/* no mapped folder contains it */
	AXIS_ARR_PATH_INVALID		/* not a path that can be mapped at all */
} AxisArrPathResult;

/**
 * axis_arr_map_path:
 * @maps: (element-type AxisDownloadPathMap): the folder table: a prefix of
 *   the app's, the library folder it is (its id in the map's volume_id) and
 *   the path below that folder
 * @arr_path: (nullable): a path as the app gives it
 * @root_id: (out): the library folder, when mapped
 * @stored: (out) (transfer full): the path relative to the library folder in
 *   the form the index stores (library/axis-library-name.h), when mapped
 *
 * The longest prefix that contains @arr_path, by whole segments, is used
 * (download/axis-download-path.h does the text).  A relative path, one with
 * a "." or ".." segment, a backslash path, or one with a character a stored
 * path cannot have is invalid.
 */
AxisArrPathResult
axis_arr_map_path(
	const GPtrArray  *maps,
	const gchar      *arr_path,
	gint64           *root_id,
	gchar           **stored
);

G_END_DECLS

#endif /* AXIS_ARR_PARSE_H */
