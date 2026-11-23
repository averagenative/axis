/*
 * axis-storage-open.c - opening one file beneath a directory descriptor by
 * the path the index stores
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-storage-open.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "library/axis-library-name.h"
#include "storage/axis-storage-fs.h"
#include "storage/axis-storage-path.h"

static void
set_problem(
	const gchar **problem,
	const gchar  *text
){
	if (problem != NULL)
		*problem = text;
}

/* The decoded bytes of @stored, split into parts, each checked. */
static gchar **
file_parts(
	const gchar  *stored,
	const gchar **problem
){
	g_autoptr(GString) bytes = NULL;
	gchar **parts;
	guint n;
	guint i;

	if (stored == NULL || stored[0] == '\0') {
		set_problem(problem, "the file's path is empty");
		return NULL;
	}
	if (strlen(stored) > 4 * AXIS_LIBRARY_PATH_MAX) {
		set_problem(problem, "the file's path is too long");
		return NULL;
	}
	if (!axis_library_name_decode(stored, &bytes)) {
		set_problem(problem, "the file's path is not in its stored form");
		return NULL;
	}
	if (bytes->len == 0 || bytes->len > AXIS_LIBRARY_PATH_MAX) {
		set_problem(problem, "the file's path is empty or too long");
		return NULL;
	}
	if (bytes->str[0] == '/') {
		set_problem(problem, "the file's path is absolute");
		return NULL;
	}
	/* A NUL among the bytes would end a part early. */
	if (memchr(bytes->str, '\0', bytes->len) != NULL) {
		set_problem(problem, "the file's path holds a zero byte");
		return NULL;
	}
	parts = g_strsplit(bytes->str, "/", -1);
	n = g_strv_length(parts);
	if (n == 0 || n > AXIS_LIBRARY_DEPTH_MAX) {
		g_strfreev(parts);
		set_problem(problem, "the file's path is too deep");
		return NULL;
	}
	for (i = 0; i < n; i++) {
		gsize length = strlen(parts[i]);

		if (length == 0 || strcmp(parts[i], ".") == 0 || strcmp(parts[i], "..") == 0) {
			g_strfreev(parts);
			set_problem(problem, "the file's path has an empty, \".\" or \"..\" part");
			return NULL;
		}
		if (length > AXIS_LIBRARY_NAME_MAX) {
			g_strfreev(parts);
			set_problem(problem, "a part of the file's path is too long");
			return NULL;
		}
	}
	return parts;
}

gboolean
axis_storage_file_path_valid(
	const gchar  *root,
	const gchar  *stored,
	const gchar **problem
){
	g_autofree gchar *normalised = NULL;
	const gchar *why = NULL;
	gchar **parts;

	set_problem(problem, NULL);
	if (root == NULL) {
		set_problem(problem, "the root's directory is missing");
		return FALSE;
	}
	if (strlen(root) > AXIS_STORAGE_PATH_MAX || !axis_storage_path_normalize(root, FALSE, &normalised, &why)) {
		set_problem(problem, "the root's directory is not a relative path in its stored form");
		return FALSE;
	}
	/* Only the one form: what normalising gives back unchanged. */
	if (strcmp(normalised, root) != 0) {
		set_problem(problem, "the root's directory is not in its normalised form");
		return FALSE;
	}
	parts = file_parts(stored, problem);
	if (parts == NULL)
		return FALSE;
	g_strfreev(parts);
	return TRUE;
}

static AxisStorageOpenStatus
status_of(gint err)
{
	switch (err) {
	case ENOENT:
		return AXIS_STORAGE_OPEN_MISSING;
	case ELOOP:
	case EXDEV:
	case ENOTDIR:
	case EISDIR:
	case ENXIO:
		return AXIS_STORAGE_OPEN_NOT_CONTAINED;
	case EINVAL:
	case ENAMETOOLONG:
		return AXIS_STORAGE_OPEN_INVALID;
	default:
		return AXIS_STORAGE_OPEN_FAILED;
	}
}

