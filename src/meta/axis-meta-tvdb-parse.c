/*
 * axis-meta-tvdb-parse.c - TheTVDB's API version 4 answers, read into the
 * metadata model
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "meta/axis-meta-tvdb-parse.h"

#include <string.h>

#include "meta/axis-meta-json.h"
#include "meta/axis-meta-tvdb-assumed.h"

#define LABEL "TheTVDB"

/* How many entries of one list are looked at, whatever the answer holds. */
#define MAX_ENTRIES 5000

/* ── Orders ─────────────────────────────────────────────────────── */

static const struct
{
	const gchar *type;
	const gchar *source;
} orders[] = {
	{ AXIS_META_TVDB_AIRED_ORDER, AXIS_META_TVDB_NAME },
	{ "dvd", "tvdb-dvd" },
	{ "alternate", "tvdb-alternate" },
	{ "regional", "tvdb-regional" },
	{ "altdvd", "tvdb-altdvd" },
	{ "alttwo", "tvdb-alttwo" }
};

static const gchar *const sources[] = {
	AXIS_META_TVDB_NAME, "tvdb-dvd", "tvdb-alternate", "tvdb-regional", "tvdb-altdvd", "tvdb-alttwo", NULL
};

const gchar *
axis_meta_tvdb_source_of_order(const gchar *type)
{
	guint i;

	for (i = 0; type != NULL && i < G_N_ELEMENTS(orders); i++) {
		if (strcmp(orders[i].type, type) == 0)
			return orders[i].source;
	}
	return NULL;
}

const gchar *
axis_meta_tvdb_order_of_source(const gchar *source)
{
	guint i;

	for (i = 0; source != NULL && i < G_N_ELEMENTS(orders); i++) {
		if (strcmp(orders[i].source, source) == 0)
			return orders[i].type;
	}
	return NULL;
}

const gchar *const *
axis_meta_tvdb_sources(void)
{
	return sources;
}

/* ── Reading ────────────────────────────────────────────────────── */

static gboolean
unreadable(
	GError      **error,
	const gchar  *why
){
	return axis_meta_json_unreadable(LABEL, error, why);
}

/* The answer, when it is an object that says it succeeded (assumed 8). */
static AxisJsonDoc *
parse(
	const gchar  *data,
	gsize         length,
	GError      **error
){
	AxisJsonDoc *doc = axis_meta_json_parse(LABEL, data, length, error);

	if (doc == NULL)
		return NULL;
	if (g_strcmp0(axis_json_string(axis_json_member(axis_json_doc_get_root(doc), "status")),
	        AXIS_META_TVDB_STATUS_SUCCESS) != 0) {
		axis_json_doc_free(doc);
		unreadable(error, "it does not say it succeeded");
		return NULL;
	}
	return doc;
}

static const AxisJsonNode *
payload(AxisJsonDoc *doc)
{
	return axis_json_member(axis_json_doc_get_root(doc), "data");
}

/* TheTVDB's own identifier of a series or an episode: a whole number (or,
 * in a search, its digits as a string). */
static gchar *
id_of(const AxisJsonNode *node)
{
	const gchar *text;

	if (axis_json_type(node) == AXIS_JSON_NUMBER)
		text = axis_json_scalar_text(node);
	else if (axis_json_type(node) == AXIS_JSON_STRING)
		text = axis_json_string(node);
	else
		return NULL;
	return axis_meta_id_valid(AXIS_META_TVDB_NAME, text) ? g_strdup(text) : NULL;
}

static gchar *
image_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	return axis_meta_tvdb_image_ref(axis_json_string(axis_json_member(object, name)));
}

static AxisMetaStatus
status_of(const gchar *text)
{
	if (g_strcmp0(text, AXIS_META_TVDB_STATUS_CONTINUING) == 0)
		return AXIS_META_STATUS_CONTINUING;
	if (g_strcmp0(text, AXIS_META_TVDB_STATUS_ENDED) == 0)
		return AXIS_META_STATUS_ENDED;
	if (g_strcmp0(text, AXIS_META_TVDB_STATUS_UPCOMING) == 0)
		return AXIS_META_STATUS_UPCOMING;
	return AXIS_META_STATUS_UNKNOWN;
}

