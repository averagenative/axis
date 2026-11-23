/*
 * axis-compat-notify.h - telling the media servers which folder changed
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A media server finds a new file by itself only in time (a watcher that may
 * be off, or its periodic scan).  After an import, an upgrade or a removal
 * the daemon therefore tells each configured server the one folder that
 * changed, so that it looks there and nowhere else:
 *
 *  - Plex: the library section whose location holds the folder, then a
 *    refresh of that section limited to the folder (a GET).  A folder no
 *    section holds is not sent and is shown as not mapped to a library;
 *    there is no fall-back to scanning every section.
 *  - Jellyfin: its "media updated" request (a POST, the one write a
 *    media-server connection can make: core/axis-http.h).
 *
 * What happened is recorded first (axis_compat_notify_record()), in the
 * compat_notify table (migration 0033), per kind of server that is set up
 * and not switched off, and the work returns: nothing is waited for and no
 * failure here reaches the import.  The notify job (queue "compat") sends a
 * row once its folder has been quiet for compat.notify_quiet; events for one
 * folder merge (created and then modified is created, anything and then
 * deleted is deleted); a failed request is tried again after one, two, four,
 * eight and sixteen minutes, then the row is dropped with a note.  Axis's
 * folder is made into the server's with the path map
 * (axis-compat-path.h) when it is sent; a folder it does not reach is never
 * sent.  The servers' own scanning stays the backstop.
 *
 * Every function that touches the database or the network blocks: call it
 * from a worker thread.
 */

#ifndef AXIS_COMPAT_NOTIFY_H
#define AXIS_COMPAT_NOTIFY_H

#include <glib.h>

#include "compat/axis-compat.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

/* A folder that keeps changing is sent at the latest this long after its
 * first change. */
#define AXIS_COMPAT_NOTIFY_MAX_WAIT_SECONDS  300

/* A request that fails is tried again this many times in all, the first
 * after a minute and each wait twice the one before. */
#define AXIS_COMPAT_NOTIFY_MAX_ATTEMPTS      6
#define AXIS_COMPAT_NOTIFY_FIRST_RETRY_SECONDS 60

/* The notify job takes at most this many due rows of a server at a time. */
#define AXIS_COMPAT_NOTIFY_BATCH             200

/* Finished rows are kept this long (the page shows the latest of them). */
#define AXIS_COMPAT_NOTIFY_KEEP_SECONDS      (30 * 86400)

/* A request that no path mapping reached is asked again when a mapping is
 * added or removed and now reaches it, if its folder last changed at most
 * this long ago (a week: a film imported over a weekend and mapped on the
 * Monday).  Older ones stay as they are: the server's own scan has most
 * likely found those files by then, and a new mapping of a whole library
 * should not set off a request for every import of the past month.  "Ask
 * again" on the page takes any one of them. */
#define AXIS_COMPAT_NOTIFY_RETRY_UNMAPPED_SECONDS (7 * 86400)

typedef enum
{
	AXIS_COMPAT_CHANGE_CREATED,
	AXIS_COMPAT_CHANGE_MODIFIED,
	AXIS_COMPAT_CHANGE_DELETED
} AxisCompatChange;

/* "created", "modified", "deleted" (as stored). */
const gchar *
axis_compat_change_name(AxisCompatChange change);

/**
 * axis_compat_change_merge:
 * @earlier: what happened to a folder before
 * @later: what happened to it since
 *
 * Anything and then deleted is deleted; deleted and then anything else is
 * modified (it is there again); otherwise created if either was, else
 * modified.
 */
AxisCompatChange
axis_compat_change_merge(
	AxisCompatChange earlier,
	AxisCompatChange later
);

/**
 * axis_compat_notify_record:
 * @pool: the database
 * @settings: where the servers' settings are read from; %NULL records nothing
 * @folder: Axis's absolute path of the folder that changed (a title's folder,
 *   never a file and never a whole root)
 * @change: what happened
 * @error: (nullable): a database failure
 *
 * Notes the change for each kind of server that is set up (an address and a
 * token) and not switched off (compat.notify_plex, compat.notify_jellyfin),
 * merging it into a row still waiting for the same server and folder.  A
 * folder that is not a clean absolute path is ignored.  Inside or outside a
 * transaction of the caller's: it uses a connection of its own.
 *
 * Returns: %TRUE unless the database failed
 */
gboolean
axis_compat_notify_record(
	AxisDbPool        *pool,
	AxisSettings      *settings,
	const gchar       *folder,
	AxisCompatChange   change,
	GError           **error
);

/**
 * axis_compat_notify_record_import:
 * @import_id: an import that has just been recorded as imported
 *
 * Records the title folder of each file the import placed, as created, or as
 * modified for an upgrade: the root's path joined with the first part of each
 * placed file's folder.  Best effort by design: the caller logs a failure and
 * goes on.
 */
