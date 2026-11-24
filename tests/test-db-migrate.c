/*
 * test-db-migrate.c - the migration runner against PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "axis-test-db.h"
#include "db/axis-db-error.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"

#ifndef AXIS_MIGRATION_DIR
#error "AXIS_MIGRATION_DIR must be defined by the build"
#endif

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	fx->pool = axis_db_pool_new(axis_test_db_get_url(fx->db), 2, &error);
	g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

static gchar *
q(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->db, sql);
}

static void
assert_query(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *value = q(fx, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql,
			expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

/* What axis_schema_migration holds, one "version name checksum" per line. */
static gchar *
history(Fixture *fx)
{
	return q(fx, "SELECT coalesce(string_agg(version || ' ' || name || ' ' || checksum, E'\\n' "
		"ORDER BY version), '') FROM axis_schema_migration");
}

static void
assert_lock_free(Fixture *fx)
{
	assert_query(fx,
		"SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = "
		"(SELECT oid FROM pg_database WHERE datname = current_database())", "0");
}

static void
assert_migration_error(
	GError      *error,
	const gchar *needle1,
	const gchar *needle2
){
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION);
	g_test_message("migration error: %s", error->message);
	if (strstr(error->message, needle1) == NULL)
		g_error("'%s' not in: %s", needle1, error->message);
	if (needle2 != NULL && strstr(error->message, needle2) == NULL)
		g_error("'%s' not in: %s", needle2, error->message);
}

#define M(v, n, s) { v, n, s, FALSE }
#define M_NOTX(v, n, s) { v, n, s, TRUE }

/* Spec: Empty database is brought up to date (with the migrations
 * compiled into the binary) */
static void
test_builtin_from_empty(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) expected = g_string_new(NULL);
	g_autofree gchar *recorded = NULL;
	const gchar *entry;
	guint i;

	if (fx->db == NULL)
		return;

	assert_query(fx, "SELECT to_regclass('axis_schema_migration') IS NULL", "t");
	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_no_error(error);

	/* Every file in data/migrations, in order, with the SHA-256 of its
	 * bytes as they are on disk. */
	dir = g_dir_open(AXIS_MIGRATION_DIR, 0, &error);
	g_assert_no_error(error);
	while ((entry = g_dir_read_name(dir)) != NULL)
		g_ptr_array_add(files, g_strdup(entry));
	g_ptr_array_sort_values(files, (GCompareFunc)strcmp);
	g_assert_cmpuint(files->len, >=, 1);

	for (i = 0; i < files->len; i++) {
		const gchar *file = g_ptr_array_index(files, i);
		g_autofree gchar *path = g_build_filename(AXIS_MIGRATION_DIR, file, NULL);
		g_autofree gchar *contents = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *checksum = NULL;
		gsize length = 0;
		guint version = 0;

		g_assert_true(axis_db_migration_parse_filename(file, &version, &name));
		g_assert_cmpuint(version, ==, i + 1);
		g_assert_true(g_file_get_contents(path, &contents, &length, &error));
		checksum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
		g_string_append_printf(expected, "%s%u %s %s", i > 0 ? "\n" : "", version, name, checksum);
	}

	recorded = history(fx);
	g_assert_cmpstr(recorded, ==, expected->str);

	/* 0001_schema_baseline: the setting table, exactly. */
	assert_query(fx,
		"SELECT string_agg(column_name || ' ' || data_type || ' ' || is_nullable || ' ' || "
		"coalesce(column_default, '-'), ', ' ORDER BY ordinal_position) "
		"FROM information_schema.columns WHERE table_name = 'setting'",
		"key text NO -, value jsonb NO -, updated_at timestamp with time zone NO now()");
	assert_query(fx,
		"SELECT string_agg(a.attname, ',') FROM pg_index i JOIN pg_attribute a "
		"ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
		"WHERE i.indrelid = 'setting'::regclass AND i.indisprimary", "key");
	axis_test_db_exec(fx->db, "INSERT INTO setting (key, value) VALUES ('k', '{\"a\": 1}')");
	assert_query(fx, "SELECT value->>'a' FROM setting WHERE key = 'k'", "1");

	assert_lock_free(fx);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

