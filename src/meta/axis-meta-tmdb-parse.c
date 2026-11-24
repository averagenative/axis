/*
 * axis-meta-tmdb-parse.c - TMDb's version 3 answers, read into the metadata
 * model
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-tmdb-parse.h"

#include <string.h>

#include "meta/axis-meta-json.h"

/* How many region entries and dates of one film are looked at. */
#define MAX_RELEASE_ENTRIES 1000

/* The longest file name of an image (before its extension). */
#define IMAGE_NAME_MAX 100

/* The shared readers (meta/axis-meta-json.h), under this file's names. */
#define text_member axis_meta_json_text
#define int_member  axis_meta_json_int
#define date_member axis_meta_json_date
#define year_of     axis_meta_json_year_of

static gboolean
unreadable(
	GError      **error,
	const gchar  *why
){
	return axis_meta_json_unreadable("TMDb", error, why);
}

static AxisJsonDoc *
parse(
	const gchar  *data,
	gsize         length,
	GError      **error
){
	return axis_meta_json_parse("TMDb", data, length, error);
}

/* ── Fields ─────────────────────────────────────────────────────── */

static gchar *
image_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const gchar *text = axis_json_string(axis_json_member(object, name));

	/* A full address, or any other form, is no image. */
	return axis_meta_tmdb_image_ref_valid(text) ? g_strdup(text) : NULL;
}

/* TMDb's identifiers are numbers. */
static gchar *
id_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const AxisJsonNode *node = axis_json_member(object, name);
	const gchar *text;

	if (axis_json_type(node) != AXIS_JSON_NUMBER)
		return NULL;
	text = axis_json_scalar_text(node);
	return axis_meta_id_valid(AXIS_META_TMDB_NAME, text) ? g_strdup(text) : NULL;
}

/* ── Status ─────────────────────────────────────────────────────── */

/* TMDb's words; anything else, or none, is unknown. */
static AxisMetaStatus
status_of(
	AxisMetaKind kind,
	const gchar *text
){
	if (text == NULL)
		return AXIS_META_STATUS_UNKNOWN;
	if (kind == AXIS_META_KIND_FILM) {
		if (strcmp(text, "Released") == 0)
			return AXIS_META_STATUS_RELEASED;
		if (strcmp(text, "Rumored") == 0 || strcmp(text, "Planned") == 0 || strcmp(text, "In Production") == 0 ||
		    strcmp(text, "Post Production") == 0)
			return AXIS_META_STATUS_UPCOMING;
		return AXIS_META_STATUS_UNKNOWN;
	}
	if (strcmp(text, "Returning Series") == 0)
		return AXIS_META_STATUS_CONTINUING;
	if (strcmp(text, "Ended") == 0 || strcmp(text, "Canceled") == 0)
		return AXIS_META_STATUS_ENDED;
	if (strcmp(text, "Planned") == 0 || strcmp(text, "In Production") == 0 || strcmp(text, "Pilot") == 0)
		return AXIS_META_STATUS_UPCOMING;
	return AXIS_META_STATUS_UNKNOWN;
}

/* ── Release dates ──────────────────────────────────────────────── */

/* The earliest theatrical (2 limited, 3 wide), digital (4) or physical (5)
 * date TMDb lists for @region (@theatrical: theatrical only), or %NULL. */
static gchar *
regional_release(
	const AxisJsonNode *node,
	const gchar        *region,
	gboolean            theatrical
){
	const AxisJsonNode *countries = axis_json_member(axis_json_member(node, "release_dates"), "results");
	gchar *best = NULL;
	guint i;

	if (region == NULL || axis_json_type(countries) != AXIS_JSON_ARRAY)
		return NULL;
	for (i = 0; i < axis_json_length(countries) && i < MAX_RELEASE_ENTRIES; i++) {
		const AxisJsonNode *country = axis_json_element(countries, i);
		const AxisJsonNode *dates;
		guint j;

		if (g_strcmp0(axis_json_string(axis_json_member(country, "iso_3166_1")), region) != 0)
			continue;
		dates = axis_json_member(country, "release_dates");
		for (j = 0; axis_json_type(dates) == AXIS_JSON_ARRAY && j < axis_json_length(dates) && j < MAX_RELEASE_ENTRIES;
		     j++) {
			const AxisJsonNode *entry = axis_json_element(dates, j);
			gint type = int_member(entry, "type", 1, 6);
			gchar *date;

			if (type < 2 || type > (theatrical ? 3 : 5))
				continue;
			date = date_member(entry, "release_date");
			if (date == NULL)
				continue;
			if (best == NULL || strcmp(date, best) < 0) {
				g_free(best);
				best = date;
			} else {
				g_free(date);
			}
		}
	}
	return best;
}

