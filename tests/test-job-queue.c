/*
 * test-job-queue.c - the in-process runner against a real PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Durability across a restart, concurrency limits and queue independence,
 * externally served queues, progress and events (delivered on the main
 * loop, in order), cooperative cancellation, schedules (after the clock went
 * back, as the build declares them, a person's), shutdown within and beyond
 * the grace period, lost leases, jobs without a handler, jobs an earlier
 * build left on another queue, retention, and the start-up checks.  Timings are shortened (poll 100 ms,
 * lease 1.5 s, sweep and schedule tick 100 ms) so that nothing waits for
 * real-world durations; every wait is for a condition, bounded.
 */

#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "core/axis-module.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "jobs/axis-job-schedule.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-imports.h"

#define LEASE_MS 1500

/*
 * The probe: one handler for every test kind, doing what the payload's
 * "mode" says and recording what happened.
 */
typedef struct
{
	GMutex      lock;
	GCond       cond;
	guint       started;
	guint       finished;
	guint       overlaps;		/* a job running twice at once */
	guint       timeouts;		/* a bounded wait that ran out */
	gboolean    gate_open;
	GHashTable *runs;		/* id -> runs started */
	GHashTable *active;		/* id -> runs going on */
	GHashTable *queue_active;	/* queue -> runs going on */
	GHashTable *queue_max;		/* queue -> most at once */
	GHashTable *reasons;		/* id -> stop reason of its latest run */
	GHashTable *attempts;		/* id -> attempt of its latest run */
	gint64      progress_set_at;	/* monotonic */
} Probe;

static guint
table_get(
	GHashTable  *table,
	gconstpointer key
){
	return GPOINTER_TO_UINT(g_hash_table_lookup(table, key));
}

static void
probe_init(Probe *probe)
{
	memset(probe, 0, sizeof(*probe));
	g_mutex_init(&probe->lock);
	g_cond_init(&probe->cond);
	probe->runs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	probe->active = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	probe->reasons = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	probe->attempts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	probe->queue_active = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	probe->queue_max = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static void
probe_clear(Probe *probe)
{
	g_hash_table_unref(probe->runs);
	g_hash_table_unref(probe->active);
	g_hash_table_unref(probe->reasons);
	g_hash_table_unref(probe->attempts);
	g_hash_table_unref(probe->queue_active);
	g_hash_table_unref(probe->queue_max);
	g_mutex_clear(&probe->lock);
	g_cond_clear(&probe->cond);
}

static gint64 *
id_key(gint64 id)
{
	gint64 *key = g_new(gint64, 1);

	*key = id;
	return key;
}

static void
probe_open_gate(Probe *probe)
{
	g_mutex_lock(&probe->lock);
	probe->gate_open = TRUE;
	g_cond_broadcast(&probe->cond);
	g_mutex_unlock(&probe->lock);
}

/* Waits for the gate (and, if @honour_cancel, for cancellation), at most
 * 10 s.  Returns TRUE if the gate opened. */
static gboolean
probe_wait_gate(
	Probe          *probe,
	AxisJobContext *context,
	gboolean        honour_cancel
){
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	gboolean open;

	g_mutex_lock(&probe->lock);
	for (;;) {
		open = probe->gate_open;
		if (open)
			break;
		if (honour_cancel && axis_job_context_is_cancelled(context))
			break;
		if (g_get_monotonic_time() >= deadline) {
			probe->timeouts++;
			break;
		}
		g_cond_wait_until(&probe->cond, &probe->lock, g_get_monotonic_time() + 20 * 1000);
	}
	g_mutex_unlock(&probe->lock);

	return open;
}

static gboolean
probe_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	Probe *probe = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *mode = json_object_get_string_member_with_default(payload, "mode", "ok");
	const gchar *queue = axis_job_context_get_queue(context);
	gint64 id = axis_job_context_get_id(context);
	gboolean ok = TRUE;
	guint n;

	g_mutex_lock(&probe->lock);
	probe->started++;
	g_hash_table_replace(probe->runs, id_key(id), GUINT_TO_POINTER(table_get(probe->runs, &id) + 1));
	if (table_get(probe->active, &id) > 0)
		probe->overlaps++;
	g_hash_table_replace(probe->active, id_key(id), GUINT_TO_POINTER(table_get(probe->active, &id) + 1));
	n = table_get(probe->queue_active, queue) + 1;
	g_hash_table_replace(probe->queue_active, g_strdup(queue), GUINT_TO_POINTER(n));
	if (n > table_get(probe->queue_max, queue))
		g_hash_table_replace(probe->queue_max, g_strdup(queue), GUINT_TO_POINTER(n));
	g_hash_table_replace(probe->attempts, id_key(id),
		GUINT_TO_POINTER(axis_job_context_get_attempt(context)));
	g_cond_broadcast(&probe->cond);
	g_mutex_unlock(&probe->lock);

	if (strcmp(mode, "ok") == 0) {
		axis_job_context_wait(context,
			(guint)json_object_get_int_member_with_default(payload, "sleep_ms", 0));
	} else if (strcmp(mode, "fail") == 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "boom");
		ok = FALSE;
	} else if (strcmp(mode, "permanent") == 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "bad payload");
		ok = FALSE;
	} else if (strcmp(mode, "gate") == 0) {
		/* Stops when told to; succeeds when the gate opens. */
		if (!probe_wait_gate(probe, context, TRUE)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "stopped");
			ok = FALSE;
		}
	} else if (strcmp(mode, "gate-succeeds") == 0) {
		/* Returns success when the gate opens or it is told to stop. */
		probe_wait_gate(probe, context, TRUE);
	} else if (strcmp(mode, "stubborn") == 0) {
		/* Ignores being told to stop. */
		probe_wait_gate(probe, context, FALSE);
	} else if (strcmp(mode, "until-cancelled") == 0) {
		if (axis_job_context_wait(context, AXIS_TEST_WAIT_MS)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "stopped");
			ok = FALSE;
		} else {
			g_mutex_lock(&probe->lock);
			probe->timeouts++;
			g_mutex_unlock(&probe->lock);
		}
	} else if (strcmp(mode, "progress") == 0) {
		axis_job_context_set_progress(context, 0.5, "halfway");
		g_mutex_lock(&probe->lock);
		probe->progress_set_at = g_get_monotonic_time();
		g_mutex_unlock(&probe->lock);
		probe_wait_gate(probe, context, TRUE);
	} else if (strcmp(mode, "progress-spam") == 0) {
		gint64 until = g_get_monotonic_time() + 1300 * 1000;
		guint i = 0;

		while (g_get_monotonic_time() < until) {
			g_autofree gchar *message = g_strdup_printf("step %u", ++i);

			axis_job_context_set_progress(context, (gdouble)(i % 100) / 100.0, message);
			g_usleep(2000);
		}
	} else {
		g_error("unknown probe mode %s", mode);
	}

	g_mutex_lock(&probe->lock);
	probe->finished++;
	g_hash_table_replace(probe->active, id_key(id), GUINT_TO_POINTER(table_get(probe->active, &id) - 1));
	g_hash_table_replace(probe->queue_active, g_strdup(queue),
		GUINT_TO_POINTER(table_get(probe->queue_active, queue) - 1));
	g_hash_table_replace(probe->reasons, id_key(id),
		GUINT_TO_POINTER(axis_job_context_get_stop_reason(context)));
	g_cond_broadcast(&probe->cond);
	g_mutex_unlock(&probe->lock);

	return ok;
}

static guint
probe_count(
	Probe *probe,
	guint *field
){
	guint n;

	g_mutex_lock(&probe->lock);
	n = *field;
	g_mutex_unlock(&probe->lock);

	return n;
}

static guint
probe_runs(
	Probe  *probe,
	gint64  id
){
	guint n;

	g_mutex_lock(&probe->lock);
	n = table_get(probe->runs, &id);
	g_mutex_unlock(&probe->lock);

	return n;
}

static guint
probe_reason(
	Probe  *probe,
	gint64  id
){
	guint n;

	g_mutex_lock(&probe->lock);
	n = table_get(probe->reasons, &id);
	g_mutex_unlock(&probe->lock);

	return n;
}

static guint
probe_queue_max(
	Probe       *probe,
	const gchar *queue
){
	guint n;

	g_mutex_lock(&probe->lock);
	n = table_get(probe->queue_max, queue);
	g_mutex_unlock(&probe->lock);

	return n;
}

