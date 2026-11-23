/*
 * test-daemon-lock.c - real axisd processes meeting the single-daemon lock
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * openspec/specs/daemon-core/spec.md: one daemon per database.  Each test makes its own
 * database and starts axisd (AXISD_PATH) on it, once or twice, each with a
 * port and a data directory of its own, as two containers pointed at one
 * PostgreSQL would be.  A daemon's log is its standard error.  The lock's
 * own behaviour, and what the job queue does with it, are tested in
 * tests/test-db-resilience.c.
 */

#include <gio/gio.h>
#include <glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-relay.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "db/axis-db-lock.h"
#include "db/axis-db-pool.h"

/* Sanitized builds on a busy machine start slowly. */
#define START_MS 120000
#define STOP_MS  60000

#define READY     "{\"status\":\"ready\"}"
#define STANDBY   "{\"status\":\"held by another daemon\"}"
#define LOCK_LOST "{\"status\":\"database lock lost\"}"

typedef struct
{
	AxisTestDb     *db;
	gchar          *dir;
	AxisTestWorker *first;
	AxisTestWorker *second;
	gchar          *first_port;
	gchar          *second_port;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->first != NULL)
		axis_test_worker_free(fx->first);
	if (fx->second != NULL)
		axis_test_worker_free(fx->second);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->first_port);
	g_free(fx->second_port);
	axis_test_log_reset();
}

static gchar *
free_port(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
		&error);
	g_autoptr(GSocketAddress) address = g_inet_socket_address_new_from_string("127.0.0.1", 0);
	g_autoptr(GSocketAddress) bound = NULL;
	guint16 port;

	g_assert_no_error(error);
	g_assert_true(g_socket_bind(socket, address, FALSE, &error));
	bound = g_socket_get_local_address(socket, &error);
	g_assert_no_error(error);
	port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
	g_socket_close(socket, NULL);
	return g_strdup_printf("%u", (guint)port);
}

/* axisd on the test's database (through @url, unless NULL), on @port with
 * the data directory @name; @wait (nullable) is AXIS_DATABASE_WAIT. */
static AxisTestWorker *
start_axisd_via(
	Fixture     *fx,
	const gchar *port,
	const gchar *name,
	const gchar *wait,
	const gchar *url
){
	g_autofree gchar *data_dir = g_build_filename(fx->dir, name, NULL);
	const gchar *argv[2];
	const gchar *env[14];
	guint n = 0;

	argv[0] = AXISD_PATH;
	argv[1] = NULL;
	env[n++] = "AXIS_DATABASE_URL";
	env[n++] = url != NULL ? url : axis_test_db_get_url(fx->db);
	env[n++] = "AXIS_DATA_DIR";
	env[n++] = data_dir;
	env[n++] = "AXIS_LISTEN_HOST";
	env[n++] = "127.0.0.1";
	env[n++] = "AXIS_LISTEN_PORT";
	env[n++] = port;
	env[n++] = "AXIS_LOG_LEVEL";
	env[n++] = "info";
	if (wait != NULL) {
		env[n++] = "AXIS_DATABASE_WAIT";
		env[n++] = wait;
	}
	env[n] = NULL;
	return axis_test_process_spawn(argv, env);
}

static AxisTestWorker *
start_axisd(
	Fixture     *fx,
	const gchar *port,
	const gchar *name,
	const gchar *wait
){
	return start_axisd_via(fx, port, name, wait, NULL);
}

static void
expect_log(
	AxisTestWorker *daemon,
	const gchar    *needle,
	guint           timeout_ms
){
	if (!axis_test_worker_wait_log(daemon, needle, timeout_ms))
		g_error("axisd did not log \"%s\"; its log:\n%s", needle, axis_test_worker_get_log(daemon));
}

/* A clean log: no sanitizer report, no password. */
static void
assert_clean(
	Fixture        *fx,
	AxisTestWorker *daemon
){
	const gchar *log = axis_test_worker_get_log(daemon);

	if (strstr(log, "Sanitizer") != NULL || strstr(log, "runtime error") != NULL)
		g_error("a sanitizer reported in axisd's log:\n%s", log);
	if (strstr(log, axis_test_db_get_password(fx->db)) != NULL)
		g_error("axisd logged the database password:\n%s", log);
}

