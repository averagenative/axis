/*
 * test-job-lifecycle.c - the job lifecycle functions against a real PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No runner here: each test is the worker itself, calling the functions
 * the HTTP worker API will call.  Claiming under real concurrency, claim
 * order and run times, leases (expiry, stale holders, heartbeats), retries
 * and backoff, de-duplication (concurrent too), cancellation, hand-back,
 * the sweeper, retention, schedule ticks, the notifications every change
 * sends, and the claim's plan on a large table.
 */

#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "db/axis-db-error.h"
#include "db/axis-db-listener.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-registry.h"
#include "jobs/axis-job-schedule.h"

#define WORKER "test-worker"
#define LONG_LEASE_MS 60000

static const gchar *const kinds_a[] = { "test.a", NULL };

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

	/* Room for every claimer thread at the barrier, and the fixture's. */
	fx->pool = axis_test_jobs_pool(fx->db, 40);
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

static gint64
enqueue_full(
	AxisDbConn  *conn,
	const gchar *kind,
	const gchar *queue,
	gint         priority,
	gint64       delay_ms,
	const gchar *key,
	guint        max_attempts,
	gboolean    *created
){
	g_autoptr(GError) error = NULL;
	AxisJobSpec spec;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.kind = kind;
	spec.queue = queue;
	spec.priority = priority;
	spec.delay_ms = delay_ms;
	spec.dedupe_key = key;
	spec.max_attempts = max_attempts;
	g_assert_true(axis_job_enqueue(conn, &spec, &id, created, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);

	return id;
}

static gint64
enqueue(
	Fixture     *fx,
	const gchar *queue,
	guint        max_attempts
){
	return enqueue_full(fx->conn, "test.a", queue, 0, 0, NULL, max_attempts, NULL);
}

static AxisJobClaim *
claim_from(
	AxisDbConn  *conn,
	const gchar *queue,
	guint        lease_ms
){
	g_autoptr(GError) error = NULL;
	AxisJobClaim *claim = NULL;

	g_assert_true(axis_job_claim(conn, queue, kinds_a, WORKER, lease_ms, &claim, &error));
	g_assert_no_error(error);

	return claim;
}

static AxisJobClaim *
claim(
	Fixture     *fx,
	const gchar *queue
){
	return claim_from(fx->conn, queue, LONG_LEASE_MS);
}

/* The job's row as JSON text, to show that nothing changed. */
static gchar *
row(
	Fixture *fx,
	gint64   id
){
	return axis_test_db_queryf(fx->db, "SELECT row_to_json(job)::text FROM job WHERE id = %"
		G_GINT64_FORMAT, id);
}

static void
assert_column(
	Fixture     *fx,
	gint64       id,
	const gchar *expression,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_queryf(fx->db,
		"SELECT (%s)::text FROM job WHERE id = %" G_GINT64_FORMAT, expression, id);

	if (g_strcmp0(value, expected) != 0)
		g_error("job %" G_GINT64_FORMAT ": %s is %s, expected %s", id, expression,
			value != NULL ? value : "NULL", expected != NULL ? expected : "NULL");
}

static void
expire_lease(
	Fixture *fx,
	gint64   id
){
	g_autofree gchar *out = axis_test_db_queryf(fx->db,
		"UPDATE job SET lease_expires_at = now() - interval '1 second' WHERE id = %"
		G_GINT64_FORMAT " RETURNING id", id);

	g_assert_nonnull(out);
}

static guint
sweep(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	guint n = 0;

	g_assert_true(axis_job_sweep_expired_leases(fx->conn, kinds_a, NULL, &n, &error));
	g_assert_no_error(error);

	return n;
}

static void
assert_lease_lost(GError *error)
{
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_LEASE_LOST);
}

static gboolean
noop_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return TRUE;
}

/* Enqueue, claim, heartbeat, succeed: what each step stores. */
static void
test_basic_lifecycle(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	gboolean cancel = TRUE;
	gboolean created = FALSE;
	AxisJobSpec spec;
	gint64 id = 0;

	if (fx->db == NULL)
		return;

	json_object_set_string_member(payload, "path", "/media/a.mkv");
	json_object_set_int_member(payload, "size", 42);
	memset(&spec, 0, sizeof(spec));
	spec.kind = "test.a";
	spec.queue = "q";
	spec.payload = payload;
	spec.priority = 3;
	spec.max_attempts = 4;
	g_assert_true(axis_job_enqueue(fx->conn, &spec, &id, &created, &error));
	g_assert_no_error(error);
	g_assert_true(created);

	assert_column(fx, id, "state", "queued");
	assert_column(fx, id, "attempts", "0");
	assert_column(fx, id, "max_attempts", "4");
	assert_column(fx, id, "priority", "3");
	assert_column(fx, id, "payload->>'path'", "/media/a.mkv");
	assert_column(fx, id, "lease_token IS NULL AND started_at IS NULL", "true");

	c = claim(fx, "q");
	g_assert_nonnull(c);
	g_assert_cmpint(c->id, ==, id);
	g_assert_cmpstr(c->kind, ==, "test.a");
	g_assert_cmpstr(c->queue, ==, "q");
	g_assert_cmpuint(c->attempt, ==, 1);
	g_assert_cmpuint(c->max_attempts, ==, 4);
	g_assert_cmpint(c->priority, ==, 3);
	g_assert_false(c->cancel_requested);
	g_assert_true(g_uuid_string_is_valid(c->lease_token));
	g_assert_nonnull(strstr(c->payload, "/media/a.mkv"));

	assert_column(fx, id, "state", "running");
	assert_column(fx, id, "attempts", "1");
	assert_column(fx, id, "lease_token::text", c->lease_token);
	assert_column(fx, id, "worker_id", WORKER);
	assert_column(fx, id, "started_at IS NOT NULL AND lease_expires_at > now() + interval '50 seconds'",
		"true");

	/* Nothing else to claim. */
	g_assert_null(claim(fx, "q"));

	g_assert_true(axis_job_heartbeat(fx->conn, id, c->lease_token, LONG_LEASE_MS, NULL,
		&cancel, &error));
	g_assert_no_error(error);
	g_assert_false(cancel);

	g_assert_true(axis_job_succeed(fx->conn, id, c->lease_token, &error));
	g_assert_no_error(error);
	assert_column(fx, id, "state", "succeeded");
	assert_column(fx, id, "lease_token IS NULL AND lease_expires_at IS NULL", "true");
	assert_column(fx, id, "finished_at IS NOT NULL", "true");

	/* Done once is done. */
	g_assert_false(axis_job_succeed(fx->conn, id, c->lease_token, &error));
	assert_lease_lost(error);
}

/* Spec: Unknown kind is refused at enqueue */
static void
test_unknown_kind_refused(Fixture *fx, gconstpointer data)
{
	static const AxisJobHandlerInfo known = { "test.a", "q", 0, noop_handler, "Doing A", AXIS_JOB_CLASS_WORK };
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *count = NULL;
	gint64 id = 0;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_job_registry_add(registry, &known, NULL, &error));

	g_assert_false(axis_job_registry_enqueue(registry, fx->conn, "test.nope", NULL, NULL,
		&id, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND);
	g_assert_nonnull(strstr(error->message, "test.nope"));
	g_clear_error(&error);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM job");
	g_assert_cmpstr(count, ==, "0");

	/* The registered kind goes to its queue with its attempts. */
	g_assert_true(axis_job_registry_enqueue(registry, fx->conn, "test.a", NULL, NULL,
		&id, NULL, &error));
	g_assert_no_error(error);
	assert_column(fx, id, "queue || '/' || max_attempts", "q/5");
}

