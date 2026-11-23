/*
 * axis-db-pool.h - a fixed-size pool of PostgreSQL connections
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The pool owns a fixed number of connection slots.  A thread acquires a
 * connection, uses it (axis-db-conn.h) and releases it; while it is held
 * no other thread can get it.  When every connection is held, acquire
 * waits, in arrival order, until one is released or the acquire timeout
 * expires.  All functions here are thread-safe.
 *
 * Connections are opened on demand, apart from the first, which
 * axis_db_pool_new() opens to prove the server is reachable and recent
 * enough.  A connection that has gone bad is closed when it is released
 * and replaced on a later acquire.  On acquire, a connection that has been
 * idle for a while (or whose socket shows the server has said something)
 * is checked with a round trip first, and re-established once if it turns
 * out to be dead.
 *
 * Everything here blocks the calling thread.  In debug builds, acquiring
 * on the thread that is running the default main context logs a warning.
 */

#ifndef AXIS_DB_POOL_H
#define AXIS_DB_POOL_H

#include <glib.h>
#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"
#include "db/axis-db-result.h"

G_BEGIN_DECLS

#define AXIS_DB_POOL_MIN_SIZE                1
#define AXIS_DB_POOL_MAX_SIZE                64
#define AXIS_DB_POOL_DEFAULT_ACQUIRE_TIMEOUT_MS 30000

/* An idle connection older than this is checked with a round trip on
 * acquire. */
#define AXIS_DB_POOL_DEFAULT_VALIDATION_INTERVAL_MS 1000

/* The limit on one connection attempt (all hosts, TLS and authentication
 * included) when the URL, or else PGCONNECT_TIMEOUT, does not set
 * connect_timeout.  The pool enforces connect_timeout itself. */
#define AXIS_DB_DEFAULT_CONNECT_TIMEOUT_S    10

/* PQserverVersion() form: 150000 is 15.0. */
#define AXIS_DB_MIN_SERVER_VERSION           150000

typedef struct _AxisDbPool AxisDbPool;

/**
 * axis_db_pool_new:
 * @url: a libpq connection URL or "key=value" connection string
 * @size: the number of connections, %AXIS_DB_POOL_MIN_SIZE to
 *   %AXIS_DB_POOL_MAX_SIZE
 * @error: (nullable): return location for an error
 *
 * Creates a pool and opens its first connection.  Fails with
 * %AXIS_DB_ERROR_CONNECT when @url cannot be parsed or the server cannot
 * be reached or refuses the connection, and with %AXIS_DB_ERROR_VERSION
 * when the server is older than PostgreSQL 15.  Messages name the server
 * by its redacted URL and never contain the password.
 *
 * Returns: (transfer full) (nullable): the pool, or %NULL on error
 */
AxisDbPool *
axis_db_pool_new(
	const gchar  *url,
	guint         size,
	GError      **error
);

/**
 * axis_db_pool_free:
 *
 * Closes every connection.  Every acquired connection must have been
 * released and nothing may be waiting: freeing a pool that is still in use
 * is a programming error, logged as a critical, and the pool is then
 * deliberately leaked rather than freed under its users.
 */
void
axis_db_pool_free(AxisDbPool *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDbPool, axis_db_pool_free)

/**
 * axis_db_pool_acquire:
 * @self: an #AxisDbPool
 * @error: (nullable): return location for an error
 *
 * Takes a connection for the calling thread's exclusive use, waiting for
 * one to be released if all are taken.  Waiters are served in the order
 * they arrived.  Fails with %AXIS_DB_ERROR_TIMEOUT if none is free within
 * the acquire timeout, and with %AXIS_DB_ERROR_CONNECT (or
 * %AXIS_DB_ERROR_VERSION) when the connection had to be opened or
 * re-established and that failed; the slot is then free again for the
 * next caller.
 *
 * Returns: (transfer none) (nullable): a connection, to be given back
 *   with axis_db_pool_release() by the same thread
 */
AxisDbConn *
axis_db_pool_acquire(
	AxisDbPool  *self,
	GError     **error
);

/**
 * axis_db_pool_release:
 * @self: the pool @conn came from
 * @conn: a connection the calling thread acquired
 *
 * Gives @conn back.  A connection released with a transaction open (or
 * aborted) is rolled back first, and that is logged as an error because
 * it is always a bug; one whose connection has gone bad is closed and
 * replaced on a later acquire.  Releasing a connection twice, or one the
 * calling thread does not hold, logs a critical and does nothing.
 */
void
axis_db_pool_release(
	AxisDbPool *self,
	AxisDbConn *conn
);

/**
 * axis_db_pool_exec:
 *
 * Acquires a connection, runs one statement as axis_db_conn_exec() and
 * releases the connection.  Fails as either of those.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_db_pool_exec(
	AxisDbPool         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	AxisDbResult      **result,
	GError            **error
);

/**
 * axis_db_pool_query_bounded:
 * @self: an #AxisDbPool
 * @sql: one statement without parameters
 * @timeout_ms: how long to wait in all for a connection and its answer
 * @result: (out) (optional) (transfer full): the result
 * @error: (nullable): return location for an error
 *
 * As axis_db_pool_ping(), running @sql instead of a trivial query: every
 * wait is bounded by @timeout_ms on the client side.  For the probes and
 * the metrics, which must answer whatever the database does.
 *
 * Returns: %TRUE if the statement succeeded in time
 */
