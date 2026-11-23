/*
 * axis-library-kind.h - what a file is, from its name, and which names the
 * scanner leaves out
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Kinds come from the extension (the bytes after the last '.', compared
 * case-insensitively in ASCII); nothing is read from the file.  Names are
 * ignored by a built-in list (filesystem and NAS housekeeping, Axis's own
 * files, partial downloads) and by a root's own patterns.  Everything here
 * works on the name's raw bytes.
 *
 * GLib only, no I/O: the pattern matcher is fuzzed
 * (tests/fuzz/fuzz-library-name.c).
 */

#ifndef AXIS_LIBRARY_KIND_H
#define AXIS_LIBRARY_KIND_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
	AXIS_FILE_KIND_VIDEO,
	AXIS_FILE_KIND_AUDIO,
	AXIS_FILE_KIND_SUBTITLE,
	AXIS_FILE_KIND_IMAGE,
	AXIS_FILE_KIND_TEXT,
	AXIS_FILE_KIND_OTHER,
	AXIS_FILE_KIND_SYMLINK
} AxisFileKind;

#define AXIS_FILE_KIND_COUNT 7

/* "video", "audio", "subtitle", "image", "text", "other", "symlink" (as
 * stored) */
const gchar *
axis_file_kind_to_string(AxisFileKind kind);

gboolean
axis_file_kind_from_string(
	const gchar  *text,
	AxisFileKind *kind
);

/* "Video", ..., "Symbolic link" */
const gchar *
axis_file_kind_label(AxisFileKind kind);

/* The label of one file's kind: "Video", "Subtitle", "Image",
 * "Text or info file", "Other", "Symbolic link". */
const gchar *
axis_file_kind_label_one(AxisFileKind kind);

/* The kind of a regular file called @name (@length bytes). */
AxisFileKind
axis_file_kind_from_name(
	const gchar *name,
	gsize        length
);

/**
 * AxisIgnoreReason:
 * @AXIS_IGNORE_NONE: indexed
 * @AXIS_IGNORE_SYSTEM: housekeeping of a filesystem, a NAS or Axis itself
 *   (".axis-volume", "lost+found", "@eaDir", ".Trash-1000", ...)
 * @AXIS_IGNORE_PARTIAL: a download in progress (".part", ".!qB",
 *   "_UNPACK_..." and the like): counted and listed in the scan's result
 * @AXIS_IGNORE_USER: one of the root's own patterns
 */
typedef enum
{
	AXIS_IGNORE_NONE,
	AXIS_IGNORE_SYSTEM,
	AXIS_IGNORE_PARTIAL,
	AXIS_IGNORE_USER
} AxisIgnoreReason;

/* Whether the built-in list leaves out @name (a directory when @is_dir). */
AxisIgnoreReason
axis_library_ignore_builtin(
	const gchar *name,
	gsize        length,
	gboolean     is_dir
);

/* Root patterns: at most this many, each 1 to this many bytes. */
#define AXIS_LIBRARY_MAX_PATTERNS   64
#define AXIS_LIBRARY_PATTERN_MAX    255

/**
 * axis_library_pattern_check:
 * @pattern: (nullable): a pattern as typed
 * @problem: (out) (optional) (transfer none): why it is refused
 *
 * A pattern is valid UTF-8, 1 to %AXIS_LIBRARY_PATTERN_MAX bytes, without
 * '/', control characters or leading or trailing white space, and not
 * only '*'s (which would leave out everything).
 *
 * Returns: %TRUE if @pattern is acceptable
 */
gboolean
axis_library_pattern_check(
	const gchar  *pattern,
	const gchar **problem
);

/**
 * axis_library_glob_match:
 * @pattern: a pattern: '*' matches any run of characters (none included),
 *   '?' exactly one character (a valid UTF-8 sequence, or a single byte
 *   that is not part of one), '\' makes the next character literal, and
 *   anything else matches itself, byte for byte (case matters)
 * @name: the name's bytes
 * @length: how many
 *
 * Returns: %TRUE if the whole of @name matches
 */
gboolean
axis_library_glob_match(
	const gchar *pattern,
	const gchar *name,
	gsize        length
);

G_END_DECLS

#endif /* AXIS_LIBRARY_KIND_H */
