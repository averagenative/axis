/*
 * axis-compat-plex.h - the parsers for the Plex Media Server's JSON answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each function reads the body of one answer (bounded by the HTTP helper's
 * size cap, and by the reader's own limits on depth, nodes and string
 * length) and returns what it holds as the structures of
 * axis-compat-model.h.  The server is untrusted:
 *
 *  - every field is optional; one that is missing, of the wrong type or
 *    out of range is "not stated", never an error and never a default that
 *    could be mistaken for a fact;
 *  - ids are kept only if they are short and made of letters, digits and a
 *    few punctuation marks (they become database keys); a longer one is
 *    dropped, not cut, so it cannot be taken for another;
 *  - names are cleaned (valid UTF-8, no control characters) and cut;
 *  - a path longer than the limit is dropped, not cut, so that a prefix of
 *    one file's path is never taken for another file;
 *  - lists are cut at their maximum length;
 *  - a decision word that is not known is %AXIS_COMPAT_DECISION_UNKNOWN.
 *
 * What is refused whole is a body that is not JSON within the reader's
 * limits, or that is not a media container at all (the wrong kind of
 * server answering); the problem is a short sentence without any of the
 * input in it.
 *
 * GLib only, no I/O: there is a fuzz target (tests/fuzz/fuzz-compat-plex.c).
 */

#ifndef AXIS_COMPAT_PLEX_H
#define AXIS_COMPAT_PLEX_H

#include <glib.h>

#include "compat/axis-compat-model.h"

G_BEGIN_DECLS

/* GET /identity */
gboolean
axis_compat_plex_parse_identity(
	const gchar        *data,
	gsize               length,
	AxisCompatIdentity *identity,
	gchar             **problem
);

/* GET / : adds the server's name to @identity (nothing else). */
gboolean
axis_compat_plex_parse_root(
	const gchar        *data,
	gsize               length,
	AxisCompatIdentity *identity,
	gchar             **problem
);

/* GET /library/sections: (element-type AxisCompatLibrary), or %NULL */
GPtrArray *
axis_compat_plex_parse_libraries(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /devices: (element-type AxisCompatDevice), or %NULL */
GPtrArray *
axis_compat_plex_parse_devices(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /accounts: (element-type AxisCompatAccount), or %NULL */
GPtrArray *
axis_compat_plex_parse_accounts(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /status/sessions: (element-type AxisCompatSession), or %NULL */
GPtrArray *
axis_compat_plex_parse_sessions(
	const gchar  *data,
	gsize         length,
	gchar       **problem
);

/* GET /status/sessions/history/all */
gboolean
axis_compat_plex_parse_history(
	const gchar       *data,
	gsize              length,
	AxisCompatHistory *history,
	gchar            **problem
);

/* GET /library/metadata/<ratingKey> */
gboolean
axis_compat_plex_parse_item(
	const gchar    *data,
	gsize           length,
	AxisCompatItem *item,
	gchar         **problem
);

/* A server id (rating key, library key, session id, device id) is kept
 * only if it is this: 1 to AXIS_COMPAT_MAX_ID_CHARS of letters, digits and
 * "_.:-". */
gboolean
axis_compat_plex_id_valid(const gchar *id);

G_END_DECLS

#endif /* AXIS_COMPAT_PLEX_H */