/*
 * The fixture
 */

typedef struct
{
	AxisTestDb      *db;
	AxisDbPool      *pool;
	AxisJobRegistry *registry;
	AxisJobQueue    *queue;
	Probe            probe;
	GPtrArray       *events;	/* AxisJobEvent, as delivered */
	GThread         *main_thread;
	guint            off_thread_events;
	gboolean         started;
} Fixture;

static const struct
{
	const gchar *kind;
	const gchar *queue;
	guint        max_attempts;
} kinds[] = {
	{ "test.probe", "test", 3 },
	{ "test.lim",   "lim",  3 },
	{ "test.slow",  "slow", 3 },
	{ "test.fast",  "fast", 3 },
	{ "test.ext",   "ext",  3 },
	{ "test.once",  "test", 1 },
	{ "test.two",   "test", 2 },
};

static void
on_job_event(
	AxisJobQueue *queue,
	AxisJobEvent *event,
	gpointer      user_data
){
	Fixture *fx = user_data;

	if (g_thread_self() != fx->main_thread)
		fx->off_thread_events++;
	g_ptr_array_add(fx->events, axis_job_event_copy(event));
}

static void
configure(Fixture *fx)
{
	fx->queue = axis_job_queue_new(fx->pool, fx->registry);
	axis_job_queue_set_poll_interval(fx->queue, 100);
	axis_job_queue_set_lease(fx->queue, LEASE_MS);
	axis_job_queue_set_sweep_interval(fx->queue, 100);
	axis_job_queue_set_schedule_interval(fx->queue, 100);
	axis_job_queue_set_backoff(fx->queue, 50, 200);
	g_signal_connect(fx->queue, "job-event", G_CALLBACK(on_job_event), fx);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint i;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	fx->main_thread = g_thread_self();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;

	fx->pool = axis_test_jobs_pool(fx->db, 16);
	probe_init(&fx->probe);
	fx->events = g_ptr_array_new_with_free_func((GDestroyNotify)axis_job_event_free);
	fx->registry = axis_job_registry_new();
	for (i = 0; i < G_N_ELEMENTS(kinds); i++) {
		AxisJobHandlerInfo info;

		memset(&info, 0, sizeof(info));
		info.kind = kinds[i].kind;
		info.queue = kinds[i].queue;
		info.max_attempts = kinds[i].max_attempts;
		info.handler = probe_handler;
		g_assert_true(axis_job_registry_add(fx->registry, &info, &fx->probe, &error));
		g_assert_no_error(error);
	}
	configure(fx);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}

	probe_open_gate(&fx->probe);
	if (fx->queue != NULL) {
		axis_job_queue_stop(fx->queue, 5000);
		g_assert_true(axis_job_queue_wait_threads(fx->queue, AXIS_TEST_WAIT_MS));
		g_clear_object(&fx->queue);
	}
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_cmpuint(fx->off_thread_events, ==, 0);
	g_assert_cmpuint(fx->probe.overlaps, ==, 0);

	axis_job_registry_unref(fx->registry);
	g_ptr_array_unref(fx->events);
	probe_clear(&fx->probe);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

static void
start(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
	fx->started = TRUE;
}

static gint64
enqueue_with(
	Fixture     *fx,
	const gchar *kind,
	const gchar *mode,
	gint64       sleep_ms,
	gint64       delay_ms
){
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	AxisJobEnqueueOptions options;
	gint64 id = 0;

	json_object_set_string_member(payload, "mode", mode);
	if (sleep_ms > 0)
		json_object_set_int_member(payload, "sleep_ms", sleep_ms);
	memset(&options, 0, sizeof(options));
	options.delay_ms = delay_ms;
	g_assert_true(axis_job_queue_enqueue(fx->queue, kind, payload, &options, &id, NULL, &error));
	g_assert_no_error(error);

	return id;
}

static gint64
enqueue_mode(
	Fixture     *fx,
	const gchar *kind,
	const gchar *mode
){
	return enqueue_with(fx, kind, mode, 0, 0);
}

static gboolean
wait_job(
	Fixture     *fx,
	gint64       id,
	const gchar *expression,
	const gchar *expected
){
	g_autofree gchar *sql = g_strdup_printf("SELECT (%s)::text FROM job WHERE id = %"
		G_GINT64_FORMAT, expression, id);

	return axis_test_wait_query(fx->db, sql, expected, AXIS_TEST_WAIT_MS);
}

static void
assert_job(
	Fixture     *fx,
	gint64       id,
	const gchar *expression,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_queryf(fx->db, "SELECT (%s)::text FROM job WHERE id = %"
		G_GINT64_FORMAT, expression, id);

	if (g_strcmp0(value, expected) != 0)
		g_error("job %" G_GINT64_FORMAT ": %s is %s, expected %s", id, expression,
			value != NULL ? value : "NULL", expected != NULL ? expected : "NULL");
}

typedef struct
{
	Probe *probe;
	guint *field;
	guint  at_least;
} CountWait;

static gboolean
count_reached(gpointer data)
{
	CountWait *wait = data;

	return probe_count(wait->probe, wait->field) >= wait->at_least;
}

typedef struct
{
	GLogLevelFlags  level;
	const gchar    *pattern;
	guint           n;
} LogWait;

static gboolean
log_seen(gpointer data)
{
	LogWait *wait = data;

	return axis_test_log_count(wait->level, wait->pattern) >= wait->n;
}

/* A log line comes from the thread that did the work, a moment after the
 * state the test waited for: wait for it too, then check it is the only
 * one. */
static void
wait_log(
	GLogLevelFlags  level,
	const gchar    *pattern,
	guint           n
){
	LogWait wait;

	wait.level = level;
	wait.pattern = pattern;
	wait.n = n;
	g_assert_true(axis_test_pump_until(log_seen, &wait, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_test_log_count(level, pattern), ==, n);
}

static void
wait_started(
	Fixture *fx,
	guint    n
){
	CountWait wait;

	wait.probe = &fx->probe;
	wait.field = &fx->probe.started;
	wait.at_least = n;
	g_assert_true(axis_test_pump_until(count_reached, &wait, AXIS_TEST_WAIT_MS));
}

/* Spec: Queued job survives restart */
static void
test_durable_across_restart(Fixture *fx, gconstpointer data)
{
	gint64 id;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_with(fx, "test.probe", "ok", 0, 1000);
	g_assert_true(axis_job_queue_stop(fx->queue, 1000));
	g_clear_object(&fx->queue);
	assert_job(fx, id, "state || '/' || attempts", "queued/0");
	g_assert_cmpuint(probe_runs(&fx->probe, id), ==, 0);

	/* A new queue (a restarted daemon) runs it. */
	configure(fx);
	start(fx);
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "succeeded/1"));
	g_assert_cmpuint(probe_runs(&fx->probe, id), ==, 1);
}

/* Spec: Unknown kind is refused at enqueue */
static void
test_unknown_kind_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *count = NULL;

	if (fx->db == NULL)
		return;

	start(fx);
	g_assert_false(axis_job_queue_enqueue(fx->queue, "test.missing", NULL, NULL, NULL, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND);
	g_assert_nonnull(strstr(error->message, "test.missing"));
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM job");
	g_assert_cmpstr(count, ==, "0");
}

