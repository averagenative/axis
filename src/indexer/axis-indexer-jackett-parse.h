/*
 * axis-indexer-jackett-parse.h - reading what GitHub answers about Jackett's
 * definitions: a commit's id, the list of one folder's files, a file's name
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is outside input (add-definition-fetch): the answers of
 * GitHub's API to the two requests that list Jackett's definition files, and
 * the names a form sends back.  A name is accepted only in one form, a file
 * name and never a path; anything else in a listing is counted and left out.
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h), no
 * I/O: fuzzed through tests/fuzz/fuzz-indexer-jackett.c.
 */

#ifndef AXIS_INDEXER_JACKETT_PARSE_H
#define AXIS_INDEXER_JACKETT_PARSE_H

#include <glib.h>

G_BEGIN_DECLS

/* A definition's file name: "[a-z0-9][a-z0-9-]*\.yml", its part before
 * ".yml" at most this long (the longest definition id). */
#define AXIS_JACKETT_MAX_STEM_BYTES 64

/* GitHub lists at most this many files of a folder through its contents
 * API; a listing of this many may have been cut. */
#define AXIS_JACKETT_LISTING_CAP    1000

/* At most this many entries of a listing are read, whatever it holds. */
#define AXIS_JACKETT_MAX_ENTRIES    4000

typedef struct
{
	gchar  *name;		/* "madeupsite.yml" */
	gchar  *blob;		/* the file's git blob id: 40 hex digits, lower case */
	gint64  size_bytes;
} AxisJackettFile;

void
axis_jackett_file_free(AxisJackettFile *file);

/* Whether @name is a definition's file name: a lower-case letter or digit,
 * then letters, digits and '-', then ".yml"; never a path, a dot file, or
 * anything with another character. */
gboolean
axis_jackett_name_valid(const gchar *name);

/* Whether @text is 40 hex digits in lower case (a commit's or a blob's id). */
gboolean
axis_jackett_id_valid(const gchar *text);

/**
 * axis_jackett_parse_commit:
 * @data: the answer to the commit request (media type
 *   "application/vnd.github.sha"): the id as text
 *
 * Returns: (transfer full) (nullable): the id (40 hex digits, lower case,
 *   white space around it dropped), or %NULL when the answer is anything else
 */
gchar *
axis_jackett_parse_commit(
	const gchar *data,
	gsize        length
);

/**
 * axis_jackett_parse_listing:
 * @data: the answer to the contents request: a JSON array of objects
 * @files: (out) (transfer full) (element-type AxisJackettFile): the files
 *   whose entry is a file with a valid name, a valid blob id and a size,
 *   each name once, by name
 * @n_left_out: (out): entries that were something else (a folder, another
 *   kind of file, a name not accepted, a second entry of a name)
 * @cut: (out): the listing held %AXIS_JACKETT_LISTING_CAP entries or more
 * @problem: (out) (optional) (transfer full): why not, a sentence
 *
 * Returns: %TRUE when the answer is a listing (which may hold no file)
 */
gboolean
axis_jackett_parse_listing(
	const gchar  *data,
	gsize         length,
	GPtrArray   **files,
	guint        *n_left_out,
	gboolean     *cut,
	gchar       **problem
);

/* The git blob id of a file's bytes: the SHA-1 of "blob <length>\0" and the
 * bytes, in lower-case hex.  What a listing gives for each file, so that a
 * file fetched can be checked to be the one listed. */
gchar *
axis_jackett_blob_id(
	const gchar *data,
	gsize        length
);

G_END_DECLS

#endif /* AXIS_INDEXER_JACKETT_PARSE_H */