/* Spec: Running again changes nothing */
static void
test_builtin_again(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	guint applied;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate(fx->pool, &error));
	applied = axis_test_log_count(G_LOG_LEVEL_INFO, "applied migration *");
	g_assert_cmpuint(applied, >=, 1);
	before = q(fx, "SELECT string_agg(version || applied_at::text, ',' ORDER BY version) "
		"FROM axis_schema_migration");

	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_no_error(error);

	after = q(fx, "SELECT string_agg(version || applied_at::text, ',' ORDER BY version) "
		"FROM axis_schema_migration");
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "applied migration *"), ==, applied);
	assert_lock_free(fx);
}

static const AxisDbMigration good[] = {
	M(1, "make_a", "CREATE TABLE a (x integer);\nINSERT INTO a VALUES (1);\n"),
	M(2, "make_b", "CREATE TABLE b (x integer);\n"),
	M(3, "make_c", "CREATE TABLE c (x integer);\n")
};

/* Spec: Failing migration leaves no trace */
static void
test_failing_migration(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration list[] = {
		M(1, "make_a", "CREATE TABLE a (x integer);\nINSERT INTO a VALUES (1);\n"),
		M(2, "half_done", "CREATE TABLE b (x integer);\nINSERT INTO b VALUES (1);\n"
			"UPDATE a SET x = 2;\nSELECT 1 / 0;\nCREATE TABLE never (x integer);\n"),
		M(3, "make_c", "CREATE TABLE c (x integer);\n")
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *recorded = NULL;

	if (fx->db == NULL)
		return;

	g_assert_false(axis_db_migrate_with(fx->pool, list, G_N_ELEMENTS(list), &error));
	assert_migration_error(error, "0002_half_done", "division by zero");
	g_clear_error(&error);

	/* None of 0002's statements took effect; 0001 stays; 0003 never ran. */
	assert_query(fx, "SELECT to_regclass('b') IS NULL", "t");
	assert_query(fx, "SELECT to_regclass('never') IS NULL", "t");
	assert_query(fx, "SELECT to_regclass('c') IS NULL", "t");
	assert_query(fx, "SELECT string_agg(x::text, ',') FROM a", "1");
	assert_query(fx, "SELECT string_agg(version::text, ',' ORDER BY version) FROM axis_schema_migration", "1");
	assert_lock_free(fx);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);

	/* Fixed, it goes through, and only the missing ones are applied. */
	g_assert_true(axis_db_migrate_with(fx->pool, good, G_N_ELEMENTS(good), &error));
	g_assert_no_error(error);
	assert_query(fx, "SELECT string_agg(version::text, ',' ORDER BY version) FROM axis_schema_migration", "1,2,3");
	assert_query(fx, "SELECT count(*) FROM a", "1");
}

typedef struct
{
	AxisTestDb *db;
	GMutex     *lock;
	GCond      *cond;
	gboolean   *go;
	gboolean    ok;
	gchar      *message;
} Runner;

static gpointer
runner_thread(gpointer data)
{
	static const AxisDbMigration slow[] = {
		M(1, "make_a", "CREATE TABLE a (x integer);\nSELECT pg_sleep(0.3);\n"),
		M(2, "make_b", "CREATE TABLE b (x integer);\nINSERT INTO b VALUES (1);\n"),
		M(3, "make_c", "CREATE TABLE c (x integer);\n")
	};
	Runner *r = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbPool) pool = NULL;

	/* Each runner its own pool, as two daemons would have. */
	pool = axis_db_pool_new(axis_test_db_get_url(r->db), 1, &error);
	g_assert_no_error(error);

	g_mutex_lock(r->lock);
	while (!*r->go)
		g_cond_wait(r->cond, r->lock);
	g_mutex_unlock(r->lock);

	r->ok = axis_db_migrate_with(pool, slow, G_N_ELEMENTS(slow), &error);
	if (error != NULL)
		r->message = g_strdup(error->message);

	return NULL;
}

