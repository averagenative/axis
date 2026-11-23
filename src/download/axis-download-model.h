/*
 * axis-download-model.h - what a download is: its states, protocols and
 * priorities, a client's view of one, and the checks on what is sent
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * GLib only, no I/O: the checks of outside input here (magnet links,
 * release names, identifiers) are fuzzed through
 * tests/fuzz/fuzz-download-release.c.
 */

#ifndef AXIS_DOWNLOAD_MODEL_H
#define AXIS_DOWNLOAD_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/* The largest release file (.nzb, .torrent) Axis uploads. */
#define AXIS_DOWNLOAD_MAX_RELEASE_BYTES  (32 * 1024 * 1024)

/* The longest magnet link accepted, in bytes. */
#define AXIS_DOWNLOAD_MAGNET_MAX         4096

/* Release names and the labels of what a download is for, in characters. */
#define AXIS_DOWNLOAD_NAME_MAX_CHARS     300

/* Messages (a client's reason, Axis's problem), in characters. */
#define AXIS_DOWNLOAD_MESSAGE_MAX_CHARS  300

/* A client's own identifier of a download, in bytes. */
#define AXIS_DOWNLOAD_REF_MAX            255

/* A client's path, in bytes. */
#define AXIS_DOWNLOAD_PATH_MAX           1024

/**
 * AxisDownloadProtocol:
 * @AXIS_DOWNLOAD_USENET: an .nzb, for a Usenet client (SABnzbd)
 * @AXIS_DOWNLOAD_TORRENT: a .torrent or a magnet link, for a torrent
 *   client (Deluge)
 * @AXIS_DOWNLOAD_YOUTUBE: a video's address, for the built-in client that
 *   runs yt-dlp (youtube/axis-youtube.h).  No indexer has releases of it
 */
typedef enum
{
	AXIS_DOWNLOAD_USENET,
	AXIS_DOWNLOAD_TORRENT,
	AXIS_DOWNLOAD_YOUTUBE
} AxisDownloadProtocol;

/* The longest address of a release that is one, in bytes. */
#define AXIS_DOWNLOAD_ADDRESS_MAX        2048

/* Whether @text can be a release's address at all: "https://" and at least
 * one more character, printable ASCII without spaces, at most
 * %AXIS_DOWNLOAD_ADDRESS_MAX bytes.  Which addresses a kind takes is the
 * kind's own, stricter check. */
gboolean
axis_download_address_valid(const gchar *text);

/* "usenet", "torrent", "youtube" */
const gchar *
axis_download_protocol_to_string(AxisDownloadProtocol protocol);

gboolean
axis_download_protocol_from_string(
	const gchar          *text,
	AxisDownloadProtocol *protocol
);

/* "Usenet", "Torrent", "YouTube" */
const gchar *
axis_download_protocol_label(AxisDownloadProtocol protocol);

/**
 * AxisDownloadState:
 * @AXIS_DOWNLOAD_SENDING: recorded, being handed to the client (or the
 *   daemon stopped while it was: reconciled with the client's list)
 * @AXIS_DOWNLOAD_QUEUED: the client has it and has not started
 * @AXIS_DOWNLOAD_DOWNLOADING: in progress
 * @AXIS_DOWNLOAD_PAUSED: the client paused it
 * @AXIS_DOWNLOAD_COMPLETED: finished, and its files are at a known place
 *   on a volume
 * @AXIS_DOWNLOAD_FAILED: the client said it failed (its reason is kept),
 *   or it could not be sent
 * @AXIS_DOWNLOAD_GONE: no longer in the client, which did not say why
 * @AXIS_DOWNLOAD_REMOVED: removed from the client by Axis
 * @AXIS_DOWNLOAD_IMPORTED: taken into the library (set by the import
 *   pipeline)
 */
typedef enum
{
	AXIS_DOWNLOAD_SENDING,
	AXIS_DOWNLOAD_QUEUED,
	AXIS_DOWNLOAD_DOWNLOADING,
	AXIS_DOWNLOAD_PAUSED,
	AXIS_DOWNLOAD_COMPLETED,
	AXIS_DOWNLOAD_FAILED,
	AXIS_DOWNLOAD_GONE,
	AXIS_DOWNLOAD_REMOVED,
	AXIS_DOWNLOAD_IMPORTED
} AxisDownloadState;

#define AXIS_DOWNLOAD_N_STATES 9

/* As stored: "sending", "queued", ... */
const gchar *
axis_download_state_to_string(AxisDownloadState state);

