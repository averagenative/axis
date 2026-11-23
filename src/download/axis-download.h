/*
 * axis-download.h - the downloads layer: the configured download clients,
 * the record of every download Axis started, sending, tracking, removing
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Axis downloads nothing itself: it drives the download clients that are
 * already running.  The one exception is a built-in kind
 * (AxisDownloadKindInfo.builtin: the YouTube kind, which runs yt-dlp), which
 * is driven through this layer like any client, so that its downloads are
 * recorded, followed and removed the same way.  The promises this layer keeps:
 *
 *  - Axis touches only its own.  Every download it sends carries the
 *    client's category (default "axis") and is recorded (0019_downloads.sql)
 *    with the client's identifier.  Tracking looks only at the client's
 *    downloads in that category, and of those only at the ones recorded;
 *    removal refuses an identifier there is no record of.  A download added
 *    by anything else is never listed, changed or removed.
 *  - A release is sent once.  The record is written ('sending') before the
 *    client is called, and given the client's identifier after.  When the
 *    call fails and the request certainly did not reach the client, the
 *    record is marked failed; when it may have (a timeout after the request
 *    went out, an answer that cannot be read), it stays 'sending', and so
 *    does one left by a daemon that stopped in between.  Tracking matches
 *    such a record against the client's list by info-hash or name, adopting
 *    the client's identifier, or marks it failed when the client does not
 *    have it: it is never sent again by this layer.
 *  - A client that cannot be reached never fails a download: its downloads
 *    keep their last known state, and the pages show how old that is.
 *  - A completed download's location is recorded only when the client's
 *    path maps (download/axis-download-path.h) to a place beneath a known
 *    volume and the volume guard finds a file or folder there, reached
 *    without a symbolic link or another filesystem.  Otherwise the download
 *    waits, and the problem is shown on it and on its client.
 *  - A client's secret is read only to make its connection, is sent only to
 *    its address (in a header or a body), and is never in a page, a log
 *    line, a job row or a message.  Session cookies stay in memory.
 *
 * Tracking: one job (download.track, on the "downloads" queue, every
 * minute) runs for most of a minute and polls each enabled client in turn
 * when it is due: every downloads.active_interval (5 s) while the client
 * has a download in progress, every downloads.idle_interval (1 min)
 * otherwise, one list call each time.  Progress is reported to the
 * listener (the web part publishes it live).
 *
 * Threads: everything that touches the database or a client blocks: call
 * it from a worker thread.  Create and free the layer on the main thread.
 */

#ifndef AXIS_DOWNLOAD_H
#define AXIS_DOWNLOAD_H

#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "download/axis-download-kind.h"
#include "download/axis-download-model.h"
#include "download/axis-download-path.h"
#include "jobs/axis-job-context.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_DOWNLOAD_MODULE_ID          "downloads"
#define AXIS_DOWNLOAD_QUEUE              "downloads"
#define AXIS_DOWNLOAD_TRACK_KIND         "download.track"
#define AXIS_DOWNLOAD_TRACK_SCHEDULE     "download.track"
#define AXIS_DOWNLOAD_TRACK_INTERVAL_S   60

/* How long one tracking job runs before it hands over to the next. */
#define AXIS_DOWNLOAD_TRACK_RUN_S        55

#define AXIS_DOWNLOAD_SETTING_ACTIVE_INTERVAL  "downloads.active_interval"
#define AXIS_DOWNLOAD_SETTING_IDLE_INTERVAL    "downloads.idle_interval"
#define AXIS_DOWNLOAD_SETTING_STALL_AFTER      "downloads.stall_after"
#define AXIS_DOWNLOAD_SETTING_GONE_AFTER       "downloads.gone_after"

/* The default category Axis gives its downloads. */
#define AXIS_DOWNLOAD_DEFAULT_CATEGORY   "axis"

/* One request's deadline at a client. */
#define AXIS_DOWNLOAD_REQUEST_TIMEOUT_MS 15000

/* The largest answer read from a client (a list of many downloads). */
#define AXIS_DOWNLOAD_MAX_ANSWER_BYTES   (8 * 1024 * 1024)

/* A 'sending' record younger than this is not reconciled yet: the client
 * may still be taking the release in. */
#define AXIS_DOWNLOAD_RECONCILE_AFTER_S  30

/* At most this many clients can be configured. */
#define AXIS_DOWNLOAD_MAX_CLIENTS        32

typedef struct _AxisDownloads AxisDownloads;

/**
 * axis_downloads_new:
 * @pool: the database, which must outlive the layer
 * @storage: the volumes, for checking completed downloads' locations
 * @settings: (nullable): the settings store (%NULL: the defaults)
 *
 * Returns: (transfer full): the layer, reference counted, with no kind yet
 */
AxisDownloads *
axis_downloads_new(
	AxisDbPool   *pool,
	AxisStorage  *storage,
	AxisSettings *settings
);

AxisDownloads *
axis_downloads_ref(AxisDownloads *self);

/* The database the layer was made with (for pages that read beside it). */
AxisDbPool *
axis_downloads_get_pool(AxisDownloads *self);

