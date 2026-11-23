/*
 * axis-db-migrate.c - schema migrations
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One run, on one connection held throughout:
 *
 *   1. statement_timeout off for this session (a migration may be long)
 *   2. pg_advisory_lock(AXIS_DB_MIGRATION_LOCK_KEY), waiting if another
 *      runner holds it; a second runner then finds the work done
 *   3. create axis_schema_migration if missing (the lock makes this safe)
 *   4. verify the whole recorded history before applying anything
 *   5. apply each pending migration with its bookkeeping row
 *   6. unlock; the connection is closed on release, so neither the lock
 *      nor the changed setting can outlive the run whatever happened
 */

#include "db/axis-db-migrate.h"
#include "db/axis-db-error.h"
#include "db/axis-db-private.h"
#include "axis-migrations-resource.h"

#include <stdlib.h>
#include <string.h>

#define RESOURCE_DIR "/axis/migrations/"

static const gchar create_table_sql[] =
	"CREATE TABLE " AXIS_DB_MIGRATION_TABLE " ("
	" version integer PRIMARY KEY CHECK (version > 0),"
	" name text NOT NULL,"
	" checksum text NOT NULL,"
	" applied_at timestamptz NOT NULL DEFAULT now()"
	")";

/*
 * The migrations compiled into this binary
 */

typedef struct
{
	GArray    *migrations;	/* of AxisDbMigration */
	GPtrArray *strings;	/* what the migrations point into */
} Builtin;

static void
builtin_clear(Builtin *builtin)
{
	if (builtin->migrations != NULL)
		g_array_unref(builtin->migrations);
	if (builtin->strings != NULL)
		g_ptr_array_unref(builtin->strings);
}

static int
compare_names(
	const void *a,
	const void *b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

static gboolean
builtin_load(
	Builtin  *builtin,
	GError  **error
){
	static gsize registered = 0;
	g_autoptr(GError) local = NULL;
	g_auto(GStrv) children = NULL;
	GResource *resource;
	guint n;
	guint i;

	if (g_once_init_enter(&registered)) {
		axis_migrations_register_resource();
		g_once_init_leave(&registered, 1);
	}
	resource = axis_migrations_get_resource();

	builtin->migrations = g_array_new(FALSE, TRUE, sizeof(AxisDbMigration));
	builtin->strings = g_ptr_array_new_with_free_func(g_free);

	children = g_resource_enumerate_children(resource, RESOURCE_DIR,
		G_RESOURCE_LOOKUP_FLAGS_NONE, &local);
	if (children == NULL) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"no migrations are compiled into this binary: %s", local->message);
		return FALSE;
	}

	n = g_strv_length(children);
	qsort(children, n, sizeof(children[0]), compare_names);

	for (i = 0; i < n; i++) {
		g_autofree gchar *path = g_strconcat(RESOURCE_DIR, children[i], NULL);
		g_autoptr(GBytes) bytes = NULL;
		AxisDbMigration migration;
		gconstpointer data;
		gchar *name = NULL;
		gchar *sql;
		gsize size;

		memset(&migration, 0, sizeof(migration));
		if (!axis_db_migration_parse_filename(children[i], &migration.version, &name)) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"compiled-in migration '%s' has a bad name", children[i]);
			return FALSE;
		}
		g_ptr_array_add(builtin->strings, name);

		bytes = g_resource_lookup_data(resource, path, G_RESOURCE_LOOKUP_FLAGS_NONE, &local);
		if (bytes == NULL) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"cannot read compiled-in migration %s: %s", children[i], local->message);
			return FALSE;
		}

		/* The checksum covers the exact bytes, so a NUL inside (which
		 * the string API would cut at) is refused. */
		data = g_bytes_get_data(bytes, &size);
		if (size > 0 && memchr(data, '\0', size) != NULL) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"compiled-in migration %s contains a NUL byte", children[i]);
			return FALSE;
		}
		sql = g_strndup(data, size);
		g_ptr_array_add(builtin->strings, sql);

		migration.name = name;
		migration.sql = sql;
		migration.no_transaction = axis_db_migration_has_no_transaction_marker(sql);
		g_array_append_val(builtin->migrations, migration);
	}

	return TRUE;
}