/* Waits for @daemon to end, with @status. */
static void
expect_exit(
	Fixture        *fx,
	AxisTestWorker *daemon,
	gint            status
){
	if (!axis_test_worker_wait_exit(daemon, STOP_MS))
		g_error("axisd did not stop; its log:\n%s", axis_test_worker_get_log(daemon));
	if (axis_test_worker_get_status(daemon) != status)
		g_error("axisd ended with %d, not %d; its log:\n%s", axis_test_worker_get_status(daemon), status,
			axis_test_worker_get_log(daemon));
	assert_clean(fx, daemon);
}

static void
stop_axisd(
	Fixture        *fx,
	AxisTestWorker *daemon
){
	axis_test_worker_signal(daemon, SIGTERM);
	expect_exit(fx, daemon, 0);
}

/* GET http://127.0.0.1:@port@path: the status (0: no answer), and the
 * body. */
static guint
http_get(
	const gchar  *port,
	const gchar  *path,
	gchar       **body
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10,
		"proxy-resolver", (GProxyResolver *)NULL, NULL);
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%s%s", port, path);
	g_autoptr(SoupMessage) message = soup_message_new(SOUP_METHOD_GET, url);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;

	if (body != NULL)
		*body = NULL;
	bytes = soup_session_send_and_read(session, message, NULL, &error);
	if (bytes == NULL)
		return 0;
	if (body != NULL) {
		gsize size = 0;
		gconstpointer data = g_bytes_get_data(bytes, &size);

		*body = g_strndup(data, size);
	}
	return soup_message_get_status(message);
}

typedef struct
{
	const gchar *port;
	const gchar *path;
	guint        status;
	const gchar *body;
} Probe;

static gboolean
probe_is(gpointer data)
{
	Probe *probe = data;
	g_autofree gchar *body = NULL;
	guint status = http_get(probe->port, probe->path, &body);

	return status == probe->status && (probe->body == NULL || g_strcmp0(body, probe->body) == 0);
}

/* Waits until @path answers @status (and @body, unless NULL). */
static void
expect_answer(
	AxisTestWorker *daemon,
	const gchar    *port,
	const gchar    *path,
	guint           status,
	const gchar    *body,
	guint           timeout_ms
){
	Probe probe;

	probe.port = port;
	probe.path = path;
	probe.status = status;
	probe.body = body;
	if (!axis_test_pump_until(probe_is, &probe, timeout_ms)) {
		g_autofree gchar *got = NULL;
		guint now = http_get(port, path, &got);

		g_error("%s answers %u %s, not %u %s; axisd's log:\n%s", path, now, got != NULL ? got : "", status,
			body != NULL ? body : "", axis_test_worker_get_log(daemon));
	}
}

/* A job of the daemon's own retention kind, which every daemon runs. */
static gchar *
insert_job(
	Fixture     *fx,
	const gchar *mark
){
	return axis_test_db_queryf(fx->db, "INSERT INTO job (kind, queue, max_attempts, last_error)"
		" VALUES ('jobs.prune', 'maintenance', 3, '%s') RETURNING id::text", mark);
}

/* Waits until job @id has succeeded, run by @daemon's workers. */
static void
expect_run_by(
	Fixture        *fx,
	const gchar    *id,
	AxisTestWorker *daemon
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state::text FROM job WHERE id = %s", id);
	g_autofree gchar *worker = NULL;
	g_autofree gchar *pid = NULL;

	if (!axis_test_wait_query(fx->db, sql, "succeeded", 60000))
		g_error("job %s did not run; axisd's log:\n%s", id, axis_test_worker_get_log(daemon));
	worker = axis_test_db_queryf(fx->db, "SELECT worker_id FROM job WHERE id = %s", id);
	pid = g_strdup_printf(":%d:", (int)axis_test_worker_get_pid(daemon));
	g_assert_nonnull(worker);
	g_assert_true(g_str_has_prefix(worker, "axisd:"));
	g_assert_nonnull(strstr(worker, pid));
}