gboolean
axis_db_pool_query_bounded(
	AxisDbPool    *self,
	const gchar   *sql,
	guint          timeout_ms,
	AxisDbResult **result,
	GError       **error
);

/**
 * axis_db_url_check:
 * @url: a libpq connection URL or "key=value" connection string
 * @error: (nullable): return location for an error
 *
 * Whether libpq can read @url at all, without connecting.  Fails with
 * %AXIS_DB_ERROR_CONNECT and the same message as axis_db_pool_new() gives
 * for such a URL (redacted, never the password).  Start-up uses it to tell
 * a URL that can never work from a server that is not up yet.
 *
 * Returns: %TRUE if @url can be parsed
 */
gboolean
axis_db_url_check(
	const gchar  *url,
	GError      **error
);

/**
 * axis_db_pool_ping:
 * @self: an #AxisDbPool
 * @timeout_ms: how long to wait in all for a connection and its answer
 * @error: (nullable): return location for an error
 *
 * Checks that the server answers: acquires a connection (re-establishing
 * it if needed) and runs a trivial query, giving up after @timeout_ms.
 * Every wait in that (for a free connection, for a new connection to be
 * made, for the answer) is bounded on the client side, so a server that
 * has stopped answering is reported rather than waited for.  Fails as
 * axis_db_pool_acquire() or axis_db_conn_exec(), or with
 * %AXIS_DB_ERROR_TIMEOUT.
 *
 * Returns: %TRUE if the server answered
 */
gboolean
axis_db_pool_ping(
	AxisDbPool  *self,
	guint        timeout_ms,
	GError     **error
);

/**
 * axis_db_pool_set_acquire_timeout:
 * @self: an #AxisDbPool
 * @timeout_ms: how long axis_db_pool_acquire() waits for a free
 *   connection; 0 means not at all
 *
 * The default is %AXIS_DB_POOL_DEFAULT_ACQUIRE_TIMEOUT_MS.  Applies to
 * acquires that start after the call.
 */
void
axis_db_pool_set_acquire_timeout(
	AxisDbPool *self,
	guint       timeout_ms
);

/**
 * axis_db_pool_set_validation_interval:
 * @self: an #AxisDbPool
 * @interval_ms: a connection idle for at least this long is checked with
 *   a round trip on acquire; 0 checks every time
 *
 * The default is %AXIS_DB_POOL_DEFAULT_VALIDATION_INTERVAL_MS.  A
 * connection whose socket shows the server has closed it or sent
 * something is checked whatever its idle time.
 */
void
axis_db_pool_set_validation_interval(
	AxisDbPool *self,
	guint       interval_ms
);

guint
axis_db_pool_get_size(AxisDbPool *self);

/**
 * axis_db_pool_get_connections_opened:
 *
 * Returns: how many server connections the pool has established since it
 *   was created, counting the first and every replacement
 */
guint
axis_db_pool_get_connections_opened(AxisDbPool *self);

/**
 * axis_db_pool_get_statements_sent:
 *
 * Returns: how many statements the pool's connections have sent since it
 *   was created (for tests that bound what a page costs)
 */
guint
axis_db_pool_get_statements_sent(AxisDbPool *self);

/* Counts one statement sent on one of the pool's connections, and tells the
 * observer, if one is set. */
void
axis_db_pool_count_statement(
	AxisDbPool         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params
);

/**
 * AxisDbStatementObserver:
 * @sql: the statement as it was sent
 * @n_params: how many parameters it has
 * @params: (nullable): their values (each %NULL for SQL NULL); valid during
 *   the call only
 * @data: as given to axis_db_pool_set_statement_observer()
 *
 * Called on the thread that sent the statement, just after it was sent.
 * It must not use the pool.
 */
typedef void (*AxisDbStatementObserver)(
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	gpointer            data
);

/**
 * axis_db_pool_set_statement_observer:
 * @observer: (nullable): told every statement the pool's connections send
 *   from now on; %NULL: none
 *
 * For tests that measure the statements the daemon's own functions send
 * (tests/test-db-scale.c): what is explained is what was sent.
 */
void
axis_db_pool_set_statement_observer(
	AxisDbPool              *self,
	AxisDbStatementObserver  observer,
	gpointer                 data
);

/**
 * axis_db_pool_get_n_in_use:
 *
 * Returns: how many connections are acquired right now
 */
guint
axis_db_pool_get_n_in_use(AxisDbPool *self);

/**
 * axis_db_pool_get_n_waiting:
 *
 * Returns: how many callers are waiting in axis_db_pool_acquire() right now
 */
guint
axis_db_pool_get_n_waiting(AxisDbPool *self);

/**
 * axis_db_check_server_version:
 * @server_version: a version as from PQserverVersion(), for example
 *   170004 for 17.4 or 90624 for 9.6.24; 0 if unknown
 * @error: (nullable): return location for an error
 *
 * Fails with %AXIS_DB_ERROR_VERSION, stating the version found and the
 * version required, when @server_version is below
 * %AXIS_DB_MIN_SERVER_VERSION.
 *
 * Returns: %TRUE if the version is supported
 */
gboolean
axis_db_check_server_version(
	gint     server_version,
	GError **error
);

/**
 * axis_db_format_server_version:
 * @server_version: a version as from PQserverVersion()
 *
 * Returns: (transfer full): "17.4", "9.6.24", or "unknown" for 0 or less
 */
gchar *
axis_db_format_server_version(gint server_version);

G_END_DECLS

#endif /* AXIS_DB_POOL_H */