/*
 * Checks before touching the database
 */

static gboolean
validate_list(
	const AxisDbMigration  *migrations,
	guint                   n,
	GError                **error
){
	guint i;

	if (n > AXIS_DB_MIGRATION_MAX_VERSION) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"too many migrations: %u", n);
		return FALSE;
	}

	for (i = 0; i < n; i++) {
		const AxisDbMigration *m = &migrations[i];
		g_autofree gchar *filename = NULL;

		if (m->version != i + 1) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"migrations must be numbered from 1 without gaps: entry %u has version %u",
				i + 1, m->version);
			return FALSE;
		}

		filename = g_strdup_printf("%04u_%s.sql", m->version, m->name != NULL ? m->name : "");
		if (m->name == NULL || !axis_db_migration_parse_filename(filename, NULL, NULL)) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"migration %04u has an invalid name '%s'",
				m->version, m->name != NULL ? m->name : "(null)");
			return FALSE;
		}

		if (m->sql == NULL) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"migration %04u_%s has no SQL", m->version, m->name);
			return FALSE;
		}

		if (!m->no_transaction != !axis_db_migration_has_no_transaction_marker(m->sql)) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"migration %04u_%s: no_transaction does not match the '%s' marker",
				m->version, m->name, AXIS_DB_NO_TRANSACTION_MARKER);
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * Steps of a run
 */

static gboolean
take_lock(
	AxisDbConn  *conn,
	const gchar *key,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[1];
	gboolean locked = FALSE;

	params[0] = key;
	if (!axis_db_conn_exec(conn, "SELECT pg_try_advisory_lock($1::bigint)", 1, params, &result, error))
		return FALSE;
	if (!axis_db_result_get_boolean(result, 0, 0, &locked)) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"unexpected answer from pg_try_advisory_lock()");
		return FALSE;
	}
	if (locked)
		return TRUE;

	g_message("another process is migrating the database; waiting for it to finish");
	return axis_db_conn_exec(conn, "SELECT pg_advisory_lock($1::bigint)", 1, params, NULL, error);
}

static void
drop_lock(
	AxisDbConn  *conn,
	const gchar *key
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) local = NULL;
	const gchar *params[1];
	gboolean unlocked = FALSE;

	/* A transaction left open by a failure would make the unlock fail
	 * too; rollback is a no-op when there is none. */
	axis_db_conn_rollback(conn, NULL);

	params[0] = key;
	if (!axis_db_conn_exec(conn, "SELECT pg_advisory_unlock($1::bigint)", 1, params, &result, &local)) {
		/* The connection is closed on release, which ends the session
		 * and with it the lock. */
		g_debug("cannot release the migration lock (the session will end anyway): %s",
			local->message);
		return;
	}
	if (!axis_db_result_get_boolean(result, 0, 0, &unlocked) || !unlocked)
		g_warning("the migration lock was not held when the run ended");
}

static gboolean
ensure_table(
	AxisDbConn  *conn,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	gboolean exists = FALSE;

	if (!axis_db_conn_exec(conn,
	                       "SELECT to_regclass('" AXIS_DB_MIGRATION_TABLE "') IS NOT NULL",
	                       0, NULL, &result, error))
		return FALSE;
	if (!axis_db_result_get_boolean(result, 0, 0, &exists)) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"unexpected answer from to_regclass()");
		return FALSE;
	}
	if (exists)
		return TRUE;

	g_info("creating table " AXIS_DB_MIGRATION_TABLE);
	return axis_db_conn_exec(conn, create_table_sql, 0, NULL, NULL, error);
}

/* Checks the recorded history against @migrations; on success sets
 * @n_applied to how many are already applied (always the first ones). */