/* How many jobs @daemon's workers claimed. */
static gchar *
claimed_by(
	Fixture        *fx,
	AxisTestWorker *daemon
){
	return axis_test_db_queryf(fx->db, "SELECT count(*) FROM job WHERE worker_id LIKE 'axisd:%%:%d:%%'",
		(int)axis_test_worker_get_pid(daemon));
}

static void
start_first(Fixture *fx)
{
	fx->first_port = free_port();
	fx->first = start_axisd(fx, fx->first_port, "first", NULL);
	expect_log(fx->first, "listening on", START_MS);
	expect_answer(fx->first, fx->first_port, "/readyz", 200, READY, 30000);
}

/* What pg_locks shows for the daemon lock in this database. */
static gchar *
lock_condition(Fixture *fx)
{
	guint64 bits = (guint64)AXIS_DB_DAEMON_LOCK_KEY;

	return g_strdup_printf("locktype = 'advisory' AND granted AND objsubid = 1 AND classid = %u AND objid = %u"
		" AND database = (SELECT oid FROM pg_database WHERE datname = '%s')", (guint)(bits >> 32),
		(guint)(bits & G_GUINT64_CONSTANT(0xffffffff)), axis_test_db_get_name(fx->db));
}

/* Ends the session that holds the daemon lock, as an administrator would. */
static void
end_lock_session(Fixture *fx)
{
	g_autofree gchar *condition = lock_condition(fx);
	g_autofree gchar *sql = g_strdup_printf("SELECT pg_terminate_backend(pid, 5000)::text FROM pg_locks WHERE %s",
		condition);
	g_autofree gchar *out = axis_test_db_admin_query(fx->db, sql);

	g_assert_cmpstr(out, ==, "true");
}

static void
allow_connections(
	Fixture  *fx,
	gboolean  allow
){
	g_autofree gchar *sql = g_strdup_printf("ALTER DATABASE %s WITH ALLOW_CONNECTIONS %s",
		axis_test_db_get_name(fx->db), allow ? "true" : "false");
	g_autofree gchar *out = axis_test_db_admin_query(fx->db, sql);
}

/*
 * Spec (database): A second daemon starts, and The first daemon stops.  The
 * second is alive and not ready, says why, names the first in its log,
 * migrates nothing and claims no job; when the first stops, it takes over.
 */
static void
test_second_waits_then_takes_over(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *names_first = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *count = NULL;

	if (fx->db == NULL)
		return;

	start_first(fx);
	fx->second_port = free_port();
	fx->second = start_axisd(fx, fx->second_port, "second", NULL);
	expect_log(fx->second, "the database is in use by another Axis daemon", START_MS);
	/* It names the first: the process id its lock's session gives. */
	names_first = g_strdup_printf("pid %d on", (int)axis_test_worker_get_pid(fx->first));
	g_assert_nonnull(strstr(axis_test_worker_get_log(fx->second), names_first));

	/* Alive, not ready, saying why, and serving nothing else. */
	expect_answer(fx->second, fx->second_port, "/livez", 200, "{\"status\":\"alive\"}", 10000);
	g_assert_cmpuint(http_get(fx->second_port, "/readyz", &body), ==, 503);
	g_assert_cmpstr(body, ==, STANDBY);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_get(fx->second_port, "/healthz", &body), ==, 503);
	g_assert_cmpstr(body, ==, STANDBY);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_get(fx->second_port, "/", &body), ==, 503);
	g_assert_true(g_str_has_prefix(body, "Axis is waiting: another Axis daemon is using this database."));
	g_assert_cmpuint(http_get(fx->second_port, "/login", NULL), ==, 503);

	/* It changed nothing: no migration ran, and the first runs the jobs. */
	id = insert_job(fx, "while the second waits");
	expect_run_by(fx, id, fx->first);
	count = claimed_by(fx, fx->second);
	g_assert_cmpstr(count, ==, "0");
	g_assert_null(strstr(axis_test_worker_get_log(fx->second), "database schema"));
	g_assert_null(strstr(axis_test_worker_get_log(fx->second), "listening on"));

	/* The first stops: the second takes over, and runs the jobs. */
	stop_axisd(fx, fx->first);
	expect_log(fx->second, "took the database lock after waiting", 60000);
	expect_log(fx->second, "listening on", START_MS);
	expect_answer(fx->second, fx->second_port, "/readyz", 200, READY, 30000);
	g_free(id);
	id = insert_job(fx, "after the takeover");
	expect_run_by(fx, id, fx->second);
	stop_axisd(fx, fx->second);
}

