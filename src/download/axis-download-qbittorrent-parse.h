/*
 * axis-download-qbittorrent-parse.h - qBittorrent's Web API v2 answers, read
 * into the downloads layer's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything qBittorrent says is untrusted input, and what is known of its
 * format is documentation and memory, not a recording (see the fixtures in
 * tests/fixtures/qbittorrent).  So every field is optional and type-checked:
 * one that is missing, null, of the wrong type, out of range or not valid
 * text is left out; an entry without a valid info-hash, or not of the
 * category asked for, is skipped, and one with a valid info-hash but no name
 * is a slot whose item's @unreadable says why.  A name is kept exactly too
 * (the item's @own_name).  Lists are cut at %AXIS_QBIT_MAX_TORRENTS,
 * strings are cleaned and cut, hashes are 40 hexadecimal characters and are
 * lower-cased.  The torrents' list and the categories are JSON; an answer
 * that is not the one JSON value expected (a list of torrents, an object of
 * categories) fails the request.  The login's, the add's and the versions'
 * answers are plain text.
 *
 * The state words of a torrent ("state"), mapped by
 * axis_qbit_state_map():
 *
 *   downloading forcedDL stalledDL metaDL forcedMetaDL   downloading
 *   queuedDL allocating checkingDL checkingResumeData     queued
 *   pausedDL stoppedDL                                    paused
 *   uploading forcedUP stalledUP queuedUP checkingUP
 *     pausedUP stoppedUP                                  completed
 *   moving                                                downloading, all of it
 *   error missingFiles                                    failed
 *
 * ("stopped" is what qBittorrent 5.x calls "paused".)  A finished torrent is
 * completed whatever it is doing with its seeding, and stays so; its
 * "content_path" (for 4.1 to 4.3.1, which have none: "save_path" and its
 * name) is its path.  "moving" is not completed: the files are not where they
 * will be.  Any other word, "unknown" included, is not known: the slot says so
 * and holds the word, for the kind to keep the last state it knew and to log
 * the word once.
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h), no
 * I/O: read by tests/fuzz/fuzz-download-qbittorrent.c.  The kind, which makes
 * the requests, is download/axis-download-qbittorrent.h.
 */

#ifndef AXIS_DOWNLOAD_QBITTORRENT_PARSE_H
#define AXIS_DOWNLOAD_QBITTORRENT_PARSE_H

#include <glib.h>

#include "download/axis-download-model.h"
#include "probe/axis-probe-json.h"

G_BEGIN_DECLS

/* At most this many torrents of a list are read. */
#define AXIS_QBIT_MAX_TORRENTS  5000

/* At most this many categories of a list are looked at. */
#define AXIS_QBIT_MAX_LIST      5000

/* Longest state word kept, in characters. */
#define AXIS_QBIT_WORD_MAX      40

/* Longest version text kept, in characters. */
#define AXIS_QBIT_VERSION_MAX   32

/**
 * AxisQbitText:
 * @AXIS_QBIT_TEXT_OK: "Ok.", or nothing at all (a 204)
 * @AXIS_QBIT_TEXT_FAILS: "Fails."
 * @AXIS_QBIT_TEXT_OTHER: anything else
 */
typedef enum
{
	AXIS_QBIT_TEXT_OK,
	AXIS_QBIT_TEXT_FAILS,
	AXIS_QBIT_TEXT_OTHER
} AxisQbitText;

/* What a login's or an add's answer says: the body trimmed of white space,
 * compared as it is (case matters). */
AxisQbitText
axis_qbit_parse_text(
	const gchar *data,
	gsize        length
);

/* A version qBittorrent says in plain text ("v4.6.2", "2.9.3"): at most
 * %AXIS_QBIT_VERSION_MAX of letters, digits, '.', '_', '+' and '-', after
 * trimming; else %NULL. */
gchar *
axis_qbit_parse_version(
	const gchar *data,
	gsize        length
);

/**
 * axis_qbit_parse_has_category:
 * @data: the body of GET /torrents/categories: an object keyed by name
 * @name: a category (case matters)
 *
 * Returns: 1 when it is a key, 0 when it is not, -1 when the answer is not
 *   one JSON object
 */