gboolean
axis_download_state_from_string(
	const gchar       *text,
	AxisDownloadState *state
);

/* "Sending", "Queued", ... */
const gchar *
axis_download_state_label(AxisDownloadState state);

/* Sending, queued, downloading or paused: what tracking follows. */
gboolean
axis_download_state_is_active(AxisDownloadState state);

/**
 * AxisDownloadPriority:
 *
 * What priority Axis gives its downloads at a client (each kind maps it to
 * its client's own scale).
 */
typedef enum
{
	AXIS_DOWNLOAD_PRIORITY_LOW,
	AXIS_DOWNLOAD_PRIORITY_NORMAL,
	AXIS_DOWNLOAD_PRIORITY_HIGH
} AxisDownloadPriority;

/* "low", "normal", "high" */
const gchar *
axis_download_priority_to_string(AxisDownloadPriority priority);

gboolean
axis_download_priority_from_string(
	const gchar          *text,
	AxisDownloadPriority *priority
);

const gchar *
axis_download_priority_label(AxisDownloadPriority priority);

/**
 * AxisDownloadClientState:
 * @AXIS_DOWNLOAD_CLIENT_UNCHECKED: not contacted since it was configured
 * @AXIS_DOWNLOAD_CLIENT_OK: the last contact worked
 * @AXIS_DOWNLOAD_CLIENT_UNREACHABLE: it could not be reached or did not
 *   answer in time
 * @AXIS_DOWNLOAD_CLIENT_REFUSED: it refused the secret
 * @AXIS_DOWNLOAD_CLIENT_FAILING: it answered, but not usefully (an error, an
 *   answer that could not be read)
 */
typedef enum
{
	AXIS_DOWNLOAD_CLIENT_UNCHECKED,
	AXIS_DOWNLOAD_CLIENT_OK,
	AXIS_DOWNLOAD_CLIENT_UNREACHABLE,
	AXIS_DOWNLOAD_CLIENT_REFUSED,
	AXIS_DOWNLOAD_CLIENT_FAILING
} AxisDownloadClientState;

const gchar *
axis_download_client_state_to_string(AxisDownloadClientState state);

gboolean
axis_download_client_state_from_string(
	const gchar             *text,
	AxisDownloadClientState *state
);

/* "Not checked yet", "Working", "Not answering", "Secret refused", "Failing" */
const gchar *
axis_download_client_state_label(AxisDownloadClientState state);

/**
 * AxisDownloadItemState:
 *
 * A download as the client reports it, in the kinds' common words.
 */
typedef enum
{
	AXIS_DOWNLOAD_ITEM_QUEUED,
	AXIS_DOWNLOAD_ITEM_DOWNLOADING,
	AXIS_DOWNLOAD_ITEM_PAUSED,
	AXIS_DOWNLOAD_ITEM_COMPLETED,
	AXIS_DOWNLOAD_ITEM_FAILED
} AxisDownloadItemState;

/**
 * AxisDownloadItem:
 * @client_ref: the client's identifier
 * @name: the client's name for it
 * @category: its category or label at the client
 * @info_hash: (nullable): a torrent's info-hash, 40 lower-case hex digits
 * @state: its state
 * @size_bytes: its size, or -1 when not known
 * @done_bytes: how much is there, or -1
 * @rate_bps: bytes a second now, or -1
 * @eta_s: seconds left, or -1
 * @path: (nullable): where the client says its files are (the client's own
 *   path), once it has finished
 * @place: (nullable): where its files are now, whatever its state, as far as
 *   the kind can tell (a torrent in progress is in its save folder already);
 *   only what accounts for them in the client's folder (the Leftovers page,
 *   a folder's removal), never what is imported
 * @message: (nullable): the client's words about it (why it failed)
 * @own_name: (nullable): its name exactly as the client gave it: a kind sets
 *   it to the text of the client's answer, before any cleaning (without one,
 *   axis_download_item_check() takes @name as it is before it cleans it);
 *   axis_download_item_check() keeps it only when it can be kept exactly
 *   (text, no control character, within %AXIS_DOWNLOAD_PATH_MAX bytes), else
 *   %NULL.  What is compared with a file's or a folder's name on disk, or with
 *   a release's name, is this, never @name (cleaned for showing: white space
 *   collapsed and trimmed, cut at %AXIS_DOWNLOAD_NAME_MAX_CHARS)
 * @unreadable: (nullable): why the client's entry cannot be used, when its
 *   identifier (@client_ref) can still be read: a kind returns such an entry
 *   instead of leaving it out, so that the layer knows which download it is
 *   (that one is kept as it was; the others are judged as ever).  Such an item
 *   is never used otherwise (axis_download_item_check() refuses it); its
 *   @category is the one the client said, or %NULL when that cannot be read
 *   either.  An entry whose identifier cannot be read is not returned: the
 *   kind marks the list not whole (axis_download_conn_set_list_partial())
 *
 * One download as a kind's list returns it.  The kinds fill the fields
 * from the client's answer; the layer checks and cleans them before using
 * any (axis_download_item_check()).
 */
