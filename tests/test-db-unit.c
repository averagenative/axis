/*
 * test-db-unit.c - database layer parts that need no server
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Server version check, migration file names and markers, checksums,
 * identifier quoting, and the errors for an unreachable server or an
 * invalid URL (which must never contain the password).
 */

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"

#define SECRET "hunter2pw"

/* A port on 127.0.0.1 that nothing listens on. */
static guint16
closed_port(void)
{
	g_autoptr(GSocket) socket = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketAddress) bound = NULL;
	g_autoptr(GError) error = NULL;
	guint16 port;

	socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_DEFAULT, &error);
	g_assert_no_error(error);
	address = g_inet_socket_address_new_from_string("127.0.0.1", 0);
	g_socket_bind(socket, address, FALSE, &error);
	g_assert_no_error(error);
	bound = g_socket_get_local_address(socket, &error);
	g_assert_no_error(error);
	port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
	g_socket_close(socket, NULL);

	return port;
}

static void
test_version_check(void)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_db_check_server_version(150000, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_check_server_version(150013, NULL));
	g_assert_true(axis_db_check_server_version(170004, NULL));
	g_assert_true(axis_db_check_server_version(180000, NULL));

	g_assert_false(axis_db_check_server_version(140010, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_VERSION);
	/* The found and the required version are both stated. */
	g_assert_nonnull(strstr(error->message, "14.10"));
	g_assert_nonnull(strstr(error->message, "15"));
	g_clear_error(&error);

	g_assert_false(axis_db_check_server_version(149999, NULL));

	g_assert_false(axis_db_check_server_version(90624, &error));
	g_assert_nonnull(strstr(error->message, "9.6.24"));
	g_clear_error(&error);

	g_assert_false(axis_db_check_server_version(0, &error));
	g_assert_nonnull(strstr(error->message, "unknown"));
}

