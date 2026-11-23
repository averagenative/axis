/*
 * test-job-stress.c - thousands of jobs through the runner at once
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three queues (3, 2 and 4 workers) and AXIS_STRESS_JOBS jobs (default
 * 3000) of every behaviour mixed: quick successes, failures retried until
 * they succeed or run out of attempts, permanent failures, jobs cancelled
 * while queued or while running (some racing their own completion), jobs
 * whose lease another sweeper takes from under them, and, all the while,
 * leases of random running jobs made to look expired.  The random seed is
 * GTest's (rerun with --seed to reproduce).
 *
 * At the end: every job is in exactly one final state and announced it
 * with exactly one final event; attempts never exceed the maximum; every
 * claim ran the handler exactly once; each job's outcome is one its
 * behaviour allows; and no job ever ran twice at once, except where the
 * earlier run's lease had been taken by the other sweeper, in which case
 * that earlier run was told so and its result was discarded.
 */

#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"

#define MAX_ATTEMPTS 3
#define CHUNK 100

typedef enum
{
	MODE_OK,
	MODE_FLAKY,
	MODE_PERMANENT,
	MODE_CANCEL_TARGET,
	MODE_RACE_CANCEL,
	MODE_LOSE_LEASE
} Mode;

static const gchar *const mode_names[] = {
	"ok", "flaky", "permanent", "until-cancelled", "race-cancel", "lose-lease"
};

/* One run of a handler. */
typedef struct
{
	gint64            id;
	guint             attempt;
	gboolean          returned;
	gboolean          overlapped;	/* a later run of the job began during it */
	AxisJobStopReason reason;
} Run;

typedef struct
{
	GMutex      lock;
	GHashTable *active;	/* id -> GPtrArray of Run going on */
	GPtrArray  *runs;	/* every Run */
	GHashTable *starts;	/* id -> runs started */
	guint       timeouts;
	guint       max_attempt;
} Probe;

typedef struct
{
	gint64               id;
	Mode                 mode;
	guint                fail_until;
	gboolean             cancel_sent;
	AxisJobCancelOutcome cancel_outcome;
	guint                final_events;
	AxisJobState         final_event_state;
} Job;

typedef struct
{
	AxisTestDb      *db;
	AxisDbPool      *pool;
	AxisJobRegistry *registry;
	AxisJobQueue    *queue;
	Probe            probe;
	GHashTable      *jobs;		/* id -> Job */
	GThread         *main_thread;
	guint            off_thread_events;
	guint            events;
} Fixture;

static const struct
{
	const gchar *kind;
	const gchar *queue;
	guint        concurrency;
} kinds[] = {
	{ "s.a", "qa", 3 },
	{ "s.b", "qb", 2 },
	{ "s.c", "qc", 4 },
};

static guint
probe_starts(
	Probe  *probe,
	gint64  id
){
	return GPOINTER_TO_UINT(g_hash_table_lookup(probe->starts, &id));
}

/* Takes the job's lease away as another sweeper would: makes it expired
 * and sweeps without sparing anyone.  The job's own keeper may renew the
 * lease between the two statements, so it tries until the job is no
 * longer running as this attempt. */
static void
steal_own_lease(AxisJobContext *context)
{
	const gchar *job_kinds[2];
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, axis_job_context_get_id(context));
	g_autofree gchar *attempt = g_strdup_printf("%u", axis_job_context_get_attempt(context));
	const gchar *params[2];
	AxisDbPool *pool = axis_job_context_get_pool(context);
	AxisDbConn *conn;
	guint tries;

	job_kinds[0] = axis_job_context_get_kind(context);
	job_kinds[1] = NULL;
	params[0] = id;
	params[1] = attempt;
	conn = axis_db_pool_acquire(pool, &error);
	g_assert_no_error(error);
	for (tries = 0; tries < 100; tries++) {
		g_autoptr(AxisDbResult) still = NULL;

		g_assert_true(axis_db_conn_exec(conn,
			"UPDATE job SET lease_expires_at = now() - interval '1 second' "
			"WHERE id = $1::bigint AND state = 'running' AND attempts = $2::integer",
			2, params, NULL, &error));
		g_assert_true(axis_job_sweep_expired_leases(conn, job_kinds, NULL, NULL, &error));
		g_assert_true(axis_db_conn_exec(conn,
			"SELECT 1 FROM job WHERE id = $1::bigint AND state = 'running' AND attempts = $2::integer",
			2, params, &still, &error));
		g_assert_no_error(error);
		if (axis_db_result_get_n_rows(still) == 0)
			break;
	}
	g_assert_cmpuint(tries, <, 100);
	axis_db_pool_release(pool, conn);
}

