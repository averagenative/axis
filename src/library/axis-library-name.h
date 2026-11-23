/*
 * axis-library-name.h - file names are bytes: storing, comparing and
 * showing them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A file name on Linux is any sequence of bytes but '/' and NUL.  The
 * database stores text, which must be valid UTF-8, so a name is stored in
 * one canonical form:
 *
 *   - bytes that are valid UTF-8 (RFC 3629: no overlong forms, no
 *     surrogates, nothing above U+10FFFF, which is exactly what PostgreSQL
 *     accepts) and contain no backslash are stored as they are;
 *   - otherwise every backslash becomes "\\" and every byte that does not
 *     belong to a valid UTF-8 sequence becomes "\xHH" (two upper-case hex
 *     digits); everything else is copied.
 *
 * The two cases cannot collide (only the second contains a backslash) and
 * the escape is undone byte for byte, so stored forms are equal exactly
 * when the names are: lookups and comparisons always use the stored form,
 * byte-wise.  Nothing is ever normalised (NFC and NFD spellings, or names
 * that differ only by case, stay different), and a stored path is the
 * stored names of its segments joined by '/', so a directory's stored path
 * is a prefix of its children's.
 *
 * What is shown is never the stored form: axis_library_name_display() for
 * pages and log lines (no control or direction-changing characters, valid
 * UTF-8), axis_library_name_exact() where the exact bytes matter.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-library-name.c).
 */

#ifndef AXIS_LIBRARY_NAME_H
#define AXIS_LIBRARY_NAME_H

#include <glib.h>

G_BEGIN_DECLS

/* A path relative to its root: at most this many bytes as on disk, and
 * this many directories deep (the root's own entries are depth 1). */
#define AXIS_LIBRARY_PATH_MAX  4096
#define AXIS_LIBRARY_DEPTH_MAX 128

/* The longest name a filesystem gives (NAME_MAX). */
#define AXIS_LIBRARY_NAME_MAX  255

/**
 * axis_library_utf8_sequence:
 * @p: bytes
 * @left: how many there are
 *
 * Returns: the length (1 to 4) of the valid UTF-8 sequence that starts at
 *   @p, or 0 when none does
 */
gsize
axis_library_utf8_sequence(
	const guchar *p,
	gsize         left
);

/* Returns: %TRUE if @length bytes at @bytes are valid UTF-8 (no NUL) */
gboolean
axis_library_utf8_valid(
	const gchar *bytes,
	gsize        length
);

/**
 * axis_library_name_encode:
 * @out: where the stored form is appended
 * @name: the name's bytes (no '/' or NUL is expected, and none is made)
 * @length: how many
 *
 * Appends the stored form of one name (or of a whole path: '/' is kept as
 * it is).
 *
 * Returns: %TRUE if the bytes were valid UTF-8
 */
gboolean
axis_library_name_encode(
	GString     *out,
	const gchar *name,
	gsize        length
);

/**
 * axis_library_name_decode:
 * @stored: a stored form (name or path)
 * @bytes: (out) (optional) (transfer full): the original bytes (->len
 *   holds the length; a NUL follows them)
 *
 * Undoes axis_library_name_encode().  Only the canonical form is accepted:
 * text that encoding the result would not give back exactly (an unknown
 * escape, lower-case hex, an escaped byte that needs no escape, a NUL) is
 * refused.
 *
 * Returns: %TRUE if @stored is a canonical stored form
 */
gboolean
axis_library_name_decode(
	const gchar  *stored,
	GString     **bytes
);

/* Returns: %TRUE if the stored form's bytes are not valid UTF-8 (FALSE for
 * one that is not canonical) */
gboolean
axis_library_name_is_invalid(const gchar *stored);

/**
 * axis_library_name_display:
 * @stored: (nullable): a stored form
 *
 * The name as a person should see it, safe for a page or a log line:
 * valid UTF-8, bytes that are not are shown as U+FFFD, control characters
 * as "\n", "\t", "\r" or "\xHH" (C1 controls and invisible direction
 * marks as "\u{XXXX}"), so that a name can neither break a line nor
 * reorder the text around it.  Not reversible: see
 * axis_library_name_exact().
 *
 * Returns: (transfer full): the text ("" for %NULL; "(unreadable)" for a
 *   form that is not canonical)
 */
gchar *
axis_library_name_display(const gchar *stored);

/**
 * axis_library_name_exact:
 * @stored: a stored form
 *
 * The exact bytes in a printable, reversible form: printable characters
 * as they are, a backslash as "\\", and every byte of anything else
 * (control and direction characters, bytes that are not valid UTF-8) as
 * "\xHH".
 *
 * Returns: (transfer full): the text ("(unreadable)" for a form that is
 *   not canonical)
 */
gchar *
axis_library_name_exact(const gchar *stored);

/**
 * axis_library_text_clean:
 * @text: (nullable): a message (from the database, the system, a mix)
 * @max_chars: at most this many characters are kept
 *
 * Makes a message safe to store and log: valid UTF-8, every control or
 * direction-changing character replaced by a space, cut to @max_chars.
 *
 * Returns: (transfer full) (nullable): the text, %NULL for %NULL
 */
gchar *
axis_library_text_clean(
	const gchar *text,
	guint        max_chars
);

/**
 * axis_library_json_append_string:
 * @out: where to append
 * @utf8: valid UTF-8
 *
 * Appends @utf8 as a JSON string literal: quotes, backslashes and control
 * characters escaped, everything else as it is.
 */
void
axis_library_json_append_string(
	GString     *out,
	const gchar *utf8
);

G_END_DECLS

#endif /* AXIS_LIBRARY_NAME_H */
