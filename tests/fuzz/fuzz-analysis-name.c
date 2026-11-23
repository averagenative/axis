/*
 * fuzz-analysis-name.c - libFuzzer target for the name normaliser
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is split at newlines into up to three texts (a path, an artist
 * and an album, a title).  Whatever they are, the functions must not
 * crash or read past them, and:
 *
 *  - normalising gives valid UTF-8 with no leading, trailing or doubled
 *    space and none of | (the keys' separator), and normalising the result
 *    again changes nothing;
 *  - the same input gives the same key twice;
 *  - a film key has a non-empty normalised title and a year from 1900 to
 *    2099; an episode key a non-empty show, a season up to 999 and an
 *    episode up to 9999;
 *  - a path that is not read as a film or an episode has no key, and the
 *    parsers' outputs are left empty.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/axis-analysis-name.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define FAIL(why) do { fprintf(stderr, "fuzz-analysis-name: %s\n", why); abort(); } while (0)

static void
check_normal(const gchar *text)
{
	gchar *again;

	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL))
		FAIL("normalised text is not UTF-8");
	if (strchr(text, '|') != NULL)
		FAIL("normalised text has the key separator");
	if (text[0] == ' ' || (text[0] != '\0' && text[strlen(text) - 1] == ' ') || strstr(text, "  ") != NULL)
		FAIL("normalised text has stray spaces");
	again = axis_analysis_normalise(text);
	if (again == NULL || strcmp(again, text) != 0) {
		fprintf(stderr, "'%s' became '%s'\n", text, again != NULL ? again : "(null)");
		FAIL("normalising is not idempotent");
	}
	g_free(again);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *text = g_strndup((const gchar *)data, size);
	gchar **parts = g_strsplit(text, "\n", 3);
	const gchar *first = parts[0] != NULL ? parts[0] : "";
	const gchar *second = parts[0] != NULL && parts[1] != NULL ? parts[1] : "";
	const gchar *third = parts[0] != NULL && parts[1] != NULL && parts[2] != NULL ? parts[2] : "";
	gchar *normal = axis_analysis_normalise(first);
	gchar *film;
	gchar *again;
	gchar *episode;
	gchar *title = (gchar *)0x1;
	gchar *show = (gchar *)0x1;
	gint year = -1;
	gint show_year = -1;
	gint season = -1;
	gint number = -1;
	gboolean is_film;
	gboolean is_episode;

	check_normal(normal);
	g_free(normal);

	film = axis_analysis_film_key(first);
	again = axis_analysis_film_key(first);
	if (g_strcmp0(film, again) != 0)
		FAIL("a film key is not stable");
	g_free(again);
	is_film = axis_analysis_parse_film(first, &title, &year);
	if (is_film != (film != NULL))
		FAIL("the film key and the film parse disagree");
	if (is_film) {
		if (title == NULL || title[0] == '\0' || year < 1900 || year > 2099)
			FAIL("a film without a title or with a year out of range");
		check_normal(title);
		g_free(title);
	} else if (title != NULL || year != 0) {
		FAIL("a film parse that failed left something");
	}
	g_free(film);

	episode = axis_analysis_episode_key(first);
	again = axis_analysis_episode_key(first);
	if (g_strcmp0(episode, again) != 0)
		FAIL("an episode key is not stable");
	g_free(again);
	is_episode = axis_analysis_parse_episode(first, &show, &show_year, &season, &number);
	if (is_episode != (episode != NULL))
		FAIL("the episode key and the episode parse disagree");
	if (is_episode) {
		if (show == NULL || show[0] == '\0' || (show_year != 0 && (show_year < 1900 || show_year > 2099)) ||
		    season < 0 || season > 999 || number < 0 || number > 9999)
			FAIL("an episode out of range");
		check_normal(show);
		g_free(show);
	} else if (show != NULL || show_year != 0 || season != 0 || number != 0) {
		FAIL("an episode parse that failed left something");
	}
	g_free(episode);

	{
		gchar *track = axis_analysis_track_key(first, second, third);
		gchar *album = axis_analysis_album_key(first, second);
		gchar *series = axis_analysis_show_key(first);

		g_free(track);
		g_free(album);
		g_free(series);
	}

	g_strfreev(parts);
	g_free(text);
	return 0;
}