/* ── Details ────────────────────────────────────────────────────── */

static AxisMetaSeason *
season_summary(const AxisJsonNode *node)
{
	gint64 number = -1;
	AxisMetaSeason *season;
	const AxisJsonNode *number_node = axis_json_member(node, "season_number");
	gint64 count = -1;

	if (axis_json_type(node) != AXIS_JSON_OBJECT || axis_json_type(number_node) != AXIS_JSON_NUMBER ||
	    !axis_json_get_integer(number_node, 0, 10000, &number))
		return NULL;
	season = axis_meta_season_new((gint)number);
	season->name = text_member(node, "name", AXIS_META_NAME_MAX_CHARS);
	season->air_date = date_member(node, "air_date");
	season->overview = text_member(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
	if (axis_json_type(axis_json_member(node, "episode_count")) == AXIS_JSON_NUMBER &&
	    axis_json_get_integer(axis_json_member(node, "episode_count"), 0, 100000, &count))
		season->episode_count = (gint)count;
	return season;
}

static void
read_genres(
	const AxisJsonNode *node,
	AxisMetaTitle      *title
){
	const AxisJsonNode *list = axis_json_member(node, "genres");
	guint i;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return;
	title->genres = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(list) && title->genres->len < AXIS_META_MAX_GENRES; i++) {
		gchar *genre = text_member(axis_json_element(list, i), "name", AXIS_META_GENRE_MAX_CHARS);

		if (genre != NULL)
			g_ptr_array_add(title->genres, genre);
	}
}

/* Films list their other names under "titles", series under "results". */
static void
read_alt_titles(
	const AxisJsonNode *node,
	AxisMetaTitle      *title
){
	const AxisJsonNode *alt = axis_json_member(node, "alternative_titles");
	const AxisJsonNode *list = axis_json_member(alt, title->kind == AXIS_META_KIND_FILM ? "titles" : "results");
	guint i;
	guint j;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return;
	title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
	for (i = 0; i < axis_json_length(list) && title->alt_titles->len < AXIS_META_MAX_ALT_TITLES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		gchar *name = text_member(entry, "title", AXIS_META_TITLE_MAX_CHARS);
		const gchar *country = axis_json_string(axis_json_member(entry, "iso_3166_1"));
		gboolean twice = FALSE;
		AxisMetaAltTitle *other;

		if (name == NULL)
			continue;
		if (!axis_meta_country_valid(country))
			country = NULL;
		for (j = 0; j < title->alt_titles->len && !twice; j++) {
			other = g_ptr_array_index(title->alt_titles, j);
			twice = strcmp(other->title, name) == 0 && g_strcmp0(other->country, country) == 0;
		}
		if (twice) {
			g_free(name);
			continue;
		}
		other = g_new0(AxisMetaAltTitle, 1);
		other->title = name;
		other->country = g_strdup(country);
		g_ptr_array_add(title->alt_titles, other);
	}
}

static void
read_external_ids(
	const AxisJsonNode *node,
	AxisMetaTitle      *title
){
	const AxisJsonNode *external = axis_json_member(node, "external_ids");
	const AxisJsonNode *tvdb = axis_json_member(external, "tvdb_id");

	axis_meta_title_add_id(title, "imdb", axis_json_string(axis_json_member(external, "imdb_id")));
	if (axis_json_type(tvdb) == AXIS_JSON_NUMBER)
		axis_meta_title_add_id(title, "tvdb", axis_json_scalar_text(tvdb));
	/* A film also names its IMDb identifier itself. */
	if (axis_meta_title_get_id(title, "imdb") == NULL)
		axis_meta_title_add_id(title, "imdb", axis_json_string(axis_json_member(node, "imdb_id")));
}

