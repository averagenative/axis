/*
 * test-db-resilience.c - experiments of the database resilience review
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What a review of how Axis behaves when its database fails measured
 * (openspec/specs/database/spec.md).  Two kinds of
 * case:
 *
 *  - /db/review/catalog/...: what the migrated schema and a pooled session
 *    look like.  They pass, and print what they find (run with --verbose,
 *    or read the "# " lines of the TAP output).
 *  - /db/review/wanted/...: what a finding of the review says should hold.
 *    They fail with the code as it is, so they skip, naming the finding
 *    (F-nn, listed in the change's design.md), unless
 *    AXIS_REVIEW_DB_FAILING=1 is set.  The change that fixes a finding
 *    turns its case into an ordinary test.
 *
 * Fixed since, and ordinary tests now:
 *
 *  - F-07 (add-single-daemon-lock): /db/review/orphan-of-same-name-swept,
 *    and the single-daemon lock itself (/db/lock/...) and what the job
 *    queue does with it (/jobs/lock/..., /jobs/take-over/...).  The lock
 *    as two real daemons meet it is in tests/test-daemon-lock.c.
 */

#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-relay.h"
#include "db/axis-db-error.h"
#include "db/axis-db-lock.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"

/* Whether a "wanted" case runs; otherwise it skips naming @finding. */
static gboolean
wanted(const gchar *finding)
{
	if (g_strcmp0(g_getenv("AXIS_REVIEW_DB_FAILING"), "1") == 0)
		return TRUE;

	g_test_skip_printf("%s of review-database-resilience: fails until it is fixed "
		"(AXIS_REVIEW_DB_FAILING=1 runs it)", finding);
	return FALSE;
}

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

/* Runs @sql on a pooled connection; fails the test if it fails. */
static AxisDbResult *
pool_query(
	Fixture     *fx,
	const gchar *sql
){
	g_autoptr(GError) error = NULL;
	AxisDbResult *result = NULL;

	g_assert_true(axis_db_pool_exec(fx->pool, sql, 0, NULL, &result, &error));
	g_assert_no_error(error);
	return result;
}

/* Prints every row of @result, its columns joined by " | ". */
static void
print_rows(
	const gchar  *title,
	AxisDbResult *result
){
	guint n = axis_db_result_get_n_rows(result);
	guint c = axis_db_result_get_n_columns(result);
	guint i;
	guint j;

	g_test_message("%s: %u row(s)", title, n);
	for (i = 0; i < n; i++) {
		GString *line = g_string_new(NULL);

		for (j = 0; j < c; j++) {
			const gchar *value = axis_db_result_get_text(result, i, j);

			if (j > 0)
				g_string_append(line, " | ");
			g_string_append(line, value != NULL ? value : "NULL");
		}
		g_test_message("  %s", line->str);
		g_string_free(line, TRUE);
	}
}

/*
 * The catalog
 */

/* Every foreign key, with what deleting the row it points at does. */
static const gchar fk_actions_sql[] =
	"SELECT c.conrelid::regclass::text, c.confrelid::regclass::text,"
	" CASE c.confdeltype WHEN 'a' THEN 'no action' WHEN 'r' THEN 'restrict'"
	"  WHEN 'c' THEN 'cascade' WHEN 'n' THEN 'set null' WHEN 'd' THEN 'set default' END,"
	" (SELECT string_agg(a.attname, ',' ORDER BY k.n) FROM unnest(c.conkey) WITH ORDINALITY k(att, n)"
	"   JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.att)"
	" FROM pg_constraint c WHERE c.contype = 'f'"
	" ORDER BY 2, 1, 4";

/* Foreign keys whose columns lead no index of the referencing table: a
 * delete of the referenced row (or a cascade) scans that table. */
static const gchar fk_unindexed_sql[] =
	"SELECT c.conrelid::regclass::text, c.confrelid::regclass::text,"
	" CASE c.confdeltype WHEN 'a' THEN 'no action' WHEN 'r' THEN 'restrict'"
	"  WHEN 'c' THEN 'cascade' WHEN 'n' THEN 'set null' WHEN 'd' THEN 'set default' END,"
	" (SELECT string_agg(a.attname, ',' ORDER BY k.n) FROM unnest(c.conkey) WITH ORDINALITY k(att, n)"
	"   JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.att)"
	" FROM pg_constraint c WHERE c.contype = 'f' AND NOT EXISTS ("
	"  SELECT 1 FROM pg_index i WHERE i.indrelid = c.conrelid AND i.indpred IS NULL"
	"   AND (string_to_array(i.indkey::text, ' ')::int2[])[1:cardinality(c.conkey)] @> c.conkey"
	"   AND (string_to_array(i.indkey::text, ' ')::int2[])[1:cardinality(c.conkey)] <@ c.conkey)"
	" ORDER BY 1, 4";

/* Tables, whether they have a primary key, and how many foreign keys
 * point at them. */
static const gchar tables_sql[] =
	"SELECT t.relname, EXISTS (SELECT 1 FROM pg_constraint p WHERE p.conrelid = t.oid AND p.contype = 'p'),"
	" (SELECT count(*) FROM pg_constraint f WHERE f.confrelid = t.oid AND f.contype = 'f'),"
	" (SELECT count(*) FROM pg_index i WHERE i.indrelid = t.oid)"
	" FROM pg_class t JOIN pg_namespace n ON n.oid = t.relnamespace"
	" WHERE t.relkind = 'r' AND n.nspname = 'public' ORDER BY 1";

static void
test_catalog_foreign_keys(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) actions = NULL;
	g_autoptr(AxisDbResult) unindexed = NULL;
	g_autoptr(AxisDbResult) summary = NULL;
	g_autoptr(AxisDbResult) tables = NULL;

	if (fx->db == NULL)
		return;

	tables = pool_query(fx, tables_sql);
	print_rows("tables (name | has a primary key | foreign keys pointing at it | indexes)", tables);
	actions = pool_query(fx, fk_actions_sql);
	print_rows("foreign keys (table | references | on delete | columns)", actions);
	summary = pool_query(fx,
		"SELECT confdeltype::text, count(*) FROM pg_constraint WHERE contype = 'f' GROUP BY 1 ORDER BY 1");
	print_rows("foreign keys by delete action (a no action, r restrict, c cascade, n set null)", summary);
	unindexed = pool_query(fx, fk_unindexed_sql);
	print_rows("foreign keys with no index on their columns (table | references | on delete | columns)",
		unindexed);

	g_assert_cmpuint(axis_db_result_get_n_rows(tables), >, 0);
}