/* A year given as a number or as four digits of text. */
static gint
year_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const AxisJsonNode *node = axis_json_member(object, name);
	gint64 value = 0;

	if (axis_json_type(node) == AXIS_JSON_NUMBER && axis_json_get_integer(node, AXIS_META_YEAR_MIN,
	        AXIS_META_YEAR_MAX, &value))
		return (gint)value;
	if (axis_json_type(node) == AXIS_JSON_STRING) {
		const gchar *text = axis_json_string(node);

		if (text != NULL && strlen(text) == 4 && g_ascii_isdigit(text[0]) && g_ascii_isdigit(text[1]) &&
		    g_ascii_isdigit(text[2]) && g_ascii_isdigit(text[3]))
			return axis_meta_json_year_of(text);
	}
	return 0;
}

/* The text under @language of an object keyed by language (a search's
 * "translations" and "overviews"). */
static gchar *
keyed_text(
	const AxisJsonNode *object,
	const gchar        *name,
	const gchar        *language,
	guint               max_chars
){
	const AxisJsonNode *map = axis_json_member(object, name);

	if (language == NULL || axis_json_type(map) != AXIS_JSON_OBJECT)
		return NULL;
	return axis_meta_json_text(map, language, max_chars);
}

/* The entry of a list of {<field>, language} for @language. */
static gchar *
listed_text(
	const AxisJsonNode *list,
	const gchar        *field,
	const gchar        *language,
	guint               max_chars
){
	guint i;

	for (i = 0; language != NULL && axis_json_type(list) == AXIS_JSON_ARRAY && i < axis_json_length(list) &&
	     i < MAX_ENTRIES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);

		if (g_strcmp0(axis_json_string(axis_json_member(entry, "language")), language) == 0)
			return axis_meta_json_text(entry, field, max_chars);
	}
	return NULL;
}

static void
add_alt(
	AxisMetaTitle *title,
	gchar         *name,
	const gchar   *language
){
	AxisMetaAltTitle *alt;
	guint i;

	if (name == NULL)
		return;
	if (title->alt_titles == NULL)
		title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
	if (!axis_meta_language_valid(language))
		language = NULL;
	for (i = 0; i < title->alt_titles->len; i++) {
		const AxisMetaAltTitle *other = g_ptr_array_index(title->alt_titles, i);

		if (strcmp(other->title, name) == 0 && g_strcmp0(other->language, language) == 0) {
			g_free(name);
			return;
		}
	}
	if (title->alt_titles->len >= AXIS_META_MAX_ALT_TITLES) {
		g_free(name);
		return;
	}
	alt = g_new0(AxisMetaAltTitle, 1);
	alt->title = name;
	alt->language = g_strdup(language);
	g_ptr_array_add(title->alt_titles, alt);
}

/* The other schemes' identifiers a record names (assumed 3). */
static void
read_remote_ids(
	const AxisJsonNode *list,
	AxisMetaTitle      *title
){
	guint i;

	for (i = 0; axis_json_type(list) == AXIS_JSON_ARRAY && i < axis_json_length(list) && i < MAX_ENTRIES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const gchar *source = axis_json_string(axis_json_member(entry, "sourceName"));
		const AxisJsonNode *id = axis_json_member(entry, "id");
		const gchar *value = axis_json_type(id) == AXIS_JSON_STRING ? axis_json_string(id) :
			axis_json_type(id) == AXIS_JSON_NUMBER ? axis_json_scalar_text(id) : NULL;

		if (g_strcmp0(source, AXIS_META_TVDB_REMOTE_IMDB) == 0)
			axis_meta_title_add_id(title, "imdb", value);
		else if (g_strcmp0(source, AXIS_META_TVDB_REMOTE_TMDB) == 0)
			axis_meta_title_add_id(title, "tmdb", value);
	}
}

