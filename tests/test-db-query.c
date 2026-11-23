/*
 * test-db-query.c - statements, results and transactions against PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"
#include "db/axis-db-pool.h"

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
	AxisDbConn *conn;
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
	fx->conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->conn != NULL)
		axis_db_pool_release(fx->pool, fx->conn);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

static void
exec_ok(
	Fixture     *fx,
	const gchar *sql
){
	g_autoptr(GError) error = NULL;

	if (!axis_db_conn_exec(fx->conn, sql, 0, NULL, NULL, &error))
		g_error("%s: %s", sql, error->message);
}

static gchar *
query_text(
	Fixture     *fx,
	const gchar *sql,
	const gchar *param
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[1];

	params[0] = param;
	if (!axis_db_conn_exec(fx->conn, sql, param != NULL ? 1 : 0, params, &result, &error))
		g_error("%s: %s", sql, error->message);
	if (axis_db_result_get_n_rows(result) == 0)
		return NULL;

	return g_strdup(axis_db_result_get_text(result, 0, 0));
}

/* Spec: Hostile parameter is stored verbatim */
static void
test_hostile_parameter(Fixture *fx, gconstpointer data)
{
	static const gchar *const hostile[] = {
		"'); DROP TABLE item; --",
		"\"; DELETE FROM item; SELECT '",
		"x' OR '1'='1",
		"$1 $2 $$ $tag$ E'\\x41' \\ \\\\ ;;; -- /* */",
		"Robert'); DROP TABLE item;--\n\tSELECT pg_sleep(10);",
		"caf\xc3\xa9 \xf0\x9f\x8e\xac \xe2\x80\x8b",
		"COMMIT; BEGIN; TRUNCATE item; COMMIT;",
		""
	};
	g_autoptr(GError) error = NULL;
	guint i;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer PRIMARY KEY, value text)");

	for (i = 0; i < G_N_ELEMENTS(hostile); i++) {
		g_autoptr(AxisDbResult) result = NULL;
		g_autofree gchar *id = g_strdup_printf("%u", i);
		const gchar *params[2];

		params[0] = id;
		params[1] = hostile[i];
		g_assert_true(axis_db_conn_exec(fx->conn,
			"INSERT INTO item (id, value) VALUES ($1, $2)", 2, params, &result, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(axis_db_result_get_affected_rows(result), ==, 1);
	}

	/* Every value comes back exactly, and nothing else happened. */
	for (i = 0; i < G_N_ELEMENTS(hostile); i++) {
		g_autofree gchar *id = g_strdup_printf("%u", i);
		g_autofree gchar *value = query_text(fx, "SELECT value FROM item WHERE id = $1", id);

		g_assert_cmpstr(value, ==, hostile[i]);
	}
	{
		g_autofree gchar *count = query_text(fx, "SELECT count(*) FROM item", NULL);

		g_assert_cmpuint((guint)g_ascii_strtoull(count, NULL, 10), ==, G_N_ELEMENTS(hostile));
	}

	/* A value used in a WHERE clause is a value, not SQL. */
	{
		g_autofree gchar *n = query_text(fx, "SELECT count(*) FROM item WHERE value = $1",
			"x' OR '1'='1");

		g_assert_cmpstr(n, ==, "1");
	}
}

/* Text with two statements is refused as a whole: the second never runs. */
static void
test_one_statement_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *exists = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer)");
	g_assert_false(axis_db_conn_exec(fx->conn, "SELECT 1; DROP TABLE item", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_test_message("two statements: %s", error->message);

	exists = query_text(fx, "SELECT to_regclass('item') IS NOT NULL", NULL);
	g_assert_cmpstr(exists, ==, "t");
}

/* A hostile name, quoted, is one identifier spelled exactly as given. */
static void
test_quoted_identifier(Fixture *fx, gconstpointer data)
{
	static const gchar name[] = "x\"; DROP TABLE item; --";
	g_autofree gchar *quoted = axis_db_quote_identifier(name);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *found = NULL;
	g_autofree gchar *still = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer)");
	sql = g_strdup_printf("CREATE TABLE %s (id integer)", quoted);
	exec_ok(fx, sql);

	found = query_text(fx, "SELECT relname FROM pg_class WHERE relname = $1", name);
	g_assert_cmpstr(found, ==, name);
	still = query_text(fx, "SELECT to_regclass('item') IS NOT NULL", NULL);
	g_assert_cmpstr(still, ==, "t");
}