/* What a pooled session runs with, and what the server it talks to does
 * about durability and corruption.  The test server is started by
 * tests/with-postgres.sh with fsync off on purpose; its data_checksums is
 * what postgres:17's own initdb gives, as in the deployment examples. */
static void
test_catalog_session(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;

	if (fx->db == NULL)
		return;

	result = pool_query(fx,
		"SELECT name, setting, unit, source FROM pg_settings WHERE name IN ("
		"'statement_timeout', 'lock_timeout', 'idle_in_transaction_session_timeout', 'idle_session_timeout',"
		"'deadlock_timeout', 'data_checksums', 'fsync', 'synchronous_commit', 'full_page_writes',"
		"'autovacuum', 'autovacuum_vacuum_scale_factor', 'autovacuum_vacuum_insert_scale_factor',"
		"'tcp_keepalives_idle', 'server_version', 'max_connections', 'TimeZone') ORDER BY 1");
	print_rows("pooled session settings (name | setting | unit | source)", result);
}

/*
 * Wanted: what the findings say should hold
 */

/* F-10: a pooled session waits for a row or table lock for as long as the
 * statement timeout allows (a minute), never less. */
static void
test_wanted_lock_timeout(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisDbResult) result = NULL;

	if (fx->db == NULL || !wanted("F-10"))
		return;

	result = pool_query(fx, "SHOW lock_timeout");
	g_test_message("lock_timeout on a pooled connection: %s", axis_db_result_get_text(result, 0, 0));
	g_assert_cmpstr(axis_db_result_get_text(result, 0, 0), !=, "0");
}

/*
 * F-11: a deadlock comes back as AXIS_DB_ERROR_QUERY, the code of any
 * failed statement, so no caller can tell it from a bug and retry it.
 */
typedef struct
{
	AxisDbPool *pool;
	GError     *error;	/* of the second update, if it failed */
	gboolean    ok;
} Side;

/* The other side: row 2, then row 1 (which the test holds). */
static gpointer
deadlock_side(gpointer data)
{
	Side *side = data;
	AxisDbConn *conn = axis_db_pool_acquire(side->pool, &side->error);

	if (conn == NULL)
		return NULL;
	side->ok = axis_db_conn_begin(conn, &side->error) &&
		axis_db_conn_exec(conn, "UPDATE review_row SET v = v + 1 WHERE id = 2", 0, NULL, NULL,
			&side->error) &&
		axis_db_conn_exec(conn, "SELECT pg_sleep(0.3)", 0, NULL, NULL, &side->error) &&
		axis_db_conn_exec(conn, "UPDATE review_row SET v = v + 1 WHERE id = 1", 0, NULL, NULL,
			&side->error) &&
		axis_db_conn_commit(conn, &side->error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(side->pool, conn);
	return NULL;
}

static void
test_wanted_deadlock_retryable(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	GError *deadlock;
	Side side;
	GThread *thread;
	AxisDbConn *conn;
	gboolean ok;

	if (fx->db == NULL)
		return;

	axis_test_db_exec(fx->db, "CREATE TABLE review_row (id integer PRIMARY KEY, v integer NOT NULL)");
	axis_test_db_exec(fx->db, "INSERT INTO review_row VALUES (1, 0), (2, 0)");

	memset(&side, 0, sizeof(side));
	side.pool = fx->pool;
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_begin(conn, &error));
	g_assert_true(axis_db_conn_exec(conn, "UPDATE review_row SET v = v + 1 WHERE id = 1", 0, NULL, NULL,
		&error));
	thread = g_thread_new("review-deadlock", deadlock_side, &side);
	/* Until the other side holds row 2. */
	g_assert_true(axis_db_conn_exec(conn, "SELECT pg_sleep(0.15)", 0, NULL, NULL, &error));
	ok = axis_db_conn_exec(conn, "UPDATE review_row SET v = v + 1 WHERE id = 2", 0, NULL, NULL, &error);
	if (ok)
		ok = axis_db_conn_commit(conn, &error);
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(fx->pool, conn);
	g_thread_join(thread);

	/* Exactly one side is chosen by the server's deadlock detector. */
	g_assert_true(ok != side.ok);
	deadlock = ok ? side.error : error;
	g_assert_nonnull(deadlock);
	g_test_message("the deadlock as the caller sees it: domain %s, code %d, \"%s\"",
		g_quark_to_string(deadlock->domain), deadlock->code, deadlock->message);
	g_assert_nonnull(strstr(deadlock->message, "40P01"));

	if (wanted("F-11"))
		g_assert_false(g_error_matches(deadlock, AXIS_DB_ERROR, AXIS_DB_ERROR_QUERY));
	g_clear_error(&side.error);
}

/*
 * A handler that waits for a gate: it succeeds once the gate is open, and
 * fails as cancelled when its context tells it to stop first (recording
 * why).  With @progress it reports progress while it waits; with
 * @linger_ms it takes that long to stop, reporting progress meanwhile.
 */
typedef struct
{
	GMutex            lock;
	GCond             cond;
	gboolean          open;
	gboolean          progress;
	guint             linger_ms;
	guint             started;
	guint             returned;
	AxisJobStopReason reason;	/* of the last run told to stop */
} Gate;

static void
gate_init(Gate *gate)
{
	memset(gate, 0, sizeof(*gate));
	g_mutex_init(&gate->lock);
	g_cond_init(&gate->cond);
}

static void
gate_clear(Gate *gate)
{
	g_mutex_clear(&gate->lock);
	g_cond_clear(&gate->cond);
}

static void
gate_open(Gate *gate)
{
	g_mutex_lock(&gate->lock);
	gate->open = TRUE;
	g_cond_broadcast(&gate->cond);
	g_mutex_unlock(&gate->lock);
}

static guint
gate_get(
	Gate  *gate,
	guint *counter
){
	guint n;

	g_mutex_lock(&gate->lock);
	n = *counter;
	g_mutex_unlock(&gate->lock);
	return n;
}