static gboolean
stress_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	Probe *probe = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *mode = json_object_get_string_member(payload, "mode");
	gint64 id = axis_job_context_get_id(context);
	guint attempt = axis_job_context_get_attempt(context);
	GPtrArray *active;
	gboolean ok = TRUE;
	Run *run;
	guint i;

	run = g_new0(Run, 1);
	run->id = id;
	run->attempt = attempt;

	g_mutex_lock(&probe->lock);
	active = g_hash_table_lookup(probe->active, &id);
	if (active == NULL) {
		gint64 *key = g_new(gint64, 1);

		*key = id;
		active = g_ptr_array_new();
		g_hash_table_insert(probe->active, key, active);
	}
	for (i = 0; i < active->len; i++)
		((Run *)g_ptr_array_index(active, i))->overlapped = TRUE;
	g_ptr_array_add(active, run);
	g_ptr_array_add(probe->runs, run);
	{
		gint64 *key = g_new(gint64, 1);

		*key = id;
		g_hash_table_replace(probe->starts, key, GUINT_TO_POINTER(probe_starts(probe, id) + 1));
	}
	probe->max_attempt = MAX(probe->max_attempt, attempt);
	g_mutex_unlock(&probe->lock);

	if (strcmp(mode, "ok") == 0 || strcmp(mode, "race-cancel") == 0) {
		g_usleep((gulong)json_object_get_int_member(payload, "sleep_us"));
	} else if (strcmp(mode, "flaky") == 0) {
		if (attempt < (guint)json_object_get_int_member(payload, "fail_until")) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "flaky failure");
			ok = FALSE;
		}
	} else if (strcmp(mode, "permanent") == 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "permanent failure");
		ok = FALSE;
	} else if (strcmp(mode, "until-cancelled") == 0) {
		if (axis_job_context_wait(context, 5000)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "cancelled");
			ok = FALSE;
		} else {
			g_mutex_lock(&probe->lock);
			probe->timeouts++;
			g_mutex_unlock(&probe->lock);
			g_test_message("timeout: job %" G_GINT64_FORMAT " until-cancelled attempt %u", id, attempt);
		}
	} else if (strcmp(mode, "lose-lease") == 0) {
		if (attempt == 1) {
			steal_own_lease(context);
			/* The keeper finds out at its next heartbeat. */
			if (!axis_job_context_wait(context, 5000)) {
				g_mutex_lock(&probe->lock);
				probe->timeouts++;
				g_mutex_unlock(&probe->lock);
				g_test_message("timeout: job %" G_GINT64_FORMAT " lose-lease attempt %u", id, attempt);
			}
		}
	} else {
		g_error("unknown mode %s", mode);
	}

	g_mutex_lock(&probe->lock);
	run->returned = TRUE;
	run->reason = axis_job_context_get_stop_reason(context);
	g_ptr_array_remove(active, run);
	g_mutex_unlock(&probe->lock);

	return ok;
}