/* Spec: NULL parameters and results */
static void
test_null_round_trip(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[2];
	gint64 n = 99;
	gboolean b = TRUE;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer PRIMARY KEY, value text)");

	params[0] = "1";
	params[1] = NULL;
	g_assert_true(axis_db_conn_exec(fx->conn, "INSERT INTO item VALUES ($1, $2)", 2, params, NULL, &error));
	params[0] = "2";
	params[1] = "";
	g_assert_true(axis_db_conn_exec(fx->conn, "INSERT INTO item VALUES ($1, $2)", 2, params, NULL, &error));
	g_assert_no_error(error);

	g_assert_true(axis_db_conn_exec(fx->conn,
		"SELECT value, value IS NULL FROM item ORDER BY id", 0, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_db_result_get_n_rows(result), ==, 2);

	/* Row 0: NULL, and the server agrees it stored NULL. */
	g_assert_true(axis_db_result_is_null(result, 0, 0));
	g_assert_null(axis_db_result_get_text(result, 0, 0));
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 1), ==, "t");
	g_assert_false(axis_db_result_get_int64(result, 0, 0, &n));
	g_assert_false(axis_db_result_get_boolean(result, 0, 0, &b));
	g_assert_cmpint(n, ==, 99);
	g_assert_true(b);

	/* Row 1: the empty string, which is not NULL. */
	g_assert_false(axis_db_result_is_null(result, 1, 0));
	g_assert_cmpstr(axis_db_result_get_text(result, 1, 0), ==, "");
	g_assert_cmpstr(axis_db_result_get_text(result, 1, 1), ==, "f");
}

static void
test_typed_getters(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 n = 0;
	gboolean b = FALSE;
	guint i;
	static const guint not_integers[] = { 6, 7, 8, 9, 10, 11 };

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_conn_exec(fx->conn,
		"SELECT 42::bigint AS answer,"		/* 0 */
		" (-9223372036854775808)::bigint AS min,"	/* 1 */
		" 9223372036854775807::bigint AS max,"	/* 2 */
		" true AS yes, false AS no,"		/* 3, 4 */
		" (-7)::smallint AS small,"		/* 5 */
		" 'abc'::text AS word,"			/* 6 */
		" ' 1'::text AS spaced,"			/* 7 */
		" '1.5'::text AS fraction,"		/* 8 */
		" '9223372036854775808'::text AS too_big,"	/* 9 */
		" '+1'::text AS plus,"			/* 10 */
		" '-'::text AS minus,"			/* 11 */
		" 'true'::text AS true_word,"		/* 12 */
		" NULL::int AS nothing",			/* 13 */
		0, NULL, &result, &error));
	g_assert_no_error(error);

	g_assert_cmpuint(axis_db_result_get_n_rows(result), ==, 1);
	g_assert_cmpuint(axis_db_result_get_n_columns(result), ==, 14);
	g_assert_cmpint(axis_db_result_get_column_index(result, "answer"), ==, 0);
	g_assert_cmpint(axis_db_result_get_column_index(result, "nothing"), ==, 13);
	g_assert_cmpint(axis_db_result_get_column_index(result, "ANSWER"), ==, -1);
	g_assert_cmpint(axis_db_result_get_column_index(result, "missing"), ==, -1);

	g_assert_true(axis_db_result_get_int64(result, 0, 0, &n));
	g_assert_cmpint(n, ==, 42);
	g_assert_true(axis_db_result_get_int64(result, 0, 1, &n));
	g_assert_cmpint(n, ==, G_MININT64);
	g_assert_true(axis_db_result_get_int64(result, 0, 2, &n));
	g_assert_cmpint(n, ==, G_MAXINT64);
	g_assert_true(axis_db_result_get_int64(result, 0, 5, &n));
	g_assert_cmpint(n, ==, -7);

	for (i = 0; i < G_N_ELEMENTS(not_integers); i++) {
		n = 1234;
		g_assert_false(axis_db_result_get_int64(result, 0, not_integers[i], &n));
		g_assert_cmpint(n, ==, 1234);
	}

	g_assert_true(axis_db_result_get_boolean(result, 0, 3, &b));
	g_assert_true(b);
	g_assert_true(axis_db_result_get_boolean(result, 0, 4, &b));
	g_assert_false(b);
	b = TRUE;
	g_assert_false(axis_db_result_get_boolean(result, 0, 6, &b));
	g_assert_false(axis_db_result_get_boolean(result, 0, 12, &b));
	g_assert_false(axis_db_result_get_boolean(result, 0, 0, &b));
	g_assert_true(b);

	g_assert_cmpstr(axis_db_result_get_text(result, 0, 6), ==, "abc");
	g_assert_true(axis_db_result_is_null(result, 0, 13));
}

