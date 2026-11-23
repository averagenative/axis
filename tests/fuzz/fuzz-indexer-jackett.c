/*
 * fuzz-indexer-jackett.c - libFuzzer target for the reading of what GitHub
 * answers about Jackett's definitions and of the names a form sends back
 * (indexer/axis-indexer-jackett-parse.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The input is an answer.  Whatever it is, the code returns without
 * crashing or allocating without bound, and:
 *
 *  - a listing gives files or a reason, never both; every file's name is of
 *    the one accepted form (lower-case letters, digits and hyphens, then
 *    ".yml": never a path), each name once, in order; every blob id is 40
 *    hex digits in lower case; no size is negative; no more files than
 *    entries are read;
 *  - a commit, when one is read, is 40 hex digits in lower case;
 *  - the input taken as a name is accepted only in that form.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "indexer/axis-indexer-jackett-parse.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_name(const gchar *name)
{
	gsize length = strlen(name);
	gsize i;

	if (length < 5 || length > AXIS_JACKETT_MAX_STEM_BYTES + 4 || strcmp(name + length - 4, ".yml") != 0)
		abort();
	if (name[0] == '-' || name[0] == '.')
		abort();
	for (i = 0; i < length - 4; i++) {
		if (strchr("abcdefghijklmnopqrstuvwxyz0123456789-", name[i]) == NULL)
			abort();
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const gchar *text = (const gchar *)data;
	GPtrArray *files = NULL;
	gchar *problem = NULL;
	gchar *commit;
	gchar *name;
	gchar *blob;
	guint left_out = 0;
	gboolean cut = FALSE;
	guint i;

	if (size > 1024 * 1024)
		return 0;
	if (axis_jackett_parse_listing(text, size, &files, &left_out, &cut, &problem)) {
		if (files == NULL || problem != NULL || files->len > AXIS_JACKETT_MAX_ENTRIES)
			abort();
		for (i = 0; i < files->len; i++) {
			const AxisJackettFile *file = g_ptr_array_index(files, i);

			if (file->name == NULL || !axis_jackett_name_valid(file->name) || !axis_jackett_id_valid(file->blob) ||
			    file->size_bytes < 0)
				abort();
			check_name(file->name);
			if (i > 0 && strcmp(((const AxisJackettFile *)g_ptr_array_index(files, i - 1))->name, file->name) >= 0)
				abort();
		}
		g_ptr_array_unref(files);
	} else {
		if (files != NULL || problem == NULL || !g_utf8_validate(problem, -1, NULL))
			abort();
		g_free(problem);
	}

	commit = axis_jackett_parse_commit(text, size);
	if (commit != NULL && (!axis_jackett_id_valid(commit) || strlen(commit) != 40))
		abort();
	g_free(commit);

	name = g_strndup(text, size);
	if (axis_jackett_name_valid(name))
		check_name(name);
	g_free(name);

	blob = axis_jackett_blob_id(text, size);
	if (!axis_jackett_id_valid(blob))
		abort();
	g_free(blob);
	return 0;
}