static gboolean
gate_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	Gate *gate = user_data;
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	gboolean open = FALSE;

	g_mutex_lock(&gate->lock);
	gate->started++;
	g_cond_broadcast(&gate->cond);
	g_mutex_unlock(&gate->lock);

	for (;;) {
		g_mutex_lock(&gate->lock);
		open = gate->open;
		g_mutex_unlock(&gate->lock);
		if (gate->progress)
			axis_job_context_set_progress(context, 0.5, "waiting at the gate");
		if (open || g_get_monotonic_time() >= deadline || axis_job_context_wait(context, 20))
			break;
	}
	/* A handler slow to stop, still reporting progress. */
	if (!open && gate->linger_ms > 0) {
		gint64 until = g_get_monotonic_time() + (gint64)gate->linger_ms * 1000;

		while (g_get_monotonic_time() < until) {
			axis_job_context_set_progress(context, 0.6, "stopping");
			g_usleep(50 * 1000);
		}
	}

	g_mutex_lock(&gate->lock);
	gate->returned++;
	if (!open)
		gate->reason = axis_job_context_get_stop_reason(context);
	g_cond_broadcast(&gate->cond);
	g_mutex_unlock(&gate->lock);

	if (open)
		return TRUE;
	if (axis_job_context_is_cancelled(context))
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "told to stop");
	else
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "the gate never opened");
	return FALSE;
}

typedef struct
{
	Gate  *gate;
	guint *counter;
	guint  at_least;
} GateWait;

static gboolean
gate_reached(gpointer data)
{
	GateWait *wait = data;

	return gate_get(wait->gate, wait->counter) >= wait->at_least;
}

/* Pumps until @counter of @gate is at least @n. */
static void
gate_wait(
	Gate  *gate,
	guint *counter,
	guint  n
){
	GateWait wait;

	wait.gate = gate;
	wait.counter = counter;
	wait.at_least = n;
	g_assert_true(axis_test_pump_until(gate_reached, &wait, AXIS_TEST_WAIT_MS));
}

/* Kind "review.gate" in queue "review", and the remote kind
 * "review.remote" (queue "remote"), which no thread here claims. */
static AxisJobRegistry *
gate_registry(Gate *gate)
{
	g_autoptr(GError) error = NULL;
	AxisJobRegistry *registry = axis_job_registry_new();
	AxisJobHandlerInfo info;

	memset(&info, 0, sizeof(info));
	info.kind = "review.gate";
	info.queue = "review";
	info.max_attempts = 3;
	info.handler = gate_handler;
	g_assert_true(axis_job_registry_add(registry, &info, gate, &error));
	g_assert_true(axis_job_registry_add_remote(registry, "review.remote", "remote", 3, "A remote job",
		AXIS_JOB_CLASS_WORK, NULL, &error));
	g_assert_no_error(error);
	return registry;
}

/* A job left running under @worker, its lease running out after @lease (an
 * interval).  Returns its id. */
static gchar *
insert_running(
	Fixture     *fx,
	const gchar *kind,
	const gchar *queue,
	const gchar *worker,
	const gchar *lease
){
	return axis_test_db_queryf(fx->db, "INSERT INTO job (kind, queue, state, max_attempts, attempts, lease_token,"
		" lease_expires_at, worker_id, started_at, heartbeat_at) VALUES ('%s', '%s', 'running', 3, 1,"
		" gen_random_uuid(), now() + interval '%s', '%s', now(), now()) RETURNING id::text",
		kind, queue, lease, worker);
}

static AxisJobQueue *
quick_queue(
	Fixture         *fx,
	AxisJobRegistry *registry,
	guint            lease_ms
){
	AxisJobQueue *queue = axis_job_queue_new(fx->pool, registry);

	axis_job_queue_set_poll_interval(queue, 100);
	axis_job_queue_set_lease(queue, lease_ms);
	axis_job_queue_set_sweep_interval(queue, 100);
	return queue;
}

static void
stop_queue(AxisJobQueue *queue)
{
	axis_job_queue_stop(queue, 5000);
	g_assert_true(axis_job_queue_wait_threads(queue, AXIS_TEST_WAIT_MS));
}

static gint64
enqueue_gate(AxisJobQueue *queue)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	gint64 id = 0;

	g_assert_true(axis_job_queue_enqueue(queue, "review.gate", payload, NULL, &id, NULL, &error));
	g_assert_no_error(error);
	return id;
}

static gboolean
wait_job(
	Fixture     *fx,
	gint64       id,
	const gchar *expression,
	const gchar *expected
){
	g_autofree gchar *sql = g_strdup_printf("SELECT %s FROM job WHERE id = %" G_GINT64_FORMAT, expression, id);

	return axis_test_wait_query(fx->db, sql, expected, AXIS_TEST_WAIT_MS);
}

static gchar *
job_value(
	Fixture     *fx,
	const gchar *id,
	const gchar *expression
){
	return axis_test_db_queryf(fx->db, "SELECT %s FROM job WHERE id = %s", expression, id);
}

/*
 * F-07, fixed by add-single-daemon-lock.  The lease sweep leaves alone
 * every job whose worker is one of the queue's own busy workers.  A
 * container that restarts keeps its host name and runs axisd as process 1,
 * so its workers used to have the names of the last run's, and a job the
 * last run was running when it died was not taken back while the new worker
 * of the same name was busy.  Worker names now carry an instance made at
 * each start.  Spec (job-queue): A restarted daemon takes back its own
 * jobs.
 */
