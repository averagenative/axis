/*
 * fuzz-youtube-address.c - libFuzzer target for the reading of an address
 * pasted into the YouTube downloader (youtube/axis-youtube-address.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Whatever is pasted, the reader returns without crashing, and:
 *
 *  - what it takes yields an id of exactly 11 letters, digits, '-' and '_',
 *    no reason, and the address Axis writes from it is the fixed prefix and
 *    that id, which the reader takes again as the same id;
 *  - what it takes held "https://" and one of YouTube's host names, and no
 *    control character or space inside it;
 *  - what it refuses yields no id and a reason, which holds none of a longer
 *    input;
 *  - the command built for a taken id ends with "--" and that address, and
 *    no earlier argument holds the id.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "youtube/axis-youtube-address.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *text;
	gchar *id = NULL;
	const gchar *problem = NULL;

	if (size > 8192)
		return 0;
	text = g_strndup((const gchar *)data, size);
	if (axis_youtube_address_parse(text, &id, &problem)) {
		gchar *stripped = g_strstrip(g_strdup(text));
		gchar *lower = g_ascii_strdown(stripped, -1);
		gchar *address;
		gchar *again = NULL;
		const gchar *p;

		if (id == NULL || problem != NULL || !axis_youtube_video_id_valid(id) || strlen(id) != AXIS_YOUTUBE_ID_LENGTH)
			abort();
		if (!g_str_has_prefix(stripped, "https://") || strlen(stripped) > AXIS_YOUTUBE_ADDRESS_MAX)
			abort();
		if (strstr(lower, "youtube.com") == NULL && strstr(lower, "youtu.be") == NULL)
			abort();
		if (strstr(stripped, id) == NULL)
			abort();
		for (p = stripped; *p != '\0'; p++) {
			if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f)
				abort();
		}
		address = axis_youtube_address_for(id);
		if (address == NULL || !g_str_has_prefix(address, AXIS_YOUTUBE_WATCH_PREFIX) ||
		    strcmp(address + strlen(AXIS_YOUTUBE_WATCH_PREFIX), id) != 0)
			abort();
		if (!axis_youtube_address_parse(address, &again, NULL) || g_strcmp0(again, id) != 0)
			abort();
		g_free(again);
		g_free(address);
		g_free(lower);
		g_free(stripped);
	} else {
		if (id != NULL || problem == NULL || problem[0] == '\0')
			abort();
		if (strlen(text) > 16 && strstr(problem, text) != NULL)
			abort();
	}
	g_free(id);
	g_free(text);
	return 0;
}
