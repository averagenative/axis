/*
 * axis-volume-marker.h - the file that identifies a volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Adding a volume writes ".axis-volume" at the top of its directory:
 *
 *   axis-volume 1
 *   uuid 3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0b
 *   created 2026-11-12T10:15:30Z
 *   note Axis recognises this disk by this file. Do not edit, delete or copy it.
 *
 * Four lines, each ended by a line feed, and nothing else.  The file has
 * exactly one form: lower-case UUID, UTC time to the second with a 'Z',
 * the note word for word.  The parser accepts that form and nothing else,
 * so a marker that was edited, truncated, padded or written by something
 * other than Axis reads as invalid, which makes the volume a mismatch that
 * a person has to look at.  Whatever is on a disk is outside input: the
 * parser reads from a fixed buffer, never allocates, and has a fuzz target
 * (tests/fuzz/fuzz-volume-marker.c).
 *
 * The parsing and formatting functions depend on GLib only
 * (axis-volume-marker-parse.c); reading and writing the file are in
 * axis-volume-marker.c and work through a directory descriptor, never a
 * path, so they act on exactly the directory the caller verified.
 */

#ifndef AXIS_VOLUME_MARKER_H
#define AXIS_VOLUME_MARKER_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_VOLUME_MARKER_NAME      ".axis-volume"
#define AXIS_VOLUME_MARKER_NOTE      "Axis recognises this disk by this file. Do not edit, delete or copy it."

/* The canonical marker is shorter; anything longer is refused unread. */
#define AXIS_VOLUME_MARKER_MAX_BYTES 512

#define AXIS_VOLUME_UUID_LEN         36

/* 9999-12-31T23:59:59Z, the last time the format can hold. */
#define AXIS_VOLUME_MARKER_MAX_TIME  G_GINT64_CONSTANT(253402300799)

/**
 * AxisVolumeMarker:
 * @uuid: the volume's identifier: 36 characters, lower-case hexadecimal
 *   in the 8-4-4-4-12 layout, not all zeros; NUL-terminated
 * @created: when the marker was written, in seconds since 1970 (UTC), 0 to
 *   %AXIS_VOLUME_MARKER_MAX_TIME
 */
typedef struct
{
	gchar  uuid[AXIS_VOLUME_UUID_LEN + 1];
	gint64 created;
} AxisVolumeMarker;

#define AXIS_VOLUME_MARKER_ERROR (axis_volume_marker_error_quark())

/**
 * AxisVolumeMarkerError:
 * @AXIS_VOLUME_MARKER_ERROR_TOO_LARGE: longer than
 *   %AXIS_VOLUME_MARKER_MAX_BYTES
 * @AXIS_VOLUME_MARKER_ERROR_VERSION: a marker of a later format version
 * @AXIS_VOLUME_MARKER_ERROR_INVALID: not a marker in the one accepted form
 */
typedef enum
{
	AXIS_VOLUME_MARKER_ERROR_TOO_LARGE,
	AXIS_VOLUME_MARKER_ERROR_VERSION,
	AXIS_VOLUME_MARKER_ERROR_INVALID
} AxisVolumeMarkerError;

GQuark
axis_volume_marker_error_quark(void);

/* Returns: %TRUE if @text is a well-formed volume UUID (see AxisVolumeMarker) */
gboolean
axis_volume_uuid_is_valid(const gchar *text);

/**
 * axis_volume_marker_parse:
 * @data: (array length=length): the file's bytes
 * @length: how many
 * @marker: (out caller-allocates): the result; untouched on failure
 * @error: (nullable): return location for an error
 *
 * Accepts exactly the canonical form described above.  Uses no more than
 * a fixed amount of memory and time whatever @length is.
 *
 * Returns: %TRUE if @data is a valid marker
 */
gboolean
axis_volume_marker_parse(
	const guint8      *data,
	gsize              length,
	AxisVolumeMarker  *marker,
	GError           **error
);

/**
 * axis_volume_marker_format:
 * @marker: a marker whose fields are valid
 * @buffer: where the file's bytes go, %AXIS_VOLUME_MARKER_MAX_BYTES long
 *
 * Writes the canonical form, which axis_volume_marker_parse() reads back
 * as the same marker.  No terminating NUL is counted.
 *
 * Returns: the number of bytes, or 0 (and a critical) when a field of
 *   @marker is not valid
 */
gsize
axis_volume_marker_format(
	const AxisVolumeMarker *marker,
	gchar                  *buffer
);

/* ── The file (axis-volume-marker.c) ─────────────────────────────── */

/**
 * AxisVolumeMarkerStatus:
 * @AXIS_VOLUME_MARKER_FOUND: a valid marker was read
 * @AXIS_VOLUME_MARKER_MISSING: there is no file of that name
 * @AXIS_VOLUME_MARKER_INVALID: there is something of that name, but not a
 *   valid marker: a symbolic link, a directory or other special file, too
 *   large, malformed, or of a later version
 * @AXIS_VOLUME_MARKER_UNREADABLE: it could not be read (an I/O or
 *   permission error); the errno is returned
 */