void
axis_downloads_unref(AxisDownloads *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDownloads, axis_downloads_unref)

/* Cancels requests in flight and refuses new ones (after the job queue
 * has stopped). */
void
axis_downloads_stop(AxisDownloads *self);

/* The module's settings, ended by an entry whose key is %NULL. */
const AxisSettingInfo *
axis_downloads_get_setting_table(void);

/**
 * axis_downloads_add_kind:
 * @info: (transfer none): the kind; it must outlive the layer
 *
 * Before the layer is used.  Fails with %AXIS_DOWNLOAD_ERROR_INVALID when
 * its name is not valid, it lacks a function or a label, or a kind of that
 * name exists.
 */
gboolean
axis_downloads_add_kind(
	AxisDownloads               *self,
	const AxisDownloadKindInfo  *info,
	GError                     **error
);

guint
axis_downloads_n_kinds(AxisDownloads *self);

const AxisDownloadKindInfo *
axis_downloads_kind_at(
	AxisDownloads *self,
	guint          index
);

/* (nullable) */
const AxisDownloadKindInfo *
axis_downloads_find_kind(
	AxisDownloads *self,
	const gchar   *name
);

/**
 * AxisDownloadTuning:
 * @request_timeout_ms: one request's deadline (0: the default)
 *
 * For tests.
 */
typedef struct
{
	guint request_timeout_ms;
} AxisDownloadTuning;

void
axis_downloads_set_tuning(
	AxisDownloads            *self,
	const AxisDownloadTuning *tuning
);

/**
 * AxisDownloadFault:
 * @AXIS_DOWNLOAD_FAULT_NONE: none
 * @AXIS_DOWNLOAD_FAULT_AFTER_RECORD: add() stops after the record is
 *   written, before the client is called, as a daemon that died there
 *   would
 * @AXIS_DOWNLOAD_FAULT_AFTER_SEND: add() stops after the client accepted
 *   the release, before its identifier is recorded
 *
 * For tests: the two places where the daemon stopping matters for sending
 * once.  add() then fails with %AXIS_DOWNLOAD_ERROR_FAILED and leaves
 * everything as a crash would; a new layer on the same database is the
 * restarted daemon.
 */
typedef enum
{
	AXIS_DOWNLOAD_FAULT_NONE,
	AXIS_DOWNLOAD_FAULT_AFTER_RECORD,
	AXIS_DOWNLOAD_FAULT_AFTER_SEND
} AxisDownloadFault;

void
axis_downloads_set_fault(
	AxisDownloads     *self,
	AxisDownloadFault  fault
);

/* ── Clients ────────────────────────────────────────────────────── */

/**
 * AxisDownloadPathMapInfo:
 * @client_prefix: the client's path
 * @volume_id: the volume
 * @volume_name: (nullable): its name, for pages
 * @rel_path: the path on it ("" for its top)
 */
typedef struct
{
	gchar  *client_prefix;
	gint64  volume_id;
	gchar  *volume_name;
	gchar  *rel_path;
} AxisDownloadPathMapInfo;

void
axis_download_path_map_info_free(AxisDownloadPathMapInfo *map);

/**
 * AxisDownloadClientInfo:
 * @id, @name, @kind, @address, @accept_untrusted, @category, @priority,
 *   @enabled: as configured
 * @base_path: the path its own paths are served under ("" for none, else
 *   "/" and segments, no trailing slash); put in front of every request
 *   made to it
 * @plain_http: its address is plain http (the page warns)
 * @secret_set: a secret is stored (never what it is)
 * @username: (nullable): the user name the client's login takes (qBittorrent's);
 *   not a secret, shown on the client's page
 * @state: what the last contact found
 * @state_message: (nullable): the last error, safe to show
 * @version: (nullable): what the client said it is
 * @category_exists: 1, 0, or -1 for not known
 * @last_checked_us: the last contact, 0 for never
 * @last_contact_us: the last contact that worked, 0 for never
 * @problem: (nullable): a problem with its downloads (an unmapped path)
 * @maps: (element-type AxisDownloadPathMapInfo): its path mappings
 * @n_active: its downloads in progress
 * @n_waiting: its downloads finished at the client whose location is not
 *   accepted
 * @n_leftovers: what the Leftovers page (the import module's) last found in
 *   the client's folder for Axis that no download and no import accounts for
 * @partial_since_us: since when (real time) the client's list of Axis's
 *   downloads has not been whole, as this process polled it; 0 when it has
 *   been whole (or not polled yet)
 */
typedef struct
{
	gint64                   id;
	gchar                   *name;
	gchar                   *kind;
	gchar                   *address;
	gboolean                 accept_untrusted;
	gchar                   *category;
	AxisDownloadPriority     priority;
	gboolean                 enabled;
	gboolean                 plain_http;
	gboolean                 secret_set;
	AxisDownloadClientState  state;
	gchar                   *state_message;
	gchar                   *version;
	gint                     category_exists;
	gint64                   last_checked_us;
	gint64                   last_contact_us;
	gchar                   *problem;
	gint64                   created_us;
	gint64                   updated_us;
	GPtrArray               *maps;
	guint                    n_active;
	guint                    n_waiting;
	gchar                   *base_path;
	gchar                   *username;
	guint                    n_leftovers;
	gint64                   partial_since_us;
} AxisDownloadClientInfo;