gboolean
axis_compat_notify_record_import(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	gint64         import_id,
	GError       **error
);

/**
 * AxisCompatNotifySummary:
 *
 * What one run of the notify job did, in rows.
 */
typedef struct
{
	guint sent;
	guint unmapped;
	guint failed;		/* will be tried again */
	guint dropped;
	guint skipped;
	guint removed;		/* old finished rows deleted */
} AxisCompatNotifySummary;

/**
 * axis_compat_notify_run:
 * @force: send every waiting row now, whatever its due time (tests)
 *
 * Sends what is due, kind by kind, one request per server per batch (Plex's
 * refresh names one folder, so it takes one request a folder).  A server that
 * does not answer makes only its own rows fail.
 *
 * Returns: %FALSE only when cancelled or the database failed
 */
gboolean
axis_compat_notify_run(
	AxisCompat              *self,
	gboolean                 force,
	GCancellable            *cancellable,
	AxisCompatNotifySummary *summary,
	GError                 **error
);

gboolean
axis_compat_run_notify_job(
	AxisCompat      *self,
	AxisJobContext  *context,
	GError         **error
);

/**
 * axis_compat_notify_retry_unmapped:
 * @kind: (nullable): the kind of server whose path map changed; %NULL for
 *   every kind
 * @count: (out) (optional): how many requests wait again
 *
 * After the path map changed: the requests that no mapping reached
 * (unmapped), whose folder last changed within
 * %AXIS_COMPAT_NOTIFY_RETRY_UNMAPPED_SECONDS, and that the map reaches now,
 * wait again (pending, due now, their tries counted from nothing): for each
 * folder only its latest request, and only when that one is unmapped (a
 * folder told since, or with a request waiting, is left as it is).  The
 * notify job sends them.  Older ones are not touched.
 */
gboolean
axis_compat_notify_retry_unmapped(
	AxisCompat   *self,
	const gchar  *kind,
	guint        *count,
	GError      **error
);

/**
 * AxisCompatAskAgain:
 * @AXIS_COMPAT_ASK_AGAIN_WAITING: the request waits again
 * @AXIS_COMPAT_ASK_AGAIN_ALREADY: a request for the same server and folder
 *   is waiting already, and is sent for it; nothing changed
 */
typedef enum
{
	AXIS_COMPAT_ASK_AGAIN_WAITING,
	AXIS_COMPAT_ASK_AGAIN_ALREADY
} AxisCompatAskAgain;

/**
 * axis_compat_notify_ask_again:
 * @id: a request that was not sent: unmapped, dropped or skipped
 *
 * A person's "Ask again": the request waits again, due now, its tries
 * counted from nothing, as if its folder had just changed.  Fails with
 * %AXIS_COMPAT_ERROR_NOT_FOUND for no such request and
 * %AXIS_COMPAT_ERROR_INVALID for one that was sent or is waiting.
 */
gboolean
axis_compat_notify_ask_again(
	AxisCompat          *self,
	gint64               id,
	AxisCompatAskAgain  *outcome,
	GError             **error
);

/* ── What the page shows ────────────────────────────────────────── */

typedef struct
{
	gint64   id;
	gchar   *server_kind;
	gchar   *folder;
	gchar   *change;
	gchar   *state;
	gint     events;
	gint     attempts;
	gint64   first_us;
	gint64   last_us;
	gint64   due_us;
	gint64   done_us;	/* 0 while pending */
	gchar   *sent_folder;	/* or NULL */
	gchar   *note;		/* or NULL */
} AxisCompatNotifyInfo;

void
axis_compat_notify_info_free(AxisCompatNotifyInfo *info);

/* The latest @limit rows, newest first (AxisCompatNotifyInfo). */
gboolean
axis_compat_notify_list(
	AxisCompat  *self,
	guint        limit,
	GPtrArray  **rows,
	GError     **error
);

/**
 * AxisCompatNotifyRoot:
 * @reach: 0 when nothing says where the server sees the root (no path map
 *   entry reaches it), 1 when the server's name for it is in no library the
 *   server has, 2 when a library holds it
 */
typedef struct
{
	gchar *server_kind;
	gint64 root_id;
	gchar *root_name;
	gchar *root_path;
	gchar *media_type;
	gchar *server_path;	/* or NULL */
	gchar *library;		/* the library's title, or NULL */
	gint   reach;
} AxisCompatNotifyRoot;

void
axis_compat_notify_root_free(AxisCompatNotifyRoot *root);

/**
 * axis_compat_notify_roots:
 *
 * For each kind of server that is set up, each root that holds movies or TV
 * (what the servers are told about), with where the server sees it and the
 * library that holds it (AxisCompatNotifyRoot).
 */
gboolean
axis_compat_notify_roots(
	AxisCompat  *self,
	GPtrArray  **roots,
	GError     **error
);

G_END_DECLS

#endif /* AXIS_COMPAT_NOTIFY_H */
