/*
 * axis-volume-marker-write.c - making a new marker and writing it
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Kept apart from reading (axis-volume-marker.c) so that a program which
 * only proves where a volume is, such as axis-worker, links no code that
 * can write on a volume.  Like reading, it works relative to a directory
 * descriptor the caller has verified, never through a path.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-volume-marker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

/* @length bytes from the kernel's random source. */
static gboolean
random_bytes(
	guint8 *out,
	gsize   length
){
	gsize got = 0;

	while (got < length) {
		ssize_t n = getrandom(out + got, length - got, 0);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return FALSE;
		got += (gsize)n;
	}

	return TRUE;
}

gboolean
axis_volume_marker_new(
	AxisVolumeMarker  *marker,
	GError           **error
){
	guint8 bytes[16];
	gint64 now;

	g_return_val_if_fail(marker != NULL, FALSE);

	if (!random_bytes(bytes, sizeof(bytes))) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"the kernel gave no random bytes for a volume identifier");
		return FALSE;
	}

	/* RFC 9562 version 4, variant 10. */
	bytes[6] = (guint8)((bytes[6] & 0x0f) | 0x40);
	bytes[8] = (guint8)((bytes[8] & 0x3f) | 0x80);
	g_snprintf(marker->uuid, sizeof(marker->uuid),
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
		bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

	now = g_get_real_time() / G_USEC_PER_SEC;
	marker->created = CLAMP(now, 0, AXIS_VOLUME_MARKER_MAX_TIME);

	return TRUE;
}

static gboolean
write_all(
	gint         fd,
	const gchar *data,
	gsize        length
){
	gsize done = 0;

	while (done < length) {
		ssize_t n = write(fd, data + done, length - done);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return FALSE;
		done += (gsize)n;
	}

	return TRUE;
}

static gboolean
fail_errno(
	GError      **error,
	gint          err,
	const gchar  *what
){
	g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "%s: %s", what, g_strerror(err));
	return FALSE;
}

/* The temporary names this process's marker writes made in its lifetime: a
 * write of its own may be in progress (or a failure may have left one), and
 * none is ever taken for another's leftover (axis_volume_marker_remove_stale()).
 * A few names per volume and lifetime. */
static GMutex      made_lock;
static GHashTable *made;	/* (made_lock) gchar* names */
static gchar      *made_last;	/* (made_lock) for tests */

static void
note_made(const gchar *name)
{
	g_mutex_lock(&made_lock);
	if (made == NULL)
		made = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_hash_table_add(made, g_strdup(name));
	g_free(made_last);
	made_last = g_strdup(name);
	g_mutex_unlock(&made_lock);
}

static gboolean
made_here(const gchar *name)
{
	gboolean found;

	g_mutex_lock(&made_lock);
	found = made != NULL && g_hash_table_contains(made, name);
	g_mutex_unlock(&made_lock);
	return found;
}

gchar *
axis_volume_marker_test_last_temporary(void)
{
	gchar *name;

	g_mutex_lock(&made_lock);
	name = g_strdup(made_last);
	g_mutex_unlock(&made_lock);
	return name;
}

/* Gives @temporary the marker's name without replacing anything.  Sets
 * errno and returns FALSE on failure (EEXIST when a marker is there). */
static gboolean
install(
	gint         dirfd,
	const gchar *temporary
){
	gint err;

	if (renameat2(dirfd, temporary, dirfd, AXIS_VOLUME_MARKER_NAME, RENAME_NOREPLACE) == 0)
		return TRUE;

	err = errno;
	if (err != EINVAL && err != ENOSYS && err != EOPNOTSUPP)
		return FALSE;

	/* A filesystem that cannot rename without replacing: a hard link
	 * fails the same way when the name exists. */
	if (linkat(dirfd, temporary, dirfd, AXIS_VOLUME_MARKER_NAME, 0) != 0)
		return FALSE;
	if (unlinkat(dirfd, temporary, 0) != 0)
		g_warning("could not remove the temporary marker file %s: %s", temporary,
			g_strerror(errno));

	return TRUE;
}