void
axis_download_client_info_free(AxisDownloadClientInfo *client);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDownloadClientInfo, axis_download_client_info_free)

/* Every client, by name. */
gboolean
axis_downloads_list_clients(
	AxisDownloads  *self,
	GPtrArray     **clients,
	GError        **error
);

/* Fails with %AXIS_DOWNLOAD_ERROR_NOT_FOUND. */
gboolean
axis_downloads_get_client(
	AxisDownloads           *self,
	gint64                   id,
	AxisDownloadClientInfo **client,
	GError                 **error
);

/**
 * AxisDownloadClientSpec:
 * @name: as typed
 * @kind: a registered kind's name (only for a new client; a client's kind
 *   does not change)
 * @address: as typed: an http or https origin
 * @accept_untrusted: accept a certificate that does not verify
 * @category: as typed (empty: "axis")
 * @priority: what its downloads get
 * @enabled: whether it is used
 * @secret: (nullable): a new secret; %NULL or "" keeps the stored one (an
 *   edit) or sets none (a new client)
 * @base_path: (nullable): as typed (empty or %NULL: none); checked by
 *   axis_download_base_path_normalize(), and stored without a trailing slash
 * @username: (nullable): the user name for a kind whose login takes one: %NULL
 *   keeps the stored one (an edit) or sets none (a new client), "" removes it;
 *   at most 128 characters, no control characters
 *
 * What an administrator submitted.
 */
typedef struct
{
	const gchar          *name;
	const gchar          *kind;
	const gchar          *address;
	gboolean              accept_untrusted;
	const gchar          *category;
	AxisDownloadPriority  priority;
	gboolean              enabled;
	const gchar          *secret;
	const gchar          *base_path;
	const gchar          *username;
} AxisDownloadClientSpec;

/**
 * axis_downloads_save_client:
 * @id: the client to change, or 0 for a new one
 * @id_out: (out) (optional): its id
 *
 * Fails with %AXIS_DOWNLOAD_ERROR_INVALID (the message says which field,
 * without its value), %AXIS_DOWNLOAD_ERROR_EXISTS for a name in use, and
 * %AXIS_DOWNLOAD_ERROR_NOT_FOUND.  A changed address, kind or secret
 * forgets the client's session.
 */
gboolean
axis_downloads_save_client(
	AxisDownloads                 *self,
	gint64                         id,
	const AxisDownloadClientSpec  *spec,
	gint64                        *id_out,
	GError                       **error
);

/**
 * axis_downloads_delete_client:
 *
 * Forgets a client, its secret and its mappings; its finished downloads
 * keep their history under its name.  Refused with
 * %AXIS_DOWNLOAD_ERROR_BUSY while it has a download in progress.
 */
gboolean
axis_downloads_delete_client(
	AxisDownloads  *self,
	gint64          id,
	GError        **error
);

/**
 * axis_downloads_add_path_map:
 * @client_prefix: the client's path as typed
 * @volume_id: a known volume
 * @rel_path: (nullable): the path on it as typed
 *
 * Fails with %AXIS_DOWNLOAD_ERROR_INVALID for a path that is not valid, a
 * volume that is not known, or too many mappings, and
 * %AXIS_DOWNLOAD_ERROR_EXISTS for a prefix mapped already.
 */
gboolean
axis_downloads_add_path_map(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_prefix,
	gint64          volume_id,
	const gchar    *rel_path,
	GError        **error
);

gboolean
axis_downloads_remove_path_map(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_prefix,
	GError        **error
);

/**
 * axis_downloads_test_client:
 * @result: (out) (optional): what the client said
 *
 * Reaches the client, authenticates and asks for its version and whether
 * its category exists; when one of its downloads has finished at the
 * client, checks that its path maps.  Stores what was found on the client
 * (its state, version, message) whatever it was.  Fails only for a
 * database error or %AXIS_DOWNLOAD_ERROR_NOT_FOUND: a client that does not
 * answer is a result, not a failure.
 */
gboolean
axis_downloads_test_client(
	AxisDownloads           *self,
	gint64                   id,
	AxisDownloadTestResult  *result,
	GError                 **error
);

/**
 * axis_downloads_builtin_client:
 * @kind: the name of a registered built-in kind
 * @client_id: (out): its one client
 *
 * The client of a built-in kind, made the first time it is asked for (named
 * by the kind's label, with no address and no secret).  It is not among the
 * clients an administrator configures: axis_downloads_list_clients() and
 * axis_downloads_count_clients() leave it out, axis_downloads_get_client()
 * does not find it, and it cannot be saved over or deleted.  Fails with
 * %AXIS_DOWNLOAD_ERROR_INVALID for a kind that is not a built-in one, and
 * %AXIS_DOWNLOAD_ERROR_EXISTS when a client an administrator added has the
 * kind's label as its name.
 */