static void
on_job_event(
	AxisJobQueue *queue,
	AxisJobEvent *event,
	gpointer      user_data
){
	Fixture *fx = user_data;
	Job *job;

	if (g_thread_self() != fx->main_thread)
		fx->off_thread_events++;
	fx->events++;

	if (event->type != AXIS_JOB_EVENT_STATE)
		return;
	if (event->state != AXIS_JOB_STATE_SUCCEEDED && event->state != AXIS_JOB_STATE_FAILED &&
	    event->state != AXIS_JOB_STATE_CANCELLED)
		return;

	job = g_hash_table_lookup(fx->jobs, &event->id);
	g_assert_nonnull(job);
	job->final_events++;
	job->final_event_state = event->state;
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

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*lost its lease*");

	fx->pool = axis_test_jobs_pool(fx->db, 16);
	g_mutex_init(&fx->probe.lock);
	fx->probe.active = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)g_ptr_array_unref);
	fx->probe.runs = g_ptr_array_new_with_free_func(g_free);
	fx->probe.starts = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	fx->jobs = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, g_free);

	fx->registry = axis_job_registry_new();
	for (i = 0; i < G_N_ELEMENTS(kinds); i++) {
		AxisJobHandlerInfo info;

		memset(&info, 0, sizeof(info));
		info.kind = kinds[i].kind;
		info.queue = kinds[i].queue;
		info.max_attempts = MAX_ATTEMPTS;
		info.handler = stress_handler;
		g_assert_true(axis_job_registry_add(fx->registry, &info, &fx->probe, &error));
		g_assert_no_error(error);
	}

	fx->queue = axis_job_queue_new(fx->pool, fx->registry);
	for (i = 0; i < G_N_ELEMENTS(kinds); i++)
		axis_job_queue_set_concurrency(fx->queue, kinds[i].queue, kinds[i].concurrency);
	axis_job_queue_set_poll_interval(fx->queue, 100);
	axis_job_queue_set_lease(fx->queue, 450);
	axis_job_queue_set_sweep_interval(fx->queue, 50);
	axis_job_queue_set_backoff(fx->queue, 5, 20);
	g_signal_connect(fx->queue, "job-event", G_CALLBACK(on_job_event), fx);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}

	axis_job_queue_stop(fx->queue, 5000);
	g_assert_true(axis_job_queue_wait_threads(fx->queue, 10000));
	g_clear_object(&fx->queue);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_job_registry_unref(fx->registry);
	g_hash_table_unref(fx->jobs);
	g_hash_table_unref(fx->probe.active);
	g_ptr_array_unref(fx->probe.runs);
	g_hash_table_unref(fx->probe.starts);
	g_mutex_clear(&fx->probe.lock);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_log_reset();
}

static Mode
pick_mode(void)
{
	gint32 r = g_test_rand_int_range(0, 100);

	if (r < 55)
		return MODE_OK;
	if (r < 70)
		return MODE_FLAKY;
	if (r < 75)
		return MODE_PERMANENT;
	if (r < 85)
		return MODE_CANCEL_TARGET;
	if (r < 98)
		return MODE_RACE_CANCEL;
	return MODE_LOSE_LEASE;
}

static void
enqueue_one(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	Job *job = g_new0(Job, 1);
	const gchar *kind = kinds[g_test_rand_int_range(0, G_N_ELEMENTS(kinds))].kind;

	job->mode = pick_mode();
	json_object_set_string_member(payload, "mode", mode_names[job->mode]);
	if (job->mode == MODE_OK || job->mode == MODE_RACE_CANCEL)
		json_object_set_int_member(payload, "sleep_us", g_test_rand_int_range(0, 1500));
	if (job->mode == MODE_FLAKY) {
		job->fail_until = (guint)g_test_rand_int_range(2, MAX_ATTEMPTS + 2);
		json_object_set_int_member(payload, "fail_until", job->fail_until);
	}

	g_assert_true(axis_job_queue_enqueue(fx->queue, kind, payload, NULL, &job->id, NULL, &error));
	g_assert_no_error(error);
	g_hash_table_insert(fx->jobs, &job->id, job);
}

static void
cancel_job(
	Fixture *fx,
	Job     *job
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_job_queue_cancel(fx->queue, job->id, &job->cancel_outcome, &error));
	g_assert_no_error(error);
	job->cancel_sent = TRUE;
}

/* Cancels every target not yet cancelled, and each racing job with a
 * chance of one in four. */
static void
send_cancels(Fixture *fx)
{
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, fx->jobs);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Job *job = value;

		if (job->cancel_sent)
			continue;
		if (job->mode == MODE_CANCEL_TARGET ||
		    (job->mode == MODE_RACE_CANCEL && g_test_rand_int_range(0, 4) == 0))
			cancel_job(fx, job);
	}
}

/* Makes the leases of two random running jobs look expired.  This queue's
 * own sweeper must leave them alone (their workers are busy here), and
 * their next heartbeat renews them. */
static void
expire_random_leases(Fixture *fx)
{
	g_free(axis_test_db_query(fx->db,
		"UPDATE job SET lease_expires_at = now() - interval '1 second' WHERE state = 'running' "
		"AND id IN (SELECT id FROM job WHERE state = 'running' ORDER BY random() LIMIT 2) "
		"RETURNING 1"));
}

static gboolean
all_finished(gpointer data)
{
	Fixture *fx = data;
	g_autofree gchar *left = axis_test_db_query(fx->db,
		"SELECT count(*) FROM job WHERE state IN ('queued', 'running')");

	return g_strcmp0(left, "0") == 0;
}