static void
read_seasons(
	const AxisJsonNode *node,
	AxisMetaTitle      *title
){
	const AxisJsonNode *list = axis_json_member(node, "seasons");
	guint i;
	guint j;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return;
	title->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	for (i = 0; i < axis_json_length(list) && title->seasons->len < AXIS_META_MAX_SEASONS; i++) {
		AxisMetaSeason *season = season_summary(axis_json_element(list, i));
		gboolean twice = FALSE;

		if (season == NULL)
			continue;
		for (j = 0; j < title->seasons->len && !twice; j++)
			twice = ((AxisMetaSeason *)g_ptr_array_index(title->seasons, j))->number == season->number;
		if (twice) {
			axis_meta_season_free(season);
			continue;
		}
		g_ptr_array_add(title->seasons, season);
	}
}

/* The first running time TMDb gives for an episode of a series. */
static gint
episode_runtime(const AxisJsonNode *node)
{
	const AxisJsonNode *list = axis_json_member(node, "episode_run_time");
	guint i;

	for (i = 0; axis_json_type(list) == AXIS_JSON_ARRAY && i < axis_json_length(list) && i < 20; i++) {
		const AxisJsonNode *item = axis_json_element(list, i);
		gint64 value = 0;

		if (axis_json_type(item) == AXIS_JSON_NUMBER && axis_json_get_integer(item, 1, 10000, &value))
			return (gint)value;
	}
	return 0;
}

/**
 * title_from:
 *
 * One title from @node, or %NULL when it lacks what makes it one (an
 * object, a TMDb identifier, a name), is of the other kind, or is adult and
 * @adult is not set (a search; a title asked for by its identifier is not
 * judged).  @detail: the full record, with what TMDb appends.
 */
