/*
 * axis-volume-write.c - the only changes Axis makes to files on a volume
 * besides its marker, all through a write guard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every system call that changes something is in this file.  Every
 * operation starts by proving the volume again (axis_volume_guard_check():
 * the guard's directory still the one at the volume's path, the marker the
 * volume's, mounted read-write), so that no caller can write through a
 * guard whose disk was swapped or unmounted after it was taken; and each
 * call is preceded by axis_volume_guard_writable() (storage writable, the
 * guard not failed):
 *
 *   mkdirat      a directory of the library or of the recycle folder
 *   fchmod       only a directory mkdirat made a moment before
 *   unlinkat(AT_REMOVEDIR)  an empty directory this run made, or an empty
 *                directory of the recycle folder
 *   openat(O_CREAT | O_EXCL)  a temporary name, and write() and fsync() to
 *                that file only
 *   linkat       a temporary name for a download's file; a final name where
 *                the filesystem has no RENAME_NOREPLACE; a file's name in the
 *                recycle folder (an upgrade that keeps the name); a moved
 *                file's new name where there is no RENAME_NOREPLACE
 *   renameat2(RENAME_NOREPLACE)  a temporary name to its final name; a
 *                library file into the recycle folder; a download's file to a
 *                temporary name in the library (a move on one volume,
 *                axis_volume_write_move_temp()), and back to its download
 *                name when the import cannot publish it
 *                (axis_volume_write_move_back())
 *   renameat2(RENAME_EXCHANGE)  the one swap of an upgrade that keeps the
 *                name, after the old file was linked into the recycle folder
 *   unlinkat     a temporary name (see axis_volume_write_unlink_temp()); a
 *                library name whose file the recycle folder already names,
 *                where the filesystem has no RENAME_NOREPLACE; a recorded
 *                file of the recycle folder; every name inside a finished
 *                Usenet download's own folder, and then that folder
 *                (axis_volume_write_remove_folder(), after a survey of the
 *                whole tree that changes nothing); the one file of a Usenet
 *                job put straight into its category's folder, by its inode
 *                (axis_volume_write_remove_file()); a download's file whose
 *                verified copy is in the library, by its inode, size and both
 *                times as they were when it was read for the copy, after a
 *                move across volumes (axis_volume_write_remove_moved()); a
 *                moved file's old name where there is no RENAME_NOREPLACE
 *   fsync       files written here and the directories changed here (a
 *                new directory's parent too)
 *
 * And for the conversions, in the work area only (AXIS_VOLUME_WORK_DIR at
 * the volume's top, checked by inode on every call), on names of Axis's:
 *
 *   fchown, fchmod, futimens, fsync  a verified output, to give it its
 *                original's owner, mode and time (axis_volume_write_settle())
 *   renameat2(RENAME_NOREPLACE)  a verified output out of the work area into
 *                its original's folder under a temporary name
 *                (axis_volume_write_adopt(); a link and the removal of the
 *                work-area name where there is no RENAME_NOREPLACE); a
 *                trial's output to its kept name (axis_volume_write_rename_work())
 *   unlinkat     a file of the work area (axis_volume_write_unlink_work())
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-volume-write.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "storage/axis-storage-error.h"
#include "storage/axis-storage-fs.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE  (1 << 1)
#endif

#define COPY_CHUNK          (1024 * 1024)
#define COPY_STEP_US        G_USEC_PER_SEC
#define COPY_CHECK_US       (5 * G_USEC_PER_SEC)
#define RECYCLE_TRIES       1000

/* ── Checks ─────────────────────────────────────────────────────── */

static gboolean
system_error(
	GError      **error,
	gint          err,
	const gchar  *what
){
	if (err == EROFS)
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY,
			"%s: the filesystem is read-only.", what);
	else if (err == EXDEV || err == ELOOP)
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
			"%s: %s.", what, err == EXDEV ? "it would cross onto another filesystem" : "a symbolic link is in the way");
	else
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "%s: %s.", what, g_strerror(err));
	return FALSE;
}

static gboolean
not_contained(
	GError      **error,
	const gchar  *text
){
	g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED, text);
	return FALSE;
}

static gboolean
invalid(
	GError      **error,
	const gchar  *text
){
	g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID, text);
	return FALSE;
}