/* Whether @fd is on the same filesystem and mount as @top. */
static gboolean
same_mount(
	gint                         fd,
	const AxisStorageFsIdentity *top,
	gint                        *err
){
	AxisStorageFsIdentity here;

	if (!axis_storage_fs_identify(fd, &here, err))
		return FALSE;
	if (here.dev != top->dev || (here.has_mnt_id && top->has_mnt_id && here.mnt_id != top->mnt_id)) {
		*err = EXDEV;
		return FALSE;
	}
	return TRUE;
}

AxisStorageOpenStatus
axis_storage_open_file_beneath(
	gint          dirfd,
	const gchar  *root,
	const gchar  *stored,
	gint         *fd_out,
	struct stat  *st_out,
	gint         *saved_errno
){
	AxisStorageFsIdentity top;
	struct stat st;
	gchar **parts;
	guint n;
	guint i;
	gint current;
	gint fd;
	gint err = 0;
	gint flags;

	*fd_out = -1;
	if (saved_errno != NULL)
		*saved_errno = 0;
	if (dirfd < 0 || !axis_storage_file_path_valid(root, stored, NULL))
		return AXIS_STORAGE_OPEN_INVALID;
	if (!axis_storage_fs_identify(dirfd, &top, &err)) {
		if (saved_errno != NULL)
			*saved_errno = err;
		return AXIS_STORAGE_OPEN_FAILED;
	}

	/* The root: no link, no other filesystem on the way. */
	current = axis_storage_fs_open_beneath(dirfd, root, &err);
	if (current < 0) {
		if (saved_errno != NULL)
			*saved_errno = err;
		return status_of(err);
	}

	parts = file_parts(stored, NULL);
	if (parts == NULL) {
		close(current);
		return AXIS_STORAGE_OPEN_INVALID;
	}
	n = g_strv_length(parts);
	for (i = 0; i + 1 < n; i++) {
		gint next = openat(current, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);

		if (next < 0) {
			err = errno;
			if (err == ENOTDIR || err == ELOOP) {
				struct stat link_st;

				if (fstatat(current, parts[i], &link_st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(link_st.st_mode))
					err = ELOOP;
			}
			close(current);
			g_strfreev(parts);
			if (saved_errno != NULL)
				*saved_errno = err;
			return status_of(err);
		}
		close(current);
		current = next;
		if (!same_mount(current, &top, &err)) {
			close(current);
			g_strfreev(parts);
			if (saved_errno != NULL)
				*saved_errno = err;
			return status_of(err);
		}
	}

	/* No blocking on a FIFO put where the file was, no access time
	 * written where the kernel allows it. */
	fd = openat(current, parts[n - 1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK | O_NOATIME);
	if (fd < 0 && errno == EPERM)
		fd = openat(current, parts[n - 1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	err = errno;
	close(current);
	g_strfreev(parts);
	if (fd < 0) {
		if (saved_errno != NULL)
			*saved_errno = err;
		return status_of(err);
	}
	if (fstat(fd, &st) != 0) {
		err = errno;
		close(fd);
		if (saved_errno != NULL)
			*saved_errno = err;
		return AXIS_STORAGE_OPEN_FAILED;
	}
	if (!S_ISREG(st.st_mode) || (guint64)st.st_dev != top.dev) {
		close(fd);
		if (saved_errno != NULL)
			*saved_errno = S_ISREG(st.st_mode) ? EXDEV : EISDIR;
		return AXIS_STORAGE_OPEN_NOT_CONTAINED;
	}
	flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

	*fd_out = fd;
	if (st_out != NULL)
		*st_out = st;
	return AXIS_STORAGE_OPEN_OK;
}

const gchar *
axis_storage_open_status_describe(AxisStorageOpenStatus status)
{
	switch (status) {
	case AXIS_STORAGE_OPEN_OK:
		return "opened";
	case AXIS_STORAGE_OPEN_INVALID:
		return "the path is not in the accepted form";
	case AXIS_STORAGE_OPEN_NOT_CONTAINED:
		return "a symbolic link, another filesystem or something other than a regular file is on the way";
	case AXIS_STORAGE_OPEN_MISSING:
		return "the file is not there";
	default:
		return "the file could not be opened";
	}
}