/* Spec: Limit is respected */
static void
test_concurrency_limit(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *done = NULL;
	guint i;

	if (fx->db == NULL)
		return;

	axis_job_queue_set_concurrency(fx->queue, "lim", 2);
	start(fx);
	for (i = 0; i < 10; i++)
		enqueue_with(fx, "test.lim", "ok", 60, 0);

	g_assert_true(axis_test_wait_query(fx->db,
		"SELECT count(*) FROM job WHERE queue = 'lim' AND state = 'succeeded'", "10",
		AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(probe_queue_max(&fx->probe, "lim"), ==, 2);
}

/* Spec: Queues are independent */
static void
test_queues_independent(Fixture *fx, gconstpointer data)
{
	gint64 slow[3];
	gint64 fast;
	guint i;

	if (fx->db == NULL)
		return;

	start(fx);
	for (i = 0; i < G_N_ELEMENTS(slow); i++)
		slow[i] = enqueue_mode(fx, "test.slow", "gate");
	wait_started(fx, 1);

	/* "slow" is saturated (concurrency 1, two more waiting). */
	fast = enqueue_mode(fx, "test.fast", "ok");
	g_assert_true(wait_job(fx, fast, "state", "succeeded"));
	assert_job(fx, slow[1], "state", "queued");
	assert_job(fx, slow[2], "state", "queued");

	probe_open_gate(&fx->probe);
	for (i = 0; i < G_N_ELEMENTS(slow); i++)
		g_assert_true(wait_job(fx, slow[i], "state", "succeeded"));
	g_assert_cmpuint(probe_queue_max(&fx->probe, "slow"), ==, 1);
}

/* Spec: Externally served queue */
static void
test_external_queue(Fixture *fx, gconstpointer data)
{
	static const gchar *const ext_kinds[] = { "test.ext", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) claim = NULL;
	AxisDbConn *conn;
	gint64 ext[3];
	gint64 fast;
	guint i;

	if (fx->db == NULL)
		return;

	axis_job_queue_set_concurrency(fx->queue, "ext", 0);
	g_assert_cmpuint(axis_job_queue_get_concurrency(fx->queue, "ext"), ==, 0);
	start(fx);
	for (i = 0; i < G_N_ELEMENTS(ext); i++)
		ext[i] = enqueue_mode(fx, "test.ext", "ok");
	fast = enqueue_mode(fx, "test.fast", "ok");
	g_assert_true(wait_job(fx, fast, "state", "succeeded"));

	/* Several poll intervals later, still untouched here. */
	axis_test_pump_for(300);
	for (i = 0; i < G_N_ELEMENTS(ext); i++) {
		assert_job(fx, ext[i], "state || '/' || attempts", "queued/0");
		g_assert_cmpuint(probe_runs(&fx->probe, ext[i]), ==, 0);
	}

	/* An external worker can claim them. */
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_job_claim(conn, "ext", ext_kinds, "remote-worker", 60000, &claim, &error));
	g_assert_nonnull(claim);
	g_assert_cmpint(claim->id, ==, ext[0]);
	g_assert_true(axis_job_succeed(conn, claim->id, claim->lease_token, &error));
	axis_db_pool_release(fx->pool, conn);
	g_assert_no_error(error);
}

typedef struct
{
	Fixture *fx;
	gint64   id;
	guint    state;
} EventWait;

static gboolean
event_seen(gpointer data)
{
	EventWait *wait = data;
	guint i;

	for (i = 0; i < wait->fx->events->len; i++) {
		AxisJobEvent *e = g_ptr_array_index(wait->fx->events, i);

		if (e->id == wait->id && e->type == AXIS_JOB_EVENT_STATE && e->state == wait->state)
			return TRUE;
	}

	return FALSE;
}

/* Events arrive in commit order, so once a job's final event has been
 * delivered every earlier event of it has been too. */
static void
wait_for_event(
	Fixture *fx,
	gint64   id,
	guint    state
){
	EventWait wait;

	wait.fx = fx;
	wait.id = id;
	wait.state = state;
	g_assert_true(axis_test_pump_until(event_seen, &wait, AXIS_TEST_WAIT_MS));
}

/* Spec: Progress is visible */
static void
test_progress_visible(Fixture *fx, gconstpointer data)
{
	gint64 id;
	gint64 set_at;
	gint64 seen_at;
	guint i;
	gboolean event = FALSE;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "progress");
	g_assert_true(wait_job(fx, id, "progress || ' ' || progress_message", "0.5 halfway"));
	seen_at = g_get_monotonic_time();
	g_mutex_lock(&fx->probe.lock);
	set_at = fx->probe.progress_set_at;
	g_mutex_unlock(&fx->probe.lock);
	g_test_message("progress stored %" G_GINT64_FORMAT " ms after it was set",
		(seen_at - set_at) / 1000);
	g_assert_cmpint(set_at, >, 0);
	g_assert_cmpint(seen_at - set_at, <, 2 * G_USEC_PER_SEC);

	probe_open_gate(&fx->probe);
	g_assert_true(wait_job(fx, id, "state", "succeeded"));
	wait_for_event(fx, id, AXIS_JOB_STATE_SUCCEEDED);
	for (i = 0; i < fx->events->len; i++) {
		AxisJobEvent *e = g_ptr_array_index(fx->events, i);

		if (e->id == id && e->type == AXIS_JOB_EVENT_PROGRESS) {
			g_assert_cmpfloat(e->progress, ==, 0.5);
			g_assert_cmpstr(e->message, ==, "halfway");
			event = TRUE;
		}
	}
	g_assert_true(event);
}

/* At most one progress write per second per job. */
static void
test_progress_throttled(Fixture *fx, gconstpointer data)
{
	guint writes = 0;
	gint64 id;
	guint i;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "progress-spam");
	g_assert_true(wait_job(fx, id, "state", "succeeded"));
	wait_for_event(fx, id, AXIS_JOB_STATE_SUCCEEDED);
	for (i = 0; i < fx->events->len; i++) {
		AxisJobEvent *e = g_ptr_array_index(fx->events, i);

		if (e->id == id && e->type == AXIS_JOB_EVENT_PROGRESS)
			writes++;
	}
	/* Hundreds of updates over 1.3 s: two writes, three at most. */
	g_test_message("%u progress writes", writes);
	g_assert_cmpuint(writes, >=, 1);
	g_assert_cmpuint(writes, <=, 3);
}

static gchar *
state_events(
	Fixture *fx,
	gint64   id
){
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; i < fx->events->len; i++) {
		AxisJobEvent *e = g_ptr_array_index(fx->events, i);

		if (e->id != id || e->type != AXIS_JOB_EVENT_STATE)
			continue;
		g_string_append_printf(out, "%s%s/%u%s", out->len > 0 ? " " : "",
			axis_job_state_to_string(e->state), e->attempts,
			e->state == AXIS_JOB_STATE_QUEUED && !e->runnable ? "(later)" : "");
	}

	return g_string_free(out, FALSE);
}

/* Spec: Events follow the lifecycle */
static void
test_event_order(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *sequence = NULL;
	g_autofree gchar *retried = NULL;
	EventWait wait;
	gint64 id;
	gint64 failing;
	guint i;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "ok");
	wait.fx = fx;
	wait.id = id;
	wait.state = AXIS_JOB_STATE_SUCCEEDED;
	g_assert_true(axis_test_pump_until(event_seen, &wait, AXIS_TEST_WAIT_MS));
	sequence = state_events(fx, id);
	g_assert_cmpstr(sequence, ==, "queued/0 running/1 succeeded/1");

	for (i = 0; i < fx->events->len; i++) {
		AxisJobEvent *e = g_ptr_array_index(fx->events, i);

		if (e->id == id) {
			g_assert_cmpstr(e->kind, ==, "test.probe");
			g_assert_cmpstr(e->queue, ==, "test");
			g_assert_cmpuint(e->max_attempts, ==, 3);
		}
	}

	/* A failure retried later, then failed for good, with its error. */
	failing = enqueue_mode(fx, "test.two", "fail");
	wait.id = failing;
	wait.state = AXIS_JOB_STATE_FAILED;
	g_assert_true(axis_test_pump_until(event_seen, &wait, AXIS_TEST_WAIT_MS));
	retried = state_events(fx, failing);
	g_assert_cmpstr(retried, ==, "queued/0 running/1 queued/1(later) running/2 failed/2");
	for (i = 0; i < fx->events->len; i++) {
		AxisJobEvent *e = g_ptr_array_index(fx->events, i);

		if (e->id == failing && e->state == AXIS_JOB_STATE_FAILED)
			g_assert_cmpstr(e->message, ==, "boom");
	}
	assert_job(fx, failing, "last_error", "boom");
}

/* Spec: Cancel running job */
static void
test_cancel_running(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	gint64 id;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "until-cancelled");
	wait_started(fx, 1);
	g_assert_true(axis_job_queue_cancel(fx->queue, id, &outcome, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_REQUESTED);

	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "cancelled/1"));
	g_assert_cmpuint(probe_reason(&fx->probe, id), ==, AXIS_JOB_STOP_CANCELLED);
	assert_job(fx, id, "last_error LIKE 'cancelled%'", "true");

	/* Not retried. */
	axis_test_pump_for(400);
	assert_job(fx, id, "state || '/' || attempts", "cancelled/1");
	g_assert_cmpuint(probe_runs(&fx->probe, id), ==, 1);
	g_assert_cmpuint(fx->probe.timeouts, ==, 0);
}