gboolean
axis_downloads_builtin_client(
	AxisDownloads  *self,
	const gchar    *kind,
	gint64         *client_id,
	GError        **error
);

/* ── Downloads ──────────────────────────────────────────────────── */

/**
 * AxisDownloadRequest:
 * @client_id: the client, or 0 for the first enabled client (by id) that
 *   takes @protocol
 * @protocol: what the release is
 * @file: (nullable): the .nzb or .torrent, at most
 *   %AXIS_DOWNLOAD_MAX_RELEASE_BYTES; uploaded unchanged
 * @magnet: (nullable): a magnet link, instead of a file (torrents)
 * @release_name: the release's name
 * @title_id: the stored title it is for (meta_title), or 0
 * @title_label: (nullable): what it is for, for pages ("Hearth (1995)")
 * @module: (nullable): the module that asked ("movies")
 * @address: (nullable): an address, instead of a file or a magnet link, for
 *   a protocol a built-in kind takes (%AXIS_DOWNLOAD_YOUTUBE: a video's
 *   address); it goes to that kind's one client, whatever @client_id says
 *
 * A release to send: exactly one of @file, @magnet and @address.
 */
typedef struct
{
	gint64                client_id;
	AxisDownloadProtocol  protocol;
	GBytes               *file;
	const gchar          *magnet;
	const gchar          *release_name;
	gint64                title_id;
	const gchar          *title_label;
	const gchar          *module;
	const gchar          *address;
} AxisDownloadRequest;

/**
 * axis_downloads_add:
 * @download_id: (out) (optional): the record, whenever one was written
 *   (also when this fails after writing it)
 *
 * Sends a release to a client, once (see the promises above).  Fails with
 * %AXIS_DOWNLOAD_ERROR_INVALID for a request that cannot be made (a magnet
 * link that is not one, a file too large or empty, a .torrent that is not
 * one, the wrong protocol for the client), %AXIS_DOWNLOAD_ERROR_NO_CLIENT,
 * %AXIS_DOWNLOAD_ERROR_EXISTS when the same torrent is in progress at that
 * client already; after the record is written, with the client's error
 * (the record is then failed, the release not sent), or with
 * %AXIS_DOWNLOAD_ERROR_UNCERTAIN (the record stays and is reconciled;
 * never send the release again because of this).
 *
 * Returns: %TRUE when the client took it and its identifier is recorded
 */
gboolean
axis_downloads_add(
	AxisDownloads              *self,
	const AxisDownloadRequest  *request,
	gint64                     *download_id,
	GError                    **error
);

/**
 * axis_downloads_remove:
 * @delete_data: also delete its files at the client
 *
 * Removes a recorded download from its client and records that.  Refused
 * with %AXIS_DOWNLOAD_ERROR_NOT_FOUND for no such record, or one with no
 * identifier of the client's (being sent, or never sent), and
 * %AXIS_DOWNLOAD_ERROR_INVALID for one already removed or gone.  When the
 * client says it does not have it, fails with %AXIS_DOWNLOAD_ERROR_GONE and
 * records it as gone (nothing was removed or deleted at the client).  With
 * @delete_data, a download whose client may have kept its files (a Usenet
 * job: SABnzbd keeps a completed job's folder; one the client no longer had)
 * has its leftover recorded to be looked at (folder_removal pending), which
 * the import module's hourly cleanup deletes under its folder rules; never
 * for an imported download, whose import looks after it.
 *
 * The place rule, which no caller can leave out: before a torrent's client
 * is asked to remove it with its data, it is asked, in the same turn, where
 * the torrent is now.  When that place maps to a volume and is the volume's
 * top, a library root, inside one (a downloads root aside) or holds one
 * (axis_download_place_in_library()), the torrent is removed WITHOUT its
 * data, and that its files were left, and why, is recorded on the download
 * (folder_removal "left").  A place that is not known to be one of those
 * (the client does not answer or list it, no path mapping reaches it) is
 * removed with its data, as asked.  A Usenet job is never looked at.
 */
gboolean
axis_downloads_remove(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        delete_data,
	GError        **error
);

/* axis_downloads_remove(), telling what the place rule did.  *@left:
 * (out) (optional) (transfer full) (nullable): when the torrent was removed
 * without its data though @delete_data was asked, what was recorded:
 * "Removed from its client without its data. Its files were left where they
 * are: WHY". */
gboolean
axis_downloads_remove_full(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        delete_data,
	gchar         **left,
	GError        **error
);

/* Removes @download_id without its data, as a confirmation that said its
 * files are in a library folder promised: asks the client where the torrent
 * is all the same, in the same turn, and when it is still such a place
 * records that and why as axis_downloads_remove_full() does (*@left).
 * Nothing is ever deleted through this. */
gboolean
axis_downloads_remove_leaving(
	AxisDownloads  *self,
	gint64          download_id,
	gchar         **left,
	GError        **error
);

