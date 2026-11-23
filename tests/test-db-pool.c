/*
 * test-db-pool.c - the connection pool against a real PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Reuse, hand-over to waiters (in order), acquire timeouts, ownership
 * checks, rollback on release, recovery from connections the server
 * terminates (pg_terminate_backend), ping, and the session settings.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "db/axis-db-conn.h"
#include "db/axis-db-error.h"
#include "db/axis-db-pool.h"

/* Generous bounds for "promptly", so a loaded machine does not fail them. */
#define PROMPT_US       (2 * G_USEC_PER_SEC)
#define WAIT_LIMIT_US   (10 * G_USEC_PER_SEC)

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint size = GPOINTER_TO_UINT(data);
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	fx->pool = axis_db_pool_new(axis_test_db_get_url(fx->db), size != 0 ? size : 4, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fx->pool);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

/* Waits (bounded) until @n callers are waiting in acquire. */
static void
wait_for_waiters(
	AxisDbPool *pool,
	guint       n
){
	gint64 deadline = g_get_monotonic_time() + WAIT_LIMIT_US;

	while (axis_db_pool_get_n_waiting(pool) != n) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_usleep(1000);
	}
}

static gint64
backend_pid(AxisDbConn *conn)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 pid = 0;

	g_assert_true(axis_db_conn_exec(conn, "SELECT pg_backend_pid()", 0, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_result_get_int64(result, 0, 0, &pid));

	return pid;
}

static void
assert_conn_works(AxisDbConn *conn)
{
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[1] = { "41" };
	gint64 value = 0;

	g_assert_true(axis_db_conn_exec(conn, "SELECT $1::int + 1", 1, params, &result, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_result_get_int64(result, 0, 0, &value));
	g_assert_cmpint(value, ==, 42);
}

/* Ends the backend @pid and waits for it to be gone. */
static void
terminate_backend(
	Fixture *fx,
	gint64   pid
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *done = NULL;

	sql = g_strdup_printf("SELECT pg_terminate_backend(%" G_GINT64_FORMAT ", 5000)", pid);
	done = axis_test_db_admin_query(fx->db, sql);
	g_assert_cmpstr(done, ==, "t");
}

/* Spec: Connections are reused */
static void
test_reuse(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *first;
	AxisDbConn *second;
	gint64 pid;

	if (fx->db == NULL)
		return;

	g_assert_cmpuint(axis_db_pool_get_size(fx->pool), ==, 4);
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 1);

	first = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	pid = backend_pid(first);
	axis_db_pool_release(fx->pool, first);

	second = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(second == first);
	g_assert_cmpint(backend_pid(second), ==, pid);
	axis_db_pool_release(fx->pool, second);

	/* Many rounds, still the one server connection. */
	{
		guint i;

		for (i = 0; i < 50; i++) {
			AxisDbConn *conn = axis_db_pool_acquire(fx->pool, &error);

			g_assert_no_error(error);
			assert_conn_works(conn);
			axis_db_pool_release(fx->pool, conn);
		}
	}
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 1);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

/* Several held at once are distinct connections. */
static void
test_distinct_connections(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conns[4];
	gint64 pids[4];
	guint i;
	guint j;

	if (fx->db == NULL)
		return;

	for (i = 0; i < 4; i++) {
		conns[i] = axis_db_pool_acquire(fx->pool, &error);
		g_assert_no_error(error);
		pids[i] = backend_pid(conns[i]);
	}
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 4);
	for (i = 0; i < 4; i++) {
		for (j = i + 1; j < 4; j++) {
			g_assert_true(conns[i] != conns[j]);
			g_assert_cmpint(pids[i], !=, pids[j]);
		}
	}
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 4);

	for (i = 0; i < 4; i++)
		axis_db_pool_release(fx->pool, conns[i]);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

typedef struct
{
	AxisDbPool *pool;
	guint       id;
	AxisDbConn *got;
	GError     *error;
	gint64      returned_at;
	gint64      hold_us;	/* hold the connection this long before release */
	GMutex     *order_lock;
	GArray     *order;	/* ids, in the order they got a connection */
} Acquirer;