/* Cancelled from somewhere else (another connection, as the HTTP API
 * would): the handler hears of it through the notification. */
static void
test_cancel_running_elsewhere(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	AxisDbConn *conn;
	gint64 id;
	gint64 asked;

	if (fx->db == NULL)
		return;

	/* A minute's lease: no heartbeat (every 20 s) can carry the request
	 * within the test, so only the notification can. */
	axis_job_queue_set_lease(fx->queue, 60000);
	start(fx);
	id = enqueue_mode(fx, "test.probe", "until-cancelled");
	wait_started(fx, 1);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_true(axis_job_cancel(conn, id, &outcome, &error));
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_REQUESTED);
	asked = g_get_monotonic_time();

	g_assert_true(wait_job(fx, id, "state", "cancelled"));
	/* Well before the first heartbeat could have told it. */
	g_assert_cmpint(g_get_monotonic_time() - asked, <, 10 * G_USEC_PER_SEC);
	g_assert_cmpuint(probe_reason(&fx->probe, id), ==, AXIS_JOB_STOP_CANCELLED);
}

/* Spec: Cancel queued job (through the queue) */
static void
test_cancel_queued(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobCancelOutcome outcome;
	gint64 id;
	gint64 after;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_with(fx, "test.probe", "ok", 0, 1000);
	g_assert_true(axis_job_queue_cancel(fx->queue, id, &outcome, &error));
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_DONE);
	after = enqueue_with(fx, "test.probe", "ok", 0, 400);
	g_assert_true(wait_job(fx, after, "state", "succeeded"));
	assert_job(fx, id, "state || '/' || attempts", "cancelled/0");
	g_assert_cmpuint(probe_runs(&fx->probe, id), ==, 0);

	g_assert_true(axis_job_queue_cancel(fx->queue, after, &outcome, &error));
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_TOO_LATE);
}

/* Retries, and permanent failures, through handlers. */
static void
test_handler_failures(Fixture *fx, gconstpointer data)
{
	gint64 failing;
	gint64 permanent;

	if (fx->db == NULL)
		return;

	start(fx);
	failing = enqueue_mode(fx, "test.probe", "fail");
	permanent = enqueue_mode(fx, "test.probe", "permanent");
	g_assert_true(wait_job(fx, failing, "state || '/' || attempts", "failed/3"));
	g_assert_true(wait_job(fx, permanent, "state || '/' || attempts", "failed/1"));
	assert_job(fx, failing, "last_error", "boom");
	assert_job(fx, permanent, "last_error", "bad payload");
	g_assert_cmpuint(probe_runs(&fx->probe, failing), ==, 3);
	g_assert_cmpuint(probe_runs(&fx->probe, permanent), ==, 1);
}

static void
upsert_schedule(
	Fixture     *fx,
	const gchar *name,
	const gchar *mode,
	guint64      interval,
	gboolean     enabled
){
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	AxisDbConn *conn;

	json_object_set_string_member(payload, "mode", mode);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_job_schedule_upsert(conn, name, "test.probe", payload, interval, enabled,
		&error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
}

static gchar *
schedule_jobs(
	Fixture     *fx,
	const gchar *name,
	const gchar *where
){
	return axis_test_db_queryf(fx->db, "SELECT count(*) FROM job WHERE dedupe_key = 'schedule:%s'%s",
		name, where);
}

typedef struct
{
	Fixture     *fx;
	const gchar *name;
	const gchar *at_least;
} ScheduleWait;

static gboolean
schedule_succeeded(gpointer data)
{
	ScheduleWait *wait = data;
	g_autofree gchar *n = axis_test_db_queryf(wait->fx->db,
		"SELECT count(*) >= %s FROM job WHERE dedupe_key = 'schedule:%s' AND state = 'succeeded'",
		wait->at_least, wait->name);

	return g_strcmp0(n, "t") == 0;
}

/* Spec: Schedule fires */
static void
test_schedule_fires(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *span = NULL;
	ScheduleWait wait;
	gdouble seconds;

	if (fx->db == NULL)
		return;

	upsert_schedule(fx, "every-second", "ok", 1, TRUE);
	start(fx);
	wait.fx = fx;
	wait.name = "every-second";
	wait.at_least = "3";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));

	span = axis_test_db_query(fx->db, "SELECT extract(epoch FROM max(started_at) - min(started_at)) "
		"FROM (SELECT started_at FROM job WHERE dedupe_key = 'schedule:every-second' "
		"ORDER BY id LIMIT 3) AS first");
	seconds = g_ascii_strtod(span, NULL);
	g_test_message("three runs spanned %.2f s", seconds);
	/* Two intervals, give or take the tick and the claim. */
	g_assert_cmpfloat(seconds, >=, 1.5);
	g_assert_cmpfloat(seconds, <=, 3.5);
}

/* Spec: No overlap */
static void
test_schedule_no_overlap(Fixture *fx, gconstpointer data)
{
	ScheduleWait wait;
	gint64 until;

	if (fx->db == NULL)
		return;

	upsert_schedule(fx, "long", "gate", 1, TRUE);
	start(fx);
	wait_started(fx, 1);

	/* An interval and a half with the first still running (so it came
	 * due again meanwhile): never a second one queued or running. */
	until = g_get_monotonic_time() + 1500 * 1000;
	while (g_get_monotonic_time() < until) {
		g_autofree gchar *all = schedule_jobs(fx, "long", "");

		g_assert_cmpstr(all, ==, "1");
		axis_test_pump_for(100);
	}
	g_assert_cmpuint(probe_count(&fx->probe, &fx->probe.started), ==, 1);

	/* Once it finishes, the next comes. */
	probe_open_gate(&fx->probe);
	wait.fx = fx;
	wait.name = "long";
	wait.at_least = "2";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));
}

/* Spec: No burst after downtime */
static void
test_schedule_no_burst(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *all = NULL;
	g_autofree gchar *out = NULL;
	ScheduleWait wait;

	if (fx->db == NULL)
		return;

	upsert_schedule(fx, "hourly", "ok", 3600, TRUE);
	out = axis_test_db_query(fx->db, "UPDATE job_schedule SET next_run_at = now() - interval "
		"'100 hours' WHERE name = 'hourly' RETURNING 1");
	start(fx);
	wait.fx = fx;
	wait.name = "hourly";
	wait.at_least = "1";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));
	/* Several ticks later, still the one. */
	axis_test_pump_for(300);
	all = schedule_jobs(fx, "hourly", "");
	g_assert_cmpstr(all, ==, "1");
}

/* Spec: Disabled schedule */
static void
test_schedule_disabled(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *none = NULL;
	ScheduleWait wait;

	if (fx->db == NULL)
		return;

	/* Both due at once: the tick that runs "on" would run "off" too. */
	upsert_schedule(fx, "off", "ok", 1, FALSE);
	upsert_schedule(fx, "on", "ok", 1, TRUE);
	start(fx);
	wait.fx = fx;
	wait.name = "on";
	wait.at_least = "1";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));
	axis_test_pump_for(300);
	none = schedule_jobs(fx, "off", "");
	g_assert_cmpstr(none, ==, "0");
}

/*
 * Spec: After the clock went back.  The tick moves next_run_at to now() plus
 * the interval by the database's clock; when that clock is stepped back (a
 * host that booted a day ahead and was put right, a virtual machine restored
 * from a snapshot), every schedule's next run lies that far ahead.  The state
 * a step back of one day leaves is made here directly.  Found by the stage 2
 * hunt of the job queue (H-2): no schedule fired until the clock caught up.
 */
