/*
 * axis-download-deluge-parse.h - Deluge's JSON-RPC answers, read into the
 * downloads layer's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything Deluge says is untrusted input, and what is known of its format
 * is documentation and memory, not a recording (see the fixtures in
 * tests/fixtures/deluge).  So every field is optional and type-checked: one
 * that is missing, null, of the wrong type, out of range or not valid text is
 * left out; an entry without a valid info-hash is skipped, and one with a
 * valid info-hash but no name, no label or no object is a slot whose item's
 * @unreadable says why (nothing else of it read).  Its name is kept exactly
 * too (the item's @own_name).  Lists are cut at %AXIS_DELUGE_MAX_TORRENTS,
 * strings are cleaned and cut, hashes are 40 hexadecimal characters and are
 * lower-cased.  An
 * answer that is not one JSON object is unreadable.
 *
 * The envelope: {"result": ..., "error": null | {"message", "code"}, "id": N}.
 * It is read only if "id" is the number that was sent and "result" and
 * "error" are not both set (an error has a null result; a success a null
 * error; neither present is unreadable).  An error with code 1, or whose
 * message says "Not authenticated", is %AXIS_DELUGE_REPLY_NOT_AUTH: the
 * session is gone, and the call did nothing.
 *
 * The state words (a torrent's "state"): "Downloading" is downloading;
 * "Queued", "Checking" and "Allocating" are queued; "Paused" is paused;
 * "Moving" is downloading, all of it (even when "is_finished" is true: the
 * files are not where they will be); "Seeding" is completed; "Error" is
 * failed (with the tracker status).  Any other torrent whose "is_finished" is
 * true is completed whatever its state says, with its "download_location"
 * joined with its name as the path; it stays completed while it seeds.  Any other
 * word is unknown: the slot says so and holds the word, for the kind to keep
 * the last state it knew and to log the word once.
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h), no
 * I/O: read by tests/fuzz/fuzz-download-deluge.c.  The kind, which makes the
 * requests, is download/axis-download-deluge.h.
 */

#ifndef AXIS_DOWNLOAD_DELUGE_PARSE_H
#define AXIS_DOWNLOAD_DELUGE_PARSE_H

#include <glib.h>

#include "download/axis-download-model.h"
#include "probe/axis-probe-json.h"

G_BEGIN_DECLS

/* At most this many torrents of a list are read. */
#define AXIS_DELUGE_MAX_TORRENTS  5000

/* At most this many elements of any other list are looked at. */
#define AXIS_DELUGE_MAX_LIST      5000

/* Longest state word kept, in characters. */
#define AXIS_DELUGE_WORD_MAX      40

/* Deluge's own words about an error, cut, in characters. */
#define AXIS_DELUGE_ERROR_MAX_CHARS 200

typedef enum
{
	AXIS_DELUGE_REPLY_OK,		/* a result, no error */
	AXIS_DELUGE_REPLY_ERROR,	/* an error of Deluge's */
	AXIS_DELUGE_REPLY_NOT_AUTH,	/* an error: not authenticated */
	AXIS_DELUGE_REPLY_UNREADABLE	/* not an envelope that can be believed */
} AxisDelugeReplyKind;

/**
 * axis_deluge_reply_parse:
 * @data: the body of a 2xx answer
 * @id: the "id" the request carried
 * @doc: (out) (transfer full): the document, for %AXIS_DELUGE_REPLY_OK
 * @result: (out) (transfer none): its "result" (owned by @doc; may be a JSON
 *   null), for %AXIS_DELUGE_REPLY_OK
 * @message: (out) (transfer full) (nullable): for an error, Deluge's words
 *   cleaned and cut (%NULL when it gave none); for unreadable, a fixed
 *   sentence fragment
 */
AxisDelugeReplyKind
axis_deluge_reply_parse(
	const gchar         *data,
	gsize                length,
	gint64               id,
	AxisJsonDoc        **doc,
	const AxisJsonNode **result,
	gchar              **message
);

/* A label Deluge's Label plugin takes, and so Axis's with this kind: 1 to 64
 * of the lower-case letters, digits, '.', '_' and '-', not starting with a
 * punctuation mark. */
gboolean
axis_deluge_label_valid(const gchar *text);

/* A result that is exactly a JSON boolean. */
gboolean
axis_deluge_parse_boolean(
	const AxisJsonNode *result,
	gboolean           *value
);

/* A result that is a string holding an info-hash (40 hexadecimal digits, of
 * either case): the hash in lower case; else %NULL. */
gchar *
axis_deluge_parse_hash(const AxisJsonNode *result);

/* A daemon's version: a result that is a string, cleaned; else %NULL. */
gchar *
axis_deluge_parse_version(const AxisJsonNode *result);

/* Whether the result, a list of names, holds @name (compared without regard
 * to case): 1, 0, or -1 when it is not a list. */
gint
axis_deluge_parse_has_name(
	const AxisJsonNode *result,
	const gchar        *name
);

/* As above, case matters (labels). */
gint
axis_deluge_parse_has_label(
	const AxisJsonNode *result,
	const gchar        *label
);