static gpointer
acquirer_thread(gpointer data)
{
	Acquirer *a = data;
	AxisDbConn *conn;

	conn = axis_db_pool_acquire(a->pool, &a->error);
	a->returned_at = g_get_monotonic_time();
	a->got = conn;
	if (conn == NULL)
		return NULL;

	if (a->order != NULL) {
		g_mutex_lock(a->order_lock);
		g_array_append_val(a->order, a->id);
		g_mutex_unlock(a->order_lock);
	}

	assert_conn_works(conn);
	if (a->hold_us > 0)
		g_usleep((gulong)a->hold_us);
	axis_db_pool_release(a->pool, conn);
	return NULL;
}

/* Spec: Waiter is served on release */
static void
test_waiter_served_on_release(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	Acquirer a;
	AxisDbConn *held;
	GThread *thread;
	gint64 released_at;

	if (fx->db == NULL)
		return;

	held = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);

	memset(&a, 0, sizeof(a));
	a.pool = fx->pool;
	thread = g_thread_new("acquirer", acquirer_thread, &a);
	wait_for_waiters(fx->pool, 1);

	/* Still waiting while the only connection is held. */
	g_usleep(100 * 1000);
	g_assert_null(a.got);
	g_assert_cmpuint(axis_db_pool_get_n_waiting(fx->pool), ==, 1);

	released_at = g_get_monotonic_time();
	axis_db_pool_release(fx->pool, held);
	g_thread_join(thread);

	g_assert_no_error(a.error);
	g_assert_true(a.got == held);
	g_assert_cmpint(a.returned_at - released_at, <, PROMPT_US);
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 1);
	g_assert_cmpuint(axis_db_pool_get_n_waiting(fx->pool), ==, 0);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

/* Waiters are served in arrival order. */
static void
test_waiters_in_order(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) order = g_array_new(FALSE, FALSE, sizeof(guint));
	GMutex order_lock;
	Acquirer a[4];
	GThread *threads[4];
	AxisDbConn *held;
	guint i;

	if (fx->db == NULL)
		return;

	g_mutex_init(&order_lock);
	held = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);

	for (i = 0; i < G_N_ELEMENTS(a); i++) {
		memset(&a[i], 0, sizeof(a[i]));
		a[i].pool = fx->pool;
		a[i].id = i;
		a[i].hold_us = 20 * 1000;
		a[i].order = order;
		a[i].order_lock = &order_lock;
		threads[i] = g_thread_new("acquirer", acquirer_thread, &a[i]);
		wait_for_waiters(fx->pool, i + 1);
	}

	axis_db_pool_release(fx->pool, held);
	for (i = 0; i < G_N_ELEMENTS(a); i++) {
		g_thread_join(threads[i]);
		g_assert_no_error(a[i].error);
	}

	g_assert_cmpuint(order->len, ==, G_N_ELEMENTS(a));
	for (i = 0; i < order->len; i++)
		g_assert_cmpuint(g_array_index(order, guint, i), ==, i);
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 1);
	g_mutex_clear(&order_lock);
}