static void
test_invalid_arguments(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad_names[] = {
		"", "Test", "1test", "te st", "test/a", "tést",
		"a234567890123456789012345678901234567890123456789012345678901234", NULL
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *count = NULL;
	AxisJobClaim *c = NULL;
	AxisJobSpec spec;
	guint i;

	if (fx->db == NULL)
		return;

	for (i = 0; bad_names[i] != NULL; i++) {
		memset(&spec, 0, sizeof(spec));
		spec.kind = bad_names[i];
		spec.queue = "q";
		spec.max_attempts = 1;
		g_assert_false(axis_job_enqueue(fx->conn, &spec, NULL, NULL, &error));
		g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
		g_clear_error(&error);

		spec.kind = "test.a";
		spec.queue = bad_names[i];
		g_assert_false(axis_job_enqueue(fx->conn, &spec, NULL, NULL, &error));
		g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
		g_clear_error(&error);
	}

	memset(&spec, 0, sizeof(spec));
	spec.kind = "test.a";
	spec.queue = "q";
	spec.max_attempts = 0;
	g_assert_false(axis_job_enqueue(fx->conn, &spec, NULL, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	spec.max_attempts = AXIS_JOB_MAX_ATTEMPTS_LIMIT + 1;
	g_assert_false(axis_job_enqueue(fx->conn, &spec, NULL, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	spec.max_attempts = 1;
	spec.dedupe_key = "";
	g_assert_false(axis_job_enqueue(fx->conn, &spec, NULL, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);

	count = axis_test_db_query(fx->db, "SELECT count(*) FROM job");
	g_assert_cmpstr(count, ==, "0");

	g_assert_false(axis_job_claim(fx->conn, "q", kinds_a, WORKER, 0, &c, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_job_claim(fx->conn, "q", NULL, WORKER, 1000, &c, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_job_claim(fx->conn, "q", kinds_a, "", 1000, &c, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_assert_null(c);
}

/*
 * Concurrent claimers.  Every round, 32 threads wait at a barrier and then
 * each claims once from a queue holding 10 jobs.
 */
#define CLAIMERS 32

typedef struct
{
	AxisDbPool   *pool;
	const gchar  *queue;
	GMutex        lock;
	GCond         cond;
	guint         ready;
	gboolean      go;
	guint         repeat;	/* 0: claim once; else claim until empty */
	GArray       *claimed;	/* gint64 ids */
	guint         empty;	/* claimers that got nothing */
	guint         errors;
} Race;

static gpointer
claimer_thread(gpointer data)
{
	Race *race = data;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(race->pool, &error);

	g_assert_no_error(error);
	g_mutex_lock(&race->lock);
	race->ready++;
	g_cond_broadcast(&race->cond);
	while (!race->go)
		g_cond_wait(&race->cond, &race->lock);
	g_mutex_unlock(&race->lock);

	for (;;) {
		AxisJobClaim *c = NULL;
		gboolean ok = axis_job_claim(conn, race->queue, kinds_a, WORKER, LONG_LEASE_MS, &c, &error);

		g_mutex_lock(&race->lock);
		if (!ok)
			race->errors++;
		else if (c == NULL)
			race->empty++;
		else
			g_array_append_val(race->claimed, c->id);
		g_mutex_unlock(&race->lock);

		axis_job_claim_free(c);
		g_clear_error(&error);
		if (!ok || c == NULL || race->repeat == 0)
			break;
	}

	axis_db_pool_release(race->pool, conn);
	return NULL;
}

static void
run_race(
	Fixture *fx,
	Race    *race,
	guint    n_threads
){
	GThread *threads[CLAIMERS];
	guint i;

	g_assert_cmpuint(n_threads, <=, CLAIMERS);
	g_mutex_init(&race->lock);
	g_cond_init(&race->cond);
	race->pool = fx->pool;
	race->claimed = g_array_new(FALSE, FALSE, sizeof(gint64));

	for (i = 0; i < n_threads; i++)
		threads[i] = g_thread_new("claimer", claimer_thread, race);

	g_mutex_lock(&race->lock);
	while (race->ready < n_threads)
		g_cond_wait(&race->cond, &race->lock);
	race->go = TRUE;
	g_cond_broadcast(&race->cond);
	g_mutex_unlock(&race->lock);

	for (i = 0; i < n_threads; i++)
		g_thread_join(threads[i]);
	g_mutex_clear(&race->lock);
	g_cond_clear(&race->cond);
}

static gint
compare_ids(
	gconstpointer a,
	gconstpointer b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return x < y ? -1 : x > y;
}

static void
assert_claimed_exactly(
	Race   *race,
	GArray *ids
){
	guint i;

	g_assert_cmpuint(race->errors, ==, 0);
	g_assert_cmpuint(race->claimed->len, ==, ids->len);
	g_array_sort(race->claimed, compare_ids);
	g_array_sort(ids, compare_ids);
	for (i = 0; i < ids->len; i++)
		g_assert_cmpint(g_array_index(race->claimed, gint64, i), ==, g_array_index(ids, gint64, i));
}

/* Spec: Concurrent claimers */
static void
test_concurrent_claimers(Fixture *fx, gconstpointer data)
{
	guint round;

	if (fx->db == NULL)
		return;

	/* Fewer jobs than claimers: each job once, the rest get nothing. */
	for (round = 0; round < 10; round++) {
		g_autofree gchar *queue = g_strdup_printf("race%u", round);
		g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autofree gchar *running = NULL;
		Race race;
		guint i;

		for (i = 0; i < 10; i++) {
			gint64 id = enqueue(fx, queue, 1);

			g_array_append_val(ids, id);
		}

		memset(&race, 0, sizeof(race));
		race.queue = queue;
		run_race(fx, &race, CLAIMERS);
		assert_claimed_exactly(&race, ids);
		g_assert_cmpuint(race.empty, ==, CLAIMERS - 10);
		g_array_unref(race.claimed);

		running = axis_test_db_queryf(fx->db, "SELECT count(*) FROM job WHERE queue = '%s' "
			"AND state = 'running' AND attempts = 1", queue);
		g_assert_cmpstr(running, ==, "10");
	}

	/* Many jobs, claimers draining them as fast as they can. */
	{
		g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		Race race;
		guint i;

		for (i = 0; i < 400; i++) {
			gint64 id = enqueue(fx, "drain", 1);

			g_array_append_val(ids, id);
		}
		memset(&race, 0, sizeof(race));
		race.queue = "drain";
		race.repeat = 1;
		run_race(fx, &race, 16);
		assert_claimed_exactly(&race, ids);
		g_assert_cmpuint(race.empty, ==, 16);
		g_array_unref(race.claimed);
	}
}

/* Spec: Claim order */
static void
test_claim_order(Fixture *fx, gconstpointer data)
{
	gint64 expected[6];
	gint64 low;
	gint64 old_zero;
	gint64 new_zero;
	gint64 high_old;
	gint64 high_new;
	gint64 older_zero;
	g_autofree gchar *out = NULL;
	guint i;

	if (fx->db == NULL)
		return;

	low = enqueue_full(fx->conn, "test.a", "q", -1, 0, NULL, 1, NULL);
	new_zero = enqueue_full(fx->conn, "test.a", "q", 0, 0, NULL, 1, NULL);
	high_new = enqueue_full(fx->conn, "test.a", "q", 5, 0, NULL, 1, NULL);
	old_zero = enqueue_full(fx->conn, "test.a", "q", 0, 0, NULL, 1, NULL);
	high_old = enqueue_full(fx->conn, "test.a", "q", 5, 0, NULL, 1, NULL);
	older_zero = enqueue_full(fx->conn, "test.a", "q", 0, 0, NULL, 1, NULL);

	/* Run times decide within a priority, whatever the order of ids. */
	out = axis_test_db_queryf(fx->db,
		"UPDATE job SET run_at = now() - CASE id"
		" WHEN %" G_GINT64_FORMAT " THEN interval '1 minute'"
		" WHEN %" G_GINT64_FORMAT " THEN interval '5 minutes'"
		" WHEN %" G_GINT64_FORMAT " THEN interval '2 minutes'"
		" WHEN %" G_GINT64_FORMAT " THEN interval '3 minutes'"
		" WHEN %" G_GINT64_FORMAT " THEN interval '4 minutes'"
		" ELSE interval '10 minutes' END RETURNING 1",
		new_zero, old_zero, high_new, high_old, older_zero);

	expected[0] = high_old;
	expected[1] = high_new;
	expected[2] = old_zero;
	expected[3] = older_zero;
	expected[4] = new_zero;
	expected[5] = low;

	for (i = 0; i < G_N_ELEMENTS(expected); i++) {
		g_autoptr(AxisJobClaim) c = claim(fx, "q");

		g_assert_nonnull(c);
		g_assert_cmpint(c->id, ==, expected[i]);
	}
	g_assert_null(claim(fx, "q"));
}

/* Only the given kinds, only the given queue. */
static void
test_claim_filters(Fixture *fx, gconstpointer data)
{
	static const gchar *const kinds_b[] = { "test.b", NULL };
	static const gchar *const no_kinds[] = { NULL };
	static const gchar *const queues[] = { "empty", "other", "q", NULL };
	g_autoptr(GError) error = NULL;
	AxisJobClaim *c = NULL;
	gint64 a;
	gint64 b;
	gint64 other;

	if (fx->db == NULL)
		return;

	a = enqueue_full(fx->conn, "test.a", "q", 0, 0, NULL, 1, NULL);
	b = enqueue_full(fx->conn, "test.b", "q", 9, 0, NULL, 1, NULL);
	other = enqueue_full(fx->conn, "test.a", "other", 0, 0, NULL, 1, NULL);

	g_assert_true(axis_job_claim(fx->conn, "q", no_kinds, WORKER, 1000, &c, &error));
	g_assert_null(c);

	g_assert_true(axis_job_claim(fx->conn, "q", kinds_b, WORKER, 1000, &c, &error));
	g_assert_nonnull(c);
	g_assert_cmpint(c->id, ==, b);
	g_clear_pointer(&c, axis_job_claim_free);

	g_assert_true(axis_job_claim(fx->conn, "q", kinds_b, WORKER, 1000, &c, &error));
	g_assert_null(c);

	/* Queues tried in order. */
	g_assert_true(axis_job_claim_any(fx->conn, queues, kinds_a, WORKER, 1000, &c, &error));
	g_assert_nonnull(c);
	g_assert_cmpint(c->id, ==, other);
	g_clear_pointer(&c, axis_job_claim_free);
	g_assert_true(axis_job_claim_any(fx->conn, queues, kinds_a, WORKER, 1000, &c, &error));
	g_assert_nonnull(c);
	g_assert_cmpint(c->id, ==, a);
	g_clear_pointer(&c, axis_job_claim_free);
	g_assert_no_error(error);
}

/* Spec: Future jobs wait */
static void
test_future_run_time(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobClaim) c = NULL;
	gint64 deadline;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue_full(fx->conn, "test.a", "q", 0, 1000, NULL, 1, NULL);
	assert_column(fx, id, "run_at > now() + interval '300 milliseconds'", "true");
	g_assert_null(claim(fx, "q"));

	deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	while ((c = claim(fx, "q")) == NULL) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_usleep(20 * 1000);
	}
	g_assert_cmpint(c->id, ==, id);
	/* By the database's clock, not before its time. */
	assert_column(fx, id, "started_at >= run_at", "true");
}

/* Spec: Worker disappears (a real lease running out) */
static void
test_lease_expiry(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobClaim) first = NULL;
	g_autoptr(AxisJobClaim) second = NULL;
	gint64 deadline;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	id = enqueue(fx, "q", 3);
	first = claim_from(fx->conn, "q", 1000);
	g_assert_nonnull(first);

	/* Not before it has expired. */
	g_assert_cmpuint(sweep(fx), ==, 0);
	assert_column(fx, id, "state", "running");

	deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	while (sweep(fx) == 0) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_usleep(20 * 1000);
	}
	assert_column(fx, id, "state", "queued");
	assert_column(fx, id, "attempts", "1");
	assert_column(fx, id, "lease_token IS NULL AND run_at <= now()", "true");
	assert_column(fx, id, "last_error LIKE '%" WORKER "%expired%'", "true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*lease of worker " WORKER
		" expired*returned to the queue*"), ==, 1);

	second = claim(fx, "q");
	g_assert_nonnull(second);
	g_assert_cmpint(second->id, ==, id);
	g_assert_cmpuint(second->attempt, ==, 2);
}

/* Spec: Stale worker cannot complete */
static void
test_stale_worker_rejected(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) a = NULL;
	g_autoptr(AxisJobClaim) b = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisJobProgress progress = { 0.5, "stale" };
	gboolean cancel = FALSE;
	AxisJobState state;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	id = enqueue(fx, "q", 3);
	a = claim(fx, "q");
	expire_lease(fx, id);
	g_assert_cmpuint(sweep(fx), ==, 1);
	b = claim(fx, "q");
	g_assert_nonnull(b);
	g_assert_cmpint(b->id, ==, id);
	g_assert_cmpstr(a->lease_token, !=, b->lease_token);

	before = row(fx, id);

	/* Everything the first holder tries is refused and changes nothing. */
	g_assert_false(axis_job_succeed(fx->conn, id, a->lease_token, &error));
	assert_lease_lost(error);
	g_clear_error(&error);
	g_assert_false(axis_job_fail(fx->conn, id, a->lease_token, "late", FALSE, 0, 0, &state, &error));
	assert_lease_lost(error);
	g_clear_error(&error);
	g_assert_false(axis_job_fail(fx->conn, id, a->lease_token, "late", TRUE, 0, 0, &state, &error));
	assert_lease_lost(error);
	g_clear_error(&error);
	g_assert_false(axis_job_heartbeat(fx->conn, id, a->lease_token, LONG_LEASE_MS, &progress,
		&cancel, &error));
	assert_lease_lost(error);
	g_clear_error(&error);
	g_assert_false(axis_job_hand_back(fx->conn, id, a->lease_token, &state, &error));
	assert_lease_lost(error);
	g_clear_error(&error);

	/* Nor does a made-up token, or the right token for another job. */
	g_assert_false(axis_job_succeed(fx->conn, id, "not-a-token", &error));
	assert_lease_lost(error);
	g_clear_error(&error);
	g_assert_false(axis_job_succeed(fx->conn, id, NULL, &error));
	assert_lease_lost(error);
	g_clear_error(&error);
	g_assert_false(axis_job_succeed(fx->conn, id + 1000, b->lease_token, &error));
	assert_lease_lost(error);
	g_clear_error(&error);

	after = row(fx, id);
	g_assert_cmpstr(before, ==, after);
	assert_column(fx, id, "state", "running");
	assert_column(fx, id, "lease_token::text", b->lease_token);

	/* The current holder is unaffected. */
	g_assert_true(axis_job_succeed(fx->conn, id, b->lease_token, &error));
	g_assert_no_error(error);
	assert_column(fx, id, "state || '/' || attempts", "succeeded/2");
}

/* Spec: Heartbeat extends the lease */
static void
test_heartbeat_extends(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	g_autofree gchar *first_expiry = NULL;
	g_autofree gchar *later = NULL;
	gint64 until;
	gint64 deadline;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	id = enqueue(fx, "q", 3);
	c = claim_from(fx->conn, "q", 600);
	first_expiry = axis_test_db_queryf(fx->db, "SELECT lease_expires_at FROM job WHERE id = %"
		G_GINT64_FORMAT, id);

	/* Two lease lengths of heartbeats every sixth of one, sweeping in
	 * between: never taken back. */
	until = g_get_monotonic_time() + 1200 * 1000;
	while (g_get_monotonic_time() < until) {
		g_assert_true(axis_job_heartbeat(fx->conn, id, c->lease_token, 600, NULL, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(sweep(fx), ==, 0);
		g_usleep(100 * 1000);
	}
	assert_column(fx, id, "state || '/' || attempts", "running/1");
	later = axis_test_db_queryf(fx->db, "SELECT lease_expires_at > '%s' FROM job WHERE id = %"
		G_GINT64_FORMAT, first_expiry, id);
	g_assert_cmpstr(later, ==, "t");
	assert_column(fx, id, "heartbeat_at > started_at", "true");

	/* Once they stop, it is taken back. */
	deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	while (sweep(fx) == 0) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_usleep(20 * 1000);
	}
	assert_column(fx, id, "state", "queued");
}

/* An expired lease that nobody swept yet is still the holder's. */
static void
test_late_heartbeat_before_sweep(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 3);
	c = claim(fx, "q");
	expire_lease(fx, id);
	g_assert_true(axis_job_heartbeat(fx->conn, id, c->lease_token, LONG_LEASE_MS, NULL, NULL,
		&error));
	g_assert_no_error(error);
	g_assert_cmpuint(sweep(fx), ==, 0);
	assert_column(fx, id, "lease_expires_at > now()", "true");
}

/* The sweeper leaves the jobs of workers it is told are alive. */
static void
test_sweep_spares_live_workers(Fixture *fx, gconstpointer data)
{
	static const gchar *const live[] = { "other", WORKER, NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	guint n = 1;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	id = enqueue(fx, "q", 3);
	c = claim(fx, "q");
	expire_lease(fx, id);
	g_assert_true(axis_job_sweep_expired_leases(fx->conn, kinds_a, live, &n, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(n, ==, 0);
	assert_column(fx, id, "state", "running");
	g_assert_cmpuint(sweep(fx), ==, 1);
}

/* Jobs of kinds without a handler here are never swept. */
static void
test_sweep_leaves_unknown_kinds(Fixture *fx, gconstpointer data)
{
	static const gchar *const gone[] = { "gone.kind", NULL };
	g_autoptr(GError) error = NULL;
	AxisJobClaim *c = NULL;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue_full(fx->conn, "gone.kind", "q", 0, 0, NULL, 1, NULL);
	g_assert_true(axis_job_claim(fx->conn, "q", gone, WORKER, LONG_LEASE_MS, &c, &error));
	g_assert_nonnull(c);
	expire_lease(fx, id);
	g_assert_cmpuint(sweep(fx), ==, 0);
	assert_column(fx, id, "state", "running");
	axis_job_claim_free(c);
}

/* A lease running out on the last attempt fails the job. */
static void
test_sweep_last_attempt_fails(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobClaim) c = NULL;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	id = enqueue(fx, "q", 1);
	c = claim(fx, "q");
	expire_lease(fx, id);
	g_assert_cmpuint(sweep(fx), ==, 1);
	assert_column(fx, id, "state || '/' || attempts", "failed/1");
	assert_column(fx, id, "last_error LIKE '%expired%'", "true");
	assert_column(fx, id, "finished_at IS NOT NULL", "true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*no attempts are left*"), ==, 1);
	g_assert_null(claim(fx, "q"));
}

/* Spec: Failure is retried later */
static void
test_retry_later(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	AxisJobState state = AXIS_JOB_STATE_FAILED;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 5);
	c = claim(fx, "q");
	g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, "disk full", FALSE,
		60000, AXIS_JOB_DEFAULT_BACKOFF_CAP_MS, &state, &error));
	g_assert_no_error(error);
	g_assert_cmpint(state, ==, AXIS_JOB_STATE_QUEUED);

	assert_column(fx, id, "state || '/' || attempts", "queued/1");
	assert_column(fx, id, "last_error", "disk full");
	/* 60 s scaled by 0.5..1: half a minute to a minute from now. */
	assert_column(fx, id, "run_at > now() + interval '29 seconds' AND "
		"run_at <= now() + interval '60 seconds'", "true");
	assert_column(fx, id, "lease_token IS NULL AND finished_at IS NULL", "true");
	g_assert_null(claim(fx, "q"));
}

/* The delay doubles from the base up to the cap, scaled by 0.5..1. */
static void
test_backoff_grows(Fixture *fx, gconstpointer data)
{
	static const gint64 bounds[] = { 1000, 2000, 4000, 4000, 4000 };
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) delays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint attempt;
	guint i;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 6);
	for (attempt = 1; attempt <= 6; attempt++) {
		g_autoptr(AxisJobClaim) c = NULL;
		g_autofree gchar *delay = NULL;
		g_autofree gchar *out = NULL;
		AxisJobState state;
		gint64 ms;

		out = axis_test_db_queryf(fx->db, "UPDATE job SET run_at = now() WHERE id = %"
			G_GINT64_FORMAT " RETURNING id", id);
		c = claim(fx, "q");
		g_assert_nonnull(c);
		g_assert_cmpuint(c->attempt, ==, attempt);
		g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, "again", FALSE, 1000, 4000,
			&state, &error));
		g_assert_no_error(error);
		if (attempt == 6) {
			g_assert_cmpint(state, ==, AXIS_JOB_STATE_FAILED);
			break;
		}
		g_assert_cmpint(state, ==, AXIS_JOB_STATE_QUEUED);

		/* Measured a little after the statement, so a little short. */
		delay = axis_test_db_queryf(fx->db, "SELECT round(extract(epoch FROM run_at - now()) "
			"* 1000) FROM job WHERE id = %" G_GINT64_FORMAT, id);
		ms = g_ascii_strtoll(delay, NULL, 10);
		g_test_message("attempt %u: retry in %" G_GINT64_FORMAT " ms", attempt, ms);
		g_assert_cmpint(ms, >=, bounds[attempt - 1] / 2 - 300);
		g_assert_cmpint(ms, <=, bounds[attempt - 1]);
	}

	/* Jitter: jobs failing together do not all retry together. */
	for (i = 0; i < 20; i++)
		enqueue(fx, "jitter", 2);
	for (i = 0; i < 20; i++) {
		g_autoptr(AxisJobClaim) c = claim(fx, "jitter");
		AxisJobState state;

		g_assert_nonnull(c);
		g_assert_true(axis_job_fail(fx->conn, c->id, c->lease_token, "x", FALSE, 100000, 100000,
			&state, &error));
	}
	{
		g_autofree gchar *distinct = axis_test_db_query(fx->db,
			"SELECT count(DISTINCT run_at) > 10 FROM job WHERE queue = 'jitter'");

		g_assert_cmpstr(distinct, ==, "t");
	}
}