typedef struct
{
	gchar                 *client_ref;
	gchar                 *name;
	gchar                 *category;
	gchar                 *info_hash;
	AxisDownloadItemState  state;
	gint64                 size_bytes;
	gint64                 done_bytes;
	gint64                 rate_bps;
	gint64                 eta_s;
	gchar                 *path;
	gchar                 *message;
	gchar                 *place;
	gchar                 *own_name;
	gchar                 *unreadable;
} AxisDownloadItem;

AxisDownloadItem *
axis_download_item_new(void);

void
axis_download_item_free(AxisDownloadItem *item);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDownloadItem, axis_download_item_free)

/* Whether @item can be used: an identifier, a name and a category of the
 * right forms.  Cleans its texts and clamps its figures in place. */
gboolean
axis_download_item_check(AxisDownloadItem *item);

/**
 * axis_download_magnet_parse:
 * @text: (nullable): a magnet link
 * @info_hash: (out) (optional) (transfer full): its info-hash, 40 lower-case
 *   hex digits (a base32 one is converted)
 * @problem: (out) (optional) (transfer none): why it is refused
 *
 * A magnet link is "magnet:?xt=urn:btih:" and 40 hexadecimal or 32 base32
 * characters, then nothing, or '&' and further parameters of URL
 * characters, in at most %AXIS_DOWNLOAD_MAGNET_MAX bytes.
 *
 * Returns: %TRUE if @text is such a link
 */
gboolean
axis_download_magnet_parse(
	const gchar  *text,
	gchar       **info_hash,
	const gchar **problem
);

/* @text cleaned for a name or a message: valid UTF-8, no control
 * characters, space collapsed, at most @max_chars characters; %NULL when
 * nothing is left. */
gchar *
axis_download_text_clean(
	const gchar *text,
	guint        max_chars
);

/* A category or label: 1 to 64 letters, digits, '.', '_' and '-', not
 * starting with a punctuation mark. */
gboolean
axis_download_category_valid(const gchar *text);

/* The longest base path stored, in bytes. */
#define AXIS_DOWNLOAD_BASE_PATH_MAX      128

/**
 * axis_download_base_path_normalize:
 * @text: (nullable): what the administrator typed
 * @normalized: (out) (optional) (transfer full): the path to store: "" for
 *   none, otherwise "/" and segments with no trailing "/"
 * @problem: (out) (optional) (transfer none): why it is refused, a fixed
 *   sentence fragment that holds none of @text
 *
 * A client's base path is the part of its address after the origin when it
 * is served under a path ("/sabnzbd").  It is accepted as "" (after
 * trimming), "/", or "/" and one or more segments of letters, digits, '.',
 * '_', '~' and '-', separated by single slashes and in at most
 * %AXIS_DOWNLOAD_BASE_PATH_MAX bytes; a trailing slash is dropped.  A
 * segment is never "." or "..", and there is no query, fragment, escape
 * ('%'), backslash or space: the text is only ever configuration, and what
 * is stored can be put in front of a path as it is.
 *
 * Returns: %TRUE if @text is acceptable
 */
gboolean
axis_download_base_path_normalize(
	const gchar  *text,
	gchar       **normalized,
	const gchar **problem
);

/* A kind's name: 1 to 16 lower-case letters and digits, starting with a
 * letter. */
gboolean
axis_download_kind_name_valid(const gchar *text);

/* A client's identifier: 1 to %AXIS_DOWNLOAD_REF_MAX bytes of printable
 * ASCII without spaces. */
gboolean
axis_download_ref_valid(const gchar *text);

/* A module's id ("movies"): as module ids are. */
gboolean
axis_download_module_valid(const gchar *text);

/* 40 lower-case hexadecimal digits. */
gboolean
axis_download_hash_valid(const gchar *text);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_MODEL_H */
