/*
 * axis-download-private.h - shared by the files of the downloads layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-download.c        lifetime, kinds, settings, the listener
 *   axis-download-conn.c   connections to one client: the session kept for
 *                          it, the calls, the mapping of errors
 *   axis-download-store.c  the database: clients, secrets, mappings, records
 *   axis-download-send.c   adding, removing, testing
 *   axis-download-track.c  polling: state mapping, completion through the
 *                          volume guard, reconciliation, the job
 */

#ifndef AXIS_DOWNLOAD_PRIVATE_H
#define AXIS_DOWNLOAD_PRIVATE_H

#include "core/axis-http.h"
#include "download/axis-download.h"

G_BEGIN_DECLS

/*
 * What the layer keeps in memory about one client: the session its calls
 * share (its cookies), made again when the client's configuration
 * changes, and when it is next due for a poll.  Calls to a client are
 * made one at a time (@call), so the session is used by one thread at a
 * time as core/axis-http.h asks.
 */
typedef struct
{
	gint              refs;		/* atomic */
	gint64            id;
	GMutex            call;
	AxisHttpSession  *session;	/* (call) */
	gint64            session_stamp;	/* (call) the configuration it was made for */
	gint64            next_poll_us;	/* (owner lock) */
	gchar            *folder_warning;	/* (owner lock) its folder for Axis is shared, as last found; NULL */
	gint64            partial_since_us;	/* (owner lock) since when its list of Axis's downloads has not been whole;
						 * 0 while it is */
} AxisDownloadSlot;

struct _AxisDownloads
{
	gint                   refs;	/* atomic */
	AxisDbPool            *pool;
	AxisStorage           *storage;	/* owned reference */
	AxisSettings          *settings;	/* owned reference, nullable */
	GPtrArray             *kinds;	/* const AxisDownloadKindInfo * */
	AxisDownloadTuning     tuning;
	gint                   fault;	/* atomic AxisDownloadFault */
	GCancellable          *cancel;
	gint                   stopped;	/* atomic */
	GMutex                 lock;
	GHashTable            *clients;	/* (lock) gint64 * -> AxisDownloadSlot * */
	GHashTable            *sending;	/* (lock) gint64 * download id: being sent by this process */
	GMutex                 listener_lock;
	AxisDownloadsListener  listener;	/* (listener_lock) */
	gpointer               listener_data;	/* (listener_lock) */
};

/* A client's configuration as a connection needs it. */
typedef struct
{
	AxisDownloadClientInfo *info;
	gchar                  *secret;	/* wiped */
	gint64                  stamp;	/* changes when the address, kind or secret does */
} AxisDownloadTarget;

void
axis_download_target_clear(AxisDownloadTarget *target);

struct _AxisDownloadConn
{
	AxisDownloads              *owner;
	const AxisDownloadKindInfo *kind;
	AxisHttpSession            *session;	/* borrowed from the client's state */
	gchar                      *secret;	/* borrowed from the target */
	const gchar                *label;
	const gchar                *base_path;	/* borrowed from the target: "" or "/a/b", put before every path */
	const gchar                *address;	/* borrowed from the target: the client's origin, as stored */
	const gchar                *username;	/* borrowed from the target: nullable */
	guint                       last_status;
	gchar                       last_refusal[AXIS_HTTP_REFUSAL_BYTES + 1];	/* a 403's body, in part; "" if none */
	gboolean                    sent;	/* a request of this connection may have reached the client */
	gboolean                    list_partial;	/* the kind's last list is not whole */
	const gchar                *category;	/* borrowed from the target: Axis's category or label there */
};

/* ── axis-download.c ─────────────────────────────────────────────── */

AxisDownloadSlot *
axis_download_slot_get(
	AxisDownloads *self,
	gint64         id
);

void
axis_download_slot_unref(AxisDownloadSlot *slot);

/* What axis_downloads_set_folder_warning() last said of client @client_id
 * (transfer full), or %NULL. */
gchar *
axis_download_folder_warning(
	AxisDownloads *self,
	gint64         client_id
);

/* Records that the latest list of client @client_id's downloads was whole or
 * not: the time it stopped being whole is kept until it is whole again. */
void
axis_download_note_partial(
	AxisDownloads *self,
	gint64         client_id,
	gboolean       partial
);

/* Since when (real time, microseconds) client @client_id's list has not been
 * whole, as last polled in this process; 0 when it has been whole. */
gint64
axis_download_partial_since(
	AxisDownloads *self,
	gint64         client_id
);

/* Forgets the session of client @id (its configuration changed). */
void
axis_download_forget_client(
	AxisDownloads *self,
	gint64         id
);