/*
 * With database.wait 0, a second daemon exits at once, naming the first.
 * With the default (forever), a waiting one stops at once on SIGTERM.
 */
static void
test_second_does_not_wait(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *names_first = NULL;
	g_autofree gchar *port = NULL;
	const gchar *log;
	gint64 sent;

	if (fx->db == NULL)
		return;

	start_first(fx);
	fx->second_port = free_port();
	fx->second = start_axisd(fx, fx->second_port, "second", "0");
	expect_exit(fx, fx->second, 1);
	log = axis_test_worker_get_log(fx->second);
	g_assert_nonnull(strstr(log, "the database is in use by another Axis daemon ("));
	g_assert_nonnull(strstr(log, "database.wait is 0, so this one does not wait for it"));
	names_first = g_strdup_printf("pid %d on", (int)axis_test_worker_get_pid(fx->first));
	g_assert_nonnull(strstr(log, names_first));
	g_assert_null(strstr(log, "listening on"));
	axis_test_worker_free(fx->second);

	port = free_port();
	fx->second = start_axisd(fx, port, "third", NULL);
	expect_log(fx->second, "the database is in use by another Axis daemon", START_MS);
	sent = g_get_monotonic_time();
	stop_axisd(fx, fx->second);
	g_assert_cmpint(g_get_monotonic_time() - sent, <, 10 * G_USEC_PER_SEC);
	g_assert_nonnull(strstr(axis_test_worker_get_log(fx->second), "shutting down before the database was ready"));

	stop_axisd(fx, fx->first);
}

/*
 * Spec (database): The lock's connection is lost.  The lock's session is
 * ended, and new connections are refused for a while, so it cannot be taken
 * again at once: the daemon suspends (not ready, saying why, nothing but the
 * probes, no job), then takes the lock again and resumes.
 */
static void
test_lost_and_taken_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *id = NULL;

	if (fx->db == NULL)
		return;

	start_first(fx);
	allow_connections(fx, FALSE);
	end_lock_session(fx);
	expect_log(fx->first, "lost the database lock", 30000);
	expect_log(fx->first, "job queue suspended", 30000);
	expect_answer(fx->first, fx->first_port, "/readyz", 503, LOCK_LOST, 10000);
	g_assert_cmpuint(http_get(fx->first_port, "/", &body), ==, 503);
	g_assert_true(g_str_has_prefix(body, "Axis has lost its hold on its database"));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_get(fx->first_port, "/livez", NULL), ==, 200);
	/* /healthz is its own handler's (the database itself may answer): its
	 * own words, not the gate's. */
	g_assert_cmpuint(http_get(fx->first_port, "/healthz", &body), !=, 0);
	g_assert_true(g_strcmp0(body, "{\"status\":\"ok\"}") == 0 || g_strcmp0(body, "{\"status\":\"degraded\"}") == 0);
	expect_log(fx->first, "cannot take the database lock again", 30000);

	allow_connections(fx, TRUE);
	expect_log(fx->first, "holds the database lock again", 60000);
	expect_log(fx->first, "job queue resumed", 10000);
	expect_answer(fx->first, fx->first_port, "/readyz", 200, READY, 30000);
	g_assert_cmpuint(http_get(fx->first_port, "/", NULL), !=, 503);
	id = insert_job(fx, "after the lock came back");
	expect_run_by(fx, id, fx->first);
	stop_axisd(fx, fx->first);
}

/*
 * A session of the test's own, on a thread of its own, that runs
 * @hold_sql (with @value as $1; inside a transaction when @transaction)
 * and keeps what it took until it is let go: then @release_sql, or the
 * rollback.  A statement that waits for a lock waits in the server's queue,
 * so the server hands the lock to it the moment its holder lets go.
 */
typedef struct
{
	AxisDbPool  *pool;
	const gchar *hold_sql;
	const gchar *release_sql;	/* nullable */
	gboolean     transaction;
	gchar       *value;
	GThread     *thread;
	GMutex       lock;
	GCond        cond;
	gboolean     held;
	gboolean     release;
} Holder;