static gboolean
verify_history(
	AxisDbConn             *conn,
	const AxisDbMigration  *migrations,
	guint                   n,
	guint                  *n_applied,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint rows;
	guint i;

	if (!axis_db_conn_exec(conn,
	                       "SELECT version, name, checksum FROM " AXIS_DB_MIGRATION_TABLE
	                       " ORDER BY version",
	                       0, NULL, &result, error))
		return FALSE;

	rows = axis_db_result_get_n_rows(result);

	/* First the most important refusal: a newer binary has been here. */
	for (i = 0; i < rows; i++) {
		gint64 version = 0;

		if (!axis_db_result_get_int64(result, i, 0, &version)) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				AXIS_DB_MIGRATION_TABLE " holds a version that is not a number");
			return FALSE;
		}
		if (version < 1 || version > (gint64)n) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"the database records migration %04" G_GINT64_FORMAT "_%s, which this "
				"build of Axis does not have: the database was written by a newer "
				"version of Axis. Run that version, or restore a backup taken before it",
				version, axis_db_result_get_text(result, i, 1));
			return FALSE;
		}
	}

	for (i = 0; i < rows; i++) {
		const AxisDbMigration *m;
		g_autofree gchar *checksum = NULL;
		const gchar *recorded_name;
		const gchar *recorded_checksum;
		gint64 version = 0;

		axis_db_result_get_int64(result, i, 0, &version);
		if (version != (gint64)i + 1) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"the recorded migration history has a gap: migration %04u is missing "
				"but %04" G_GINT64_FORMAT " is recorded", i + 1, version);
			return FALSE;
		}

		m = &migrations[i];
		recorded_name = axis_db_result_get_text(result, i, 1);
		recorded_checksum = axis_db_result_get_text(result, i, 2);
		checksum = axis_db_migration_checksum(m->sql);

		if (g_strcmp0(recorded_name, m->name) != 0) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"migration %04u was applied as '%s' but this build calls it '%s': "
				"applied migrations must never be renamed or replaced",
				m->version, recorded_name != NULL ? recorded_name : "", m->name);
			return FALSE;
		}
		if (g_strcmp0(recorded_checksum, checksum) != 0) {
			g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"migration %04u_%s has been changed since it was applied (the database "
				"records checksum %s, this build has %s): applied migrations must never "
				"be edited; put the change in a new migration",
				m->version, m->name,
				recorded_checksum != NULL ? recorded_checksum : "(none)", checksum);
			return FALSE;
		}
	}

	*n_applied = rows;
	return TRUE;
}

static gboolean
record(
	AxisDbConn             *conn,
	const AxisDbMigration  *m,
	GError                **error
){
	g_autofree gchar *version = g_strdup_printf("%u", m->version);
	g_autofree gchar *checksum = axis_db_migration_checksum(m->sql);
	const gchar *params[3];

	params[0] = version;
	params[1] = m->name;
	params[2] = checksum;

	return axis_db_conn_exec(conn,
		"INSERT INTO " AXIS_DB_MIGRATION_TABLE " (version, name, checksum) VALUES ($1, $2, $3)",
		3, params, NULL, error);
}

static gboolean
apply(
	AxisDbConn             *conn,
	const AxisDbMigration  *m,
	GError                **error
){
	g_autoptr(GError) local = NULL;
	gint64 start = g_get_monotonic_time();

	if (m->no_transaction) {
		/* One statement, outside any transaction (the extended protocol
		 * refuses more than one); recorded only once it has worked. */
		if (!axis_db_conn_run(conn, m->sql, 0, NULL, FALSE, -1, NULL, &local))
			goto fail;
		if (!record(conn, m, &local))
			goto fail;
	} else {
		if (!axis_db_conn_begin(conn, &local))
			goto fail;
		if (!axis_db_conn_run(conn, m->sql, 0, NULL, TRUE, -1, NULL, &local))
			goto fail;
		if (axis_db_conn_get_transaction_status(conn) != PQTRANS_INTRANS) {
			g_set_error(&local, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
				"it ended the transaction itself (COMMIT or ROLLBACK in the file), "
				"so it may be partly applied; migrations must not manage transactions");
			goto fail;
		}
		if (!record(conn, m, &local))
			goto fail;
		if (!axis_db_conn_commit(conn, &local))
			goto fail;
	}

	g_info("applied migration %04u_%s in %" G_GINT64_FORMAT " ms",
		m->version, m->name, (g_get_monotonic_time() - start) / 1000);
	return TRUE;

fail:
	axis_db_conn_rollback(conn, NULL);
	g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
		"migration %04u_%s failed: %s", m->version, m->name, local->message);
	return FALSE;
}

