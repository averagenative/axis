/*
 * axis-db-private.h - shared internals of src/db; not for use elsewhere
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_DB_PRIVATE_H
#define AXIS_DB_PRIVATE_H

#include <glib.h>
#include <libpq-fe.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-pool.h"
#include "db/axis-db-result.h"

G_BEGIN_DECLS

/*
 * One pool slot.  The slot structs live as long as the pool.
 *
 * Fields marked (pool) are read and written only with the pool's mutex
 * held.  Fields marked (holder) belong to whichever thread holds the
 * connection, from acquire to release; while the slot is free nobody
 * touches them except, under the mutex, to check whether @pg is set.
 * The mutex hand-over at acquire and release orders the holders' accesses.
 */
struct _AxisDbConn
{
	AxisDbPool *pool;
	guint       index;

	gboolean    in_use;            /* (pool) acquired and not yet released */
	gboolean    releasing;         /* (pool) axis_db_pool_release() running */
	gpointer    owner;             /* (pool) the GThread holding it; read
	                                * atomically by the holder's checks */

	PGconn     *pg;                /* (holder) NULL: not connected */
	gboolean    close_on_release;  /* (holder) state unknown or settings
	                                * changed: close instead of reusing */
	gint64      idle_since;        /* (holder) monotonic time of release */
};

typedef enum
{
	AXIS_DB_WAIT_READY,
	AXIS_DB_WAIT_TIMEOUT,
	AXIS_DB_WAIT_FAILED
} AxisDbWait;

/* Waits until @pg's socket is readable (or writable, with @for_write) or
 * the monotonic @deadline passes (-1: no deadline).  Retries on EINTR.  On a
 * thread with an interrupt attached (axis_db_interrupt_attach()), a raised
 * interrupt ends the wait as a deadline that has passed, at once. */
AxisDbWait
axis_db_wait_socket(
	PGconn   *pg,
	gboolean  for_write,
	gint64    deadline
);

/* Whether the calling thread has an interrupt attached, and whether that
 * one is raised: for the waits that are not on a socket (a pool slot). */
gboolean
axis_db_thread_is_interruptible(void);

gboolean
axis_db_thread_is_interrupted(void);

/* Takes ownership of @res. */
AxisDbResult *
axis_db_result_new_take(PGresult *res);

/* The command tag ("COMMIT", "INSERT 0 1", ...), owned by @self. */
const gchar *
axis_db_result_get_command_tag(AxisDbResult *self);

/*
 * Runs @sql.  With @simple, through the simple query protocol (PQexec()
 * semantics: several statements allowed, no parameters); used only for
 * migration scripts.  Otherwise as axis_db_conn_exec().  @deadline is a
 * g_get_monotonic_time() value, or -1 to wait as long as it takes; when it
 * passes, the query is abandoned, the connection is marked to be closed on
 * release, and the call fails with AXIS_DB_ERROR_TIMEOUT.
 */
gboolean
axis_db_conn_run(
	AxisDbConn         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	gboolean            simple,
	gint64              deadline,
	AxisDbResult      **result,
	GError            **error
);

/* The connection will be closed, not reused, when it is released. */
void
axis_db_conn_set_close_on_release(AxisDbConn *self);

/* PQtransactionStatus() of the connection (PQTRANS_UNKNOWN if closed). */
PGTransactionStatusType
axis_db_conn_get_transaction_status(AxisDbConn *self);

/* Removes every occurrence of the pool's password from @text.  Returns a
 * newly allocated string; NULL for NULL. */
gchar *
axis_db_pool_scrub(
	AxisDbPool  *self,
	const gchar *text
);

/* The pool's URL as axis_config_redact_url() renders it.  Owned by the
 * pool. */
const gchar *
axis_db_pool_get_redacted_url(AxisDbPool *self);

/* Acquire with an absolute deadline (g_get_monotonic_time()) instead of the
 * acquire timeout. */
AxisDbConn *
axis_db_pool_acquire_until(
	AxisDbPool  *self,
	gint64       deadline,
	GError     **error
);

/*
 * Opens a server connection that is not one of the pool's slots, with the
 * pool's URL and session settings, for a caller that keeps it to itself
 * (axis-db-listener.c).  The attempt gives up at @deadline or at the
 * connect timeout, whichever comes first (-1: the connect timeout alone).
 * Fails as axis_db_pool_acquire() does when it has to connect.  The caller
 * owns the connection and PQfinish()es it; it does not count towards
 * axis_db_pool_get_connections_opened().
 */
PGconn *
axis_db_pool_connect_dedicated(
	AxisDbPool  *self,
	gint64       deadline,
	GError     **error
);

G_END_DECLS

#endif /* AXIS_DB_PRIVATE_H */