/* One component: 1 to 255 bytes, no '/' or NUL, not "." or "..". */
static gboolean
component_valid(const gchar *name)
{
	gsize n;

	if (name == NULL)
		return FALSE;
	n = strlen(name);
	return n > 0 && n <= 255 && strchr(name, '/') == NULL && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

gboolean
axis_volume_write_temp_name_valid(const gchar *name)
{
	gsize prefix = strlen(AXIS_VOLUME_TEMP_PREFIX);
	gsize n;
	gsize i;

	if (name == NULL || !g_str_has_prefix(name, AXIS_VOLUME_TEMP_PREFIX))
		return FALSE;
	n = strlen(name);
	if (n <= prefix || n > prefix + 20)
		return FALSE;
	for (i = prefix; i < n; i++) {
		if (!g_ascii_isdigit(name[i]))
			return FALSE;
	}
	return TRUE;
}

/* @fd is on the guard's filesystem and mount. */
static gboolean
on_guard_fs(
	AxisVolumeGuard  *guard,
	gint              fd,
	GError          **error
){
	AxisStorageFsIdentity top;
	AxisStorageFsIdentity here;
	gint err = 0;

	if (fd < 0)
		return invalid(error, "There is no directory to work in.");
	if (!axis_storage_fs_identify(axis_volume_guard_get_fd(guard), &top, &err) ||
	    !axis_storage_fs_identify(fd, &here, &err))
		return system_error(error, err, "Cannot tell where a directory is");
	if (here.dev != top.dev || (here.has_mnt_id && top.has_mnt_id && here.mnt_id != top.mnt_id))
		return not_contained(error, "A directory is not on the volume's own filesystem.");
	return TRUE;
}

/*
 * The volume proven again, now: a write guard, storage writable, and
 * axis_volume_guard_check() (the volume's path still leads to the directory
 * the guard holds, its marker is the volume's, it is mounted read-write).
 * A guard's proof is from when it was taken; a disk swapped or lazily
 * unmounted since would leave its descriptors on the old one, and nothing
 * may be written there.  The check runs on a helper thread with a
 * deadline, so a hung disk makes it fail rather than block; a failure
 * marks the guard failed, so everything after it through the guard is
 * refused too.  It costs a helper thread, a path walk and a small read:
 * once per operation, which is a handful per file placed.
 */
static gboolean
proven(
	AxisVolumeGuard  *guard,
	GError          **error
){
	return axis_volume_guard_writable(guard, error) && axis_volume_guard_check(guard, error);
}

/* The checks every operation starts with. */
static gboolean
may_write(
	AxisVolumeGuard  *guard,
	gint              dir,
	GError          **error
){
	g_return_val_if_fail(guard != NULL, FALSE);

	return proven(guard, error) && on_guard_fs(guard, dir, error);
}

/* What @name in @dir is, without following it.  %FALSE with errno set. */
static gboolean
look(
	gint         dir,
	const gchar *name,
	struct stat *st
){
	return fstatat(dir, name, st, AT_SYMLINK_NOFOLLOW) == 0;
}

/* %FALSE when the directory could not be flushed to the disk.  A directory
 * whose fsync fails still holds what was renamed into it (the rename was
 * made), so most callers carry on; one that is to delete something because a
 * name is on the disk (a move across volumes deletes the download's file once
 * its copy has its name) must not (axis_volume_write_publish_synced()).  A
 * filesystem that cannot fsync a directory at all (EINVAL) is taken as
 * flushed: it has no other way to be. */
static gboolean
sync_dir(gint dir)
{
	if (fsync(dir) != 0 && errno != EINVAL) {
		g_debug("storage: fsync of a directory failed: %s", g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

/* Tests: the directory fsyncs after a publish or a swap fail; those of a
 * folder made, and of a folder flushed for what is in it, fail. */
static gint fail_publish_sync;
static gint fail_folder_sync;

void
axis_volume_write_test_fail_publish_sync(gboolean fail)
{
	g_atomic_int_set(&fail_publish_sync, fail ? 1 : 0);
}

void
axis_volume_write_test_fail_folder_sync(gboolean fail)
{
	g_atomic_int_set(&fail_folder_sync, fail ? 1 : 0);
}

/* The directory fsync of a folder that holds a folder just made, or that is
 * flushed for what is in it. */
static gboolean
sync_folder(gint dir)
{
	if (g_atomic_int_get(&fail_folder_sync)) {
		g_debug("storage: fsync of a directory failed: %s (a test's)", g_strerror(EIO));
		return FALSE;
	}
	return sync_dir(dir);
}

/* The directory fsync after a final name was given. */
static gboolean
sync_published(gint dir)
{
	if (g_atomic_int_get(&fail_publish_sync)) {
		g_debug("storage: fsync of a directory failed: %s (a test's)", g_strerror(EIO));
		return FALSE;
	}
	return sync_dir(dir);
}

gboolean
axis_volume_write_flush_dir(
	AxisVolumeGuard *guard,
	gint             dir
){
	if (guard == NULL || !on_guard_fs(guard, dir, NULL))
		return FALSE;
	return sync_folder(dir);
}

/* ── Directories ────────────────────────────────────────────────── */

gint
axis_volume_write_mkdir(
	AxisVolumeGuard  *guard,
	gint              parent,
	const gchar      *name,
	guint             mode,
	gboolean         *created,
	guint64          *ino,
	GError          **error
){
	return axis_volume_write_mkdir_synced(guard, parent, name, mode, created, ino, NULL, error);
}

gint
axis_volume_write_mkdir_synced(
	AxisVolumeGuard  *guard,
	gint              parent,
	const gchar      *name,
	guint             mode,
	gboolean         *created,
	guint64          *ino,
	gboolean         *synced,
	GError          **error
){
	struct stat st;
	gboolean made = FALSE;
	gint fd;

	if (created != NULL)
		*created = FALSE;
	if (ino != NULL)
		*ino = 0;
	if (synced != NULL)
		*synced = TRUE;
	if (!may_write(guard, parent, error))
		return -1;
	if (!component_valid(name)) {
		invalid(error, "A folder's name is not allowed.");
		return -1;
	}
	if (mkdirat(parent, name, (mode_t)(mode & 07777)) == 0) {
		made = TRUE;
	} else if (errno != EEXIST) {
		system_error(error, errno, "Cannot make a folder");
		return -1;
	}
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		gint err = errno;

		if (err == ELOOP || err == ENOTDIR)
			not_contained(error, "Something that is not a folder is where a folder should be.");
		else
			system_error(error, err, "Cannot open a folder");
		return -1;
	}
	if (!on_guard_fs(guard, fd, error) || fstat(fd, &st) != 0) {
		if (error != NULL && *error == NULL)
			system_error(error, errno, "Cannot read a folder");
		close(fd);
		return -1;
	}
	/* Only a directory made now gets the mode; the umask aside. */
	if (made && fchmod(fd, (mode_t)(mode & 07777)) != 0) {
		gint err = errno;

		close(fd);
		system_error(error, err, "Cannot set a new folder's mode");
		if (unlinkat(parent, name, AT_REMOVEDIR) != 0)
			g_debug("storage: a folder just made could not be removed again: %s", g_strerror(errno));
		return -1;
	}
	/* The new entry is on the disk before anything is placed beneath it; when
	 * it cannot be flushed, the caller is told (a move across volumes then
	 * keeps the download's file). */
	if (made && !sync_folder(parent) && synced != NULL)
		*synced = FALSE;
	if (created != NULL)
		*created = made;
	if (ino != NULL)
		*ino = (guint64)st.st_ino;
	return fd;
}

gboolean
axis_volume_write_rmdir_created(
	AxisVolumeGuard  *guard,
	gint              parent,
	const gchar      *name,
	guint64           ino,
	GError          **error
){
	struct stat st;

	if (!may_write(guard, parent, error))
		return FALSE;
	if (!component_valid(name) || !look(parent, name, &st))
		return TRUE;
	if (!S_ISDIR(st.st_mode) || (guint64)st.st_ino != ino)
		return TRUE;
	/* The system refuses a directory that is not empty. */
	if (unlinkat(parent, name, AT_REMOVEDIR) != 0 && errno != ENOTEMPTY && errno != EEXIST)
		g_debug("storage: a folder made by this run could not be removed: %s", g_strerror(errno));
	return TRUE;
}

/* ── Temporary names ────────────────────────────────────────────── */

gboolean
axis_volume_write_unlink_temp(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           expected_ino,
	gboolean          last_link_ok,
	gboolean         *removed,
	GError          **error
){
	struct stat st;

	if (removed != NULL)
		*removed = FALSE;
	if (!may_write(guard, dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name))
		return invalid(error, "Only a temporary name of Axis's can be removed.");
	if (!look(dir, temp_name, &st)) {
		if (errno == ENOENT)
			return TRUE;
		return system_error(error, errno, "Cannot look at a temporary file");
	}
	if (!S_ISREG(st.st_mode))
		return not_contained(error, "A temporary name is not a regular file: it is left as it is.");
	if (expected_ino != 0 && (guint64)st.st_ino != expected_ino)
		return not_contained(error, "A temporary name leads to another file than Axis made: it is left as it is.");
	if (st.st_nlink < 2 && !last_link_ok)
		return not_contained(error, "A temporary name is the only name of its file: it is kept.");
	if (unlinkat(dir, temp_name, 0) != 0) {
		if (errno == ENOENT)
			return TRUE;
		return system_error(error, errno, "Cannot remove a temporary file");
	}
	if (removed != NULL)
		*removed = TRUE;
	return TRUE;
}

gboolean
axis_volume_write_unlink_temp_beside(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           expected_ino,
	gint              other_dir,
	const gchar      *other_name,
	gboolean         *removed,
	GError          **error
){
	struct stat st;
	struct stat other;

	if (removed != NULL)
		*removed = FALSE;
	if (!may_write(guard, dir, error) || !on_guard_fs(guard, other_dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name))
		return invalid(error, "Only a temporary name of Axis's can be removed.");
	if (expected_ino == 0 || !component_valid(other_name) || axis_volume_write_temp_name_valid(other_name))
		return invalid(error, "A temporary name is removed only beside another name of its file that is known.");
	if (!look(dir, temp_name, &st)) {
		if (errno == ENOENT)
			return TRUE;
		return system_error(error, errno, "Cannot look at a temporary file");
	}
	if (!S_ISREG(st.st_mode))
		return not_contained(error, "A temporary name is not a regular file: it is left as it is.");
	if ((guint64)st.st_ino != expected_ino)
		return not_contained(error, "A temporary name leads to another file than Axis made: it is left as it is.");
	/* The other name, now: the very file, on the same filesystem.  Its link
	 * count says nothing (a name outside, of a cross-seed or a backup, would
	 * count too). */
	if (!look(other_dir, other_name, &other) || !S_ISREG(other.st_mode) || other.st_ino != st.st_ino ||
	    other.st_dev != st.st_dev)
		return not_contained(error, "A temporary name is kept: no other name of its file in the library or the "
			"download leads to it, so it may be the file's one name there.");
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	if (unlinkat(dir, temp_name, 0) != 0) {
		if (errno == ENOENT)
			return TRUE;
		return system_error(error, errno, "Cannot remove a temporary file");
	}
	if (removed != NULL)
		*removed = TRUE;
	return TRUE;
}

gboolean
axis_volume_write_link_temp(
	AxisVolumeGuard  *guard,
	gint              src_dir,
	const gchar      *src_name,
	guint64           src_ino,
	gint              dst_dir,
	const gchar      *temp_name,
	GError          **error
){
	struct stat st;

	if (!may_write(guard, src_dir, error) || !on_guard_fs(guard, dst_dir, error))
		return FALSE;
	if (!component_valid(src_name) || !axis_volume_write_temp_name_valid(temp_name))
		return invalid(error, "A name to link is not allowed.");
	if (!look(src_dir, src_name, &st))
		return system_error(error, errno, "Cannot look at the download's file");
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != src_ino)
		return not_contained(error, "The download's file is not the one that was checked.");
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	/* Flags 0: a link at @src_name is linked as itself, never followed (and
	 * it was refused above anyway). */
	if (linkat(src_dir, src_name, dst_dir, temp_name, 0) != 0) {
		gint err = errno;

		if (err == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The temporary name is taken already.");
			return FALSE;
		}
		return system_error(error, err, "Cannot hardlink the download's file");
	}
	if (!look(dst_dir, temp_name, &st) || (guint64)st.st_ino != src_ino) {
		/* The source's name was replaced between the look and the link: the
		 * new name is a second name of something else, whose own name
		 * stays.  It goes again. */
		if (unlinkat(dst_dir, temp_name, 0) != 0)
			g_debug("storage: a wrong link could not be removed again: %s", g_strerror(errno));
		return not_contained(error, "The download's file changed while it was being linked.");
	}
	return TRUE;
}

/* ── Moving a download's file ───────────────────────────────────── */

/*
 * @from_name in @from_dir, which must be the regular file @ino, gets the name
 * @to_name in @to_dir instead: one rename that never replaces a name, or,
 * where the filesystem has no RENAME_NOREPLACE, a link (which never replaces
 * one either), checked, and then the removal of the old name.  The file has
 * one of its names, or both, at every moment.
 */
static gboolean
rename_checked(
	AxisVolumeGuard  *guard,
	gint              from_dir,
	const gchar      *from_name,
	guint64           ino,
	gint              to_dir,
	const gchar      *to_name,
	GError          **error
){
	struct stat st;

	if (!look(from_dir, from_name, &st))
		return system_error(error, errno, "Cannot look at the file to move");
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != ino)
		return not_contained(error, "The file to move is not the one that was checked.");
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	if (renameat2(from_dir, from_name, to_dir, to_name, RENAME_NOREPLACE) == 0) {
		if (!look(to_dir, to_name, &st) || (guint64)st.st_ino != ino) {
			/* The old name was replaced between the look and the rename, so
			 * something else was moved: it goes back, if nothing took its name
			 * (this filesystem has RENAME_NOREPLACE: the rename just made
			 * shows it). */
			if (renameat2(to_dir, to_name, from_dir, from_name, RENAME_NOREPLACE) != 0)
				g_warning("storage: a file moved by mistake keeps its new name: %s", g_strerror(errno));
			return not_contained(error, "The file changed while it was being moved.");
		}
	} else {
		gint err = errno;

		if (err == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A file of that name is there already.");
			return FALSE;
		}
		if (err != EINVAL && err != ENOSYS)
			return system_error(error, err, "Cannot move the file");
		/* No RENAME_NOREPLACE here: a link (which never replaces a name
		 * either), checked before the old name goes.  What is linked is never
		 * moved by mistake: a link of the wrong file is removed again, and the
		 * wrong file keeps its own name. */
		if (!axis_volume_guard_writable(guard, error))
			return FALSE;
		if (linkat(from_dir, from_name, to_dir, to_name, 0) != 0) {
			err = errno;
			if (err == EEXIST) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A file of that name is there already.");
				return FALSE;
			}
			return system_error(error, err, "Cannot move the file");
		}
		if (!look(to_dir, to_name, &st) || (guint64)st.st_ino != ino) {
			/* The old name was replaced between the look and the link: the
			 * new name is a second name of something else, and goes again. */
			if (unlinkat(to_dir, to_name, 0) != 0)
				g_debug("storage: a wrong link could not be removed again: %s", g_strerror(errno));
			return not_contained(error, "The file changed while it was being moved.");
		}
		if (unlinkat(from_dir, from_name, 0) != 0)
			return system_error(error, errno, "Cannot take the file's old name away (it has its new name as well)");
	}
	sync_dir(to_dir);
	sync_dir(from_dir);
	return TRUE;
}