/**
 * axis_deluge_parse_host:
 * @result: the answer of web.get_hosts: a list of [id, host, port, ...]
 * @id: (out) (transfer full): the first host's identifier
 * @host: (out) (transfer full) (nullable): its name or address, cleaned
 * @port: (out): its port, or 0
 *
 * Returns: %TRUE if the list holds a host with an identifier Deluge's
 *   web.connect can be given (1 to %AXIS_DOWNLOAD_REF_MAX printable
 *   characters, no space)
 */
gboolean
axis_deluge_parse_host(
	const AxisJsonNode  *result,
	gchar              **id,
	gchar              **host,
	gint                *port
);

/**
 * axis_deluge_says_not_in_session:
 * @words: (nullable): Deluge's words about an error (as the envelope gave
 *   them, cleaned and cut)
 * @hash: the torrent asked about (40 hexadecimal digits)
 *
 * Whether the words say that Deluge has no torrent @hash: its core raises
 * InvalidTorrentError "torrent_id <hash> not in session." for a hash it does
 * not have, and the web interface passes that on as the error's message
 * (seen from Deluge 2.2.0 for core.remove_torrent: "Failure: [...
 * InvalidTorrentError...: torrent_id <hash> not in session. ]").  Both the
 * error's name and the sentence with this very hash must be there; case is
 * not looked at.
 */
gboolean
axis_deluge_says_not_in_session(
	const gchar *words,
	const gchar *hash
);

/**
 * axis_deluge_parse_label_folder:
 * @result: the result of the Label plugin's label.get_options: an object
 * @folder: (out) (transfer full): when 1, the label's "Move completed to"
 *   folder as Deluge gives it (valid text, no control characters; not yet
 *   checked as a path)
 *
 * Whether the label moves its finished torrents to a folder of its own:
 * "apply_move_completed" and "move_completed" both exactly true and
 * "move_completed_path" a text that is not empty.
 *
 * Returns: 1 with *@folder, 0 when it does not, -1 when @result is not an
 *   object
 */
gint
axis_deluge_parse_label_folder(
	const AxisJsonNode  *result,
	gchar              **folder
);

/* The text of @key in the result of core.get_config_values (an object), valid
 * text without control characters; %NULL when it is not there or is not
 * that. */
gchar *
axis_deluge_parse_config_text(
	const AxisJsonNode *result,
	const gchar        *key
);

/* The labels a result of label.get_labels lists (at most
 * %AXIS_DELUGE_MAX_LIST, each valid as a label), or %NULL when it is not a
 * list. */
GPtrArray *
axis_deluge_parse_labels(const AxisJsonNode *result);

/**
 * AxisDelugeSlot:
 * @item: the download as the layer takes it (the layer checks it again); its
 *   identifier and info-hash are the same lower-case hash
 * @known: the state word is one the kind knows, or the torrent is finished
 *   (which settles its state); when %FALSE @item's state is only a
 *   placeholder and @word is why
 * @word: (nullable): the state word, cleaned and cut
 */
typedef struct
{
	AxisDownloadItem *item;
	gboolean          known;
	gchar            *word;
} AxisDelugeSlot;

void
axis_deluge_slot_free(AxisDelugeSlot *slot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDelugeSlot, axis_deluge_slot_free)

/**
 * axis_deluge_parse_label_of:
 * @result: the result of core.get_torrents_status asked for one torrent
 * @hash: that torrent's
 * @label: (out) (transfer full): its label as Deluge says it ("" for none)
 * @name: (out) (transfer full) (optional) (nullable): its name, cleaned
 *
 * Returns: 1 when the result holds the torrent with a label (Deluge has it),
 *   0 when it does not hold it (Deluge has no such torrent), -1 when it cannot
 *   be read (not an object, the torrent's entry not one, no "label" in it) or
 *   cannot be told (more than %AXIS_DELUGE_MAX_TORRENTS entries without it:
 *   Deluge left out the filter, and the rest was not looked at)
 */
gint
axis_deluge_parse_label_of(
	const AxisJsonNode  *result,
	const gchar         *hash,
	gchar              **label,
	gchar              **name
);

/**
 * axis_deluge_parse_torrents:
 * @result: the result of core.get_torrents_status: an object keyed by hash
 *
 * Returns: (transfer full) (nullable) (element-type AxisDelugeSlot): the
 *   slots with a valid hash, a name and a label, at most
 *   %AXIS_DELUGE_MAX_TORRENTS, a hash once; %NULL when the result is not an
 *   object
 */
GPtrArray *
axis_deluge_parse_torrents(const AxisJsonNode *result);

/* axis_deluge_parse_torrents(), saying in *@unreadable (optional) how many
 * entries could not be read even by their hash (left out) and in *@cut
 * (optional) whether the list was cut at %AXIS_DELUGE_MAX_TORRENTS. */
GPtrArray *
axis_deluge_parse_torrents_counted(
	const AxisJsonNode *result,
	guint              *unreadable,
	gboolean           *cut
);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_DELUGE_PARSE_H */