/* Spec: Attempts exhausted */
static void
test_attempts_exhausted(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	AxisJobState state;
	guint attempt;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 2);
	for (attempt = 1; attempt <= 2; attempt++) {
		g_autoptr(AxisJobClaim) c = claim(fx, "q");
		g_autofree gchar *message = g_strdup_printf("failure %u", attempt);

		g_assert_nonnull(c);
		g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, message, FALSE, 0, 0,
			&state, &error));
		g_assert_no_error(error);
		g_assert_cmpint(state, ==, attempt == 1 ? AXIS_JOB_STATE_QUEUED : AXIS_JOB_STATE_FAILED);
	}

	assert_column(fx, id, "state || '/' || attempts", "failed/2");
	assert_column(fx, id, "last_error", "failure 2");
	assert_column(fx, id, "finished_at IS NOT NULL AND lease_token IS NULL", "true");
	g_assert_null(claim(fx, "q"));
	g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_TOO_LATE);
	assert_column(fx, id, "state", "failed");
}

/* Spec: Permanent failure skips retries */
static void
test_permanent_failure(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	AxisJobState state;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 5);
	c = claim(fx, "q");
	g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, "bad payload", TRUE, 0, 0,
		&state, &error));
	g_assert_no_error(error);
	g_assert_cmpint(state, ==, AXIS_JOB_STATE_FAILED);
	assert_column(fx, id, "state || '/' || attempts", "failed/1");
	assert_column(fx, id, "last_error", "bad payload");
	g_assert_null(claim(fx, "q"));
}