gboolean
axis_volume_write_move_temp(
	AxisVolumeGuard  *guard,
	gint              src_dir,
	const gchar      *src_name,
	guint64           src_ino,
	gint              dst_dir,
	const gchar      *temp_name,
	GError          **error
){
	if (!may_write(guard, src_dir, error) || !on_guard_fs(guard, dst_dir, error))
		return FALSE;
	if (!component_valid(src_name) || axis_volume_write_temp_name_valid(src_name) ||
	    !axis_volume_write_temp_name_valid(temp_name))
		return invalid(error, "A name to move is not allowed.");
	return rename_checked(guard, src_dir, src_name, src_ino, dst_dir, temp_name, error);
}

gboolean
axis_volume_write_move_back(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           ino,
	gint              src_dir,
	const gchar      *src_name,
	GError          **error
){
	if (!may_write(guard, dir, error) || !on_guard_fs(guard, src_dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name) || !component_valid(src_name) ||
	    axis_volume_write_temp_name_valid(src_name))
		return invalid(error, "A name to move back is not allowed.");
	return rename_checked(guard, dir, temp_name, ino, src_dir, src_name, error);
}

gboolean
axis_volume_write_remove_moved(
	AxisVolumeGuard            *guard,
	gint                        dir,
	const gchar                *name,
	guint64                     ino,
	guint64                     size,
	gint64                      mtime_ns,
	gint64                      ctime_ns,
	AxisVolumeRecycledOutcome  *outcome,
	GError                    **error
){
	struct stat st;
	struct stat named;
	gint fd;

	*outcome = AXIS_VOLUME_RECYCLED_CHANGED;
	if (!may_write(guard, dir, error))
		return FALSE;
	if (!component_valid(name) || axis_volume_write_temp_name_valid(name))
		return invalid(error, "The download's file name is not allowed.");
	if (ino == 0 || mtime_ns == 0 || ctime_ns == 0)
		return invalid(error, "The download's file is not known by its inode and its times.");
	/* Not opened unless it is a regular file (a device is never opened). */
	if (!look(dir, name, &named)) {
		if (errno == ENOENT) {
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
			return TRUE;
		}
		return system_error(error, errno, "Cannot look at the download's file");
	}
	if (!S_ISREG(named.st_mode))
		return TRUE;
	fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT) {
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
			return TRUE;
		}
		if (errno == ELOOP)
			return TRUE;
		return system_error(error, errno, "Cannot open the download's file");
	}
	/* On the descriptor: the very file copied, untouched since (a file
	 * deleted and another made at its name can have its inode number and its
	 * size; not its times). */
	if (fstat(fd, &st) != 0) {
		gint err = errno;

		close(fd);
		return system_error(error, err, "Cannot look at the download's file");
	}
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != ino || (guint64)st.st_size != size ||
	    (guint64)st.st_dev != axis_volume_guard_get_dev(guard) ||
	    (gint64)st.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) + st.st_mtim.tv_nsec != mtime_ns ||
	    (gint64)st.st_ctim.tv_sec * G_GINT64_CONSTANT(1000000000) + st.st_ctim.tv_nsec != ctime_ns) {
		close(fd);
		return TRUE;
	}
	/* Its name still leads to it, just before it goes. */
	if (!look(dir, name, &named)) {
		gboolean gone = errno == ENOENT;

		close(fd);
		if (gone)
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
		return TRUE;
	}
	if (named.st_ino != st.st_ino || named.st_dev != st.st_dev) {
		close(fd);
		return TRUE;
	}
	if (!axis_volume_guard_writable(guard, error)) {
		close(fd);
		return FALSE;
	}
	if (unlinkat(dir, name, 0) != 0) {
		gint err = errno;

		close(fd);
		if (err == ENOENT) {
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
			return TRUE;
		}
		return system_error(error, err, "Cannot remove the download's file");
	}
	close(fd);
	sync_dir(dir);
	*outcome = AXIS_VOLUME_RECYCLED_REMOVED;
	return TRUE;
}

/* ── Copying ────────────────────────────────────────────────────── */

static gboolean
write_all(
	gint          fd,
	const guchar *data,
	gsize         length,
	gint         *err
){
	while (length > 0) {
		gssize n = write(fd, data, length);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			*err = errno;
			return FALSE;
		}
		if (n == 0) {
			*err = EIO;
			return FALSE;
		}
		data += n;
		length -= (gsize)n;
	}
	return TRUE;
}

