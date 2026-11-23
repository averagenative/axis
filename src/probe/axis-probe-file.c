/*
 * axis-probe-file.c - opening an indexed file through a volume guard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The scanner walks with openat() from the root's descriptor, never
 * following a link; so does this, along the path the index recorded.  A
 * directory or file that was replaced by a link, a mount of another
 * filesystem, or a pipe or device since the scan is "not there".
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "probe/axis-probe-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-error.h"

void
axis_probe_file_close(AxisProbeFile *file)
{
	if (file->fd >= 0)
		close(file->fd);
	if (file->dir_fd >= 0)
		close(file->dir_fd);
	g_free(file->name);
	memset(file, 0, sizeof(*file));
	file->fd = -1;
	file->dir_fd = -1;
}

/* Explains why an openat() failed: the volume going away is the volume's
 * error; anything else is a path that is not there as it was. */
static void
open_failed(
	AxisVolumeGuard  *guard,
	const gchar      *what,
	gint              saved_errno,
	GError          **error
){
	if (!axis_volume_guard_check(guard, error))
		return;
	if (saved_errno == ENOENT || saved_errno == ENOTDIR || saved_errno == ELOOP ||
	    saved_errno == EXDEV || saved_errno == EISDIR)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s is no longer there as it was indexed.", what);
	else
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "Cannot open %s: %s", what,
			g_strerror(saved_errno));
}

gboolean
axis_probe_file_open(
	AxisVolumeGuard  *guard,
	const gchar      *root_rel_path,
	const gchar      *rel_path,
	AxisProbeFile    *file,
	GError          **error
){
	g_autoptr(GString) bytes = NULL;
	g_auto(GStrv) segments = NULL;
	struct stat root_st;
	struct stat st;
	gint current;
	guint count;
	guint i;
	gint fd;
	gint flags;

	memset(file, 0, sizeof(*file));
	file->fd = -1;
	file->dir_fd = -1;

	if (!axis_library_name_decode(rel_path, &bytes) || bytes->len == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"The index holds a path that is not in its stored form.");
		return FALSE;
	}
	segments = g_strsplit(bytes->str, "/", -1);
	count = g_strv_length(segments);
	for (i = 0; i < count; i++) {
		if (segments[i][0] == '\0' || strcmp(segments[i], ".") == 0 || strcmp(segments[i], "..") == 0) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				"The index holds a path with an empty or relative part.");
			return FALSE;
		}
	}

	current = axis_volume_guard_open_dir(guard, root_rel_path, error);
	if (current < 0)
		return FALSE;
	if (fstat(current, &root_st) != 0) {
		gint saved = errno;

		close(current);
		open_failed(guard, "the root", saved, error);
		return FALSE;
	}

	for (i = 0; i + 1 < count; i++) {
		gint next = openat(current, segments[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);

		if (next < 0) {
			gint saved = errno;

			close(current);
			open_failed(guard, "A folder of the file", saved, error);
			return FALSE;
		}
		close(current);
		current = next;
		if (fstat(current, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_dev != root_st.st_dev) {
			close(current);
			if (!axis_volume_guard_check(guard, error))
				return FALSE;
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
				"A folder of the file is on another filesystem than its root.");
			return FALSE;
		}
	}

	/* No blocking on a pipe that was put where the file was, no access
	 * time written. */
	fd = openat(current, segments[count - 1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOATIME);
	if (fd < 0 && errno == EPERM)
		fd = openat(current, segments[count - 1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		gint saved = errno;

		close(current);
		open_failed(guard, "The file", saved, error);
		return FALSE;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != root_st.st_dev) {
		close(fd);
		close(current);
		if (!axis_volume_guard_check(guard, error))
			return FALSE;
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"The file is no longer a regular file on its root's filesystem.");
		return FALSE;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	file->fd = fd;
	file->dir_fd = current;
	file->name = g_strdup(segments[count - 1]);
	file->st = st;
	return TRUE;
}

gboolean
axis_probe_stamp_matches(
	const struct stat     *st,
	const AxisProbeTarget *target
){
	return S_ISREG(st->st_mode) &&
		(gint64)st->st_size == target->size &&
		(gint64)st->st_mtim.tv_sec == target->mtime_sec && (gint)st->st_mtim.tv_nsec == target->mtime_nsec &&
		(gint64)st->st_ctim.tv_sec == target->ctime_sec && (gint)st->st_ctim.tv_nsec == target->ctime_nsec &&
		(gint64)st->st_ino == target->inode;
}

gboolean
axis_probe_file_unchanged(
	const AxisProbeFile   *file,
	const AxisProbeTarget *target
){
	struct stat open_st;
	struct stat name_st;

	if (fstat(file->fd, &open_st) != 0 || !axis_probe_stamp_matches(&open_st, target))
		return FALSE;
	/* The name still leads to the file that was probed. */
	if (fstatat(file->dir_fd, file->name, &name_st, AT_SYMLINK_NOFOLLOW) != 0)
		return FALSE;
	return S_ISREG(name_st.st_mode) && name_st.st_ino == open_st.st_ino && name_st.st_dev == open_st.st_dev;
}
