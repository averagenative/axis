/*
 * axis-storage-fs.c - the few filesystem operations the volume code needs,
 * all through directory descriptors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-storage-fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static void
set_errno(
	gint *saved_errno,
	gint  value
){
	if (saved_errno != NULL)
		*saved_errno = value;
}

/*
 * After openat() of @name in @dirfd failed with @err: a symbolic link is
 * reported as ELOOP whichever flags made the open fail (O_PATH with
 * O_DIRECTORY says ENOTDIR for one).
 */
static gint
classify_open_error(
	gint         dirfd,
	const gchar *name,
	gint         err
){
	struct stat st;

	if ((err == ENOTDIR || err == ELOOP) && fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
	    S_ISLNK(st.st_mode))
		return ELOOP;

	return err;
}

/* The next '/'-separated segment of @path from *@at, NUL-terminated in
 * @buffer (AXIS segments are at most 255 bytes); FALSE at the end. */
static gboolean
next_segment(
	const gchar *path,
	gsize       *at,
	gchar       *buffer,
	gsize        size,
	gboolean    *last
){
	gsize begin;
	gsize length;

	while (path[*at] == '/')
		(*at)++;
	if (path[*at] == '\0')
		return FALSE;

	begin = *at;
	while (path[*at] != '\0' && path[*at] != '/')
		(*at)++;
	length = *at - begin;
	if (length >= size) {
		/* Left pointing at it, so the caller can tell this from the end. */
		*at = begin;
		return FALSE;
	}

	memcpy(buffer, path + begin, length);
	buffer[length] = '\0';

	/* Whether anything but slashes follows. */
	{
		gsize rest = *at;

		while (path[rest] == '/')
			rest++;
		*last = path[rest] == '\0';
	}

	return TRUE;
}

gint
axis_storage_fs_open_path(
	const gchar *path,
	gint        *saved_errno
){
	gchar segment[256];
	gsize at = 0;
	gboolean last = FALSE;
	gint fd;

	set_errno(saved_errno, 0);
	if (path == NULL || path[0] != '/') {
		set_errno(saved_errno, EINVAL);
		return -1;
	}

	fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		set_errno(saved_errno, errno);
		return -1;
	}

	while (next_segment(path, &at, segment, sizeof(segment), &last)) {
		gint flags = O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
		gint next;

		/* Only the directory itself is opened for reading; the ones on
		 * the way need only be searchable. */
		flags |= last ? (O_RDONLY | O_NOCTTY) : O_PATH;
		next = openat(fd, segment, flags);
		if (next < 0) {
			gint err = classify_open_error(fd, segment, errno);

			close(fd);
			set_errno(saved_errno, err);
			return -1;
		}
		close(fd);
		fd = next;
		if (last)
			return fd;
	}

	/* "/" itself, or a segment too long to hold. */
	close(fd);
	set_errno(saved_errno, path[at] != '\0' ? ENAMETOOLONG : EINVAL);
	return -1;
}

gint
axis_storage_fs_open_beneath(
	gint         dirfd,
	const gchar *relative,
	gint        *saved_errno
){
	AxisStorageFsIdentity top;
	gchar segment[256];
	gsize at = 0;
	gboolean last = FALSE;
	gint fd;
	gint err = 0;

	set_errno(saved_errno, 0);
	if (dirfd < 0 || relative == NULL || relative[0] == '/') {
		set_errno(saved_errno, EINVAL);
		return -1;
	}

	if (!axis_storage_fs_identify(dirfd, &top, &err)) {
		set_errno(saved_errno, err);
		return -1;
	}

	/* A new open file description, so that reading it does not move
	 * the position of the caller's descriptor. */
	fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		set_errno(saved_errno, errno);
		return -1;
	}

	while (next_segment(relative, &at, segment, sizeof(segment), &last)) {
		AxisStorageFsIdentity here;
		gint next;

		if (strcmp(segment, ".") == 0 || strcmp(segment, "..") == 0) {
			close(fd);
			set_errno(saved_errno, EINVAL);
			return -1;
		}

		next = openat(fd, segment, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		if (next < 0) {
			err = classify_open_error(fd, segment, errno);
			close(fd);
			set_errno(saved_errno, err);
			return -1;
		}
		close(fd);
		fd = next;

		if (!axis_storage_fs_identify(fd, &here, &err)) {
			close(fd);
			set_errno(saved_errno, err);
			return -1;
		}
		if (here.dev != top.dev || (here.has_mnt_id && top.has_mnt_id && here.mnt_id != top.mnt_id)) {
			close(fd);
			set_errno(saved_errno, EXDEV);
			return -1;
		}
	}

	if (relative[at] != '\0') {
		/* A segment too long to hold. */
		close(fd);
		set_errno(saved_errno, ENAMETOOLONG);
		return -1;
	}

	return fd;
}

