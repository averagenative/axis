/*
 * axis-storage-fs.h - the few filesystem operations the volume code needs,
 * all through directory descriptors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Paths are opened one component at a time with O_NOFOLLOW, so that no
 * symbolic link anywhere in a path is ever followed, and every later
 * operation is relative to the descriptor that was opened and checked:
 * there is no window between checking a directory and using it in which
 * something else could be mounted or renamed into its place.
 *
 * Every function here may block on a filesystem that has stopped
 * answering.  Call them from a storage helper thread (axis-storage-probe.h),
 * never from the main loop.  Failures return %FALSE or -1 with the errno
 * in *@saved_errno.
 */

#ifndef AXIS_STORAGE_FS_H
#define AXIS_STORAGE_FS_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * axis_storage_fs_open_path:
 * @path: an absolute, normalised path (axis-storage-path.h)
 * @saved_errno: (out): why it failed: ENOENT for a missing component,
 *   ELOOP for a symbolic link, ENOTDIR for something that is not a
 *   directory, or what the filesystem said
 *
 * Opens the directory at @path for reading, walking from "/" without
 * following any symbolic link.
 *
 * Returns: a descriptor (close-on-exec) the caller closes, or -1
 */
gint
axis_storage_fs_open_path(
	const gchar *path,
	gint        *saved_errno
);

/**
 * axis_storage_fs_open_beneath:
 * @dirfd: a directory, such as a volume's top
 * @relative: a relative, normalised path ("" for @dirfd itself)
 * @saved_errno: (out): as axis_storage_fs_open_path(), and EXDEV when a
 *   component is on another filesystem or mount than @dirfd
 *
 * Opens the directory @relative beneath @dirfd one component at a time,
 * never following a symbolic link and never crossing onto another
 * filesystem or mount, so the result is always inside @dirfd's tree.
 *
 * Returns: a new descriptor the caller closes, or -1
 */
gint
axis_storage_fs_open_beneath(
	gint         dirfd,
	const gchar *relative,
	gint        *saved_errno
);

/**
 * AxisStorageFsIdentity:
 * @dev: st_dev
 * @ino: st_ino
 * @mnt_id: the mount's id (statx), when @has_mnt_id
 * @mount_root: whether the directory is the top of a mount
 */
typedef struct
{
	guint64  dev;
	guint64  ino;
	guint64  mnt_id;
	gboolean has_mnt_id;
	gboolean mount_root;
} AxisStorageFsIdentity;

/* What @fd is.  Returns: %TRUE on success */
gboolean
axis_storage_fs_identify(
	gint                   fd,
	AxisStorageFsIdentity *identity,
	gint                  *saved_errno
);

/**
 * AxisStorageFsInfo:
 * @read_only: the filesystem is mounted read-only (here, or everywhere)
 * @total_bytes: its size
 * @free_bytes: free space
 * @available_bytes: free space this process may use
 * @magic: the filesystem type (statfs f_type)
 * @fsid: the filesystem id (statfs f_fsid) in hexadecimal
 */
typedef struct
{
	gboolean read_only;
	guint64  total_bytes;
	guint64  free_bytes;
	guint64  available_bytes;
	guint64  magic;
	gchar    fsid[17];
} AxisStorageFsInfo;

/* About the filesystem @fd is on.  Returns: %TRUE on success */
gboolean
axis_storage_fs_info(
	gint               fd,
	AxisStorageFsInfo *info,
	gint              *saved_errno
);

/* Hexadecimal digits of the longest filesystem UUID the kernel gives. */
#define AXIS_STORAGE_FS_UUID_CHARS 32

/**
 * axis_storage_fs_read_uuid:
 * @fd: anything on the filesystem
 * @out: (out caller-allocates): %AXIS_STORAGE_FS_UUID_CHARS + 1 bytes: the
 *   filesystem's own UUID in lower-case hexadecimal, or "" when there is none
 *
 * The UUID the kernel holds for the filesystem (FS_IOC_GETFSUUID): the one
 * blkid shows, read through @fd without privileges, and so also inside a
 * container, where /dev/disk/by-uuid is not there.  Linux 6.10 and later
 * give it for the filesystems that publish one (ext4 and tmpfs do; btrfs
 * does not); an older kernel answers ENOTTY.  Call it only on a local
 * filesystem (axis_storage_fs_type_is_local()): on a FUSE filesystem the
 * request would go to its program.
 *
 * Returns: %TRUE when the kernel gave one
 */
gboolean
axis_storage_fs_read_uuid(
	gint   fd,
	gchar *out
);

/*
 * Whether @magic (statfs f_type) is a filesystem on this machine's own
 * disks, or tmpfs: not a network filesystem, a FUSE one (ntfs-3g,
 * mergerfs, sshfs, the file sharing of Docker Desktop) or a layered one,
 * whose identifier says nothing about which disk is behind it.
 */
gboolean
axis_storage_fs_type_is_local(guint64 magic);

/*
 * Whether @fsid (AxisStorageFsInfo.fsid) tells nothing of the filesystem
 * itself: zero, or the number of the device @dev it is on, which is what
 * xfs, exFAT, FAT, f2fs, UDF and ISO 9660 report.  A device number changes
 * when disks are found in another order, and another disk later gets it.
 * ext4 (from its UUID), btrfs (its UUID and subvolume), NTFS (its serial
 * number), ZFS, JFS and tmpfs (a UUID made at each mount) report one of
 * their own.
 */
gboolean
axis_storage_fs_fsid_is_device(
	const gchar *fsid,
	guint64      dev
);

/**
 * axis_storage_fs_openat_noatime:
 * @dirfd: a directory
 * @name: what to open in it
 * @flags: openat()'s flags
 *
 * openat() with O_NOATIME as well, where the kernel allows it (the file's
 * owner, or CAP_FOWNER), and without it on EPERM: what a check reads every
 * minute (the marker, the volume's top) is not an access time written to
 * the disk each time.
 *
 * Returns: the descriptor, or -1 with errno set
 */
gint
axis_storage_fs_openat_noatime(
	gint         dirfd,
	const gchar *name,
	gint         flags
);

/**
 * axis_storage_fs_is_empty:
 * @dirfd: a directory (not consumed)
 * @empty: (out): whether it has no entry but "." and ".."
 *
 * Reads only until the first entry, without updating the directory's access
 * time where that is allowed.  Returns: %TRUE on success
 */
gboolean
axis_storage_fs_is_empty(
	gint      dirfd,
	gboolean *empty,
	gint     *saved_errno
);

/* Returns: (transfer none): "ext4" and the like, or "unknown" */
const gchar *
axis_storage_fs_type_name(guint64 magic);

/* Returns: (transfer full): "major:minor" */
gchar *
axis_storage_fs_device_text(guint64 dev);

G_END_DECLS

#endif /* AXIS_STORAGE_FS_H */
