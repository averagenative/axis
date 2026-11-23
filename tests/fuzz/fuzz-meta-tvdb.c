/*
 * fuzz-meta-tvdb.c - libFuzzer target for the TheTVDB provider's reading of
 * TheTVDB's answers (meta/axis-meta-tvdb-parse.h) and its rules about
 * TheTVDB's own forms: image addresses, the key and PIN, the language
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' a search, '1' a series' record,
 * '2' a remote id's answer, '3' one or two pages of episodes, '4' the list of
 * orders, '5' a login, '6' text for the forms); the rest is the input.
 * Whatever it is, the parser returns without crashing or allocating without
 * bound, and what it returns passes the checks the store applies before
 * anything is kept: axis_meta_title_check() for every title,
 * axis_meta_season_check() for every season.  A failure is always an
 * %AXIS_META_ERROR_UNREADABLE and returns (or adds) nothing.  An image is a
 * path on TheTVDB's one image origin or nothing; a token is printable
 * without spaces; a value refused as a key never has itself in the reason.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "meta/axis-meta-tvdb-parse.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_failure(GError *error)
{
	if (error == NULL || error->domain != AXIS_META_ERROR || error->code != AXIS_META_ERROR_UNREADABLE)
		abort();
	g_error_free(error);
}

/* A reference passes the model's check (no "." or ".." part, no query). */
static void
check_image(const gchar *ref)
{
	if (ref != NULL && (!axis_meta_image_ref_valid(ref) || strstr(ref, "/../") != NULL || strchr(ref, '?') != NULL ||
	    strchr(ref, '#') != NULL || strchr(ref, ':') != NULL))
		abort();
}

static void
check_title(
	const AxisMetaTitle *title,
	gboolean             detail
){
	if (title == NULL || title->kind != AXIS_META_KIND_SERIES || title->detail != detail ||
	    !axis_meta_title_check(title, "tvdb", NULL))
		abort();
	check_image(title->poster_ref);
	check_image(title->backdrop_ref);
}

static void
check_titles(GPtrArray *titles)
{
	guint i;

	if (titles == NULL || titles->len > AXIS_META_MAX_RESULTS)
		abort();
	for (i = 0; i < titles->len; i++)
		check_title(g_ptr_array_index(titles, i), FALSE);
}

static void
check_search(
	gint         which,
	const gchar *text,
	gsize        length
){
	GPtrArray *titles = NULL;
	GError *error = NULL;
	gboolean ok;

	if (which == 0)
		ok = axis_meta_tvdb_parse_search(text, length, "eng", &titles, &error);
	else
		ok = axis_meta_tvdb_parse_remote(text, length, "imdb", "tt9000081", "eng", &titles, &error);
	if (!ok) {
		check_failure(error);
		if (titles != NULL)
			abort();
		return;
	}
	check_titles(titles);
	/* A search's brief record claims no other scheme. */
	if (which == 0) {
		guint i;

		for (i = 0; i < titles->len; i++) {
			if (((AxisMetaTitle *)g_ptr_array_index(titles, i))->ids->len != 1)
				abort();
		}
	}
	g_ptr_array_unref(titles);
}

static void
check_series(
	const gchar *text,
	gsize        length
){
	AxisMetaTitle *title = NULL;
	GError *error = NULL;

	if (!axis_meta_tvdb_parse_series(text, length, "eng", &title, &error)) {
		check_failure(error);
		if (title != NULL)
			abort();
		return;
	}
	check_title(title, TRUE);
	if (title->seasons != NULL) {
		guint i;

		for (i = 0; i < title->seasons->len; i++) {
			if (((AxisMetaSeason *)g_ptr_array_index(title->seasons, i))->episodes != NULL)
				abort();
		}
	}
	axis_meta_title_free(title);
}