static void
test_schedule_after_the_clock_went_back(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *out = NULL;
	g_autofree gchar *ahead = NULL;
	ScheduleWait wait;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the database's clock went back*");
	upsert_schedule(fx, "every-second", "ok", 1, TRUE);
	upsert_schedule(fx, "hourly", "ok", 3600, TRUE);
	/* The last ticks ran when the clock read a day later than it does now;
	 * the hourly one is within its interval and stays as it is. */
	out = axis_test_db_query(fx->db, "UPDATE job_schedule SET next_run_at = now() + CASE name "
		"WHEN 'every-second' THEN interval '1 day 1 second' ELSE interval '30 minutes' END, "
		"last_run_at = now() + interval '1 day' RETURNING 1");

	start(fx);
	wait.fx = fx;
	wait.name = "every-second";
	wait.at_least = "2";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));

	/* Said once, when it was brought back, not at every tick. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*schedule 'every-second'*clock went back*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*schedule 'hourly'*"), ==, 0);
	ahead = axis_test_db_query(fx->db, "SELECT (next_run_at BETWEEN now() + interval '29 minutes' AND now() + "
		"interval '31 minutes')::text FROM job_schedule WHERE name = 'hourly'");
	g_assert_cmpstr(ahead, ==, "true");
}

/* A queue on @registry with the fixture's short timings, started. */
static AxisJobQueue *
start_queue_with(
	Fixture         *fx,
	AxisJobRegistry *registry
){
	g_autoptr(GError) error = NULL;
	AxisJobQueue *queue = axis_job_queue_new(fx->pool, registry);

	axis_job_queue_set_poll_interval(queue, 100);
	axis_job_queue_set_lease(queue, LEASE_MS);
	axis_job_queue_set_sweep_interval(queue, 100);
	axis_job_queue_set_schedule_interval(queue, 100);
	axis_job_queue_set_backoff(queue, 50, 200);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	return queue;
}

static void
stop_queue(AxisJobQueue **queue)
{
	axis_job_queue_stop(*queue, 5000);
	g_assert_true(axis_job_queue_wait_threads(*queue, AXIS_TEST_WAIT_MS));
	g_clear_object(queue);
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* A registry of the probe kind alone, declaring @schedule (nullable). */
static AxisJobRegistry *
build_with(
	Fixture                   *fx,
	const AxisJobScheduleInfo *schedule
){
	g_autoptr(GError) error = NULL;
	AxisJobRegistry *registry = axis_job_registry_new();
	AxisJobHandlerInfo info;

	memset(&info, 0, sizeof(info));
	info.kind = "test.probe";
	info.queue = "test";
	info.max_attempts = 3;
	info.handler = probe_handler;
	g_assert_true(axis_job_registry_add(registry, &info, &fx->probe, &error));
	if (schedule != NULL)
		g_assert_true(axis_job_registry_add_schedule(registry, schedule, &error));
	g_assert_no_error(error);
	return registry;
}

/*
 * Spec: A newer build changes a schedule.  Found by the stage 2 hunt of the
 * job queue (H-3): a schedule's interval and payload stayed those of the
 * build that first made its row.
 */
static void
test_schedule_follows_the_build(Fixture *fx, gconstpointer data)
{
	AxisJobScheduleInfo info;
	g_autoptr(AxisJobRegistry) older = NULL;
	g_autoptr(AxisJobRegistry) newer = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *stored = NULL;

	if (fx->db == NULL)
		return;

	/* The older build: hourly, no payload. */
	memset(&info, 0, sizeof(info));
	info.name = "test.tidy";
	info.kind = "test.probe";
	info.interval_seconds = 3600;
	older = build_with(fx, &info);
	queue = start_queue_with(fx, older);
	stop_queue(&queue);

	/* The newer build: every minute, with a payload it needs. */
	info.payload = "{\"mode\": \"ok\", \"version\": 2}";
	info.interval_seconds = 60;
	newer = build_with(fx, &info);
	queue = start_queue_with(fx, newer);
	stored = axis_test_db_query(fx->db, "SELECT extract(epoch FROM run_interval)::bigint || ' ' || "
		"coalesce(payload->>'version', '(none)') || ' ' || enabled || ' ' || declared FROM job_schedule "
		"WHERE name = 'test.tidy'");
	stop_queue(&queue);

	g_assert_cmpstr(stored, ==, "60 2 true true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE,
		"*schedule 'test.tidy' follows this build now: test.probe every 60 s*every 3600 s*"), ==, 1);
}

/*
 * Spec: A newer build drops a schedule; the build declares it again.  Found
 * by the stage 2 hunt of the job queue (H-3): a schedule no module declared
 * any more kept firing for as long as its kind existed.
 */
static void
test_schedule_dropped_by_the_build(Fixture *fx, gconstpointer data)
{
	AxisJobScheduleInfo info;
	g_autoptr(AxisJobRegistry) without = NULL;
	g_autoptr(AxisJobRegistry) with = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *kept = NULL;
	g_autofree gchar *fired = NULL;
	ScheduleWait wait;

	if (fx->db == NULL)
		return;

	/* Left by an older build that declared it. */
	out = axis_test_db_query(fx->db, "INSERT INTO job_schedule (name, kind, payload, run_interval) "
		"VALUES ('test.dropped', 'test.probe', '{}', interval '1 second') RETURNING 1");

	/* This build has the kind and no schedule of it. */
	without = build_with(fx, NULL);
	queue = start_queue_with(fx, without);
	axis_test_pump_for(1500);
	fired = schedule_jobs(fx, "test.dropped", "");
	kept = axis_test_db_query(fx->db, "SELECT enabled::text FROM job_schedule WHERE name = 'test.dropped'");
	stop_queue(&queue);
	g_assert_cmpstr(fired, ==, "0");
	g_assert_cmpstr(kept, ==, "false");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE,
		"*schedule 'test.dropped' (test.probe) is disabled: no module of this build declares it*"), ==, 1);

	/* A later build that declares it again: enabled, and it fires. */
	memset(&info, 0, sizeof(info));
	info.name = "test.dropped";
	info.kind = "test.probe";
	info.interval_seconds = 1;
	with = build_with(fx, &info);
	queue = start_queue_with(fx, with);
	wait.fx = fx;
	wait.name = "test.dropped";
	wait.at_least = "1";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));
	stop_queue(&queue);
}

/*
 * Spec: Switched off by hand.  Before the build's schedules were told from a
 * person's, the only way to switch one off was in the database; the
 * migration marks every stored schedule the build's.  Such a schedule stays
 * off when the build that declares it starts: only one the build itself
 * disabled is enabled again.  Found by an independent review of these
 * fixes: the first start enabled it again.
 */
static void
test_schedule_switched_off_by_hand(Fixture *fx, gconstpointer data)
{
	AxisJobScheduleInfo info;
	g_autoptr(AxisJobRegistry) build = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *fired = NULL;

	if (fx->db == NULL)
		return;

	/* As the migration leaves one an older build made and a person
	 * switched off in the database. */
	out = axis_test_db_query(fx->db, "INSERT INTO job_schedule (name, kind, payload, run_interval, enabled) "
		"VALUES ('test.off', 'test.probe', '{}', interval '1 second', false) RETURNING 1");
	memset(&info, 0, sizeof(info));
	info.name = "test.off";
	info.kind = "test.probe";
	info.interval_seconds = 1;
	build = build_with(fx, &info);
	queue = start_queue_with(fx, build);
	axis_test_pump_for(1500);
	fired = schedule_jobs(fx, "test.off", "");
	stored = axis_test_db_query(fx->db, "SELECT enabled || ' ' || declared || ' ' || (retired_at IS NULL) "
		"FROM job_schedule WHERE name = 'test.off'");
	stop_queue(&queue);

	g_assert_cmpstr(fired, ==, "0");
	g_assert_cmpstr(stored, ==, "false true true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE,
		"*schedule 'test.off' stays disabled: it was switched off, not by a build*"), ==, 1);
}

/* Spec: A person's schedule. */
static void
test_schedule_of_a_person_is_kept(Fixture *fx, gconstpointer data)
{
	AxisJobScheduleInfo info;
	g_autoptr(AxisJobRegistry) build = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autofree gchar *stored = NULL;

	if (fx->db == NULL)
		return;

	/* The build declares "test.tidy" hourly; a person made it every two
	 * hours, and made "test.mine" too, which no module declares. */
	upsert_schedule(fx, "test.tidy", "ok", 7200, TRUE);
	upsert_schedule(fx, "test.mine", "ok", 3600, TRUE);
	memset(&info, 0, sizeof(info));
	info.name = "test.tidy";
	info.kind = "test.probe";
	info.interval_seconds = 3600;
	build = build_with(fx, &info);
	queue = start_queue_with(fx, build);
	stored = axis_test_db_query(fx->db, "SELECT string_agg(name || ' ' || extract(epoch FROM run_interval)::bigint "
		"|| ' ' || enabled || ' ' || declared, ', ' ORDER BY name) FROM job_schedule WHERE name LIKE 'test.%'");
	stop_queue(&queue);

	g_assert_cmpstr(stored, ==, "test.mine 3600 true false, test.tidy 7200 true false");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*schedule 'test.*"), ==, 0);
}