/* Spec: Acquire times out */
static void
test_acquire_timeout(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	Acquirer a;
	AxisDbConn *held;
	AxisDbConn *conn;
	GThread *thread;
	gint64 start;

	if (fx->db == NULL)
		return;

	held = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	axis_db_pool_set_acquire_timeout(fx->pool, 300);

	memset(&a, 0, sizeof(a));
	a.pool = fx->pool;
	start = g_get_monotonic_time();
	thread = g_thread_new("acquirer", acquirer_thread, &a);
	g_thread_join(thread);

	g_assert_null(a.got);
	g_assert_error(a.error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	g_test_message("timeout error: %s", a.error->message);
	g_assert_cmpint(a.returned_at - start, >=, 300 * 1000);
	g_assert_cmpint(a.returned_at - start, <, 300 * 1000 + PROMPT_US);
	g_clear_error(&a.error);
	/* The waiter that gave up is gone from the queue. */
	g_assert_cmpuint(axis_db_pool_get_n_waiting(fx->pool), ==, 0);

	/* Zero means do not wait at all. */
	axis_db_pool_set_acquire_timeout(fx->pool, 0);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_null(conn);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	g_clear_error(&error);

	/* And the pool still works once the connection comes back. */
	axis_db_pool_release(fx->pool, held);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(conn == held);
	axis_db_pool_release(fx->pool, conn);
}

/* A thread whose database work can be called off (AxisDbInterrupt). */
typedef struct
{
	AxisDbPool      *pool;
	AxisDbInterrupt *interrupt;
	const gchar     *sql;
	gboolean         ok;
	GError          *error;
	gint64           returned_at;
	gboolean         again_ok;
	gint64           again_took;
} Interrupted;

static gpointer
interrupted_thread(gpointer data)
{
	Interrupted *work = data;
	gint64 start;

	axis_db_interrupt_attach(work->interrupt);
	work->ok = axis_db_pool_exec(work->pool, work->sql, 0, NULL, NULL, &work->error);
	work->returned_at = g_get_monotonic_time();
	/* What it tries after that fails without waiting. */
	start = g_get_monotonic_time();
	work->again_ok = axis_db_pool_exec(work->pool, "SELECT 1", 0, NULL, NULL, NULL);
	work->again_took = g_get_monotonic_time() - start;
	axis_db_interrupt_attach(NULL);

	return NULL;
}

/*
 * An interrupt raised from another thread ends the wait its thread is in at
 * once, as a deadline that has passed: the wait for a connection of the
 * pool, and the wait for a statement's answer, neither of which had a
 * deadline of its own here.  A thread without one is not touched.
 */
static void
test_interrupt(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbInterrupt *interrupt;
	Interrupted work;
	AxisDbConn *held;
	AxisDbConn *conn;
	GThread *thread;
	gint64 raised_at;
	gint64 deadline;

	if (fx->db == NULL)
		return;

	/* Waiting for the pool's only connection, for up to 30 seconds. */
	held = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	interrupt = axis_db_interrupt_new();
	g_assert_false(axis_db_interrupt_is_raised(interrupt));
	memset(&work, 0, sizeof(work));
	work.pool = fx->pool;
	work.interrupt = interrupt;
	work.sql = "SELECT 1";
	thread = g_thread_new("interrupted", interrupted_thread, &work);
	wait_for_waiters(fx->pool, 1);
	g_usleep(100 * 1000);
	g_assert_cmpuint(axis_db_pool_get_n_waiting(fx->pool), ==, 1);
	raised_at = g_get_monotonic_time();
	axis_db_interrupt_raise(interrupt);
	axis_db_interrupt_raise(interrupt);
	g_thread_join(thread);
	g_assert_true(axis_db_interrupt_is_raised(interrupt));
	g_assert_false(work.ok);
	g_assert_error(work.error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	g_assert_cmpint(work.returned_at - raised_at, <, 500 * 1000);
	g_assert_false(work.again_ok);
	g_assert_cmpint(work.again_took, <, 500 * 1000);
	g_clear_error(&work.error);
	g_assert_cmpuint(axis_db_pool_get_n_waiting(fx->pool), ==, 0);
	axis_db_interrupt_free(interrupt);
	axis_db_pool_release(fx->pool, held);

	/* In a statement that takes 8 seconds. */
	interrupt = axis_db_interrupt_new();
	memset(&work, 0, sizeof(work));
	work.pool = fx->pool;
	work.interrupt = interrupt;
	work.sql = "SELECT pg_sleep(8)";
	thread = g_thread_new("interrupted", interrupted_thread, &work);
	deadline = g_get_monotonic_time() + WAIT_LIMIT_US;
	while (axis_db_pool_get_n_in_use(fx->pool) != 1) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_usleep(1000);
	}
	g_usleep(300 * 1000);
	raised_at = g_get_monotonic_time();
	axis_db_interrupt_raise(interrupt);
	g_thread_join(thread);
	g_assert_false(work.ok);
	g_assert_error(work.error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	g_assert_cmpint(work.returned_at - raised_at, <, 500 * 1000);
	g_assert_false(work.again_ok);
	g_assert_cmpint(work.again_took, <, 500 * 1000);
	g_clear_error(&work.error);
	axis_db_interrupt_free(interrupt);

	/* The connection came back, closed; a thread that has no interrupt gets
	 * a new one, which works. */
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	assert_conn_works(conn);
	axis_db_pool_release(fx->pool, conn);
}

/*
 * Many threads, few connections: every acquire succeeds, no connection is
 * ever held by two threads at once, and everything comes back.
 */
typedef struct
{
	AxisDbPool *pool;
	GMutex     *lock;
	GHashTable *held;	/* AxisDbConn* currently held */
	guint       iterations;
	guint       failures;
	guint       overlaps;
} Stress;

static gpointer
stress_thread(gpointer data)
{
	Stress *s = data;
	guint i;

	for (i = 0; i < s->iterations; i++) {
		g_autoptr(GError) error = NULL;
		AxisDbConn *conn = axis_db_pool_acquire(s->pool, &error);
		gboolean overlap;

		if (conn == NULL) {
			g_mutex_lock(s->lock);
			s->failures++;
			g_mutex_unlock(s->lock);
			continue;
		}

		g_mutex_lock(s->lock);
		overlap = !g_hash_table_add(s->held, conn);
		if (overlap)
			s->overlaps++;
		g_mutex_unlock(s->lock);

		if (i % 10 == 0)
			assert_conn_works(conn);
		else if (i % 3 == 0)
			g_thread_yield();

		g_mutex_lock(s->lock);
		if (!overlap)
			g_hash_table_remove(s->held, conn);
		g_mutex_unlock(s->lock);

		axis_db_pool_release(s->pool, conn);
	}

	return NULL;
}

static void
test_contention(Fixture *fx, gconstpointer data)
{
	GThread *threads[16];
	GMutex lock;
	Stress s;
	guint i;

	if (fx->db == NULL)
		return;

	g_mutex_init(&lock);
	memset(&s, 0, sizeof(s));
	s.pool = fx->pool;
	s.lock = &lock;
	s.held = g_hash_table_new(g_direct_hash, g_direct_equal);
	s.iterations = 300;
	axis_db_pool_set_acquire_timeout(fx->pool, 20000);

	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		threads[i] = g_thread_new("stress", stress_thread, &s);
	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		g_thread_join(threads[i]);

	g_assert_cmpuint(s.failures, ==, 0);
	g_assert_cmpuint(s.overlaps, ==, 0);
	g_assert_cmpuint(g_hash_table_size(s.held), ==, 0);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
	g_assert_cmpuint(axis_db_pool_get_n_waiting(fx->pool), ==, 0);
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), <=, 3);

	g_hash_table_unref(s.held);
	g_mutex_clear(&lock);
}