static void
check_episodes(
	const gchar *text,
	gsize        length
){
	GPtrArray *seasons = g_ptr_array_new_with_free_func((GDestroyNotify)axis_meta_season_free);
	gsize half = length / 2;
	gboolean more = FALSE;
	guint round;
	guint i;

	/* The whole input as one page, then its two halves as two more: what
	 * pages add up to is checked too. */
	for (round = 0; round < 3; round++) {
		const gchar *page = round == 2 ? text + half : text;
		gsize n = round == 0 ? length : round == 1 ? half : length - half;
		guint before = seasons->len;
		GError *error = NULL;

		if (!axis_meta_tvdb_parse_episodes(page, n, seasons, &more, &error)) {
			check_failure(error);
			if (more || seasons->len != before)
				abort();
		}
	}
	if (seasons->len > AXIS_META_MAX_SEASONS)
		abort();
	for (i = 0; i < seasons->len; i++) {
		const AxisMetaSeason *season = g_ptr_array_index(seasons, i);

		if (!axis_meta_season_check(season, NULL) || season->episodes == NULL ||
		    season->episodes->len > AXIS_META_MAX_EPISODES || (i > 0 &&
		    ((AxisMetaSeason *)g_ptr_array_index(seasons, i - 1))->number >= season->number))
			abort();
	}
	g_ptr_array_unref(seasons);
}

static void
check_orders(
	const gchar *text,
	gsize        length
){
	GPtrArray *names = NULL;
	GError *error = NULL;

	if (!axis_meta_tvdb_parse_orders(text, length, &names, &error)) {
		check_failure(error);
		if (names != NULL)
			abort();
		return;
	}
	if (names == NULL || names->len > AXIS_META_MAX_SOURCES * 4)
		abort();
	g_ptr_array_unref(names);
}

static void
check_login(
	const gchar *text,
	gsize        length
){
	gchar *token = NULL;
	GError *error = NULL;
	const gchar *p;

	if (!axis_meta_tvdb_parse_login(text, length, &token, &error)) {
		check_failure(error);
		if (token != NULL)
			abort();
		return;
	}
	if (token == NULL || token[0] == '\0' || strlen(token) > AXIS_META_TOKEN_MAX_BYTES)
		abort();
	for (p = token; *p != '\0'; p++) {
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
			abort();
	}
	g_free(token);
}

static void
check_forms(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar *ref = axis_meta_tvdb_image_ref(copy);
	gchar *reason = NULL;
	guint size;

	/* An image: a path on the one origin, which the path for every size
	 * gives back as it is. */
	if (ref != NULL) {
		check_image(ref);
		if (!g_str_has_prefix(copy, "https://artworks.thetvdb.com/") || strcmp(copy + 28, ref) != 0)
			abort();
		for (size = 0; size < AXIS_META_ART_N_SIZES; size++) {
			gchar *path = axis_meta_tvdb_image_path(ref, (AxisMetaArtSize)size);

			if (path == NULL || strcmp(path, ref) != 0)
				abort();
			g_free(path);
		}
	}
	g_free(ref);
	/* A path of any other form has none. */
	ref = axis_meta_tvdb_image_path(copy, AXIS_META_ART_POSTER);
	if (ref != NULL)
		check_image(ref);
	g_free(ref);
	/* A key or a PIN: refused with a reason that does not hold it. */
	if (!axis_meta_tvdb_secret_valid(copy, &reason)) {
		if (reason == NULL || (strlen(copy) >= 8 && strstr(reason, copy) != NULL))
			abort();
	} else if (reason != NULL || strlen(copy) > 255 || strchr(copy, '"') != NULL || strchr(copy, ' ') != NULL) {
		abort();
	}
	g_free(reason);
	if (axis_meta_tvdb_language_valid(copy) && strlen(copy) != 3)
		abort();
	/* The orders and their sources map to each other. */
	if (axis_meta_tvdb_source_of_order(copy) != NULL &&
	    g_strcmp0(axis_meta_tvdb_order_of_source(axis_meta_tvdb_source_of_order(copy)), copy) != 0)
		abort();
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
		check_search(0, text, length);
		break;
	case '1':
		check_series(text, length);
		break;
	case '2':
		check_search(1, text, length);
		break;
	case '3':
		check_episodes(text, length);
		break;
	case '4':
		check_orders(text, length);
		break;
	case '5':
		check_login(text, length);
		break;
	case '6':
		check_forms(text, length);
		break;
	default:
		break;
	}
	return 0;
}