/**
 * axis_downloads_removal_leaves:
 * @why: (out) (transfer full) (nullable): when a removal with its data would
 *   leave the files now, the reason, a sentence ("its client says the
 *   torrent's files are at ..., which is inside a library folder. ...")
 *
 * What the place rule of axis_downloads_remove() would do now, for a
 * confirmation: asks the client where the torrent is and removes nothing.
 * *@why stays %NULL for a Usenet job, a download that is not at a client, a
 * client that does not answer and a place not known to be a library folder.
 * Fails for no such record or a database error.
 */
gboolean
axis_downloads_removal_leaves(
	AxisDownloads  *self,
	gint64          download_id,
	gchar         **why,
	GError        **error
);

/**
 * axis_download_place_in_library:
 * @place: (nullable): where the client says a torrent's files are now (its
 *   own path)
 * @mapped: whether @place maps to a volume through the client's path
 *   mappings; then @rel (the path on it) and @roots
 * @roots: (element-type utf8) (nullable): the library roots of that volume,
 *   as pairs: each root's path on the volume, then "d" for a downloads root
 *   and "" for any other
 * @why: (out) (transfer full) (nullable): when %TRUE, the reason, a sentence
 *
 * %TRUE only when the place is known to be one a client must not delete at:
 * it maps to a volume and is the volume's top, a library root, inside one (a
 * downloads root aside, which is where downloads are) or holds one.  A place
 * that is not named, is not a plain full path or is reached by no mapping is
 * %FALSE (not known).  Pure.
 */
gboolean
axis_download_place_in_library(
	const gchar      *place,
	gboolean          mapped,
	const gchar      *rel,
	const GPtrArray  *roots,
	gchar           **why
);

/**
 * axis_downloads_note_left:
 * @text: why, in sentences, for the Downloads page
 *
 * Records that Axis left the files of @download_id where they are, and why
 * (folder_removal "left", which no cleanup looks at again): a torrent Axis
 * did not ask its client to delete because its files are in a library
 * folder now.  Nothing is asked of a client.
 */
gboolean
axis_downloads_note_left(
	AxisDownloads  *self,
	gint64          download_id,
	const gchar    *text,
	GError        **error
);

/**
 * axis_downloads_mark_imported:
 * @conn: a connection, in the import's own transaction
 * @marked: (out): whether the record was completed (or imported) and now is
 *   imported
 *
 * The import pipeline's mark: a completed download has been taken into the
 * library.  Nothing is sent to the client.  Call
 * axis_downloads_notify_changed() after the transaction is committed.
 */
gboolean
axis_downloads_mark_imported(
	AxisDbConn  *conn,
	gint64       download_id,
	gboolean    *marked,
	GError     **error
);

/* Tells the listener that a record changed (after a commit of the caller's
 * own). */
void
axis_downloads_notify_changed(
	AxisDownloads *self,
	gint64         download_id
);

/**
 * axis_downloads_remove_imported:
 *
 * After its import was recorded: asks the client to remove a download with
 * its data (its files were hardlinked or copied into the library, whose names
 * are not the client's).  The record stays imported.  A torrent is removed
 * only when @torrents is true (the administrator has not chosen to leave
 * torrents seeding); refused with %AXIS_DOWNLOAD_ERROR_INVALID for a
 * download that is not imported, or is a torrent and @torrents is false, and
 * %AXIS_DOWNLOAD_ERROR_NOT_FOUND for one with no identifier at its client;
 * then the client's error, %AXIS_DOWNLOAD_ERROR_GONE when the client says it
 * does not have it (something else removed it, and the client deleted none
 * of its files now), %AXIS_DOWNLOAD_ERROR_NOT_OURS for a torrent the client
 * has in another category or label (a person moved it there: nothing is
 * asked of it).  Like every removal it goes through the client and takes
 * only what the client holds.  The place rule of axis_downloads_remove()
 * holds here too: a torrent its client says is in a library folder now is
 * removed without its data.
 */
gboolean
axis_downloads_remove_imported(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        torrents,
	GError        **error
);

/* axis_downloads_remove_imported(), telling what the place rule did: *@left
 * as axis_downloads_remove_full()'s (nothing is recorded on the download,
 * which stays imported: its import says it). */
gboolean
axis_downloads_remove_imported_full(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean        torrents,
	gchar         **left,
	GError        **error
);

/**
 * axis_downloads_release:
 * @was_gone: (out): the client said at once that it had no such torrent
 *   (something else removed it), so nothing was removed now
 *
 * Before an import moves a torrent's files: asks the client to remove the
 * torrent WITHOUT its data, then asks again until the client itself says it
 * no longer has it (%AXIS_DOWNLOAD_ERROR_GONE: Deluge's "not in session",
 * qBittorrent's list without it), so that the files are moved only once the
 * client has let go of them and will not look for them.  The record is not
 * changed.  Refused with %AXIS_DOWNLOAD_ERROR_INVALID for a download that is
 * not a completed (or imported) torrent, %AXIS_DOWNLOAD_ERROR_NOT_FOUND for
 * one with no identifier at its client; %AXIS_DOWNLOAD_ERROR_REFUSED when
 * the client still has it after being asked three times;
 * %AXIS_DOWNLOAD_ERROR_NOT_OURS when the client has it in another category or
 * label (left alone: its files must not be moved); otherwise the client's
 * error (it cannot be reached: the caller waits).
 */