static gpointer
holder_main(gpointer data)
{
	Holder *holder = data;
	g_autoptr(GError) error = NULL;
	const gchar *params[1];
	AxisDbConn *conn = axis_db_pool_acquire(holder->pool, &error);
	gboolean ok = conn != NULL;

	params[0] = holder->value;
	if (ok && holder->transaction)
		ok = axis_db_conn_begin(conn, &error);
	if (ok)
		ok = axis_db_conn_exec(conn, holder->hold_sql, 1, params, NULL, &error);
	if (!ok)
		g_printerr("# the test's session could not take what it holds: %s\n",
			error != NULL ? error->message : "no connection");

	g_mutex_lock(&holder->lock);
	holder->held = ok;
	g_cond_broadcast(&holder->cond);
	while (!holder->release)
		g_cond_wait(&holder->cond, &holder->lock);
	g_mutex_unlock(&holder->lock);

	if (conn != NULL) {
		if (ok && holder->release_sql != NULL)
			axis_db_conn_exec(conn, holder->release_sql, 1, params, NULL, NULL);
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(holder->pool, conn);
	}
	return NULL;
}

static Holder *
holder_start(
	Fixture     *fx,
	const gchar *hold_sql,
	const gchar *release_sql,
	gboolean     transaction,
	const gchar *value
){
	g_autoptr(GError) error = NULL;
	Holder *holder = g_new0(Holder, 1);

	holder->pool = axis_db_pool_new(axis_test_db_get_url(fx->db), 1, &error);
	g_assert_no_error(error);
	holder->hold_sql = hold_sql;
	holder->release_sql = release_sql;
	holder->transaction = transaction;
	holder->value = g_strdup(value);
	g_mutex_init(&holder->lock);
	g_cond_init(&holder->cond);
	holder->thread = g_thread_new("test-holder", holder_main, holder);
	return holder;
}

static gboolean
holder_holds(gpointer data)
{
	Holder *holder = data;
	gboolean held;

	g_mutex_lock(&holder->lock);
	held = holder->held;
	g_mutex_unlock(&holder->lock);
	return held;
}

static void
holder_let_go(Holder *holder)
{
	g_mutex_lock(&holder->lock);
	holder->release = TRUE;
	g_cond_broadcast(&holder->cond);
	g_mutex_unlock(&holder->lock);
	g_thread_join(holder->thread);
	axis_db_pool_free(holder->pool);
	g_mutex_clear(&holder->lock);
	g_cond_clear(&holder->cond);
	g_free(holder->value);
	g_free(holder);
}

/* Waits until @n sessions of the test's database wait for a lock of
 * @wait_event ("advisory", "tuple", "transactionid", ...; NULL: any). */
static void
expect_lock_waits(
	Fixture     *fx,
	const gchar *wait_event,
	guint        n
){
	g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM pg_stat_activity WHERE datname = current_database()"
		" AND wait_event_type = 'Lock'%s%s%s", wait_event != NULL ? " AND wait_event = '" : "",
		wait_event != NULL ? wait_event : "", wait_event != NULL ? "'" : "");
	g_autofree gchar *expected = g_strdup_printf("%u", n);

	if (!axis_test_wait_query(fx->db, sql, expected, 60000)) {
		/* This wait has failed once in a full run and never alone: say what
		 * the daemon and its jobs were doing, so that the next time tells. */
		g_autofree gchar *jobs = axis_test_db_query(fx->db, "SELECT coalesce(string_agg(id::text || ' ' || kind ||"
			" ' ' || state || ' attempts ' || attempts::text, '; ' ORDER BY id), 'none') FROM job");

		g_test_message("jobs when the wait for a lock ran out: %s", jobs != NULL ? jobs : "(not read)");
		if (fx->first != NULL)
			g_test_message("the first daemon's log: %s", axis_test_worker_get_log(fx->first));
		g_assert_true(FALSE);
	}
}

/*
 * Spec (database): The lock is taken by another while it is lost.  The
 * first daemon's lock session ends and another session takes the lock: the
 * first stops its work, finds the lock held when it tries to take it again,
 * and stops (status 1); the second, waiting all along, takes over once
 * that session lets go.
 */