/* A new series record with its own id, or %NULL without one. */
static AxisMetaTitle *
series_new(
	const AxisJsonNode *id_node,
	gboolean            detail
){
	g_autofree gchar *id = id_of(id_node);
	AxisMetaTitle *title;

	if (id == NULL)
		return NULL;
	title = axis_meta_title_new(AXIS_META_KIND_SERIES);
	title->provider = g_strdup(AXIS_META_TVDB_NAME);
	title->provider_id = g_strdup(id);
	title->detail = detail;
	axis_meta_title_add_id(title, AXIS_META_TVDB_NAME, id);
	return title;
}

static void
language_of(
	const AxisJsonNode *object,
	const gchar        *name,
	AxisMetaTitle      *title
){
	const gchar *language = axis_json_string(axis_json_member(object, name));

	if (axis_meta_language_valid(language))
		title->original_language = g_strdup(language);
}

/* One result of a search (assumed 9), or %NULL. */
static AxisMetaTitle *
search_result(
	const AxisJsonNode *node,
	const gchar        *language
){
	const gchar *type = axis_json_string(axis_json_member(node, "type"));
	AxisMetaTitle *title;

	if (axis_json_type(node) != AXIS_JSON_OBJECT || (type != NULL && strcmp(type, "series") != 0))
		return NULL;
	title = series_new(axis_json_member(node, AXIS_META_TVDB_SEARCH_ID_MEMBER), FALSE);
	if (title == NULL)
		return NULL;
	title->original_title = axis_meta_json_text(node, "name", AXIS_META_TITLE_MAX_CHARS);
	title->title = keyed_text(node, "translations", language, AXIS_META_TITLE_MAX_CHARS);
	if (title->title == NULL)
		title->title = g_strdup(title->original_title);
	if (title->title == NULL) {
		axis_meta_title_free(title);
		return NULL;
	}
	title->overview = keyed_text(node, "overviews", language, AXIS_META_OVERVIEW_MAX_CHARS);
	if (title->overview == NULL)
		title->overview = axis_meta_json_text(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
	title->first_air_date = axis_meta_json_date(node, "first_air_time");
	title->year = year_member(node, "year");
	if (title->year == 0)
		title->year = axis_meta_json_year_of(title->first_air_date);
	language_of(node, "primary_language", title);
	title->poster_ref = image_member(node, "image_url");
	return title;
}

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

static void
add_result(
	GPtrArray     *out,
	AxisMetaTitle *title
){
	if (title == NULL)
		return;
	if (out->len >= AXIS_META_MAX_RESULTS || listed(out, title->provider_id)) {
		axis_meta_title_free(title);
		return;
	}
	g_ptr_array_add(out, title);
}

gboolean
axis_meta_tvdb_parse_search(
	const gchar  *data,
	gsize         length,
	const gchar  *language,
	GPtrArray   **titles,
	GError      **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;
	guint i;

	*titles = NULL;
	if (doc == NULL)
		return FALSE;
	list = payload(doc);
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of results");
	*titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	for (i = 0; i < axis_json_length(list) && i < MAX_ENTRIES && (*titles)->len < AXIS_META_MAX_RESULTS; i++)
		add_result(*titles, search_result(axis_json_element(list, i), language));
	return TRUE;
}

/* ── A series' record ───────────────────────────────────────────── */

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
	for (i = 0; i < axis_json_length(list) && i < MAX_ENTRIES && title->genres->len < AXIS_META_MAX_GENRES; i++) {
		gchar *genre = axis_meta_json_text(axis_json_element(list, i), "name", AXIS_META_GENRE_MAX_CHARS);
		gboolean twice = FALSE;
		guint j;

		for (j = 0; genre != NULL && j < title->genres->len && !twice; j++)
			twice = strcmp(g_ptr_array_index(title->genres, j), genre) == 0;
		if (genre == NULL || twice) {
			g_free(genre);
			continue;
		}
		g_ptr_array_add(title->genres, genre);
	}
}

