/*
 * axis-volume-guard.h - the only way to touch files on a volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A guard is proof, at the moment it was taken, that a volume is there:
 * its directory was opened from its path without following a symbolic
 * link, the marker in that directory carries the volume's UUID, and (for
 * a write guard) storage is not read-only and the filesystem is mounted
 * read-write.  The guard holds that directory's descriptor, and every file
 * operation is made relative to it (or to a descriptor opened through
 * axis_volume_guard_open_dir()), never through a path, so what was checked
 * is what is used.  Nothing in the storage API hands out a volume's path
 * for opening files.
 *
 * A volume added from a read-only disk without a marker is proved by its
 * filesystem identity instead (AxisVolumeFsIdentity), for reading only.  A
 * write guard always needs the marker: on such a volume, mounted read-write,
 * acquiring one writes the marker first, through the directory the guard
 * then holds, and fails with %AXIS_STORAGE_ERROR_UNAVAILABLE when it cannot;
 * its checks then demand the marker.
 *
 * How code uses it:
 *
 *   g_autoptr(AxisVolumeGuard) guard =
 *           axis_volume_guard_acquire(storage, volume_id, AXIS_VOLUME_INTENT_READ, &error);
 *   if (guard == NULL)
 *           return FALSE;     // UNAVAILABLE: try again later; never "files are gone"
 *   fd = axis_volume_guard_open_dir(guard, root->rel_path, &error);
 *   while (walking) {
 *           ... openat(fd, name, O_NOFOLLOW ...) ...
 *           if (time_for_a_check && !axis_volume_guard_check(guard, &error))
 *                   stop, report the scan incomplete, change nothing
 *   }
 *
 * A guard does not stop the disk from going away.  It goes stale
 * silently; axis_volume_guard_check() finds out.  Long operations call it
 * at safe points (each check costs a helper thread, a walk of the path
 * and a small read, so not for every file), and before anything they
 * conclude from an absence: a file that cannot be found is only "not
 * there" if the check after it still passes.
 *
 * %AXIS_STORAGE_ERROR_UNAVAILABLE from any function here means "retry
 * later".  A guard is used by one thread at a time.  The storage must
 * outlive its guards.
 */

#ifndef AXIS_VOLUME_GUARD_H
#define AXIS_VOLUME_GUARD_H

#include <glib.h>

#include "storage/axis-storage.h"

G_BEGIN_DECLS

/**
 * AxisVolumeIntent:
 * @AXIS_VOLUME_INTENT_READ: reading only
 * @AXIS_VOLUME_INTENT_WRITE: the holder will create, change, rename or
 *   delete files; refused in read-only mode and on a read-only mount
 */
typedef enum
{
	AXIS_VOLUME_INTENT_READ,
	AXIS_VOLUME_INTENT_WRITE
} AxisVolumeIntent;

typedef struct _AxisVolumeGuard AxisVolumeGuard;

/**
 * axis_volume_guard_acquire:
 * @storage: the storage
 * @volume_id: the volume
 * @intent: what the holder will do
 * @error: (nullable): return location for an error
 *
 * Checks the volume now, on a helper with a deadline.  Fails with
 * %AXIS_STORAGE_ERROR_UNAVAILABLE when it is offline, mismatched or does
 * not answer in time (distinct from any not-found error about a file),
 * with %AXIS_STORAGE_ERROR_READ_ONLY for a write guard in read-only mode
 * or on a read-only mount, and with %AXIS_STORAGE_ERROR_NOT_FOUND when no
 * such volume is registered.  Does not change the volume's stored state:
 * the scheduled check does that.  A write guard on a volume without a
 * marker writes the marker first (and records that the volume has it), or
 * fails with %AXIS_STORAGE_ERROR_UNAVAILABLE.
 *
 * Returns: (transfer full) (nullable): the guard; release it with
 *   axis_volume_guard_release()
 */
AxisVolumeGuard *
axis_volume_guard_acquire(
	AxisStorage       *storage,
	gint64             volume_id,
	AxisVolumeIntent   intent,
	GError           **error
);

/**
 * axis_volume_guard_check:
 * @self: a guard
 * @error: (nullable): return location for an error
 *
 * Verifies that the guard still holds: the volume is still registered,
 * its path still leads to the very directory the guard holds, the marker
 * read through the guard's own descriptor still matches, and, for a write
 * guard, writing is still allowed.  Fails as axis_volume_guard_acquire().
 * A guard that failed a check stays failed: release it and acquire a new
 * one later.
 *
 * Returns: %TRUE if the guard still holds
 */
gboolean
axis_volume_guard_check(
	AxisVolumeGuard  *self,
	GError          **error
);

/**
 * axis_volume_guard_writable:
 * @self: a guard
 * @error: (nullable): return location for an error
 *
 * Without touching the filesystem: whether writing through @self is allowed
 * now.  Fails with %AXIS_STORAGE_ERROR_READ_ONLY for a guard taken for
 * reading or while storage is in read-only mode, and with
 * %AXIS_STORAGE_ERROR_UNAVAILABLE for a guard whose check failed.  Every
 * write operation (axis-volume-write.h) asks this before each change it
 * makes.
 *
 * Returns: %TRUE if writing is allowed
 */
gboolean
axis_volume_guard_writable(
	AxisVolumeGuard  *self,
	GError          **error
);

/**
 * axis_volume_guard_get_fd:
 *
 * Returns: the volume's top directory, open for reading; owned by the
 *   guard and valid until it is released.  Do not close it, and do not
 *   read it as a directory stream (open the top with
 *   axis_volume_guard_open_dir() for that).
 */
gint
axis_volume_guard_get_fd(AxisVolumeGuard *self);

/**
 * axis_volume_guard_open_dir:
 * @self: a guard
 * @relative: a directory relative to the volume's top ("" for the top),
 *   as typed or stored; normalised here
 * @error: (nullable): return location for an error
 *
 * Opens a directory beneath the volume's top one component at a time,
 * never following a symbolic link and never onto another filesystem or
 * mount.  Fails with %AXIS_STORAGE_ERROR_INVALID for a malformed path,
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED for a symbolic link or another
 * filesystem on the way, and otherwise, after checking the guard again,
 * either with the check's error (the volume went away) or with a #GIOError
 * such as %G_IO_ERROR_NOT_FOUND (the volume is there; the directory is
 * not).
 *
 * Returns: a descriptor the caller closes, or -1
 */
gint
axis_volume_guard_open_dir(
	AxisVolumeGuard  *self,
	const gchar      *relative,
	GError          **error
);

gint64
axis_volume_guard_get_volume_id(AxisVolumeGuard *self);

/* Returns: (transfer none): the volume's UUID */
const gchar *
axis_volume_guard_get_uuid(AxisVolumeGuard *self);

AxisVolumeIntent
axis_volume_guard_get_intent(AxisVolumeGuard *self);

/* The device the volume's top directory is on, for code that must stay
 * on its filesystem. */
guint64
axis_volume_guard_get_dev(AxisVolumeGuard *self);

/* Closes the descriptor and frees the guard. */
void
axis_volume_guard_release(AxisVolumeGuard *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVolumeGuard, axis_volume_guard_release)

G_END_DECLS

#endif /* AXIS_VOLUME_GUARD_H */