gboolean
axis_downloads_release(
	AxisDownloads  *self,
	gint64          download_id,
	gboolean       *was_gone,
	GError        **error
);

/**
 * axis_downloads_set_folder_warning:
 * @text: (nullable): why the client's folder for Axis is not its own (another
 *   client keeps Axis's downloads there too), or %NULL when it is
 *
 * Kept in memory with the client, as the Leftovers listing (hourly, and when
 * the page is opened) and the client's test last found it, and said on the
 * client's page with its problems from the next look at the client on.
 */
void
axis_downloads_set_folder_warning(
	AxisDownloads *self,
	gint64         client_id,
	const gchar   *text
);

/**
 * AxisDownloadNeighbour:
 * @client_id: another client than the one asked about
 * @name: its name
 * @folder: (nullable): its folder for Axis, as it says
 *   (axis_downloads_category_folder()), normalised; %NULL when it keeps Axis's
 *   downloads in no folder of their own, its kind cannot say, or it could not
 *   be asked
 * @folder_failure: (nullable): why @folder is not known when the client could
 *   not be asked
 * @folder_none: (nullable): why @folder is not known when the client answered
 *   (it keeps Axis's downloads in no folder of their own, or in none Axis can
 *   use, or its kind cannot say): then whether it is this folder cannot be
 *   told either
 * @items: (nullable): everything it lists now, in every category or label
 *   (axis_downloads_client_items_full()), %NULL when it could not be asked
 * @unreadable: as axis_downloads_client_items_full() says
 * @unreadable_why: (nullable): which entry, and why, when @unreadable
 * @failure: (nullable): why @items could not be had
 */
typedef struct
{
	gint64     client_id;
	gchar     *name;
	gchar     *folder;
	gchar     *folder_failure;
	gchar     *folder_none;
	GPtrArray *items;
	guint      unreadable;
	gchar     *unreadable_why;
	gchar     *failure;
} AxisDownloadNeighbour;

void
axis_download_neighbour_free(AxisDownloadNeighbour *neighbour);

/**
 * axis_downloads_neighbours:
 * @client_id: the client whose folder is looked at
 * @volume: the volume of that place
 * @rel: the place on it
 * @out: (out) (transfer full) (element-type AxisDownloadNeighbour): the other
 *   clients configured (switched off in Axis too: the program still runs)
 *   that could have files in or around @rel: with a path mapping onto @volume
 *   that holds @rel, or lies inside it.  Each is asked where it keeps Axis's
 *   downloads and everything it has now, in every category or label
 *
 * For what accounts for an entry of a client's folder for Axis (the Leftovers
 * page, a download's folder after its import): a download of another client in
 * that folder is not a leftover either.  Read only.
 */
gboolean
axis_downloads_neighbours(
	AxisDownloads  *self,
	gint64          client_id,
	gint64          volume,
	const gchar    *rel,
	GPtrArray     **out,
	GError        **error
);

/**
 * axis_downloads_find_torrent:
 * @found: (out): the client has the torrent @client_ref, in any category
 * @ours: (out): it has it in Axis's category or label
 * @category: (out) (transfer full) (optional): the category or label it has
 *   it in, when found ("" for none)
 *
 * Asks client @client_id, read only, whether it has a torrent, wherever it
 * keeps it (a torrent a person moved to another category is the person's
 * now).  Fails with %AXIS_DOWNLOAD_ERROR_INVALID for a kind that cannot look
 * a torrent up, and with the client's error when it cannot be asked.
 */
gboolean
axis_downloads_find_torrent(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_ref,
	gboolean       *found,
	gboolean       *ours,
	gchar         **category,
	GError        **error
);

/**
 * axis_downloads_category_folder:
 * @client_id: a configured client
 * @folder: (out) (transfer full): where the client puts the finished
 *   downloads of Axis's category: the client's own absolute path, normalised
 *
 * Asks the client (its kind's category_folder(): SABnzbd's category, Deluge's
 * label, qBittorrent's category).  Read only.  Fails with
 * %AXIS_DOWNLOAD_ERROR_NOT_FOUND for no such client,
 * %AXIS_DOWNLOAD_ERROR_INVALID for a kind that cannot say,
 * %AXIS_DOWNLOAD_ERROR_REFUSED (with the reason, for the page)
 * when the client's configuration gives the category no folder of its own or
 * none Axis can use, and with the client's error when it cannot be asked.
 */
gboolean
axis_downloads_category_folder(
	AxisDownloads  *self,
	gint64          client_id,
	gchar         **folder,
	GError        **error
);

