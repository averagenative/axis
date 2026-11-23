/*
 * axis-db-migrate.h - schema migrations
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Migrations are the files data/migrations/NNNN_name.sql, compiled into
 * the binary.  Numbers are four digits, start at 0001 and have no gaps;
 * the build refuses anything else.  A migration is applied once, in
 * ascending order, and recorded in the table axis_schema_migration
 * (version, name, checksum, applied_at), where checksum is the SHA-256 of
 * the file's exact bytes in lower-case hex.  An applied migration is never
 * changed: a fix is a new migration.
 *
 * A migration runs in its own transaction together with its bookkeeping
 * row, so it is applied completely or not at all.  A migration that
 * cannot run in a transaction (CREATE INDEX CONCURRENTLY, for example)
 * starts with the line
 *
 *   -- axis:no-transaction
 *
 * and must be a single statement (the server refuses more).  It is run on
 * its own and recorded only after it succeeds.  If it fails part-way, what
 * it left behind (an invalid index, say) stays, and it runs again next
 * time, so write it to be re-runnable (IF NOT EXISTS).
 */

#ifndef AXIS_DB_MIGRATE_H
#define AXIS_DB_MIGRATE_H

#include <glib.h>
#include "db/axis-db-pool.h"

G_BEGIN_DECLS

#define AXIS_DB_MIGRATION_TABLE "axis_schema_migration"

/* The session advisory lock (pg_advisory_lock(bigint)) held by a runner
 * for its whole run.  Its bytes spell "axis_mig". */
#define AXIS_DB_MIGRATION_LOCK_KEY G_GINT64_CONSTANT(0x617869735f6d6967)

#define AXIS_DB_MIGRATION_MAX_VERSION  9999
#define AXIS_DB_MIGRATION_NAME_MAX_LEN 63

#define AXIS_DB_NO_TRANSACTION_MARKER "-- axis:no-transaction"

/**
 * AxisDbMigration:
 * @version: 1 to %AXIS_DB_MIGRATION_MAX_VERSION
 * @name: the file name's name part ("schema_baseline"): 1 to
 *   %AXIS_DB_MIGRATION_NAME_MAX_LEN lower-case ASCII letters, digits and
 *   '_', starting with a letter
 * @sql: the file's contents; the checksum is computed over exactly these
 *   bytes (up to the terminating NUL)
 * @no_transaction: whether @sql starts with the no-transaction marker;
 *   must agree with axis_db_migration_has_no_transaction_marker()
 */
typedef struct
{
	guint        version;
	const gchar *name;
	const gchar *sql;
	gboolean     no_transaction;
} AxisDbMigration;

/**
 * axis_db_migrate:
 * @pool: the pool to use; one connection is held for the whole run
 * @error: (nullable): return location for an error
 *
 * Brings the database up to date with the migrations compiled into this
 * binary, as axis_db_migrate_with().
 *
 * Returns: %TRUE when the schema is up to date
 */
gboolean
axis_db_migrate(
	AxisDbPool  *pool,
	GError     **error
);

/**
 * axis_db_migrate_latest_version:
 *
 * Returns: the newest migration version compiled into this binary (read
 *   once), or 0 if none can be read
 */
guint
axis_db_migrate_latest_version(void);

/**
 * axis_db_migrate_check_current:
 * @pool: the pool
 * @timeout_ms: the deadline for the whole check (axis_db_pool_query_bounded())
 * @error: (nullable): return location for an error
 *
 * Whether the database answers within @timeout_ms and its newest recorded
 * migration is exactly axis_db_migrate_latest_version(): the readiness
 * check.  Fails as axis_db_pool_query_bounded(), or with
 * %AXIS_DB_ERROR_MIGRATION when the version differs (behind, or written by
 * a newer build).
 *
 * Returns: %TRUE if the schema is the one this build expects
 */
gboolean
axis_db_migrate_check_current(
	AxisDbPool  *pool,
	guint        timeout_ms,
	GError     **error
);

/**
 * axis_db_migrate_with:
 * @pool: the pool to use; one connection is held for the whole run
 * @migrations: (array length=n_migrations): the migrations, in ascending
 *   version order, numbered from 1 with no gaps
 * @n_migrations: the number of entries in @migrations
 * @error: (nullable): return location for an error
 *
 * Takes the migration advisory lock (logging that it waits when another
 * runner holds it), creates axis_schema_migration if it does not exist,
 * and checks the recorded history against @migrations before applying
 * anything.  It fails with %AXIS_DB_ERROR_MIGRATION, applying nothing,
 * when the database records a version not in @migrations (it was written
 * by a newer binary), a recorded checksum or name differs from the
 * migration of the same version (an applied migration was edited), or the
 * recorded versions are not 1..N without gaps.  It then applies each
 * pending migration in order, with statement_timeout disabled, and stops
 * at the first that fails with an error naming it; the migrations before
 * it stay applied.  The lock is released on every path.
 *
 * Also fails with %AXIS_DB_ERROR_MIGRATION, before touching the database,
 * when @migrations itself breaks the rules above, and as
 * axis_db_pool_acquire() when no connection can be had.
 *
 * Returns: %TRUE when every migration in @migrations is applied
 */
gboolean
axis_db_migrate_with(
	AxisDbPool             *pool,
	const AxisDbMigration  *migrations,
	guint                   n_migrations,
	GError                **error
);

/**
 * axis_db_migration_parse_filename:
 * @filename: a file name without directory, such as
 *   "0001_schema_baseline.sql"
 * @version: (out) (optional): the version; untouched on failure
 * @name: (out) (optional) (transfer full): the name part; untouched on
 *   failure
 *
 * Accepts exactly: four ASCII digits forming a number from 1 to 9999, '_',
 * a name of 1 to %AXIS_DB_MIGRATION_NAME_MAX_LEN lower-case ASCII letters,
 * digits and '_' that starts with a letter, and ".sql".  Nothing before,
 * nothing after.  These are the rules the build applies to
 * data/migrations.
 *
 * Returns: %TRUE if @filename is a valid migration file name
 */
gboolean
axis_db_migration_parse_filename(
	const gchar  *filename,
	guint        *version,
	gchar       **name
);

/**
 * axis_db_migration_has_no_transaction_marker:
 * @sql: (nullable): a migration's contents
 *
 * Returns: %TRUE if the first line of @sql (up to the first "\n" or the
 *   end), with trailing spaces, tabs and a carriage return removed, is
 *   exactly %AXIS_DB_NO_TRANSACTION_MARKER
 */
gboolean
axis_db_migration_has_no_transaction_marker(const gchar *sql);

/**
 * axis_db_migration_checksum:
 * @sql: a migration's contents
 *
 * Returns: (transfer full): the SHA-256 of @sql's bytes, in lower-case hex
 */
gchar *
axis_db_migration_checksum(const gchar *sql);

G_END_DECLS

#endif /* AXIS_DB_MIGRATE_H */