/* Releasing twice must not put one connection in the pool twice. */
static void
test_double_release(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	AxisDbConn *again;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*released by a thread that does not hold it*");

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_CRITICAL, "*released by a thread*"), ==, 1);

	/* One connection, so a second acquire must time out. */
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	axis_db_pool_set_acquire_timeout(fx->pool, 0);
	again = axis_db_pool_acquire(fx->pool, &error);
	g_assert_null(again);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	axis_db_pool_release(fx->pool, conn);
}

typedef struct
{
	AxisDbPool *pool;
	AxisDbConn *conn;
	GMutex      lock;
	GCond       cond;
	gboolean    holding;
	gboolean    may_release;
	gboolean    worked;
} Holder;

static gpointer
holder_thread(gpointer data)
{
	Holder *h = data;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(h->pool, &error);

	g_assert_no_error(error);
	g_mutex_lock(&h->lock);
	h->conn = conn;
	h->holding = TRUE;
	g_cond_broadcast(&h->cond);
	while (!h->may_release)
		g_cond_wait(&h->cond, &h->lock);
	g_mutex_unlock(&h->lock);

	assert_conn_works(conn);
	h->worked = TRUE;
	axis_db_pool_release(h->pool, conn);
	return NULL;
}

/* A stale release from a thread that no longer holds the connection
 * cannot take it from the thread that now does. */
static void
test_stale_release_from_other_thread(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *mine;
	GThread *thread;
	Holder h;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*released by a thread that does not hold it*");
	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*used by a thread that has not acquired it*");

	mine = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, mine);

	memset(&h, 0, sizeof(h));
	h.pool = fx->pool;
	g_mutex_init(&h.lock);
	g_cond_init(&h.cond);
	thread = g_thread_new("holder", holder_thread, &h);
	g_mutex_lock(&h.lock);
	while (!h.holding)
		g_cond_wait(&h.cond, &h.lock);
	g_mutex_unlock(&h.lock);
	g_assert_true(h.conn == mine);

	/* The old holder releases again, and tries to use it. */
	axis_db_pool_release(fx->pool, mine);
	g_assert_false(axis_db_conn_exec(mine, "SELECT 1", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY);
	g_clear_error(&error);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 1);

	g_mutex_lock(&h.lock);
	h.may_release = TRUE;
	g_cond_broadcast(&h.cond);
	g_mutex_unlock(&h.lock);
	g_thread_join(thread);

	g_assert_true(h.worked);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_CRITICAL, "*released by a thread*"), ==, 1);
	g_mutex_clear(&h.lock);
	g_cond_clear(&h.cond);
}

