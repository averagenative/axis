/*
 * fuzz-storage-path.c - libFuzzer target for the checks on volume and root
 * paths and names typed into forms
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked (an absolute path, a relative
 * path, a name); the rest is the input.  Besides memory safety: what is
 * accepted is valid UTF-8 without control characters, has no "." or ".."
 * segment, no empty segment and no trailing slash, is within its bounds,
 * and normalising it again gives it back unchanged.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "storage/axis-storage-path.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_path(
	const gchar *path,
	gboolean     absolute
){
	gchar **segments;
	gchar *again = NULL;
	guint i;
	gsize length = strlen(path);
	const gchar *p;

	if (!g_utf8_validate(path, -1, NULL) || length > AXIS_STORAGE_PATH_MAX)
		abort();
	for (p = path; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || (guchar)*p == 0x7f)
			abort();
	}
	if (absolute && (path[0] != '/' || length < 2))
		abort();
	if (!absolute && path[0] == '/')
		abort();
	if (length > 0 && path[length - 1] == '/')
		abort();

	segments = g_strsplit(absolute ? path + 1 : path, "/", -1);
	for (i = 0; length > 0 && segments[i] != NULL; i++) {
		if (segments[i][0] == '\0' || strcmp(segments[i], ".") == 0 || strcmp(segments[i], "..") == 0 ||
		    strlen(segments[i]) > AXIS_STORAGE_SEGMENT_MAX)
			abort();
	}
	if (i > AXIS_STORAGE_DEPTH_MAX)
		abort();
	g_strfreev(segments);

	/* Stable: its normal form is itself. */
	if (!axis_storage_path_normalize(path, absolute, &again, NULL) || strcmp(again, path) != 0)
		abort();
	if (!axis_storage_path_contains(path, path))
		abort();
	g_free(again);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	gchar *text;
	gchar *out = NULL;
	const gchar *problem = NULL;

	if (size == 0)
		return 0;
	text = g_strndup((const gchar *)data + 1, size - 1);

	switch (data[0] % 3) {
	case 0:
	case 1:
		if (axis_storage_path_normalize(text, data[0] % 3 == 0, &out, &problem)) {
			if (problem != NULL)
				abort();
			check_path(out, data[0] % 3 == 0);
		} else if (problem == NULL || out != NULL) {
			abort();
		}
		break;
	default:
		if (axis_storage_label_normalize(text, &out, &problem)) {
			gchar *again = NULL;

			if (!g_utf8_validate(out, -1, NULL) || g_utf8_strlen(out, -1) < 1 ||
			    g_utf8_strlen(out, -1) > AXIS_STORAGE_LABEL_MAX_CHARS)
				abort();
			if (!axis_storage_label_normalize(out, &again, NULL) || strcmp(again, out) != 0)
				abort();
			g_free(again);
		} else if (problem == NULL || out != NULL) {
			abort();
		}
		break;
	}

	g_free(out);
	g_free(text);
	return 0;
}