/*
 * Spec: Jobs an earlier build queued on another queue.  The build that
 * gave searches a queue of their own (H-4 of the stage 2 hunt of the job
 * queue) finds the searches an earlier build queued on "downloads": they run
 * on their kind's queue now; a finished job keeps the queue it ran on.
 */
static void
test_jobs_moved_to_their_kinds_queue(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *queued = NULL;
	g_autofree gchar *running = NULL;
	g_autofree gchar *finished = NULL;
	g_autofree gchar *where = NULL;
	gint64 queued_id;
	gint64 running_id;

	if (fx->db == NULL)
		return;

	queued = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, priority) "
		"VALUES ('test.probe', 'downloads', 3, 10) RETURNING id");
	running = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"lease_token, lease_expires_at, started_at, worker_id) VALUES ('test.probe', 'downloads', 3, 'running', 1, "
		"gen_random_uuid(), now() - interval '1 minute', now(), 'gone-worker') RETURNING id");
	finished = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"finished_at) VALUES ('test.probe', 'downloads', 3, 'succeeded', 1, now()) RETURNING id");
	queued_id = g_ascii_strtoll(queued, NULL, 10);
	running_id = g_ascii_strtoll(running, NULL, 10);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker gone-worker expired*");

	start(fx);
	g_assert_true(wait_job(fx, queued_id, "state", "succeeded"));
	g_assert_true(wait_job(fx, running_id, "state", "succeeded"));
	where = axis_test_db_query(fx->db, "SELECT string_agg(queue || '/' || priority, ' ' ORDER BY id) FROM job");
	g_assert_cmpstr(where, ==, "test/10 test/0 downloads/0");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*moved 1 queued and 1 running job(s) of kind "
		"'test.probe' from queue 'downloads' to 'test'*"), ==, 1);
}

/*
 * An import or identify job whose payload names nothing never will: it
 * fails at its first attempt, not after all of them.  Found by the stage 2
 * hunt of the job queue (a minor finding): the error was a retryable one.
 * The real handlers run here; they look at the payload before anything else.
 */
static void
test_payload_naming_nothing(Fixture *fx, gconstpointer data)
{
	const AxisModuleInfo *(*const getters[])(void) = { axis_web_imports_get_module, axis_web_identify_get_module };
	static const gchar *const wanted[] = { AXIS_IMPORT_KIND, AXIS_IDENTIFY_KIND };
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	gint64 ids[2];
	guint i;

	if (fx->db == NULL)
		return;

	for (i = 0; i < G_N_ELEMENTS(getters); i++) {
		const AxisModuleInfo *module = getters[i]();
		guint j;

		for (j = 0; module->job_handlers[j].kind != NULL; j++) {
			if (strcmp(module->job_handlers[j].kind, wanted[i]) == 0)
				g_assert_true(axis_job_registry_add(registry, &module->job_handlers[j], NULL, &error));
		}
		g_assert_no_error(error);
	}
	queue = start_queue_with(fx, registry);
	for (i = 0; i < G_N_ELEMENTS(wanted); i++) {
		g_autoptr(JsonObject) payload = json_object_new();

		json_object_set_string_member(payload, "import", "not a number");
		g_assert_true(axis_job_queue_enqueue(queue, wanted[i], payload, NULL, &ids[i], NULL, &error));
		g_assert_no_error(error);
	}
	for (i = 0; i < G_N_ELEMENTS(wanted); i++) {
		g_assert_true(wait_job(fx, ids[i], "state", "failed"));
		assert_job(fx, ids[i], "attempts", "1");
		assert_job(fx, ids[i], "max_attempts > 1", "true");
		assert_job(fx, ids[i], "last_error LIKE 'The job names no %'", "true");
	}
	stop_queue(&queue);
}

typedef struct
{
	Probe *probe;
	guint  delay_ms;
} Opener;

static gpointer
opener_thread(gpointer data)
{
	Opener *opener = data;

	g_usleep((gulong)opener->delay_ms * 1000);
	probe_open_gate(opener->probe);
	return NULL;
}

/* Spec: Job finishes within grace */
static void
test_shutdown_within_grace(Fixture *fx, gconstpointer data)
{
	GThread *thread;
	Opener opener;
	gint64 id;
	gint64 began;
	gint64 took;
	gboolean clean;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "stubborn");
	wait_started(fx, 1);

	/* The handler ignores the request to stop and finishes 300 ms later,
	 * well within the grace period. */
	opener.probe = &fx->probe;
	opener.delay_ms = 300;
	thread = g_thread_new("opener", opener_thread, &opener);
	began = g_get_monotonic_time();
	clean = axis_job_queue_stop(fx->queue, 5000);
	took = g_get_monotonic_time() - began;
	g_thread_join(thread);

	g_assert_true(clean);
	g_assert_cmpint(took, >=, 250 * 1000);
	g_assert_cmpint(took, <, 4 * G_USEC_PER_SEC);
	assert_job(fx, id, "state || '/' || attempts", "succeeded/1");
	g_assert_cmpuint(probe_reason(&fx->probe, id), ==, AXIS_JOB_STOP_SHUTDOWN);
}

/* Spec: Job exceeds grace */
static void
test_shutdown_beyond_grace(Fixture *fx, gconstpointer data)
{
	gint64 id;
	gint64 began;
	gint64 took;
	gboolean clean;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*did not finish within the shutdown grace period*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*stopped with * thread(s) still running*");

	start(fx);
	id = enqueue_mode(fx, "test.probe", "stubborn");
	wait_started(fx, 1);
	assert_job(fx, id, "state || '/' || attempts", "running/1");

	began = g_get_monotonic_time();
	clean = axis_job_queue_stop(fx->queue, 300);
	took = g_get_monotonic_time() - began;
	g_test_message("stop took %" G_GINT64_FORMAT " ms", took / 1000);

	/* Back in the queue with the attempt count it had before it started,
	 * and the handler still running. */
	g_assert_false(clean);
	g_assert_cmpint(took, <, (300 + AXIS_JOB_QUEUE_STOP_MARGIN_MS) * 1000 + G_USEC_PER_SEC);
	assert_job(fx, id, "state || '/' || attempts", "queued/0");
	assert_job(fx, id, "lease_token IS NULL AND finished_at IS NULL", "true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING,
		"*did not finish within the shutdown grace period; it was handed back*"), ==, 1);
	g_assert_cmpuint(probe_count(&fx->probe, &fx->probe.finished), ==, 0);

	/* When it does finish, its success is discarded. */
	probe_open_gate(&fx->probe);
	g_assert_true(axis_job_queue_wait_threads(fx->queue, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(probe_count(&fx->probe, &fx->probe.finished), ==, 1);
	assert_job(fx, id, "state || '/' || attempts", "queued/0");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE,
		"*succeeded after the shutdown grace period ended*discarded*"), ==, 1);

	/* The next start runs it as its first attempt. */
	g_clear_object(&fx->queue);
	fx->probe.gate_open = TRUE;
	configure(fx);
	start(fx);
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "succeeded/1"));
}

/* A handler that stops when told to: the job is handed back at once. */
static void
test_shutdown_hands_back(Fixture *fx, gconstpointer data)
{
	gint64 id;
	gint64 began;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "until-cancelled");
	wait_started(fx, 1);
	began = g_get_monotonic_time();
	g_assert_true(axis_job_queue_stop(fx->queue, 5000));
	g_assert_cmpint(g_get_monotonic_time() - began, <, 2 * G_USEC_PER_SEC);
	assert_job(fx, id, "state || '/' || attempts", "queued/0");
	g_assert_cmpuint(probe_reason(&fx->probe, id), ==, AXIS_JOB_STOP_SHUTDOWN);
	g_assert_cmpuint(fx->probe.timeouts, ==, 0);

	/* Stopping again does nothing. */
	g_assert_true(axis_job_queue_stop(fx->queue, 0));
}

