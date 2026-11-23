/*
 * axis-storage-open.h - opening one file beneath a directory descriptor by
 * the path the index stores, never following a link and never leaving the
 * directory's filesystem
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A worker is told where a file is as a volume, a root directory relative
 * to the volume (axis-storage-path.h's relative form) and the file's path
 * relative to the root in the index's stored form (axis-library-name.h).
 * These functions check both texts by the rules the daemon stores them by
 * and open the file one component at a time from a descriptor of the
 * volume's proven top directory: a symbolic link anywhere, a component on
 * another filesystem or mount, or anything but a regular file at the end
 * is refused, so nothing outside the volume is ever opened.  The file is
 * opened read-only, without blocking on a FIFO and without writing its
 * access time where that is allowed.
 *
 * Descriptor-based and free of the database: linked by the daemon and by
 * axis-worker.  The checks of the texts are GLib only (fuzzed through
 * tests/fuzz/fuzz-worker-response.c); the opening blocks on the
 * filesystem.
 */

#ifndef AXIS_STORAGE_OPEN_H
#define AXIS_STORAGE_OPEN_H

#include <glib.h>
#include <sys/stat.h>

G_BEGIN_DECLS

/**
 * AxisStorageOpenStatus:
 * @AXIS_STORAGE_OPEN_OK: opened
 * @AXIS_STORAGE_OPEN_INVALID: the root or the path is not in its one
 *   accepted form (absolute, a "." or ".." part, empty parts, too long or
 *   too deep, not a canonical stored form); nothing was opened
 * @AXIS_STORAGE_OPEN_NOT_CONTAINED: a symbolic link, a component on another
 *   filesystem or mount, a component that is not a directory, or at the end
 *   something that is not a regular file
 * @AXIS_STORAGE_OPEN_MISSING: a component is not there
 * @AXIS_STORAGE_OPEN_FAILED: another error (in *saved_errno)
 */
typedef enum
{
	AXIS_STORAGE_OPEN_OK,
	AXIS_STORAGE_OPEN_INVALID,
	AXIS_STORAGE_OPEN_NOT_CONTAINED,
	AXIS_STORAGE_OPEN_MISSING,
	AXIS_STORAGE_OPEN_FAILED
} AxisStorageOpenStatus;

/**
 * axis_storage_file_path_valid:
 * @root: (nullable): a root's directory relative to its volume, as stored
 *   (normalised; "" for the volume's top)
 * @stored: (nullable): a file's path relative to the root, in the stored
 *   form
 * @problem: (out) (optional) (transfer none): why not, a sentence that does
 *   not repeat the input
 *
 * Whether both are in the form the daemon stores: @root is a normalised
 * relative path (axis_storage_path_normalize() gives it back unchanged),
 * @stored is a canonical stored form whose bytes are at most
 * AXIS_LIBRARY_PATH_MAX long and AXIS_LIBRARY_DEPTH_MAX deep, with no
 * empty, "." or ".." part and no part longer than NAME_MAX.
 */
gboolean
axis_storage_file_path_valid(
	const gchar  *root,
	const gchar  *stored,
	const gchar **problem
);

/**
 * axis_storage_open_file_beneath:
 * @dirfd: the volume's top directory
 * @root: the root's directory, as for axis_storage_file_path_valid()
 * @stored: the file's path, as for axis_storage_file_path_valid()
 * @fd: (out): the open file (close-on-exec, blocking), or -1
 * @st: (out) (optional): what fstat() said of it
 * @saved_errno: (out) (optional): the errno of a failure
 *
 * Returns: what happened
 */
AxisStorageOpenStatus
axis_storage_open_file_beneath(
	gint          dirfd,
	const gchar  *root,
	const gchar  *stored,
	gint         *fd,
	struct stat  *st,
	gint         *saved_errno
);

/* A sentence for a status ("a symbolic link or another filesystem is on the
 * way", ...). */
const gchar *
axis_storage_open_status_describe(AxisStorageOpenStatus status);

G_END_DECLS

#endif /* AXIS_STORAGE_OPEN_H */