gboolean
axis_storage_fs_identify(
	gint                   fd,
	AxisStorageFsIdentity *identity,
	gint                  *saved_errno
){
	struct statx stx;
	struct statx parent;
	gint up;

	memset(identity, 0, sizeof(*identity));
	set_errno(saved_errno, 0);

	if (statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
	          STATX_TYPE | STATX_INO | STATX_MNT_ID, &stx) != 0) {
		struct stat st;

		if (errno != ENOSYS || fstat(fd, &st) != 0) {
			set_errno(saved_errno, errno);
			return FALSE;
		}
		identity->dev = (guint64)st.st_dev;
		identity->ino = (guint64)st.st_ino;
		/* Without statx, a mount root is a directory whose parent is
		 * on another device. */
		up = openat(fd, "..", O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (up >= 0) {
			struct stat above;

			if (fstat(up, &above) == 0)
				identity->mount_root = above.st_dev != st.st_dev || above.st_ino == st.st_ino;
			close(up);
		}
		return TRUE;
	}

	identity->dev = (guint64)makedev(stx.stx_dev_major, stx.stx_dev_minor);
	identity->ino = (guint64)stx.stx_ino;
	if (stx.stx_mask & STATX_MNT_ID) {
		identity->mnt_id = (guint64)stx.stx_mnt_id;
		identity->has_mnt_id = TRUE;
	}

	if (stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) {
		identity->mount_root = (stx.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0;
		return TRUE;
	}

	/* An older kernel: compare with the parent's mount (or device). */
	up = openat(fd, "..", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (up < 0) {
		set_errno(saved_errno, errno);
		return FALSE;
	}
	if (statx(up, "", AT_EMPTY_PATH, STATX_INO | STATX_MNT_ID, &parent) != 0) {
		set_errno(saved_errno, errno);
		close(up);
		return FALSE;
	}
	close(up);

	if (identity->has_mnt_id && (parent.stx_mask & STATX_MNT_ID))
		identity->mount_root = parent.stx_mnt_id != stx.stx_mnt_id;
	else
		identity->mount_root = makedev(parent.stx_dev_major, parent.stx_dev_minor) != identity->dev;
	/* "/" is its own parent, and the top of the root mount. */
	if (parent.stx_ino == stx.stx_ino &&
	    makedev(parent.stx_dev_major, parent.stx_dev_minor) == identity->dev)
		identity->mount_root = TRUE;

	return TRUE;
}

gboolean
axis_storage_fs_info(
	gint               fd,
	AxisStorageFsInfo *info,
	gint              *saved_errno
){
	struct statvfs vfs;
	struct statfs fs;

	memset(info, 0, sizeof(*info));
	set_errno(saved_errno, 0);

	if (fstatvfs(fd, &vfs) != 0 || fstatfs(fd, &fs) != 0) {
		set_errno(saved_errno, errno);
		return FALSE;
	}

	info->read_only = (vfs.f_flag & ST_RDONLY) != 0;
	info->total_bytes = (guint64)vfs.f_blocks * (guint64)vfs.f_frsize;
	info->free_bytes = (guint64)vfs.f_bfree * (guint64)vfs.f_frsize;
	info->available_bytes = (guint64)vfs.f_bavail * (guint64)vfs.f_frsize;
	info->magic = (guint64)(gulong)fs.f_type;
	g_snprintf(info->fsid, sizeof(info->fsid), "%08x%08x", (guint)fs.f_fsid.__val[0],
		(guint)fs.f_fsid.__val[1]);

	return TRUE;
}

/* struct fsuuid2 and FS_IOC_GETFSUUID of <linux/fs.h> (Linux 6.10), spelt
 * out so that older kernel headers build too: the same 17 bytes give the
 * same request number. */
typedef struct
{
	guint8 len;
	guint8 uuid[16];
} FsUuid2;

#define AXIS_FS_IOC_GETFSUUID _IOR(0x15, 0, FsUuid2)

G_STATIC_ASSERT(sizeof(FsUuid2) == 17);
G_STATIC_ASSERT(AXIS_STORAGE_FS_UUID_CHARS == 2 * 16);

gboolean
axis_storage_fs_read_uuid(
	gint   fd,
	gchar *out
){
	FsUuid2 got;
	guint i;
	gboolean zero = TRUE;

	out[0] = '\0';
	memset(&got, 0, sizeof(got));
	if (ioctl(fd, AXIS_FS_IOC_GETFSUUID, &got) != 0 || got.len == 0 || got.len > sizeof(got.uuid))
		return FALSE;

	for (i = 0; i < got.len; i++) {
		g_snprintf(out + 2 * i, 3, "%02x", got.uuid[i]);
		if (got.uuid[i] != 0)
			zero = FALSE;
	}
	if (zero) {
		out[0] = '\0';
		return FALSE;
	}
	return TRUE;
}

gboolean
axis_storage_fs_type_is_local(guint64 magic)
{
	static const guint64 local[] = {
		0xEF53,		/* ext2, ext3, ext4 */
		0x58465342,	/* xfs */
		0x9123683E,	/* btrfs */
		0x2FC12FC1,	/* zfs */
		0xF2F52010,	/* f2fs */
		0xCA451A4E,	/* bcachefs */
		0x3153464A,	/* jfs */
		0x52654973,	/* reiserfs */
		0x7366746E,	/* ntfs3 */
		0x5346544E,	/* ntfs (the old in-kernel driver) */
		0x2011BAB0,	/* exfat */
		0x4D44,		/* vfat */
		0x15013346,	/* udf */
		0x9660,		/* iso9660 */
		0x73717368,	/* squashfs */
		0x01021994,	/* tmpfs */
		0
	};
	guint i;

	for (i = 0; local[i] != 0; i++) {
		if (local[i] == magic)
			return TRUE;
	}
	return FALSE;
}

gboolean
axis_storage_fs_fsid_is_device(
	const gchar *fsid,
	guint64      dev
){
	gchar encoded[17];
	guint32 major_number = major((dev_t)dev);
	guint32 minor_number = minor((dev_t)dev);
	/* The kernel's new_encode_dev(), which huge_encode_dev() is for the
	 * device numbers that fit, as the filesystems above put it in f_fsid. */
	guint32 number = (minor_number & 0xff) | (major_number << 8) | ((minor_number & ~0xffU) << 12);

	if (fsid == NULL || fsid[0] == '\0' || strspn(fsid, "0") == strlen(fsid))
		return TRUE;

	g_snprintf(encoded, sizeof(encoded), "%08x%08x", number, 0U);
	return strcmp(fsid, encoded) == 0;
}

gint
axis_storage_fs_openat_noatime(
	gint         dirfd,
	const gchar *name,
	gint         flags
){
	gint fd = openat(dirfd, name, flags | O_NOATIME);

	/* Not the file's owner: as it is. */
	if (fd < 0 && errno == EPERM)
		fd = openat(dirfd, name, flags);
	return fd;
}

gboolean
axis_storage_fs_is_empty(
	gint      dirfd,
	gboolean *empty,
	gint     *saved_errno
){
	struct dirent *entry;
	DIR *dir;
	gint fd;

	*empty = FALSE;
	set_errno(saved_errno, 0);

	fd = axis_storage_fs_openat_noatime(dirfd, ".", O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		set_errno(saved_errno, errno);
		return FALSE;
	}
	dir = fdopendir(fd);
	if (dir == NULL) {
		set_errno(saved_errno, errno);
		close(fd);
		return FALSE;
	}

	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			if (errno != 0) {
				set_errno(saved_errno, errno);
				closedir(dir);
				return FALSE;
			}
			*empty = TRUE;
			break;
		}
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			break;
	}

	closedir(dir);
	return TRUE;
}