/*
 * Spec: Handler missing after restart; A schedule whose kind changed.  The
 * queued jobs of a kind this build does not have are failed (kept), which
 * frees their keys: a schedule now of another kind fires again instead of
 * waiting behind its old job for good.  Found by an independent review of
 * fix-jobs-hunt-findings.  The running job and the schedule of the kind
 * are left as they are.
 */
static void
test_missing_handler(Fixture *fx, gconstpointer data)
{
	static const gchar *const gone_kinds[] = { "gone.kind", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) claim = NULL;
	g_autofree gchar *untouched = NULL;
	g_autofree gchar *warning = NULL;
	g_autofree gchar *said = NULL;
	AxisDbConn *conn;
	AxisJobSpec spec;
	ScheduleWait wait;
	gint64 ok;
	guint i;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*job kind 'gone.kind' has no handler*");

	/* Left by a daemon that had the kind: one running with an expired
	 * lease, two queued (one of them the old job of a schedule whose kind
	 * this build changed), and a schedule. */
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	memset(&spec, 0, sizeof(spec));
	spec.kind = "gone.kind";
	spec.queue = "test";
	spec.max_attempts = 1;
	for (i = 0; i < 2; i++)
		g_assert_true(axis_job_enqueue(conn, &spec, NULL, NULL, &error));
	g_assert_true(axis_job_claim(conn, "test", gone_kinds, "old-daemon", 60000, &claim, &error));
	spec.dedupe_key = "schedule:renamed";
	g_assert_true(axis_job_enqueue(conn, &spec, NULL, NULL, &error));
	g_assert_true(axis_job_schedule_upsert(conn, "gone-schedule", "gone.kind", NULL, 1, TRUE, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	upsert_schedule(fx, "renamed", "ok", 1, TRUE);
	axis_test_db_exec(fx->db, "UPDATE job SET lease_expires_at = now() - interval '1 hour' "
		"WHERE state = 'running'");

	start(fx);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*job kind 'gone.kind'*"), ==, 2);
	said = axis_test_log_find(G_LOG_LEVEL_WARNING, "*job kind 'gone.kind'*failed*");
	g_test_message("%s", said);
	g_assert_nonnull(strstr(said, "its 2 queued job(s) are failed, not deleted"));
	warning = axis_test_log_find(G_LOG_LEVEL_WARNING, "*job kind 'gone.kind'*left untouched*");
	g_test_message("%s", warning);
	g_assert_nonnull(strstr(warning, "0 queued and 1 running"));
	g_assert_nonnull(strstr(warning, "'gone-schedule'"));

	/* The renamed schedule fires, its key free again. */
	wait.fx = fx;
	wait.name = "renamed";
	wait.at_least = "1";
	g_assert_true(axis_test_pump_until(schedule_succeeded, &wait, AXIS_TEST_WAIT_MS));

	/* Other work goes on; sweeps and ticks pass; the running one and the
	 * failed ones stay as they are. */
	ok = enqueue_mode(fx, "test.probe", "ok");
	g_assert_true(wait_job(fx, ok, "state", "succeeded"));
	axis_test_pump_for(300);
	untouched = axis_test_db_query(fx->db, "SELECT string_agg(state || '/' || attempts, ' ' "
		"ORDER BY id) FROM job WHERE kind = 'gone.kind'");
	g_assert_cmpstr(untouched, ==, "running/1 failed/0 failed/0");
	g_free(untouched);
	untouched = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind = 'gone.kind' AND state = 'failed' "
		"AND last_error LIKE 'this build of Axis has no handler for its kind, gone.kind:%'");
	g_assert_cmpstr(untouched, ==, "2");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*job kind 'gone.kind'*"), ==, 2);
}

/* Spec: Old finished jobs are pruned (by the built-in scheduled job) */
static void
test_retention(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisModuleRegistry) modules = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *left = NULL;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_module_registry_register(modules, axis_job_builtin_get_module(), NULL, &error));
	g_assert_true(axis_job_registry_add_modules(fx->registry, modules, &error));
	g_assert_no_error(error);

	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, state, max_attempts, attempts, finished_at, last_error) VALUES"
		" ('test.probe', 'test', 'succeeded', 1, 1, now() - interval '8 days', 'a'),"
		" ('test.probe', 'test', 'succeeded', 1, 1, now() - interval '6 days', 'b'),"
		" ('test.probe', 'test', 'cancelled', 1, 0, now() - interval '9 days', 'c'),"
		" ('test.probe', 'test', 'failed', 1, 1, now() - interval '31 days', 'd'),"
		" ('test.probe', 'test', 'failed', 1, 1, now() - interval '8 days', 'e')");
	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, max_attempts, created_at, run_at, last_error) VALUES"
		" ('test.probe', 'test', 1, now() - interval '90 days', now() + interval '1 hour', 'f')");

	start(fx);
	g_assert_true(axis_test_wait_query(fx->db,
		"SELECT state FROM job WHERE kind = '" AXIS_JOB_PRUNE_KIND "'", "succeeded", AXIS_TEST_WAIT_MS));
	left = axis_test_db_query(fx->db, "SELECT string_agg(last_error, '' ORDER BY last_error) "
		"FROM job WHERE kind = 'test.probe'");
	g_assert_cmpstr(left, ==, "bef");

	/* Its default schedule: hourly, next run an hour away. */
	g_free(left);
	left = axis_test_db_query(fx->db, "SELECT run_interval || ' ' || (next_run_at > now() + "
		"interval '3500 seconds') FROM job_schedule WHERE name = '" AXIS_JOB_PRUNE_SCHEDULE "'");
	g_assert_cmpstr(left, ==, "01:00:00 true");
}

/*
 * Another sweeper (not this queue's, which spares its own workers) takes
 * a job whose handler is running: the next heartbeat finds out, the
 * handler is told, and its success is discarded rather than recorded.
 */
static void
test_lease_lost_discarded(Fixture *fx, gconstpointer data)
{
	static const gchar *const probe_kinds[] = { "test.probe", NULL };
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	CountWait wait;
	guint swept = 0;
	guint tries;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lost its lease*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	start(fx);
	id = enqueue_mode(fx, "test.probe", "gate-succeeds");
	wait_started(fx, 1);

	/* The job's keeper may renew the lease between the two statements:
	 * try until the sweep takes it. */
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	for (tries = 0; tries < 100 && swept == 0; tries++) {
		axis_test_db_exec(fx->db, "UPDATE job SET lease_expires_at = now() - interval '1 second' "
			"WHERE state = 'running'");
		g_assert_true(axis_job_sweep_expired_leases(conn, probe_kinds, NULL, &swept, &error));
		g_assert_no_error(error);
	}
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(swept, ==, 1);

	wait.probe = &fx->probe;
	wait.field = &fx->probe.finished;
	wait.at_least = 1;
	g_assert_true(axis_test_pump_until(count_reached, &wait, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(probe_reason(&fx->probe, id), ==, AXIS_JOB_STOP_LEASE_LOST);
	wait_log(G_LOG_LEVEL_MESSAGE, "*succeeded after losing its lease; the result is discarded*", 1);

	/* It runs again, as attempt 2, and that result counts. */
	probe_open_gate(&fx->probe);
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "succeeded/2"));
	g_assert_cmpuint(probe_runs(&fx->probe, id), ==, 2);
}

/*
 * The same, but the job is claimed again here, by the queue's second
 * worker, while the first run is still going: both runs are tracked, and
 * the first is still told that it lost the lease.
 */
