/*
 * test-compat-jobs.c - the compat jobs on the real job queue: they run on a
 * queue of their own, fail softly with a cleaned error, and leave the token
 * out of the job table; a hung media server delays no other queue
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "axis-test-compat.h"
#include "axis-test-jobs.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-web-compat.h"

typedef struct
{
	AxisTestCompat *t;
	AxisWebCompat  *web;
	AxisJobQueue   *queue;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->t == NULL) return; } while (0)

static gboolean
noop_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return TRUE;
}

static const AxisJobHandlerInfo noop_info = {
	"test.noop", AXIS_JOB_MAINTENANCE_QUEUE, 1, noop_handler, "Doing nothing", AXIS_JOB_CLASS_UPKEEP
};

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	const AxisModuleInfo *module = axis_web_compat_get_module();
	guint i;

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	fx->t = axis_test_compat_new(FALSE);
	if (fx->t == NULL)
		return;
	axis_test_compat_connect(fx->t);
	fx->web = axis_web_compat_new(fx->t->compat);
	for (i = 0; module->job_handlers[i].kind != NULL; i++)
		g_assert_true(axis_job_registry_add(registry, &module->job_handlers[i], fx->web, &error));
	g_assert_true(axis_job_registry_add(registry, &noop_info, NULL, &error));
	fx->queue = axis_job_queue_new(fx->t->lib->pool, registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t != NULL) {
		g_assert_true(axis_job_queue_stop(fx->queue, 5000));
		g_object_unref(fx->queue);
		axis_web_compat_free(fx->web);
	}
	axis_test_compat_free(fx->t);
	axis_test_log_reset();
}

static gint64
request(
	Fixture  *fx,
	gboolean  history
){
	gint64 id = 0;
	g_autoptr(GError) error = NULL;

	g_assert_true(history ? axis_compat_request_history(fx->t->compat, &id, &error) :
		axis_compat_request_poll(fx->t->compat, &id, &error));
	g_assert_no_error(error);
	axis_job_queue_wake(fx->queue);
	return id;
}

static gboolean
reached(
	Fixture     *fx,
	gint64       id,
	const gchar *state,
	guint        timeout_ms
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_wait_query(fx->t->lib->db, sql, state, timeout_ms);
}

static void
assert_token_nowhere(Fixture *fx)
{
	g_assert_cmpint(axis_test_compat_count(fx->t, "job j", "to_jsonb(j)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state s", "to_jsonb(s)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
}

static void
test_the_module_declares_its_own_queue_and_schedules(Fixture *fx, gconstpointer data)
{
	const AxisModuleInfo *module = axis_web_compat_get_module();
	guint i;

	g_assert_cmpstr(module->id, ==, "compat");
	for (i = 0; module->job_handlers[i].kind != NULL; i++)
		g_assert_cmpstr(module->job_handlers[i].queue, ==, AXIS_COMPAT_QUEUE);
	g_assert_cmpuint(i, ==, 3);
	g_assert_cmpstr(module->job_schedules[0].kind, ==, AXIS_COMPAT_POLL_KIND);
	g_assert_cmpuint(module->job_schedules[0].interval_seconds, ==, 30);
	g_assert_cmpstr(module->job_schedules[1].kind, ==, AXIS_COMPAT_HISTORY_KIND);
	g_assert_cmpuint(module->job_schedules[1].interval_seconds, ==, 3600);
	/* Telling the media servers which folder changed: upkeep, on the same
	 * queue (no queue of its own), with a plain label. */
	g_assert_cmpstr(module->job_handlers[2].kind, ==, AXIS_COMPAT_NOTIFY_KIND);
	g_assert_cmpint(module->job_handlers[2].job_class, ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_cmpstr(module->job_handlers[2].label, ==, "Telling the media servers which folders changed");
	g_assert_cmpstr(module->job_schedules[2].kind, ==, AXIS_COMPAT_NOTIFY_KIND);
	g_assert_cmpuint(module->job_schedules[2].interval_seconds, ==, AXIS_COMPAT_NOTIFY_INTERVAL_SECONDS);
	/* The queue is not the one the Analysis and Savings refreshes hold. */
	g_assert_cmpstr(AXIS_COMPAT_QUEUE, !=, AXIS_JOB_MAINTENANCE_QUEUE);
}