typedef enum
{
	AXIS_VOLUME_MARKER_FOUND,
	AXIS_VOLUME_MARKER_MISSING,
	AXIS_VOLUME_MARKER_INVALID,
	AXIS_VOLUME_MARKER_UNREADABLE
} AxisVolumeMarkerStatus;

/**
 * axis_volume_marker_read:
 * @dirfd: the volume's top directory
 * @marker: (out caller-allocates): the marker, when found
 * @saved_errno: (out) (optional): the errno of an unreadable marker, or
 *   of a version error (EPROTO); 0 otherwise
 *
 * Opens the marker relative to @dirfd without following a symbolic link
 * and without blocking on a FIFO, refuses anything that is not a regular
 * file of at most %AXIS_VOLUME_MARKER_MAX_BYTES, and parses it.  Blocks
 * on the filesystem: call it from a helper thread with a deadline.
 *
 * Returns: what was found
 */
AxisVolumeMarkerStatus
axis_volume_marker_read(
	gint              dirfd,
	AxisVolumeMarker *marker,
	gint             *saved_errno
);

/**
 * axis_volume_marker_new:
 * @marker: (out caller-allocates): a new marker
 * @error: (nullable): return location for an error
 *
 * A fresh marker: a random (version 4) UUID from the kernel's random
 * source, created now.  Fails only when the kernel gives no random bytes.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_volume_marker_new(
	AxisVolumeMarker  *marker,
	GError           **error
);

/**
 * axis_volume_marker_write:
 * @dirfd: the volume's top directory, already verified by the caller
 * @marker: the marker
 * @error: (nullable): return location for an error
 *
 * Writes the marker without ever replacing a file: to a new temporary
 * file in @dirfd (exclusive create), flushed with fsync, then given its
 * name with a rename that fails if the name exists (or a hard link where
 * the filesystem cannot rename that way), then the directory is flushed
 * too.  Fails with %G_IO_ERROR_EXISTS, removing its temporary file, when a
 * marker appeared meanwhile, and with another #GIOError when writing
 * fails.  The only file it ever removes is its own temporary one (but see
 * axis_volume_marker_remove_stale()).  Blocks on the filesystem.
 *
 * Returns: %TRUE when the marker is in place and flushed
 */
gboolean
axis_volume_marker_write(
	gint                     dirfd,
	const AxisVolumeMarker  *marker,
	GError                 **error
);

/* A temporary marker file found the same at two looks this far apart is one
 * a write that will never finish left (a crash, a power cut): a write takes
 * seconds at most (AXIS_STORAGE_WRITE_TIMEOUT_MS). */
#define AXIS_VOLUME_MARKER_STALE_SECONDS 600

/**
 * AxisVolumeMarkerSeen:
 * @ino: a temporary marker file's inode at a look
 * @size: its size
 * @mtime_ns: its modification time, in nanoseconds since the epoch
 * @seen_us: since when it is so (the monotonic clock)
 *
 * What axis_volume_marker_remove_stale() found of one name, for its next
 * look.
 */
typedef struct
{
	guint64 ino;
	guint64 size;
	gint64  mtime_ns;
	gint64  seen_us;
} AxisVolumeMarkerSeen;

/* A table for axis_volume_marker_remove_stale(): name -> AxisVolumeMarkerSeen. */
GHashTable *
axis_volume_marker_seen_new(void);

/**
 * axis_volume_marker_remove_stale:
 * @dirfd: the volume's top directory, already verified by the caller
 * @seen: what the earlier looks at this directory found (from
 *   axis_volume_marker_seen_new()), updated
 * @now_us: the monotonic clock now (g_get_monotonic_time())
 * @age_us: how long, at least, a name must have been the same file
 * @saved_errno: (out) (optional): the errno when the directory could not be
 *   read, else 0
 *
 * Removes what an interrupted axis_volume_marker_write() left in @dirfd:
 * names of exactly the form it makes (the marker's name, ".tmp-" and 16
 * lower-case hexadecimal digits), only regular files no bigger than a
 * marker (never a link, never followed), never one this process's own
 * writes made, and only one found the very same file (inode, size,
 * modification time) at an earlier look @age_us or more before @now_us.
 * No file's time is compared with a clock: a clock stepped, or a server's
 * clock behind, cannot make a write in progress look old.  At most 64 at a
 * time.  Blocks on the filesystem.
 *
 * Returns: how many were removed
 */
guint
axis_volume_marker_remove_stale(
	gint         dirfd,
	GHashTable  *seen,
	gint64       now_us,
	gint64       age_us,
	gint        *saved_errno
);

/* Tests: the temporary name of this process's latest marker write. */
gchar *
axis_volume_marker_test_last_temporary(void);

G_END_DECLS

#endif /* AXIS_VOLUME_MARKER_H */
