/*
 * axis-volume-marker.c - reading the marker file
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here works relative to a directory descriptor the caller has
 * verified, never through a path, so that the file read is in exactly that
 * directory even if something is mounted or renamed at its path meanwhile.
 * Making and writing a marker is in axis-volume-marker-write.c, which a
 * program that only reads markers (axis-worker) does not link.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-volume-marker.h"
#include "storage/axis-storage-fs.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

AxisVolumeMarkerStatus
axis_volume_marker_read(
	gint              dirfd,
	AxisVolumeMarker *marker,
	gint             *saved_errno
){
	guint8 buffer[AXIS_VOLUME_MARKER_MAX_BYTES + 1];
	gsize length = 0;
	struct stat st;
	gint fd;
	g_autoptr(GError) error = NULL;

	if (saved_errno != NULL)
		*saved_errno = 0;

	/* O_NOFOLLOW: a symbolic link is not a marker.  O_NONBLOCK: a FIFO
	 * given the marker's name must not hang the open.  O_NOATIME where it
	 * is allowed: read at every check, it is no reason to write. */
	fd = axis_storage_fs_openat_noatime(dirfd, AXIS_VOLUME_MARKER_NAME,
		O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		gint err = errno;

		if (err == ENOENT)
			return AXIS_VOLUME_MARKER_MISSING;
		if (err == ELOOP || err == ENXIO || err == EISDIR)
			return AXIS_VOLUME_MARKER_INVALID;
		if (saved_errno != NULL)
			*saved_errno = err;
		return AXIS_VOLUME_MARKER_UNREADABLE;
	}

	if (fstat(fd, &st) != 0) {
		gint err = errno;

		close(fd);
		if (saved_errno != NULL)
			*saved_errno = err;
		return AXIS_VOLUME_MARKER_UNREADABLE;
	}
	if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > AXIS_VOLUME_MARKER_MAX_BYTES) {
		close(fd);
		return AXIS_VOLUME_MARKER_INVALID;
	}

	/* One more byte than allowed, so that a file that grew after the
	 * fstat is still caught. */
	while (length < sizeof(buffer)) {
		ssize_t n = read(fd, buffer + length, sizeof(buffer) - length);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			gint err = errno;

			close(fd);
			if (saved_errno != NULL)
				*saved_errno = err;
			return AXIS_VOLUME_MARKER_UNREADABLE;
		}
		if (n == 0)
			break;
		length += (gsize)n;
	}
	close(fd);

	if (!axis_volume_marker_parse(buffer, length, marker, &error)) {
		if (saved_errno != NULL &&
		    g_error_matches(error, AXIS_VOLUME_MARKER_ERROR, AXIS_VOLUME_MARKER_ERROR_VERSION))
			*saved_errno = EPROTO;
		return AXIS_VOLUME_MARKER_INVALID;
	}

	return AXIS_VOLUME_MARKER_FOUND;
}