static void
check_outcome(
	Fixture       *fx,
	Job           *job,
	AxisJobState   state,
	guint          attempts,
	const gchar   *last_error
){
	gboolean lease_failure = state == AXIS_JOB_STATE_FAILED && last_error != NULL &&
		strstr(last_error, "expired") != NULL;
	gboolean ok;

	switch (job->mode) {
	case MODE_OK:
		ok = state == AXIS_JOB_STATE_SUCCEEDED || lease_failure;
		break;
	case MODE_FLAKY:
		if (job->fail_until <= MAX_ATTEMPTS)
			ok = (state == AXIS_JOB_STATE_SUCCEEDED && attempts >= job->fail_until) || lease_failure;
		else
			ok = state == AXIS_JOB_STATE_FAILED && attempts == MAX_ATTEMPTS;
		break;
	case MODE_PERMANENT:
		ok = state == AXIS_JOB_STATE_FAILED;
		break;
	case MODE_CANCEL_TARGET:
		/* (A lease taken by the other sweeper on every attempt would fail
		 * it before the cancel; that is allowed too.) */
		ok = (state == AXIS_JOB_STATE_CANCELLED && job->cancel_sent &&
			job->cancel_outcome != AXIS_JOB_CANCEL_TOO_LATE) || lease_failure;
		break;
	case MODE_RACE_CANCEL:
		if (!job->cancel_sent)
			ok = state == AXIS_JOB_STATE_SUCCEEDED || lease_failure;
		else if (job->cancel_outcome == AXIS_JOB_CANCEL_DONE)
			ok = state == AXIS_JOB_STATE_CANCELLED;
		else if (job->cancel_outcome == AXIS_JOB_CANCEL_TOO_LATE)
			ok = state == AXIS_JOB_STATE_SUCCEEDED || lease_failure;
		else
			ok = state == AXIS_JOB_STATE_SUCCEEDED || state == AXIS_JOB_STATE_CANCELLED;
		break;
	case MODE_LOSE_LEASE:
	default:
		ok = (state == AXIS_JOB_STATE_SUCCEEDED && attempts >= 2) || lease_failure;
		break;
	}

	if (!ok)
		g_error("job %" G_GINT64_FORMAT " (%s, fail_until %u, cancel %s/%d) ended %s after %u "
			"attempt(s) (%s)", job->id, mode_names[job->mode], job->fail_until,
			job->cancel_sent ? "sent" : "not sent", job->cancel_outcome,
			axis_job_state_to_string(state), attempts, last_error != NULL ? last_error : "no error");
}