/* Its aliases, and its name in every other language. */
static void
read_other_names(
	const AxisJsonNode *node,
	const gchar        *language,
	AxisMetaTitle      *title
){
	const AxisJsonNode *aliases = axis_json_member(node, "aliases");
	const AxisJsonNode *names = axis_json_member(axis_json_member(node, "translations"), "nameTranslations");
	guint i;

	title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
	for (i = 0; axis_json_type(aliases) == AXIS_JSON_ARRAY && i < axis_json_length(aliases) && i < MAX_ENTRIES; i++) {
		const AxisJsonNode *entry = axis_json_element(aliases, i);

		add_alt(title, axis_meta_json_text(entry, "name", AXIS_META_TITLE_MAX_CHARS),
			axis_json_string(axis_json_member(entry, "language")));
	}
	for (i = 0; axis_json_type(names) == AXIS_JSON_ARRAY && i < axis_json_length(names) && i < MAX_ENTRIES; i++) {
		const AxisJsonNode *entry = axis_json_element(names, i);
		const gchar *in = axis_json_string(axis_json_member(entry, "language"));
		gchar *name = axis_meta_json_text(entry, "name", AXIS_META_TITLE_MAX_CHARS);

		/* The name shown is not another name of it. */
		if (name != NULL && g_strcmp0(name, title->title) == 0 && g_strcmp0(in, language) == 0) {
			g_free(name);
			continue;
		}
		add_alt(title, name, in);
	}
}

/* The orders its seasons are numbered in (assumed 6), and its aired
 * order's seasons. */
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
	title->sources = g_ptr_array_new();
	title->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	for (i = 0; i < axis_json_length(list) && i < MAX_ENTRIES; i++) {
		const AxisJsonNode *entry = axis_json_element(list, i);
		const AxisJsonNode *type = axis_json_member(entry, AXIS_META_TVDB_SEASON_TYPE_MEMBER);
		const gchar *source = axis_meta_tvdb_source_of_order(axis_json_string(axis_json_member(type, "type")));
		const AxisJsonNode *number_node = axis_json_member(entry, "number");
		gint64 number = -1;
		gboolean twice = FALSE;
		AxisMetaSeason *season;

		if (source == NULL)
			continue;
		for (j = 0; j < title->sources->len && !twice; j++)
			twice = strcmp(g_ptr_array_index(title->sources, j), source) == 0;
		if (!twice && title->sources->len < AXIS_META_MAX_SOURCES)
			g_ptr_array_add(title->sources, (gpointer)source);
		if (strcmp(source, AXIS_META_TVDB_NAME) != 0 || axis_json_type(number_node) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(number_node, 0, 10000, &number) || title->seasons->len >= AXIS_META_MAX_SEASONS)
			continue;
		twice = FALSE;
		for (j = 0; j < title->seasons->len && !twice; j++)
			twice = ((AxisMetaSeason *)g_ptr_array_index(title->seasons, j))->number == (gint)number;
		if (twice)
			continue;
		season = axis_meta_season_new((gint)number);
		season->name = axis_meta_json_text(entry, "name", AXIS_META_NAME_MAX_CHARS);
		g_ptr_array_add(title->seasons, season);
	}
}

/* The sources are the parser's own constants, not copies. */
static void
own_sources(AxisMetaTitle *title)
{
	GPtrArray *copy;
	guint i;

	if (title->sources == NULL)
		return;
	copy = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < title->sources->len; i++)
		g_ptr_array_add(copy, g_strdup(g_ptr_array_index(title->sources, i)));
	g_ptr_array_unref(title->sources);
	title->sources = copy;
}