/* The temporary copy goes again (a copy: its source is still there). */
static void
drop_copy(
	gint         dir,
	const gchar *temp_name,
	guint64      ino
){
	struct stat st;

	if (look(dir, temp_name, &st) && S_ISREG(st.st_mode) && (guint64)st.st_ino == ino &&
	    unlinkat(dir, temp_name, 0) != 0)
		g_warning("storage: a temporary copy could not be removed: %s", g_strerror(errno));
}

/*
 * The same file, untouched, from @a (before its first byte was read) to @b
 * (after its last): a write changes its modification time (unless something
 * set it back) and its change time, and anything else done to it (a mode, an
 * owner, a link, times set by hand) the latter.
 *
 * One change of the change time is not a change of what was read: the file
 * deleted meanwhile (no name left, its data unwritten: the same size and
 * modification time), with nothing at its name in @dir now.  What was read
 * is all of it, and nothing can remove it afterwards, so the copy stands, as
 * the import promises.  A file renamed over its name also leaves the one
 * read without a name, unwritten; its name then leads to the other, newer
 * file, and the copy is of what was replaced: a change.  Without @name
 * (-1, %NULL) the two cannot be told apart, and neither stands.
 */
static gboolean
same_source(
	const struct stat *a,
	const struct stat *b,
	gint               dir,
	const gchar       *name
){
	struct stat st;

	if (a->st_size != b->st_size || a->st_ino != b->st_ino || a->st_dev != b->st_dev ||
	    a->st_mtim.tv_sec != b->st_mtim.tv_sec || a->st_mtim.tv_nsec != b->st_mtim.tv_nsec)
		return FALSE;
	if (a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec)
		return TRUE;
	if (a->st_nlink == 0 || b->st_nlink != 0 || dir < 0 || name == NULL)
		return FALSE;
	return fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
}

gboolean
axis_volume_write_copy_temp(
	AxisVolumeGuard     *guard,
	gint                 source,
	gint                 src_dir,
	const gchar         *src_name,
	guint64              size,
	gint                 dst_dir,
	const gchar         *temp_name,
	AxisVolumeCopyStep   step,
	gpointer             step_data,
	guint64             *ino_out,
	AxisVolumeSourceId  *copied,
	gchar              **checksum_out,
	GError             **error
){
	g_autoptr(GChecksum) written_sum = NULL;
	g_autoptr(GChecksum) read_sum = NULL;
	g_autofree guchar *buffer = NULL;
	g_autofree gchar *expected = NULL;
	struct stat before;
	struct stat after;
	struct stat st;
	guint64 offset = 0;
	guint64 ino;
	gint64 last_step;
	gint64 last_check;
	gint err = 0;
	gint fd;

	*ino_out = 0;
	*checksum_out = NULL;
	if (copied != NULL)
		memset(copied, 0, sizeof(*copied));
	if (!may_write(guard, dst_dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name))
		return invalid(error, "A temporary name is not allowed.");
	/* What the file is, on the descriptor it is read through, before its
	 * first byte is: what is copied is this, or nothing. */
	if (fstat(source, &before) != 0)
		return system_error(error, errno, "Cannot read the download's file");
	if (!S_ISREG(before.st_mode) || (guint64)before.st_size != size)
		return not_contained(error, "The download's file is not the one that was checked.");

	fd = openat(dst_dir, temp_name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC, 0644);
	if (fd < 0) {
		if (errno == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The temporary name is taken already.");
			return FALSE;
		}
		return system_error(error, errno, "Cannot make the temporary file");
	}
	if (fstat(fd, &st) != 0) {
		err = errno;
		close(fd);
		if (unlinkat(dst_dir, temp_name, 0) != 0)
			g_warning("storage: a temporary file could not be removed: %s", g_strerror(errno));
		return system_error(error, err, "Cannot read the temporary file");
	}
	ino = (guint64)st.st_ino;
	*ino_out = ino;
	if (step != NULL && !step(step_data, ino, 0, error))
		goto fail;

	buffer = g_malloc(COPY_CHUNK);
	written_sum = g_checksum_new(G_CHECKSUM_SHA256);
	last_step = last_check = g_get_monotonic_time();
	while (offset < size) {
		gsize want = (gsize)MIN((guint64)COPY_CHUNK, size - offset);
		gssize n = pread(source, buffer, want, (off_t)offset);
		gint64 now;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			system_error(error, errno, "Cannot read the download's file");
			goto fail;
		}
		if (n == 0) {
			not_contained(error, "The download's file became shorter while it was copied.");
			goto fail;
		}
		if (!axis_volume_guard_writable(guard, error))
			goto fail;
		g_checksum_update(written_sum, buffer, n);
		if (!write_all(fd, buffer, (gsize)n, &err)) {
			system_error(error, err, "Cannot write the copy");
			goto fail;
		}
		offset += (guint64)n;
		now = g_get_monotonic_time();
		/* The whole size is said once, below: written, the source the same
		 * throughout, and flushed. */
		if (step != NULL && offset < size && now - last_step >= COPY_STEP_US) {
			last_step = now;
			if (!step(step_data, ino, offset, error))
				goto fail;
		}
		if (now - last_check >= COPY_CHECK_US) {
			last_check = now;
			if (!axis_volume_guard_check(guard, error))
				goto fail;
		}
	}
	/* Nothing more than what was checked, and the same file throughout, to
	 * the nanosecond of both its times.  Anything done to it after this is
	 * for its removal to find (axis_volume_write_remove_moved(), given
	 * *@copied). */
	{
		guchar extra;
		gssize n = pread(source, &extra, 1, (off_t)size);

		if (n != 0 || fstat(source, &after) != 0 || !same_source(&before, &after, src_dir, src_name)) {
			not_contained(error, "The download's file changed while it was copied.");
			goto fail;
		}
	}
	if (fsync(fd) != 0) {
		system_error(error, errno, "Cannot flush the copy to the disk");
		goto fail;
	}
	/* So that reading it back reads the disk, not the cache, where the
	 * system lets it. */
	(void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	if (step != NULL && !step(step_data, ino, offset, error))
		goto fail;
	close(fd);
	fd = -1;

	/* Read back what was written, from the start. */
	fd = openat(dst_dir, temp_name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st) != 0 || (guint64)st.st_ino != ino) {
		not_contained(error, "The copy could not be read back.");
		goto fail;
	}
	if ((guint64)st.st_size != size) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The copy has %" G_GUINT64_FORMAT " bytes, not %"
			G_GUINT64_FORMAT ".", (guint64)st.st_size, size);
		goto fail;
	}
	(void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	read_sum = g_checksum_new(G_CHECKSUM_SHA256);
	offset = 0;
	for (;;) {
		gssize n = read(fd, buffer, COPY_CHUNK);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			system_error(error, errno, "Cannot read the copy back");
			goto fail;
		}
		if (n == 0)
			break;
		g_checksum_update(read_sum, buffer, n);
		offset += (guint64)n;
	}
	close(fd);
	fd = -1;
	expected = g_strdup(g_checksum_get_string(written_sum));
	if (offset != size || strcmp(expected, g_checksum_get_string(read_sum)) != 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"The copy read back is not what was written: its checksum differs.");
		goto fail;
	}
	*checksum_out = g_steal_pointer(&expected);
	if (copied != NULL) {
		copied->ino = (guint64)before.st_ino;
		copied->size = (guint64)before.st_size;
		copied->mtime_ns = (gint64)before.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) + before.st_mtim.tv_nsec;
		copied->ctime_ns = (gint64)before.st_ctim.tv_sec * G_GINT64_CONSTANT(1000000000) + before.st_ctim.tv_nsec;
	}
	return TRUE;

fail:
	if (fd >= 0)
		close(fd);
	drop_copy(dst_dir, temp_name, ino);
	return FALSE;
}

/* ── Final names ────────────────────────────────────────────────── */

gboolean
axis_volume_write_publish(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	GError          **error
){
	return axis_volume_write_publish_synced(guard, dir, temp_name, temp_ino, final_name, NULL, error);
}

