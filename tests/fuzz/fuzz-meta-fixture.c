/*
 * fuzz-meta-fixture.c - libFuzzer target for the fixture metadata
 * provider's parser (tests/axis-test-meta-fixture-parse.h) and the model's
 * cleaning of what providers say (meta/axis-meta-model.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is read ('0' film results, '1' series
 * results, '2' a film, '3' a series, '4' a season, '5' search text); the
 * rest is the input.  Whatever it is, the parser returns without crashing
 * or allocating without bound, and what it returns passes the checks the
 * store applies before anything is kept: axis_meta_title_check() for every
 * title, axis_meta_season_check() for a season.  A failure is always an
 * %AXIS_META_ERROR_UNREADABLE and returns nothing.  Search text is cleaned
 * the same way twice, its key is the same for the same text, valid UTF-8 and
 * at most 300 bytes, and cleaning what was cleaned changes nothing.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../axis-test-meta-fixture-parse.h"
#include "meta/axis-meta-provider.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_failure(GError *error)
{
	if (error == NULL || error->domain != AXIS_META_ERROR || error->code != AXIS_META_ERROR_UNREADABLE)
		abort();
}

static void
check_results(
	AxisMetaKind kind,
	const gchar *text,
	gsize        length
){
	GPtrArray *titles = NULL;
	GError *error = NULL;
	guint i;

	if (!axis_test_meta_fixture_parse_results("fixture", kind, text, length, &titles, &error)) {
		check_failure(error);
		if (titles != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (titles == NULL || titles->len > AXIS_META_MAX_RESULTS)
		abort();
	for (i = 0; i < titles->len; i++) {
		const AxisMetaTitle *title = g_ptr_array_index(titles, i);

		if (title->kind != kind || title->detail || !axis_meta_title_check(title, "fixture", NULL))
			abort();
	}
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

	if (!axis_test_meta_fixture_parse_title("fixture", kind, text, length, &title, &error)) {
		check_failure(error);
		if (title != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (title == NULL || title->kind != kind || !title->detail || !axis_meta_title_check(title, "fixture", NULL))
		abort();
	axis_meta_title_free(title);
}

static void
check_season(
	const gchar *text,
	gsize        length
){
	AxisMetaSeason *season = NULL;
	GError *error = NULL;

	if (!axis_test_meta_fixture_parse_season(1, text, length, &season, &error)) {
		check_failure(error);
		if (season != NULL)
			abort();
		g_error_free(error);
		return;
	}
	if (season == NULL || season->number != 1 || !axis_meta_season_check(season, NULL))
		abort();
	axis_meta_season_free(season);
}

static void
check_query(
	const gchar *text,
	gsize        length
){
	gchar *copy = g_strndup(text, length);
	gchar *clean = axis_meta_query_clean(copy);
	gchar *again = axis_meta_query_clean(clean);
	gchar *key = axis_meta_query_key(copy);
	gchar *key_again = axis_meta_query_key(copy);
	gchar *recleaned = clean != NULL ? axis_meta_clean_text(clean, AXIS_META_QUERY_MAX_CHARS) : NULL;

	if ((clean == NULL) != (again == NULL) || (clean != NULL && strcmp(clean, again) != 0))
		abort();
	if (clean != NULL && (!g_utf8_validate(clean, -1, NULL) || g_utf8_strlen(clean, -1) > AXIS_META_QUERY_MAX_CHARS))
		abort();
	if (clean != NULL && strcmp(recleaned, clean) != 0)
		abort();
	if ((key == NULL) != (key_again == NULL) || (key != NULL && strcmp(key, key_again) != 0))
		abort();
	if (key != NULL && (!g_utf8_validate(key, -1, NULL) || strlen(key) > 300 || key[0] == '\0'))
		abort();
	g_free(copy);
	g_free(clean);
	g_free(again);
	g_free(key);
	g_free(key_again);
	g_free(recleaned);
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
		check_results(AXIS_META_KIND_FILM, text, length);
		break;
	case '1':
		check_results(AXIS_META_KIND_SERIES, text, length);
		break;
	case '2':
		check_title(AXIS_META_KIND_FILM, text, length);
		break;
	case '3':
		check_title(AXIS_META_KIND_SERIES, text, length);
		break;
	case '4':
		check_season(text, length);
		break;
	case '5':
		check_query(text, length);
		break;
	default:
		break;
	}
	return 0;
}