static void
test_out_of_range(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*out of range*");
	g_assert_true(axis_db_conn_exec(fx->conn, "SELECT 1", 0, NULL, &result, &error));
	g_assert_null(axis_db_result_get_text(result, 1, 0));
	g_assert_null(axis_db_result_get_text(result, 0, 1));
	g_assert_true(axis_db_result_is_null(result, 5, 5));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_CRITICAL, "*out of range*"), ==, 3);
}

static void
test_affected_rows(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbResult *result = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_conn_exec(fx->conn, "CREATE TABLE item (id integer)", 0, NULL, &result, &error));
	g_assert_cmpuint(axis_db_result_get_affected_rows(result), ==, 0);
	g_assert_cmpuint(axis_db_result_get_n_rows(result), ==, 0);
	axis_db_result_free(result);

	g_assert_true(axis_db_conn_exec(fx->conn,
		"INSERT INTO item SELECT generate_series(1, 5)", 0, NULL, &result, &error));
	g_assert_cmpuint(axis_db_result_get_affected_rows(result), ==, 5);
	axis_db_result_free(result);

	g_assert_true(axis_db_conn_exec(fx->conn, "UPDATE item SET id = id + 10 WHERE id > 2",
		0, NULL, &result, &error));
	g_assert_cmpuint(axis_db_result_get_affected_rows(result), ==, 3);
	axis_db_result_free(result);

	g_assert_true(axis_db_conn_exec(fx->conn, "DELETE FROM item WHERE false", 0, NULL, &result, &error));
	g_assert_cmpuint(axis_db_result_get_affected_rows(result), ==, 0);
	axis_db_result_free(result);

	g_assert_true(axis_db_conn_exec(fx->conn, "SELECT * FROM item", 0, NULL, &result, &error));
	g_assert_cmpuint(axis_db_result_get_affected_rows(result), ==, 5);
	g_assert_cmpuint(axis_db_result_get_n_rows(result), ==, 5);
	axis_db_result_free(result);
	g_assert_no_error(error);
}

/* A result outlives the connection it came from. */
static void
test_result_outlives_release(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_conn_exec(fx->conn, "SELECT 'kept'", 0, NULL, &result, &error));
	axis_db_pool_release(fx->pool, fx->conn);
	fx->conn = NULL;
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 0), ==, "kept");
}

/* Spec: Rollback discards changes */
static void
test_rollback_discards(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *outside = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer)");

	g_assert_false(axis_db_conn_in_transaction(fx->conn));
	g_assert_true(axis_db_conn_begin(fx->conn, &error));
	g_assert_true(axis_db_conn_in_transaction(fx->conn));
	exec_ok(fx, "INSERT INTO item VALUES (1)");
	g_free(count);
	count = query_text(fx, "SELECT count(*) FROM item", NULL);
	g_assert_cmpstr(count, ==, "1");
	g_assert_true(axis_db_conn_rollback(fx->conn, &error));
	g_assert_no_error(error);
	g_assert_false(axis_db_conn_in_transaction(fx->conn));

	g_free(count);
	count = query_text(fx, "SELECT count(*) FROM item", NULL);
	g_assert_cmpstr(count, ==, "0");
	/* Seen from another session too. */
	outside = axis_test_db_query(fx->db, "SELECT count(*) FROM item");
	g_assert_cmpstr(outside, ==, "0");

	/* Rolling back with nothing open does nothing, successfully. */
	g_assert_true(axis_db_conn_rollback(fx->conn, &error));
	g_assert_no_error(error);
}

static void
test_commit_keeps(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *outside = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer)");
	g_assert_true(axis_db_conn_begin(fx->conn, &error));
	exec_ok(fx, "INSERT INTO item VALUES (1)");
	exec_ok(fx, "INSERT INTO item VALUES (2)");
	g_assert_true(axis_db_conn_commit(fx->conn, &error));
	g_assert_no_error(error);
	g_assert_false(axis_db_conn_in_transaction(fx->conn));

	outside = axis_test_db_query(fx->db, "SELECT count(*) FROM item");
	g_assert_cmpstr(outside, ==, "2");
}