/* The listener, if any, with @ids (it may be empty with @set_changed). */
void
axis_download_notify(
	AxisDownloads *self,
	GArray        *ids,
	gboolean       set_changed
);

/* A setting's value, or @fallback without a settings store. */
gint64
axis_download_setting_duration(
	AxisDownloads *self,
	const gchar   *key,
	gint64         fallback
);

gint64
axis_download_setting_integer(
	AxisDownloads *self,
	const gchar   *key,
	gint64         fallback
);

/* What a failed contact with a client means for it: its state and a
 * sentence for its page (the error's own words, already free of secrets). */
gchar *
axis_download_contact_problem(
	const GError            *failure,
	AxisDownloadClientState *state
);

/* Whether download @id is being sent by this process now. */
gboolean
axis_download_is_sending(
	AxisDownloads *self,
	gint64         id
);

void
axis_download_mark_sending(
	AxisDownloads *self,
	gint64         id,
	gboolean       sending
);

/* ── axis-download-conn.c ────────────────────────────────────────── */

typedef gboolean (*AxisDownloadCallFunc)(
	AxisDownloads              *self,
	const AxisDownloadTarget   *target,
	const AxisDownloadKindInfo *kind,
	AxisDownloadConn           *conn,
	gpointer                    data,
	GError                    **error
);

/*
 * Loads client @id's configuration and secret, takes its call lock, makes
 * (or reuses) its session and runs @func with a connection to it.  Fails
 * with %AXIS_DOWNLOAD_ERROR_NOT_FOUND for no such client, and with
 * %AXIS_DOWNLOAD_ERROR_INVALID when its kind is not one this daemon has.
 * *@sent (optional) says whether a request may have reached the client.
 */
gboolean
axis_download_with_client(
	AxisDownloads         *self,
	gint64                 id,
	GCancellable          *cancellable,
	AxisDownloadCallFunc   func,
	gpointer               data,
	gboolean              *sent,
	GError               **error
);

/* ── axis-download-store.c ───────────────────────────────────────── */

/* Every client (or the one of @id, when not 0), by name. */
gboolean
axis_download_store_clients(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **clients,
	GError     **error
);

/* The secret of client @id and when it was set (in microseconds); %NULL
 * when none is set.  The only reader of the secrets table. */
gboolean
axis_download_store_secret(
	AxisDbConn  *conn,
	gint64       id,
	gchar      **secret,
	gint64      *set_us,
	GError     **error
);

/* Stores what a contact with client @id found. */
gboolean
axis_download_store_client_state(
	AxisDbConn              *conn,
	gint64                   id,
	AxisDownloadClientState  state,
	const gchar             *message,
	const gchar             *version,
	gint                     category_exists,
	gboolean                 contact,
	GError                 **error
);

gboolean
axis_download_store_client_problem(
	AxisDbConn   *conn,
	gint64        id,
	const gchar  *problem,
	GError      **error
);

/* The mappings of client @id, as AxisDownloadPathMap. */
gboolean
axis_download_store_maps(
	AxisDbConn  *conn,
	gint64       id,
	GPtrArray  **maps,
	GError     **error
);

/* Records by the clause @where (a constant of this layer's own, with $1
 * for @param), in @order. */
gboolean
axis_download_store_records(
	AxisDbConn   *conn,
	const gchar  *where,
	const gchar  *param,
	const gchar  *order,
	guint         limit,
	GPtrArray   **downloads,
	GError      **error
);

/* "123" for SQL parameters. */
gchar *
axis_download_int_text(gint64 value);

/* ── axis-download-track.c ───────────────────────────────────────── */

/*
 * Whether @client_path is a place for a completed download: mapped by
 * @maps, beneath a volume the guard finds online, a regular file or a
 * directory reached without a symbolic link or another filesystem.  When
 * not, *@problem says why (a sentence fragment without the path), and the
 * download waits.  With @maps %NULL, @client_path is a built-in kind's own
 * path ("/<volume id>/<path on the volume>", AxisDownloadKindInfo.builtin):
 * nothing is mapped, and the place is checked the same way.
 */
gboolean
axis_download_locate(
	AxisDownloads    *self,
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	gchar           **problem
);

/* As axis_download_locate(), with the inode of what is at the place (the
 * file or the folder), recorded with the location so that a leftover of the
 * download can be told from something else at its place later. */
gboolean
axis_download_locate_full(
	AxisDownloads    *self,
	const GPtrArray  *maps,
	const gchar      *client_path,
	gint64           *volume_id,
	gchar           **rel_path,
	guint64          *inode,
	gchar           **problem
);

G_END_DECLS

#endif /* AXIS_DOWNLOAD_PRIVATE_H */