/**
 * axis_downloads_client_items_full:
 * @items: (out) (transfer full): AxisDownloadItem, checked as the layer
 *   checks them (those it would not use are left out)
 * @unreadable: (out) (optional): how much of the answer Axis could not use:
 *   items left out (an entry that cannot be read, or not used), items whose
 *   place could not be read, and one more when the kind says its list is not
 *   whole (it reached the most it reads, or holds an entry whose identifier
 *   cannot be read).  Not 0: what the client has is not all known
 * @why: (out) (optional) (transfer full): when @unreadable is not 0, which
 *   entry (its identifier and name, as far as they can be read) and why, for
 *   a person: the first of them
 *
 * Everything client @client_id has now, in every category or label (another
 * program's downloads too, a torrent a person moved to another category
 * among them), in every state, with each item's name and, where the kind can
 * tell, its place (where its files are now, in progress too).  Read only;
 * nothing is recorded.  For whether anything of the client's is at a path
 * (the Leftovers page, a download's folder after its import).
 *
 * Returns: %TRUE when the client answered
 */
gboolean
axis_downloads_client_items_full(
	AxisDownloads  *self,
	gint64          client_id,
	GPtrArray     **items,
	guint          *unreadable,
	gchar         **why,
	GError        **error
);

/* axis_downloads_client_items_full() without @why. */
gboolean
axis_downloads_client_items(
	AxisDownloads  *self,
	gint64          client_id,
	GPtrArray     **items,
	guint          *unreadable,
	GError        **error
);

/**
 * axis_downloads_map_client_path:
 * @client_path: a path of the client's
 * @volume_id: (out): the volume, when mapped
 * @rel_path: (out) (transfer full): the path on it, when mapped
 * @prefix: (out) (transfer full) (optional): the client prefix of the mapping
 *   used, when mapped
 * @problem: (out) (transfer full): why not, when it is not mapped
 *
 * Client @client_id's path mappings as they are now, applied to
 * @client_path (text only: nothing is looked at on a volume).
 *
 * Returns: %TRUE when mapped; %FALSE with @problem when not, or with @error
 *   for a database failure
 */
gboolean
axis_downloads_map_client_path(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_path,
	gint64         *volume_id,
	gchar         **rel_path,
	gchar         **prefix,
	gchar         **problem,
	GError        **error
);

/**
 * axis_downloads_remove_ref:
 *
 * As axis_downloads_remove(), by the client's identifier: refused with
 * %AXIS_DOWNLOAD_ERROR_NOT_FOUND, sending nothing, unless a live record of
 * that client has it.  Nothing Axis did not add can be removed.
 */
gboolean
axis_downloads_remove_ref(
	AxisDownloads  *self,
	gint64          client_id,
	const gchar    *client_ref,
	gboolean        delete_data,
	GError        **error
);

/**
 * AxisDownloadInfo:
 * @id: the record
 * @client_id: its client (0 when the client was removed)
 * @client_name: the client's name
 * @client_state: the client's state now
 * @client_contact_us: the client's last contact that worked
 * @client_ref: (nullable): the client's identifier
 * @info_hash: (nullable)
 * @protocol, @release_name, @title_id, @title_label, @module, @state: as
 *   recorded
 * @size_bytes, @done_bytes, @rate_bps, @eta_s: -1 when not known
 * @client_path: (nullable): where the client says its files are
 * @volume_id: the volume of its location (0 for none)
 * @volume_name: (nullable): that volume's name
 * @rel_path: (nullable): its location on it
 * @message: (nullable): the client's words (why it failed)
 * @problem: (nullable): Axis's (why it waits)
 * @stalled: a torrent that has not grown for downloads.stall_after
 * @seen_us: when the client last listed it (its state is that old)
 * @created_us, @sent_us, @updated_us, @completed_us, @finished_us: 0 for
 *   none
 * @folder_removal, @folder_removal_text: (nullable): for a download removed
 *   from its client with its data outside an import, what became of the
 *   files the client may have left (pending, waiting, done, gone, left; the
 *   import module's cleanup decides), and the words for the page
 */
typedef struct
{
	gint64                   id;
	gint64                   client_id;
	gchar                   *client_name;
	AxisDownloadClientState  client_state;
	gint64                   client_contact_us;
	gchar                   *client_ref;
	gchar                   *info_hash;
	AxisDownloadProtocol     protocol;
	gchar                   *release_name;
	gint64                   title_id;
	gchar                   *title_label;
	gchar                   *module;
	AxisDownloadState        state;
	gint64                   size_bytes;
	gint64                   done_bytes;
	gint64                   rate_bps;
	gint64                   eta_s;
	gchar                   *client_path;
	gint64                   volume_id;
	gchar                   *volume_name;
	gchar                   *rel_path;
	gchar                   *message;
	gchar                   *problem;
	gboolean                 stalled;
	gint64                   seen_us;
	gint64                   created_us;
	gint64                   sent_us;
	gint64                   updated_us;
	gint64                   completed_us;
	gint64                   finished_us;
	gchar                   *folder_removal;
	gchar                   *folder_removal_text;
} AxisDownloadInfo;

void
axis_download_info_free(AxisDownloadInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDownloadInfo, axis_download_info_free)

/* The fraction done, 0 to 1, or -1 when it cannot be told. */
gdouble
axis_download_info_progress(const AxisDownloadInfo *info);

