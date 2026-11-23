/*
 * axis-download-path.h - from a download client's paths to Axis's volumes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A download client names its files by its own paths ("/downloads/complete/
 * Film.2020"), which are its container's, not Axis's.  An administrator
 * maps prefixes of them to a volume and a path on it: a client path beneath
 * a prefix is the same path beneath that place on the volume.  These
 * functions only do the text: the result is a volume and a path relative to
 * it, which the layer then checks through the volume guard
 * (storage/axis-volume-guard.h) before it records anything.  Nothing here
 * guesses: a path that no prefix contains is unmapped, and a path with a
 * "." or ".." segment, a relative path, or one with a character a stored
 * path cannot have, is refused (storage/axis-storage-path.h decides).
 *
 * GLib only, no I/O: fuzzed through tests/fuzz/fuzz-download-release.c.
 */

#ifndef AXIS_DOWNLOAD_PATH_H
#define AXIS_DOWNLOAD_PATH_H

#include <glib.h>

G_BEGIN_DECLS

/* Path mappings of one client, at most. */
#define AXIS_DOWNLOAD_MAX_PATH_MAPS 32

/**
 * AxisDownloadPathMap:
 * @client_prefix: a normalised absolute path of the client's
 * @volume_id: the volume it is on for Axis
 * @rel_path: a normalised path relative to the volume ("" for its top)
 */
typedef struct
{
	gchar  *client_prefix;
	gint64  volume_id;
	gchar  *rel_path;
} AxisDownloadPathMap;

AxisDownloadPathMap *
axis_download_path_map_new(
	const gchar *client_prefix,
	gint64       volume_id,
	const gchar *rel_path
);

void
axis_download_path_map_free(AxisDownloadPathMap *map);

/**
 * axis_download_path_map_normalize:
 * @client_prefix: the client's prefix as typed
 * @rel_path: (nullable): the path on the volume as typed (%NULL or "" for
 *   its top)
 * @prefix_out: (out) (transfer full) (optional): the prefix in its one form
 * @rel_out: (out) (transfer full) (optional): the path in its one form
 * @problem: (out) (transfer none) (optional): why it is refused
 *
 * Returns: %TRUE if both are acceptable
 */
gboolean
axis_download_path_map_normalize(
	const gchar  *client_prefix,
	const gchar  *rel_path,
	gchar       **prefix_out,
	gchar       **rel_out,
	const gchar **problem
);

/**
 * AxisDownloadPathResult:
 * @AXIS_DOWNLOAD_PATH_MAPPED: a mapping contains it
 * @AXIS_DOWNLOAD_PATH_UNMAPPED: no mapping contains it
 * @AXIS_DOWNLOAD_PATH_INVALID: it is not a path that can be mapped at all
 */
typedef enum
{
	AXIS_DOWNLOAD_PATH_MAPPED,
	AXIS_DOWNLOAD_PATH_UNMAPPED,
	AXIS_DOWNLOAD_PATH_INVALID
} AxisDownloadPathResult;

/**
 * axis_download_path_apply:
 * @maps: (element-type AxisDownloadPathMap): a client's mappings
 * @client_path: (nullable): where the client says the files are
 * @volume_id: (out): the volume, when mapped
 * @rel_path: (out) (transfer full): the path relative to it, when mapped;
 *   never "" (the top of a volume is not where a download's files are)
 * @problem: (out) (transfer none) (optional): why not, a sentence fragment
 *   without the path in it
 *
 * The longest prefix that contains @client_path (by whole segments) is
 * used.
 */
AxisDownloadPathResult
axis_download_path_apply(
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	const gchar     **problem
);

/**
 * axis_download_path_apply_full:
 * @prefix: (out) (transfer full) (optional): when mapped, the client prefix
 *   of the mapping used (normalised)
 *
 * As axis_download_path_apply(), and says which mapping was used (so that a
 * caller can tell whether that one mapping also holds a folder around the
 * path).
 */
AxisDownloadPathResult
axis_download_path_apply_full(
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	gchar           **prefix,
	const gchar     **problem
);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_PATH_H */