gboolean
axis_volume_write_publish_synced(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	gboolean         *synced,
	GError          **error
){
	struct stat st;

	if (synced != NULL)
		*synced = FALSE;
	if (!may_write(guard, dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name) || !component_valid(final_name) ||
	    axis_volume_write_temp_name_valid(final_name) || final_name[0] == '.')
		return invalid(error, "A final name is not allowed.");
	if (!look(dir, temp_name, &st))
		return system_error(error, errno, "Cannot look at the temporary file");
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != temp_ino)
		return not_contained(error, "The temporary name leads to another file than Axis made.");
	if (renameat2(dir, temp_name, dir, final_name, RENAME_NOREPLACE) != 0) {
		gint err = errno;

		if (err == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A file of that name is there already.");
			return FALSE;
		}
		if (err != EINVAL && err != ENOSYS)
			return system_error(error, err, "Cannot give the file its name");
		/* No RENAME_NOREPLACE here: a link never replaces a name either. */
		if (!axis_volume_guard_writable(guard, error))
			return FALSE;
		if (linkat(dir, temp_name, dir, final_name, 0) != 0) {
			err = errno;
			if (err == EEXIST) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A file of that name is there already.");
				return FALSE;
			}
			return system_error(error, err, "Cannot give the file its name");
		}
		if (!look(dir, final_name, &st) || (guint64)st.st_ino != temp_ino)
			return not_contained(error, "The final name does not lead to the file placed.");
		if (unlinkat(dir, temp_name, 0) != 0)
			g_warning("storage: a temporary name could not be removed after its link: %s", g_strerror(errno));
	}
	if (!look(dir, final_name, &st) || (guint64)st.st_ino != temp_ino)
		return not_contained(error, "The final name does not lead to the file placed.");
	if (sync_published(dir)) {
		if (synced != NULL)
			*synced = TRUE;
	} else {
		g_warning("storage: a file has its final name, but its folder could not be flushed to the disk");
	}
	return TRUE;
}

gboolean
axis_volume_write_swap(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	guint64           old_ino,
	GError          **error
){
	return axis_volume_write_swap_synced(guard, dir, temp_name, temp_ino, final_name, old_ino, NULL, error);
}

gboolean
axis_volume_write_swap_synced(
	AxisVolumeGuard  *guard,
	gint              dir,
	const gchar      *temp_name,
	guint64           temp_ino,
	const gchar      *final_name,
	guint64           old_ino,
	gboolean         *synced,
	GError          **error
){
	struct stat temp_st;
	struct stat final_st;

	if (synced != NULL)
		*synced = FALSE;
	if (!may_write(guard, dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name) || !component_valid(final_name) ||
	    axis_volume_write_temp_name_valid(final_name) || final_name[0] == '.')
		return invalid(error, "A final name is not allowed.");
	if (!look(dir, temp_name, &temp_st) || !S_ISREG(temp_st.st_mode) || (guint64)temp_st.st_ino != temp_ino)
		return not_contained(error, "The temporary name leads to another file than Axis made.");
	if (!look(dir, final_name, &final_st) || !S_ISREG(final_st.st_mode) || (guint64)final_st.st_ino != old_ino)
		return not_contained(error, "The file to replace is not the one Axis knows.");
	if (final_st.st_nlink < 2)
		return not_contained(error, "The file to replace has no other name: its name in the recycle folder must be "
			"made first.");
	if (renameat2(dir, temp_name, dir, final_name, RENAME_EXCHANGE) != 0) {
		gint err = errno;

		if (err == EINVAL || err == ENOSYS) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"This filesystem cannot swap two names at once.");
			return FALSE;
		}
		return system_error(error, err, "Cannot swap the new file in");
	}
	if (!look(dir, temp_name, &temp_st) || (guint64)temp_st.st_ino != old_ino ||
	    !look(dir, final_name, &final_st) || (guint64)final_st.st_ino != temp_ino) {
		/* Something else was there: put the names back as they were. */
		if (renameat2(dir, temp_name, dir, final_name, RENAME_EXCHANGE) != 0)
			g_warning("storage: a swap could not be undone: %s", g_strerror(errno));
		return not_contained(error, "The file to replace changed while it was being replaced.");
	}
	if (sync_published(dir)) {
		if (synced != NULL)
			*synced = TRUE;
	} else {
		g_warning("storage: a new file has its final name, but its folder could not be flushed to the disk");
	}
	return TRUE;
}

/* ── The recycle folder ─────────────────────────────────────────── */

/* The place @where in the recycle folder, made as needed. */
static gint
open_recycle(
	AxisVolumeGuard     *guard,
	const gchar *const  *where,
	guint                dir_mode,
	GError             **error
){
	gint dir;
	guint i;

	dir = axis_volume_write_mkdir(guard, axis_volume_guard_get_fd(guard), AXIS_VOLUME_RECYCLE_DIR, dir_mode, NULL,
		NULL, error);
	for (i = 0; dir >= 0 && where != NULL && where[i] != NULL; i++) {
		gint next = axis_volume_write_mkdir(guard, dir, where[i], dir_mode, NULL, NULL, error);

		close(dir);
		dir = next;
		if (i >= 64) {
			if (dir >= 0)
				close(dir);
			invalid(error, "The place in the recycle folder is too deep.");
			return -1;
		}
	}
	return dir;
}

/* @base, or @base and ".n", within 255 bytes. */
static gchar *
recycle_candidate(
	const gchar *base,
	guint        n
){
	gchar suffix[16];
	gsize room;

	if (n == 0)
		return g_strdup(base);
	g_snprintf(suffix, sizeof(suffix), ".%u", n);
	room = 255 - strlen(suffix);
	return g_strdup_printf("%.*s%s", (gint)MIN(strlen(base), room), base, suffix);
}

gboolean
axis_volume_write_retire(
	AxisVolumeGuard      *guard,
	gint                  src_dir,
	const gchar          *src_name,
	guint64               src_ino,
	const gchar *const   *where,
	guint                 dir_mode,
	AxisVolumeRetireHow   how,
	gchar               **recycle_name,
	GError              **error
){
	g_autofree gchar *chosen = NULL;
	struct stat st;
	gint dir;
	guint n;

	*recycle_name = NULL;
	if (!may_write(guard, src_dir, error))
		return FALSE;
	if (!component_valid(src_name))
		return invalid(error, "The name to retire is not allowed.");
	if (!look(src_dir, src_name, &st))
		return system_error(error, errno, "Cannot look at the file to retire");
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != src_ino)
		return not_contained(error, "The file to retire is not the one Axis knows.");
	dir = open_recycle(guard, where, dir_mode, error);
	if (dir < 0)
		return FALSE;

	for (n = 0; n < RECYCLE_TRIES; n++) {
		gint rc;

		g_free(chosen);
		chosen = recycle_candidate(src_name, n);
		if (!axis_volume_guard_writable(guard, error)) {
			close(dir);
			return FALSE;
		}
		if (how == AXIS_VOLUME_RETIRE_LINK) {
			rc = linkat(src_dir, src_name, dir, chosen, 0);
		} else {
			rc = renameat2(src_dir, src_name, dir, chosen, RENAME_NOREPLACE);
			if (rc != 0 && (errno == EINVAL || errno == ENOSYS)) {
				/* No RENAME_NOREPLACE: a link, then the library's name goes
				 * once the recycle folder's name is certainly the file. */
				rc = linkat(src_dir, src_name, dir, chosen, 0);
				if (rc == 0) {
					struct stat a;
					struct stat b;

					if (!look(dir, chosen, &a) || (guint64)a.st_ino != src_ino || !look(src_dir, src_name, &b) ||
					    (guint64)b.st_ino != src_ino) {
						close(dir);
						return not_contained(error, "The file to retire changed while it was being retired.");
					}
					if (unlinkat(src_dir, src_name, 0) != 0) {
						gint err = errno;

						close(dir);
						return system_error(error, err, "Cannot take the old file's name away (it is in the "
							"recycle folder as well)");
					}
				}
			}
		}
		if (rc == 0)
			break;
		if (errno != EEXIST) {
			gint err = errno;

			close(dir);
			return system_error(error, err, "Cannot retire the old file");
		}
	}
	if (n == RECYCLE_TRIES) {
		close(dir);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The recycle folder has too many files of that name.");
		return FALSE;
	}
	if (!look(dir, chosen, &st) || (guint64)st.st_ino != src_ino) {
		/* Something else was moved: put it back where it was, if nothing
		 * took that name meanwhile. */
		if (how == AXIS_VOLUME_RETIRE_MOVE && renameat2(dir, chosen, src_dir, src_name, RENAME_NOREPLACE) != 0)
			g_warning("storage: a file retired by mistake stays in the recycle folder: %s", g_strerror(errno));
		close(dir);
		return not_contained(error, "The file to retire changed while it was being retired.");
	}
	sync_dir(dir);
	sync_dir(src_dir);
	close(dir);
	*recycle_name = g_steal_pointer(&chosen);
	return TRUE;
}