/* Spec: Two runners at once */
static void
test_concurrent_runners(Fixture *fx, gconstpointer data)
{
	GMutex lock;
	GCond cond;
	gboolean go = FALSE;
	Runner runners[2];
	GThread *threads[2];
	guint i;

	if (fx->db == NULL)
		return;

	g_mutex_init(&lock);
	g_cond_init(&cond);
	for (i = 0; i < 2; i++) {
		memset(&runners[i], 0, sizeof(runners[i]));
		runners[i].db = fx->db;
		runners[i].lock = &lock;
		runners[i].cond = &cond;
		runners[i].go = &go;
		threads[i] = g_thread_new("runner", runner_thread, &runners[i]);
	}

	/* Both pools are connected before either is let go. */
	g_usleep(200 * 1000);
	g_mutex_lock(&lock);
	go = TRUE;
	g_cond_broadcast(&cond);
	g_mutex_unlock(&lock);

	for (i = 0; i < 2; i++) {
		g_thread_join(threads[i]);
		if (!runners[i].ok)
			g_error("runner %u failed: %s", i, runners[i].message);
		g_free(runners[i].message);
	}

	assert_query(fx, "SELECT string_agg(version || ':' || n, ',' ORDER BY version) FROM "
		"(SELECT version, count(*) AS n FROM axis_schema_migration GROUP BY version) s",
		"1:1,2:1,3:1");
	assert_query(fx, "SELECT count(*) FROM b", "1");
	assert_lock_free(fx);
	g_test_message("runners that waited for the lock: %u",
		axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*waiting for it to finish*"));

	g_mutex_clear(&lock);
	g_cond_clear(&cond);
}

/* A runner started while the lock is held waits, says so, and then finds
 * the work it would have done. */
static void
test_waits_for_lock(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *locked = NULL;
	g_autofree gchar *sql = NULL;
	GMutex lock;
	GCond cond;
	gboolean go = TRUE;
	Runner runner;
	GThread *thread;

	if (fx->db == NULL)
		return;

	sql = g_strdup_printf("SELECT pg_advisory_lock(%" G_GINT64_FORMAT ")", AXIS_DB_MIGRATION_LOCK_KEY);
	locked = q(fx, sql);

	g_mutex_init(&lock);
	g_cond_init(&cond);
	memset(&runner, 0, sizeof(runner));
	runner.db = fx->db;
	runner.lock = &lock;
	runner.cond = &cond;
	runner.go = &go;
	thread = g_thread_new("runner", runner_thread, &runner);

	/* Bounded wait for the "waiting" message, then let it go. */
	{
		gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

		while (axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*waiting for it to finish*") == 0) {
			g_assert_cmpint(g_get_monotonic_time(), <, deadline);
			g_usleep(10 * 1000);
		}
	}
	assert_query(fx, "SELECT to_regclass('axis_schema_migration') IS NULL", "t");

	g_free(sql);
	sql = g_strdup_printf("SELECT pg_advisory_unlock(%" G_GINT64_FORMAT ")", AXIS_DB_MIGRATION_LOCK_KEY);
	g_free(locked);
	locked = q(fx, sql);
	g_assert_cmpstr(locked, ==, "t");

	g_thread_join(thread);
	if (!runner.ok)
		g_error("runner failed: %s", runner.message);
	assert_query(fx, "SELECT count(*) FROM axis_schema_migration", "3");

	g_mutex_clear(&lock);
	g_cond_clear(&cond);
}

/* Spec: Applied migration was edited */
static void
test_edited_checksum(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration edited[] = {
		M(1, "make_a", "CREATE TABLE a (x integer);\nINSERT INTO a VALUES (1); \n"),
		M(2, "make_b", "CREATE TABLE b (x integer);\n")
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate_with(fx->pool, good, 1, &error));
	g_assert_no_error(error);
	before = history(fx);

	/* One added space is an edit. */
	g_assert_false(axis_db_migrate_with(fx->pool, edited, G_N_ELEMENTS(edited), &error));
	assert_migration_error(error, "0001_make_a", "changed since it was applied");

	after = history(fx);
	g_assert_cmpstr(after, ==, before);
	assert_query(fx, "SELECT to_regclass('b') IS NULL", "t");
	assert_lock_free(fx);
}

/* A renamed migration is refused the same way. */
static void
test_renamed(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration renamed[] = {
		M(1, "make_alpha", "CREATE TABLE a (x integer);\nINSERT INTO a VALUES (1);\n")
	};
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate_with(fx->pool, good, 1, &error));
	g_assert_false(axis_db_migrate_with(fx->pool, renamed, 1, &error));
	assert_migration_error(error, "make_a", "make_alpha");
}

