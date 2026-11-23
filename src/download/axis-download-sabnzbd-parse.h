/*
 * axis-download-sabnzbd-parse.h - SABnzbd's JSON answers, read into the
 * downloads layer's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything SABnzbd says is untrusted input, and what is known of its
 * format is documentation and memory, not a recording (see the fixtures in
 * tests/fixtures/sabnzbd).  So every field is optional and type-checked:
 * one that is missing, null, of the wrong type, out of range or not valid
 * text is left out; an entry without an identifier is skipped, and one with
 * an identifier but no name or no category is a slot whose item's
 * @unreadable says why.  A name is kept exactly too (the item's @own_name).
 * Lists are cut at %AXIS_SABNZBD_MAX_SLOTS, strings are cleaned
 * and cut, sizes are SABnzbd's decimal megabyte strings ("mb", "mbleft")
 * in the queue and a number of bytes ("bytes") in the history, read with
 * range checks.  An answer that is not one JSON object is unreadable.
 *
 * A 200 answer is not yet a success: SABnzbd reports most refusals as
 * {"status": false, "error": "..."}, which is
 * %AXIS_SABNZBD_REPLY_REFUSED with its cleaned words, or
 * %AXIS_SABNZBD_REPLY_BAD_KEY when those words are about the API key.
 *
 * The status words: in the queue "Downloading", "Fetching" and "Grabbing"
 * are downloading, "Queued" and "Propagating" queued, "Paused" paused; in
 * the history "Completed" is completed (with the job's final folder,
 * "storage"), "Failed" failed (with "fail_message"), and the statuses of
 * the work after the download ("Verifying", "Repairing", "Extracting",
 * "Moving", "Running", "QuickCheck", and "Queued" and "Fetching" there)
 * are downloading at 100 %, because the files are not yet where they will
 * be.  Any other word is unknown: the slot says so and holds the word, for
 * the kind to keep the last state it knew and to log the word once.
 *
 * GLib only (and the GLib-only JSON reader of probe/axis-probe-json.h), no
 * I/O: read by tests/fuzz/fuzz-download-sabnzbd.c.  The kind, which makes
 * the requests, is download/axis-download-sabnzbd.h.
 */

#ifndef AXIS_DOWNLOAD_SABNZBD_PARSE_H
#define AXIS_DOWNLOAD_SABNZBD_PARSE_H

#include <glib.h>

#include "download/axis-download-model.h"
#include "probe/axis-probe-json.h"

G_BEGIN_DECLS

/* At most this many entries of a queue or a history are read. */
#define AXIS_SABNZBD_MAX_SLOTS     5000

/* Longest status word kept, in characters. */
#define AXIS_SABNZBD_WORD_MAX      40

/* What a size in megabytes may be at most (a petabyte). */
#define AXIS_SABNZBD_MAX_MEGABYTES 1000000000.0

typedef enum
{
	AXIS_SABNZBD_REPLY_OK,		/* an object with no failure status */
	AXIS_SABNZBD_REPLY_REFUSED,	/* "status": false */
	AXIS_SABNZBD_REPLY_BAD_KEY,	/* refused, and about the API key */
	AXIS_SABNZBD_REPLY_UNREADABLE	/* not one JSON object */
} AxisSabnzbdReplyKind;

/**
 * axis_sabnzbd_reply_parse:
 * @data: the body of a 2xx answer
 * @doc: (out) (transfer full): the document, for %AXIS_SABNZBD_REPLY_OK
 * @message: (out) (transfer full) (nullable): for a refusal, SABnzbd's words
 *   cleaned and cut (%NULL when it gave none); for unreadable, a fixed
 *   sentence fragment
 *
 * An old SABnzbd answers a wrong key in plain text ("error: API Key
 * Incorrect") even when JSON is asked for: that is %AXIS_SABNZBD_REPLY_BAD_KEY
 * too.
 */