gboolean
axis_volume_write_remove_recycled(
	AxisVolumeGuard            *guard,
	const gchar *const         *parts,
	guint64                     ino,
	guint64                     size,
	AxisVolumeRecycledOutcome  *outcome,
	GError                    **error
){
	gint dirs[66];
	guint depth = 0;
	guint n;
	guint i;
	struct stat st;
	gboolean ok = TRUE;

	*outcome = AXIS_VOLUME_RECYCLED_CHANGED;
	g_return_val_if_fail(guard != NULL, FALSE);
	if (!proven(guard, error))
		return FALSE;
	n = parts != NULL ? g_strv_length((gchar **)parts) : 0;
	if (n < 2 || n > 64 || strcmp(parts[0], AXIS_VOLUME_RECYCLE_DIR) != 0)
		return invalid(error, "Only a file inside the recycle folder can be removed from it.");
	for (i = 0; i < n; i++) {
		if (!component_valid(parts[i]))
			return invalid(error, "A recorded place in the recycle folder is not allowed.");
	}

	/* The directories, from the top, none a link or on another filesystem. */
	dirs[0] = axis_volume_guard_get_fd(guard);
	for (i = 0; i + 1 < n; i++) {
		gint next = openat(dirs[depth], parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);

		if (next < 0) {
			gint err = errno;

			*outcome = err == ENOENT ? AXIS_VOLUME_RECYCLED_GONE : AXIS_VOLUME_RECYCLED_CHANGED;
			if (err != ENOENT && err != ELOOP && err != ENOTDIR)
				ok = system_error(error, err, "Cannot open a folder of the recycle folder");
			goto out;
		}
		dirs[++depth] = next;
		if (!on_guard_fs(guard, next, NULL)) {
			*outcome = AXIS_VOLUME_RECYCLED_CHANGED;
			goto out;
		}
	}
	if (!look(dirs[depth], parts[n - 1], &st)) {
		if (errno == ENOENT)
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
		else
			ok = system_error(error, errno, "Cannot look at a file of the recycle folder");
		goto out;
	}
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != ino || (guint64)st.st_size != size) {
		*outcome = AXIS_VOLUME_RECYCLED_CHANGED;
		goto out;
	}
	if (!axis_volume_guard_writable(guard, error)) {
		ok = FALSE;
		goto out;
	}
	if (unlinkat(dirs[depth], parts[n - 1], 0) != 0) {
		if (errno == ENOENT)
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
		else
			ok = system_error(error, errno, "Cannot remove a file of the recycle folder");
		goto out;
	}
	*outcome = AXIS_VOLUME_RECYCLED_REMOVED;
	sync_dir(dirs[depth]);
	/* The folders above it, while they are empty; never the recycle folder
	 * itself (dirs[1] is it, parts[0] its name). */
	for (i = depth; i >= 2; i--) {
		if (unlinkat(dirs[i - 1], parts[i - 1], AT_REMOVEDIR) != 0)
			break;
	}

out:
	for (i = depth; i >= 1; i--)
		close(dirs[i]);
	return ok;
}

/* ── A download's leftover folder ───────────────────────────────── */

/* How often (in names removed) the volume is proven again. */
#define FOLDER_PROOF_EVERY 500

typedef struct
{
	AxisVolumeGuard          *guard;
	guint                     seen;	/* names looked at by the survey */
	guint                     removed;
	AxisVolumeFolderOutcome   refusal;	/* why the survey refused */
} FolderWalk;

/* The names in @dir (bytes), other than "." and "..": a new reading of it,
 * not the descriptor's own position.  %NULL with errno set. */
static GPtrArray *
folder_names(gint dir)
{
	GPtrArray *names;
	struct dirent *entry;
	DIR *stream;
	gint copy;

	copy = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (copy < 0)
		return NULL;
	stream = fdopendir(copy);
	if (stream == NULL) {
		gint err = errno;

		close(copy);
		errno = err;
		return NULL;
	}
	names = g_ptr_array_new_with_free_func(g_free);
	errno = 0;
	while ((entry = readdir(stream)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		g_ptr_array_add(names, g_strdup(entry->d_name));
		if (names->len > AXIS_VOLUME_FOLDER_ENTRIES)
			break;
		errno = 0;
	}
	if (errno != 0) {
		gint err = errno;

		closedir(stream);
		g_ptr_array_unref(names);
		errno = err;
		return NULL;
	}
	closedir(stream);
	return names;
}

/* Looks at the tree below @dir, changing nothing: %FALSE with walk->refusal
 * set when it is not one to remove, or with @error for the system's. */
static gboolean
folder_survey(
	FolderWalk  *walk,
	gint         dir,
	guint        depth,
	GError     **error
){
	g_autoptr(GPtrArray) names = folder_names(dir);
	guint i;

	if (names == NULL)
		return system_error(error, errno, "Cannot read a folder of the download");
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		struct stat st;
		gint child;
		gboolean ok;

		if (++walk->seen > AXIS_VOLUME_FOLDER_ENTRIES) {
			walk->refusal = AXIS_VOLUME_FOLDER_TOO_BIG;
			return FALSE;
		}
		if (!look(dir, name, &st)) {
			if (errno == ENOENT)
				continue;
			return system_error(error, errno, "Cannot look at a name in the download's folder");
		}
		if (!S_ISDIR(st.st_mode))
			continue;
		if (depth + 1 > AXIS_VOLUME_FOLDER_DEPTH) {
			walk->refusal = AXIS_VOLUME_FOLDER_TOO_BIG;
			return FALSE;
		}
		child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
		if (child < 0) {
			if (errno == ENOENT || errno == ELOOP || errno == ENOTDIR)
				continue;	/* Gone, or a name now: removed as a name. */
			return system_error(error, errno, "Cannot open a folder in the download's folder");
		}
		if (!on_guard_fs(walk->guard, child, NULL)) {
			close(child);
			walk->refusal = AXIS_VOLUME_FOLDER_OTHER_FS;
			return FALSE;
		}
		ok = folder_survey(walk, child, depth + 1, error);
		close(child);
		if (!ok)
			return FALSE;
	}
	return TRUE;
}

/* Removes every name below @dir, deepest first. */
static gboolean
folder_empty(
	FolderWalk  *walk,
	gint         dir,
	guint        depth,
	GError     **error
){
	g_autoptr(GPtrArray) names = folder_names(dir);
	guint i;

	if (names == NULL)
		return system_error(error, errno, "Cannot read a folder of the download");
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		struct stat st;
		gint flags = 0;

		if (!look(dir, name, &st)) {
			if (errno == ENOENT)
				continue;
			return system_error(error, errno, "Cannot look at a name in the download's folder");
		}
		if (S_ISDIR(st.st_mode)) {
			gint child;
			gboolean ok;

			if (depth + 1 > AXIS_VOLUME_FOLDER_DEPTH) {
				walk->refusal = AXIS_VOLUME_FOLDER_TOO_BIG;
				return not_contained(error, "A folder inside the download's folder is deeper than Axis removes.");
			}
			child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
			if (child < 0) {
				if (errno == ENOENT)
					continue;
				if (errno != ELOOP && errno != ENOTDIR)
					return system_error(error, errno, "Cannot open a folder in the download's folder");
				/* It became a link or a file meanwhile: removed as a name. */
			} else {
				if (!on_guard_fs(walk->guard, child, NULL)) {
					close(child);
					walk->refusal = AXIS_VOLUME_FOLDER_OTHER_FS;
					return not_contained(error, "A folder inside the download's folder is another filesystem now.");
				}
				ok = folder_empty(walk, child, depth + 1, error);
				close(child);
				if (!ok)
					return FALSE;
				flags = AT_REMOVEDIR;
			}
		}
		if (walk->removed > 0 && walk->removed % FOLDER_PROOF_EVERY == 0 && !proven(walk->guard, error))
			return FALSE;
		if (!axis_volume_guard_writable(walk->guard, error))
			return FALSE;
		if (unlinkat(dir, name, flags) != 0) {
			if (errno == ENOENT)
				continue;
			return system_error(error, errno, flags != 0 ? "Cannot remove a folder in the download's folder" :
				"Cannot remove a file of the download's folder");
		}
		walk->removed++;
	}
	return TRUE;
}