/* Spec: Token stays secret (job rows) / Session recorded once */
static void
test_a_poll_job_runs_and_the_token_is_not_in_the_job_table(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *queue = NULL;

	SKIP_WITHOUT_DB(fx);
	id = request(fx, FALSE);
	g_assert_true(reached(fx, id, "succeeded", AXIS_TEST_WAIT_MS));
	queue = axis_test_db_queryf(fx->t->lib->db, "SELECT queue FROM job WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(queue, ==, "compat");
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "last_poll_ok_at IS NOT NULL"), ==, 1);

	id = request(fx, TRUE);
	g_assert_true(reached(fx, id, "succeeded", AXIS_TEST_WAIT_MS));
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "history_at IS NOT NULL"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_play", NULL), ==, 4);
	assert_token_nowhere(fx);
}

/* A failing server: the job fails with a cleaned message; nothing else is affected. */
static void
test_a_failing_server_fails_the_job_softly_without_the_token(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *error = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	id = request(fx, FALSE);
	g_assert_true(reached(fx, id, "failed", AXIS_TEST_WAIT_MS));
	error = axis_test_db_queryf(fx->t->lib->db, "SELECT last_error FROM job WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(error, "token was refused"));
	g_assert_null(strstr(error, AXIS_TEST_COMPAT_TOKEN));
	/* The failure is recorded for the page, with the back-off. */
	g_assert_cmpint(axis_test_compat_count(fx->t, "compat_state", "failures = 1 AND last_error IS NOT NULL"), ==, 1);
	assert_token_nowhere(fx);
}

/* A server that never answers holds its own queue, and no other. */
static void
test_a_hung_server_delays_no_other_queue(Fixture *fx, gconstpointer data)
{
	AxisCompatTuning tuning;
	AxisJobSpec spec;
	gint64 hung;
	gint64 noop = 0;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *message = NULL;
	gint64 start;

	SKIP_WITHOUT_DB(fx);
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	axis_compat_set_tuning(fx->t->compat, &tuning);
	axis_test_plex_route(fx->t->plex, "/identity", -1, AXIS_TEST_PLEX_HANG, 200, NULL, 0);
	hung = request(fx, FALSE);
	g_assert_true(reached(fx, hung, "running", AXIS_TEST_WAIT_MS));

	memset(&spec, 0, sizeof(spec));
	spec.kind = "test.noop";
	spec.queue = AXIS_JOB_MAINTENANCE_QUEUE;
	spec.max_attempts = 1;
	{
		AxisDbConn *conn = axis_db_pool_acquire(fx->t->lib->pool, &error);

		g_assert_nonnull(conn);
		g_assert_true(axis_job_enqueue(conn, &spec, &noop, NULL, &error));
		axis_db_pool_release(fx->t->lib->pool, conn);
	}
	axis_job_queue_wake(fx->queue);
	start = g_get_monotonic_time();
	g_assert_true(reached(fx, noop, "succeeded", 2000));
	g_assert_cmpint(g_get_monotonic_time() - start, <, 2 * G_USEC_PER_SEC);
	/* The hung poll is still waiting for its deadline. */
	g_assert_true(reached(fx, hung, "running", 10));

	g_assert_true(reached(fx, hung, "failed", AXIS_TEST_WAIT_MS));
	message = axis_test_db_queryf(fx->t->lib->db, "SELECT last_error FROM job WHERE id = %" G_GINT64_FORMAT, hung);
	g_assert_nonnull(strstr(message, "in time"));
	assert_token_nowhere(fx);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(name, func) g_test_add("/compat-jobs/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)
	ADD("module", test_the_module_declares_its_own_queue_and_schedules);
	ADD("poll-and-history", test_a_poll_job_runs_and_the_token_is_not_in_the_job_table);
	ADD("failure", test_a_failing_server_fails_the_job_softly_without_the_token);
	ADD("hung", test_a_hung_server_delays_no_other_queue);
#undef ADD
	return g_test_run();
}
