/*
 * axis-compat-private.h - shared by the files of the compat module
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_COMPAT_PRIVATE_H
#define AXIS_COMPAT_PRIVATE_H

#include "compat/axis-compat.h"
#include "core/axis-http.h"
#include "compat/axis-compat-model.h"
#include "db/axis-db-conn.h"
#include "db/axis-db-result.h"

G_BEGIN_DECLS

struct _AxisCompat
{
	gint           refs;
	AxisDbPool    *pool;
	AxisStorage   *storage;
	AxisSettings  *settings;
	AxisEstimator *estimator;
	gint           enabled;	/* atomic */
	AxisCompatTuning tuning;
	gint           retention_run_s;	/* atomic: the monotonic second retention last ran, 0 for never */
};

typedef struct _AxisCompatKind AxisCompatKind;

/* The connection to one server as the settings describe it right now (a
 * copy).  The address, the secret and the header made of the secret are
 * those of one server kind's settings and travel together: the HTTP helper
 * is given this and nothing else, so no code can send one server's secret to
 * another server's address. */
typedef struct
{
	const AxisCompatKind *kind;
	AxisCompatAddress     address;
	gchar                *token;		/* the secret: only ever passed to the HTTP helper */
	gchar                *auth_value;	/* the request header's value made of it */
	gboolean              untrusted;
} AxisCompatConnection;

/**
 * AxisCompatKind:
 *
 * What differs between kinds of media server: the settings the connection
 * is read from, how the secret is sent, and the requests that read the
 * server's identity, libraries, devices and sessions (each returning the
 * model of axis-compat-model.h).  The collection jobs loop over the kinds
 * and call through this table; nothing else in the module names a server's
 * endpoints.
 */
struct _AxisCompatKind
{
	const gchar *name;		/* stored as server_kind: lower case letters and digits */
	const gchar *label;		/* shown: "Plex" */
	const gchar *product;		/* shown when the server gives no name */
	const gchar *address_setting;
	const gchar *token_setting;
	const gchar *untrusted_setting;
	/* The boolean setting that switches telling this server which folder
	 * changed off (on by default). */
	const gchar *notify_setting;
	/* The request header that carries the secret, and the text before and
	 * after the secret in its value. */
	const gchar *auth_header;
	const gchar *auth_prefix;
	const gchar *auth_suffix;
	/* Whether the server keeps a play history that can be imported. */
	gboolean     has_history;
	/* Whether a session does not say which library it is of, so that the
	 * item's path decides, and a session whose library cannot be decided
	 * counts toward every library of the server. */
	gboolean     library_from_path;
	/* %TRUE if @secret may be put in the header (no quote or backslash in
	 * a value that has them around it). */
	gboolean   (*secret_usable)(const gchar *secret);

	gboolean   (*fetch_identity)(AxisCompat *self, const AxisCompatConnection *connection,
	                             GCancellable *cancellable, AxisCompatIdentity *identity, GError **error);
	gboolean   (*fetch_libraries)(AxisCompat *self, const AxisCompatConnection *connection,
	                              GCancellable *cancellable, GPtrArray **libraries, GError **error);
	gboolean   (*fetch_devices)(AxisCompat *self, const AxisCompatConnection *connection,
	                            GCancellable *cancellable, GPtrArray **devices, GError **error);
	gboolean   (*fetch_sessions)(AxisCompat *self, const AxisCompatConnection *connection,
	                             GCancellable *cancellable, GPtrArray **sessions, GError **error);
	/* The file of the item a session plays, when the session does not say. */
	gboolean   (*fetch_item_file)(AxisCompat *self, const AxisCompatConnection *connection,
	                              GCancellable *cancellable, const gchar *item_id, gchar **file, GError **error);
	/* Tells the server what changed: for each item (AxisCompatNotifyItem,
	 * its @server_folder made from the path map) sets its result, and its note
	 * when it did not go.  Returns %FALSE, with @error, only when cancelled. */
	gboolean   (*notify)(AxisCompat *self, const AxisCompatConnection *connection,
	                     GCancellable *cancellable, GPtrArray *items, GError **error);
};

typedef enum
{
	AXIS_COMPAT_NOTIFY_UNDECIDED,
	AXIS_COMPAT_NOTIFY_SENT,
	AXIS_COMPAT_NOTIFY_UNMAPPED,	/* nothing was sent: no library holds the folder */
	AXIS_COMPAT_NOTIFY_FAILED	/* the request failed: tried again later */
} AxisCompatNotifyResult;