gint
axis_qbit_parse_has_category(
	const gchar *data,
	gsize        length,
	const gchar *name
);

/**
 * AxisQbitMapped:
 * @state: the state of Axis it means
 * @finished: the torrent has all of what it wanted (the files are complete)
 * @moving: its files are being moved
 */
typedef struct
{
	AxisDownloadItemState state;
	gboolean              finished;
	gboolean              moving;
} AxisQbitMapped;

/* Maps a state word; %FALSE when it is not one qBittorrent documents (and
 * *@mapped is then queued, not finished). */
gboolean
axis_qbit_state_map(
	const gchar    *word,
	AxisQbitMapped *mapped
);

/**
 * AxisQbitSlot:
 * @item: the download as the layer takes it (the layer checks it again); its
 *   identifier and info-hash are the same lower-case hash
 * @known: the state word is one the kind knows; when %FALSE @item's state is
 *   only a placeholder and @word is why
 * @word: (nullable): the state word, cleaned and cut
 */
typedef struct
{
	AxisDownloadItem *item;
	gboolean          known;
	gchar            *word;
} AxisQbitSlot;

void
axis_qbit_slot_free(AxisQbitSlot *slot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisQbitSlot, axis_qbit_slot_free)

/**
 * axis_qbit_parse_torrents:
 * @data: the body of GET /torrents/info: a list of objects
 * @category: (nullable): only torrents of exactly this category are kept
 *   (qBittorrent's own filter may include sub-categories); %NULL keeps every
 *   torrent, whose category is then "" when it has none
 * @problem: (out) (optional) (transfer full): why it cannot be read
 *
 * Returns: (transfer full) (nullable) (element-type AxisQbitSlot): the slots
 *   with a valid hash (and the category asked for), at most
 *   %AXIS_QBIT_MAX_TORRENTS, a hash once; one without a name Axis can read has
 *   its item's @unreadable set (and nothing else of it read); %NULL when the
 *   answer is not one JSON list
 */
GPtrArray *
axis_qbit_parse_torrents(
	const gchar  *data,
	gsize         length,
	const gchar  *category,
	gchar       **problem
);

/* axis_qbit_parse_torrents(), saying in *@unreadable (optional) how many
 * entries of @category (or of any, without one) could not be read even by
 * their hash (not an object, no valid hash: left out) and in *@cut (optional)
 * whether the list
 * was cut at %AXIS_QBIT_MAX_TORRENTS.  An entry whose hash is read but not its
 * name is a slot (its item's @unreadable set), not counted here. */
GPtrArray *
axis_qbit_parse_torrents_counted(
	const gchar  *data,
	gsize         length,
	const gchar  *category,
	guint        *unreadable,
	gboolean     *cut,
	gchar       **problem
);

/* Whether @text is an info-hash: 40 hexadecimal digits, of either case. */
gboolean
axis_qbit_hash_text_valid(const gchar *text);

/**
 * AxisQbitCategory:
 * @name: its name, as qBittorrent keys it
 * @save_path: (nullable): its "savePath" as given ("" when it has none or
 *   it is not text); %NULL when it is text that is not valid (control
 *   characters, invalid UTF-8, too long)
 */
typedef struct
{
	gchar *name;
	gchar *save_path;
} AxisQbitCategory;

void
axis_qbit_category_free(AxisQbitCategory *category);

/* The categories GET /torrents/categories lists (an object keyed by name),
 * at most %AXIS_QBIT_MAX_LIST, or %NULL when the answer is not one JSON
 * object. */
GPtrArray *
axis_qbit_parse_categories(
	const gchar *data,
	gsize        length
);

/* A path answered as plain text (GET /app/defaultSavePath), trimmed: valid
 * UTF-8, no control characters, at most %AXIS_DOWNLOAD_PATH_MAX bytes, not
 * empty; else %NULL.  Not yet checked as a path. */
gchar *
axis_qbit_parse_path_text(
	const gchar *data,
	gsize        length
);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_QBITTORRENT_PARSE_H */
