/*
 * axis-download-kind.h - what a kind of download client is: a table of
 * functions, the protocol it takes, and the connection it is given
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A kind (SABnzbd, Deluge; the tests' "fixture") is a static
 * AxisDownloadKindInfo in a file of its own that speaks its client's API.
 * It never makes a request by itself and never sees an address: the layer
 * gives each function an AxisDownloadConn to one configured client, and
 * the kind asks it for GETs and POSTs of paths on that client's origin
 * (core/axis-http.h: the same fixed origin, redirects refused, a POST never
 * followed, bounded answers and deadlines).  The client's secret is the
 * kind's to place where its API wants it (a header, a login body), and
 * nowhere else: never in a path, unless the client's API allows nothing
 * else, and then in a POST's body rather than its query.  A session cookie
 * a client sets is kept by the connection, in memory, and sent back to it.
 *
 * What a kind returns is checked by the layer before it is used: an item
 * whose identifier, name or category is not of the right form is dropped
 * (axis_download_item_check()), a category other than Axis's is never
 * looked at, and an identifier Axis has no record of is never acted on.
 *
 * Errors: %AXIS_DOWNLOAD_ERROR_AUTH when the client refused the secret,
 * %AXIS_DOWNLOAD_ERROR_UNREACHABLE when it could not be reached,
 * %AXIS_DOWNLOAD_ERROR_REFUSED when it answered that it will not do what
 * was asked (a bad release, an unknown identifier),
 * %AXIS_DOWNLOAD_ERROR_UNREADABLE when its answer could not be read; what
 * a connection function reported is passed on as it is.  Messages never
 * hold the secret (the connection's are scrubbed; a kind writes its own
 * from fixed text and the client's cleaned words).
 *
 * Every function runs on a worker thread and may block on the network.
 * Calls to one client are made one at a time.
 */

#ifndef AXIS_DOWNLOAD_KIND_H
#define AXIS_DOWNLOAD_KIND_H

#include <gio/gio.h>
#include <glib.h>

#include "download/axis-download-model.h"

G_BEGIN_DECLS

#define AXIS_DOWNLOAD_ERROR (axis_download_error_quark())

/**
 * AxisDownloadError:
 * @AXIS_DOWNLOAD_ERROR_INVALID: the request is not one that can be made (a
 *   bad magnet link, a file too large, a release for the other protocol)
 * @AXIS_DOWNLOAD_ERROR_NOT_FOUND: no such client or download, or a download
 *   Axis did not add
 * @AXIS_DOWNLOAD_ERROR_NO_CLIENT: no enabled client takes the release
 * @AXIS_DOWNLOAD_ERROR_EXISTS: that release is being downloaded at that
 *   client already, or a client of that name exists
 * @AXIS_DOWNLOAD_ERROR_AUTH: the client refused the secret
 * @AXIS_DOWNLOAD_ERROR_UNREACHABLE: the client could not be reached or did
 *   not answer in time
 * @AXIS_DOWNLOAD_ERROR_REFUSED: the client answered that it will not
 * @AXIS_DOWNLOAD_ERROR_UNREADABLE: the client's answer could not be read
 * @AXIS_DOWNLOAD_ERROR_UNCERTAIN: a release was handed over, but whether the
 *   client took it is not known; it is recorded and will be matched against
 *   the client's list, and must not be sent again
 * @AXIS_DOWNLOAD_ERROR_BUSY: a download of the client is being sent or
 *   removed now; try again in a moment
 * @AXIS_DOWNLOAD_ERROR_CANCELLED: the daemon or the job is stopping
 * @AXIS_DOWNLOAD_ERROR_FAILED: anything else
 * @AXIS_DOWNLOAD_ERROR_GONE: (remove()) the client says it does not have that
 *   download: something else removed it already (a person, a plugin of the
 *   client, another program).  Nothing was removed now, and nothing of its
 *   files was deleted by this call.  Only a kind that can tell this from a
 *   refusal by the client's own answer says it; one that cannot reports what
 *   it can (SABnzbd's history delete answers the same either way)
 * @AXIS_DOWNLOAD_ERROR_NOT_OURS: (remove(), find()) the client has that
 *   torrent, but in another category or label than Axis's (a person moved it
 *   there, to keep it): it is not Axis's any more, and nothing was asked of
 *   the client about it
 */
