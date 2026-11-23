/*
 * axis-probe-file.h - opening an indexed file through a volume guard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The probe and the estimator's sample measurement read media files the
 * same way: from the root's directory descriptor of a guard, one name at a
 * time, never following a link or leaving the root's filesystem, read-only.
 */

#ifndef AXIS_PROBE_FILE_H
#define AXIS_PROBE_FILE_H

#include <sys/stat.h>

#include <glib.h>

#include "storage/axis-volume-guard.h"

G_BEGIN_DECLS

typedef struct
{
	gint         fd;	/* the file, read-only */
	gint         dir_fd;	/* its directory */
	gchar       *name;	/* its name in the directory, raw bytes */
	struct stat  st;	/* as of the open */
} AxisProbeFile;

/**
 * axis_probe_file_open:
 * @guard: a read guard of the file's volume
 * @root_rel_path: the root's directory on the volume
 * @rel_path: the file's path in the root, in the stored form
 * @file: (out): the open file
 * @error: (nullable): return location for an error
 *
 * Opens the root through the guard, then each directory and the file by
 * name with O_NOFOLLOW, checking that none is a link, none is on another
 * filesystem than the root, and the file is a regular file.  Fails with
 * %G_IO_ERROR_NOT_FOUND when the path is not there as a regular file (the
 * volume is there: the guard was checked), with the guard's error when the
 * volume went away, and otherwise as the system says.
 */
gboolean
axis_probe_file_open(
	AxisVolumeGuard  *guard,
	const gchar      *root_rel_path,
	const gchar      *rel_path,
	AxisProbeFile    *file,
	GError          **error
);

void
axis_probe_file_close(AxisProbeFile *file);

G_END_DECLS

#endif /* AXIS_PROBE_FILE_H */
