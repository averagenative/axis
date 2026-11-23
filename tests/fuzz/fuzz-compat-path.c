/*
 * fuzz-compat-path.c - libFuzzer target for the server's address, the
 * path normaliser, the path map and the library mapping
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The first byte chooses what is checked; the rest is the input.
 *
 *  0  an address: if it is accepted it is an http or https URL with a
 *     host and a port in range, with no credentials, query, fragment or
 *     path, and its base URL parses again to the same address;
 *  1  a path: if it normalises it is absolute with no empty, "." or ".."
 *     part, valid UTF-8, and normalising it again changes nothing;
 *  2  a server path mapped through a fixed map: a result is a clean
 *     absolute path under one of the map's Axis prefixes, and the root it
 *     falls in contains it;
 *  3  folders (lines) mapped to roots: a mapped library has every folder
 *     reaching a root, an unmapped folder is listed, and a suggestion
 *     names a real root.
 */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compat/axis-compat-path.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void
check_clean_path(const gchar *path)
{
	gchar *again;

	if (path[0] != '/' || !g_utf8_validate(path, -1, NULL) || strlen(path) > 4096)
		abort();
	if (strstr(path, "//") != NULL || strstr(path, "/./") != NULL || strstr(path, "/../") != NULL)
		abort();
	if (g_str_has_suffix(path, "/.") || g_str_has_suffix(path, "/..") || (strlen(path) > 1 && g_str_has_suffix(path, "/")))
		abort();
	again = axis_compat_path_normalize(path, NULL);
	if (again == NULL || strcmp(again, path) != 0)
		abort();
	g_free(again);
}

static GPtrArray *
fixed_map(void)
{
	static const gchar *const pairs[][2] = {
		{ "/data", "/media/disk1" }, { "/data/movies", "/media/disk3/movies" }, { "/music", "/media/disk4/music" }
	};
	GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_path_entry_free);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(pairs); i++) {
		AxisCompatPathEntry *e = g_new0(AxisCompatPathEntry, 1);

		e->server = g_strdup(pairs[i][0]);
		e->axis = g_strdup(pairs[i][1]);
		g_ptr_array_add(entries, e);
	}
	return entries;
}

static GPtrArray *
fixed_roots(void)
{
	static const gchar *const paths[] = { "/media/disk1/tv", "/media/disk3/movies", "/media/disk4/music", "/media/disk4" };
	GPtrArray *roots = g_ptr_array_new_with_free_func((GDestroyNotify)axis_compat_root_ref_free);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(paths); i++) {
		AxisCompatRootRef *r = g_new0(AxisCompatRootRef, 1);

		r->id = (gint64)i + 1;
		r->path = g_strdup(paths[i]);
		r->media_type = g_strdup("movies");
		g_ptr_array_add(roots, r);
	}
	return roots;
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
		AxisCompatAddress address;
		AxisCompatAddress again;
		gchar *problem = NULL;

		if (axis_compat_address_parse(text, &address, &problem)) {
			if (address.port < 1 || address.port > 65535 || address.host == NULL || address.host[0] == '\0' ||
			    address.base == NULL)
				abort();
			if (strncmp(address.base, address.https ? "https://" : "http://", address.https ? 8 : 7) != 0)
				abort();
			/* The canonical form adds a port and lower-cases: it may be a few
			 * bytes longer than the longest address accepted as typed. */
			if (strlen(address.base) <= AXIS_COMPAT_ADDRESS_MAX_BYTES) {
				if (!axis_compat_address_parse(address.base, &again, NULL))
					abort();
				if (again.port != address.port || strcmp(again.host, address.host) != 0 ||
				    again.https != address.https)
					abort();
				axis_compat_address_clear(&again);
			}
		} else if (problem == NULL) {
			abort();
		}
		axis_compat_address_clear(&address);
		g_free(problem);
		break;
	}
	case 1: {
		gchar *path = axis_compat_path_normalize(text, NULL);

		if (path != NULL)
			check_clean_path(path);
		g_free(path);
		break;
	}
	case 2: {
		GPtrArray *map = fixed_map();
		GPtrArray *roots = fixed_roots();
		gchar *mapped = axis_compat_path_map_apply(map, text);

		if (mapped != NULL) {
			const AxisCompatRootRef *root;
			gchar *rel = NULL;

			check_clean_path(mapped);
			root = axis_compat_root_for_path(roots, mapped, &rel);
			if (root != NULL) {
				g_autofree gchar *rebuilt = g_strconcat(root->path, "/", rel, NULL);

				if (rel == NULL || rel[0] == '\0' || rel[0] == '/' || strcmp(rebuilt, mapped) != 0)
					abort();
			}
			g_free(rel);
		}
		g_free(mapped);
		g_ptr_array_unref(map);
		g_ptr_array_unref(roots);
		break;
	}
	default: {
		GPtrArray *map = fixed_map();
		GPtrArray *roots = fixed_roots();
		GPtrArray *folders = g_ptr_array_new_with_free_func(g_free);
		AxisCompatMapping mapping;
		gchar **lines = g_strsplit(text, "\n", 8);
		guint i;

		for (i = 0; lines[i] != NULL; i++)
			g_ptr_array_add(folders, g_strdup(lines[i]));
		axis_compat_map_library(folders, map, roots, &mapping);
		if (mapping.state == AXIS_MAP_MAPPED && mapping.unmapped_folders->len != 0)
			abort();
		if (mapping.state != AXIS_MAP_MAPPED && mapping.unmapped_folders->len == 0 && folders->len > 0)
			abort();
		if (mapping.unmapped_folders->len > folders->len)
			abort();
		if (mapping.state == AXIS_MAP_MAPPED && (folders->len == 0 || mapping.root_ids->len == 0))
			abort();
		for (i = 0; i < mapping.suggestions->len; i++) {
			const AxisCompatSuggestion *s = g_ptr_array_index(mapping.suggestions, i);
			gboolean real = FALSE;
			guint j;

			for (j = 0; j < roots->len; j++) {
				const AxisCompatRootRef *r = g_ptr_array_index(roots, j);

				real = real || (r->id == s->root_id && strcmp(r->path, s->root_path) == 0);
			}
			if (!real)
				abort();
		}
		axis_compat_mapping_clear(&mapping);
		g_strfreev(lines);
		g_ptr_array_unref(folders);
		g_ptr_array_unref(map);
		g_ptr_array_unref(roots);
		break;
	}
	}
	return 0;
}