typedef enum
{
	AXIS_DOWNLOAD_ERROR_INVALID,
	AXIS_DOWNLOAD_ERROR_NOT_FOUND,
	AXIS_DOWNLOAD_ERROR_NO_CLIENT,
	AXIS_DOWNLOAD_ERROR_EXISTS,
	AXIS_DOWNLOAD_ERROR_AUTH,
	AXIS_DOWNLOAD_ERROR_UNREACHABLE,
	AXIS_DOWNLOAD_ERROR_REFUSED,
	AXIS_DOWNLOAD_ERROR_UNREADABLE,
	AXIS_DOWNLOAD_ERROR_UNCERTAIN,
	AXIS_DOWNLOAD_ERROR_BUSY,
	AXIS_DOWNLOAD_ERROR_CANCELLED,
	AXIS_DOWNLOAD_ERROR_FAILED,
	AXIS_DOWNLOAD_ERROR_GONE,
	AXIS_DOWNLOAD_ERROR_NOT_OURS
} AxisDownloadError;

GQuark
axis_download_error_quark(void);

/* One configured client, as a kind's function talks to it. */
typedef struct _AxisDownloadConn AxisDownloadConn;

/**
 * axis_download_conn_post:
 * @path: an absolute path on the client's origin, with an optional query
 *   that holds no secret.  A client served under a base path (set by the
 *   administrator, "/sabnzbd") is asked for that prefix and this path: the
 *   kind names its own path ("/api") and never sees the prefix
 * @content_type: the body's media type
 * @body: the body
 * @headers: (nullable): further request headers, name and value pairs
 *   ended by %NULL
 * @answer: (out) (transfer full): the body of a 2xx answer
 *
 * Fails with %AXIS_DOWNLOAD_ERROR_AUTH for a 401 or 403,
 * %AXIS_DOWNLOAD_ERROR_UNREACHABLE when the client could not be reached or
 * did not answer in time, %AXIS_DOWNLOAD_ERROR_UNREADABLE for an answer too
 * large, %AXIS_DOWNLOAD_ERROR_CANCELLED, and %AXIS_DOWNLOAD_ERROR_FAILED for
 * any other status or a redirect (never followed).
 *
 * Returns: %TRUE on a 2xx answer
 */
gboolean
axis_download_conn_post(
	AxisDownloadConn    *conn,
	const gchar         *path,
	const gchar         *content_type,
	GBytes              *body,
	const gchar *const  *headers,
	GBytes             **answer,
	GError             **error
);

/* As axis_download_conn_post(), a GET. */
gboolean
axis_download_conn_get(
	AxisDownloadConn    *conn,
	const gchar         *path,
	const gchar *const  *headers,
	GBytes             **answer,
	GError             **error
);

/* The client's secret, for the kind to place where its API wants it.
 * Never log it, never put it in a message.  (nullable): none is set. */
const gchar *
axis_download_conn_get_secret(AxisDownloadConn *conn);

/* The client's user name, for a login that takes one (qBittorrent's).  Not a
 * secret, but it is Axis's configuration: send it to the client only.
 * (nullable): none is set. */
const gchar *
axis_download_conn_get_username(AxisDownloadConn *conn);

/* The client's own address, as configured: scheme, host and port, with no
 * base path and no slash at the end.  For a header the client's API wants to
 * see (qBittorrent's Referer and Origin), never for a request: requests are
 * made by the connection, to this address only. */
const gchar *
axis_download_conn_get_address(AxisDownloadConn *conn);

