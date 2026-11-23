/*
 * axis-storage-path.h - checking the paths and names an administrator types
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Volume paths, root paths and labels arrive from forms.  Before anything
 * else sees them they go through these functions, which accept a small,
 * safe subset and normalise it: valid UTF-8 (the database stores text),
 * no control characters (so a path can be logged and shown without forging
 * a line), no "." or ".." segment anywhere (they are refused, not
 * resolved: a path means what it says or nothing), repeated and trailing
 * slashes collapsed, and bounded length and depth.  Symbolic links are a
 * property of the filesystem, not of the text, and are refused later, when
 * the path is opened.
 *
 * GLib only; there is a fuzz target (tests/fuzz/fuzz-storage-path.c).
 */

#ifndef AXIS_STORAGE_PATH_H
#define AXIS_STORAGE_PATH_H

#include <glib.h>

G_BEGIN_DECLS

/* The longest input looked at, and the longest normalised path. */
#define AXIS_STORAGE_INPUT_MAX       4096
#define AXIS_STORAGE_PATH_MAX        1024
#define AXIS_STORAGE_SEGMENT_MAX     255
#define AXIS_STORAGE_DEPTH_MAX       64

/* Names of volumes and roots, in characters. */
#define AXIS_STORAGE_LABEL_MAX_CHARS 64

/**
 * axis_storage_path_normalize_exact:
 *
 * As axis_storage_path_normalize(), for a path Axis did not type but was
 * told (a download client's): one that begins or ends with white space is
 * refused rather than trimmed, since the trimmed path would be another
 * place.  Several slashes in a row and a slash at the end still mean the one
 * path they mean.
 */
gboolean
axis_storage_path_normalize_exact(
	const gchar  *input,
	gboolean      absolute,
	gchar       **normalised,
	const gchar **problem
);

/**
 * axis_storage_path_normalize:
 * @input: (nullable): what was typed; leading and trailing white space is
 *   ignored
 * @absolute: %TRUE for a volume's path (must start with '/', and may not
 *   be "/" itself), %FALSE for a path relative to a volume (must not start
 *   with '/'; empty means the volume's top directory)
 * @normalised: (out) (transfer full) (optional): the path in its one form:
 *   segments joined by single slashes, no trailing slash
 * @problem: (out) (transfer none) (optional): why it was refused, a
 *   sentence that does not repeat the input
 *
 * Returns: %TRUE if @input is acceptable
 */
gboolean
axis_storage_path_normalize(
	const gchar  *input,
	gboolean      absolute,
	gchar       **normalised,
	const gchar **problem
);

/**
 * axis_storage_path_contains:
 * @outer: a normalised path
 * @inner: a normalised path of the same kind (both absolute or both
 *   relative)
 *
 * Compares by whole segments, so "/media/disk" does not contain
 * "/media/disk2".  A relative "" (a volume's top) contains every relative
 * path.
 *
 * Returns: %TRUE if @inner is @outer or lies below it
 */
gboolean
axis_storage_path_contains(
	const gchar *outer,
	const gchar *inner
);

/**
 * axis_storage_label_normalize:
 * @input: (nullable): a name as typed; leading and trailing white space is
 *   ignored
 * @label: (out) (transfer full) (optional): the name
 * @problem: (out) (transfer none) (optional): why it was refused
 *
 * A volume's or root's name: 1 to %AXIS_STORAGE_LABEL_MAX_CHARS
 * characters of valid UTF-8 without control characters.
 *
 * Returns: %TRUE if @input is acceptable
 */
gboolean
axis_storage_label_normalize(
	const gchar  *input,
	gchar       **label,
	const gchar **problem
);

/**
 * axis_storage_path_last_segment:
 * @path: a normalised path
 *
 * Returns: (transfer full): its last segment ("" for "")
 */
gchar *
axis_storage_path_last_segment(const gchar *path);

G_END_DECLS

#endif /* AXIS_STORAGE_PATH_H */