/* Error texts are cut, and made valid UTF-8, rather than refused. */
static void
test_error_text_cleaned(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	g_autofree gchar *longer = g_strnfill(AXIS_JOB_MAX_ERROR_CHARS + 500, 'x');
	g_autofree gchar *length = NULL;
	AxisJobState state;
	gint64 id;

	if (fx->db == NULL)
		return;

	longer[10] = '\xff';
	id = enqueue(fx, "q", 5);
	c = claim(fx, "q");
	g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, longer, TRUE, 0, 0, &state, &error));
	g_assert_no_error(error);
	length = g_strdup_printf("%d", AXIS_JOB_MAX_ERROR_CHARS);
	assert_column(fx, id, "char_length(last_error)", length);
}

/* Spec: Duplicate enqueue returns the existing job */
static void
test_dedupe_duplicate(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobClaim) c = NULL;
	gboolean created = FALSE;
	gint64 first;
	gint64 again;
	g_autofree gchar *count = NULL;

	if (fx->db == NULL)
		return;

	first = enqueue_full(fx->conn, "test.a", "q", 0, 0, "scan:/media/a", 1, &created);
	g_assert_true(created);
	again = enqueue_full(fx->conn, "test.a", "q", 0, 0, "scan:/media/a", 1, &created);
	g_assert_false(created);
	g_assert_cmpint(again, ==, first);

	/* Running counts as unfinished too. */
	c = claim(fx, "q");
	g_assert_cmpint(c->id, ==, first);
	again = enqueue_full(fx->conn, "test.a", "q", 0, 0, "scan:/media/a", 1, &created);
	g_assert_false(created);
	g_assert_cmpint(again, ==, first);

	count = axis_test_db_query(fx->db, "SELECT count(*) || '/' || sum(duplicates) FROM job");
	g_assert_cmpstr(count, ==, "1/2");
	assert_column(fx, first, "state", "running");

	/* A job without a key is never a duplicate. */
	g_assert_cmpint(enqueue(fx, "q", 1), !=, enqueue(fx, "q", 1));
}

/* Spec: Key is free after completion */
static void
test_dedupe_free_after_finish(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	AxisJobState state;
	gboolean created = FALSE;
	gint64 ids[4];
	guint i;

	if (fx->db == NULL)
		return;

	/* Succeeded, failed, cancelled while queued, cancelled while running. */
	for (i = 0; i < G_N_ELEMENTS(ids); i++) {
		ids[i] = enqueue_full(fx->conn, "test.a", "q", 0, 0, "k", 1, &created);
		g_assert_true(created);
		if (i > 0)
			g_assert_cmpint(ids[i], !=, ids[i - 1]);

		if (i == 2) {
			g_assert_true(axis_job_cancel(fx->conn, ids[i], &outcome, &error));
			g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_DONE);
		} else {
			g_autoptr(AxisJobClaim) c = claim(fx, "q");

			g_assert_cmpint(c->id, ==, ids[i]);
			if (i == 0) {
				g_assert_true(axis_job_succeed(fx->conn, c->id, c->lease_token, &error));
			} else if (i == 1) {
				g_assert_true(axis_job_fail(fx->conn, c->id, c->lease_token, "x", TRUE, 0, 0,
					&state, &error));
			} else {
				g_assert_true(axis_job_cancel(fx->conn, c->id, &outcome, &error));
				g_assert_true(axis_job_fail(fx->conn, c->id, c->lease_token, "x", FALSE, 0, 0,
					&state, &error));
				g_assert_cmpint(state, ==, AXIS_JOB_STATE_CANCELLED);
			}
		}
		g_assert_no_error(error);
	}

	g_assert_cmpint(enqueue_full(fx->conn, "test.a", "q", 0, 0, "k", 1, &created), >, ids[3]);
	g_assert_true(created);
}

/* Spec: Concurrent duplicate enqueues */
#define ENQUEUERS 16

typedef struct
{
	AxisDbPool  *pool;
	const gchar *key;
	GMutex       lock;
	GCond        cond;
	guint        ready;
	gboolean     go;
	gint64       ids[ENQUEUERS];
	gboolean     created[ENQUEUERS];
	gint         next;
	guint        errors;
} Duplicates;

static gpointer
enqueuer_thread(gpointer data)
{
	Duplicates *d = data;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn = axis_db_pool_acquire(d->pool, &error);
	AxisJobSpec spec;
	gint slot;

	g_assert_no_error(error);
	slot = g_atomic_int_add(&d->next, 1);

	g_mutex_lock(&d->lock);
	d->ready++;
	g_cond_broadcast(&d->cond);
	while (!d->go)
		g_cond_wait(&d->cond, &d->lock);
	g_mutex_unlock(&d->lock);

	memset(&spec, 0, sizeof(spec));
	spec.kind = "test.a";
	spec.queue = "q";
	spec.dedupe_key = d->key;
	spec.max_attempts = 1;
	if (!axis_job_enqueue(conn, &spec, &d->ids[slot], &d->created[slot], &error)) {
		g_test_message("enqueue failed: %s", error->message);
		g_atomic_int_inc((gint *)&d->errors);
	}

	axis_db_pool_release(d->pool, conn);
	return NULL;
}

static void
test_dedupe_concurrent(Fixture *fx, gconstpointer data)
{
	guint round;

	if (fx->db == NULL)
		return;

	for (round = 0; round < 25; round++) {
		g_autofree gchar *key = g_strdup_printf("race:%u", round);
		g_autofree gchar *count = NULL;
		g_autofree gchar *sql = NULL;
		GThread *threads[ENQUEUERS];
		Duplicates d;
		guint n_created = 0;
		guint i;

		memset(&d, 0, sizeof(d));
		d.pool = fx->pool;
		d.key = key;
		g_mutex_init(&d.lock);
		g_cond_init(&d.cond);
		for (i = 0; i < ENQUEUERS; i++)
			threads[i] = g_thread_new("enqueuer", enqueuer_thread, &d);
		g_mutex_lock(&d.lock);
		while (d.ready < ENQUEUERS)
			g_cond_wait(&d.cond, &d.lock);
		d.go = TRUE;
		g_cond_broadcast(&d.cond);
		g_mutex_unlock(&d.lock);
		for (i = 0; i < ENQUEUERS; i++)
			g_thread_join(threads[i]);

		g_assert_cmpuint(d.errors, ==, 0);
		for (i = 0; i < ENQUEUERS; i++) {
			g_assert_cmpint(d.ids[i], ==, d.ids[0]);
			if (d.created[i])
				n_created++;
		}
		g_assert_cmpuint(n_created, ==, 1);

		sql = g_strdup_printf("SELECT count(*) || '/' || max(duplicates) FROM job WHERE dedupe_key = '%s'",
			key);
		count = axis_test_db_query(fx->db, sql);
		g_assert_cmpstr(count, ==, "1/15");
		g_mutex_clear(&d.lock);
		g_cond_clear(&d.cond);
	}
}

/* The conflict target infers the partial unique index. */
static void
test_dedupe_arbiter_index(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GString) plan = g_string_new(NULL);
	g_autofree gchar *index = NULL;
	guint i;

	if (fx->db == NULL)
		return;

	index = axis_test_db_query(fx->db,
		"SELECT indisunique AND indpred IS NOT NULL FROM pg_index "
		"WHERE indexrelid = 'job_dedupe_key_idx'::regclass");
	g_assert_cmpstr(index, ==, "t");

	g_assert_true(axis_db_conn_exec(fx->conn,
		"EXPLAIN INSERT INTO job (kind, queue, max_attempts, dedupe_key) VALUES ('test.a', 'q', 1, 'k')"
		" ON CONFLICT (dedupe_key) WHERE dedupe_key IS NOT NULL AND state IN ('queued', 'running')"
		" DO UPDATE SET duplicates = job.duplicates + 1", 0, NULL, &result, &error));
	g_assert_no_error(error);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_string_append_printf(plan, "%s\n", axis_db_result_get_text(result, i, 0));
	g_test_message("%s", plan->str);
	g_assert_nonnull(strstr(plan->str, "Conflict Arbiter Indexes: job_dedupe_key_idx"));
}

/* Spec: Cancel queued job */
static void
test_cancel_queued(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 1);
	g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_DONE);
	assert_column(fx, id, "state", "cancelled");
	assert_column(fx, id, "finished_at IS NOT NULL AND attempts = 0", "true");
	g_assert_null(claim(fx, "q"));

	/* Not even after its run time. */
	assert_column(fx, id, "run_at <= now()", "true");
	g_assert_null(claim(fx, "q"));
}

/* Spec: Cancel running job (the lifecycle part: the request reaches the
 * holder through the heartbeat, and its failure ends the job cancelled) */
static void
test_cancel_running(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	AxisJobCancelOutcome outcome;
	AxisJobState state;
	gboolean requested = FALSE;
	gint64 id;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 5);
	c = claim(fx, "q");
	g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_REQUESTED);
	assert_column(fx, id, "state || '/' || cancel_requested", "running/true");

	/* Asking twice is harmless. */
	g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_REQUESTED);

	g_assert_true(axis_job_heartbeat(fx->conn, id, c->lease_token, LONG_LEASE_MS, NULL,
		&requested, &error));
	g_assert_true(requested);

	g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, "stopped", FALSE, 0, 0, &state, &error));
	g_assert_no_error(error);
	g_assert_cmpint(state, ==, AXIS_JOB_STATE_CANCELLED);
	assert_column(fx, id, "state || '/' || attempts", "cancelled/1");
	assert_column(fx, id, "finished_at IS NOT NULL", "true");
	g_assert_null(claim(fx, "q"));
}

/* After a cancel request: success still counts; a hand-back or a lost
 * lease ends the job cancelled rather than queuing it again. */