gboolean
axis_volume_write_remove_folder(
	AxisVolumeGuard           *guard,
	gint                       parent,
	const gchar               *name,
	guint64                    ino,
	AxisVolumeFolderOutcome   *outcome,
	guint                     *removed,
	GError                   **error
){
	FolderWalk walk;
	struct stat st;
	GError *local = NULL;
	gboolean ok;
	gint dir;

	*outcome = AXIS_VOLUME_FOLDER_CHANGED;
	if (removed != NULL)
		*removed = 0;
	if (!may_write(guard, parent, error))
		return FALSE;
	if (!component_valid(name))
		return invalid(error, "The download's folder name is not allowed.");
	if (!look(parent, name, &st)) {
		if (errno == ENOENT) {
			*outcome = AXIS_VOLUME_FOLDER_GONE;
			return TRUE;
		}
		return system_error(error, errno, "Cannot look at the download's folder");
	}
	if (!S_ISDIR(st.st_mode)) {
		*outcome = AXIS_VOLUME_FOLDER_NOT_FOLDER;
		return TRUE;
	}
	if (ino != 0 && (guint64)st.st_ino != ino)
		return TRUE;
	dir = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (dir < 0) {
		if (errno == ENOENT) {
			*outcome = AXIS_VOLUME_FOLDER_GONE;
			return TRUE;
		}
		if (errno == ELOOP || errno == ENOTDIR) {
			*outcome = AXIS_VOLUME_FOLDER_NOT_FOLDER;
			return TRUE;
		}
		return system_error(error, errno, "Cannot open the download's folder");
	}
	if (fstat(dir, &st) != 0) {
		gint err = errno;

		close(dir);
		return system_error(error, err, "Cannot read the download's folder");
	}
	if (ino != 0 && (guint64)st.st_ino != ino) {
		close(dir);
		return TRUE;
	}
	if (!on_guard_fs(guard, dir, NULL)) {
		close(dir);
		*outcome = AXIS_VOLUME_FOLDER_OTHER_FS;
		return TRUE;
	}

	/* The whole tree first, changing nothing. */
	memset(&walk, 0, sizeof(walk));
	walk.guard = guard;
	walk.refusal = AXIS_VOLUME_FOLDER_REMOVED;
	if (!folder_survey(&walk, dir, 0, error)) {
		close(dir);
		if (walk.refusal == AXIS_VOLUME_FOLDER_REMOVED)
			return FALSE;
		*outcome = walk.refusal;
		return TRUE;
	}

	/* Then its names, and the folder once it is still the one opened. */
	walk.refusal = AXIS_VOLUME_FOLDER_REMOVED;
	ok = folder_empty(&walk, dir, 0, &local);
	if (removed != NULL)
		*removed = walk.removed;
	if (ok) {
		struct stat held;

		sync_dir(dir);
		if (fstat(dir, &held) != 0) {
			gint err = errno;

			close(dir);
			return system_error(error, err, "Cannot read the download's folder");
		}
		close(dir);
		if (!look(parent, name, &st)) {
			if (errno != ENOENT)
				return system_error(error, errno, "Cannot look at the download's folder");
			/* Emptied, and its name gone meanwhile. */
			*outcome = AXIS_VOLUME_FOLDER_REMOVED;
			return TRUE;
		}
		if (!S_ISDIR(st.st_mode) || st.st_ino != held.st_ino || st.st_dev != held.st_dev) {
			/* Emptied, and its name taken meanwhile: what is there is left. */
			*outcome = AXIS_VOLUME_FOLDER_CHANGED;
			return TRUE;
		}
		if (!axis_volume_guard_writable(guard, error))
			return FALSE;
		if (unlinkat(parent, name, AT_REMOVEDIR) != 0 && errno != ENOENT)
			return system_error(error, errno, "Cannot remove the download's folder");
		sync_dir(parent);
		*outcome = AXIS_VOLUME_FOLDER_REMOVED;
		return TRUE;
	}
	close(dir);
	if (walk.refusal != AXIS_VOLUME_FOLDER_REMOVED &&
	    g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED)) {
		/* Found on the way (it changed after the survey): stopped there. */
		g_error_free(local);
		*outcome = walk.refusal;
		return TRUE;
	}
	g_propagate_error(error, local);
	return FALSE;
}

gboolean
axis_volume_write_remove_file(
	AxisVolumeGuard            *guard,
	gint                        dir,
	const gchar                *name,
	guint64                     ino,
	AxisVolumeRecycledOutcome  *outcome,
	GError                    **error
){
	struct stat st;

	*outcome = AXIS_VOLUME_RECYCLED_CHANGED;
	if (!may_write(guard, dir, error))
		return FALSE;
	if (!component_valid(name))
		return invalid(error, "The download's file name is not allowed.");
	if (!look(dir, name, &st)) {
		if (errno == ENOENT) {
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
			return TRUE;
		}
		return system_error(error, errno, "Cannot look at the download's file");
	}
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != ino ||
	    (guint64)st.st_dev != axis_volume_guard_get_dev(guard))
		return TRUE;
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	if (unlinkat(dir, name, 0) != 0) {
		if (errno == ENOENT) {
			*outcome = AXIS_VOLUME_RECYCLED_GONE;
			return TRUE;
		}
		return system_error(error, errno, "Cannot remove the download's file");
	}
	sync_dir(dir);
	*outcome = AXIS_VOLUME_RECYCLED_REMOVED;
	return TRUE;
}

/* ── The work area (conversions) ────────────────────────────────── */

/* 1 to @max digits without a leading zero; *@end after them. */
static gboolean
number_of(
	const gchar  *p,
	guint         max,
	const gchar **end
){
	guint n = 0;

	if (*p == '0')
		return FALSE;
	while (g_ascii_isdigit(p[n]) && n <= max)
		n++;
	if (n == 0 || n > max)
		return FALSE;
	*end = p + n;
	return TRUE;
}

gboolean
axis_volume_write_work_name_valid(const gchar *name)
{
	const gchar *p = NULL;

	if (name == NULL)
		return FALSE;
	if (g_str_has_prefix(name, "trial-")) {
		if (!number_of(name + 6, 19, &p))
			return FALSE;
		return strcmp(p, ".mkv") == 0 || strcmp(p, ".mp4") == 0;
	}
	if (!number_of(name, 19, &p) || *p != '-' || !number_of(p + 1, 3, &p))
		return FALSE;
	return strcmp(p, ".tmp") == 0;
}

/* @work is the directory AXIS_VOLUME_WORK_DIR at the top of the guard's
 * volume, not some other directory. */
static gboolean
is_work_area(
	AxisVolumeGuard  *guard,
	gint              work,
	GError          **error
){
	struct stat entry;
	struct stat here;

	if (!on_guard_fs(guard, work, error))
		return FALSE;
	if (!look(axis_volume_guard_get_fd(guard), AXIS_VOLUME_WORK_DIR, &entry) || !S_ISDIR(entry.st_mode) ||
	    fstat(work, &here) != 0 || entry.st_ino != here.st_ino || entry.st_dev != here.st_dev)
		return not_contained(error, "The directory is not the volume's work area.");
	return TRUE;
}

static gboolean
may_write_work(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	GError          **error
){
	if (!may_write(guard, work, error) || !is_work_area(guard, work, error))
		return FALSE;
	if (!axis_volume_write_work_name_valid(name))
		return invalid(error, "Only a name of Axis's in the work area can be used.");
	return TRUE;
}

