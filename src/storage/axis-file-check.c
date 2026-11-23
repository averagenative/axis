/*
 * axis-file-check.c - a file's stamp, and a digest of samples of its content
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-file-check.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

AxisFileStampResult
axis_file_stamp_compare(
	const struct stat   *st,
	const AxisFileStamp *want,
	gboolean             keeps_identity
){
	g_return_val_if_fail(st != NULL && want != NULL, AXIS_FILE_STAMP_SIZE);

	if ((gint64)st->st_size != want->size)
		return AXIS_FILE_STAMP_SIZE;
	if ((gint64)st->st_mtim.tv_sec != want->mtime_sec)
		return AXIS_FILE_STAMP_TIME;
	if (want->one_link && st->st_nlink != 1)
		return AXIS_FILE_STAMP_LINKS;
	if (!keeps_identity)
		return AXIS_FILE_STAMP_SAME;
	if ((gint)st->st_mtim.tv_nsec != want->mtime_nsec)
		return AXIS_FILE_STAMP_SUBSECOND;
	if (want->inode >= 0 && (gint64)st->st_ino != want->inode)
		return AXIS_FILE_STAMP_INODE;
	return AXIS_FILE_STAMP_SAME;
}

const gchar *
axis_file_stamp_describe(AxisFileStampResult result)
{
	switch (result) {
	case AXIS_FILE_STAMP_SAME:
		return "it is the same";
	case AXIS_FILE_STAMP_SIZE:
		return "its size differs";
	case AXIS_FILE_STAMP_TIME:
		return "its modification time differs";
	case AXIS_FILE_STAMP_LINKS:
		return "it has another hard link";
	case AXIS_FILE_STAMP_SUBSECOND:
		return "its modification time differs below a second";
	case AXIS_FILE_STAMP_INODE:
		return "its inode number differs";
	default:
		return "it differs";
	}
}

gint64
axis_file_sample_offset(
	gint64  size,
	guint   k,
	gint64 *length
){
	const gint64 block = AXIS_FILE_SAMPLE_BLOCK;
	const gint64 all = (gint64)AXIS_FILE_SAMPLE_BLOCKS * block;

	*length = 0;
	if (size <= 0 || k >= AXIS_FILE_SAMPLE_BLOCKS)
		return 0;
	if (size <= all) {
		if (k == 0)
			*length = size;
		return 0;
	}
	/* The first block at 0, the last ending at the end, the others evenly
	 * between: (size - block) / (blocks - 1) apart, which is more than a
	 * block, so none overlaps the next.  size is below 2^53 (the stamp's
	 * bound), so the product fits. */
	*length = block;
	return (size - block) * (gint64)k / (AXIS_FILE_SAMPLE_BLOCKS - 1);
}

gboolean
axis_file_sample_digest(
	gint    fd,
	gint64  size,
	gchar **hex,
	gint   *err
){
	g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
	g_autofree guchar *buffer = NULL;
	g_autofree gchar *head = NULL;
	guint k;

	*hex = NULL;
	if (err != NULL)
		*err = 0;
	if (fd < 0 || size < 0) {
		if (err != NULL)
			*err = EINVAL;
		return FALSE;
	}
	buffer = g_malloc(AXIS_FILE_SAMPLE_BLOCK);
	head = g_strdup_printf(AXIS_FILE_SAMPLE_LABEL "\n%" G_GINT64_FORMAT "\n", size);
	g_checksum_update(sum, (const guchar *)head, (gssize)strlen(head));
	for (k = 0; k < AXIS_FILE_SAMPLE_BLOCKS; k++) {
		gint64 length = 0;
		gint64 offset = axis_file_sample_offset(size, k, &length);
		gint64 done = 0;

		while (done < length) {
			gsize want = (gsize)MIN(length - done, (gint64)AXIS_FILE_SAMPLE_BLOCK);
			gssize n = pread(fd, buffer, want, (off_t)(offset + done));

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				/* Shorter than its stamp says: not that file. */
				if (err != NULL)
					*err = n < 0 ? errno : EIO;
				return FALSE;
			}
			g_checksum_update(sum, buffer, n);
			done += n;
		}
	}
	*hex = g_strdup(g_checksum_get_string(sum));
	return TRUE;
}

#define WHOLE_BLOCK (4 * 1024 * 1024)

gboolean
axis_file_whole_digest(
	gint           fd,
	gint64         size,
	const gint    *stop,
	gchar        **hex,
	gint          *err
){
	g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
	g_autofree guchar *buffer = NULL;
	gint64 done = 0;
	guchar probe;

	*hex = NULL;
	if (err != NULL)
		*err = 0;
	if (fd < 0 || size < 0) {
		if (err != NULL)
			*err = EINVAL;
		return FALSE;
	}
	buffer = g_malloc(WHOLE_BLOCK);
	while (done < size) {
		gsize want = (gsize)MIN(size - done, (gint64)WHOLE_BLOCK);
		gssize n;

		if (stop != NULL && g_atomic_int_get(stop) != 0) {
			if (err != NULL)
				*err = ECANCELED;
			return FALSE;
		}
		n = pread(fd, buffer, want, (off_t)done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			/* Shorter than it must be: not that file. */
			if (err != NULL)
				*err = n < 0 ? errno : EIO;
			return FALSE;
		}
		g_checksum_update(sum, buffer, n);
		done += n;
	}
	/* And not longer either. */
	for (;;) {
		gssize n = pread(fd, &probe, 1, (off_t)size);

		if (n < 0 && errno == EINTR)
			continue;
		if (n != 0) {
			if (err != NULL)
				*err = n < 0 ? errno : EIO;
			return FALSE;
		}
		break;
	}
	*hex = g_strdup(g_checksum_get_string(sum));
	return TRUE;
}