/* The status of the last answer (0 for none). */
guint
axis_download_conn_get_last_status(AxisDownloadConn *conn);

/* The first bytes of the body of the last answer when it was a 403 ("" for
 * any other answer).  The client's own words: to be compared with, never put
 * in a message. */
const gchar *
axis_download_conn_get_last_refusal(AxisDownloadConn *conn);

/* Whether the connection holds a session cookie of the client's. */
gboolean
axis_download_conn_has_session(AxisDownloadConn *conn);

/* Forgets the session cookies (before logging in again). */
void
axis_download_conn_forget_session(AxisDownloadConn *conn);

/* What messages call the client: its configured name. */
const gchar *
axis_download_conn_get_label(AxisDownloadConn *conn);

/* A kind's list says, by this, that it is not whole (it reached a limit of
 * what it reads, or it holds an entry whose identifier cannot be read): what
 * it does not show is not taken for absent where that matters (gone or failed
 * downloads, what may be deleted in the client's folder for Axis).  An entry
 * whose identifier is read and nothing else is returned instead, its item's
 * @unreadable set: it does not make the list not whole. */
void
axis_download_conn_set_list_partial(AxisDownloadConn *conn);

/* Axis's category (or label) at the client, as configured: what a torrent of
 * Axis's carries there.  A removal acts only on a torrent that still carries
 * it (%AXIS_DOWNLOAD_ERROR_NOT_OURS otherwise). */
const gchar *
axis_download_conn_get_category(AxisDownloadConn *conn);

/**
 * AxisDownloadRelease:
 * @protocol: what it is for
 * @file: (nullable): the .nzb or .torrent as it was fetched, to be uploaded
 *   unchanged
 * @file_name: (nullable): a name for the uploaded file ("<release>.nzb")
 * @magnet: (nullable): a magnet link (checked), instead of a file
 * @name: the release's name
 * @info_hash: (nullable): a torrent's info-hash, 40 lower-case hex digits
 * @address: (nullable): an address (axis_download_address_valid()), instead
 *   of a file or a magnet link: what a built-in kind fetches
 *
 * What add() hands to the client: exactly one of @file, @magnet and
 * @address.
 */
typedef struct
{
	AxisDownloadProtocol  protocol;
	GBytes               *file;
	const gchar          *file_name;
	const gchar          *magnet;
	const gchar          *name;
	const gchar          *info_hash;
	const gchar          *address;
} AxisDownloadRelease;

/**
 * AxisDownloadTestResult:
 * @version: (nullable): what the client says it is
 * @category_exists: whether the category is configured at the client: 1,
 *   0, or -1 when the kind cannot tell
 * @note: (nullable): anything else the administrator should know
 */
typedef struct
{
	gchar *version;
	gint   category_exists;
	gchar *note;
} AxisDownloadTestResult;

void
axis_download_test_result_clear(AxisDownloadTestResult *result);

typedef struct _AxisDownloadKindInfo AxisDownloadKindInfo;