static void
test_version_format(void)
{
	static const struct { gint version; const gchar *text; } cases[] = {
		{ 170004, "17.4" },
		{ 150000, "15.0" },
		{ 100001, "10.1" },
		{ 90624,  "9.6.24" },
		{ 80423,  "8.4.23" },
		{ 0,      "unknown" },
		{ -1,     "unknown" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *text = axis_db_format_server_version(cases[i].version);

		g_assert_cmpstr(text, ==, cases[i].text);
	}
}

static void
test_parse_filename_valid(void)
{
	static const struct { const gchar *file; guint version; const gchar *name; } cases[] = {
		{ "0001_schema_baseline.sql", 1, "schema_baseline" },
		{ "0042_a.sql", 42, "a" },
		{ "9999_z9_.sql", 9999, "z9_" },
		{ "0100_add_job_queue2.sql", 100, "add_job_queue2" },
		{ "0001_abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijk.sql", 1,
		  "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijk" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *name = NULL;
		guint version = 0;

		g_assert_true(axis_db_migration_parse_filename(cases[i].file, &version, &name));
		g_assert_cmpuint(version, ==, cases[i].version);
		g_assert_cmpstr(name, ==, cases[i].name);
	}

	/* Both outputs are optional. */
	g_assert_true(axis_db_migration_parse_filename("0001_x.sql", NULL, NULL));
	g_assert_cmpuint(strlen(cases[4].name), ==, AXIS_DB_MIGRATION_NAME_MAX_LEN);
}

static void
test_parse_filename_invalid(void)
{
	static const gchar *const bad[] = {
		"", "0001", "0001_", "0001_.sql", "0001.sql", "_x.sql",
		"0000_zero.sql", "001_x.sql", "00001_x.sql", "1_x.sql", "abcd_x.sql",
		"0001-x.sql", "0001__x.sql", "0001_9x.sql", "0001_X.sql", "0001_xY.sql",
		"0001_x-y.sql", "0001_x y.sql", "0001_x.y.sql", "0001_x.SQL", "0001_x.sql.bak",
		"0001_x.sql~", " 0001_x.sql", "0001_x.sql ", "0001_x\n.sql", "0001_x",
		"0001_x.sq", ".sql", "+001_x.sql", "-001_x.sql", "0x01_x.sql",
		"0001_\xc3\xa9t\xc3\xa9.sql",
		"0001_abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl.sql",
		NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		gchar *name = (gchar *)"untouched";
		guint version = 12345;

		if (axis_db_migration_parse_filename(bad[i], &version, &name))
			g_error("accepted '%s'", bad[i]);
		g_assert_cmpuint(version, ==, 12345);
		g_assert_cmpstr(name, ==, "untouched");
	}

	g_assert_false(axis_db_migration_parse_filename(NULL, NULL, NULL));
}

static void
test_no_transaction_marker(void)
{
	static const gchar *const yes[] = {
		"-- axis:no-transaction\nCREATE INDEX CONCURRENTLY i ON t (x);\n",
		"-- axis:no-transaction",
		"-- axis:no-transaction\n",
		"-- axis:no-transaction\r\nCREATE INDEX CONCURRENTLY i ON t (x);",
		"-- axis:no-transaction \t\nSELECT 1",
		"-- axis:no-transaction  \r\n",
		NULL
	};
	static const gchar *const no[] = {
		"", "CREATE TABLE t (x int);\n",
		"--axis:no-transaction\n",
		" -- axis:no-transaction\n",
		"-- axis:no-transactions\n",
		"-- Axis:no-transaction\n",
		"-- axis:no-transaction x\n",
		"-- axis:no-transaction;\n",
		"-- axis:no-transaction\r \n",
		"-- axis:no-transactio\n",
		"\n-- axis:no-transaction\n",
		"SELECT 1;\n-- axis:no-transaction\n",
		"\xef\xbb\xbf-- axis:no-transaction\n",
		NULL
	};
	guint i;

	for (i = 0; yes[i] != NULL; i++) {
		if (!axis_db_migration_has_no_transaction_marker(yes[i]))
			g_error("marker not found in '%s'", yes[i]);
	}
	for (i = 0; no[i] != NULL; i++) {
		if (axis_db_migration_has_no_transaction_marker(no[i]))
			g_error("marker found in '%s'", no[i]);
	}
	g_assert_false(axis_db_migration_has_no_transaction_marker(NULL));
}

static void
test_checksum(void)
{
	g_autofree gchar *a = axis_db_migration_checksum("SELECT 1;\n");
	g_autofree gchar *b = axis_db_migration_checksum("SELECT 1; \n");

	/* sha256sum of the same bytes */
	g_assert_cmpstr(a, ==, "b4e0497804e46e0a0b0b8c31975b062152d551bac49c3c2e80932567b4085dcd");
	/* Whitespace counts. */
	g_assert_cmpstr(a, !=, b);
}

static void
test_quote_identifier(void)
{
	static const struct { const gchar *in; const gchar *out; } cases[] = {
		{ "setting", "\"setting\"" },
		{ "Mixed Case", "\"Mixed Case\"" },
		{ "a\"b", "\"a\"\"b\"" },
		{ "\"", "\"\"\"\"" },
		{ "x\"; DROP TABLE t; --", "\"x\"\"; DROP TABLE t; --\"" },
		{ "caf\xc3\xa9", "\"caf\xc3\xa9\"" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autofree gchar *quoted = axis_db_quote_identifier(cases[i].in);

		g_assert_cmpstr(quoted, ==, cases[i].out);
	}

	g_assert_null(axis_db_quote_identifier(NULL));
	g_assert_null(axis_db_quote_identifier(""));
	g_assert_null(axis_db_quote_identifier("bad\xff"));
}

static void
assert_connect_error_hides(
	const gchar *url,
	const gchar *secret,
	const gchar *must_contain
){
	g_autoptr(GError) error = NULL;
	AxisDbPool *pool;

	pool = axis_db_pool_new(url, 2, &error);
	g_assert_null(pool);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT);
	g_test_message("error for %s: %s", must_contain, error->message);
	if (strstr(error->message, secret) != NULL)
		g_error("the error contains the password: %s", error->message);
	if (must_contain != NULL)
		g_assert_nonnull(strstr(error->message, must_contain));
}

/* Spec: Unreachable database is reported */
static void
test_unreachable_server(void)
{
	guint16 port = closed_port();
	g_autofree gchar *url = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *keywords = NULL;

	url = g_strdup_printf("postgresql://axis:" SECRET "@127.0.0.1:%u/axis", (guint)port);
	expected = g_strdup_printf("postgresql://axis:****@127.0.0.1:%u/axis", (guint)port);
	assert_connect_error_hides(url, SECRET, expected);

	keywords = g_strdup_printf("host=127.0.0.1 port=%u user=axis password=" SECRET " dbname=axis",
		(guint)port);
	assert_connect_error_hides(keywords, SECRET, "password=****");

	g_free(url);
	url = g_strdup_printf("postgresql://127.0.0.1:%u/axis?user=axis&password=" SECRET, (guint)port);
	assert_connect_error_hides(url, SECRET, "password=****");
}

/* libpq's own message for an unparsable URL quotes the bad part, which
 * may be the password. */
static void
test_invalid_url_hides_password(void)
{
	assert_connect_error_hides("postgresql://axis:" SECRET "%zz@127.0.0.1/axis", SECRET,
		"invalid database URL");
	assert_connect_error_hides("postgresql://axis:" SECRET "@127.0.0.1/axis?bogus_option=1",
		SECRET, "invalid database URL");
	assert_connect_error_hides("host=127.0.0.1 password='" SECRET, SECRET, "invalid database URL");
	assert_connect_error_hides("host=127.0.0.1 " SECRET, SECRET, "invalid database URL");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/db/unit/version-check", test_version_check);
	g_test_add_func("/db/unit/version-format", test_version_format);
	g_test_add_func("/db/unit/migration-name/valid", test_parse_filename_valid);
	g_test_add_func("/db/unit/migration-name/invalid", test_parse_filename_invalid);
	g_test_add_func("/db/unit/no-transaction-marker", test_no_transaction_marker);
	g_test_add_func("/db/unit/checksum", test_checksum);
	g_test_add_func("/db/unit/quote-identifier", test_quote_identifier);
	g_test_add_func("/db/unit/unreachable-server", test_unreachable_server);
	g_test_add_func("/db/unit/invalid-url-hides-password", test_invalid_url_hides_password);

	return g_test_run();
}
