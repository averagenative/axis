/*
 * axis-test-meta-fixture-parse.c - test support: the fixture provider's
 * answer format, read into the metadata model
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-test-meta-fixture-parse.h"

#include <string.h>

#include "meta/axis-meta-provider.h"
#include "probe/axis-probe-json.h"

static gboolean
unreadable(
	GError      **error,
	const gchar  *why
){
	g_set_error(error, AXIS_META_ERROR, AXIS_META_ERROR_UNREADABLE, "the fixture answer cannot be read: %s", why);
	return FALSE;
}

static gchar *
text_member(
	const AxisJsonNode *object,
	const gchar        *name,
	guint               max_chars
){
	return axis_meta_clean_text(axis_json_string(axis_json_member(object, name)), max_chars);
}

/* A whole number member within [@min, @max], else 0. */
static gint
int_member(
	const AxisJsonNode *object,
	const gchar        *name,
	gint64              min,
	gint64              max
){
	gint64 value = 0;

	if (!axis_json_get_integer(axis_json_member(object, name), min, max, &value))
		return 0;
	return (gint)value;
}

static gchar *
date_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const gchar *text = axis_json_string(axis_json_member(object, name));

	return axis_meta_date_valid(text) ? g_strdup(text) : NULL;
}

static gchar *
ref_member(
	const AxisJsonNode *object,
	const gchar        *name
){
	const gchar *text = axis_json_string(axis_json_member(object, name));

	return axis_meta_image_ref_valid(text) ? g_strdup(text) : NULL;
}

/* The id as a string, from a string or a number. */
static gchar *
id_member(const AxisJsonNode *object)
{
	const gchar *text = axis_json_scalar_text(axis_json_member(object, "id"));

	return text != NULL ? g_strdup(text) : NULL;
}