/**
 * AxisDownloadKindInfo:
 * @name: 1 to 16 lower-case letters and digits, as stored
 * @label: its name for people ("SABnzbd")
 * @protocol: what it takes
 * @secret_label: what its secret is called ("API key", "Password")
 * @category_label: what Axis's tag is called there ("Category", "Label")
 * @address_help: (nullable): the form's words about its address
 * @username_label: (nullable): what its user name is called ("User name"),
 *   when its login takes one; %NULL for a kind that has none.  Last in the
 *   structure, after @data
 * @test: reaches the client, authenticates, and fills @result (its
 *   version, whether @category exists)
 * @add: hands @release to the client in @category at @priority; returns
 *   the client's identifier of it in @client_ref
 * @list: the client's downloads in @category, as AxisDownloadItem (only
 *   that category, if the client can filter; the layer filters again); with
 *   @category %NULL, everything the client has, in every category or label
 *   (for whether anything of the client's is at a path), with no state
 *   remembered for tracking
 * @remove: removes the download @client_ref, and its files when
 *   @delete_data; %AXIS_DOWNLOAD_ERROR_GONE when the client's own answer says
 *   it does not have it
 * @data: the kind's own, for its functions
 * @category_folder: (nullable): where the client puts the finished
 *   downloads of @category, as the client's own absolute path: the folder
 *   of that category alone, never one shared with other categories (nor the
 *   client's own folder for every download).  Fails with
 *   %AXIS_DOWNLOAD_ERROR_REFUSED (and the reason, for the page) when the
 *   client's configuration gives the category no folder of its own, or none
 *   Axis can use, and with the usual errors when the client cannot be asked.
 *   Read only.  A Usenet kind is asked after an import (Axis removes the
 *   job's leftover folder, which is inside it) and before one, for a job
 *   reported by a file inside its folder; a torrent kind only for a torrent
 *   its client no longer has after its import, whose leftover files Axis
 *   then removes
 * @find: (nullable): a torrent kind's look-up of the torrent @client_ref in
 *   every category or label, read only: %TRUE with *@item (its category or
 *   label as the client says it, "" for none) when the client has it, %TRUE
 *   with %NULL when its own answer says it has not, %FALSE with the usual
 *   errors when it cannot be asked; last in the structure.  The remove() of
 *   such a kind acts only on a torrent in Axis's category or label
 *   (axis_download_conn_get_category()), and fails with
 *   %AXIS_DOWNLOAD_ERROR_NOT_OURS, asking nothing more, for one in another
 * @builtin: the kind is a part of Axis itself, not a program behind an
 *   address (the YouTube kind, which runs yt-dlp): it has one client, which
 *   the layer makes for it (axis_downloads_builtin_client()) and no
 *   administrator adds, changes or deletes, with no address, secret or path
 *   mapping; its functions get a connection that can make no request
 *   (axis_download_conn_get() and _post() are not for it).  A path it reports
 *   for a finished download is Axis's own: "/" and the volume's id, "/" and
 *   the path on that volume ("/3/YouTube/A Channel/a file.mkv"), checked
 *   through the volume guard like any other.  That file is in the library
 *   already (the kind put it there), so its download is recorded as imported
 *   the moment tracking sees it finished, never as completed: it is not one
 *   waiting to be imported.  Its label is a client's name no administrator
 *   can give a client of their own.  Last in the structure
 */
struct _AxisDownloadKindInfo
{
	const gchar          *name;
	const gchar          *label;
	AxisDownloadProtocol  protocol;
	const gchar          *secret_label;
	const gchar          *category_label;
	const gchar          *address_help;

	gboolean (*test)(const AxisDownloadKindInfo *self, AxisDownloadConn *conn, const gchar *category,
	                 AxisDownloadTestResult *result, GError **error);
	gboolean (*add)(const AxisDownloadKindInfo *self, AxisDownloadConn *conn, const AxisDownloadRelease *release,
	                const gchar *category, AxisDownloadPriority priority, gchar **client_ref, GError **error);
	gboolean (*list)(const AxisDownloadKindInfo *self, AxisDownloadConn *conn, const gchar *category,
	                 GPtrArray **items, GError **error);
	gboolean (*remove)(const AxisDownloadKindInfo *self, AxisDownloadConn *conn, const gchar *client_ref,
	                   gboolean delete_data, GError **error);

	gpointer data;

	const gchar *username_label;

	gboolean (*category_folder)(const AxisDownloadKindInfo *self, AxisDownloadConn *conn, const gchar *category,
	                            gchar **folder, GError **error);

	gboolean (*find)(const AxisDownloadKindInfo *self, AxisDownloadConn *conn, const gchar *client_ref,
	                 AxisDownloadItem **item, GError **error);

	gboolean builtin;
};

G_END_DECLS

#endif /* AXIS_DOWNLOAD_KIND_H */
