/*
 * axis-download-bencode.h - the little of a .torrent file Axis reads: its
 * info-hash and its name
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A release file is uploaded to the download client as it is; the only
 * reason to look inside a .torrent is to know its info-hash, so that the
 * download can be recognised in the client's list (after a restart in the
 * middle of sending, for one).  The info-hash (v1) is the SHA-1 of the
 * exact bytes of the bencoded value of the top-level dictionary's "info"
 * key.
 *
 * The reader checks the whole file as bencode (integers, byte strings,
 * lists, dictionaries with byte-string keys), with a bounded depth and
 * within the bytes given, and refuses anything else: trailing bytes, an
 * integer with a leading zero or "-0", a string longer than what is left,
 * a top level that is not a dictionary, two "info" keys, an "info" that is
 * not a dictionary.  Keys are not required to be sorted (some tools write
 * them unsorted); the hash is of the bytes as they are.
 *
 * GLib only, no I/O, no allocation proportional to the input beyond the
 * caller's buffer: fuzzed through tests/fuzz/fuzz-download-bencode.c.
 */

#ifndef AXIS_DOWNLOAD_BENCODE_H
#define AXIS_DOWNLOAD_BENCODE_H

#include <glib.h>

G_BEGIN_DECLS

/* Nesting deeper than this is refused. */
#define AXIS_BENCODE_MAX_DEPTH 48

/* Lists and dictionaries with more elements than this, in all, are refused. */
#define AXIS_BENCODE_MAX_ITEMS 4000000

/**
 * axis_download_torrent_read:
 * @data: the file
 * @length: its length in bytes
 * @info_hash: (out) (optional) (transfer full): 40 lower-case hex digits
 * @name: (out) (optional) (transfer full) (nullable): its "info"/"name",
 *   cleaned as a release name, or %NULL when it has none that is text
 * @problem: (out) (optional) (transfer none): why it is refused, a phrase
 *
 * Returns: %TRUE if @data is a torrent file
 */
gboolean
axis_download_torrent_read(
	const guint8  *data,
	gsize          length,
	gchar        **info_hash,
	gchar        **name,
	const gchar  **problem
);

/**
 * axis_bencode_valid:
 *
 * Returns: %TRUE if @data is exactly one bencoded value, as the reader
 *   above accepts it
 */
gboolean
axis_bencode_valid(
	const guint8 *data,
	gsize         length
);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_BENCODE_H */
