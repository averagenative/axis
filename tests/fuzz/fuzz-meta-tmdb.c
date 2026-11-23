/*
 * fuzz-meta-tmdb.c - libFuzzer target for the TMDb provider's reading of
 * TMDb's answers (meta/axis-meta-tmdb-parse.h) and its few rules about
 * TMDb's own forms: the key, language, region and image file names
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' film search, '1' series search,
 * '2' film find, '3' series find with adult titles, '4' a film, '5' a
 * series, '6' season 1, '7' season 0, '8' text for the forms); the rest is
 * the input.  Whatever it is, the parser returns without crashing or
 * allocating without bound, and what it returns passes the checks the store
 * applies before anything is kept: axis_meta_title_check() for every title,
 * axis_meta_season_check() for a season.  A failure is always an
 * %AXIS_META_ERROR_UNREADABLE and returns nothing.  An image path is made
 * only for a file name of the strict form and then has one shape; a value
 * refused as the key never has itself in the reason.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "meta/axis-meta-tmdb-parse.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_failure(GError *error)
{
	if (error == NULL || error->domain != AXIS_META_ERROR || error->code != AXIS_META_ERROR_UNREADABLE)
		abort();
}

static void
check_titles(
	GPtrArray    *titles,
	AxisMetaKind  kind
){
	guint i;

	if (titles == NULL || titles->len > AXIS_META_MAX_RESULTS)
		abort();
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		if (title->kind != kind || title->detail || !axis_meta_title_check(title, "tmdb", NULL))
			abort();
		if (title->alt_titles != NULL || title->seasons != NULL || title->genres != NULL)
			abort();
		if (title->poster_ref != NULL && !axis_meta_tmdb_image_ref_valid(title->poster_ref))
			abort();
		if (title->backdrop_ref != NULL && !axis_meta_tmdb_image_ref_valid(title->backdrop_ref))
			abort();
	}
}

static void
check_search(
	AxisMetaKind kind,
	gboolean     find,
	gboolean     adult,
	const gchar *text,
	gsize        length
){
	GPtrArray *titles = NULL;
	GError *error = NULL;
	gboolean ok;

	if (find)
		ok = axis_meta_tmdb_parse_find(kind, "imdb", "tt0600003", text, length, adult, &titles, &error);
	else
		ok = axis_meta_tmdb_parse_search(kind, text, length, adult, &titles, &error);
	if (!ok) {
		check_failure(error);
		if (titles != NULL)
			abort();
		g_error_free(error);
		return;
	}
	check_titles(titles, kind);
	g_ptr_array_unref(titles);
}

static void
check_title(
	AxisMetaKind kind,
	const gchar *text,
	gsize        length
){
	AxisMetaTitle *title = NULL;
	GError *error = NULL;

	if (!axis_meta_tmdb_parse_title(kind, "US", text, length, &title, &error)) {
		check_failure(error);
		if (title != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (title == NULL || title->kind != kind || !title->detail || !axis_meta_title_check(title, "tmdb", NULL))
		abort();
	if (kind == AXIS_META_KIND_FILM && (title->seasons != NULL || title->first_air_date != NULL))
		abort();
	if (kind == AXIS_META_KIND_SERIES && title->release_date != NULL)
		abort();
	axis_meta_title_free(title);
}

static void
check_season(
	gint         number,
	const gchar *text,
	gsize        length
){
	AxisMetaSeason *season = NULL;
	GError *error = NULL;

	if (!axis_meta_tmdb_parse_season(number, text, length, &season, &error)) {
		check_failure(error);
		if (season != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (season == NULL || season->number != number || !axis_meta_season_check(season, NULL))
		abort();
	axis_meta_season_free(season);
}

static void
check_forms(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar *reason = NULL;
	guint size;

	/* A key: refused with a reason that does not hold it. */
	if (copy[0] != '\0') {
		if (!axis_meta_tmdb_key_valid(copy, &reason)) {
			if (reason == NULL || (strlen(copy) >= 16 && strstr(reason, copy) != NULL))
				abort();
			g_free(reason);
		} else if (reason != NULL) {
			abort();
		}
	}
	/* A language and a region: a code of the stated shapes, nothing longer. */
	if (axis_meta_tmdb_language_valid(copy)) {
		gsize n = strlen(copy);

		if (n != 2 && n != 3 && n != 5 && n != 6)
			abort();
	}
	if (axis_meta_tmdb_region_valid(copy) && strlen(copy) != 2)
		abort();
	/* An image reference: the path has one shape, for every size. */
	for (size = 0; size < AXIS_META_ART_N_SIZES; size++) {
		gchar *path = axis_meta_tmdb_image_path(copy, (AxisMetaArtSize)size);

		if (axis_meta_tmdb_image_ref_valid(copy)) {
			if (path == NULL || !g_str_has_prefix(path, "/t/p/w") || !axis_meta_image_ref_valid(path) ||
			    strchr(path, '?') != NULL || strchr(path, '#') != NULL || strstr(path, "..") != NULL ||
			    !g_str_has_suffix(path, copy) || strlen(path) > 199 + 10)
				abort();
		} else if (path != NULL) {
			abort();
		}
		g_free(path);
	}
	g_free(copy);
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
		check_search(AXIS_META_KIND_FILM, FALSE, FALSE, text, length);
		break;
	case '1':
		check_search(AXIS_META_KIND_SERIES, FALSE, TRUE, text, length);
		break;
	case '2':
		check_search(AXIS_META_KIND_FILM, TRUE, FALSE, text, length);
		break;
	case '3':
		check_search(AXIS_META_KIND_SERIES, TRUE, TRUE, text, length);
		break;
	case '4':
		check_title(AXIS_META_KIND_FILM, text, length);
		break;
	case '5':
		check_title(AXIS_META_KIND_SERIES, text, length);
		break;
	case '6':
		check_season(1, text, length);
		break;
	case '7':
		check_season(0, text, length);
		break;
	case '8':
		check_forms(text, length);
		break;
	default:
		break;
	}
	return 0;
}