static void
test_cancel_running_outcomes(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	AxisJobState state;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	{
		g_autoptr(AxisJobClaim) c = NULL;

		id = enqueue(fx, "q", 5);
		c = claim(fx, "q");
		g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
		g_assert_true(axis_job_succeed(fx->conn, id, c->lease_token, &error));
		assert_column(fx, id, "state", "succeeded");
	}
	{
		g_autoptr(AxisJobClaim) c = NULL;

		id = enqueue(fx, "q", 5);
		c = claim(fx, "q");
		g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
		g_assert_true(axis_job_hand_back(fx->conn, id, c->lease_token, &state, &error));
		g_assert_cmpint(state, ==, AXIS_JOB_STATE_CANCELLED);
		assert_column(fx, id, "state || '/' || attempts", "cancelled/1");
	}
	{
		g_autoptr(AxisJobClaim) c = NULL;

		id = enqueue(fx, "q", 5);
		c = claim(fx, "q");
		g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
		expire_lease(fx, id);
		g_assert_cmpuint(sweep(fx), ==, 1);
		assert_column(fx, id, "state", "cancelled");
	}
	g_assert_no_error(error);
	g_assert_null(claim(fx, "q"));
}

/* Spec: Cancel finished job */
static void
test_cancel_finished(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	AxisJobState state;
	gint64 ids[3];
	guint i;

	if (fx->db == NULL)
		return;

	for (i = 0; i < 3; i++) {
		g_autoptr(AxisJobClaim) c = NULL;

		ids[i] = enqueue(fx, "q", 1);
		if (i == 2) {
			g_assert_true(axis_job_cancel(fx->conn, ids[i], &outcome, &error));
			continue;
		}
		c = claim(fx, "q");
		if (i == 0)
			g_assert_true(axis_job_succeed(fx->conn, c->id, c->lease_token, &error));
		else
			g_assert_true(axis_job_fail(fx->conn, c->id, c->lease_token, "x", FALSE, 0, 0,
				&state, &error));
	}
	g_assert_no_error(error);

	for (i = 0; i < 3; i++) {
		g_autofree gchar *before = row(fx, ids[i]);
		g_autofree gchar *after = NULL;

		g_assert_true(axis_job_cancel(fx->conn, ids[i], &outcome, &error));
		g_assert_no_error(error);
		g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_TOO_LATE);
		after = row(fx, ids[i]);
		g_assert_cmpstr(before, ==, after);
	}

	g_assert_false(axis_job_cancel(fx->conn, ids[2] + 1000, &outcome, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND);
}

/* A hand-back does not use up an attempt and keeps the job's place. */
static void
test_hand_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	g_autoptr(AxisJobClaim) again = NULL;
	AxisJobState state;
	gint64 id;
	gint64 later;

	if (fx->db == NULL)
		return;

	id = enqueue(fx, "q", 1);
	c = claim(fx, "q");
	later = enqueue(fx, "q", 1);
	g_assert_true(axis_job_hand_back(fx->conn, id, c->lease_token, &state, &error));
	g_assert_no_error(error);
	g_assert_cmpint(state, ==, AXIS_JOB_STATE_QUEUED);
	assert_column(fx, id, "state || '/' || attempts", "queued/0");
	assert_column(fx, id, "lease_token IS NULL AND finished_at IS NULL", "true");

	again = claim(fx, "q");
	g_assert_cmpint(again->id, ==, id);
	g_assert_cmpuint(again->attempt, ==, 1);
	g_assert_cmpint(later, >, id);
}

/* Spec: Old finished jobs are pruned */
static void
test_prune(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *left = NULL;
	g_autofree gchar *expected = NULL;
	gint64 ids[8];
	guint64 deleted = 0;
	guint i;

	if (fx->db == NULL)
		return;

	for (i = 0; i < G_N_ELEMENTS(ids); i++)
		ids[i] = enqueue(fx, "q", 1);

	/*
	 * 0 succeeded 8 days ago      gone      4 failed 31 days ago      gone
	 * 1 succeeded 6 days ago      kept      5 failed 8 days ago       kept
	 * 2 cancelled 8 days ago      gone      6 queued, created 90 days kept
	 * 3 cancelled 1 hour ago      kept      7 running, started 90 days kept
	 */
	out = axis_test_db_queryf(fx->db,
		"UPDATE job SET"
		" state = (CASE id WHEN %1$" G_GINT64_FORMAT " THEN 'succeeded' WHEN %2$" G_GINT64_FORMAT
		"  THEN 'succeeded' WHEN %3$" G_GINT64_FORMAT " THEN 'cancelled' WHEN %4$" G_GINT64_FORMAT
		"  THEN 'cancelled' WHEN %5$" G_GINT64_FORMAT " THEN 'failed' WHEN %6$" G_GINT64_FORMAT
		"  THEN 'failed' WHEN %8$" G_GINT64_FORMAT " THEN 'running' ELSE 'queued' END)::job_state,"
		" finished_at = CASE id WHEN %1$" G_GINT64_FORMAT " THEN now() - interval '8 days'"
		"  WHEN %2$" G_GINT64_FORMAT " THEN now() - interval '6 days'"
		"  WHEN %3$" G_GINT64_FORMAT " THEN now() - interval '8 days'"
		"  WHEN %4$" G_GINT64_FORMAT " THEN now() - interval '1 hour'"
		"  WHEN %5$" G_GINT64_FORMAT " THEN now() - interval '31 days'"
		"  WHEN %6$" G_GINT64_FORMAT " THEN now() - interval '8 days' END,"
		" lease_token = CASE id WHEN %8$" G_GINT64_FORMAT " THEN gen_random_uuid() END,"
		" lease_expires_at = CASE id WHEN %8$" G_GINT64_FORMAT " THEN now() + interval '1 hour' END,"
		" attempts = CASE id WHEN %8$" G_GINT64_FORMAT " THEN 1 ELSE 0 END,"
		" created_at = now() - interval '90 days', started_at = now() - interval '90 days'"
		" WHERE id = ANY (ARRAY[%1$" G_GINT64_FORMAT ", %2$" G_GINT64_FORMAT ", %3$" G_GINT64_FORMAT
		", %4$" G_GINT64_FORMAT ", %5$" G_GINT64_FORMAT ", %6$" G_GINT64_FORMAT ", %7$" G_GINT64_FORMAT
		", %8$" G_GINT64_FORMAT "]) RETURNING 1",
		ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], ids[6], ids[7]);

	g_assert_true(axis_job_prune(fx->conn, AXIS_JOB_DEFAULT_KEEP_FINISHED_MS,
		AXIS_JOB_DEFAULT_KEEP_FAILED_MS, &deleted, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(deleted, ==, 3);

	left = axis_test_db_query(fx->db, "SELECT string_agg(id::text, ',' ORDER BY id) FROM job");
	expected = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT ",%"
		G_GINT64_FORMAT ",%" G_GINT64_FORMAT, ids[1], ids[3], ids[5], ids[6], ids[7]);
	g_assert_cmpstr(left, ==, expected);

	/* A job retried by a person keeps its earlier runs no longer than a
	 * failed job is kept (an independent review of fix-jobs-hunt-findings):
	 * one that ended 31 days ago goes, one of 8 days stays, in order. */
	g_free(out);
	out = axis_test_db_queryf(fx->db, "UPDATE job SET earlier_runs = jsonb_build_array("
		"jsonb_build_object('state', 'failed', 'error', 'old', 'finished_at', now() - interval '31 days',"
		" 'retried_at', now() - interval '20 days'),"
		"jsonb_build_object('state', 'failed', 'error', 'recent', 'finished_at', now() - interval '8 days',"
		" 'retried_at', now() - interval '7 days'))"
		" WHERE id = %" G_GINT64_FORMAT " RETURNING 1", ids[6]);
	g_assert_true(axis_job_prune(fx->conn, AXIS_JOB_DEFAULT_KEEP_FINISHED_MS,
		AXIS_JOB_DEFAULT_KEEP_FAILED_MS, &deleted, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(deleted, ==, 0);
	g_free(left);
	left = axis_test_db_queryf(fx->db, "SELECT jsonb_array_length(earlier_runs) || ' ' || (earlier_runs->0->>'error') "
		"FROM job WHERE id = %" G_GINT64_FORMAT, ids[6]);
	g_assert_cmpstr(left, ==, "1 recent");
}

/*
 * The claim statement's plan with 100 000 claimable jobs (and as many
 * finished ones) in the table: read from auto_explain, so it is the plan
 * of the very statement axis_job_claim() runs.
 */
static void
test_claim_plan(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *out = NULL;
	gint64 start;
	gint64 took;

	if (fx->db == NULL)
		return;

	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, state, max_attempts, priority, run_at, finished_at)"
		" SELECT 'test.a', 'q' || (i % 4), 'succeeded', 1, 0, now(), now()"
		" FROM generate_series(1, 100000) AS i");
	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, max_attempts, priority, run_at)"
		" SELECT CASE WHEN i % 10 = 0 THEN 'test.other' ELSE 'test.a' END, 'big', 1, i % 5,"
		"  now() - (i || ' seconds')::interval"
		" FROM generate_series(1, 100000) AS i");
	axis_test_db_exec(fx->db, "ANALYZE job");

	g_assert_true(axis_db_conn_exec(fx->conn, "LOAD 'auto_explain'", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_exec(fx->conn,
		"SELECT set_config('auto_explain.log_min_duration', '0', false),"
		" set_config('auto_explain.log_analyze', 'on', false),"
		" set_config('auto_explain.log_buffers', 'on', false),"
		" set_config('auto_explain.log_level', 'notice', false)", 0, NULL, NULL, &error));
	g_assert_no_error(error);

	start = g_get_monotonic_time();
	c = claim(fx, "big");
	took = g_get_monotonic_time() - start;
	g_assert_nonnull(c);
	g_assert_true(axis_db_conn_exec(fx->conn,
		"SELECT set_config('auto_explain.log_min_duration', '-1', false)", 0, NULL, NULL, &error));

	plan = axis_test_log_find(G_LOG_LEVEL_DEBUG, "*FOR UPDATE SKIP LOCKED*");
	g_assert_nonnull(plan);
	g_test_message("claim with 200000 rows (100000 claimable) took %" G_GINT64_FORMAT
		" us round trip; plan: %s", took, plan);
	g_assert_nonnull(strstr(plan, "Index Scan using job_claim_idx on job"));
	g_assert_null(strstr(plan, "Seq Scan"));
	g_assert_null(strstr(plan, "Sort"));

	/* The highest priority, oldest run time. */
	out = axis_test_db_queryf(fx->db, "SELECT priority = 4 AND run_at <= ALL (SELECT run_at FROM job "
		"WHERE queue = 'big' AND priority = 4 AND kind = 'test.a') FROM job WHERE id = %"
		G_GINT64_FORMAT, c->id);
	g_assert_cmpstr(out, ==, "t");
}