static void
test_lost_to_another(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, AXIS_DB_DAEMON_LOCK_KEY);
	Holder *taker;
	const gchar *log;

	if (fx->db == NULL)
		return;

	start_first(fx);
	fx->second_port = free_port();
	fx->second = start_axisd(fx, fx->second_port, "second", NULL);
	expect_log(fx->second, "the database is in use by another Axis daemon", START_MS);

	/* The test's session waits for the lock, then the first's ends. */
	taker = holder_start(fx, "SELECT pg_advisory_lock($1::bigint)", "SELECT pg_advisory_unlock($1::bigint)", FALSE,
		key);
	expect_lock_waits(fx, "advisory", 1);
	end_lock_session(fx);
	g_assert_true(axis_test_pump_until(holder_holds, taker, AXIS_TEST_WAIT_MS));

	expect_log(fx->first, "lost the database lock", 30000);
	expect_log(fx->first, "another session took the database lock while this daemon had lost it", 60000);
	expect_exit(fx, fx->first, 1);
	log = axis_test_worker_get_log(fx->first);
	g_assert_nonnull(strstr(log, "stopped: another session took the database lock"));
	g_assert_null(strstr(axis_test_worker_get_log(fx->second), "listening on"));

	/* The test's session lets go: the second, waiting all along, takes
	 * over. */
	holder_let_go(taker);
	expect_log(fx->second, "took the database lock after waiting", 60000);
	expect_log(fx->second, "listening on", START_MS);
	expect_answer(fx->second, fx->second_port, "/readyz", 200, READY, 30000);
	stop_axisd(fx, fx->second);
}

/*
 * Spec (database): A handler that does not stop.  The daemon's retention
 * job is held inside its DELETE by a row the test keeps locked, so it cannot
 * return; then the lock's session ends.  Twenty seconds later the process
 * ends with status 3, rather than let that handler run on beside a daemon
 * that may take the database.
 */
static void
test_stuck_handler_ends_process(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *old = NULL;
	g_autofree gchar *id = NULL;
	Holder *row;
	gint64 lost;
	gint64 took_ms;

	if (fx->db == NULL)
		return;

	start_first(fx);
	/* The retention run its schedule starts with is over; then a finished
	 * job old enough for the next run to delete, locked by the test. */
	g_assert_true(axis_test_wait_query(fx->db, "SELECT count(*) FROM job WHERE kind = 'jobs.prune'"
		" AND state IN ('queued', 'running')", "0", 60000));
	old = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, state, max_attempts, attempts, finished_at,"
		" last_error) VALUES ('jobs.prune', 'maintenance', 'succeeded', 3, 1, now() - interval '8 days', 'old')"
		" RETURNING id::text");
	row = holder_start(fx, "SELECT id FROM job WHERE id = $1::bigint FOR UPDATE", NULL, TRUE, old);
	g_assert_true(axis_test_pump_until(holder_holds, row, AXIS_TEST_WAIT_MS));

	/* The retention job runs, and waits for that row. */
	id = insert_job(fx, "held by the test");
	expect_lock_waits(fx, NULL, 1);

	lost = g_get_monotonic_time();
	end_lock_session(fx);
	expect_log(fx->first, "lost the database lock", 30000);
	expect_log(fx->first, "running job(s) told to stop", 10000);
	expect_exit(fx, fx->first, 3);
	took_ms = (g_get_monotonic_time() - lost) / 1000;
	g_test_message("the process ended %" G_GINT64_FORMAT " ms after the lock's session ended", took_ms);
	g_assert_cmpint(took_ms, >=, 24000);
	g_assert_cmpint(took_ms, <, 50000);
	g_assert_nonnull(strstr(axis_test_worker_get_log(fx->first), "a job handler did not stop within 25 s after the "
		"database lock was lost; ending the process at once"));
	g_assert_null(strstr(axis_test_worker_get_log(fx->first), "holds the database lock again"));

	holder_let_go(row);
}

