/*
 * fuzz-meta-tmdb-list.c - libFuzzer target for the reading of TMDb's lists
 * and genre names (axis_meta_tmdb_parse_list(), axis_meta_tmdb_parse_genres())
 * and for the paths of its lists
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' a list of films, '1' a list of
 * series with adult titles, '2' genres, '3' text for a recommendations path);
 * the rest is the input.  Whatever it is, the parser returns without
 * crashing or allocating without bound, and what it returns passes the
 * checks the metadata layer applies before anything is stored:
 * axis_meta_list_page_check() for a list (every title a brief record of the
 * kind asked for, each once, ratings and genre numbers in range) and
 * axis_meta_genres_check() for genres.  A failure is always an
 * %AXIS_META_ERROR_UNREADABLE and returns nothing.  A path is made only for
 * an identifier of TMDb's form, and then has one shape.
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
check_list(
	AxisMetaKind kind,
	gboolean     adult,
	const gchar *text,
	gsize        length
){
	AxisMetaListPage *page = NULL;
	GError *error = NULL;
	guint i;

	if (!axis_meta_tmdb_parse_list(kind, text, length, adult, &page, &error)) {
		check_failure(error);
		if (page != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (page == NULL || !axis_meta_list_page_check(page, kind, "tmdb", NULL))
		abort();
	for (i = 0; i < page->entries->len; i++) {
		const AxisMetaListEntry *entry = g_ptr_array_index(page->entries, i);

		/* A brief record: nothing of a full one. */
		if (entry->title->genres != NULL || entry->title->alt_titles != NULL || entry->title->seasons != NULL)
			abort();
		if (entry->title->poster_ref != NULL && !axis_meta_tmdb_image_ref_valid(entry->title->poster_ref))
			abort();
		/* No vote, no rating. */
		if (entry->votes == 0 && entry->rating != -1)
			abort();
	}
	axis_meta_list_page_free(page);
}

static void
check_genres(
	const gchar *text,
	gsize        length
){
	GPtrArray *genres = NULL;
	GError *error = NULL;

	if (!axis_meta_tmdb_parse_genres(text, length, &genres, &error)) {
		check_failure(error);
		if (genres != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (genres == NULL || !axis_meta_genres_check(genres, NULL))
		abort();
	g_ptr_array_unref(genres);
}

static void
check_path(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	guint kind;

	for (kind = AXIS_META_KIND_FILM; kind <= AXIS_META_KIND_SERIES; kind++) {
		gchar *path = axis_meta_tmdb_list_path((AxisMetaKind)kind, AXIS_META_LIST_RECOMMENDED, copy);

		if (axis_meta_id_valid("tmdb", copy)) {
			if (path == NULL || !g_str_has_prefix(path, "/3/") || !g_str_has_suffix(path, "/recommendations") ||
			    strchr(path, '?') != NULL || strchr(path, '#') != NULL || strstr(path, "..") != NULL ||
			    strstr(path, copy) == NULL)
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
		check_list(AXIS_META_KIND_FILM, FALSE, text, length);
		break;
	case '1':
		check_list(AXIS_META_KIND_SERIES, TRUE, text, length);
		break;
	case '2':
		check_genres(text, length);
		break;
	case '3':
		check_path(text, length);
		break;
	default:
		break;
	}
	return 0;
}
