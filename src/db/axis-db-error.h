/*
 * axis-db-error.h - error domain of the database layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No message in this domain ever contains the database password: text
 * that comes from libpq or the server is scrubbed of it, and the database
 * URL only ever appears passed through axis_config_redact_url().
 */

#ifndef AXIS_DB_ERROR_H
#define AXIS_DB_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define AXIS_DB_ERROR (axis_db_error_quark())

/**
 * AxisDbError:
 * @AXIS_DB_ERROR_CONNECT: no working connection could be made: the URL is
 *   invalid, the server refused or could not be reached, or a connection
 *   found dead on acquire could not be re-established
 * @AXIS_DB_ERROR_TIMEOUT: a wait ran out: no connection became free within
 *   the acquire timeout, a deadline-bounded query (ping) did not finish, or
 *   the server cancelled a statement (statement_timeout, SQLSTATE 57014)
 * @AXIS_DB_ERROR_VERSION: the server is older than PostgreSQL 15
 * @AXIS_DB_ERROR_QUERY: the server rejected a statement, or the statement
 *   could not be sent
 * @AXIS_DB_ERROR_UNIQUE_VIOLATION: a statement broke a unique constraint
 *   (SQLSTATE 23505)
 * @AXIS_DB_ERROR_CONNECTION_LOST: the connection went away while the
 *   caller was using it; the pool replaces it on a later acquire
 * @AXIS_DB_ERROR_MIGRATION: the schema cannot be brought up to date: a
 *   migration failed (the message names it), or the recorded history does
 *   not match the migrations in this binary
 */
typedef enum
{
	AXIS_DB_ERROR_CONNECT,
	AXIS_DB_ERROR_TIMEOUT,
	AXIS_DB_ERROR_VERSION,
	AXIS_DB_ERROR_QUERY,
	AXIS_DB_ERROR_UNIQUE_VIOLATION,
	AXIS_DB_ERROR_CONNECTION_LOST,
	AXIS_DB_ERROR_MIGRATION
} AxisDbError;

GQuark
axis_db_error_quark(void);

G_END_DECLS

#endif /* AXIS_DB_ERROR_H */
