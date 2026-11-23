/*
 * axis-db-conn.h - one pooled PostgreSQL connection
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A connection belongs to its pool (axis-db-pool.h) and is used by exactly
 * one thread between axis_db_pool_acquire() and axis_db_pool_release().
 * Every call here blocks that thread until the server answers, so never
 * use one on the main loop's thread: hand the work to a GTask or an
 * htmx-glib threaded response.
 *
 * Statements are SQL text plus parameters, sent separately with
 * PQexecParams() semantics (extended protocol, text format), so a value
 * can never change the shape of a statement.  There is deliberately no
 * function that formats SQL from a printf-style template.  An identifier
 * that has to be chosen at run time goes through
 * axis_db_quote_identifier(); values always go in as parameters.
 *
 * Every connection has these session settings: application_name=axis,
 * client_encoding=UTF8, TimeZone=UTC, statement_timeout=60s and
 * idle_in_transaction_session_timeout=5min.
 */

#ifndef AXIS_DB_CONN_H
#define AXIS_DB_CONN_H

#include <glib.h>
#include "db/axis-db-error.h"
#include "db/axis-db-result.h"

G_BEGIN_DECLS

#define AXIS_DB_STATEMENT_TIMEOUT_MS            60000
#define AXIS_DB_IDLE_IN_TRANSACTION_TIMEOUT_MS  300000

/* The protocol's limit on parameters per statement. */
#define AXIS_DB_MAX_PARAMS 65535

typedef struct _AxisDbConn AxisDbConn;

/**
 * AxisDbInterrupt:
 *
 * A way to call off the database work of a thread from another one.  Work
 * that must end promptly when its owner stops (a background reading at
 * shutdown) attaches one to its thread; once it is raised, every wait of
 * that thread on the database ends at once, as if its deadline had passed:
 * the wait for a pool connection, a connection attempt, a statement's
 * answer (%AXIS_DB_ERROR_TIMEOUT, %AXIS_DB_ERROR_CONNECT), and every later
 * one too, so that whatever the thread still tries fails without waiting.
 * A statement called off this way is abandoned as one that ran out of time
 * is: its connection is closed when released.  Raised is for good: an
 * interrupt is made for one owner and raised when that owner stops.
 *
 * A thread that has none attached (every thread, to begin with) waits as
 * the calls here say.
 */
typedef struct _AxisDbInterrupt AxisDbInterrupt;

AxisDbInterrupt *
axis_db_interrupt_new(void);

/* Only once no thread has it attached.  %NULL is ignored. */
void
axis_db_interrupt_free(AxisDbInterrupt *self);

/* From any thread; raising it again does nothing. */
void
axis_db_interrupt_raise(AxisDbInterrupt *self);

/* From any thread; %NULL is not raised. */
gboolean
axis_db_interrupt_is_raised(AxisDbInterrupt *self);

/* Attaches @self to the calling thread (borrowed: it must outlive the
 * attachment), instead of the one attached before; %NULL detaches.  A
 * thread of a pool detaches before it gives itself back. */
void
axis_db_interrupt_attach(AxisDbInterrupt *self);

/**
 * axis_db_conn_exec:
 * @self: a connection held by the calling thread
 * @sql: one SQL statement, using $1, $2, ... for parameters
 * @n_params: the number of entries in @params (at most %AXIS_DB_MAX_PARAMS)
 * @params: (array length=n_params) (nullable): the parameter values in text
 *   format; a %NULL entry is SQL NULL
 * @result: (out) (optional) (nullable) (transfer full): the result; pass
 *   %NULL when the rows (or the affected-row count) are not wanted
 * @error: (nullable): return location for an error
 *
 * Runs one statement.  Text containing more than one statement is refused
 * by the server.
 *
 * Fails with %AXIS_DB_ERROR_UNIQUE_VIOLATION for SQLSTATE 23505,
 * %AXIS_DB_ERROR_TIMEOUT when the server cancelled the statement (57014),
 * %AXIS_DB_ERROR_CONNECTION_LOST when the connection went away (then every
 * later call on @self fails the same way until it is released; the pool
 * replaces it), and %AXIS_DB_ERROR_QUERY otherwise.  The message is the
 * server's, with its SQLSTATE.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_db_conn_exec(
	AxisDbConn         *self,
	const gchar        *sql,
	guint               n_params,
	const gchar *const *params,
	AxisDbResult      **result,
	GError            **error
);

/**
 * axis_db_conn_begin:
 * @self: a connection held by the calling thread
 * @error: (nullable): return location for an error
 *
 * Opens a transaction.  Fails with %AXIS_DB_ERROR_QUERY, changing
 * nothing, if one is already open: there is no nesting.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_db_conn_begin(
	AxisDbConn  *self,
	GError     **error
);

/**
 * axis_db_conn_commit:
 * @self: a connection held by the calling thread
 * @error: (nullable): return location for an error
 *
 * Commits the open transaction.  If a statement in it has failed, the
 * server can only roll it back: this then rolls back and fails with
 * %AXIS_DB_ERROR_QUERY.  A commit the server refuses (for example a
 * deferred constraint) also fails, and the transaction is gone either
 * way.  Fails with %AXIS_DB_ERROR_QUERY when no transaction is open.
 *
 * Returns: %TRUE if the transaction was committed
 */
gboolean
axis_db_conn_commit(
	AxisDbConn  *self,
	GError     **error
);

/**
 * axis_db_conn_rollback:
 * @self: a connection held by the calling thread
 * @error: (nullable): return location for an error
 *
 * Rolls back the open transaction, failed or not.  Succeeds and does
 * nothing when no transaction is open, so cleanup paths can call it
 * unconditionally.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_db_conn_rollback(
	AxisDbConn  *self,
	GError     **error
);

/**
 * axis_db_conn_in_transaction:
 *
 * Returns: %TRUE if a transaction is open on @self, including one that a
 *   failed statement has aborted and that still needs a rollback
 */
gboolean
axis_db_conn_in_transaction(AxisDbConn *self);

/**
 * axis_db_quote_identifier:
 * @identifier: a table, column or other name
 *
 * Quotes @identifier for use as a SQL identifier: wraps it in double
 * quotes and doubles any double quote inside, so it is always one
 * identifier, spelled exactly as given (case included).  Connections use
 * client_encoding UTF8, so this is safe for any valid UTF-8.
 *
 * Returns: (transfer full) (nullable): the quoted identifier, or %NULL if
 *   @identifier is %NULL, empty, or not valid UTF-8
 */
gchar *
axis_db_quote_identifier(const gchar *identifier);

G_END_DECLS

#endif /* AXIS_DB_CONN_H */