/* Spec: Released connection never carries an open transaction */
static void
test_release_rolls_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *count = NULL;
	AxisDbConn *conn;
	AxisDbConn *again;

	if (fx->db == NULL)
		return;

	axis_test_db_exec(fx->db, "CREATE TABLE t (x int)");
	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*released with a transaction open*");
	axis_test_log_allow(G_LOG_LEVEL_CRITICAL, "*released with a failed transaction*");

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "INSERT INTO t VALUES (1)", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_in_transaction(conn));
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_CRITICAL, "*transaction open*"), ==, 1);

	/* The same connection, with no transaction and no row. */
	again = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(again == conn);
	g_assert_false(axis_db_conn_in_transaction(again));
	g_assert_true(axis_db_conn_exec(again, "SELECT count(*) FROM t", 0, NULL, &result, &error));
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 0), ==, "0");

	/* Same for a transaction a failed statement aborted. */
	g_assert_true(axis_db_conn_begin(again, &error));
	g_assert_true(axis_db_conn_exec(again, "INSERT INTO t VALUES (2)", 0, NULL, NULL, &error));
	g_assert_false(axis_db_conn_exec(again, "SELECT no_such_column FROM t", 0, NULL, NULL, &error));
	g_clear_error(&error);
	axis_db_pool_release(fx->pool, again);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_CRITICAL, "*failed transaction*"), ==, 1);

	again = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_false(axis_db_conn_in_transaction(again));
	assert_conn_works(again);
	axis_db_pool_release(fx->pool, again);

	count = axis_test_db_query(fx->db, "SELECT count(*) FROM t");
	g_assert_cmpstr(count, ==, "0");
	/* Nothing had to be reconnected for this. */
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 1);
}

/* Spec: Connection killed while idle */
static void
test_killed_while_idle(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	gint64 old_pid;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*no longer usable*");

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	old_pid = backend_pid(conn);
	axis_db_pool_release(fx->pool, conn);

	terminate_backend(fx, old_pid);
	/* Idle for longer than the validation interval, as a connection
	 * killed while idle normally is. */
	g_usleep((AXIS_DB_POOL_DEFAULT_VALIDATION_INTERVAL_MS + 200) * 1000);

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	assert_conn_works(conn);
	g_assert_cmpint(backend_pid(conn), !=, old_pid);
	axis_db_pool_release(fx->pool, conn);

	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 2);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*no longer usable*"), ==, 1);
}

/* With validation on every acquire, even a connection killed a moment
 * after release is caught. */
static void
test_killed_while_idle_validate_always(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	gint64 old_pid;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*no longer usable*");
	axis_db_pool_set_validation_interval(fx->pool, 0);

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	old_pid = backend_pid(conn);
	axis_db_pool_release(fx->pool, conn);

	terminate_backend(fx, old_pid);

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	assert_conn_works(conn);
	g_assert_cmpint(backend_pid(conn), !=, old_pid);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 2);
}

typedef struct
{
	AxisDbPool *pool;
	GMutex      lock;
	GCond       cond;
	gint64      pid;
	gboolean    ok;
	GError     *error;
	gint64      took_us;
} Sleeper;

static gpointer
sleeper_thread(gpointer data)
{
	Sleeper *s = data;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(s->pool, &error);
	gint64 start;

	g_assert_no_error(error);
	g_mutex_lock(&s->lock);
	s->pid = backend_pid(conn);
	g_cond_broadcast(&s->cond);
	g_mutex_unlock(&s->lock);

	start = g_get_monotonic_time();
	s->ok = axis_db_conn_exec(conn, "SELECT pg_sleep(30)", 0, NULL, NULL, &s->error);
	s->took_us = g_get_monotonic_time() - start;

	/* Every later call on it fails the same way, until it is released. */
	if (!s->ok) {
		g_assert_false(axis_db_conn_exec(conn, "SELECT 1", 0, NULL, NULL, &error));
		g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);
	}

	axis_db_pool_release(s->pool, conn);
	return NULL;
}