gboolean
axis_meta_tvdb_parse_series(
	const gchar    *data,
	gsize           length,
	const gchar    *language,
	AxisMetaTitle **title_out,
	GError        **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *node;
	const AxisJsonNode *translations;
	AxisMetaTitle *title;

	*title_out = NULL;
	if (doc == NULL)
		return FALSE;
	node = payload(doc);
	if (axis_json_type(node) != AXIS_JSON_OBJECT || (title = series_new(axis_json_member(node, "id"), TRUE)) == NULL)
		return unreadable(error, "it is not a series");
	translations = axis_json_member(node, "translations");
	title->original_title = axis_meta_json_text(node, "name", AXIS_META_TITLE_MAX_CHARS);
	title->title = listed_text(axis_json_member(translations, "nameTranslations"), "name", language,
		AXIS_META_TITLE_MAX_CHARS);
	if (title->title == NULL)
		title->title = g_strdup(title->original_title);
	if (title->title == NULL) {
		axis_meta_title_free(title);
		return unreadable(error, "the series has no name");
	}
	title->overview = listed_text(axis_json_member(translations, "overviewTranslations"), "overview", language,
		AXIS_META_OVERVIEW_MAX_CHARS);
	if (title->overview == NULL)
		title->overview = axis_meta_json_text(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
	title->first_air_date = axis_meta_json_date(node, "firstAired");
	title->last_air_date = axis_meta_json_date(node, "lastAired");
	title->year = year_member(node, "year");
	if (title->year == 0)
		title->year = axis_meta_json_year_of(title->first_air_date);
	title->status = status_of(axis_json_string(axis_json_member(axis_json_member(node, "status"), "name")));
	title->runtime = axis_meta_json_int(node, "averageRuntime", 1, 10000);
	language_of(node, "originalLanguage", title);
	title->poster_ref = image_member(node, "image");
	read_genres(node, title);
	read_other_names(node, language, title);
	read_remote_ids(axis_json_member(node, "remoteIds"), title);
	read_seasons(node, title);
	own_sources(title);
	*title_out = title;
	return TRUE;
}

gboolean
axis_meta_tvdb_parse_remote(
	const gchar  *data,
	gsize         length,
	const gchar  *scheme,
	const gchar  *value,
	const gchar  *language,
	GPtrArray   **titles,
	GError      **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;
	guint i;

	*titles = NULL;
	if (doc == NULL)
		return FALSE;
	list = payload(doc);
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of results");
	*titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	for (i = 0; i < axis_json_length(list) && i < MAX_ENTRIES && (*titles)->len < AXIS_META_MAX_RESULTS; i++) {
		const AxisJsonNode *series = axis_json_member(axis_json_element(list, i), "series");
		AxisMetaTitle *title;

		if (axis_json_type(series) != AXIS_JSON_OBJECT || (title = series_new(axis_json_member(series, "id"),
		        FALSE)) == NULL)
			continue;
		title->original_title = axis_meta_json_text(series, "name", AXIS_META_TITLE_MAX_CHARS);
		title->title = g_strdup(title->original_title);
		if (title->title == NULL) {
			axis_meta_title_free(title);
			continue;
		}
		title->overview = axis_meta_json_text(series, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
		title->first_air_date = axis_meta_json_date(series, "firstAired");
		title->year = year_member(series, "year");
		if (title->year == 0)
			title->year = axis_meta_json_year_of(title->first_air_date);
		title->poster_ref = image_member(series, "image");
		add_result(*titles, title);
	}
	/* What was asked for is known to belong to the one series that came
	 * back (not to a list of them). */
	if ((*titles)->len == 1 && scheme != NULL && value != NULL)
		axis_meta_title_add_id(g_ptr_array_index(*titles, 0), scheme, value);
	return TRUE;
}

/* ── Episodes ───────────────────────────────────────────────────── */

static gint
compare_seasons(
	gconstpointer a,
	gconstpointer b
){
	const AxisMetaSeason *x = *(AxisMetaSeason *const *)a;
	const AxisMetaSeason *y = *(AxisMetaSeason *const *)b;

	return x->number < y->number ? -1 : x->number > y->number;
}

static AxisMetaSeason *
season_for(
	GPtrArray *seasons,
	gint       number
){
	AxisMetaSeason *season;
	guint i;

	for (i = 0; i < seasons->len; i++) {
		season = g_ptr_array_index(seasons, i);
		if (season->number == number)
			return season;
	}
	if (seasons->len >= AXIS_META_MAX_SEASONS)
		return NULL;
	season = axis_meta_season_new(number);
	season->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_episode_free);
	season->episode_count = 0;
	g_ptr_array_add(seasons, season);
	return season;
}

/* The places ("season:number") and the ids of the episodes @seasons hold. */
static void
note_read(
	GPtrArray  *seasons,
	GHashTable *places,
	GHashTable *ids
){
	guint i;
	guint j;

	for (i = 0; i < seasons->len; i++) {
		const AxisMetaSeason *season = g_ptr_array_index(seasons, i);

		for (j = 0; season->episodes != NULL && j < season->episodes->len; j++) {
			const AxisMetaEpisode *episode = g_ptr_array_index(season->episodes, j);

			g_hash_table_add(places, g_strdup_printf("%d:%d", season->number, episode->number));
			if (episode->provider_id != NULL)
				g_hash_table_add(ids, g_strdup(episode->provider_id));
		}
	}
}

/* Whether an episode at its place or of its id was read; if not, it is now. */
static gboolean
already_read(
	GHashTable            *places,
	GHashTable            *ids,
	const AxisMetaEpisode *episode
){
	gchar *place = g_strdup_printf("%d:%d", episode->season_number, episode->number);

	if (g_hash_table_contains(places, place) ||
	    (episode->provider_id != NULL && g_hash_table_contains(ids, episode->provider_id))) {
		g_free(place);
		return TRUE;
	}
	g_hash_table_add(places, place);
	if (episode->provider_id != NULL)
		g_hash_table_add(ids, g_strdup(episode->provider_id));
	return FALSE;
}

gboolean
axis_meta_tvdb_parse_episodes(
	const gchar  *data,
	gsize         length,
	GPtrArray    *seasons,
	gboolean     *more,
	GError      **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	g_autoptr(GPtrArray) page = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_episode_free);
	g_autoptr(GHashTable) places = NULL;
	g_autoptr(GHashTable) ids = NULL;
	const AxisJsonNode *list;
	const AxisJsonNode *next;
	guint i;

	g_return_val_if_fail(seasons != NULL && more != NULL, FALSE);

	*more = FALSE;
	if (doc == NULL)
		return FALSE;
	list = axis_json_member(payload(doc), "episodes");
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of episodes");
	/* The page is read whole before any of it is added. */
	for (i = 0; i < axis_json_length(list) && i < MAX_ENTRIES; i++) {
		const AxisJsonNode *node = axis_json_element(list, i);
		const AxisJsonNode *season_node = axis_json_member(node, "seasonNumber");
		const AxisJsonNode *number_node = axis_json_member(node, "number");
		gint64 season_number = -1;
		gint64 number = -1;
		AxisMetaEpisode *episode;

		if (axis_json_type(node) != AXIS_JSON_OBJECT || axis_json_type(season_node) != AXIS_JSON_NUMBER ||
		    axis_json_type(number_node) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(season_node, 0, 10000, &season_number) ||
		    !axis_json_get_integer(number_node, 0, 100000, &number))
			continue;
		episode = axis_meta_episode_new();
		episode->season_number = (gint)season_number;
		episode->number = (gint)number;
		episode->absolute_number = axis_meta_json_int(node, "absoluteNumber", 1, 1000000);
		episode->name = axis_meta_json_text(node, "name", AXIS_META_NAME_MAX_CHARS);
		episode->air_date = axis_meta_json_date(node, "aired");
		episode->overview = axis_meta_json_text(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
		episode->runtime = axis_meta_json_int(node, "runtime", 1, 10000);
		episode->provider_id = id_of(axis_json_member(node, "id"));
		if (episode->provider_id != NULL && !axis_meta_episode_id_valid(episode->provider_id))
			g_clear_pointer(&episode->provider_id, g_free);
		g_ptr_array_add(page, episode);
	}
	next = axis_json_member(axis_json_member(axis_json_doc_get_root(doc), "links"), "next");
	*more = axis_json_type(next) == AXIS_JSON_STRING && axis_json_string(next) != NULL &&
		axis_json_string(next)[0] != '\0';
	places = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	note_read(seasons, places, ids);
	for (i = 0; i < page->len; i++) {
		AxisMetaEpisode *episode = g_ptr_array_index(page, i);
		AxisMetaSeason *season;

		if (already_read(places, ids, episode))
			continue;
		season = season_for(seasons, episode->season_number);
		if (season == NULL || season->episodes->len >= AXIS_META_MAX_EPISODES)
			continue;
		g_ptr_array_add(season->episodes, episode);
		page->pdata[i] = NULL;
		season->episode_count = (gint)season->episodes->len;
	}
	g_ptr_array_sort(seasons, compare_seasons);
	return TRUE;
}

gboolean
axis_meta_tvdb_parse_orders(
	const gchar  *data,
	gsize         length,
	GPtrArray   **names,
	GError      **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;
	guint i;

	*names = NULL;
	if (doc == NULL)
		return FALSE;
	list = payload(doc);
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of episode orders");
	*names = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_json_length(list) && i < MAX_ENTRIES && (*names)->len < AXIS_META_MAX_SOURCES * 4; i++) {
		gchar *name = axis_meta_json_text(axis_json_element(list, i), "name", AXIS_META_NAME_MAX_CHARS);

		if (name != NULL)
			g_ptr_array_add(*names, name);
	}
	return TRUE;
}