static AxisMetaSeason *
season_summary(const AxisJsonNode *node)
{
	gint64 number = -1;
	gint64 count = -1;
	AxisMetaSeason *season;

	if (axis_json_type(node) != AXIS_JSON_OBJECT ||
	    !axis_json_get_integer(axis_json_member(node, "number"), 0, 10000, &number))
		return NULL;
	season = axis_meta_season_new((gint)number);
	season->name = text_member(node, "name", AXIS_META_NAME_MAX_CHARS);
	season->air_date = date_member(node, "air_date");
	season->overview = text_member(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
	if (axis_json_get_integer(axis_json_member(node, "episodes"), 0, 100000, &count))
		season->episode_count = (gint)count;
	return season;
}

/* One title from @node, or %NULL when it lacks what makes it one. */
static AxisMetaTitle *
title_from(
	const gchar        *provider,
	AxisMetaKind        kind,
	const AxisJsonNode *node,
	gboolean            detail
){
	g_autofree gchar *id = NULL;
	const gchar *kind_text;
	const AxisJsonNode *list;
	const AxisJsonNode *ids;
	AxisMetaKind stated;
	AxisMetaTitle *title;
	gint year;
	guint i;

	if (axis_json_type(node) != AXIS_JSON_OBJECT)
		return NULL;
	kind_text = axis_json_string(axis_json_member(node, "kind"));
	if (kind_text != NULL && (!axis_meta_kind_from_string(kind_text, &stated) || stated != kind))
		return NULL;
	id = id_member(node);
	if (!axis_meta_id_valid(provider, id))
		return NULL;
	title = axis_meta_title_new(kind);
	title->provider = g_strdup(provider);
	title->provider_id = g_strdup(id);
	title->detail = detail;
	axis_meta_title_add_id(title, provider, id);
	title->title = text_member(node, "title", AXIS_META_TITLE_MAX_CHARS);
	if (title->title == NULL) {
		axis_meta_title_free(title);
		return NULL;
	}
	title->original_title = text_member(node, "original_title", AXIS_META_TITLE_MAX_CHARS);
	year = int_member(node, "year", AXIS_META_YEAR_MIN, AXIS_META_YEAR_MAX);
	title->year = year;
	title->overview = text_member(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
	title->runtime = int_member(node, "runtime", 1, 10000);
	title->status = axis_meta_status_from_string(axis_json_string(axis_json_member(node, "status")));
	{
		const gchar *language = axis_json_string(axis_json_member(node, "language"));

		if (axis_meta_language_valid(language))
			title->original_language = g_strdup(language);
	}
	title->release_date = date_member(node, "release_date");
	/* A full record's year of its theatrical release in the region. */
	if (detail)
		title->regional_year = int_member(node, "regional_year", AXIS_META_YEAR_MIN, AXIS_META_YEAR_MAX);
	title->first_air_date = date_member(node, "first_air_date");
	title->last_air_date = date_member(node, "last_air_date");
	title->poster_ref = ref_member(node, "poster");
	title->backdrop_ref = ref_member(node, "backdrop");
	/* The provider's adult mark (the fixture provider filters nothing): as
	 * TMDb's, every answer read says it, one without the member not adult. */
	if (!axis_json_get_boolean(axis_json_member(node, "adult"), &title->adult))
		title->adult = FALSE;
	title->adult_known = TRUE;

	ids = axis_json_member(node, "ids");
	for (i = 0; i < axis_json_length(ids) && axis_json_type(ids) == AXIS_JSON_OBJECT; i++) {
		const gchar *scheme = axis_json_member_name(ids, i);

		/* Not its own scheme twice, and only schemes it may name. */
		if (g_strcmp0(scheme, provider) == 0)
			continue;
		axis_meta_title_add_id(title, scheme, axis_json_scalar_text(axis_json_member_value(ids, i)));
	}

	list = axis_json_member(node, "genres");
	if (axis_json_type(list) == AXIS_JSON_ARRAY) {
		title->genres = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; i < axis_json_length(list) && title->genres->len < AXIS_META_MAX_GENRES; i++) {
			gchar *genre = axis_meta_clean_text(axis_json_string(axis_json_element(list, i)), AXIS_META_GENRE_MAX_CHARS);

			if (genre != NULL)
				g_ptr_array_add(title->genres, genre);
		}
	}

	list = axis_json_member(node, "alt_titles");
	if (axis_json_type(list) == AXIS_JSON_ARRAY) {
		title->alt_titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_alt_title_free);
		for (i = 0; i < axis_json_length(list) && title->alt_titles->len < AXIS_META_MAX_ALT_TITLES; i++) {
			const AxisJsonNode *entry = axis_json_element(list, i);
			gchar *name = text_member(entry, "title", AXIS_META_TITLE_MAX_CHARS);
			const gchar *country = axis_json_string(axis_json_member(entry, "country"));
			const gchar *language = axis_json_string(axis_json_member(entry, "language"));
			AxisMetaAltTitle *alt;

			if (name == NULL)
				continue;
			alt = g_new0(AxisMetaAltTitle, 1);
			alt->title = name;
			alt->country = axis_meta_country_valid(country) ? g_strdup(country) : NULL;
			alt->language = axis_meta_language_valid(language) ? g_strdup(language) : NULL;
			g_ptr_array_add(title->alt_titles, alt);
		}
	}

	list = axis_json_member(node, "seasons");
	if (kind == AXIS_META_KIND_SERIES && axis_json_type(list) == AXIS_JSON_ARRAY) {
		title->seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
		for (i = 0; i < axis_json_length(list) && title->seasons->len < AXIS_META_MAX_SEASONS; i++) {
			AxisMetaSeason *season = season_summary(axis_json_element(list, i));
			guint j;
			gboolean twice = FALSE;

			if (season == NULL)
				continue;
			for (j = 0; j < title->seasons->len; j++)
				twice = twice || ((AxisMetaSeason *)g_ptr_array_index(title->seasons, j))->number == season->number;
			if (twice) {
				axis_meta_season_free(season);
				continue;
			}
			g_ptr_array_add(title->seasons, season);
		}
	}
	return title;
}

static AxisJsonDoc *
parse(
	const gchar  *data,
	gsize         length,
	GError      **error
){
	g_autofree gchar *problem = NULL;
	AxisJsonDoc *doc;

	if (data == NULL || length == 0) {
		unreadable(error, "it is empty");
		return NULL;
	}
	doc = axis_json_parse(data, length, &problem);
	if (doc == NULL)
		unreadable(error, problem != NULL ? problem : "it is not JSON");
	return doc;
}