/* Spec: Database is newer than the binary */
static void
test_newer_database(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate_with(fx->pool, good, 1, &error));
	axis_test_db_exec(fx->db,
		"INSERT INTO axis_schema_migration (version, name, checksum) "
		"VALUES (5, 'from_the_future', 'f00d')");

	g_assert_false(axis_db_migrate_with(fx->pool, good, G_N_ELEMENTS(good), &error));
	assert_migration_error(error, "newer version", "0005_from_the_future");

	/* Nothing applied: 0002 was pending and stays so. */
	assert_query(fx, "SELECT to_regclass('b') IS NULL", "t");
	assert_query(fx, "SELECT string_agg(version::text, ',' ORDER BY version) FROM axis_schema_migration", "1,5");
	assert_lock_free(fx);
}

static void
test_history_gap(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *checksum = axis_db_migration_checksum(good[2].sql);
	g_autofree gchar *sql = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate_with(fx->pool, good, 1, &error));
	sql = g_strdup_printf("INSERT INTO axis_schema_migration (version, name, checksum) "
		"VALUES (3, 'make_c', '%s')", checksum);
	axis_test_db_exec(fx->db, sql);

	g_assert_false(axis_db_migrate_with(fx->pool, good, G_N_ELEMENTS(good), &error));
	assert_migration_error(error, "gap", "0002");
	assert_query(fx, "SELECT to_regclass('b') IS NULL", "t");
}

/* The marker runs a migration outside a transaction (CREATE INDEX
 * CONCURRENTLY refuses to run inside one), and records it afterwards. */
static void
test_no_transaction_marker(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration with_marker[] = {
		M(1, "make_t", "CREATE TABLE t (x integer, y integer);\n"),
		M_NOTX(2, "index_x", "-- axis:no-transaction\n-- SPDX-License-Identifier: AGPL-3.0-or-later\n"
			"CREATE INDEX CONCURRENTLY t_x ON t (x);\n")
	};
	static const AxisDbMigration without_marker[] = {
		M(1, "make_t", "CREATE TABLE t (x integer, y integer);\n"),
		M_NOTX(2, "index_x", "-- axis:no-transaction\n-- SPDX-License-Identifier: AGPL-3.0-or-later\n"
			"CREATE INDEX CONCURRENTLY t_x ON t (x);\n"),
		M(3, "index_y", "CREATE INDEX CONCURRENTLY t_y ON t (y);\n")
	};
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate_with(fx->pool, with_marker, G_N_ELEMENTS(with_marker), &error));
	g_assert_no_error(error);
	assert_query(fx, "SELECT indisvalid FROM pg_index WHERE indexrelid = 't_x'::regclass", "t");
	assert_query(fx, "SELECT string_agg(version::text, ',' ORDER BY version) FROM axis_schema_migration", "1,2");

	/* Without the marker the same statement runs in a transaction, which
	 * the server refuses. */
	g_assert_false(axis_db_migrate_with(fx->pool, without_marker, G_N_ELEMENTS(without_marker), &error));
	assert_migration_error(error, "0003_index_y", "transaction block");
	assert_query(fx, "SELECT to_regclass('t_y') IS NULL", "t");
	assert_lock_free(fx);
}