/* ── Login ──────────────────────────────────────────────────────── */

gboolean
axis_meta_tvdb_parse_login(
	const gchar  *data,
	gsize         length,
	gchar       **token,
	GError      **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const gchar *text;
	const gchar *p;

	*token = NULL;
	if (doc == NULL)
		return FALSE;
	text = axis_json_string(axis_json_member(payload(doc), "token"));
	if (text == NULL || text[0] == '\0' || strlen(text) > AXIS_META_TOKEN_MAX_BYTES)
		return unreadable(error, "it has no token");
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			return unreadable(error, "its token is not one a header can carry");
	}
	*token = g_strdup(text);
	return TRUE;
}

/* ── Forms ──────────────────────────────────────────────────────── */

gchar *
axis_meta_tvdb_image_ref(const gchar *url)
{
	static const gchar *const extensions[] = { ".jpg", ".jpeg", ".png", ".webp", NULL };
	const gchar *path;
	guint i;

	if (url == NULL || !g_str_has_prefix(url, AXIS_META_TVDB_IMAGE_ORIGIN "/"))
		return NULL;
	path = url + strlen(AXIS_META_TVDB_IMAGE_ORIGIN);
	if (!axis_meta_image_ref_valid(path))
		return NULL;
	for (i = 0; extensions[i] != NULL; i++) {
		if (g_str_has_suffix(path, extensions[i]) && strlen(path) > strlen(extensions[i]) + 1 &&
		    path[strlen(path) - strlen(extensions[i]) - 1] != '/')
			return g_strdup(path);
	}
	return NULL;
}

