/*
 * axis-worker-output.c - the only changes axis-worker makes on a volume: a
 * conversion's output in the volume's work area
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The worker links libaxis-core.a alone, which has no volume guard and so
 * not the daemon's write layer (src/storage/axis-volume-write.c); this file
 * is the worker's equivalent, kept as small as it can be.  Its only
 * filesystem-changing calls:
 *
 *   openat(O_CREAT | O_EXCL | O_NOFOLLOW, 0600)  the output, under the name
 *                the daemon chose, in the work area (never an existing file)
 *   fsync        that file, before it is reported
 *   unlinkat     that file again, when the job ends without a result, or
 *                with one the daemon did not take (by its name, only while
 *                it is still the file this worker made)
 *
 * "Still the file this worker made" is decided by a descriptor of it that
 * is open at the moment of the comparison (the one it writes with, or a
 * read-only one opened while that was still open): both sides of the
 * comparison then come through the same mount while the file is held, so
 * it holds on a mount that makes inode numbers up (FUSE without use_ino
 * gives a file a new number once it is closed and forgotten) as on a local
 * disk.  The descriptor is closed before the name is removed: NFS and FUSE
 * rename an open file they are asked to remove (.nfs..., .fuse_hidden...)
 * instead of removing it.
 *
 * The work area is reached beneath the volume's proved directory
 * (AxisVolumeProof), never through a path, without following a link and
 * without leaving the volume's filesystem.  The worker never makes the work
 * area (the daemon does), and never renames, replaces or removes anything
 * else on a volume.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-worker-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

gint
axis_worker_output_area(
	gint     top,
	guint64  dev,
	gint    *err
){
	struct stat st;
	gint fd;

	*err = 0;
	fd = openat(top, AXIS_WORKER_WORK_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		*err = errno;
		return -1;
	}
	if (fstat(fd, &st) != 0 || (guint64)st.st_dev != dev) {
		*err = errno != 0 ? errno : EXDEV;
		close(fd);
		return -1;
	}
	return fd;
}

/* Whatever regular file has the name, right after this worker made it
 * (its descriptor could not even be looked at). */
static void
remove_fresh(
	gint         area,
	const gchar *name
){
	struct stat st;

	if (fstatat(area, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode))
		(void)unlinkat(area, name, 0);
}

gint
axis_worker_output_create(
	gint         area,
	const gchar *name,
	gint        *err
){
	struct stat st;
	gint fd;

	*err = 0;
	if (!axis_convert_output_name_valid(name)) {
		*err = EINVAL;
		return -1;
	}
	fd = openat(area, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC, 0600);
	if (fd < 0) {
		*err = errno;
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		*err = errno != 0 ? errno : EINVAL;
		close(fd);
		remove_fresh(area, name);
		return -1;
	}
	return fd;
}

gboolean
axis_worker_output_sync(
	gint  fd,
	gint *err
){
	*err = 0;
	if (fsync(fd) != 0) {
		*err = errno;
		return FALSE;
	}
	return TRUE;
}

gint
axis_worker_output_anchor(
	gint         area,
	const gchar *name,
	gint         out,
	gint        *err
){
	struct stat held;
	struct stat now;
	gint fd;

	*err = 0;
	if (area < 0 || out < 0 || !axis_convert_output_name_valid(name)) {
		*err = EINVAL;
		return -1;
	}
	fd = openat(area, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		*err = errno;
		return -1;
	}
	if (fstat(out, &held) != 0 || fstat(fd, &now) != 0 || !S_ISREG(now.st_mode) || now.st_dev != held.st_dev ||
	    now.st_ino != held.st_ino) {
		*err = errno != 0 ? errno : ESTALE;
		close(fd);
		return -1;
	}
	return fd;
}

gboolean
axis_worker_output_remove_held(
	gint         area,
	const gchar *name,
	gint        *held
){
	struct stat mine;
	struct stat st;
	gboolean same;

	if (*held < 0)
		return FALSE;
	if (area < 0 || !axis_convert_output_name_valid(name)) {
		close(*held);
		*held = -1;
		return FALSE;
	}
	if (fstatat(area, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		gboolean gone = errno == ENOENT;

		close(*held);
		*held = -1;
		return gone;
	}
	/* Only the file this worker made, compared while it is held open. */
	same = fstat(*held, &mine) == 0 && S_ISREG(st.st_mode) && st.st_dev == mine.st_dev && st.st_ino == mine.st_ino;
	close(*held);
	*held = -1;
	if (!same)
		return FALSE;
	if (unlinkat(area, name, 0) != 0)
		return errno == ENOENT;
	return TRUE;
}