static AxisMetaTitle *
title_from(
	AxisMetaKind        kind,
	const AxisJsonNode *node,
	gboolean            detail,
	gboolean            adult,
	const gchar        *region
){
	const gchar *name_member = kind == AXIS_META_KIND_FILM ? "title" : "name";
	const gchar *original_member = kind == AXIS_META_KIND_FILM ? "original_title" : "original_name";
	g_autofree gchar *id = NULL;
	const gchar *media;
	const gchar *language;
	gboolean adult_flag = FALSE;
	AxisMetaTitle *title;
	gchar *release;

	if (axis_json_type(node) != AXIS_JSON_OBJECT)
		return NULL;
	media = axis_json_string(axis_json_member(node, "media_type"));
	if (media != NULL && strcmp(media, kind == AXIS_META_KIND_FILM ? "movie" : "tv") != 0)
		return NULL;
	if (!axis_json_get_boolean(axis_json_member(node, "adult"), &adult_flag))
		adult_flag = FALSE;
	if (!detail && !adult && adult_flag)
		return NULL;
	id = id_member(node, "id");
	if (id == NULL)
		return NULL;
	title = axis_meta_title_new(kind);
	/* Kept with the record however the title came to be stored. */
	title->adult = adult_flag;
	title->provider = g_strdup(AXIS_META_TMDB_NAME);
	title->provider_id = g_strdup(id);
	title->detail = detail;
	axis_meta_title_add_id(title, AXIS_META_TMDB_NAME, id);
	title->title = text_member(node, name_member, AXIS_META_TITLE_MAX_CHARS);
	title->original_title = text_member(node, original_member, AXIS_META_TITLE_MAX_CHARS);
	if (title->title == NULL) {
		/* TMDb names every title; without a name use the original. */
		title->title = g_strdup(title->original_title);
	}
	if (title->title == NULL) {
		axis_meta_title_free(title);
		return NULL;
	}
	title->overview = text_member(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
	language = axis_json_string(axis_json_member(node, "original_language"));
	if (axis_meta_language_valid(language))
		title->original_language = g_strdup(language);
	title->poster_ref = image_member(node, "poster_path");
	title->backdrop_ref = image_member(node, "backdrop_path");

	if (kind == AXIS_META_KIND_FILM) {
		/* TMDb's own date gives the year, here and in a search alike. */
		release = date_member(node, "release_date");
		title->year = year_of(release);
		if (detail) {
			gchar *regional = regional_release(node, region, FALSE);
			g_autofree gchar *theatrical = regional_release(node, region, TRUE);
			gint in_region = year_of(theatrical);

			/* The year of its theatrical release in the region counts as one
			 * of its years only near its first release: a disc decades later
			 * is not a year it came out in. */
			if (in_region != 0 && title->year != 0 && in_region != title->year && ABS(in_region - title->year) <= 2)
				title->regional_year = in_region;
			if (regional != NULL) {
				g_free(release);
				release = regional;
			}
		}
		title->release_date = release;
		if (title->year == 0)
			title->year = year_of(title->release_date);
	} else {
		title->first_air_date = date_member(node, "first_air_date");
		title->year = year_of(title->first_air_date);
	}
	if (!detail)
		return title;

	title->status = status_of(kind, axis_json_string(axis_json_member(node, "status")));
	if (kind == AXIS_META_KIND_FILM) {
		title->runtime = int_member(node, "runtime", 1, 10000);
	} else {
		title->last_air_date = date_member(node, "last_air_date");
		title->runtime = episode_runtime(node);
		read_seasons(node, title);
	}
	read_genres(node, title);
	read_alt_titles(node, title);
	read_external_ids(node, title);
	return title;
}

/* ── Calls ──────────────────────────────────────────────────────── */

/* Whether a title with this TMDb id is in @out already. */
static gboolean
listed(
	GPtrArray   *out,
	const gchar *id
){
	guint i;

	for (i = 0; i < out->len; i++) {
		if (strcmp(((AxisMetaTitle *)g_ptr_array_index(out, i))->provider_id, id) == 0)
			return TRUE;
	}
	return FALSE;
}

static GPtrArray *
read_list(
	AxisMetaKind        kind,
	const AxisJsonNode *list,
	gboolean            adult
){
	GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	guint i;

	for (i = 0; i < axis_json_length(list) && out->len < AXIS_META_MAX_RESULTS; i++) {
		AxisMetaTitle *title = title_from(kind, axis_json_element(list, i), FALSE, adult, NULL);

		if (title == NULL)
			continue;
		if (listed(out, title->provider_id)) {
			axis_meta_title_free(title);
			continue;
		}
		g_ptr_array_add(out, title);
	}
	return out;
}

gboolean
axis_meta_tmdb_parse_search(
	AxisMetaKind   kind,
	const gchar   *data,
	gsize          length,
	gboolean       adult,
	GPtrArray    **titles,
	GError       **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;

	*titles = NULL;
	if (doc == NULL)
		return FALSE;
	list = axis_json_member(axis_json_doc_get_root(doc), "results");
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of results");
	*titles = read_list(kind, list, adult);
	return TRUE;
}

gboolean
axis_meta_tmdb_parse_find(
	AxisMetaKind   kind,
	const gchar   *scheme,
	const gchar   *value,
	const gchar   *data,
	gsize          length,
	gboolean       adult,
	GPtrArray    **titles,
	GError       **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;

	*titles = NULL;
	if (doc == NULL)
		return FALSE;
	list = axis_json_member(axis_json_doc_get_root(doc), kind == AXIS_META_KIND_FILM ? "movie_results" : "tv_results");
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of results");
	*titles = read_list(kind, list, adult);
	/* What was asked for is known to belong to the one title that came back
	 * (not to a list of them). */
	if ((*titles)->len == 1 && scheme != NULL && value != NULL)
		axis_meta_title_add_id(g_ptr_array_index(*titles, 0), scheme, value);
	return TRUE;
}

gboolean
axis_meta_tmdb_parse_title(
	AxisMetaKind    kind,
	const gchar    *region,
	const gchar    *data,
	gsize           length,
	AxisMetaTitle **title,
	GError        **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);

	*title = NULL;
	if (doc == NULL)
		return FALSE;
	*title = title_from(kind, axis_json_doc_get_root(doc), TRUE, TRUE,
		axis_meta_tmdb_region_valid(region) ? region : NULL);
	if (*title == NULL)
		return unreadable(error, "it is not a title of the kind asked for");
	return TRUE;
}

