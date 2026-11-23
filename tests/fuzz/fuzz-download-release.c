/*
 * fuzz-download-release.c - libFuzzer target for what a download client or
 * a caller hands the downloads layer: magnet links, names, a client's
 * items, and a client's paths through the path mappings
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked; the rest is the input.
 *
 *  0  a magnet link: if accepted it starts with the btih prefix, is within
 *     the limit, and its hash is 40 lower-case hex digits;
 *  1  a name or a message: the cleaned text is valid UTF-8, without
 *     control characters, within its length, and cleaning it again
 *     changes nothing;
 *  2  a client's item, fields split on newlines: if the layer accepts it,
 *     its identifier is printable ASCII, its name clean, its hash valid or
 *     absent, its path free of control characters, its figures sane;
 *  3  a client's path through fixed mappings: a mapped result is a
 *     non-empty relative path with no '.' or '..' segment, on the volume
 *     of the longest prefix containing the path.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "download/axis-download-model.h"
#include "download/axis-download-path.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_clean(
	const gchar *text,
	guint        max_chars
){
	const gchar *p;
	gchar *again;

	if (text == NULL)
		return;
	if (!g_utf8_validate(text, -1, NULL) || text[0] == '\0' || g_utf8_strlen(text, -1) > (glong)max_chars)
		abort();
	for (p = text; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			abort();
	}
	again = axis_download_text_clean(text, max_chars);
	if (again == NULL || strcmp(again, text) != 0)
		abort();
	g_free(again);
}

static GPtrArray *
fixed_maps(void)
{
	GPtrArray *maps = g_ptr_array_new_with_free_func((GDestroyNotify)axis_download_path_map_free);

	g_ptr_array_add(maps, axis_download_path_map_new("/downloads", 1, "dl"));
	g_ptr_array_add(maps, axis_download_path_map_new("/downloads/tv", 2, ""));
	g_ptr_array_add(maps, axis_download_path_map_new("/data/complete", 3, "media/complete"));
	return maps;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	g_autofree gchar *text = NULL;
	guint choice;

	if (size < 1)
		return 0;
	choice = (guint)(data[0] % 4);
	text = g_strndup((const gchar *)data + 1, size - 1);

	switch (choice) {
	case 0: {
		gchar *hash = NULL;
		const gchar *problem = NULL;

		if (axis_download_magnet_parse(text, &hash, &problem)) {
			if (g_ascii_strncasecmp(text, "magnet:?xt=urn:btih:", 20) != 0 ||
			    strlen(text) > AXIS_DOWNLOAD_MAGNET_MAX || !axis_download_hash_valid(hash))
				abort();
		} else if (problem == NULL || hash != NULL) {
			abort();
		}
		g_free(hash);
		break;
	}
	case 1: {
		gchar *clean = axis_download_text_clean(text, 40);

		check_clean(clean, 40);
		g_free(clean);
		break;
	}
	case 2: {
		gchar **fields = g_strsplit(text, "\n", 8);
		AxisDownloadItem *item = axis_download_item_new();
		guint n = g_strv_length(fields);

		item->client_ref = n > 0 ? g_strdup(fields[0]) : NULL;
		item->name = n > 1 ? g_strdup(fields[1]) : NULL;
		item->category = n > 2 ? g_strdup(fields[2]) : NULL;
		item->info_hash = n > 3 && fields[3][0] != '\0' ? g_strdup(fields[3]) : NULL;
		item->path = n > 4 && fields[4][0] != '\0' ? g_strdup(fields[4]) : NULL;
		item->message = n > 5 ? g_strdup(fields[5]) : NULL;
		item->state = (AxisDownloadItemState)(n > 6 ? (guint)strlen(fields[6]) % 7 : 0);
		item->size_bytes = n > 7 ? g_ascii_strtoll(fields[7], NULL, 10) : -1;
		item->done_bytes = (gint64)size * 1000 - 5000;
		if (axis_download_item_check(item)) {
			const gchar *p;

			if (!axis_download_ref_valid(item->client_ref) || item->name == NULL)
				abort();
			check_clean(item->name, AXIS_DOWNLOAD_NAME_MAX_CHARS);
			check_clean(item->message, AXIS_DOWNLOAD_MESSAGE_MAX_CHARS);
			if (item->info_hash != NULL && !axis_download_hash_valid(item->info_hash))
				abort();
			for (p = item->path; p != NULL && *p != '\0'; p++) {
				if ((guchar)*p < 0x20 || *p == 0x7f)
					abort();
			}
			if (item->size_bytes < -1 || item->done_bytes < -1 ||
			    (item->size_bytes >= 0 && item->done_bytes > item->size_bytes))
				abort();
		}
		axis_download_item_free(item);
		g_strfreev(fields);
		break;
	}
	default: {
		GPtrArray *maps = fixed_maps();
		gint64 volume = 0;
		gchar *rel = NULL;
		const gchar *problem = NULL;
		AxisDownloadPathResult result = axis_download_path_apply(maps, text, &volume, &rel, &problem);

		if (result == AXIS_DOWNLOAD_PATH_MAPPED) {
			gchar **segments;
			guint i;

			if (rel == NULL || rel[0] == '\0' || rel[0] == '/' || volume < 1 || volume > 3 ||
			    !g_utf8_validate(rel, -1, NULL) || g_str_has_suffix(rel, "/") || strstr(rel, "//") != NULL)
				abort();
			segments = g_strsplit(rel, "/", -1);
			for (i = 0; segments[i] != NULL; i++) {
				if (strcmp(segments[i], ".") == 0 || strcmp(segments[i], "..") == 0)
					abort();
			}
			g_strfreev(segments);
			/* The longest prefix wins. */
			if (volume == 1 && (g_str_has_prefix(text, "/downloads/tv/") || strcmp(text, "/downloads/tv") == 0))
				abort();
		} else if (rel != NULL || problem == NULL) {
			abort();
		}
		g_free(rel);
		g_ptr_array_unref(maps);
		break;
	}
	}
	return 0;
}