AxisSabnzbdReplyKind
axis_sabnzbd_reply_parse(
	const gchar   *data,
	gsize          length,
	AxisJsonDoc  **doc,
	gchar        **message
);

/* mode=version: its "version", cleaned; %NULL when it has none. */
gchar *
axis_sabnzbd_parse_version(const AxisJsonNode *root);

/* mode=get_cats: whether @category is among "categories" (compared without
 * regard to case): 1, 0, or -1 when the answer has no list of them. */
gint
axis_sabnzbd_parse_categories(
	const AxisJsonNode *root,
	const gchar        *category
);

/**
 * axis_sabnzbd_parse_added:
 * @root: the answer of mode=addfile
 * @nzo_id: (out) (transfer full): the identifier of the one job it made
 * @message: (out) (transfer full): when this fails, why, as a fixed
 *   sentence plus SABnzbd's cleaned "error" when it gave one
 *
 * Success is "status" true and exactly one valid identifier in "nzo_ids":
 * anything else is a refusal.
 *
 * Returns: %TRUE for a job made
 */
gboolean
axis_sabnzbd_parse_added(
	const AxisJsonNode  *root,
	gchar              **nzo_id,
	gchar              **message
);

/* A SABnzbd job identifier: 1 to 64 letters, digits, '_', '.' and '-'. */
gboolean
axis_sabnzbd_ref_valid(const gchar *text);

/**
 * AxisSabnzbdSlot:
 * @item: the download as the layer takes it (the layer checks it again)
 * @known: its status word is one of the known words; when %FALSE @item's
 *   state is only a placeholder and @word is why
 * @word: (nullable): the status word, cleaned and cut to
 *   %AXIS_SABNZBD_WORD_MAX characters
 * @history: it was in the history (else the queue)
 */
typedef struct
{
	AxisDownloadItem *item;
	gboolean          known;
	gchar            *word;
	gboolean          history;
} AxisSabnzbdSlot;

void
axis_sabnzbd_slot_free(AxisSabnzbdSlot *slot);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisSabnzbdSlot, axis_sabnzbd_slot_free)

/**
 * axis_sabnzbd_parse_queue:
 * @root: the answer of mode=queue
 *
 * Returns: (transfer full) (nullable) (element-type AxisSabnzbdSlot): the
 *   slots with an identifier and a name, at most %AXIS_SABNZBD_MAX_SLOTS;
 *   %NULL when the answer has no "queue" with a list of "slots"
 */
GPtrArray *
axis_sabnzbd_parse_queue(const AxisJsonNode *root);

/* As above for mode=history ("history" and its "slots"). */
GPtrArray *
axis_sabnzbd_parse_history(const AxisJsonNode *root);

/* As axis_sabnzbd_parse_queue() and axis_sabnzbd_parse_history(), saying in
 * *@unreadable how many slots could not be read even by their identifier
 * (left out). */
GPtrArray *
axis_sabnzbd_parse_queue_counted(
	const AxisJsonNode *root,
	guint              *unreadable
);

GPtrArray *
axis_sabnzbd_parse_history_counted(
	const AxisJsonNode *root,
	guint              *unreadable
);

/* mode=queue&name=delete: whether the identifiers it removed ("nzo_ids")
 * include @nzo_id. */
gboolean
axis_sabnzbd_parse_removed(
	const AxisJsonNode *root,
	const gchar        *nzo_id
);

/* The longest folder setting of SABnzbd's that is read, in bytes. */
#define AXIS_SABNZBD_PATH_MAX      1024

/**
 * axis_sabnzbd_parse_category_dir:
 * @root: the answer of mode=get_config&section=categories: "config" holding
 *   "categories", a list of objects with "name" and "dir" (verified against
 *   SABnzbd 5.1.3; a list at the top level is read too)
 * @category: the category's name, compared without regard to case
 * @dir: (out) (transfer full): its "dir" as configured ("" for none), when
 *   it is a string of at most %AXIS_SABNZBD_PATH_MAX bytes without control
 *   characters
 *
 * Returns: 1 when the category is listed with such a "dir", 0 when it is not
 *   listed, -1 when the answer has no list of categories or the entry's
 *   "dir" cannot be read
 */