/*
 * Finding 6 of the review of add-single-daemon-lock: a slow server does not
 * end the daemon.  The lock's connection (the second the daemon opens; the
 * pool's first comes before it) stops passing anything for a while, both
 * ends open, as when PostgreSQL stalls: the check goes unanswered, another
 * connection confirms that the daemon still holds the lock, and nothing is
 * lost.  Before the fix one late answer counted as a loss, and the check's
 * timeout closed the connection, releasing the lock itself.
 */
static void
test_slow_check_not_lost(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *url = NULL;
	AxisTestRelay *relay;

	if (fx->db == NULL)
		return;

	relay = axis_test_relay_new(axis_test_db_get_host(fx->db), axis_test_db_get_port(fx->db));
	url = axis_test_db_url_via(fx->db, axis_test_relay_get_port(relay), NULL);
	fx->first_port = free_port();
	fx->first = start_axisd_via(fx, fx->first_port, "first", NULL, url);
	expect_log(fx->first, "listening on", START_MS);
	expect_answer(fx->first, fx->first_port, "/readyz", 200, READY, 30000);

	axis_test_relay_stall(relay, 2, TRUE);
	expect_log(fx->first, "the lock's connection did not answer within 5 s", 30000);
	expect_log(fx->first, "the server confirms on another connection that this daemon still holds the lock", 30000);
	g_assert_cmpuint(http_get(fx->first_port, "/readyz", NULL), ==, 200);
	axis_test_relay_stall(relay, 2, FALSE);
	expect_log(fx->first, "the lock's connection answers again", 30000);
	g_assert_null(strstr(axis_test_worker_get_log(fx->first), "lost the database lock"));
	expect_answer(fx->first, fx->first_port, "/readyz", 200, READY, 10000);

	stop_axisd(fx, fx->first);
	axis_test_relay_free(relay);
}

/*
 * Spec (database): silence that lasts is a loss.  The lock's connection
 * stalls and no new connection is answered either: the lock is counted lost
 * once no question about it has been confirmed for 20 seconds (the check
 * comes every 5), and taken again once the server can be reached.
 */
static void
test_silence_is_lost(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *url = NULL;
	AxisTestRelay *relay;
	gint64 stalled;
	gint64 took_ms;

	if (fx->db == NULL)
		return;

	relay = axis_test_relay_new(axis_test_db_get_host(fx->db), axis_test_db_get_port(fx->db));
	url = axis_test_db_url_via(fx->db, axis_test_relay_get_port(relay), NULL);
	fx->first_port = free_port();
	fx->first = start_axisd_via(fx, fx->first_port, "first", NULL, url);
	expect_log(fx->first, "listening on", START_MS);
	expect_answer(fx->first, fx->first_port, "/readyz", 200, READY, 30000);

	axis_test_relay_hold_new(relay, TRUE);
	axis_test_relay_stall(relay, 2, TRUE);
	stalled = g_get_monotonic_time();
	expect_log(fx->first, "lost the database lock: the server has not confirmed it for 20 s", 60000);
	took_ms = (g_get_monotonic_time() - stalled) / 1000;
	g_test_message("counted lost %" G_GINT64_FORMAT " ms after the connections went silent", took_ms);
	g_assert_cmpint(took_ms, >=, 14000);
	g_assert_cmpint(took_ms, <, 35000);
	expect_answer(fx->first, fx->first_port, "/readyz", 503, LOCK_LOST, 10000);

	axis_test_relay_hold_new(relay, FALSE);
	axis_test_relay_stall(relay, 2, FALSE);
	expect_log(fx->first, "holds the database lock again", 60000);
	expect_answer(fx->first, fx->first_port, "/readyz", 200, READY, 30000);

	stop_axisd(fx, fx->first);
	axis_test_relay_free(relay);
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);

	alarm(900);

	ADD("/daemon/lock/second-waits-then-takes-over", test_second_waits_then_takes_over);
	ADD("/daemon/lock/second-does-not-wait", test_second_does_not_wait);
	ADD("/daemon/lock/lost-and-taken-again", test_lost_and_taken_again);
	ADD("/daemon/lock/lost-to-another", test_lost_to_another);
	ADD("/daemon/lock/stuck-handler-ends-process", test_stuck_handler_ends_process);
	ADD("/daemon/lock/slow-check-not-lost", test_slow_check_not_lost);
	ADD("/daemon/lock/silence-is-lost", test_silence_is_lost);

	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
