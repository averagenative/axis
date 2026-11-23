/*
 * test-db-listener.c - the dedicated LISTEN connection against a real PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Receiving notifications in order, waking a wait from another thread,
 * deadlines, a terminated connection reported and reconnected, a
 * notification sent from the listener itself arriving after everything
 * committed before it, and errors without the password.
 */

#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "db/axis-db-error.h"
#include "db/axis-db-listener.h"
#include "db/axis-db-pool.h"

#define CHANNEL "test_channel"

typedef struct
{
	AxisTestDb     *db;
	AxisDbPool     *pool;
	AxisDbListener *listener;
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
	fx->listener = axis_db_listener_new(fx->pool, CHANNEL, -1, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fx->listener);
	g_assert_true(axis_db_listener_is_connected(fx->listener));
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_db_listener_free(fx->listener);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

static gchar *
receive(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	gchar *payload;

	while ((payload = axis_db_listener_pop(fx->listener)) == NULL) {
		AxisDbListenerWait wait = axis_db_listener_wait(fx->listener, -1, deadline, &error);

		g_assert_no_error(error);
		g_assert_cmpint(wait, ==, AXIS_DB_LISTENER_READY);
	}

	return payload;
}

static void
test_receive_in_order(Fixture *fx, gconstpointer data)
{
	guint i;

	if (fx->db == NULL)
		return;

	/* One transaction's in the order sent, then the next's. */
	axis_test_db_exec(fx->db, "BEGIN; SELECT pg_notify('" CHANNEL "', 'a'); "
		"SELECT pg_notify('" CHANNEL "', 'b'); NOTIFY other_channel, 'x'; COMMIT");
	axis_test_db_exec(fx->db, "SELECT pg_notify('" CHANNEL "', 'c')");
	axis_test_db_exec(fx->db, "BEGIN; SELECT pg_notify('" CHANNEL "', 'never'); ROLLBACK");
	for (i = 0; i < 3; i++) {
		g_autofree gchar *payload = receive(fx);
		gchar expected[2] = { (gchar)('a' + i), '\0' };

		g_assert_cmpstr(payload, ==, expected);
	}
	g_assert_null(axis_db_listener_pop(fx->listener));
}

static void
test_deadline(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 start;
	gint64 took;

	if (fx->db == NULL)
		return;

	start = g_get_monotonic_time();
	g_assert_cmpint(axis_db_listener_wait(fx->listener, -1, start + 200 * 1000, &error), ==,
		AXIS_DB_LISTENER_TIMEOUT);
	took = g_get_monotonic_time() - start;
	g_assert_no_error(error);
	g_assert_cmpint(took, >=, 200 * 1000);
	g_assert_cmpint(took, <, 2 * G_USEC_PER_SEC);
}

typedef struct
{
	gint fd;
} Waker;

static gpointer
waker_thread(gpointer data)
{
	Waker *waker = data;

	g_usleep(100 * 1000);
	g_assert_cmpint(write(waker->fd, "x", 1), ==, 1);
	return NULL;
}

static void
test_wake_fd(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	GThread *thread;
	Waker waker;
	gint fds[2];

	if (fx->db == NULL)
		return;

	g_assert_true(g_unix_open_pipe(fds, O_CLOEXEC, &error));
	waker.fd = fds[1];
	thread = g_thread_new("waker", waker_thread, &waker);
	g_assert_cmpint(axis_db_listener_wait(fx->listener, fds[0], g_get_monotonic_time() +
		AXIS_TEST_WAIT_MS * 1000, &error), ==, AXIS_DB_LISTENER_WOKEN);
	g_thread_join(thread);
	close(fds[0]);
	close(fds[1]);
}

/* A connection the server ends is reported, not hidden, and comes back
 * with its LISTEN in place. */
static void
test_lost_and_reconnected(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *done = NULL;
	g_autofree gchar *payload = NULL;
	AxisDbListenerWait wait;

	if (fx->db == NULL)
		return;

	done = axis_test_db_admin_query(fx->db, "SELECT count(pg_terminate_backend(pid, 5000)) "
		"FROM pg_stat_activity WHERE query = 'LISTEN \"" CHANNEL "\"'");
	g_assert_cmpstr(done, ==, "1");

	wait = axis_db_listener_wait(fx->listener, -1, g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000,
		&error);
	g_assert_cmpint(wait, ==, AXIS_DB_LISTENER_LOST);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);
	g_test_message("%s", error->message);
	g_clear_error(&error);
	g_assert_false(axis_db_listener_is_connected(fx->listener));

	/* Disconnected, a wait is only for the clock (and the wake fd). */
	g_assert_cmpint(axis_db_listener_wait(fx->listener, -1, g_get_monotonic_time() + 50 * 1000,
		&error), ==, AXIS_DB_LISTENER_TIMEOUT);
	g_assert_false(axis_db_listener_notify(fx->listener, "x", -1, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);
	g_clear_error(&error);

	g_assert_true(axis_db_listener_reconnect(fx->listener, -1, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_listener_is_connected(fx->listener));
	axis_test_db_exec(fx->db, "SELECT pg_notify('" CHANNEL "', 'again')");
	payload = receive(fx);
	g_assert_cmpstr(payload, ==, "again");
	g_assert_true(axis_db_listener_ping(fx->listener, -1, &error));
	g_assert_no_error(error);
}

/* Its own notification comes back after everything committed before it. */
static void
test_notify_is_a_barrier(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint i;

	if (fx->db == NULL)
		return;

	for (i = 0; i < 20; i++) {
		g_autofree gchar *sql = g_strdup_printf("SELECT pg_notify('" CHANNEL "', 'n%u')", i);

		axis_test_db_exec(fx->db, sql);
	}
	g_assert_true(axis_db_listener_notify(fx->listener, "barrier", -1, &error));
	g_assert_no_error(error);
	for (i = 0; i < 20; i++) {
		g_autofree gchar *payload = receive(fx);
		g_autofree gchar *expected = g_strdup_printf("n%u", i);

		g_assert_cmpstr(payload, ==, expected);
	}
	{
		g_autofree gchar *payload = receive(fx);

		g_assert_cmpstr(payload, ==, "barrier");
	}
}

static void
test_connect_error_hides_password(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *out = NULL;
	AxisDbListener *listener;

	if (fx->db == NULL)
		return;

	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS false", axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
	listener = axis_db_listener_new(fx->pool, CHANNEL, -1, &error);
	g_assert_null(listener);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT);
	g_test_message("%s", error->message);
	g_assert_null(strstr(error->message, axis_test_db_get_password(fx->db)));
	g_free(sql);
	g_free(out);
	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS true", axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	alarm(120);

	ADD("/db/listener/receive-in-order", test_receive_in_order);
	ADD("/db/listener/deadline", test_deadline);
	ADD("/db/listener/wake-fd", test_wake_fd);
	ADD("/db/listener/lost-and-reconnected", test_lost_and_reconnected);
	ADD("/db/listener/notify-is-a-barrier", test_notify_is_a_barrier);
	ADD("/db/listener/connect-error-hides-password", test_connect_error_hides_password);

	return g_test_run();
}
