/*
 * axis-indexer-jackett-parse.c - reading what GitHub answers about Jackett's
 * definitions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "indexer/axis-indexer-jackett-parse.h"

#include <string.h>

#include "probe/axis-probe-json.h"

void
axis_jackett_file_free(AxisJackettFile *file)
{
	if (file == NULL)
		return;
	g_free(file->name);
	g_free(file->blob);
	g_free(file);
}

gboolean
axis_jackett_name_valid(const gchar *name)
{
	gsize length;
	gsize stem;
	gsize i;

	if (name == NULL)
		return FALSE;
	length = strlen(name);
	if (length < 5 || length > AXIS_JACKETT_MAX_STEM_BYTES + 4 || strcmp(name + length - 4, ".yml") != 0)
		return FALSE;
	stem = length - 4;
	for (i = 0; i < stem; i++) {
		gchar c = name[i];
		gboolean plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

		if (!plain && !(c == '-' && i > 0))
			return FALSE;
	}
	return TRUE;
}

gboolean
axis_jackett_id_valid(const gchar *text)
{
	gsize i;

	if (text == NULL || strlen(text) != 40)
		return FALSE;
	for (i = 0; i < 40; i++) {
		gchar c = text[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return FALSE;
	}
	return TRUE;
}

gchar *
axis_jackett_parse_commit(
	const gchar *data,
	gsize        length
){
	gsize start = 0;
	gsize end = length;
	gchar *id;

	if (data == NULL)
		return NULL;
	while (start < end && (data[start] == ' ' || data[start] == '\n' || data[start] == '\r' || data[start] == '\t'))
		start++;
	while (end > start && (data[end - 1] == ' ' || data[end - 1] == '\n' || data[end - 1] == '\r' ||
	       data[end - 1] == '\t'))
		end--;
	if (end - start != 40)
		return NULL;
	id = g_strndup(data + start, 40);
	if (!axis_jackett_id_valid(id)) {
		g_free(id);
		return NULL;
	}
	return id;
}

static gint
by_name(
	gconstpointer a,
	gconstpointer b
){
	const AxisJackettFile *one = *(const AxisJackettFile *const *)a;
	const AxisJackettFile *other = *(const AxisJackettFile *const *)b;

	return strcmp(one->name, other->name);
}

gboolean
axis_jackett_parse_listing(
	const gchar  *data,
	gsize         length,
	GPtrArray   **files,
	guint        *n_left_out,
	gboolean     *cut,
	gchar       **problem
){
	g_autoptr(AxisJsonDoc) doc = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autofree gchar *why = NULL;
	const AxisJsonNode *root;
	GPtrArray *found;
	guint n;
	guint i;

	*files = NULL;
	*n_left_out = 0;
	*cut = FALSE;
	if (problem != NULL)
		*problem = NULL;
	doc = data != NULL && length > 0 ? axis_json_parse(data, length, &why) : NULL;
	root = doc != NULL ? axis_json_doc_get_root(doc) : NULL;
	if (root == NULL || axis_json_type(root) != AXIS_JSON_ARRAY) {
		if (problem != NULL)
			*problem = g_strdup_printf("GitHub's answer is not a list of files%s%s.", why != NULL ? ": " : "",
				why != NULL ? why : "");
		return FALSE;
	}
	n = axis_json_length(root);
	*cut = n >= AXIS_JACKETT_LISTING_CAP;
	found = g_ptr_array_new_with_free_func((GDestroyNotify)axis_jackett_file_free);
	seen = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < n; i++) {
		const AxisJsonNode *entry = axis_json_element(root, i);
		const gchar *type = axis_json_string(axis_json_member(entry, "type"));
		const gchar *name = axis_json_string(axis_json_member(entry, "name"));
		const gchar *blob = axis_json_string(axis_json_member(entry, "sha"));
		const AxisJsonNode *size_node = axis_json_member(entry, "size");
		AxisJackettFile *file;
		gint64 size = 0;

		if (i >= AXIS_JACKETT_MAX_ENTRIES) {
			*n_left_out += n - i;
			*cut = TRUE;
			break;
		}
		if (axis_json_type(entry) != AXIS_JSON_OBJECT || g_strcmp0(type, "file") != 0 ||
		    !axis_jackett_name_valid(name) || !axis_jackett_id_valid(blob) ||
		    axis_json_type(size_node) != AXIS_JSON_NUMBER ||
		    !axis_json_get_integer(size_node, 0, G_MAXINT32, &size) || g_hash_table_contains(seen, name)) {
			(*n_left_out)++;
			continue;
		}
		file = g_new0(AxisJackettFile, 1);
		file->name = g_strdup(name);
		file->blob = g_strdup(blob);
		file->size_bytes = size;
		g_hash_table_add(seen, file->name);
		g_ptr_array_add(found, file);
	}
	g_ptr_array_sort(found, by_name);
	*files = found;
	return TRUE;
}

gchar *
axis_jackett_blob_id(
	const gchar *data,
	gsize        length
){
	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
	g_autofree gchar *head = g_strdup_printf("blob %" G_GSIZE_FORMAT, length);
	gchar *id;

	/* The header ends with its NUL. */
	g_checksum_update(sum, (const guchar *)head, (gssize)strlen(head) + 1);
	if (length > 0)
		g_checksum_update(sum, (const guchar *)data, (gssize)length);
	id = g_strdup(g_checksum_get_string(sum));
	g_checksum_free(sum);
	return id;
}