/* Spec: Connection killed mid-query */
static void
test_killed_mid_query(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	AxisDbConn *conn;
	GThread *thread;
	Sleeper s;
	gint64 deadline;

	if (fx->db == NULL)
		return;

	memset(&s, 0, sizeof(s));
	s.pool = fx->pool;
	g_mutex_init(&s.lock);
	g_cond_init(&s.cond);
	thread = g_thread_new("sleeper", sleeper_thread, &s);

	g_mutex_lock(&s.lock);
	while (s.pid == 0)
		g_cond_wait(&s.cond, &s.lock);
	g_mutex_unlock(&s.lock);

	/* Wait until the server is actually running the sleep. */
	sql = g_strdup_printf("SELECT count(*) FROM pg_stat_activity WHERE pid = %" G_GINT64_FORMAT
		" AND state = 'active' AND query LIKE 'SELECT pg_sleep%%'", s.pid);
	deadline = g_get_monotonic_time() + WAIT_LIMIT_US;
	for (;;) {
		g_autofree gchar *n = axis_test_db_admin_query(fx->db, sql);

		if (g_strcmp0(n, "1") == 0)
			break;
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_usleep(10 * 1000);
	}

	terminate_backend(fx, s.pid);
	g_thread_join(thread);

	g_assert_false(s.ok);
	g_assert_error(s.error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);
	g_test_message("mid-query error: %s", s.error->message);
	g_assert_cmpint(s.took_us, <, 20 * G_USEC_PER_SEC);
	g_clear_error(&s.error);

	/* The next acquire yields a working connection. */
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	assert_conn_works(conn);
	g_assert_cmpint(backend_pid(conn), !=, s.pid);
	axis_db_pool_release(fx->pool, conn);

	g_mutex_clear(&s.lock);
	g_cond_clear(&s.cond);
}

/* A server that refuses new connections: acquire fails without leaking
 * the slot, and the pool recovers when the server comes back. */
static void
test_reconnect_failure_frees_slot(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *out = NULL;
	AxisDbConn *conn;
	guint i;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*no longer usable*");
	axis_db_pool_set_validation_interval(fx->pool, 0);

	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS false",
		axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
	g_free(out);
	g_free(sql);
	sql = g_strdup_printf("SELECT count(pg_terminate_backend(pid, 5000)) FROM pg_stat_activity "
		"WHERE datname = '%s'", axis_test_db_get_name(fx->db));
	out = axis_test_db_admin_query(fx->db, sql);
	axis_test_db_disconnect(fx->db);

	/* More attempts than there are slots: none may leak one. */
	for (i = 0; i < axis_db_pool_get_size(fx->pool) * 2; i++) {
		conn = axis_db_pool_acquire(fx->pool, &error);
		g_assert_null(conn);
		g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT);
		if (strstr(error->message, axis_test_db_get_password(fx->db)) != NULL)
			g_error("the error contains the password: %s", error->message);
		g_clear_error(&error);
		g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
	}

	g_assert_false(axis_db_pool_ping(fx->pool, 2000, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_free(sql);
	sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS true",
		axis_test_db_get_name(fx->db));
	g_free(out);
	out = axis_test_db_admin_query(fx->db, sql);

	g_assert_true(axis_db_pool_ping(fx->pool, 5000, &error));
	g_assert_no_error(error);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	assert_conn_works(conn);
	axis_db_pool_release(fx->pool, conn);
}

static void
test_ping(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_db_pool_ping(fx->pool, 2000, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
}

/* A ping whose wait for a connection runs out reports a timeout promptly. */
static void
test_ping_timeout(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *held;
	gint64 start;

	if (fx->db == NULL)
		return;

	held = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);

	start = g_get_monotonic_time();
	g_assert_false(axis_db_pool_ping(fx->pool, 200, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 200 * 1000 + PROMPT_US);

	axis_db_pool_release(fx->pool, held);
}

static void
assert_setting(
	AxisDbConn  *conn,
	const gchar *name,
	const gchar *expected
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[1];

	params[0] = name;
	g_assert_true(axis_db_conn_exec(conn, "SELECT current_setting($1)", 1, params, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 0), ==, expected);
}

static void
test_session_settings(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conns[2];
	guint i;

	if (fx->db == NULL)
		return;

	/* The eagerly opened one and one opened on demand. */
	conns[0] = axis_db_pool_acquire(fx->pool, &error);
	conns[1] = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);

	for (i = 0; i < 2; i++) {
		assert_setting(conns[i], "application_name", "axis");
		assert_setting(conns[i], "client_encoding", "UTF8");
		assert_setting(conns[i], "TimeZone", "UTC");
		assert_setting(conns[i], "statement_timeout", "1min");
		assert_setting(conns[i], "idle_in_transaction_session_timeout", "5min");
		/* (scale-database-first-batch: no statement is compiled.) */
		assert_setting(conns[i], "jit", "off");
	}

	axis_db_pool_release(fx->pool, conns[0]);
	axis_db_pool_release(fx->pool, conns[1]);
}