gboolean
axis_volume_marker_write(
	gint                     dirfd,
	const AxisVolumeMarker  *marker,
	GError                 **error
){
	gchar content[AXIS_VOLUME_MARKER_MAX_BYTES];
	gchar temporary[sizeof(AXIS_VOLUME_MARKER_NAME) + 32];
	guint8 suffix[8];
	gsize length;
	gint fd;
	gint err;

	g_return_val_if_fail(dirfd >= 0, FALSE);
	g_return_val_if_fail(marker != NULL, FALSE);

	length = axis_volume_marker_format(marker, content);
	if (length == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"the marker is not valid");
		return FALSE;
	}

	if (!random_bytes(suffix, sizeof(suffix))) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"the kernel gave no random bytes for a temporary file name");
		return FALSE;
	}
	g_snprintf(temporary, sizeof(temporary), "%s.tmp-%02x%02x%02x%02x%02x%02x%02x%02x",
		AXIS_VOLUME_MARKER_NAME, suffix[0], suffix[1], suffix[2], suffix[3], suffix[4],
		suffix[5], suffix[6], suffix[7]);
	/* Before it exists: never taken by this process for another's leftover. */
	note_made(temporary);

	fd = openat(dirfd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NOCTTY | O_CLOEXEC,
		0644);
	if (fd < 0)
		return fail_errno(error, errno, "cannot create the marker");

	if (!write_all(fd, content, length) || fsync(fd) != 0) {
		err = errno != 0 ? errno : EIO;
		close(fd);
		unlinkat(dirfd, temporary, 0);
		return fail_errno(error, err, "cannot write the marker");
	}
	if (close(fd) != 0) {
		err = errno;
		unlinkat(dirfd, temporary, 0);
		return fail_errno(error, err, "cannot write the marker");
	}

	if (!install(dirfd, temporary)) {
		err = errno;
		unlinkat(dirfd, temporary, 0);
		if (err == EEXIST) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
				"a marker appeared in the directory meanwhile; nothing was replaced");
			return FALSE;
		}
		return fail_errno(error, err, "cannot put the marker in place");
	}

	/* The name itself must survive a crash too. */
	if (fsync(dirfd) != 0)
		return fail_errno(error, errno, "cannot flush the directory after writing the marker");

	return TRUE;
}

#define TEMPORARY_PREFIX AXIS_VOLUME_MARKER_NAME ".tmp-"
#define STALE_MAX        64

/* Exactly a name axis_volume_marker_write() makes. */
static gboolean
is_temporary(const gchar *name)
{
	gsize prefix = strlen(TEMPORARY_PREFIX);
	gsize i;

	if (!g_str_has_prefix(name, TEMPORARY_PREFIX) || strlen(name) != prefix + 16)
		return FALSE;
	for (i = prefix; i < prefix + 16; i++) {
		if (!g_ascii_isdigit(name[i]) && (name[i] < 'a' || name[i] > 'f'))
			return FALSE;
	}
	return TRUE;
}

GHashTable *
axis_volume_marker_seen_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

guint
axis_volume_marker_remove_stale(
	gint         dirfd,
	GHashTable  *seen,
	gint64       now_us,
	gint64       age_us,
	gint        *saved_errno
){
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GHashTable) still = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	struct dirent *entry;
	DIR *dir;
	guint removed = 0;
	guint i;
	gint fd;

	if (saved_errno != NULL)
		*saved_errno = 0;
	g_return_val_if_fail(dirfd >= 0, 0);
	g_return_val_if_fail(seen != NULL, 0);

	/* A reading of its own, so that the caller's descriptor keeps its
	 * place; the names first, then the removals. */
	fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_NOCTTY | O_CLOEXEC);
	if (fd < 0 || (dir = fdopendir(fd)) == NULL) {
		if (saved_errno != NULL)
			*saved_errno = errno;
		if (fd >= 0)
			close(fd);
		return 0;
	}
	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			if (errno != 0 && saved_errno != NULL)
				*saved_errno = errno;
			break;
		}
		if (is_temporary(entry->d_name) && names->len < STALE_MAX)
			g_ptr_array_add(names, g_strdup(entry->d_name));
	}
	closedir(dir);

	/* No clock is compared with a file's time (a clock stepped, a network
	 * server's clock behind): a name goes only once two looks of this
	 * process, @age_us apart by the monotonic clock, found the very same
	 * file (its inode, size and modification time), and never one this
	 * process's own writes made. */
	for (i = 0; i < names->len; i++) {
		const gchar *name = g_ptr_array_index(names, i);
		AxisVolumeMarkerSeen *before = g_hash_table_lookup(seen, name);
		AxisVolumeMarkerSeen *now;
		struct stat st;

		if (made_here(name) || fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode) ||
		    st.st_size > AXIS_VOLUME_MARKER_MAX_BYTES)
			continue;
		now = g_new0(AxisVolumeMarkerSeen, 1);
		now->ino = (guint64)st.st_ino;
		now->size = (guint64)st.st_size;
		now->mtime_ns = (gint64)st.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) + st.st_mtim.tv_nsec;
		now->seen_us = now_us;
		if (before != NULL && before->ino == now->ino && before->size == now->size &&
		    before->mtime_ns == now->mtime_ns) {
			if (now_us - before->seen_us >= age_us) {
				g_free(now);
				if (unlinkat(dirfd, name, 0) == 0)
					removed++;
				continue;
			}
			/* Unchanged, not yet for long enough: since the first look. */
			now->seen_us = before->seen_us;
		}
		g_hash_table_replace(still, g_strdup(name), now);
	}
	/* What was seen and is not there any more, or was removed, is
	 * forgotten. */
	g_hash_table_remove_all(seen);
	{
		GHashTableIter iter;
		gpointer key;
		gpointer value;

		g_hash_table_iter_init(&iter, still);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			g_hash_table_iter_steal(&iter);
			g_hash_table_replace(seen, key, value);
		}
	}
	if (removed > 0 && fsync(dirfd) != 0 && saved_errno != NULL)
		*saved_errno = errno;
	return removed;
}