gboolean
axis_volume_write_settle(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	guint64           ino,
	guint             mode,
	guint             uid,
	guint             gid,
	gint64            mtime_sec,
	gint              mtime_nsec,
	gboolean          owner_must_match,
	struct stat      *st,
	GError          **error
){
	struct timespec times[2];
	struct stat now;
	gint fd;

	memset(st, 0, sizeof(*st));
	if (!may_write_work(guard, work, name, error))
		return FALSE;
	if (mtime_nsec < 0 || mtime_nsec > 999999999)
		return invalid(error, "A modification time is not valid.");
	fd = openat(work, name, O_RDONLY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ELOOP)
			return not_contained(error, "The output is a symbolic link.");
		return system_error(error, errno, "Cannot open the new file");
	}
	if (fstat(fd, &now) != 0) {
		gint err = errno;

		close(fd);
		return system_error(error, err, "Cannot look at the new file");
	}
	if (!S_ISREG(now.st_mode) || (guint64)now.st_ino != ino || now.st_nlink != 1) {
		close(fd);
		return not_contained(error, "The new file is not the one that was checked.");
	}
	if (now.st_uid != (uid_t)uid || now.st_gid != (gid_t)gid) {
		if (!axis_volume_guard_writable(guard, error)) {
			close(fd);
			return FALSE;
		}
		if (fchown(fd, (uid_t)uid, (gid_t)gid) != 0) {
			gint err = errno;

			if (owner_must_match || (err != EPERM && err != EACCES)) {
				close(fd);
				if (err == EPERM || err == EACCES) {
					g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
						"The new file's owner and group cannot be made the original's.");
					return FALSE;
				}
				return system_error(error, err, "Cannot give the new file its owner");
			}
			g_debug("storage: a new file keeps its owner: %s", g_strerror(err));
		}
	}
	times[0].tv_sec = 0;
	times[0].tv_nsec = UTIME_OMIT;
	times[1].tv_sec = (time_t)mtime_sec;
	times[1].tv_nsec = mtime_nsec;
	if (!axis_volume_guard_writable(guard, error)) {
		close(fd);
		return FALSE;
	}
	if (fchmod(fd, (mode_t)(mode & 0777)) != 0 || futimens(fd, times) != 0 || fsync(fd) != 0 || fstat(fd, st) != 0) {
		gint err = errno;

		close(fd);
		memset(st, 0, sizeof(*st));
		return system_error(error, err, "Cannot settle the new file");
	}
	close(fd);
	if (!look(work, name, &now) || (guint64)now.st_ino != ino) {
		memset(st, 0, sizeof(*st));
		return not_contained(error, "The new file's name changed while it was settled.");
	}
	return TRUE;
}

gboolean
axis_volume_write_adopt(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	guint64           ino,
	gint              dir,
	const gchar      *temp_name,
	GError          **error
){
	struct stat st;

	if (!may_write_work(guard, work, name, error) || !on_guard_fs(guard, dir, error))
		return FALSE;
	if (!axis_volume_write_temp_name_valid(temp_name))
		return invalid(error, "A temporary name is not allowed.");
	if (!look(work, name, &st))
		return system_error(error, errno, "Cannot look at the new file");
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != ino || st.st_nlink != 1)
		return not_contained(error, "The new file is not the one that was checked.");
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	if (renameat2(work, name, dir, temp_name, RENAME_NOREPLACE) != 0) {
		gint err = errno;

		if (err == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The temporary name is taken already.");
			return FALSE;
		}
		if (err != EINVAL && err != ENOSYS)
			return system_error(error, err, "Cannot move the new file into its folder");
		/* No RENAME_NOREPLACE: a link never replaces a name either. */
		if (!axis_volume_guard_writable(guard, error))
			return FALSE;
		if (linkat(work, name, dir, temp_name, 0) != 0) {
			err = errno;
			if (err == EEXIST) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The temporary name is taken already.");
				return FALSE;
			}
			return system_error(error, err, "Cannot move the new file into its folder");
		}
		if (!look(dir, temp_name, &st) || (guint64)st.st_ino != ino) {
			/* The work area's name was replaced between the look and the
			 * link: the new name is a second name of something else. */
			if (unlinkat(dir, temp_name, 0) != 0)
				g_debug("storage: a wrong link could not be removed again: %s", g_strerror(errno));
			return not_contained(error, "The new file changed while it was being moved.");
		}
		if (unlinkat(work, name, 0) != 0)
			return system_error(error, errno, "Cannot take the new file's name in the work area away (it has its "
				"temporary name as well)");
	}
	if (!look(dir, temp_name, &st) || (guint64)st.st_ino != ino) {
		/* Something else was moved: put it back, if nothing took the name. */
		if (renameat2(dir, temp_name, work, name, RENAME_NOREPLACE) != 0)
			g_warning("storage: a file moved by mistake stays under a temporary name: %s", g_strerror(errno));
		return not_contained(error, "The new file changed while it was being moved.");
	}
	sync_dir(dir);
	sync_dir(work);
	return TRUE;
}

gboolean
axis_volume_write_rename_work(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *from,
	guint64           ino,
	const gchar      *to,
	GError          **error
){
	struct stat st;

	if (!may_write_work(guard, work, from, error))
		return FALSE;
	if (!axis_volume_write_work_name_valid(to) || strcmp(from, to) == 0)
		return invalid(error, "Only a name of Axis's in the work area can be given.");
	if (!look(work, from, &st))
		return system_error(error, errno, "Cannot look at a file of the work area");
	if (!S_ISREG(st.st_mode) || (guint64)st.st_ino != ino)
		return not_contained(error, "A file of the work area is not the one recorded.");
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	if (renameat2(work, from, work, to, RENAME_NOREPLACE) != 0) {
		gint err = errno;

		if (err == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A file of that name is in the work area already.");
			return FALSE;
		}
		if (err != EINVAL && err != ENOSYS)
			return system_error(error, err, "Cannot rename a file of the work area");
		if (!axis_volume_guard_writable(guard, error))
			return FALSE;
		if (linkat(work, from, work, to, 0) != 0) {
			err = errno;
			if (err == EEXIST) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
					"A file of that name is in the work area already.");
				return FALSE;
			}
			return system_error(error, err, "Cannot rename a file of the work area");
		}
		if (!look(work, to, &st) || (guint64)st.st_ino != ino) {
			if (unlinkat(work, to, 0) != 0)
				g_debug("storage: a wrong link could not be removed again: %s", g_strerror(errno));
			return not_contained(error, "A file of the work area changed while it was renamed.");
		}
		if (unlinkat(work, from, 0) != 0)
			return system_error(error, errno, "Cannot take a file's old name in the work area away");
	}
	if (!look(work, to, &st) || (guint64)st.st_ino != ino) {
		if (renameat2(work, to, work, from, RENAME_NOREPLACE) != 0)
			g_warning("storage: a file of the work area renamed by mistake keeps its new name: %s", g_strerror(errno));
		return not_contained(error, "A file of the work area changed while it was renamed.");
	}
	sync_dir(work);
	return TRUE;
}

gboolean
axis_volume_write_unlink_work(
	AxisVolumeGuard  *guard,
	gint              work,
	const gchar      *name,
	guint64           ino,
	gboolean         *removed,
	GError          **error
){
	struct stat st;

	if (removed != NULL)
		*removed = FALSE;
	if (!may_write_work(guard, work, name, error))
		return FALSE;
	if (!look(work, name, &st)) {
		if (errno == ENOENT)
			return TRUE;
		return system_error(error, errno, "Cannot look at a file of the work area");
	}
	if (!S_ISREG(st.st_mode) || (ino != 0 && (guint64)st.st_ino != ino))
		return not_contained(error, "A file of the work area is not the one recorded: it is left as it is.");
	if (!axis_volume_guard_writable(guard, error))
		return FALSE;
	if (unlinkat(work, name, 0) != 0) {
		if (errno == ENOENT)
			return TRUE;
		return system_error(error, errno, "Cannot remove a file of the work area");
	}
	sync_dir(work);
	if (removed != NULL)
		*removed = TRUE;
	return TRUE;
}
