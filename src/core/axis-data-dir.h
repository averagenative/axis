/*
 * axis-data-dir.h - the one directory the daemon keeps files of its own in
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything else the daemon knows is in PostgreSQL; this directory holds
 * only derived data that can be fetched or computed again (artwork, for
 * now), so losing it loses nothing.
 *
 * It is opened once, at start-up (data.dir, core/axis-config.h), and from
 * then on everything is done beneath that descriptor: a file is named
 * relative to it (or to a subdirectory opened from it), never by a path
 * built from text, so renaming or replacing the directory by its path
 * changes nothing for the running daemon.
 *
 * axis_data_dir_open() refuses the directory, naming it, when:
 *
 *  - it is absent and cannot be created (its parent must exist; only the
 *    last part of the path is created, with mode 0700);
 *  - it is a symbolic link or not a directory;
 *  - it belongs neither to the daemon's user nor to one of its groups with
 *    write permission for that group (axis_data_dir_owner_check()), or
 *    anyone may write in it;
 *  - a file cannot be created in it (read-only, full, no permission).
 *
 * The group rule is what lets a container run as any user id the platform
 * assigns, with no chown at start: OpenShift gives an arbitrary user id
 * with group 0 (the image's directory is group 0 and mode 0770), and
 * Kubernetes gives a volume to the pod's fsGroup.
 *
 * Every function blocks on the filesystem: call them at start-up or from a
 * worker thread.
 */

#ifndef AXIS_DATA_DIR_H
#define AXIS_DATA_DIR_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AxisDataDir AxisDataDir;

/**
 * AxisDataDirOwner:
 * @AXIS_DATA_DIR_OWN: the process's user owns it
 * @AXIS_DATA_DIR_GROUP: one of the process's groups owns it and may write
 *   and search it
 * @AXIS_DATA_DIR_EVERYONE: refused: every user may write in it
 * @AXIS_DATA_DIR_OTHER: refused: neither of the above
 */
typedef enum
{
	AXIS_DATA_DIR_OWN,
	AXIS_DATA_DIR_GROUP,
	AXIS_DATA_DIR_EVERYONE,
	AXIS_DATA_DIR_OTHER
} AxisDataDirOwner;

/**
 * axis_data_dir_owner_check:
 * @uid: the directory's owner
 * @gid: the directory's group
 * @mode: its st_mode
 * @euid: the process's effective user
 * @groups: (array length=n_groups): the process's effective and
 *   supplementary groups
 * @n_groups: their number
 *
 * The ownership rule axis_data_dir_open() applies, on its own.
 *
 * Returns: whether, and why, the directory is acceptable
 */
AxisDataDirOwner
axis_data_dir_owner_check(
	guint        uid,
	guint        gid,
	guint        mode,
	guint        euid,
	const guint *groups,
	guint        n_groups
);

/**
 * axis_data_dir_open:
 * @path: an absolute, normalised path (as data.dir is)
 * @error: (nullable): return location for an %AXIS_ERROR_DATA_DIR
 *
 * Returns: (transfer full) (nullable): the directory, reference counted
 */
AxisDataDir *
axis_data_dir_open(
	const gchar  *path,
	GError      **error
);

AxisDataDir *
axis_data_dir_ref(AxisDataDir *self);

void
axis_data_dir_unref(AxisDataDir *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDataDir, axis_data_dir_unref)

/* Returns: (transfer none): the path it was opened at (for messages only) */
const gchar *
axis_data_dir_get_path(AxisDataDir *self);

/* Returns: its descriptor, owned by @self */
gint
axis_data_dir_get_fd(AxisDataDir *self);

/**
 * axis_data_dir_subdir:
 * @self: the data directory
 * @name: 1 to 32 lower-case letters, digits and '-', starting with a letter
 * @error: (nullable): return location for an %AXIS_ERROR_DATA_DIR
 *
 * Opens the subdirectory @name beneath the descriptor, creating it (mode
 * 0700) when absent, with the same checks as the directory itself.  The
 * descriptor is opened once and kept: later calls return the same one.
 *
 * Returns: a descriptor owned by @self, or -1
 */
gint
axis_data_dir_subdir(
	AxisDataDir  *self,
	const gchar  *name,
	GError      **error
);

G_END_DECLS

#endif /* AXIS_DATA_DIR_H */
