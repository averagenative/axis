/*
 * axis-file-check.h - whether a file one machine opened is the file another
 * recorded: its stamp, and a digest of samples of its content
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The daemon records a file's stamp (size, modification time to the
 * nanosecond, inode, links) from its own view of the disk; a worker that
 * reaches the same disk through a mount compares what it sees with it before
 * it encodes.  A local filesystem or NFS shows the worker the server's own
 * inode numbers and times; sshfs and SMB/CIFS commonly do not (sshfs gives
 * made-up inode numbers and whole seconds, and every file one link; SMB
 * times are in tenths of a microsecond and its inode numbers are the
 * server's file index, not its inode).  So a comparison says what differs,
 * and a caller that was told its mount does not keep file identities
 * ignores the inode and the part of the time below a second.
 *
 * What stands in for the identity there is the content: the daemon reads
 * samples of the file (axis_file_sample_digest()) when it hands the work
 * out, and the worker reads the same samples of what it sees and requires
 * the same digest.  The samples are AXIS_FILE_SAMPLE_BLOCKS blocks of
 * AXIS_FILE_SAMPLE_BLOCK bytes: the first, the last, and the others spread
 * evenly between them (the whole file when it is no larger than all of
 * them), and the digest is SHA-256 over a fixed label, the size and the
 * blocks in order.  Both sides compute it with this one function.
 *
 * Descriptor-based, GLib only: linked by the daemon and by axis-worker.
 * Reading blocks on the filesystem; comparing does not.
 */

#ifndef AXIS_FILE_CHECK_H
#define AXIS_FILE_CHECK_H

#include <glib.h>
#include <sys/stat.h>

G_BEGIN_DECLS

#define AXIS_FILE_SAMPLE_BLOCK   (1024 * 1024)
#define AXIS_FILE_SAMPLE_BLOCKS  8

/* What the digest's input starts with: the scheme's name and version. */
#define AXIS_FILE_SAMPLE_LABEL   "axis-file-sample-1"

/**
 * AxisFileStamp:
 * @size: the size in bytes
 * @mtime_sec, @mtime_nsec: the modification time
 * @inode: the inode number, or -1 when it is not compared
 * @one_link: whether the file must have exactly one link
 */
typedef struct
{
	gint64   size;
	gint64   mtime_sec;
	gint     mtime_nsec;
	gint64   inode;
	gboolean one_link;
} AxisFileStamp;

/**
 * AxisFileStampResult:
 * @AXIS_FILE_STAMP_SAME: everything compared is equal
 * @AXIS_FILE_STAMP_SIZE: the size differs
 * @AXIS_FILE_STAMP_TIME: the modification time differs in its seconds
 * @AXIS_FILE_STAMP_LINKS: it has more than one link (as far as the mount
 *   says: a mount that says one link for every file cannot be caught here)
 * @AXIS_FILE_STAMP_SUBSECOND: the size and the time's seconds are equal,
 *   but the time below a second differs (only when identities are kept)
 * @AXIS_FILE_STAMP_INODE: the size and time are equal, but the inode
 *   differs (only when identities are kept)
 *
 * In this order of precedence: the first that applies is the answer.
 */
typedef enum
{
	AXIS_FILE_STAMP_SAME,
	AXIS_FILE_STAMP_SIZE,
	AXIS_FILE_STAMP_TIME,
	AXIS_FILE_STAMP_LINKS,
	AXIS_FILE_STAMP_SUBSECOND,
	AXIS_FILE_STAMP_INODE
} AxisFileStampResult;

/**
 * axis_file_stamp_compare:
 * @st: what fstat() said of the file here
 * @want: what it must be
 * @keeps_identity: the mount shows the server's inode numbers and times to
 *   the nanosecond: compare them.  %FALSE compares the size, the time's
 *   seconds and the links only.
 */
AxisFileStampResult
axis_file_stamp_compare(
	const struct stat   *st,
	const AxisFileStamp *want,
	gboolean             keeps_identity
);

/* A short phrase for a result ("its size differs"). */
const gchar *
axis_file_stamp_describe(AxisFileStampResult result);

/**
 * axis_file_sample_offset:
 * @size: the file's size
 * @k: which block, 0 to AXIS_FILE_SAMPLE_BLOCKS - 1
 * @length: (out): how many bytes it has
 *
 * Where block @k starts.  A file no larger than all the blocks together is
 * one "block" (k = 0, its whole length; the others are empty).
 */
gint64
axis_file_sample_offset(
	gint64  size,
	guint   k,
	gint64 *length
);

/**
 * axis_file_sample_digest:
 * @fd: the file, open for reading (its offset is not moved: pread())
 * @size: the size it must have (the stamp's)
 * @hex: (out) (transfer full): 64 lower-case hexadecimal digits
 * @err: (out) (optional): errno of a failure; EIO for a file shorter than
 *   @size
 *
 * Returns: FALSE when the samples cannot all be read
 */
gboolean
axis_file_sample_digest(
	gint    fd,
	gint64  size,
	gchar **hex,
	gint   *err
);

/**
 * axis_file_whole_digest:
 * @fd: the file, open for reading (its offset is not moved: pread(), so a
 *   tool reading the same descriptor at the same time is not disturbed)
 * @size: the size it must have: it is read to there and must end there
 * @stop: (nullable): read atomically between blocks; when it is not 0 the
 *   reading stops and fails with ECANCELED
 * @hex: (out) (transfer full): the SHA-256 of its bytes, plain (what
 *   sha256sum says of the file), 64 lower-case hexadecimal digits
 * @err: (out) (optional): errno of a failure; EIO for a file shorter or
 *   longer than @size
 *
 * The digest that binds a worker's measurements of a converted file to the
 * file the daemon has: the worker computes it while it reads the file for
 * its checks, the daemon on its own disk.
 *
 * Returns: FALSE when the file cannot be read whole
 */
gboolean
axis_file_whole_digest(
	gint           fd,
	gint64         size,
	const gint    *stop,
	gchar        **hex,
	gint          *err
);

G_END_DECLS

#endif /* AXIS_FILE_CHECK_H */