gboolean
axis_meta_tmdb_parse_season(
	gint             number,
	const gchar     *data,
	gsize            length,
	AxisMetaSeason **season_out,
	GError         **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *root;
	const AxisJsonNode *list;
	AxisMetaSeason *season;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GHashTable) ids = NULL;
	guint i;

	*season_out = NULL;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	season = season_summary(root);
	if (season == NULL || season->number != number) {
		if (season != NULL)
			axis_meta_season_free(season);
		return unreadable(error, "it is not the season asked for");
	}
	list = axis_json_member(root, "episodes");
	if (axis_json_type(list) != AXIS_JSON_ARRAY) {
		axis_meta_season_free(season);
		return unreadable(error, "it has no list of episodes");
	}
	season->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_episode_free);
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < axis_json_length(list) && season->episodes->len < AXIS_META_MAX_EPISODES; i++) {
		const AxisJsonNode *node = axis_json_element(list, i);
		const AxisJsonNode *number_node = axis_json_member(node, "episode_number");
		const AxisJsonNode *id_node = axis_json_member(node, "id");
		gint64 episode_number = -1;
		gint64 episode_id = 0;
		AxisMetaEpisode *episode;

		if (axis_json_type(node) != AXIS_JSON_OBJECT || axis_json_type(number_node) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(number_node, 0, 100000, &episode_number) ||
		    !g_hash_table_add(seen, GINT_TO_POINTER((gint)episode_number + 1)))
			continue;
		episode = axis_meta_episode_new();
		episode->season_number = season->number;
		episode->number = (gint)episode_number;
		episode->name = text_member(node, "name", AXIS_META_NAME_MAX_CHARS);
		episode->air_date = date_member(node, "air_date");
		episode->overview = text_member(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
		episode->runtime = int_member(node, "runtime", 1, 10000);
		/* TMDb's own id of the episode stays when the episode is numbered
		 * anew; one listed twice is kept by neither but the first. */
		if (axis_json_type(id_node) == AXIS_JSON_NUMBER &&
		    axis_json_get_integer(id_node, 1, G_GINT64_CONSTANT(999999999999), &episode_id)) {
			gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, episode_id);

			if (g_hash_table_contains(ids, id)) {
				g_free(id);
			} else {
				episode->provider_id = g_strdup(id);
				g_hash_table_add(ids, id);
			}
		}
		g_ptr_array_add(season->episodes, episode);
	}
	season->episode_count = (gint)season->episodes->len;
	*season_out = season;
	return TRUE;
}

/* ── Lists ──────────────────────────────────────────────────────── */

static const gchar *
media_of_kind(AxisMetaKind kind)
{
	return kind == AXIS_META_KIND_FILM ? "movie" : "tv";
}

gchar *
axis_meta_tmdb_list_path(
	AxisMetaKind  kind,
	AxisMetaList  list,
	const gchar  *id
){
	const gchar *media;

	if (kind != AXIS_META_KIND_FILM && kind != AXIS_META_KIND_SERIES)
		return NULL;
	media = media_of_kind(kind);
	if ((list == AXIS_META_LIST_RECOMMENDED) != (id != NULL))
		return NULL;
	switch (list) {
	case AXIS_META_LIST_POPULAR:
		return g_strdup_printf("/3/%s/popular", media);
	case AXIS_META_LIST_TRENDING:
		return g_strdup_printf("/3/trending/%s/week", media);
	case AXIS_META_LIST_NOW:
		return g_strdup_printf("/3/%s/%s", media, kind == AXIS_META_KIND_FILM ? "now_playing" : "on_the_air");
	case AXIS_META_LIST_UPCOMING:
		/* TMDb has no such list of series. */
		return kind == AXIS_META_KIND_FILM ? g_strdup("/3/movie/upcoming") : NULL;
	case AXIS_META_LIST_TOP_RATED:
		return g_strdup_printf("/3/%s/top_rated", media);
	case AXIS_META_LIST_RECOMMENDED:
		if (!axis_meta_id_valid(AXIS_META_TMDB_NAME, id))
			return NULL;
		return g_strdup_printf("/3/%s/%s/recommendations", media, id);
	default:
		return NULL;
	}
}

