/*
 * axis-storage-error.h - error domain of the storage layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The one rule callers must keep: %AXIS_STORAGE_ERROR_UNAVAILABLE means
 * "the volume is not there right now; try again later".  It never means
 * that a file, directory, root or volume has gone, and code that receives
 * it must not delete, forget or mark missing anything because of it.  A
 * file that really is absent on a volume that is online is reported with
 * %G_IO_ERROR_NOT_FOUND, and only after the volume was checked again.
 */

#ifndef AXIS_STORAGE_ERROR_H
#define AXIS_STORAGE_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_STORAGE_ERROR (axis_storage_error_quark())

/**
 * AxisStorageError:
 * @AXIS_STORAGE_ERROR_UNAVAILABLE: the volume is offline, mismatched or did
 *   not answer in time; retry later (see above)
 * @AXIS_STORAGE_ERROR_READ_ONLY: a write was refused: storage is in
 *   read-only mode, or the filesystem is mounted read-only
 * @AXIS_STORAGE_ERROR_INVALID: a path, name or other input is malformed
 *   (the message says how, and never repeats the input)
 * @AXIS_STORAGE_ERROR_REFUSED: the request is well formed but not allowed:
 *   a duplicate, nested or overlapping volume or root, a directory that
 *   cannot become a volume (the message explains)
 * @AXIS_STORAGE_ERROR_NOT_CONTAINED: a path does not stay inside its
 *   volume: it passes through a symbolic link or onto another filesystem
 * @AXIS_STORAGE_ERROR_CONFIRM: what the administrator confirmed is no
 *   longer what is there; nothing was done, and the request must be
 *   confirmed again
 * @AXIS_STORAGE_ERROR_NOT_FOUND: no volume or root with that identifier is
 *   registered (about the records, never about files)
 * @AXIS_STORAGE_ERROR_FAILED: something else went wrong, such as an I/O
 *   error while writing the marker
 */
typedef enum
{
	AXIS_STORAGE_ERROR_UNAVAILABLE,
	AXIS_STORAGE_ERROR_READ_ONLY,
	AXIS_STORAGE_ERROR_INVALID,
	AXIS_STORAGE_ERROR_REFUSED,
	AXIS_STORAGE_ERROR_NOT_CONTAINED,
	AXIS_STORAGE_ERROR_CONFIRM,
	AXIS_STORAGE_ERROR_NOT_FOUND,
	AXIS_STORAGE_ERROR_FAILED
} AxisStorageError;

GQuark
axis_storage_error_quark(void);

G_END_DECLS

#endif /* AXIS_STORAGE_ERROR_H */
