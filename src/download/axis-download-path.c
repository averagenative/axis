/*
 * axis-download-path.c - mapping a download client's paths onto volumes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "download/axis-download-path.h"

#include <string.h>

#include "storage/axis-storage-path.h"

AxisDownloadPathMap *
axis_download_path_map_new(
	const gchar *client_prefix,
	gint64       volume_id,
	const gchar *rel_path
){
	AxisDownloadPathMap *map = g_new0(AxisDownloadPathMap, 1);

	map->client_prefix = g_strdup(client_prefix);
	map->volume_id = volume_id;
	map->rel_path = g_strdup(rel_path != NULL ? rel_path : "");
	return map;
}

void
axis_download_path_map_free(AxisDownloadPathMap *map)
{
	if (map == NULL)
		return;
	g_free(map->client_prefix);
	g_free(map->rel_path);
	g_free(map);
}

gboolean
axis_download_path_map_normalize(
	const gchar  *client_prefix,
	const gchar  *rel_path,
	gchar       **prefix_out,
	gchar       **rel_out,
	const gchar **problem
){
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *rel = NULL;
	const gchar *why = NULL;

	if (!axis_storage_path_normalize(client_prefix, TRUE, &prefix, &why)) {
		if (problem != NULL)
			*problem = why != NULL ? why : "the client's path is not valid";
		return FALSE;
	}
	if (!axis_storage_path_normalize(rel_path != NULL ? rel_path : "", FALSE, &rel, &why)) {
		if (problem != NULL)
			*problem = why != NULL ? why : "the path on the volume is not valid";
		return FALSE;
	}
	if (prefix_out != NULL)
		*prefix_out = g_steal_pointer(&prefix);
	if (rel_out != NULL)
		*rel_out = g_steal_pointer(&rel);
	return TRUE;
}

AxisDownloadPathResult
axis_download_path_apply(
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	const gchar     **problem
){
	return axis_download_path_apply_full(maps, client_path, volume_id, rel_path, NULL, problem);
}

AxisDownloadPathResult
axis_download_path_apply_full(
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	gchar           **prefix,
	const gchar     **problem
){
	g_autofree gchar *path = NULL;
	const AxisDownloadPathMap *best = NULL;
	const gchar *why = NULL;
	const gchar *rest;
	gsize best_length = 0;
	guint i;

	*volume_id = 0;
	*rel_path = NULL;
	if (prefix != NULL)
		*prefix = NULL;
	if (client_path == NULL || client_path[0] == '\0') {
		if (problem != NULL)
			*problem = "the client gave no path";
		return AXIS_DOWNLOAD_PATH_INVALID;
	}
	/* "..", ".", a relative path, a control character: refused, never
	 * resolved.  White space at either end: refused, never trimmed (the
	 * trimmed path is another place). */
	if (!axis_storage_path_normalize_exact(client_path, TRUE, &path, &why)) {
		if (problem != NULL)
			*problem = "the client's path cannot be used (it is relative, has a '.' or '..' segment or a character "
				"a path here cannot have, or begins or ends with white space, which Axis never trims away)";
		return AXIS_DOWNLOAD_PATH_INVALID;
	}
	for (i = 0; maps != NULL && i < maps->len; i++) {
		const AxisDownloadPathMap *map = g_ptr_array_index((GPtrArray *)maps, i);
		gsize length;

		if (map->client_prefix == NULL || !axis_storage_path_contains(map->client_prefix, path))
			continue;
		length = strlen(map->client_prefix);
		if (best == NULL || length > best_length) {
			best = map;
			best_length = length;
		}
	}
	if (best == NULL) {
		if (problem != NULL)
			*problem = "no path mapping of the client contains the path it gave";
		return AXIS_DOWNLOAD_PATH_UNMAPPED;
	}
	rest = path + best_length;
	while (*rest == '/')
		rest++;
	{
		g_autofree gchar *joined = NULL;
		g_autofree gchar *normal = NULL;

		if (best->rel_path == NULL || best->rel_path[0] == '\0')
			joined = g_strdup(rest);
		else if (rest[0] == '\0')
			joined = g_strdup(best->rel_path);
		else
			joined = g_strconcat(best->rel_path, "/", rest, NULL);
		if (!axis_storage_path_normalize(joined, FALSE, &normal, &why) || normal[0] == '\0') {
			if (problem != NULL)
				*problem = normal != NULL && normal[0] == '\0' ?
					"the path maps to the top of a volume, which is not a download's place" :
					"the mapped path cannot be used";
			return AXIS_DOWNLOAD_PATH_INVALID;
		}
		*volume_id = best->volume_id;
		*rel_path = g_steal_pointer(&normal);
		if (prefix != NULL)
			*prefix = g_strdup(best->client_prefix);
	}
	return AXIS_DOWNLOAD_PATH_MAPPED;
}