static void
test_no_transaction_failures(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration failing[] = {
		M(1, "make_t", "CREATE TABLE t (x integer);\n"),
		M_NOTX(2, "index_bad", "-- axis:no-transaction\nCREATE INDEX CONCURRENTLY t_bad ON t (nope);\n")
	};
	/* More than one statement is refused whole: neither table appears. */
	static const AxisDbMigration two_statements[] = {
		M(1, "make_t", "CREATE TABLE t (x integer);\n"),
		M_NOTX(2, "two", "-- axis:no-transaction\nCREATE TABLE u (x integer);\nCREATE TABLE v (x integer);\n")
	};
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	g_assert_false(axis_db_migrate_with(fx->pool, failing, G_N_ELEMENTS(failing), &error));
	assert_migration_error(error, "0002_index_bad", "nope");
	g_clear_error(&error);
	assert_query(fx, "SELECT string_agg(version::text, ',' ORDER BY version) FROM axis_schema_migration", "1");

	g_assert_false(axis_db_migrate_with(fx->pool, two_statements, G_N_ELEMENTS(two_statements), &error));
	assert_migration_error(error, "0002_two", "multiple commands");
	assert_query(fx, "SELECT to_regclass('u') IS NULL AND to_regclass('v') IS NULL", "t");
	assert_query(fx, "SELECT string_agg(version::text, ',' ORDER BY version) FROM axis_schema_migration", "1");
	assert_lock_free(fx);
}

/* A migration that ends the transaction itself is not recorded and is
 * reported. */
static void
test_migration_ends_transaction(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration list[] = {
		M(1, "sneaky", "CREATE TABLE a (x integer);\nCOMMIT;\nCREATE TABLE b (x integer);\n")
	};
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	g_assert_false(axis_db_migrate_with(fx->pool, list, 1, &error));
	assert_migration_error(error, "0001_sneaky", "ended the transaction");
	assert_query(fx, "SELECT count(*) FROM axis_schema_migration", "0");
	assert_lock_free(fx);
}

/* statement_timeout is off while migrating, and back on afterwards. */
static void
test_statement_timeout_disabled(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration list[] = {
		M(1, "check_timeout",
			"DO $$ BEGIN\n"
			"  IF current_setting('statement_timeout') <> '0' THEN\n"
			"    RAISE EXCEPTION 'statement_timeout is %', current_setting('statement_timeout');\n"
			"  END IF;\n"
			"END $$;\n")
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_migrate_with(fx->pool, list, 1, &error));
	g_assert_no_error(error);

	g_assert_true(axis_db_pool_exec(fx->pool, "SELECT current_setting('statement_timeout')",
		0, NULL, &result, &error));
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 0), ==, "1min");
}

/* A list that breaks the rules is refused before the database is touched. */
static void
test_invalid_lists(Fixture *fx, gconstpointer data)
{
	static const AxisDbMigration gap[] = {
		M(1, "a", "SELECT 1;"),
		M(3, "c", "SELECT 1;")
	};
	static const AxisDbMigration from_two[] = {
		M(2, "b", "SELECT 1;")
	};
	static const AxisDbMigration bad_name[] = {
		M(1, "Bad-Name", "SELECT 1;")
	};
	static const AxisDbMigration flag_without_marker[] = {
		M_NOTX(1, "a", "SELECT 1;")
	};
	static const AxisDbMigration marker_without_flag[] = {
		M(1, "a", "-- axis:no-transaction\nSELECT 1;")
	};
	static const AxisDbMigration no_sql[] = {
		M(1, "a", NULL)
	};
	static const struct { const AxisDbMigration *list; guint n; const gchar *needle; } cases[] = {
		{ gap, 2, "without gaps" },
		{ from_two, 1, "without gaps" },
		{ bad_name, 1, "invalid name" },
		{ flag_without_marker, 1, "marker" },
		{ marker_without_flag, 1, "marker" },
		{ no_sql, 1, "no SQL" }
	};
	guint i;

	if (fx->db == NULL)
		return;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_db_migrate_with(fx->pool, cases[i].list, cases[i].n, &error));
		assert_migration_error(error, cases[i].needle, NULL);
	}

	assert_query(fx, "SELECT to_regclass('axis_schema_migration') IS NULL", "t");
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

