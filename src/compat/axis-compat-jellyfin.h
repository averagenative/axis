/*
 * axis-compat-jellyfin.h - the parsers for the Jellyfin server's JSON answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each function reads the body of one answer and returns what it holds as
 * the structures of axis-compat-model.h, exactly as axis-compat-plex.h does
 * for Plex, and with the same promises: the server is untrusted, so every
 * field is optional, every number range-checked, every text cleaned and cut,
 * every list cut, a path too long to keep is dropped whole, and a word the
 * daemon does not know is unknown.  The shapes were taken from a real
 * server (Jellyfin 10.11.8; tests/fixtures/compat/jellyfin/recorded) and
 * its published API description.
 *
 * What differs from Plex:
 *
 *  - Jellyfin answers are plain JSON documents: /System/Info is an object,
 *    /Library/VirtualFolders and /Sessions are arrays, /Devices and /Items
 *    are objects with an "Items" array.
 *  - A device id (the id a client makes up for itself, often a long base64
 *    text) is not always short or made of the characters an id may have.
 *    One that is not is kept as "h:" and the first 40 hex digits of its
 *    SHA-256 so that it stays the same device and never becomes a key that
 *    could be mistaken for another.
 *  - A session lists the item it plays only while it plays; one that plays
 *    nothing is left out, and is not "a session".
 *  - A session states why it is converted (TranscodingInfo.TranscodeReasons);
 *    those words are kept (cleaned, cut), and mean something only through
 *    the table of axis-compat-reasons.h.
 *  - The item's library is not stated: it is worked out from its path and
 *    the libraries' folders.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-compat-jellyfin.c).
 */

#ifndef AXIS_COMPAT_JELLYFIN_H
#define AXIS_COMPAT_JELLYFIN_H

#include <glib.h>

#include "compat/axis-compat-model.h"

G_BEGIN_DECLS

/* GET /System/Info */
gboolean
axis_compat_jellyfin_parse_identity(
	const gchar        *data,
	gsize               length,
	AxisCompatIdentity *identity,
	gchar             **problem
);

/* GET /Library/VirtualFolders: (element-type AxisCompatLibrary), or %NULL */
GPtrArray *
axis_compat_jellyfin_parse_libraries(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /Devices: (element-type AxisCompatDevice), or %NULL */
GPtrArray *
axis_compat_jellyfin_parse_devices(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /Sessions: (element-type AxisCompatSession) of the sessions that are
 * playing something, or %NULL */
GPtrArray *
axis_compat_jellyfin_parse_sessions(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /Items?Ids=<id>&Fields=Path: the one item's path */
gboolean
axis_compat_jellyfin_parse_item(
	const gchar    *data,
	gsize           length,
	AxisCompatItem *item,
	gchar         **problem
);

/**
 * axis_compat_jellyfin_key:
 * @raw: an id as the server states it
 *
 * Returns: (transfer full) (nullable): @raw if it is a valid id
 *   (axis_compat_plex_id_valid()), else "h:" and 40 hex digits of its SHA-256
 *   for a text that is valid UTF-8 without control characters of at most
 *   512 bytes, else %NULL
 */
gchar *
axis_compat_jellyfin_key(const gchar *raw);

G_END_DECLS

#endif /* AXIS_COMPAT_JELLYFIN_H */