gint
axis_sabnzbd_parse_category_dir(
	const AxisJsonNode  *root,
	const gchar         *category,
	gchar              **dir
);

/**
 * axis_sabnzbd_parse_other_dirs:
 * @root: as for axis_sabnzbd_parse_category_dir()
 * @category: Axis's category (left out, compared without regard to case)
 * @names: (out) (transfer full) (element-type utf8): the other categories'
 *   names ("*", SABnzbd's own for jobs without one, among them)
 * @dirs: (out) (transfer full) (element-type utf8): their "dir" settings, in
 *   the same order; %NULL in it for one that cannot be read (not a string,
 *   too long, a control character)
 *
 * At most %AXIS_SABNZBD_MAX_SLOTS categories are read.
 *
 * Returns: %FALSE when the answer has no list of categories
 */
/**
 * axis_sabnzbd_folder_shared:
 * @complete_dir: (nullable): SABnzbd's folder for finished downloads
 * @folder: the folder of Axis's category (axis_sabnzbd_category_folder())
 * @dir: (nullable): another category's folder setting
 * @theirs: (out) (transfer full): its folder, when it is @folder or inside it
 *
 * Whether another category of SABnzbd's puts its finished jobs in the folder
 * of Axis's category, or in a folder inside it.  A category with no folder of
 * its own, and one whose folder is the folder for every finished download,
 * put their jobs beside Axis's folder: not shared.
 *
 * Returns: 1 shared, 0 not, -1 when it cannot be told (a setting that cannot
 *   be read, a relative one with no full @complete_dir, a "." or ".." part)
 */
gint
axis_sabnzbd_folder_shared(
	const gchar  *complete_dir,
	const gchar  *folder,
	const gchar  *dir,
	gchar       **theirs
);

gboolean
axis_sabnzbd_parse_other_dirs(
	const AxisJsonNode  *root,
	const gchar         *category,
	GPtrArray          **names,
	GPtrArray          **dirs
);

/* mode=get_config&section=misc&keyword=complete_dir: "config", "misc",
 * "complete_dir" (or "complete_dir" of "misc" at the top level), as above;
 * %NULL when there is none that can be read. */
gchar *
axis_sabnzbd_parse_complete_dir(const AxisJsonNode *root);

/**
 * axis_sabnzbd_category_folder:
 * @complete_dir: (nullable): SABnzbd's folder for finished downloads
 * @dir: the category's folder setting
 * @folder: (out) (transfer full): the category's own folder, normalised
 * @problem: (out) (transfer none): why there is none Axis can use
 *
 * Where SABnzbd puts a category's finished jobs, as it works it out: a
 * trailing '*' (all of a job's files straight into the folder, with no job
 * folder) is not part of the folder; an absolute @dir is the folder; a
 * relative one is beneath @complete_dir, which must then be absolute.  An
 * empty @dir, or one that comes to @complete_dir itself, is no folder of the
 * category's own (its jobs would sit among every other category's), and a
 * "." or ".." part is refused, never resolved.
 *
 * Returns: %TRUE when the category has a folder of its own
 */
gboolean
axis_sabnzbd_category_folder(
	const gchar  *complete_dir,
	const gchar  *dir,
	gchar       **folder,
	const gchar **problem
);

/* SABnzbd's decimal megabytes ("1949.06", or a number) as bytes: %TRUE only
 * for a finite value from 0 to %AXIS_SABNZBD_MAX_MEGABYTES. */
gboolean
axis_sabnzbd_megabytes_parse(
	const AxisJsonNode *node,
	gint64             *bytes
);

/* "H:MM:SS" (and "D:HH:MM:SS", and "MM:SS") as seconds, or -1. */
gint64
axis_sabnzbd_timeleft_parse(const gchar *text);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_SABNZBD_PARSE_H */