static void
test_stress(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *env = g_getenv("AXIS_STRESS_JOBS");
	guint n_jobs = env != NULL ? (guint)g_ascii_strtoull(env, NULL, 10) : 3000;
	guint lease_lost_runs = 0;
	guint overlaps = 0;
	guint expiries = 0;
	guint cancels = 0;
	guint by_state[5] = { 0, 0, 0, 0, 0 };
	gint64 began = g_get_monotonic_time();
	gint64 deadline;
	AxisDbConn *conn;
	guint enqueued = 0;
	guint i;

	if (fx->db == NULL)
		return;

	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);

	/* Enqueue in chunks, cancelling and expiring as it goes, so that
	 * cancels find jobs both queued and running. */
	while (enqueued < n_jobs) {
		for (i = 0; i < CHUNK && enqueued < n_jobs; i++, enqueued++)
			enqueue_one(fx);
		send_cancels(fx);
		expire_random_leases(fx);
		expiries++;
		while (g_main_context_iteration(NULL, FALSE))
			;
	}

	deadline = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
	while (!all_finished(fx)) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		send_cancels(fx);
		expire_random_leases(fx);
		expiries++;
		axis_test_pump_for(20);
	}
	g_test_message("%u jobs finished in %.1f s", n_jobs,
		(g_get_monotonic_time() - began) / (gdouble)G_USEC_PER_SEC);

	/* Stopping delivers the last events. */
	g_assert_true(axis_job_queue_stop(fx->queue, 5000));
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_cmpuint(axis_db_pool_get_n_in_use(fx->pool), ==, 0);
	g_assert_cmpuint(fx->off_thread_events, ==, 0);
	g_assert_cmpuint(fx->probe.timeouts, ==, 0);
	g_assert_cmpuint(fx->probe.max_attempt, <=, MAX_ATTEMPTS);

	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_exec(conn, "SELECT id, state::text, attempts, max_attempts, last_error "
		"FROM job ORDER BY id", 0, NULL, &result, &error));
	axis_db_pool_release(fx->pool, conn);
	g_assert_no_error(error);
	g_assert_cmpuint(axis_db_result_get_n_rows(result), ==, n_jobs);

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		AxisJobState state = AXIS_JOB_STATE_QUEUED;
		gint64 id = 0;
		gint64 attempts = 0;
		gint64 max_attempts = 0;
		Job *job;

		g_assert_true(axis_db_result_get_int64(result, i, 0, &id));
		g_assert_true(axis_job_state_from_string(axis_db_result_get_text(result, i, 1), &state));
		g_assert_true(axis_db_result_get_int64(result, i, 2, &attempts));
		g_assert_true(axis_db_result_get_int64(result, i, 3, &max_attempts));
		job = g_hash_table_lookup(fx->jobs, &id);
		g_assert_nonnull(job);
		by_state[state]++;
		if (job->cancel_sent)
			cancels++;

		/* One final state, announced once. */
		g_assert_true(state == AXIS_JOB_STATE_SUCCEEDED || state == AXIS_JOB_STATE_FAILED ||
			state == AXIS_JOB_STATE_CANCELLED);
		if (job->final_events != 1 || job->final_event_state != state)
			g_error("job %" G_GINT64_FORMAT " is %s but had %u final event(s), the last %s", id,
				axis_job_state_to_string(state), job->final_events,
				axis_job_state_to_string(job->final_event_state));

		/* Never more attempts than allowed; every claim ran the handler
		 * exactly once. */
		g_assert_cmpint(attempts, <=, max_attempts);
		g_assert_cmpint(max_attempts, ==, MAX_ATTEMPTS);
		if (probe_starts(&fx->probe, id) != (guint)attempts)
			g_error("job %" G_GINT64_FORMAT " (%s): %u handler runs for %" G_GINT64_FORMAT
				" attempts", id, mode_names[job->mode], probe_starts(&fx->probe, id), attempts);

		check_outcome(fx, job, state, (guint)attempts, axis_db_result_get_text(result, i, 4));
	}

	/* No job ran twice at once, unless the earlier run's lease had been
	 * taken (by the other sweeper); and then that earlier run's result was
	 * discarded, whether its keeper told it in time (the handler stopped
	 * with AXIS_JOB_STOP_LEASE_LOST) or it finished first (the result was
	 * refused at the database).  The queue logs each discard by attempt. */
	for (i = 0; i < fx->probe.runs->len; i++) {
		Run *run = g_ptr_array_index(fx->probe.runs, i);
		g_autofree gchar *discarded = NULL;
		Job *job;

		g_assert_true(run->returned);
		if (run->reason == AXIS_JOB_STOP_LEASE_LOST)
			lease_lost_runs++;
		if (!run->overlapped)
			continue;
		overlaps++;
		job = g_hash_table_lookup(fx->jobs, &run->id);
		discarded = g_strdup_printf("job %" G_GINT64_FORMAT " (*, attempt %u of %d) *lease*"
			"discarded*", run->id, run->attempt, MAX_ATTEMPTS);
		if (axis_test_log_count(G_LOG_LEVEL_MESSAGE, discarded) != 1)
			g_error("job %" G_GINT64_FORMAT " (%s) attempt %u was still running when a later "
				"attempt began, but its result was not discarded as a lost lease's (stop reason "
				"%d)", run->id, mode_names[job->mode], run->attempt, run->reason);
	}

	g_test_message("%u jobs: %u succeeded, %u failed, %u cancelled; %u handler runs, %u of them "
		"after losing their lease, %u overlapping a later run; %u cancels sent; %u rounds of "
		"lease expiry; %u events", n_jobs, by_state[AXIS_JOB_STATE_SUCCEEDED],
		by_state[AXIS_JOB_STATE_FAILED], by_state[AXIS_JOB_STATE_CANCELLED],
		fx->probe.runs->len, lease_lost_runs, overlaps, cancels, expiries, fx->events);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	alarm(240);

	g_test_add("/job/stress", Fixture, NULL, fixture_setup, test_stress, fixture_teardown);

	return g_test_run();
}