static void
test_orphan_of_same_name_swept(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *old_name = NULL;
	g_autofree gchar *earlier_name = NULL;
	g_autofree gchar *old_orphan = NULL;
	g_autofree gchar *orphan = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *swept_sql = NULL;
	Gate gate;
	gint64 busy;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lease of worker*expired*");
	gate_init(&gate);
	registry = gate_registry(&gate);

	/* What earlier runs in this container left, their leases about to run
	 * out: one named as daemons before the fix named their first worker of
	 * queue "review" (the name the review found shared), one by a run since
	 * (same host, same process id, another instance). */
	old_name = g_strdup_printf("axisd:%s:%d/review/1", g_get_host_name(), (int)getpid());
	earlier_name = g_strdup_printf("axisd:%s:%d:0123456789abcdef/review/1", g_get_host_name(), (int)getpid());
	old_orphan = insert_running(fx, "review.gate", "review", old_name, "300 milliseconds");
	orphan = insert_running(fx, "review.gate", "review", earlier_name, "300 milliseconds");

	queue = quick_queue(fx, registry, 1500);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* This run's workers have a name of their own. */
	prefix = g_strdup_printf("axisd:%s:%d:", g_get_host_name(), (int)getpid());
	g_assert_true(g_str_has_prefix(axis_job_queue_get_daemon_id(queue), prefix));
	g_assert_cmpuint(strlen(axis_job_queue_get_daemon_id(queue)), ==, strlen(prefix) + AXIS_JOB_INSTANCE_LEN);
	g_assert_false(g_str_has_prefix(earlier_name, axis_job_queue_get_daemon_id(queue)));

	/* The new worker of queue "review" is busy with new work throughout. */
	busy = enqueue_gate(queue);
	gate_wait(&gate, &gate.started, 1);

	/* Both are taken back once their leases have run out, with sweeps every
	 * 100 ms, while that worker is still busy. */
	swept_sql = g_strdup_printf("SELECT count(*) FROM job WHERE id IN (%s, %s) AND state = 'queued'"
		" AND last_error LIKE '%%lease%%expired%%'", old_orphan, orphan);
	g_assert_true(axis_test_wait_query(fx->db, swept_sql, "2", 3000));
	g_assert_cmpuint(gate_get(&gate, &gate.started), ==, 1);
	g_assert_true(wait_job(fx, busy, "state::text", "running"));

	/* Then each runs once more. */
	gate_open(&gate);
	g_assert_true(wait_job(fx, busy, "state::text", "succeeded"));
	g_assert_true(wait_job(fx, g_ascii_strtoll(old_orphan, NULL, 10), "state::text || attempts", "succeeded2"));
	g_assert_true(wait_job(fx, g_ascii_strtoll(orphan, NULL, 10), "state::text || attempts", "succeeded2"));

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * Spec (job-queue): Jobs an earlier run left are taken back at start.  A
 * daemon that holds the database alone takes back, once the take-over delay
 * has passed, what in-process workers of other daemon runs left running,
 * whatever their leases; not the jobs of remote workers, nor those of
 * daemons from before the lock (which may still be running).
 */
static void
test_take_over_earlier_runs(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *here = NULL;
	g_autofree gchar *elsewhere = NULL;
	g_autofree gchar *old_daemon = NULL;
	g_autofree gchar *remote = NULL;
	g_autofree gchar *value = NULL;
	g_autofree gchar *name = NULL;
	Gate gate;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*its daemon * no longer runs*");
	gate_init(&gate);
	gate_open(&gate);
	registry = gate_registry(&gate);

	/* Every lease has an hour to run. */
	name = g_strdup_printf("axisd:%s:%d:fedcba9876543210/review/1", g_get_host_name(), (int)getpid());
	here = insert_running(fx, "review.gate", "review", name, "1 hour");
	elsewhere = insert_running(fx, "review.gate", "review", "axisd:other-box:1:00112233aabbccdd/review/2", "1 hour");
	g_free(name);
	name = g_strdup_printf("axisd:%s:%d/review/1", g_get_host_name(), (int)getpid());
	old_daemon = insert_running(fx, "review.gate", "review", name, "1 hour");
	remote = insert_running(fx, "review.remote", "remote", "worker:attic", "1 hour");

	queue = quick_queue(fx, registry, 1500);
	axis_job_queue_set_take_over(queue, 1000);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* Not before the delay. */
	value = job_value(fx, here, "state::text");
	g_assert_cmpstr(value, ==, "running");

	/* Then at once, and they run again. */
	g_assert_true(wait_job(fx, g_ascii_strtoll(here, NULL, 10),
		"(last_error LIKE 'taken back from axisd:%:fedcba9876543210, which no longer runs%')::text", "true"));
	g_assert_true(wait_job(fx, g_ascii_strtoll(here, NULL, 10), "state::text || attempts", "succeeded2"));
	g_assert_true(wait_job(fx, g_ascii_strtoll(elsewhere, NULL, 10), "state::text || attempts", "succeeded2"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*its daemon * no longer runs*"), ==, 2);

	/* The rest keep their leases. */
	g_free(value);
	value = job_value(fx, old_daemon, "state::text || ' ' || (lease_expires_at > now() + interval '50 minutes')");
	g_assert_cmpstr(value, ==, "running true");
	g_free(value);
	value = job_value(fx, remote, "state::text || ' ' || worker_id");
	g_assert_cmpstr(value, ==, "running worker:attic");

	stop_queue(queue);
	gate_clear(&gate);
}

/* Spec (job-queue): a take-over still to come when the queue is suspended
 * waits a whole delay from the resume (another daemon may have held the
 * database meanwhile). */
static void
test_take_over_after_resume(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *earlier = NULL;
	g_autofree gchar *value = NULL;
	Gate gate;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*its daemon * no longer runs*");
	gate_init(&gate);
	gate_open(&gate);
	registry = gate_registry(&gate);
	earlier = insert_running(fx, "review.gate", "review", "axisd:other-box:1:00112233aabbccdd/review/1", "1 hour");

	queue = quick_queue(fx, registry, 1500);
	axis_job_queue_set_take_over(queue, 3000);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	axis_job_queue_suspend(queue);

	/* Suspended past the delay: nothing is taken back. */
	axis_test_pump_for(4000);
	value = job_value(fx, earlier, "state::text");
	g_assert_cmpstr(value, ==, "running");

	/* Resumed: not at once, but a whole delay later. */
	axis_job_queue_resume(queue);
	axis_test_pump_for(1000);
	g_free(value);
	value = job_value(fx, earlier, "state::text");
	g_assert_cmpstr(value, ==, "running");
	g_assert_true(wait_job(fx, g_ascii_strtoll(earlier, NULL, 10), "state::text || attempts", "succeeded2"));

	stop_queue(queue);
	gate_clear(&gate);
}

/* Spec (job-queue): a queue that is not told it holds the database alone
 * (every test, every remote worker's API) takes nothing back early. */
static void
test_take_over_off_by_default(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *earlier = NULL;
	g_autofree gchar *value = NULL;
	Gate gate;

	if (fx->db == NULL)
		return;

	gate_init(&gate);
	gate_open(&gate);
	registry = gate_registry(&gate);
	earlier = insert_running(fx, "review.gate", "review", "axisd:other-box:1:00112233aabbccdd/review/1", "1 hour");

	queue = quick_queue(fx, registry, 1500);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	axis_test_pump_for(1500);
	value = job_value(fx, earlier, "state::text");
	g_assert_cmpstr(value, ==, "running");

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * The single-daemon lock (db/axis-db-lock.h)
 */

static gint64
lock_deadline(void)
{
	return g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
}

typedef struct
{
	AxisDbLock *lock;
} LockTry;

static gboolean
lock_taken(gpointer data)
{
	LockTry *try = data;

	return axis_db_lock_try(try->lock, lock_deadline(), NULL, NULL) == AXIS_DB_LOCK_TAKEN;
}

/* Ends @pid's session from the server's side, as an administrator would. */
static void
end_session(
	Fixture *fx,
	gint     pid
){
	g_autofree gchar *sql = g_strdup_printf("SELECT pg_terminate_backend(%d, 5000)::text", pid);
	g_autofree gchar *out = axis_test_db_admin_query(fx->db, sql);

	g_assert_cmpstr(out, ==, "true");
}

/* Spec (database): A second daemon starts.  The second lock is refused,
 * says who holds it, and is taken once the first goes. */
static void
test_lock_second_busy(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbLock) first = NULL;
	g_autoptr(AxisDbLock) second = NULL;
	g_autoptr(AxisDbLockHolder) holder = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *expected = NULL;
	LockTry try;
	gint pid;

	if (fx->db == NULL)
		return;

	first = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review lock one");
	second = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review lock two");
	g_assert_false(axis_db_lock_is_held(first));
	g_assert_cmpint(axis_db_lock_try(first, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	g_assert_no_error(error);
	g_assert_true(axis_db_lock_is_held(first));
	pid = axis_db_lock_get_backend_pid(first);
	g_assert_cmpint(pid, >, 0);
	/* Taken twice is still taken. */
	g_assert_cmpint(axis_db_lock_try(first, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);

	g_assert_cmpint(axis_db_lock_try(second, lock_deadline(), &holder, &error), ==, AXIS_DB_LOCK_BUSY);
	g_assert_no_error(error);
	g_assert_false(axis_db_lock_is_held(second));
	g_assert_nonnull(holder);
	g_assert_cmpint(holder->pid, ==, pid);
	g_assert_cmpstr(holder->application, ==, "review lock one");
	g_assert_nonnull(holder->since);
	text = axis_db_lock_holder_describe(holder);
	expected = g_strdup_printf("session %d (review lock one)", pid);
	g_assert_true(g_str_has_prefix(text, expected));
	g_test_message("the second lock is held by %s", text);
	/* Asked again, as a standby asks, on the connection it kept. */
	g_assert_cmpint(axis_db_lock_try(second, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_BUSY);
	g_assert_cmpint(axis_db_lock_check(first, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_HELD);
	g_assert_no_error(error);

	/* Spec (database): The first daemon stops. */
	g_clear_pointer(&first, axis_db_lock_free);
	try.lock = second;
	g_assert_true(axis_test_pump_until(lock_taken, &try, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_db_lock_is_held(second));
	g_assert_cmpint(axis_db_lock_get_backend_pid(second), >, 0);
	g_assert_cmpint(axis_db_lock_check(second, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_HELD);
	g_assert_no_error(error);
}

/* A session that ended without the server noticing yet, found holding the
 * lock under this lock's own name, is ended and the lock taken. */
static void
test_lock_earlier_session_ended(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbLock) earlier = NULL;
	g_autoptr(AxisDbLock) now = NULL;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*an earlier connection of this daemon*");
	earlier = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review same name");
	now = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review same name");
	g_assert_cmpint(axis_db_lock_try(earlier, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	g_assert_cmpint(axis_db_lock_try(now, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	g_assert_no_error(error);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*an earlier connection of this daemon*"), ==, 1);

	/* The one that was ended sees it at once. */
	g_assert_cmpint(axis_db_lock_wait(earlier, -1, lock_deadline(), &error), ==, AXIS_DB_LOCK_LOST);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);
	g_assert_false(axis_db_lock_is_held(earlier));
}

/* Spec (database): The lock's connection is lost.  The server ends the
 * session: the lock sees it at once, and can be taken again on a new
 * session. */
static void
test_lock_lost_noticed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbLock) lock = NULL;
	gint64 start;
	gint pid;

	if (fx->db == NULL)
		return;

	lock = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review lost");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	pid = axis_db_lock_get_backend_pid(lock);
	/* Nothing happens: the wait runs out. */
	g_assert_cmpint(axis_db_lock_wait(lock, -1, g_get_monotonic_time() + 200 * 1000, &error), ==,
		AXIS_DB_LOCK_TIMEOUT);
	g_assert_no_error(error);

	end_session(fx, pid);
	start = g_get_monotonic_time();
	g_assert_cmpint(axis_db_lock_wait(lock, -1, lock_deadline(), &error), ==, AXIS_DB_LOCK_LOST);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);
	g_test_message("noticed %" G_GINT64_FORMAT " ms after the session was ended: %s",
		(g_get_monotonic_time() - start) / 1000, error->message);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 3 * G_USEC_PER_SEC);
	g_assert_false(axis_db_lock_is_held(lock));
	g_assert_cmpint(axis_db_lock_get_backend_pid(lock), ==, 0);
	g_clear_error(&error);
	g_assert_cmpint(axis_db_lock_check(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_GONE);
	g_clear_error(&error);

	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	g_assert_no_error(error);
	g_assert_cmpint(axis_db_lock_get_backend_pid(lock), !=, pid);
	g_assert_cmpint(axis_db_lock_check(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_HELD);
}

/* A session of the test's own, named @name, waiting for the daemon lock
 * (in the server's queue) and keeping it once granted, until let go. */
typedef struct
{
	AxisDbPool  *pool;
	const gchar *name;
	GThread     *thread;
	GMutex       lock;
	GCond        cond;
	gboolean     held;
	gboolean     release;
} Waiter;

static gpointer
waiter_main(gpointer data)
{
	Waiter *waiter = data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *key = g_strdup_printf("%" G_GINT64_FORMAT, AXIS_DB_DAEMON_LOCK_KEY);
	const gchar *params[1];
	AxisDbConn *conn = axis_db_pool_acquire(waiter->pool, &error);
	gboolean ok = conn != NULL;

	params[0] = waiter->name;
	if (ok)
		ok = axis_db_conn_exec(conn, "SELECT set_config('application_name', $1, false)", 1, params, NULL, &error);
	params[0] = key;
	if (ok)
		ok = axis_db_conn_exec(conn, "SELECT pg_advisory_lock($1::bigint)", 1, params, NULL, &error);
	g_mutex_lock(&waiter->lock);
	waiter->held = ok;
	g_cond_broadcast(&waiter->cond);
	while (!waiter->release)
		g_cond_wait(&waiter->cond, &waiter->lock);
	g_mutex_unlock(&waiter->lock);
	if (conn != NULL) {
		/* Closed, not reused: its name and lock go with it. */
		axis_db_conn_exec(conn, "SELECT pg_advisory_unlock_all()", 0, NULL, NULL, NULL);
		axis_db_pool_release(waiter->pool, conn);
	}
	return NULL;
}

static gboolean
waiter_holds(gpointer data)
{
	Waiter *waiter = data;
	gboolean held;

	g_mutex_lock(&waiter->lock);
	held = waiter->held;
	g_mutex_unlock(&waiter->lock);
	return held;
}

/*
 * Finding 5 of the review: a session of this lock's own name that still
 * holds the lock after the earlier one was ended is the lock's own, never
 * another daemon's.  Here the server hands the lock, the moment the earlier
 * session ends, to a second session of the same name that was waiting for
 * it: the try says OURS (before the fix: BUSY, and the daemon would have
 * stopped, saying another daemon took its lock), and the lock is taken once
 * that one goes.
 */
static void
test_lock_ours_not_busy(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbLock) earlier = NULL;
	g_autoptr(AxisDbLock) now = NULL;
	g_autoptr(AxisDbLockHolder) holder = NULL;
	Waiter waiter;
	LockTry try;
	gint64 started;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*an earlier connection of this daemon*");
	earlier = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review ours");
	g_assert_cmpint(axis_db_lock_try(earlier, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);

	memset(&waiter, 0, sizeof(waiter));
	waiter.pool = axis_test_jobs_pool(fx->db, 1);
	waiter.name = "review ours";
	g_mutex_init(&waiter.lock);
	g_cond_init(&waiter.cond);
	waiter.thread = g_thread_new("review-waiter", waiter_main, &waiter);
	g_assert_true(axis_test_wait_query(fx->db, "SELECT count(*) FROM pg_stat_activity WHERE datname = current_database()"
		" AND wait_event_type = 'Lock' AND wait_event = 'advisory'", "1", AXIS_TEST_WAIT_MS));

	/* Within its 3 s deadline, though ending a session may be waited for. */
	now = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review ours");
	started = g_get_monotonic_time();
	g_assert_cmpint(axis_db_lock_try(now, g_get_monotonic_time() + 3 * G_USEC_PER_SEC, &holder, &error), ==,
		AXIS_DB_LOCK_OURS);
	g_assert_no_error(error);
	g_assert_null(holder);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3 * G_USEC_PER_SEC);
	g_assert_true(axis_test_pump_until(waiter_holds, &waiter, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*earlier connection of this daemon (session *) still "
		"held the lock*was ended*"), ==, 1);

	/* The waiting one goes: taken (its session is ended in turn, or it has
	 * gone by itself). */
	g_mutex_lock(&waiter.lock);
	waiter.release = TRUE;
	g_cond_broadcast(&waiter.cond);
	g_mutex_unlock(&waiter.lock);
	g_thread_join(waiter.thread);
	try.lock = now;
	g_assert_true(axis_test_pump_until(lock_taken, &try, AXIS_TEST_WAIT_MS));

	axis_db_pool_free(waiter.pool);
	g_mutex_clear(&waiter.lock);
	g_cond_clear(&waiter.cond);
}

/*
 * Finding 6 of the review: a check that is not answered proves nothing.  The
 * lock's connection is stalled (both ends open, nothing passes): the check
 * is silent, the connection stays open and the lock held, and another
 * connection confirms that the session still holds the lock.  Once the
 * connection passes data again, the check that was in flight is answered,
 * with the time it was sent.  A session that has ended is told apart: the
 * question on another connection says so.
 */
static void
test_lock_silent_then_verified(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	AxisTestRelay *relay;
	AxisDbPool *pool;
	AxisDbLock *lock;
	gint64 asked;
	gint64 sent = 0;
	gint64 vouched = 0;

	if (fx->db == NULL)
		return;

	relay = axis_test_relay_new(axis_test_db_get_host(fx->db), axis_test_db_get_port(fx->db));
	url = axis_test_db_url_via(fx->db, axis_test_relay_get_port(relay), NULL);
	pool = axis_db_pool_new(url, 2, &error);
	g_assert_no_error(error);
	lock = axis_db_lock_new(pool, AXIS_DB_DAEMON_LOCK_KEY, "review silent");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	/* The pool's first connection, then the lock's. */
	g_assert_cmpuint(axis_test_relay_count(relay), ==, 2);

	axis_test_relay_stall(relay, 2, TRUE);
	asked = g_get_monotonic_time();
	g_assert_cmpint(axis_db_lock_check(lock, g_get_monotonic_time() + 500 * 1000, &sent, &error), ==,
		AXIS_DB_LOCK_SILENT);
	g_assert_true(axis_db_lock_is_held(lock));
	g_assert_cmpint(axis_db_lock_verify(lock, lock_deadline(), &vouched, &error), ==, AXIS_DB_LOCK_HELD);
	g_assert_no_error(error);
	g_assert_cmpint(vouched, >=, asked);
	/* Still in flight, still silent. */
	g_assert_cmpint(axis_db_lock_check(lock, g_get_monotonic_time() + 300 * 1000, &sent, &error), ==,
		AXIS_DB_LOCK_SILENT);

	axis_test_relay_stall(relay, 2, FALSE);
	g_assert_cmpint(axis_db_lock_check(lock, lock_deadline(), &sent, &error), ==, AXIS_DB_LOCK_HELD);
	g_assert_no_error(error);
	g_assert_cmpint(sent, >=, asked);
	g_assert_cmpint(sent, <, vouched);

	/* The session ends: the question on another connection says no. */
	end_session(fx, axis_db_lock_get_backend_pid(lock));
	g_assert_cmpint(axis_db_lock_verify(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_GONE);
	g_assert_error(error, AXIS_DB_ERROR, AXIS_DB_ERROR_CONNECTION_LOST);

	axis_db_lock_free(lock);
	axis_db_pool_free(pool);
	axis_test_relay_free(relay);
}

/* Fences the queue with @lock's session. */
static void
fence_queue(
	AxisJobQueue *queue,
	AxisDbLock   *lock
){
	axis_job_queue_set_lock(queue, AXIS_DB_DAEMON_LOCK_KEY, axis_db_lock_get_backend_pid(lock),
		axis_db_lock_get_backend_start(lock));
}

/* After the lock's session ended outside it: the lock learns it on its next
 * use, as the daemon's watcher does, and is taken on a new session. */
static void
take_again(AxisDbLock *lock)
{
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(axis_db_lock_check(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_GONE);
	g_clear_error(&error);
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	g_assert_no_error(error);
}

/*
 * Spec (job-queue): Claims and renewals need the lock.  With the lock, a job
 * runs and its lease is renewed.  The moment the lock's session ends, the
 * running job's next renewal is refused by the fence: its handler is told to
 * stop as for a shutdown and the job handed back (its lease token is still
 * good, so it is not left running until its lease runs out), and nothing new
 * is claimed.  Held again on a new session, claims go on.
 */
static void
test_queue_fenced(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(AxisDbLock) lock = NULL;
	Gate gate;
	gint64 first;
	gint64 second;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lease was not renewed*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*cannot renew job leases*");
	lock = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review fence");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	gate_init(&gate);
	registry = gate_registry(&gate);
	queue = quick_queue(fx, registry, 600);
	fence_queue(queue, lock);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* Held: the job runs past its 600 ms lease, renewed. */
	first = enqueue_gate(queue);
	gate_wait(&gate, &gate.started, 1);
	axis_test_pump_for(1200);
	g_assert_true(wait_job(fx, first, "state::text || ' ' || (lease_expires_at > now())", "running true"));
	g_assert_cmpuint(gate_get(&gate, &gate.returned), ==, 0);

	/* The lock's session ends: the next renewal is refused, the handler
	 * stops, and the job is handed back, its attempt not counted. */
	end_session(fx, axis_db_lock_get_backend_pid(lock));
	gate_wait(&gate, &gate.returned, 1);
	g_assert_cmpint(gate.reason, ==, AXIS_JOB_STOP_SHUTDOWN);
	g_assert_true(wait_job(fx, first, "state::text || attempts", "queued0"));
	second = enqueue_gate(queue);
	axis_test_pump_for(1000);
	g_assert_true(wait_job(fx, second, "state::text", "queued"));
	g_assert_true(wait_job(fx, first, "state::text", "queued"));
	g_assert_cmpuint(gate_get(&gate, &gate.started), ==, 1);

	/* Held again, by a new session. */
	take_again(lock);
	fence_queue(queue, lock);
	gate_open(&gate);
	g_assert_true(wait_job(fx, first, "state::text || attempts", "succeeded1"));
	g_assert_true(wait_job(fx, second, "state::text || attempts", "succeeded1"));

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * Finding 1 of the review of add-single-daemon-lock.  The daemon suspends
 * its queue after its lock's session is really gone, and a handler that
 * reports progress takes a while to stop.  No lease is renewed while the
 * queue is suspended, so no renewal is refused on the way: the job is handed
 * back (before the fix each refused renewal marked the lease lost and the
 * hand-back was discarded, leaving the job running until its lease ran out).
 */
static void
test_queue_suspended_after_lock_gone(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(AxisDbLock) lock = NULL;
	Gate gate;
	gint64 job;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lease was not renewed*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*cannot renew job leases*");
	lock = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review suspended");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	gate_init(&gate);
	gate.progress = TRUE;
	gate.linger_ms = 2500;
	registry = gate_registry(&gate);
	/* Renewed every second, and progress written every second. */
	queue = quick_queue(fx, registry, 3000);
	fence_queue(queue, lock);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	job = enqueue_gate(queue);
	gate_wait(&gate, &gate.started, 1);
	axis_test_pump_for(1200);

	/* The session is gone, then the daemon notices and suspends. */
	end_session(fx, axis_db_lock_get_backend_pid(lock));
	axis_job_queue_suspend(queue);
	gate_wait(&gate, &gate.returned, 1);
	g_assert_cmpint(gate.reason, ==, AXIS_JOB_STOP_SHUTDOWN);
	g_assert_true(axis_job_queue_wait_handlers(queue, AXIS_TEST_WAIT_MS));
	g_assert_true(wait_job(fx, job, "state::text || attempts", "queued0"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*lost its lease*"), ==, 0);

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * Finding 4 of the review: the fence names the lock's session by its
 * process id and its start, because process ids repeat after a server
 * restart.  With the right process id but another start, nothing is claimed.
 */
static void
test_queue_fence_names_the_session(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(AxisDbLock) lock = NULL;
	Gate gate;
	gint64 job;

	if (fx->db == NULL)
		return;

	lock = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review start");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	g_assert_nonnull(axis_db_lock_get_backend_start(lock));
	gate_init(&gate);
	gate_open(&gate);
	registry = gate_registry(&gate);
	queue = quick_queue(fx, registry, 1500);
	axis_job_queue_set_lock(queue, AXIS_DB_DAEMON_LOCK_KEY, axis_db_lock_get_backend_pid(lock),
		"2000-01-01 00:00:00.000000");
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	job = enqueue_gate(queue);
	axis_test_pump_for(1000);
	g_assert_true(wait_job(fx, job, "state::text", "queued"));

	fence_queue(queue, lock);
	g_assert_true(wait_job(fx, job, "state::text", "succeeded"));

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * Finding 2 of the review: the take-over and the sweep are fenced.  The
 * queue starts with a lock whose session has already ended (the daemon has
 * not noticed yet): neither the job an earlier run left nor the one whose
 * lease ran out is touched, and the take-over is tried again rather than
 * given up.  Held again, both are taken back and run.
 */
static void
test_take_over_fenced(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(AxisDbLock) lock = NULL;
	g_autofree gchar *earlier = NULL;
	g_autofree gchar *expired = NULL;
	g_autofree gchar *value = NULL;
	Gate gate;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*its daemon * no longer runs*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lease of worker*expired*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*job queue maintenance*");
	gate_init(&gate);
	gate_open(&gate);
	registry = gate_registry(&gate);
	earlier = insert_running(fx, "review.gate", "review", "axisd:other-box:1:00112233aabbccdd/review/1", "1 hour");
	expired = insert_running(fx, "review.gate", "review", "worker:attic", "-1 second");

	lock = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review take-over");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	queue = quick_queue(fx, registry, 1500);
	fence_queue(queue, lock);
	axis_job_queue_set_take_over(queue, 300);
	end_session(fx, axis_db_lock_get_backend_pid(lock));
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	axis_test_pump_for(1500);
	value = job_value(fx, earlier, "state::text");
	g_assert_cmpstr(value, ==, "running");
	g_free(value);
	value = job_value(fx, expired, "state::text");
	g_assert_cmpstr(value, ==, "running");

	take_again(lock);
	fence_queue(queue, lock);
	g_assert_true(wait_job(fx, g_ascii_strtoll(earlier, NULL, 10), "state::text || attempts", "succeeded2"));
	g_assert_true(wait_job(fx, g_ascii_strtoll(expired, NULL, 10), "state::text || attempts", "succeeded2"));

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * Finding 7 of the review: the worker API's claims and expiries are fenced
 * (renewals and reports are not: see the change's design).  With a fence
 * that does not hold, a remote worker is given nothing and no lease is taken
 * back.
 */
static void
test_remote_fenced(Fixture *fx, gconstpointer data)
{
	static const gchar *const kinds[] = { "review.remote", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbLock) lock = NULL;
	g_autoptr(AxisJobClaim) claim = NULL;
	g_autofree gchar *job = NULL;
	g_autofree gchar *value = NULL;
	AxisJobRemoteFilter filter;
	AxisJobFence good;
	AxisJobFence bad;
	AxisDbConn *conn;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lease of worker*expired*");
	job = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts) VALUES ('review.remote', 'remote', 3)"
		" RETURNING id::text");
	lock = axis_db_lock_new(fx->pool, AXIS_DB_DAEMON_LOCK_KEY, "review remote");
	g_assert_cmpint(axis_db_lock_try(lock, lock_deadline(), NULL, &error), ==, AXIS_DB_LOCK_TAKEN);
	memset(&good, 0, sizeof(good));
	good.lock_key = AXIS_DB_DAEMON_LOCK_KEY;
	good.holder_pid = axis_db_lock_get_backend_pid(lock);
	g_strlcpy(good.holder_start, axis_db_lock_get_backend_start(lock), sizeof(good.holder_start));
	bad = good;
	g_strlcpy(bad.holder_start, "2000-01-01 00:00:00.000000", sizeof(bad.holder_start));
	memset(&filter, 0, sizeof(filter));
	filter.worker = "attic";
	filter.machine = "worker-attic";
	filter.volumes_json = "[]";

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_job_claim_remote_fenced(conn, "remote", kinds, "worker:attic", &filter, 60000, &bad, &claim,
		&error));
	g_assert_no_error(error);
	g_assert_null(claim);
	g_assert_true(axis_job_claim_remote_fenced(conn, "remote", kinds, "worker:attic", &filter, 60000, &good, &claim,
		&error));
	g_assert_no_error(error);
	g_assert_nonnull(claim);
	g_assert_cmpint(claim->id, ==, g_ascii_strtoll(job, NULL, 10));

	g_assert_false(axis_job_expire_remote_fenced(conn, claim->id, claim->lease_token, &bad, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_FENCED);
	g_clear_error(&error);
	value = job_value(fx, job, "state::text");
	g_assert_cmpstr(value, ==, "running");
	g_assert_true(axis_job_expire_remote_fenced(conn, claim->id, claim->lease_token, &good, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	g_free(value);
	value = job_value(fx, job, "state::text");
	g_assert_cmpstr(value, ==, "queued");
}

/*
 * Spec (job-queue): Suspended while the lock is lost.  Suspended, the queue
 * tells its running handler to stop as for a shutdown (its job is handed
 * back, the attempt not counted) and claims nothing; resumed, it goes on.
 */
static void
test_queue_suspend_resume(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	Gate gate;
	gint64 first;
	gint64 second;

	if (fx->db == NULL)
		return;

	gate_init(&gate);
	registry = gate_registry(&gate);
	queue = quick_queue(fx, registry, 1500);
	g_assert_false(axis_job_queue_is_suspended(queue));
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	first = enqueue_gate(queue);
	gate_wait(&gate, &gate.started, 1);
	g_assert_false(axis_job_queue_wait_handlers(queue, 100));

	axis_job_queue_suspend(queue);
	g_assert_true(axis_job_queue_is_suspended(queue));
	g_assert_true(axis_job_queue_wait_handlers(queue, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(gate_get(&gate, &gate.returned), ==, 1);
	g_assert_cmpint(gate.reason, ==, AXIS_JOB_STOP_SHUTDOWN);
	g_assert_true(wait_job(fx, first, "state::text || attempts", "queued0"));

	/* Nothing is claimed while suspended, the job handed back included. */
	second = enqueue_gate(queue);
	axis_test_pump_for(800);
	g_assert_true(wait_job(fx, second, "state::text", "queued"));
	g_assert_true(wait_job(fx, first, "state::text", "queued"));
	g_assert_cmpuint(gate_get(&gate, &gate.started), ==, 1);

	gate_open(&gate);
	axis_job_queue_resume(queue);
	g_assert_false(axis_job_queue_is_suspended(queue));
	g_assert_true(wait_job(fx, first, "state::text || attempts", "succeeded1"));
	g_assert_true(wait_job(fx, second, "state::text || attempts", "succeeded1"));

	stop_queue(queue);
	gate_clear(&gate);
}

/*
 * F-08: the start-up checks and /readyz look only at the migration table.
 * A table dropped by hand (or a migration recorded but not applied) is not
 * noticed: the daemon starts, says it is ready, and the pages that use the
 * table fail one by one.
 */
static void
test_wanted_missing_table_noticed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gboolean migrated;
	gboolean current;

	if (fx->db == NULL)
		return;

	axis_test_db_exec(fx->db, "DROP TABLE setup_step");
	migrated = axis_db_migrate(fx->pool, &error);
	g_clear_error(&error);
	current = axis_db_migrate_check_current(fx->pool, 2000, &error);
	g_test_message("with table setup_step dropped by hand: migrate %s, readiness check %s",
		migrated ? "succeeds" : "fails", current ? "says ready" : "says not ready");
	g_clear_error(&error);

	if (wanted("F-08"))
		g_assert_false(migrated && current);
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	alarm(240);

	ADD("/db/review/catalog/foreign-keys", test_catalog_foreign_keys);
	ADD("/db/review/catalog/session", test_catalog_session);
	ADD("/db/review/wanted/lock-timeout", test_wanted_lock_timeout);
	ADD("/db/review/wanted/deadlock-retryable", test_wanted_deadlock_retryable);
	ADD("/db/review/orphan-of-same-name-swept", test_orphan_of_same_name_swept);
	ADD("/db/review/wanted/missing-table-noticed", test_wanted_missing_table_noticed);
	ADD("/db/lock/second-busy", test_lock_second_busy);
	ADD("/db/lock/earlier-session-ended", test_lock_earlier_session_ended);
	ADD("/db/lock/lost-noticed", test_lock_lost_noticed);
	ADD("/db/lock/ours-not-busy", test_lock_ours_not_busy);
	ADD("/db/lock/silent-then-verified", test_lock_silent_then_verified);
	ADD("/jobs/lock/fenced", test_queue_fenced);
	ADD("/jobs/lock/suspended-after-lock-gone", test_queue_suspended_after_lock_gone);
	ADD("/jobs/lock/fence-names-the-session", test_queue_fence_names_the_session);
	ADD("/jobs/lock/suspend-resume", test_queue_suspend_resume);
	ADD("/jobs/lock/remote-fenced", test_remote_fenced);
	ADD("/jobs/take-over/fenced", test_take_over_fenced);
	ADD("/jobs/take-over/earlier-runs", test_take_over_earlier_runs);
	ADD("/jobs/take-over/after-resume", test_take_over_after_resume);
	ADD("/jobs/take-over/off-by-default", test_take_over_off_by_default);

	return g_test_run();
}