static void
test_lease_stolen_and_reclaimed_here(Fixture *fx, gconstpointer data)
{
	static const gchar *const lim_kinds[] = { "test.lim", NULL };
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	guint swept = 0;
	guint tries;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lost its lease*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");

	axis_job_queue_set_concurrency(fx->queue, "lim", 2);
	start(fx);
	id = enqueue_mode(fx, "test.lim", "gate");
	wait_started(fx, 1);

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	for (tries = 0; tries < 100 && swept == 0; tries++) {
		axis_test_db_exec(fx->db, "UPDATE job SET lease_expires_at = now() - interval '1 second' "
			"WHERE state = 'running'");
		g_assert_true(axis_job_sweep_expired_leases(conn, lim_kinds, NULL, &swept, &error));
		g_assert_no_error(error);
	}
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpuint(swept, ==, 1);

	/* The second worker takes it at once; the first run ends when its
	 * keeper finds the lease gone. */
	wait_started(fx, 2);
	g_assert_true(axis_test_wait_query(fx->db, "SELECT state || '/' || attempts FROM job",
		"running/2", AXIS_TEST_WAIT_MS));
	{
		CountWait wait;

		wait.probe = &fx->probe;
		wait.field = &fx->probe.finished;
		wait.at_least = 1;
		g_assert_true(axis_test_pump_until(count_reached, &wait, AXIS_TEST_WAIT_MS));
	}
	wait_log(G_LOG_LEVEL_WARNING, "*job * (test.lim, attempt 1 of 3) lost its lease*", 1);
	wait_log(G_LOG_LEVEL_MESSAGE,
		"*(test.lim, attempt 1 of 3) failed after losing its lease; the result is discarded*", 1);
	assert_job(fx, id, "state || '/' || attempts", "running/2");
	g_assert_cmpuint(fx->probe.timeouts, ==, 0);

	probe_open_gate(&fx->probe);
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "succeeded/2"));

	/* The one overlap there was, which the teardown would object to. */
	g_assert_cmpuint(fx->probe.overlaps, ==, 1);
	fx->probe.overlaps = 0;
}

/* A job running here whose lease ran out (heartbeats failing while the
 * database was away, say) is not taken from under it by this queue's own
 * sweeper: the next heartbeat renews it. */
static void
test_local_expiry_renewed(Fixture *fx, gconstpointer data)
{
	gint64 id;

	if (fx->db == NULL)
		return;

	start(fx);
	id = enqueue_mode(fx, "test.probe", "gate");
	wait_started(fx, 1);
	axis_test_db_exec(fx->db, "UPDATE job SET lease_expires_at = now() - interval '1 second' "
		"WHERE state = 'running'");
	g_assert_true(wait_job(fx, id, "lease_expires_at > now()", "true"));
	axis_test_pump_for(300);
	assert_job(fx, id, "state || '/' || attempts", "running/1");

	probe_open_gate(&fx->probe);
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "succeeded/1"));
	g_assert_cmpuint(probe_runs(&fx->probe, id), ==, 1);
}

/* Spec: Worker disappears (an external worker, at the runner) */
static void
test_orphan_swept(Fixture *fx, gconstpointer data)
{
	static const gchar *const probe_kinds[] = { "test.probe", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobClaim) claim = NULL;
	AxisDbConn *conn;
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker ghost expired*");

	axis_job_queue_set_concurrency(fx->queue, "test", 0);
	start(fx);
	id = enqueue_mode(fx, "test.probe", "ok");
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_true(axis_job_claim(conn, "test", probe_kinds, "ghost", 200, &claim, &error));
	axis_db_pool_release(fx->pool, conn);
	g_assert_nonnull(claim);

	/* The ghost never heartbeats: the queue's sweeper takes the job back. */
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "queued/1"));
	wait_log(G_LOG_LEVEL_WARNING, "*ghost expired*", 1);
}

/* Heartbeats keep a job that runs longer than its lease. */
static void
test_heartbeat_keeps_lease(Fixture *fx, gconstpointer data)
{
	gint64 id;

	if (fx->db == NULL)
		return;

	axis_job_queue_set_lease(fx->queue, 600);
	start(fx);
	id = enqueue_with(fx, "test.probe", "ok", 1300, 0);
	g_assert_true(wait_job(fx, id, "state || '/' || attempts", "succeeded/1"));
	assert_job(fx, id, "finished_at - started_at > interval '1200 milliseconds'", "true");
}

/* The pool must cover every worker and the queue's own two threads. */
static void
test_pool_too_small(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	AxisDbPool *small;

	if (fx->db == NULL)
		return;

	small = axis_test_jobs_pool(fx->db, 6);
	queue = axis_job_queue_new(small, fx->registry);
	g_assert_cmpuint(axis_job_queue_get_total_concurrency(queue), ==, 5);
	g_assert_false(axis_job_queue_start(queue, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CONFIG);
	g_test_message("%s", error->message);
	g_assert_nonnull(strstr(error->message, "has 6 connections"));
	g_assert_nonnull(strstr(error->message, "needs 7"));
	g_clear_error(&error);

	/* Lowering the concurrency is the other way out. */
	g_clear_object(&queue);
	queue = axis_job_queue_new(small, fx->registry);
	axis_job_queue_set_concurrency(queue, "ext", 0);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_queue_stop(queue, 1000));
	g_clear_object(&queue);
	axis_db_pool_free(small);
}

static void
test_start_errors(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	if (fx->db == NULL)
		return;

	axis_job_queue_set_concurrency(fx->queue, "nope", 2);
	g_assert_false(axis_job_queue_start(fx->queue, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "'nope'"));
	g_clear_error(&error);
	g_clear_object(&fx->queue);

	configure(fx);
	start(fx);
	g_assert_false(axis_job_queue_start(fx->queue, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_STATE);
	g_clear_error(&error);

	/* The registry is in use now. */
	{
		AxisJobHandlerInfo info;

		memset(&info, 0, sizeof(info));
		info.kind = "test.late";
		info.queue = "test";
		info.handler = probe_handler;
		g_assert_false(axis_job_registry_add(fx->registry, &info, NULL, &error));
		g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_STATE);
	}
}

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	alarm(240);

	ADD("/job/queue/durable-across-restart", test_durable_across_restart);
	ADD("/job/queue/unknown-kind-refused", test_unknown_kind_refused);
	ADD("/job/queue/concurrency-limit", test_concurrency_limit);
	ADD("/job/queue/queues-independent", test_queues_independent);
	ADD("/job/queue/external-queue", test_external_queue);
	ADD("/job/queue/progress-visible", test_progress_visible);
	ADD("/job/queue/progress-throttled", test_progress_throttled);
	ADD("/job/queue/event-order", test_event_order);
	ADD("/job/queue/cancel/running", test_cancel_running);
	ADD("/job/queue/cancel/running-elsewhere", test_cancel_running_elsewhere);
	ADD("/job/queue/cancel/queued", test_cancel_queued);
	ADD("/job/queue/handler-failures", test_handler_failures);
	ADD("/job/queue/schedule/fires", test_schedule_fires);
	ADD("/job/queue/schedule/no-overlap", test_schedule_no_overlap);
	ADD("/job/queue/schedule/no-burst", test_schedule_no_burst);
	ADD("/job/queue/schedule/disabled", test_schedule_disabled);
	ADD("/job/queue/schedule/after-the-clock-went-back", test_schedule_after_the_clock_went_back);
	ADD("/job/queue/schedule/follows-the-build", test_schedule_follows_the_build);
	ADD("/job/queue/schedule/dropped-by-the-build", test_schedule_dropped_by_the_build);
	ADD("/job/queue/schedule/of-a-person-is-kept", test_schedule_of_a_person_is_kept);
	ADD("/job/queue/schedule/switched-off-by-hand", test_schedule_switched_off_by_hand);
	ADD("/job/queue/start/jobs-moved-to-their-kinds-queue", test_jobs_moved_to_their_kinds_queue);
	ADD("/job/queue/payload-naming-nothing", test_payload_naming_nothing);
	ADD("/job/queue/shutdown/within-grace", test_shutdown_within_grace);
	ADD("/job/queue/shutdown/beyond-grace", test_shutdown_beyond_grace);
	ADD("/job/queue/shutdown/hands-back", test_shutdown_hands_back);
	ADD("/job/queue/missing-handler", test_missing_handler);
	ADD("/job/queue/retention", test_retention);
	ADD("/job/queue/lease/lost-result-discarded", test_lease_lost_discarded);
	ADD("/job/queue/lease/stolen-and-reclaimed-here", test_lease_stolen_and_reclaimed_here);
	ADD("/job/queue/lease/local-expiry-renewed", test_local_expiry_renewed);
	ADD("/job/queue/lease/orphan-swept", test_orphan_swept);
	ADD("/job/queue/lease/heartbeat-keeps-lease", test_heartbeat_keeps_lease);
	ADD("/job/queue/start/pool-too-small", test_pool_too_small);
	ADD("/job/queue/start/errors", test_start_errors);

	return g_test_run();
}