/*
 * Schedules, one tick at a time
 */

static AxisJobRegistry *
schedule_registry(void)
{
	AxisJobRegistry *registry = axis_job_registry_new();
	AxisJobHandlerInfo info;
	g_autoptr(GError) error = NULL;

	memset(&info, 0, sizeof(info));
	info.kind = "test.a";
	info.queue = "q";
	info.max_attempts = 2;
	info.handler = noop_handler;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_no_error(error);

	return registry;
}

static guint
tick(
	Fixture         *fx,
	AxisJobRegistry *registry
){
	g_autoptr(GError) error = NULL;
	guint n = 99;

	g_assert_true(axis_job_schedule_tick(fx->conn, registry, &n, &error));
	g_assert_no_error(error);

	return n;
}

static void
make_due(
	Fixture     *fx,
	const gchar *name,
	const gchar *ago
){
	g_autofree gchar *out = axis_test_db_queryf(fx->db,
		"UPDATE job_schedule SET next_run_at = now() - interval '%s' WHERE name = '%s' RETURNING 1",
		ago, name);

	g_assert_nonnull(out);
}

/* Spec: No burst after downtime; Spec: No overlap (at the tick) */
static void
test_schedule_tick(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobRegistry) registry = schedule_registry();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(AxisJobClaim) c = NULL;
	g_autofree gchar *jobs = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *last = NULL;

	if (fx->db == NULL)
		return;

	json_object_set_boolean_member(payload, "deep", TRUE);
	g_assert_true(axis_job_schedule_upsert(fx->conn, "nightly", "test.a", payload, 3600, TRUE, &error));
	g_assert_no_error(error);

	/* Down for a hundred intervals: one job, and the next run an interval
	 * from now. */
	make_due(fx, "nightly", "100 hours");
	g_assert_cmpuint(tick(fx, registry), ==, 1);
	g_assert_cmpuint(tick(fx, registry), ==, 0);
	jobs = axis_test_db_query(fx->db, "SELECT count(*) || ' ' || min(dedupe_key) || ' ' || "
		"min(queue) || ' ' || min(max_attempts) || ' ' || bool_and((payload->>'deep')::boolean) FROM job");
	g_assert_cmpstr(jobs, ==, "1 schedule:nightly q 2 true");
	next = axis_test_db_query(fx->db, "SELECT next_run_at BETWEEN now() + interval '3590 seconds' "
		"AND now() + interval '3610 seconds' FROM job_schedule WHERE name = 'nightly'");
	g_assert_cmpstr(next, ==, "t");
	last = axis_test_db_query(fx->db, "SELECT last_job_id = (SELECT id FROM job) FROM job_schedule");
	g_assert_cmpstr(last, ==, "t");

	/* Due again while the first is queued, then while it runs: nothing. */
	make_due(fx, "nightly", "1 second");
	g_assert_cmpuint(tick(fx, registry), ==, 0);
	c = claim(fx, "q");
	g_assert_nonnull(c);
	make_due(fx, "nightly", "1 second");
	g_assert_cmpuint(tick(fx, registry), ==, 0);
	g_free(jobs);
	jobs = axis_test_db_query(fx->db, "SELECT count(*) FROM job");
	g_assert_cmpstr(jobs, ==, "1");
	/* Each of those ticks still moved the schedule on. */
	g_free(next);
	next = axis_test_db_query(fx->db, "SELECT next_run_at > now() FROM job_schedule");
	g_assert_cmpstr(next, ==, "t");

	/* Finished: the next one comes. */
	g_assert_true(axis_job_succeed(fx->conn, c->id, c->lease_token, &error));
	make_due(fx, "nightly", "1 second");
	g_assert_cmpuint(tick(fx, registry), ==, 1);
	g_free(jobs);
	jobs = axis_test_db_query(fx->db, "SELECT count(*) FROM job");
	g_assert_cmpstr(jobs, ==, "2");
}

/* Spec: Disabled schedule (at the tick); schedules of kinds without a
 * handler are left as they are */
