/*
 * axis-test-db.h - test support: a fresh database per test, and log checks
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The server comes from AXIS_TEST_DATABASE_URL, which tests/with-postgres.sh
 * sets (make test runs every test program through it).  Each test makes
 * its own uniquely named database there and drops it at the end, so tests
 * cannot see each other's data.  The helpers talk to the server with plain
 * libpq, never through the code under test.
 *
 * Without AXIS_TEST_DATABASE_URL a database test fails with instructions,
 * unless AXIS_SKIP_DB_TESTS=1, which makes it skip.
 */

#ifndef AXIS_TEST_DB_H
#define AXIS_TEST_DB_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AxisTestDb AxisTestDb;

/**
 * axis_test_db_new:
 *
 * Creates a fresh, empty database for the running test.
 *
 * Returns: (nullable): the database; %NULL when the test has been marked
 *   skipped (AXIS_SKIP_DB_TESTS=1), in which case the test should return
 */
AxisTestDb *
axis_test_db_new(void);

/* Drops the database, disconnecting anything still using it. */
void
axis_test_db_free(AxisTestDb *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTestDb, axis_test_db_free)

/* postgresql://user:password@host:port/<this test's database> */
const gchar *
axis_test_db_get_url(AxisTestDb *self);

const gchar *
axis_test_db_get_name(AxisTestDb *self);

/* The server's password (to check that no message contains it). */
const gchar *
axis_test_db_get_password(AxisTestDb *self);

/* The URL with @dbname in place of this test's database. */
gchar *
axis_test_db_url_for(
	AxisTestDb  *self,
	const gchar *dbname
);

/* This test's database at 127.0.0.1:@port instead of the server's own
 * address (for a proxy in between), with @query (nullable, such as
 * "connect_timeout=2") appended. */
gchar *
axis_test_db_url_via(
	AxisTestDb  *self,
	guint16      port,
	const gchar *query
);

/* The server's host and port, as the URL gives them. */
const gchar *
axis_test_db_get_host(AxisTestDb *self);

guint16
axis_test_db_get_port(AxisTestDb *self);

/* Runs @sql in the test database on the helper's own connection; fails the
 * test if it fails. */
void
axis_test_db_exec(
	AxisTestDb  *self,
	const gchar *sql
);

/* Runs @sql in the test database; returns the first column of the first
 * row (NULL for no row or SQL NULL).  Fails the test if @sql fails. */
gchar *
axis_test_db_query(
	AxisTestDb  *self,
	const gchar *sql
);

/* As axis_test_db_query(), on the server's maintenance database instead
 * of the test database (for ALTER DATABASE, pg_terminate_backend, ...). */
gchar *
axis_test_db_admin_query(
	AxisTestDb  *self,
	const gchar *sql
);

/* Closes the helper's connection to the test database (it is reopened on
 * the next query). */
void
axis_test_db_disconnect(AxisTestDb *self);

/*
 * Log checks.  g_test_init() makes warnings and criticals fatal; after
 * axis_test_log_init(), a message matching a pattern given to
 * axis_test_log_allow() is recorded instead of aborting the test.
 * Everything logged in the "axis" domain, at any level, is recorded, so a
 * test can also count info messages.
 */

/* Call at the start of every test (a fixture's setup): GLib forgets the
 * fatal handler after each test case. */
void
axis_test_log_init(void);

/* Allows (and keeps allowing) messages at @level matching @pattern, a
 * GPatternSpec glob such as "*rolling it back*". */
void
axis_test_log_allow(
	GLogLevelFlags  level,
	const gchar    *pattern
);

/* Forgets every recorded message and every allowance. */
void
axis_test_log_reset(void);

/* How many recorded messages at @level match @pattern. */
guint
axis_test_log_count(
	GLogLevelFlags  level,
	const gchar    *pattern
);

/* The last recorded message at @level matching @pattern, or NULL. */
gchar *
axis_test_log_find(
	GLogLevelFlags  level,
	const gchar    *pattern
);

/*
 * A port on 127.0.0.1 for a daemon the test starts, or one that nothing
 * listens on: free now, and picked at random below the kernel's range of
 * ephemeral ports (32768 and up on Linux).  The test programs run many at
 * a time (TEST_JOBS): a port got by binding port 0 and closing it again is
 * one the kernel may hand, a moment later, to another program that binds
 * port 0; one from here it does not hand out, and two programs that ask
 * here at the same moment get the same one once in thousands of times.
 */
guint16
axis_test_free_port(void);

G_END_DECLS

#endif /* AXIS_TEST_DB_H */