static void
test_transaction_misuse(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *outside = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer)");

	/* No nesting. */
	g_assert_true(axis_db_conn_begin(fx->conn, &error));
	g_assert_false(axis_db_conn_begin(fx->conn, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_clear_error(&error);
	g_assert_true(axis_db_conn_in_transaction(fx->conn));

	/* A failed statement aborts the transaction: commit rolls back and
	 * says so. */
	exec_ok(fx, "INSERT INTO item VALUES (1)");
	g_assert_false(axis_db_conn_exec(fx->conn, "INSERT INTO nowhere VALUES (1)", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_assert_nonnull(strstr(error->message, "42P01"));
	g_clear_error(&error);
	g_assert_true(axis_db_conn_in_transaction(fx->conn));
	g_assert_false(axis_db_conn_commit(fx->conn, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_assert_nonnull(strstr(error->message, "rolled back"));
	g_clear_error(&error);
	g_assert_false(axis_db_conn_in_transaction(fx->conn));
	outside = axis_test_db_query(fx->db, "SELECT count(*) FROM item");
	g_assert_cmpstr(outside, ==, "0");

	/* Commit with nothing open. */
	g_assert_false(axis_db_conn_commit(fx->conn, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
}

/* A commit the server refuses (a deferred constraint) fails, and the
 * transaction is gone. */
static void
test_commit_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *outside = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer, CONSTRAINT one UNIQUE (id) DEFERRABLE INITIALLY DEFERRED)");
	g_assert_true(axis_db_conn_begin(fx->conn, &error));
	exec_ok(fx, "INSERT INTO item VALUES (1)");
	exec_ok(fx, "INSERT INTO item VALUES (1)");
	g_assert_false(axis_db_conn_commit(fx->conn, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION);
	g_clear_error(&error);
	g_assert_false(axis_db_conn_in_transaction(fx->conn));
	outside = axis_test_db_query(fx->db, "SELECT count(*) FROM item");
	g_assert_cmpstr(outside, ==, "0");
}

static void
test_unique_violation(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	exec_ok(fx, "CREATE TABLE item (id integer PRIMARY KEY)");
	exec_ok(fx, "INSERT INTO item VALUES (7)");
	g_assert_false(axis_db_conn_exec(fx->conn, "INSERT INTO item VALUES (7)", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_UNIQUE_VIOLATION);
	g_test_message("unique violation: %s", error->message);
	g_assert_nonnull(strstr(error->message, "23505"));
	g_assert_nonnull(strstr(error->message, "(id)=(7)"));
}

static void
test_pool_exec(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[1] = { "hello" };

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_pool_exec(fx->pool, "SELECT upper($1)", 1, params, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 0), ==, "HELLO");
	/* The fixture holds one connection; pool_exec gave its own back. */
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 1);

	g_assert_false(axis_db_pool_exec(fx->pool, "SELEC 1", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 1);
}

/* Statements that would leave the connection in a state nothing here can
 * handle are refused, and the connection is not reused. */
static void
test_unsupported_statements(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	const gchar **params;
	guint opened;

	if (fx->db == NULL)
		return;

	g_assert_false(axis_db_conn_exec(fx->conn, "", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_clear_error(&error);

	params = g_new0(const gchar *, AXIS_DB_MAX_PARAMS + 1);
	g_assert_false(axis_db_conn_exec(fx->conn, "SELECT 1", AXIS_DB_MAX_PARAMS + 1, params, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_clear_error(&error);
	g_free(params);

	exec_ok(fx, "CREATE TABLE item (id integer)");
	opened = axis_db_pool_get_connections_opened(fx->pool);
	g_assert_false(axis_db_conn_exec(fx->conn, "COPY item FROM STDIN", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_clear_error(&error);

	axis_db_pool_release(fx->pool, fx->conn);
	fx->conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	exec_ok(fx, "SELECT 1");
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, opened + 1);
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("/db/query/hostile-parameter", test_hostile_parameter);
	ADD("/db/query/one-statement-only", test_one_statement_only);
	ADD("/db/query/quoted-identifier", test_quoted_identifier);
	ADD("/db/query/null-round-trip", test_null_round_trip);
	ADD("/db/query/typed-getters", test_typed_getters);
	ADD("/db/query/out-of-range", test_out_of_range);
	ADD("/db/query/affected-rows", test_affected_rows);
	ADD("/db/query/result-outlives-release", test_result_outlives_release);
	ADD("/db/query/pool-exec", test_pool_exec);
	ADD("/db/query/unique-violation", test_unique_violation);
	ADD("/db/query/unsupported-statements", test_unsupported_statements);
	ADD("/db/transaction/rollback-discards", test_rollback_discards);
	ADD("/db/transaction/commit-keeps", test_commit_keeps);
	ADD("/db/transaction/misuse", test_transaction_misuse);
	ADD("/db/transaction/commit-refused", test_commit_refused);

	return g_test_run();
}