/* One folder of a batch, as a kind's notify() sees it. */
typedef struct
{
	gint64                 id;
	gchar                 *folder;		/* Axis's absolute path */
	gchar                 *server_folder;	/* the server's name for it */
	gint                   change;		/* AxisCompatChange */
	AxisCompatNotifyResult result;
	gchar                 *note;		/* why not, cleaned, never the secret */
} AxisCompatNotifyItem;

void
axis_compat_notify_item_free(AxisCompatNotifyItem *item);

/* The notify() of each kind (axis-compat-notify-plex.c, -jellyfin.c). */
gboolean
axis_compat_plex_notify(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                  *items,
	GError                    **error
);

gboolean
axis_compat_jellyfin_notify(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GPtrArray                  *items,
	GError                    **error
);

const AxisCompatKind *
axis_compat_kind_at(guint index);

/* %NULL for a name that is not a kind. */
const AxisCompatKind *
axis_compat_kind_find(const gchar *name);

void
axis_compat_connection_clear(AxisCompatConnection *connection);

/* Fails with %AXIS_COMPAT_ERROR_NOT_CONFIGURED without an address and a token. */
gboolean
axis_compat_connection_load(
	AxisCompat            *self,
	const AxisCompatKind  *kind,
	AxisCompatConnection  *connection,
	GError               **error
);

/* GET @path from the server: parses nothing, puts nothing in a message but
 * the helper's cleaned text. */
gboolean
axis_compat_fetch(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	const gchar                *path,
	const gchar *const         *headers,
	GCancellable               *cancellable,
	GBytes                    **body,
	GError                    **error
);

/* An error from a parser: the problem sentence, in our domain. */
void
axis_compat_set_server_error(
	GError      **error,
	const gchar  *what,
	const gchar  *problem
);

/* The server's identity, through its kind. */
gboolean
axis_compat_fetch_identity(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	AxisCompatIdentity         *identity,
	GError                    **error
);

/* The path map (AxisCompatPathEntry) of one kind of server, or of all when
 * @kind is %NULL, and the roots with their absolute paths
 * (AxisCompatRootRef), read on a connection the caller holds. */
gboolean
axis_compat_load_path_map(
	AxisDbConn  *conn,
	const gchar *kind,
	GPtrArray  **entries,
	GError     **error
);

gboolean
axis_compat_load_roots(
	AxisDbConn  *conn,
	GPtrArray  **roots,
	GError     **error
);

/* The key of a device in the tables the report builds: the kind and the
 * server's id of the device. */
gchar *
axis_compat_device_key(
	const gchar *kind,
	const gchar *client_id
);

/* Loads the capability rows (AxisCompatObsRow, axis-compat-report.h) of one
 * device (@kind and @client_id), or of all when @client_id is %NULL, into
 * @by_device (axis_compat_device_key() -> GPtrArray). */
gboolean
axis_compat_load_observations(
	AxisDbConn   *conn,
	const gchar  *kind,
	const gchar  *client_id,
	GHashTable   *by_device,
	GError      **error
);

/* A PostgreSQL text[] literal of @items (utf8 strings). */
gchar *
axis_compat_array_literal(GPtrArray *items);

/* Makes sure the state row of @kind exists. */
gboolean
axis_compat_ensure_state(
	AxisDbConn  *conn,
	const gchar *kind,
	GError     **error
);

/* Notes which server the records of @kind belong to and when collection
 * started.  When it is another server than before, what was recorded about
 * the old one (plays, libraries, sessions, devices) is removed first: ids of
 * libraries mean nothing across servers.  The other kinds' records are not
 * touched.  Inside the caller's transaction or not. */
gboolean
axis_compat_adopt_server(
	AxisDbConn               *conn,
	const gchar              *kind,
	const AxisCompatIdentity *identity,
	GError                  **error
);

/* Fetches the libraries and stores them (the folders as the server states
 * them); for a kind with no history, the devices too. */
gboolean
axis_compat_refresh_libraries(
	AxisCompat                 *self,
	const AxisCompatConnection *connection,
	GCancellable               *cancellable,
	GError                    **error
);

/* Records a failure of @kind: the cleaned message, the count and the
 * back-off. */
void
axis_compat_note_failure(
	AxisCompat   *self,
	const gchar  *kind,
	const GError *error,
	gboolean      history
);

G_END_DECLS

#endif /* AXIS_COMPAT_PRIVATE_H */