gboolean
axis_meta_tmdb_list_regional(
	AxisMetaKind kind,
	AxisMetaList list
){
	return kind == AXIS_META_KIND_FILM && list != AXIS_META_LIST_TRENDING && list != AXIS_META_LIST_RECOMMENDED;
}

/* The genre numbers of an entry: whole numbers 1 and up, each once. */
static void
read_genre_ids(
	const AxisJsonNode *node,
	GArray             *out
){
	const AxisJsonNode *list = axis_json_member(node, "genre_ids");
	guint i;
	guint j;

	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return;
	for (i = 0; i < axis_json_length(list) && out->len < AXIS_META_MAX_GENRES && i < 1000; i++) {
		const AxisJsonNode *item = axis_json_element(list, i);
		gint64 value = 0;
		gboolean twice = FALSE;
		gint id;

		if (axis_json_type(item) != AXIS_JSON_NUMBER || !axis_json_get_integer(item, 1, G_MAXINT32, &value))
			continue;
		id = (gint)value;
		for (j = 0; j < out->len && !twice; j++)
			twice = g_array_index(out, gint, j) == id;
		if (!twice)
			g_array_append_val(out, id);
	}
}

/* TMDb's vote_average (0 to 10) in tenths, when somebody voted. */
static void
read_rating(
	const AxisJsonNode *node,
	AxisMetaListEntry  *entry
){
	const AxisJsonNode *average = axis_json_member(node, "vote_average");
	gdouble value = 0.0;

	entry->votes = int_member(node, "vote_count", 0, G_MAXINT32);
	entry->rating = -1;
	if (entry->votes > 0 && axis_json_type(average) == AXIS_JSON_NUMBER &&
	    axis_json_get_double(average, 0.0, 10.0, &value))
		entry->rating = MIN(100, (gint)(value * 10.0 + 0.5));
}

gboolean
axis_meta_tmdb_parse_list(
	AxisMetaKind        kind,
	const gchar        *data,
	gsize               length,
	gboolean            adult,
	AxisMetaListPage  **page_out,
	GError            **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *root;
	const AxisJsonNode *list;
	AxisMetaListPage *page;
	guint i;

	*page_out = NULL;
	if (doc == NULL)
		return FALSE;
	root = axis_json_doc_get_root(doc);
	list = axis_json_member(root, "results");
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of results");
	page = axis_meta_list_page_new();
	page->total_pages = (guint)MIN(int_member(root, "total_pages", 0, G_MAXINT32), AXIS_META_LIST_MAX_PAGE);
	for (i = 0; i < axis_json_length(list) && page->entries->len < AXIS_META_MAX_RESULTS; i++) {
		const AxisJsonNode *node = axis_json_element(list, i);
		AxisMetaTitle *title = title_from(kind, node, FALSE, adult, NULL);
		AxisMetaListEntry *entry;
		guint j;
		gboolean twice = FALSE;

		if (title == NULL)
			continue;
		for (j = 0; j < page->entries->len && !twice; j++) {
			const AxisMetaListEntry *other = g_ptr_array_index(page->entries, j);

			twice = strcmp(other->title->provider_id, title->provider_id) == 0;
		}
		if (twice) {
			axis_meta_title_free(title);
			continue;
		}
		entry = axis_meta_list_entry_new(title);
		read_rating(node, entry);
		read_genre_ids(node, entry->genre_ids);
		g_ptr_array_add(page->entries, entry);
	}
	*page_out = page;
	return TRUE;
}