static void
test_schedule_disabled(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobRegistry) registry = schedule_registry();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_job_schedule_upsert(fx->conn, "off", "test.a", NULL, 1, FALSE, &error));
	g_assert_true(axis_job_schedule_upsert(fx->conn, "orphan", "gone.kind", NULL, 1, TRUE, &error));
	g_assert_no_error(error);
	make_due(fx, "off", "1 hour");
	make_due(fx, "orphan", "1 hour");
	before = axis_test_db_query(fx->db, "SELECT next_run_at FROM job_schedule WHERE name = 'orphan'");

	g_assert_cmpuint(tick(fx, registry), ==, 0);
	after = axis_test_db_query(fx->db, "SELECT next_run_at FROM job_schedule WHERE name = 'orphan'");
	g_assert_cmpstr(before, ==, after);

	g_assert_true(axis_job_schedule_set_enabled(fx->conn, "off", TRUE, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(tick(fx, registry), ==, 1);

	g_assert_false(axis_job_schedule_set_enabled(fx->conn, "nope", TRUE, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND);
}

/* A default schedule is the build's: made once, then kept as the build
 * declares it, until a person changes it. */
static void
test_schedule_ensure(Fixture *fx, gconstpointer data)
{
	static const AxisJobScheduleInfo info = { "daily", "test.a", "{\"x\": 1}", 86400 };
	static const AxisJobScheduleInfo newer = { "daily", "test.a", "{\"x\": 2}", 3600 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stored = NULL;
	gboolean created = FALSE;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_job_schedule_ensure(fx->conn, &info, &created, &error));
	g_assert_no_error(error);
	g_assert_true(created);
	g_assert_true(axis_job_schedule_ensure(fx->conn, &info, &created, &error));
	g_assert_false(created);

	/* A newer build's declaration wins; its next run, an hour at the most
	 * from now, is no later than it was. */
	axis_test_db_exec(fx->db, "UPDATE job_schedule SET next_run_at = now() + interval '20 hours'");
	g_assert_true(axis_job_schedule_ensure(fx->conn, &newer, &created, &error));
	g_assert_false(created);
	stored = axis_test_db_query(fx->db, "SELECT kind || ' ' || (payload->>'x') || ' ' || run_interval || ' ' || "
		"enabled || ' ' || declared || ' ' || (next_run_at <= now() + interval '1 hour') FROM job_schedule");
	g_assert_cmpstr(stored, ==, "test.a 2 01:00:00 true true true");

	/* A person switched it off: it is the person's now, and the build
	 * leaves it as it is. */
	g_assert_true(axis_job_schedule_set_enabled(fx->conn, "daily", FALSE, &error));
	g_assert_true(axis_job_schedule_ensure(fx->conn, &info, &created, &error));
	g_assert_false(created);
	g_free(stored);
	stored = axis_test_db_query(fx->db, "SELECT kind || ' ' || (payload->>'x') || ' ' || run_interval || ' ' || "
		"enabled || ' ' || declared FROM job_schedule");
	g_assert_cmpstr(stored, ==, "test.a 2 01:00:00 false false");
}

/* Spec: A newer build drops a schedule (one statement at a time). */
static void
test_schedule_retire(Fixture *fx, gconstpointer data)
{
	static const AxisJobScheduleInfo kept = { "kept", "test.a", NULL, 60 };
	static const AxisJobScheduleInfo dropped = { "dropped", "test.a", NULL, 60 };
	static const gchar *const declared[] = { "kept", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stored = NULL;
	guint n = 99;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_job_schedule_ensure(fx->conn, &kept, NULL, &error));
	g_assert_true(axis_job_schedule_ensure(fx->conn, &dropped, NULL, &error));
	g_assert_true(axis_job_schedule_upsert(fx->conn, "persons", "test.a", NULL, 60, TRUE, &error));
	g_assert_no_error(error);

	g_assert_true(axis_job_schedule_retire(fx->conn, declared, &n, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(n, ==, 1);
	g_assert_true(axis_job_schedule_retire(fx->conn, declared, &n, &error));
	g_assert_cmpuint(n, ==, 0);
	stored = axis_test_db_query(fx->db, "SELECT string_agg(name || ' ' || enabled || ' ' || (retired_at IS NOT NULL), "
		"', ' ORDER BY name) FROM job_schedule");
	g_assert_cmpstr(stored, ==, "dropped false true, kept true false, persons true false");

	/* Declared again: enabled again, no longer marked. */
	g_assert_true(axis_job_schedule_ensure(fx->conn, &dropped, NULL, &error));
	g_free(stored);
	stored = axis_test_db_query(fx->db, "SELECT enabled || ' ' || (retired_at IS NULL) FROM job_schedule "
		"WHERE name = 'dropped'");
	g_assert_cmpstr(stored, ==, "true true");

	/* Switched off in the database, not by a start: it stays off. */
	axis_test_db_exec(fx->db, "UPDATE job_schedule SET enabled = false WHERE name = 'kept'");
	g_assert_true(axis_job_schedule_ensure(fx->conn, &kept, NULL, &error));
	g_free(stored);
	stored = axis_test_db_query(fx->db, "SELECT enabled::text FROM job_schedule WHERE name = 'kept'");
	g_assert_cmpstr(stored, ==, "false");
}

/* Spec: After the clock went back (at the tick). */
static void
test_schedule_pull_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobRegistry) registry = schedule_registry();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the database's clock went back*");
	g_assert_true(axis_job_schedule_upsert(fx->conn, "nightly", "test.a", NULL, 3600, TRUE, &error));
	g_assert_true(axis_job_schedule_upsert(fx->conn, "orphan", "gone.kind", NULL, 3600, TRUE, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->db, "UPDATE job_schedule SET next_run_at = now() + interval '2 days'");
	before = axis_test_db_query(fx->db, "SELECT next_run_at FROM job_schedule WHERE name = 'orphan'");

	/* Brought back to an interval from now, not fired, said once. */
	g_assert_cmpuint(tick(fx, registry), ==, 0);
	g_assert_cmpuint(tick(fx, registry), ==, 0);
	next = axis_test_db_query(fx->db, "SELECT next_run_at BETWEEN now() + interval '3590 seconds' "
		"AND now() + interval '3610 seconds' FROM job_schedule WHERE name = 'nightly'");
	g_assert_cmpstr(next, ==, "t");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING,
		"*schedule 'nightly' was next due in 17*s, more than its interval of 3600 s ahead*"), ==, 1);

	/* A kind without a handler here is left as it is. */
	after = axis_test_db_query(fx->db, "SELECT next_run_at FROM job_schedule WHERE name = 'orphan'");
	g_assert_cmpstr(before, ==, after);
}

/* Spec (job-ui): Retry; the rules of axis_job_retry() itself. */
static void
test_person_retry(Fixture *fx, gconstpointer data)
{
	static const gchar *const known[] = { "test.a", NULL };
	static const gchar *const queues[] = { "q", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *failed_text = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *done_before = NULL;
	g_autofree gchar *done_after = NULL;
	AxisJobRetryOutcome outcome = AXIS_JOB_RETRY_NOT_ENDED;
	gint64 failed;
	gint64 done;
	gint64 other;
	gint64 live = -1;
	gint64 holder;
	gint64 orphan;
	guint i;

	if (fx->db == NULL)
		return;

	/* Failed on a queue its kind is no longer claimed from, with a key, a
	 * priority and a cancel request left over. */
	failed_text = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"started_at, finished_at, last_error, worker_id, cancel_requested, progress, dedupe_key, priority, "
		"payload) VALUES ('test.a', 'old', 3, 'failed', 3, now(), now(), 'run 0', 'w1', true, 0.5, "
		"'test.a:1', 7, '{\"n\": 1}') RETURNING id");
	failed = g_ascii_strtoll(failed_text, NULL, 10);
	g_assert_true(axis_job_retry(fx->conn, failed, known, queues, "root", &outcome, &live, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_JOB_RETRY_DONE);
	g_assert_cmpint(live, ==, 0);
	stored = axis_test_db_queryf(fx->db, "SELECT state || ' ' || queue || ' ' || attempts || ' ' || max_attempts "
		"|| ' ' || dedupe_key || ' ' || priority || ' ' || (payload->>'n') || ' ' || cancel_requested || ' ' || "
		"coalesce(progress::text, '-') || ' ' || coalesce(last_error, '-') || ' ' || (finished_at IS NULL) || ' ' "
		"|| (run_at <= now()) || ' ' || (earlier_runs->0->>'error') || ' ' || (earlier_runs->0->>'attempts') "
		"FROM job WHERE id = %" G_GINT64_FORMAT, failed);
	g_assert_cmpstr(stored, ==, "queued q 0 3 test.a:1 7 1 false - - true true run 0 3");

	/* It is claimed as itself, and the key finds it. */
	{
		g_autoptr(AxisJobClaim) c = claim(fx, "q");
		gboolean created = TRUE;

		g_assert_nonnull(c);
		g_assert_cmpint(c->id, ==, failed);
		g_assert_false(c->cancel_requested);
		g_assert_cmpuint(c->attempt, ==, 1);
		other = enqueue_full(fx->conn, "test.a", "q", 0, 0, "test.a:1", 3, &created);
		g_assert_false(created);
		g_assert_cmpint(other, ==, failed);
		g_assert_true(axis_job_fail(fx->conn, c->id, c->lease_token, "run 1", TRUE, 10, 10, NULL, &error));
	}

	/* Another job of its key queued: that one does the work. */
	holder = enqueue_full(fx->conn, "test.a", "q", 0, 0, "test.a:1", 3, NULL);
	g_assert_true(axis_job_retry(fx->conn, failed, known, queues, "root", &outcome, &live, &error));
	g_assert_cmpint(outcome, ==, AXIS_JOB_RETRY_LIVE);
	g_assert_cmpint(live, ==, holder);
	g_free(stored);
	stored = axis_test_db_queryf(fx->db, "SELECT state || ' ' || jsonb_array_length(earlier_runs) FROM job "
		"WHERE id = %" G_GINT64_FORMAT, failed);
	g_assert_cmpstr(stored, ==, "failed 1");

	/* Not a failed or cancelled job: nothing changes. */
	done = enqueue(fx, "q2", 1);
	axis_test_db_exec(fx->db, "UPDATE job SET state = 'succeeded', finished_at = now() WHERE queue = 'q2'");
	done_before = row(fx, done);
	g_assert_true(axis_job_retry(fx->conn, done, known, queues, NULL, &outcome, &live, &error));
	g_assert_cmpint(outcome, ==, AXIS_JOB_RETRY_NOT_ENDED);
	done_after = row(fx, done);
	g_assert_cmpstr(done_before, ==, done_after);

	/* No such job; a kind nothing here runs. */
	g_assert_false(axis_job_retry(fx->conn, 987654, known, queues, NULL, &outcome, &live, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND);
	g_clear_error(&error);
	orphan = enqueue_full(fx->conn, "gone.kind", "q", 0, 0, NULL, 1, NULL);
	axis_test_db_exec(fx->db, "UPDATE job SET state = 'failed', finished_at = now() WHERE kind = 'gone.kind'");
	g_assert_false(axis_job_retry(fx->conn, orphan, known, queues, NULL, &outcome, &live, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND);
	g_clear_error(&error);

	/* The latest earlier runs are kept, the oldest dropped. */
	axis_test_db_exec(fx->db, "DELETE FROM job WHERE dedupe_key = 'test.a:1' AND state = 'queued'");
	for (i = 1; i <= AXIS_JOB_EARLIER_RUNS_KEPT + 1; i++) {
		g_autofree gchar *sql = g_strdup_printf("UPDATE job SET last_error = 'run %u' WHERE id = %"
			G_GINT64_FORMAT, i, failed);

		axis_test_db_exec(fx->db, sql);
		g_assert_true(axis_job_retry(fx->conn, failed, known, queues, NULL, &outcome, &live, &error));
		g_assert_no_error(error);
		g_assert_cmpint(outcome, ==, AXIS_JOB_RETRY_DONE);
		axis_test_db_exec(fx->db, "UPDATE job SET state = 'failed', finished_at = now() WHERE dedupe_key = "
			"'test.a:1'");
	}
	g_free(stored);
	stored = axis_test_db_queryf(fx->db, "SELECT jsonb_array_length(earlier_runs) || ' ' || "
		"(earlier_runs->0->>'error') || ' ' || (earlier_runs->-1->>'error') FROM job WHERE id = %" G_GINT64_FORMAT,
		failed);
	g_assert_cmpstr(stored, ==, "20 run 2 run 21");
}

typedef struct
{
	AxisDbPool          *pool;
	gint64               id;
	gint                 done;	/* atomic */
	gboolean             ok;
	AxisJobRetryOutcome  outcome;
	gint64               live;
	GError              *error;
} Retrier;

static gpointer
retrier_thread(gpointer data)
{
	static const gchar *const known[] = { "test.a", NULL };
	static const gchar *const queues[] = { "q", NULL };
	Retrier *r = data;
	AxisDbConn *conn = axis_db_pool_acquire(r->pool, &r->error);

	if (conn != NULL) {
		r->ok = axis_job_retry(conn, r->id, known, queues, NULL, &r->outcome, &r->live, &r->error);
		axis_db_pool_release(r->pool, conn);
	}
	g_atomic_int_set(&r->done, 1);
	return NULL;
}

/*
 * Spec (job-ui): Retry while the work is being done, when the other job
 * commits after the retry looked: the key's index stops the retry, which
 * says which job holds the key.
 */
static void
test_person_retry_key_taken_meanwhile(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *failed_text = NULL;
	g_autofree gchar *holder_text = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *out = NULL;
	GThread *thread;
	Retrier r;

	if (fx->db == NULL)
		return;

	failed_text = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"finished_at, dedupe_key) VALUES ('test.a', 'q', 1, 'failed', 1, now(), 'test.a:race') RETURNING id");
	out = axis_test_db_query(fx->db, "BEGIN");
	holder_text = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, dedupe_key) "
		"VALUES ('test.a', 'q', 1, 'test.a:race') RETURNING id");

	memset(&r, 0, sizeof(r));
	r.pool = fx->pool;
	r.id = g_ascii_strtoll(failed_text, NULL, 10);
	thread = g_thread_new("retrier", retrier_thread, &r);
	/* It waits on the key's index for the other transaction. */
	g_usleep(300 * 1000);
	g_assert_cmpint(g_atomic_int_get(&r.done), ==, 0);
	g_free(out);
	out = axis_test_db_query(fx->db, "COMMIT");
	g_thread_join(thread);

	g_assert_no_error(r.error);
	g_assert_true(r.ok);
	g_assert_cmpint(r.outcome, ==, AXIS_JOB_RETRY_LIVE);
	g_assert_cmpint(r.live, ==, g_ascii_strtoll(holder_text, NULL, 10));
	state = axis_test_db_queryf(fx->db, "SELECT state || ' ' || jsonb_array_length(earlier_runs) FROM job "
		"WHERE id = %s", failed_text);
	g_assert_cmpstr(state, ==, "failed 0");
}

/* An earlier run keeps at most AXIS_JOB_EARLIER_ERROR_CHARS of its error. */
static void
test_person_retry_error_capped(Fixture *fx, gconstpointer data)
{
	static const gchar *const known[] = { "test.a", NULL };
	static const gchar *const queues[] = { "q", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *failed_text = NULL;
	g_autofree gchar *kept = NULL;
	AxisJobRetryOutcome outcome = AXIS_JOB_RETRY_NOT_ENDED;

	if (fx->db == NULL)
		return;

	failed_text = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"finished_at, last_error) VALUES ('test.a', 'q', 1, 'failed', 1, now(), repeat('x', 1999) || 'y') "
		"RETURNING id");
	g_assert_true(axis_job_retry(fx->conn, g_ascii_strtoll(failed_text, NULL, 10), known, queues, NULL, &outcome,
		NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_JOB_RETRY_DONE);
	kept = axis_test_db_queryf(fx->db, "SELECT length(earlier_runs->0->>'error') FROM job WHERE id = %s",
		failed_text);
	g_assert_cmpstr(kept, ==, G_STRINGIFY(AXIS_JOB_EARLIER_ERROR_CHARS));
}

/*
 * A job deleted (retention) between the retry's snapshot and its update is
 * not there: the answer is that there is no such job, not that it is not
 * failed.  Found by an independent review of these fixes.
 */
static void
test_person_retry_pruned_meanwhile(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *failed_text = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *sql = NULL;
	GThread *thread;
	Retrier r;

	if (fx->db == NULL)
		return;

	failed_text = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"finished_at) VALUES ('test.a', 'q', 1, 'failed', 1, now()) RETURNING id");
	out = axis_test_db_query(fx->db, "BEGIN");
	sql = g_strdup_printf("DELETE FROM job WHERE id = %s RETURNING id", failed_text);
	g_free(out);
	out = axis_test_db_query(fx->db, sql);

	memset(&r, 0, sizeof(r));
	r.pool = fx->pool;
	r.id = g_ascii_strtoll(failed_text, NULL, 10);
	thread = g_thread_new("retrier", retrier_thread, &r);
	/* It waits on the row the deletion holds. */
	g_usleep(300 * 1000);
	g_assert_cmpint(g_atomic_int_get(&r.done), ==, 0);
	g_free(out);
	out = axis_test_db_query(fx->db, "COMMIT");
	g_thread_join(thread);

	g_assert_false(r.ok);
	g_assert_error(r.error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND);
	g_clear_error(&r.error);
}

/* Spec: Jobs an earlier build queued on another queue (one statement). */
static void
test_move_to_kind_queues(Fixture *fx, gconstpointer data)
{
	static const gchar *const known[] = { "test.a", NULL };
	static const gchar *const queues[] = { "q", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	guint n = 99;

	if (fx->db == NULL)
		return;

	axis_test_db_exec(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, lease_token, "
		"lease_expires_at, started_at, finished_at) VALUES "
		"('test.a', 'old', 3, 'queued', 0, NULL, NULL, NULL, NULL),"
		"('test.a', 'old', 3, 'running', 1, gen_random_uuid(), now() + interval '1 minute', now(), NULL),"
		"('test.a', 'old', 3, 'succeeded', 1, NULL, NULL, now(), now()),"
		"('test.a', 'q', 3, 'queued', 0, NULL, NULL, NULL, NULL),"
		"('gone.kind', 'old', 3, 'queued', 0, NULL, NULL, NULL, NULL)");
	before = axis_test_db_query(fx->db, "SELECT string_agg(state || ' ' || coalesce(lease_token::text, '-'), ', ' "
		"ORDER BY id) FROM job");

	g_assert_true(axis_job_move_to_kind_queues(fx->conn, known, queues, &n, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(n, ==, 2);
	after = axis_test_db_query(fx->db, "SELECT string_agg(state || ' ' || coalesce(lease_token::text, '-'), ', ' "
		"ORDER BY id) FROM job");
	g_assert_cmpstr(before, ==, after);
	g_free(after);
	after = axis_test_db_query(fx->db, "SELECT string_agg(queue, ' ' ORDER BY id) FROM job");
	g_assert_cmpstr(after, ==, "q q old q old");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE,
		"*moved 1 queued and 1 running job(s) of kind 'test.a' from queue 'old' to 'q'*"), ==, 1);

	g_assert_true(axis_job_move_to_kind_queues(fx->conn, known, queues, &n, &error));
	g_assert_cmpuint(n, ==, 0);
}

/*
 * Notifications
 */

static gchar *
next_notification(AxisDbListener *listener)
{
	g_autoptr(GError) error = NULL;
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	gchar *payload;

	while ((payload = axis_db_listener_pop(listener)) == NULL) {
		AxisDbListenerWait wait = axis_db_listener_wait(listener, -1, deadline, &error);

		g_assert_no_error(error);
		g_assert_cmpint(wait, ==, AXIS_DB_LISTENER_READY);
	}

	return payload;
}

static void
assert_event(
	AxisDbListener *listener,
	const gchar    *type,
	gint64          id,
	const gchar    *state
){
	g_autofree gchar *payload = next_notification(listener);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *object;

	g_assert_true(json_parser_load_from_data(parser, payload, -1, &error));
	object = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpstr(json_object_get_string_member(object, "type"), ==, type);
	g_assert_cmpint(json_object_get_int_member(object, "id"), ==, id);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, state);
	g_assert_cmpstr(json_object_get_string_member(object, "kind"), ==, "test.a");
	g_assert_cmpstr(json_object_get_string_member(object, "queue"), ==, "q");
}

/* Every change is announced, in order, and only once it commits. */
static void
test_notifications(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbListener) listener = NULL;
	g_autoptr(AxisJobClaim) c = NULL;
	g_autofree gchar *payload = NULL;
	AxisJobProgress progress = { 0.25, "a quarter" };
	AxisJobCancelOutcome outcome;
	AxisJobState state;
	gint64 id;
	gint64 other;

	if (fx->db == NULL)
		return;

	listener = axis_db_listener_new(fx->pool, AXIS_JOB_CHANNEL, -1, &error);
	g_assert_no_error(error);

	id = enqueue(fx, "q", 2);
	assert_event(listener, "state", id, "queued");
	c = claim(fx, "q");
	assert_event(listener, "state", id, "running");
	g_assert_true(axis_job_heartbeat(fx->conn, id, c->lease_token, LONG_LEASE_MS, &progress, NULL,
		&error));
	assert_event(listener, "progress", id, "running");
	g_assert_true(axis_job_heartbeat(fx->conn, id, c->lease_token, LONG_LEASE_MS, NULL, NULL, &error));
	g_assert_true(axis_job_fail(fx->conn, id, c->lease_token, "boom", FALSE, 0, 0, &state, &error));
	assert_event(listener, "state", id, "queued");
	axis_job_claim_free(g_steal_pointer(&c));
	c = claim(fx, "q");
	assert_event(listener, "state", id, "running");
	g_assert_true(axis_job_cancel(fx->conn, id, &outcome, &error));
	assert_event(listener, "cancel-requested", id, "running");
	g_assert_true(axis_job_succeed(fx->conn, id, c->lease_token, &error));
	assert_event(listener, "state", id, "succeeded");
	g_assert_no_error(error);

	/* A duplicate changes nothing and says nothing; a rolled-back enqueue
	 * never happened. */
	other = enqueue_full(fx->conn, "test.a", "q", 0, 0, "k", 1, NULL);
	assert_event(listener, "state", other, "queued");
	enqueue_full(fx->conn, "test.a", "q", 0, 0, "k", 1, NULL);
	g_assert_true(axis_db_conn_begin(fx->conn, &error));
	enqueue(fx, "q", 1);
	g_assert_true(axis_db_conn_rollback(fx->conn, &error));
	g_assert_true(axis_db_listener_notify(listener, "barrier", -1, &error));
	g_assert_no_error(error);
	payload = next_notification(listener);
	g_assert_cmpstr(payload, ==, "barrier");
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	/* A backstop against a hang anywhere. */
	alarm(240);

	ADD("/job/lifecycle/basic", test_basic_lifecycle);
	ADD("/job/lifecycle/unknown-kind-refused", test_unknown_kind_refused);
	ADD("/job/lifecycle/invalid-arguments", test_invalid_arguments);
	ADD("/job/lifecycle/claim/concurrent-claimers", test_concurrent_claimers);
	ADD("/job/lifecycle/claim/order", test_claim_order);
	ADD("/job/lifecycle/claim/filters", test_claim_filters);
	ADD("/job/lifecycle/claim/future-run-time", test_future_run_time);
	ADD("/job/lifecycle/claim/plan", test_claim_plan);
	ADD("/job/lifecycle/lease/expiry", test_lease_expiry);
	ADD("/job/lifecycle/lease/stale-worker-rejected", test_stale_worker_rejected);
	ADD("/job/lifecycle/lease/heartbeat-extends", test_heartbeat_extends);
	ADD("/job/lifecycle/lease/late-heartbeat-before-sweep", test_late_heartbeat_before_sweep);
	ADD("/job/lifecycle/sweep/spares-live-workers", test_sweep_spares_live_workers);
	ADD("/job/lifecycle/sweep/leaves-unknown-kinds", test_sweep_leaves_unknown_kinds);
	ADD("/job/lifecycle/sweep/last-attempt-fails", test_sweep_last_attempt_fails);
	ADD("/job/lifecycle/retry/later", test_retry_later);
	ADD("/job/lifecycle/retry/backoff-grows", test_backoff_grows);
	ADD("/job/lifecycle/retry/attempts-exhausted", test_attempts_exhausted);
	ADD("/job/lifecycle/retry/permanent-failure", test_permanent_failure);
	ADD("/job/lifecycle/retry/error-text-cleaned", test_error_text_cleaned);
	ADD("/job/lifecycle/dedupe/duplicate", test_dedupe_duplicate);
	ADD("/job/lifecycle/dedupe/free-after-finish", test_dedupe_free_after_finish);
	ADD("/job/lifecycle/dedupe/concurrent", test_dedupe_concurrent);
	ADD("/job/lifecycle/dedupe/arbiter-index", test_dedupe_arbiter_index);
	ADD("/job/lifecycle/cancel/queued", test_cancel_queued);
	ADD("/job/lifecycle/cancel/running", test_cancel_running);
	ADD("/job/lifecycle/cancel/running-outcomes", test_cancel_running_outcomes);
	ADD("/job/lifecycle/cancel/finished", test_cancel_finished);
	ADD("/job/lifecycle/hand-back", test_hand_back);
	ADD("/job/lifecycle/prune", test_prune);
	ADD("/job/schedule/tick", test_schedule_tick);
	ADD("/job/schedule/disabled", test_schedule_disabled);
	ADD("/job/schedule/ensure", test_schedule_ensure);
	ADD("/job/schedule/retire", test_schedule_retire);
	ADD("/job/schedule/pull-back", test_schedule_pull_back);
	ADD("/job/lifecycle/person-retry", test_person_retry);
	ADD("/job/lifecycle/person-retry/key-taken-meanwhile", test_person_retry_key_taken_meanwhile);
	ADD("/job/lifecycle/person-retry/pruned-meanwhile", test_person_retry_pruned_meanwhile);
	ADD("/job/lifecycle/person-retry/error-capped", test_person_retry_error_capped);
	ADD("/job/lifecycle/move-to-kind-queues", test_move_to_kind_queues);
	ADD("/job/lifecycle/notifications", test_notifications);

	return g_test_run();
}
