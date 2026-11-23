/*
 * fuzz-download-bencode.c - libFuzzer target for the bencode reader that
 * finds a .torrent's info-hash
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Whatever the bytes: the reader returns, reads nothing beyond them, and
 * when it accepts them the hash is 40 lower-case hex digits, the whole of
 * the input is one valid bencoded value, the name (if any) is clean text,
 * and reading the same bytes again gives the same hash.  An input that
 * axis_bencode_valid() refuses is never accepted as a torrent.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "download/axis-download-bencode.h"
#include "download/axis-download-model.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* A copy of exactly @size bytes, so that a read past the end is caught. */
	guint8 *copy = g_memdup2(data, size);
	gchar *hash = NULL;
	gchar *name = NULL;
	gchar *again = NULL;
	const gchar *problem = NULL;
	gboolean valid = axis_bencode_valid(copy, size);

	if (axis_download_torrent_read(copy, size, &hash, &name, &problem)) {
		if (!valid || !axis_download_hash_valid(hash) || problem != NULL)
			abort();
		if (name != NULL && (!g_utf8_validate(name, -1, NULL) || name[0] == '\0' ||
		                     g_utf8_strlen(name, -1) > AXIS_DOWNLOAD_NAME_MAX_CHARS))
			abort();
		if (!axis_download_torrent_read(copy, size, &again, NULL, NULL) || strcmp(again, hash) != 0)
			abort();
	} else if (problem == NULL || hash != NULL || name != NULL) {
		abort();
	}
	g_free(hash);
	g_free(name);
	g_free(again);
	g_free(copy);
	return 0;
}