/* A statement the server cancels for its timeout is a TIMEOUT error, and
 * the connection stays usable. */
static void
test_statement_timeout(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;

	if (fx->db == NULL)
		return;

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_exec(conn, "SET statement_timeout = 100", 0, NULL, NULL, &error));
	g_assert_false(axis_db_conn_exec(conn, "SELECT pg_sleep(5)", 0, NULL, NULL, &error));
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT);
	g_clear_error(&error);
	assert_conn_works(conn);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(axis_db_pool_get_connections_opened(fx->pool), ==, 1);
}

/* A wrong password, or a database that does not exist, is reported
 * without the password. */
static void
test_connect_errors_hide_password(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *wrong = NULL;
	g_autofree gchar *wrong_password = NULL;
	const gchar *password;
	gchar *at;
	AxisDbPool *pool;

	if (fx->db == NULL)
		return;

	password = axis_test_db_get_password(fx->db);

	url = axis_test_db_url_for(fx->db, "axis_no_such_database");
	pool = axis_db_pool_new(url, 1, &error);
	g_assert_null(pool);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT);
	g_test_message("no such database: %s", error->message);
	g_assert_nonnull(strstr(error->message, "axis_no_such_database"));
	if (strstr(error->message, password) != NULL)
		g_error("the error contains the password: %s", error->message);
	g_clear_error(&error);

	/* Same URL with the password replaced. */
	wrong = g_strdup(axis_test_db_get_url(fx->db));
	at = strstr(wrong, password);
	g_assert_nonnull(at);
	memset(at, 'z', strlen(password));
	wrong_password = g_strnfill(strlen(password), 'z');
	pool = axis_db_pool_new(wrong, 1, &error);
	g_assert_null(pool);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECT);
	g_test_message("wrong password: %s", error->message);
	g_assert_nonnull(strstr(error->message, "password authentication failed"));
	g_assert_null(strstr(error->message, wrong_password));
	g_assert_null(strstr(error->message, password));
}

#ifdef AXIS_DEBUG
static void
test_main_thread_warning(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*main loop's thread*");

	/* Not owning the default context: no warning. */
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*main loop's thread*"), ==, 0);

	g_assert_true(g_main_context_acquire(NULL));
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_main_context_release(NULL);
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*main loop's thread*"), ==, 1);
}
#endif

#define ADD(path, size, func) \
	g_test_add(path, Fixture, GUINT_TO_POINTER(size), fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("/db/pool/reuse", 4, test_reuse);
	ADD("/db/pool/distinct-connections", 4, test_distinct_connections);
	ADD("/db/pool/waiter-served-on-release", 1, test_waiter_served_on_release);
	ADD("/db/pool/waiters-in-order", 1, test_waiters_in_order);
	ADD("/db/pool/acquire-timeout", 1, test_acquire_timeout);
	ADD("/db/pool/interrupt", 1, test_interrupt);
	ADD("/db/pool/contention", 3, test_contention);
	ADD("/db/pool/double-release", 1, test_double_release);
	ADD("/db/pool/stale-release-from-other-thread", 1, test_stale_release_from_other_thread);
	ADD("/db/pool/release-rolls-back", 1, test_release_rolls_back);
	ADD("/db/pool/killed-while-idle", 1, test_killed_while_idle);
	ADD("/db/pool/killed-while-idle-validate-always", 1, test_killed_while_idle_validate_always);
	ADD("/db/pool/killed-mid-query", 1, test_killed_mid_query);
	ADD("/db/pool/reconnect-failure-frees-slot", 2, test_reconnect_failure_frees_slot);
	ADD("/db/pool/ping", 2, test_ping);
	ADD("/db/pool/ping-timeout", 1, test_ping_timeout);
	ADD("/db/pool/session-settings", 2, test_session_settings);
	ADD("/db/pool/statement-timeout", 1, test_statement_timeout);
	ADD("/db/pool/connect-errors-hide-password", 1, test_connect_errors_hide_password);
#ifdef AXIS_DEBUG
	ADD("/db/pool/main-thread-warning", 1, test_main_thread_warning);
#endif

	return g_test_run();
}