const gchar *
axis_storage_fs_type_name(guint64 magic)
{
	static const struct
	{
		guint64      magic;
		const gchar *name;
	} names[] = {
		{ 0xEF53,     "ext4" },	/* ext2, ext3 and ext4 share it */
		{ 0x58465342, "xfs" },
		{ 0x9123683E, "btrfs" },
		{ 0x2FC12FC1, "zfs" },
		{ 0xF2F52010, "f2fs" },
		{ 0x01021994, "tmpfs" },
		{ 0x794C7630, "overlay" },
		{ 0x6969,     "nfs" },
		{ 0xFF534D42, "cifs" },
		{ 0xFE534D42, "smb2" },
		{ 0x65735546, "fuse" },
		{ 0x4D44,     "vfat" },
		{ 0x2011BAB0, "exfat" },
		{ 0x5346544E, "ntfs" },
		{ 0x7366746E, "ntfs3" },
		{ 0x3153464A, "jfs" },
		{ 0x52654973, "reiserfs" },
		{ 0x9660,     "iso9660" },
		{ 0x15013346, "udf" },
		{ 0x73717368, "squashfs" },
		{ 0xCA451A4E, "bcachefs" },
		{ 0, NULL }
	};
	guint i;

	for (i = 0; names[i].name != NULL; i++) {
		if (names[i].magic == magic)
			return names[i].name;
	}

	return "unknown";
}

gchar *
axis_storage_fs_device_text(guint64 dev)
{
	return g_strdup_printf("%u:%u", major((dev_t)dev), minor((dev_t)dev));
}