gchar *
axis_meta_tvdb_image_path(
	const gchar     *ref,
	AxisMetaArtSize  size
){
	g_autofree gchar *url = NULL;
	g_autofree gchar *again = NULL;

	if (ref == NULL || (guint)size >= AXIS_META_ART_N_SIZES)
		return NULL;
	/* Only what an answer could have given. */
	url = g_strconcat(AXIS_META_TVDB_IMAGE_ORIGIN, ref, NULL);
	again = axis_meta_tvdb_image_ref(url);
	if (again == NULL || strcmp(again, ref) != 0)
		return NULL;
	return g_steal_pointer(&again);
}

gboolean
axis_meta_tvdb_secret_valid(
	const gchar  *text,
	gchar       **reason
){
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > 255) {
		*reason = g_strdup("must be from 1 to 255 characters");
		return FALSE;
	}
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || *p == '"' || *p == '\\') {
			*reason = g_strdup("must be as TheTVDB gives it: letters, digits and punctuation, without spaces, "
				"quotes or backslashes");
			return FALSE;
		}
	}
	return TRUE;
}

gboolean
axis_meta_tvdb_language_valid(const gchar *text)
{
	return text != NULL && strlen(text) == 3 && g_ascii_islower(text[0]) && g_ascii_islower(text[1]) &&
		g_ascii_islower(text[2]);
}