gboolean
axis_meta_tmdb_parse_genres(
	const gchar  *data,
	gsize         length,
	GPtrArray   **genres_out,
	GError      **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;
	GPtrArray *genres;
	guint i;
	guint j;

	*genres_out = NULL;
	if (doc == NULL)
		return FALSE;
	list = axis_json_member(axis_json_doc_get_root(doc), "genres");
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of genres");
	genres = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_genre_free);
	for (i = 0; i < axis_json_length(list) && genres->len < AXIS_META_MAX_GENRE_LIST; i++) {
		const AxisJsonNode *node = axis_json_element(list, i);
		const AxisJsonNode *id_node = axis_json_member(node, "id");
		gint64 id = 0;
		gchar *name;
		gboolean twice = FALSE;
		AxisMetaGenre *genre;

		if (axis_json_type(id_node) != AXIS_JSON_NUMBER || !axis_json_get_integer(id_node, 1, G_MAXINT32, &id))
			continue;
		for (j = 0; j < genres->len && !twice; j++)
			twice = ((const AxisMetaGenre *)g_ptr_array_index(genres, j))->id == (gint)id;
		if (twice)
			continue;
		name = text_member(node, "name", AXIS_META_GENRE_MAX_CHARS);
		if (name == NULL)
			continue;
		genre = g_new0(AxisMetaGenre, 1);
		genre->id = (gint)id;
		genre->name = name;
		g_ptr_array_add(genres, genre);
	}
	*genres_out = genres;
	return TRUE;
}

/* ── TMDb's own forms ───────────────────────────────────────────── */

gboolean
axis_meta_tmdb_image_ref_valid(const gchar *ref)
{
	const gchar *dot;
	gsize n;
	gsize i;

	if (ref == NULL || ref[0] != '/')
		return FALSE;
	dot = strrchr(ref, '.');
	if (dot == NULL)
		return FALSE;
	n = (gsize)(dot - ref) - 1;
	if (n < 1 || n > IMAGE_NAME_MAX)
		return FALSE;
	for (i = 1; i <= n; i++) {
		if (!g_ascii_isalnum(ref[i]))
			return FALSE;
	}
	return (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".png") == 0 || strcmp(dot, ".webp") == 0) &&
		axis_meta_image_ref_valid(ref);
}

gchar *
axis_meta_tmdb_image_path(
	const gchar     *ref,
	AxisMetaArtSize  size
){
	static const gchar *const widths[AXIS_META_ART_N_SIZES] = { "w342", "w780", "w1280" };

	if ((guint)size >= AXIS_META_ART_N_SIZES || !axis_meta_tmdb_image_ref_valid(ref))
		return NULL;
	return g_strdup_printf("/t/p/%s%s", widths[size], ref);
}

gboolean
axis_meta_tmdb_key_valid(
	const gchar  *text,
	gchar       **reason
){
	const gchar *p;
	gsize n = 0;
	gboolean hex = TRUE;

	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f) {
			*reason = g_strdup("must be the token as TMDb gives it: letters, digits and punctuation, without spaces");
			return FALSE;
		}
		hex = hex && g_ascii_isxdigit(*p);
		n++;
	}
	if (hex && n == 32) {
		*reason = g_strdup("looks like TMDb's short \"API Key\", which Axis does not use: paste the \"API Read Access "
			"Token\" instead, the much longer value (it starts with \"eyJ\") on the same API page of your TMDb account "
			"settings");
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_meta_tmdb_language_valid(const gchar *text)
{
	gsize n;
	gsize i;

	if (text == NULL)
		return FALSE;
	n = strlen(text);
	if (n != 2 && n != 3 && n != 5 && n != 6)
		return FALSE;
	for (i = 0; i < n; i++) {
		gboolean lower_part = n == 2 || n == 3 || (n == 5 && i < 2) || (n == 6 && i < 3);
		gboolean hyphen = (n == 5 && i == 2) || (n == 6 && i == 3);

		if (hyphen) {
			if (text[i] != '-')
				return FALSE;
		} else if (lower_part) {
			if (!g_ascii_islower(text[i]))
				return FALSE;
		} else if (!g_ascii_isupper(text[i])) {
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
axis_meta_tmdb_region_valid(const gchar *text)
{
	return axis_meta_country_valid(text);
}
