/*
 * fuzz-identify-episode.c - libFuzzer target for what identification reads
 * from a series' folders and episode files (identify/axis-identify-rules.h:
 * axis_identify_season_folder(), axis_identify_episodes_read(),
 * axis_identify_series_name(), and through them axis_release_states())
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked; the rest is the input.
 *
 *  0  any bytes as a folder's name: a season from 0 to 10000 or -1, and a
 *     season is only found in a name that starts with "s" (Season, Series,
 *     S01, Specials) once spaces are taken off.
 *  1  a file name and a season folder (the second byte: -1 to 30): what its
 *     name holds is either a reason (no date, no episodes) or a date or
 *     episodes (not both), at most AXIS_IDENTIFY_FILE_EPISODES of them, each
 *     once; a season folder's disagreement only with a season folder; the
 *     words are a non-empty sentence of valid UTF-8.
 *  2  up to 16 file names, one per line: the name the series is read by is
 *     one of them that reads as an episode of a title, or none; what they
 *     hold together (axis_identify_shape_add()) counts every file once, in
 *     one of its four kinds, and each episode once, ascending.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "identify/axis-identify-rules.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
season_folder(
	const gchar *input,
	gsize        length
){
	g_autofree gchar *name = g_strndup(input, length);
	g_autofree gchar *trimmed = NULL;
	gint season = axis_identify_season_folder(name);

	if (season < -1 || season > 10000)
		abort();
	if (season >= 0) {
		trimmed = g_strstrip(g_strdup(name));
		if (trimmed[0] != 's' && trimmed[0] != 'S')
			abort();
	}
}

static void
episodes(
	const gchar *input,
	gsize        length,
	gint         folder_season
){
	g_autofree gchar *name = g_strndup(input, length);
	g_autoptr(AxisIdentifyName) read = NULL;
	AxisIdentifyEpisodes out;
	guint i;
	guint k;

	if (!g_utf8_validate(name, -1, NULL))
		return;
	read = axis_identify_name_read(name, 2026);
	axis_identify_episodes_read(read->release, folder_season, &out);
	if (out.text == NULL || out.text[0] == '\0' || !g_utf8_validate(out.text, -1, NULL))
		abort();
	if (out.code != NULL) {
		if (out.n != 0 || out.date[0] != '\0')
			abort();
		if (strcmp(out.code, "season_folder") == 0 && folder_season < 0)
			abort();
		if (strcmp(out.code, "no_episode") != 0 && strcmp(out.code, "absolute_only") != 0 &&
		    strcmp(out.code, "cross_season") != 0 && strcmp(out.code, "season_pack") != 0 &&
		    strcmp(out.code, "season_folder") != 0)
			abort();
	} else {
		if ((out.date[0] != '\0') == (out.n > 0))
			abort();
		if (out.n > AXIS_IDENTIFY_FILE_EPISODES)
			abort();
		if (out.n > 0 && (out.season < 0 || (folder_season >= 0 && out.season != folder_season)))
			abort();
		for (i = 0; i < out.n; i++) {
			for (k = i + 1; k < out.n; k++) {
				if (out.numbers[i] == out.numbers[k])
					abort();
			}
		}
		if (out.date[0] != '\0' && strlen(out.date) != 10)
			abort();
	}
	axis_identify_episodes_clear(&out);
}

static void
series_name(
	const gchar *input,
	gsize        length
){
	g_autofree gchar *copy = g_strndup(input, length);
	g_auto(GStrv) parts = g_strsplit(copy, "\n", 17);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func((GDestroyNotify)axis_identify_name_free);
	gint at;
	guint i;

	for (i = 0; parts[i] != NULL && i < 16; i++) {
		if (!g_utf8_validate(parts[i], -1, NULL))
			continue;
		g_ptr_array_add(names, axis_identify_name_read(parts[i], 2026));
	}
	at = axis_identify_series_name((const AxisIdentifyName *const *)names->pdata, names->len);
	if (at < -1 || at >= (gint)names->len)
		abort();
	if (at >= 0) {
		const AxisIdentifyName *name = g_ptr_array_index(names, at);

		if (name->key == NULL || !axis_release_is_episode(name->release))
			abort();
	}

	/* What the files hold together: every file counted once, each episode
	 * once, ascending; the words valid UTF-8. */
	{
		g_auto(AxisIdentifyShape) shape = { 0 };
		g_autofree gchar *words = NULL;

		axis_identify_shape_init(&shape);
		for (i = 0; i < names->len; i++) {
			const AxisIdentifyName *name = g_ptr_array_index(names, i);
			AxisIdentifyEpisodes held;

			axis_identify_episodes_read(name->release, (gint)(i % 3) - 1, &held);
			axis_identify_shape_add(&shape, &held);
			axis_identify_episodes_clear(&held);
		}
		axis_identify_shape_finish(&shape);
		if (shape.n_files != names->len ||
		    shape.n_numbered + shape.n_specials + shape.n_dated + shape.n_none != shape.n_files)
			abort();
		for (i = 1; i < shape.episodes->len; i++) {
			if (g_array_index(shape.episodes, gint64, i - 1) >= g_array_index(shape.episodes, gint64, i))
				abort();
		}
		words = axis_identify_shape_describe(&shape);
		if (words == NULL || !g_utf8_validate(words, -1, NULL))
			abort();
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1)
		return 0;
	switch (data[0] % 3) {
	case 0:
		season_folder((const gchar *)data + 1, size - 1);
		break;
	case 1:
		if (size < 2)
			return 0;
		episodes((const gchar *)data + 2, size - 2, (gint)(data[1] % 32) - 1);
		break;
	default:
		series_name((const gchar *)data + 1, size - 1);
		break;
	}
	return 0;
}