gboolean
axis_test_meta_fixture_parse_results(
	const gchar   *provider,
	AxisMetaKind   kind,
	const gchar   *data,
	gsize          length,
	GPtrArray    **titles,
	GError       **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);
	const AxisJsonNode *list;
	GPtrArray *out;
	guint i;

	*titles = NULL;
	if (doc == NULL)
		return FALSE;
	list = axis_json_member(axis_json_doc_get_root(doc), "results");
	if (axis_json_type(list) != AXIS_JSON_ARRAY)
		return unreadable(error, "it has no list of results");
	out = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_title_free);
	for (i = 0; i < axis_json_length(list) && out->len < AXIS_META_MAX_RESULTS; i++) {
		AxisMetaTitle *title = title_from(provider, kind, axis_json_element(list, i), FALSE);
		guint j;
		gboolean twice = FALSE;

		if (title == NULL)
			continue;
		/* Brief records: what a search shows, no more. */
		g_clear_pointer(&title->alt_titles, g_ptr_array_unref);
		g_clear_pointer(&title->seasons, g_ptr_array_unref);
		g_clear_pointer(&title->genres, g_ptr_array_unref);
		for (j = 0; j < out->len; j++)
			twice = twice || strcmp(((AxisMetaTitle *)g_ptr_array_index(out, j))->provider_id, title->provider_id) == 0;
		if (twice) {
			axis_meta_title_free(title);
			continue;
		}
		g_ptr_array_add(out, title);
	}
	*titles = out;
	return TRUE;
}

gboolean
axis_test_meta_fixture_parse_title(
	const gchar    *provider,
	AxisMetaKind    kind,
	const gchar    *data,
	gsize           length,
	AxisMetaTitle **title,
	GError        **error
){
	g_autoptr(AxisJsonDoc) doc = parse(data, length, error);

	*title = NULL;
	if (doc == NULL)
		return FALSE;
	*title = title_from(provider, kind, axis_json_doc_get_root(doc), TRUE);
	if (*title == NULL)
		return unreadable(error, "it is not a title of the kind asked for");
	return TRUE;
}

gboolean
axis_test_meta_fixture_parse_season(
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
		axis_meta_season_free(season);
		return unreadable(error, "it is not the season asked for");
	}
	list = axis_json_member(root, "episodes");
	if (axis_json_type(list) != AXIS_JSON_ARRAY) {
		axis_meta_season_free(season);
		return unreadable(error, "it has no list of episodes");
	}
	season->episode_count = -1;
	season->episodes = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_episode_free);
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < axis_json_length(list) && season->episodes->len < AXIS_META_MAX_EPISODES; i++) {
		const AxisJsonNode *node = axis_json_element(list, i);
		gint64 episode_number = -1;
		AxisMetaEpisode *episode;
		gchar *id;

		if (axis_json_type(node) != AXIS_JSON_OBJECT ||
		    !axis_json_get_integer(axis_json_member(node, "number"), 0, 100000, &episode_number) ||
		    !g_hash_table_add(seen, GINT_TO_POINTER((gint)episode_number + 1)))
			continue;
		episode = axis_meta_episode_new();
		episode->season_number = season->number;
		episode->number = (gint)episode_number;
		episode->absolute_number = int_member(node, "absolute", 1, 1000000);
		episode->name = text_member(node, "name", AXIS_META_NAME_MAX_CHARS);
		episode->air_date = date_member(node, "air_date");
		episode->overview = text_member(node, "overview", AXIS_META_OVERVIEW_MAX_CHARS);
		episode->runtime = int_member(node, "runtime", 1, 10000);
		/* The provider's own id of the episode (optional: the fixtures of a
		 * provider that gives none leave it out). */
		id = id_member(node);
		if (id != NULL && axis_meta_episode_id_valid(id) && !g_hash_table_contains(ids, id)) {
			episode->provider_id = g_strdup(id);
			g_hash_table_add(ids, id);
		} else {
			g_free(id);
		}
		g_ptr_array_add(season->episodes, episode);
	}
	season->episode_count = (gint)season->episodes->len;
	*season_out = season;
	return TRUE;
}