/* Fails with %AXIS_DOWNLOAD_ERROR_NOT_FOUND. */
gboolean
axis_downloads_get(
	AxisDownloads     *self,
	gint64             id,
	AxisDownloadInfo **info,
	GError           **error
);

/**
 * AxisDownloadList:
 * @AXIS_DOWNLOAD_LIST_ACTIVE: sending, queued, downloading or paused (and
 *   finished at the client but waiting for its location), oldest first
 * @AXIS_DOWNLOAD_LIST_RECENT: the others, most recently changed first
 * @AXIS_DOWNLOAD_LIST_ARRIVED: completed or imported, most recent first
 */
typedef enum
{
	AXIS_DOWNLOAD_LIST_ACTIVE,
	AXIS_DOWNLOAD_LIST_RECENT,
	AXIS_DOWNLOAD_LIST_ARRIVED
} AxisDownloadList;

gboolean
axis_downloads_list(
	AxisDownloads     *self,
	AxisDownloadList   which,
	guint              limit,
	GPtrArray        **downloads,
	GError           **error
);

/* The records of @ids (gint64) that exist, in the order of @ids. */
gboolean
axis_downloads_get_many(
	AxisDownloads  *self,
	GArray         *ids,
	GPtrArray     **downloads,
	GError        **error
);

/* How many clients are configured (enabled or not). */
gboolean
axis_downloads_count_clients(
	AxisDownloads  *self,
	guint          *count,
	GError        **error
);

/* ── Tracking ───────────────────────────────────────────────────── */

/**
 * AxisDownloadPollReport:
 * @reached: the client answered the list
 * @listed: items of Axis's category it listed (recorded or not)
 * @updated: records changed
 * @completed: records that became completed
 * @failed: records that became failed
 * @gone: records that became gone
 * @adopted: 'sending' records matched to the client's copy
 * @not_found: 'sending' records the client did not have, marked failed
 * @waiting: completed at the client, location not accepted
 * @ignored: items in Axis's category with no record (never touched)
 * @partial: the list was not whole (the kind cut it at the most it reads, or
 *   it held an entry whose identifier Axis could not read): a record it did
 *   not show was neither counted as missing nor failed
 * @unseen: records the list did not show while it was not whole
 * @held: records kept as they were because the client's entry for them
 *   could not be read but by its identifier (of Axis's category, or of one
 *   that could not be read); the other records were judged as ever.  A
 *   'sending' record the list does not show is held too while such an entry
 *   might be it
 */
typedef struct
{
	gboolean reached;
	guint    listed;
	guint    updated;
	guint    completed;
	guint    failed;
	guint    gone;
	guint    adopted;
	guint    not_found;
	guint    waiting;
	guint    ignored;
	gboolean partial;
	guint    unseen;
	guint    held;
} AxisDownloadPollReport;

/**
 * axis_downloads_poll_client:
 * @cancellable: (nullable)
 * @report: (out) (optional)
 *
 * Polls one client now: one list call, then the records are updated.  A
 * client that cannot be reached is recorded as such and changes no
 * download.  Fails only for a database error, cancellation or
 * %AXIS_DOWNLOAD_ERROR_NOT_FOUND.
 */
gboolean
axis_downloads_poll_client(
	AxisDownloads           *self,
	gint64                   client_id,
	GCancellable            *cancellable,
	AxisDownloadPollReport  *report,
	GError                 **error
);

/**
 * axis_downloads_run_tracking:
 * @context: (nullable): the job; without one, every followed client is
 *   polled once
 * @run_ms: how long to keep polling (0: %AXIS_DOWNLOAD_TRACK_RUN_S)
 *
 * The tracking job: polls each client it follows whenever it is due until
 * @run_ms has passed or the job is told to stop.  It follows every enabled
 * client, and a client switched off while it still has downloads in
 * progress (switched off means that no new download is sent to it; those it
 * has are followed to the end).  With no client to follow (none set up, or
 * the last one switched off with nothing in progress) it ends at once,
 * holding no place on its queue: the next run, a minute later, finds a
 * client added meanwhile.
 *
 * Returns: %FALSE only for a database error or cancellation by a user
 */
gboolean
axis_downloads_run_tracking(
	AxisDownloads   *self,
	AxisJobContext  *context,
	guint            run_ms,
	GError         **error
);

/**
 * AxisDownloadsListener:
 * @ids: (element-type gint64): the records that changed
 * @set_changed: a record started or stopped being in progress (lists
 *   should be fetched again), or one was added or removed
 * @user_data: (closure)
 *
 * Called on the thread that changed them (a job's, a page's worker),
 * after the change is committed.
 */
typedef void (*AxisDownloadsListener)(
	GArray   *ids,
	gboolean  set_changed,
	gpointer  user_data
);

/* One listener; %NULL removes it.  Thread-safe: once this returns with
 * %NULL, the old listener is not called again. */
void
axis_downloads_set_listener(
	AxisDownloads         *self,
	AxisDownloadsListener  listener,
	gpointer               user_data
);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_H */
