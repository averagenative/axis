/*
 * fuzz-import-subtitle.c - libFuzzer target for the subtitle name reader and
 * the names subtitles are given
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is a subtitle file's name (any bytes); a second part after 0x1f,
 * when there is one, is the film's stem.  When the name reads as a subtitle:
 * its language is a two- or three-letter code or none, its extension one of
 * the subtitle extensions, and the name it is given is at most 255 bytes,
 * begins with the stem (cut on a character boundary when it must be), ends
 * with the extension, says its language (or "und" and a number) and is valid
 * UTF-8 whenever the stem is.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "import/axis-import-name.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const guint8 *split;
	gsize name_length;
	g_autofree gchar *stem = NULL;
	g_autofree gchar *named = NULL;
	g_autofree gchar *mark = NULL;
	AxisImportSubtitle subtitle;
	guint number;

	if (size > 4096)
		return 0;
	split = memchr(data, 0x1f, size);
	name_length = split != NULL ? (gsize)(split - data) : size;
	if (!axis_import_subtitle_read((const gchar *)data, name_length, &subtitle))
		return 0;
	if (subtitle.extension == NULL || subtitle.extension[0] != '.')
		abort();
	if (subtitle.language != NULL && (strlen(subtitle.language) < 2 || strlen(subtitle.language) > 3))
		abort();
	/* The stem: the rest, or a fixed one; valid UTF-8 as names Axis gives
	 * are. */
	if (split != NULL) {
		g_autofree gchar *raw = g_strndup((const gchar *)split + 1, size - name_length - 1);

		stem = axis_import_name_clean(raw, -1);
	} else {
		stem = g_strdup("Hearth (1995) {tmdb-6001}");
	}
	number = size % 7;
	named = axis_import_subtitle_name(stem, &subtitle, number);
	if (strlen(named) > AXIS_IMPORT_COMPONENT_MAX)
		abort();
	if (!g_utf8_validate(named, -1, NULL))
		abort();
	if (!g_str_has_suffix(named, subtitle.extension))
		abort();
	mark = g_strdup_printf(".%s", subtitle.language != NULL ? subtitle.language : "und");
	if (strstr(named, mark) == NULL)
		abort();
	/* The whole stem whenever it leaves room for the marks (at most 24
	 * bytes: ".und.forced.sdh.6.srt"). */
	if (strlen(stem) + 24 <= AXIS_IMPORT_COMPONENT_MAX && !g_str_has_prefix(named, stem))
		abort();
	return 0;
}