/*
 * Spec (deployment): ready only at the expected schema version -- before
 * the migrations, after them, and when a newer build has migrated it.
 */
static void
test_check_current(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *files = NULL;
	g_autofree gchar *count = NULL;
	guint latest = axis_db_migrate_latest_version();

	if (fx->db == NULL)
		return;

	/* Nothing applied yet: not current. */
	g_assert_false(axis_db_migrate_check_current(fx->pool, 2000, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_cmpuint(latest, >, 0);
	count = q(fx, "SELECT max(version)::text FROM axis_schema_migration");
	files = g_strdup_printf("%u", latest);
	g_assert_cmpstr(count, ==, files);
	g_assert_true(axis_db_migrate_check_current(fx->pool, 2000, &error));
	g_assert_no_error(error);

	axis_test_db_exec(fx->db, "INSERT INTO axis_schema_migration (version, name, checksum) "
		"VALUES (9999, 'from_a_newer_build', repeat('0', 64))");
	g_assert_false(axis_db_migrate_check_current(fx->pool, 2000, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_MIGRATION);
	g_assert_nonnull(strstr(error->message, "9999"));
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

/*
 * add-request-portal, slice P1 (task 1.1): 0083_requests.sql on a database
 * that has accounts already.  The accounts keep everything they had and get
 * the two switches off and no limit of their own; and the rules the tables
 * hold by themselves hold: one open request for a movie and for a season, a
 * season's "open" following its request's decision.
 */
static void
test_requests_on_accounts(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GPtrArray) keep = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GArray) before = g_array_new(FALSE, TRUE, sizeof(AxisDbMigration));
	g_autofree gchar *title = NULL;
	g_autofree gchar *accounts = NULL;
	const gchar *entry;
	guint requests_version = 0;
	guint i;

	if (fx->db == NULL)
		return;

	dir = g_dir_open(AXIS_MIGRATION_DIR, 0, &error);
	g_assert_no_error(error);
	while ((entry = g_dir_read_name(dir)) != NULL)
		g_ptr_array_add(files, g_strdup(entry));
	g_ptr_array_sort_values(files, (GCompareFunc)strcmp);
	/* Every migration before the requests', as an older daemon applied them. */
	for (i = 0; i < files->len; i++) {
		const gchar *file = g_ptr_array_index(files, i);
		g_autofree gchar *path = g_build_filename(AXIS_MIGRATION_DIR, file, NULL);
		AxisDbMigration migration;
		gchar *contents = NULL;
		gchar *name = NULL;
		guint version = 0;

		g_assert_true(axis_db_migration_parse_filename(file, &version, &name));
		g_ptr_array_add(keep, name);
		if (g_strcmp0(name, "requests") == 0) {
			requests_version = version;
			break;
		}
		g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
		g_ptr_array_add(keep, contents);
		memset(&migration, 0, sizeof(migration));
		migration.version = version;
		migration.name = name;
		migration.sql = contents;
		migration.no_transaction = axis_db_migration_has_no_transaction_marker(contents);
		g_array_append_val(before, migration);
	}
	g_assert_cmpuint(requests_version, >, 1);
	g_assert_true(axis_db_migrate_with(fx->pool, (const AxisDbMigration *)before->data, before->len, &error));
	g_assert_no_error(error);
	assert_query(fx, "SELECT to_regclass('request') IS NULL", "t");
	axis_test_db_exec(fx->db, "INSERT INTO app_user (username, password_hash, role, disabled, auth_epoch) VALUES"
		" ('root', '$argon2id$not-a-password', 'admin', false, 3), ('robin', '$argon2id$not-a-password', 'user', true, 7)");
	axis_test_db_exec(fx->db, "INSERT INTO meta_title (kind, title, provider, provider_id) VALUES"
		" ('film', 'A Made-Up Film', 'fixture', 'm1'), ('series', 'A Made-Up Series', 'fixture', 's1')");
	axis_test_db_exec(fx->db, "INSERT INTO web_session (token_hash, public_id, user_id, user_epoch, created_at,"
		" last_seen_at, client_address, user_agent) SELECT repeat('a', 64), 'AAAAAAAAAAAAAAAAAAAAAA', id, auth_epoch,"
		" now(), now(), '192.0.2.1', 'test' FROM app_user WHERE username = 'root'");

	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_no_error(error);
	/* Slice P2 (task 2.2), 0084_title_adult.sql and 0085_portal_api.sql on that
	 * database: a stored title's adult mark is not known (NULL: no provider's
	 * answer has said yet), a session that existed is one of the engine's
	 * interface, an account can be a requester, and only the three roles and
	 * the two kinds are taken. */
	assert_query(fx, "SELECT count(*) FILTER (WHERE adult IS NULL) || ' ' || count(*) FROM meta_title", "2 2");
	assert_query(fx, "SELECT kind FROM web_session", "web");
	axis_test_db_exec(fx->db, "INSERT INTO app_user (username, password_hash, role) VALUES"
		" ('quill', '$argon2id$not-a-password', 'requester')");
	axis_test_db_exec(fx->db, "DELETE FROM app_user WHERE username = 'quill'");
	assert_query(fx, "WITH tried AS (INSERT INTO portal_token (token_hash, created_by) VALUES (repeat('b', 64), 'root')"
		" RETURNING revoked_at IS NULL AND last_used_at IS NULL) SELECT count(*) FROM tried", "1");
	{
		static const gchar *const refused[] = {
			"INSERT INTO app_user (username, password_hash, role) VALUES ('wren', '$argon2id$not-a-password', 'owner')",
			"UPDATE web_session SET kind = 'other'",
			"INSERT INTO portal_token (token_hash, created_by) VALUES ('not-a-hash', 'root')",
			"INSERT INTO portal_token (token_hash, created_by) VALUES (repeat('b', 64), 'root')",
			"UPDATE portal_token SET revoked_at = now()",
			NULL
		};

		for (i = 0; refused[i] != NULL; i++) {
			g_autoptr(GError) local = NULL;

			g_assert_false(axis_db_pool_exec(fx->pool, refused[i], 0, NULL, NULL, &local));
			g_assert_nonnull(local);
		}
	}
	accounts = q(fx, "SELECT string_agg(username || ' ' || role || ' ' || disabled || ' ' || auth_epoch || ' ' ||"
		" request_auto_movies || ' ' || request_auto_series || ' ' || coalesce(request_movies_limit::text, '-') || ' '"
		" || coalesce(request_seasons_limit::text, '-'), ', ' ORDER BY username) FROM app_user");
	g_assert_cmpstr(accounts, ==, "robin user true 7 false false - -, root admin false 3 false false - -");

	/* One open request for a movie: a second is refused by the database. */
	axis_test_db_exec(fx->db, "INSERT INTO request (public_id, title_id, kind, requested_by, requested_name, created_at,"
		" counted_at) SELECT 'AAAAAAAAAAAAAAAAAAAAA1', t.id, 'movie', u.id, u.username, now(), now()"
		" FROM meta_title t, app_user u WHERE t.provider_id = 'm1' AND u.username = 'robin'");
	assert_query(fx, "WITH tried AS (INSERT INTO request (public_id, title_id, kind, requested_name, created_at,"
		" counted_at) SELECT 'AAAAAAAAAAAAAAAAAAAAA2', id, 'movie', 'robin', now(), now() FROM meta_title"
		" WHERE provider_id = 'm1' ON CONFLICT DO NOTHING RETURNING 1) SELECT count(*) FROM tried", "0");
	/* Declined, it is open no more, and the title can be asked for again. */
	axis_test_db_exec(fx->db, "UPDATE request SET decision = 'declined' WHERE public_id = 'AAAAAAAAAAAAAAAAAAAAA1'");
	assert_query(fx, "WITH tried AS (INSERT INTO request (public_id, title_id, kind, requested_name, created_at,"
		" counted_at) SELECT 'AAAAAAAAAAAAAAAAAAAAA2', id, 'movie', 'robin', now(), now() FROM meta_title"
		" WHERE provider_id = 'm1' ON CONFLICT DO NOTHING RETURNING 1) SELECT count(*) FROM tried", "1");

	/* One open request for a season; a season's "open" follows its request. */
	title = q(fx, "SELECT id::text FROM meta_title WHERE provider_id = 's1'");
	axis_test_db_exec(fx->db, "INSERT INTO request (public_id, title_id, kind, requested_name, created_at, counted_at)"
		" SELECT 'AAAAAAAAAAAAAAAAAAAAA3', id, 'series', 'robin', now(), now() FROM meta_title WHERE provider_id = 's1'");
	axis_test_db_exec(fx->db, "INSERT INTO request (public_id, title_id, kind, requested_name, created_at, counted_at)"
		" SELECT 'AAAAAAAAAAAAAAAAAAAAA4', id, 'series', 'robin', now(), now() FROM meta_title WHERE provider_id = 's1'");
	axis_test_db_exec(fx->db, "INSERT INTO request_season (request_id, title_id, season) SELECT id, title_id, 1"
		" FROM request WHERE public_id = 'AAAAAAAAAAAAAAAAAAAAA3'");
	assert_query(fx, "WITH tried AS (INSERT INTO request_season (request_id, title_id, season) SELECT id, title_id, 1"
		" FROM request WHERE public_id = 'AAAAAAAAAAAAAAAAAAAAA4' ON CONFLICT DO NOTHING RETURNING 1)"
		" SELECT count(*) FROM tried", "0");
	axis_test_db_exec(fx->db, "UPDATE request SET decision = 'withdrawn' WHERE public_id = 'AAAAAAAAAAAAAAAAAAAAA3'");
	assert_query(fx, "SELECT bool_or(open)::text FROM request_season", "false");
	assert_query(fx, "WITH tried AS (INSERT INTO request_season (request_id, title_id, season) SELECT id, title_id, 1"
		" FROM request WHERE public_id = 'AAAAAAAAAAAAAAAAAAAAA4' ON CONFLICT DO NOTHING RETURNING 1)"
		" SELECT count(*) FROM tried", "1");

	/* An account that is deleted leaves its requests, naming no one. */
	axis_test_db_exec(fx->db, "DELETE FROM app_user WHERE username = 'robin'");
	assert_query(fx, "SELECT count(*) FROM request WHERE public_id = 'AAAAAAAAAAAAAAAAAAAAA1'"
		" AND requested_by IS NULL AND requested_name = 'robin'", "1");
	assert_lock_free(fx);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("/db/migrate/builtin-from-empty", test_builtin_from_empty);
	ADD("/db/migrate/builtin-again", test_builtin_again);
	ADD("/db/migrate/failing-migration", test_failing_migration);
	ADD("/db/migrate/concurrent-runners", test_concurrent_runners);
	ADD("/db/migrate/waits-for-lock", test_waits_for_lock);
	ADD("/db/migrate/edited-checksum", test_edited_checksum);
	ADD("/db/migrate/renamed", test_renamed);
	ADD("/db/migrate/newer-database", test_newer_database);
	ADD("/db/migrate/history-gap", test_history_gap);
	ADD("/db/migrate/no-transaction-marker", test_no_transaction_marker);
	ADD("/db/migrate/no-transaction-failures", test_no_transaction_failures);
	ADD("/db/migrate/migration-ends-transaction", test_migration_ends_transaction);
	ADD("/db/migrate/statement-timeout-disabled", test_statement_timeout_disabled);
	ADD("/db/migrate/invalid-lists", test_invalid_lists);
	ADD("/db/migrate/check-current", test_check_current);
	ADD("/db/migrate/requests-on-accounts", test_requests_on_accounts);

	return g_test_run();
}