gboolean
axis_db_migrate_with(
	AxisDbPool             *pool,
	const AxisDbMigration  *migrations,
	guint                   n_migrations,
	GError                **error
){
	g_autofree gchar *key = NULL;
	g_autoptr(GError) local = NULL;
	AxisDbConn *conn;
	gboolean locked = FALSE;
	gboolean ok = FALSE;
	guint n_applied = 0;
	guint i;

	g_return_val_if_fail(pool != NULL, FALSE);
	g_return_val_if_fail(n_migrations == 0 || migrations != NULL, FALSE);

	if (!validate_list(migrations, n_migrations, error))
		return FALSE;

	conn = axis_db_pool_acquire(pool, error);
	if (conn == NULL)
		return FALSE;

	/* The settings and the lock below belong to this session; closing
	 * it afterwards guarantees neither outlives the run. */
	axis_db_conn_set_close_on_release(conn);

	key = g_strdup_printf("%" G_GINT64_FORMAT, AXIS_DB_MIGRATION_LOCK_KEY);

	if (!axis_db_conn_exec(conn, "SELECT set_config('statement_timeout', '0', false)",
	                       0, NULL, NULL, &local) ||
	    !take_lock(conn, key, &local)) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"cannot start migrating the database: %s", local->message);
		goto out;
	}
	locked = TRUE;

	if (!ensure_table(conn, &local)) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"cannot create " AXIS_DB_MIGRATION_TABLE ": %s", local->message);
		goto out;
	}

	if (!verify_history(conn, migrations, n_migrations, &n_applied, error))
		goto out;

	for (i = n_applied; i < n_migrations; i++) {
		if (!apply(conn, &migrations[i], error))
			goto out;
	}

	if (n_applied == n_migrations)
		g_info("database schema is up to date at version %04u", n_migrations);
	else
		g_info("database schema migrated from version %04u to %04u", n_applied, n_migrations);
	ok = TRUE;

out:
	if (locked)
		drop_lock(conn, key);
	axis_db_pool_release(pool, conn);

	return ok;
}

guint
axis_db_migrate_latest_version(void)
{
	static gsize latest = 0;

	if (g_once_init_enter(&latest)) {
		Builtin builtin;
		guint found = 0;
		guint i;

		memset(&builtin, 0, sizeof(builtin));
		if (builtin_load(&builtin, NULL)) {
			for (i = 0; i < builtin.migrations->len; i++)
				found = MAX(found, g_array_index(builtin.migrations, AxisDbMigration, i).version);
		}
		builtin_clear(&builtin);
		/* g_once_init_leave() takes no zero: one more than the version. */
		g_once_init_leave(&latest, (gsize)found + 1);
	}

	return (guint)(latest - 1);
}

gboolean
axis_db_migrate_check_current(
	AxisDbPool  *pool,
	guint        timeout_ms,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint expected = axis_db_migrate_latest_version();
	gint64 recorded = 0;

	g_return_val_if_fail(pool != NULL, FALSE);

	if (!axis_db_pool_query_bounded(pool,
	                                "SELECT coalesce(max(version), 0) FROM axis_schema_migration",
	                                timeout_ms, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != 1 || !axis_db_result_get_int64(result, 0, 0, &recorded)) {
		g_set_error_literal(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"the schema version cannot be read");
		return FALSE;
	}
	if (recorded != (gint64)expected) {
		g_set_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION,
			"the database's schema is at version %" G_GINT64_FORMAT " and this build expects %u",
			recorded, expected);
		return FALSE;
	}

	return TRUE;
}

gboolean
axis_db_migrate(
	AxisDbPool  *pool,
	GError     **error
){
	Builtin builtin;
	gboolean ok;

	g_return_val_if_fail(pool != NULL, FALSE);

	memset(&builtin, 0, sizeof(builtin));
	if (!builtin_load(&builtin, error)) {
		builtin_clear(&builtin);
		return FALSE;
	}

	ok = axis_db_migrate_with(pool,
		(const AxisDbMigration *)(gpointer)builtin.migrations->data,
		builtin.migrations->len, error);
	builtin_clear(&builtin);

	return ok;
}
