/*
 * test-modules.c - module switches: dependencies, hooks, persistence and
 * what the job queue does for a module that is off
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The registry tests need no database.  The job tests run a real queue
 * against a throwaway PostgreSQL with two modules, one of which is
 * switched off and on while jobs are queued, running and scheduled.
 */

#include <glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "core/axis-error.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "core/axis-settings.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"

/* ── Registry ───────────────────────────────────────────────────── */

typedef struct
{
	GString  *events;	/* "start:<id>;" and "stop:<id>;" */
	gchar    *fail_start;	/* the id whose start fails */
} Hooks;

typedef struct
{
	Hooks       *hooks;
	const gchar *id;
} Ctx;

static gboolean
hook_start(gpointer user_data, GError **error)
{
	Ctx *ctx = user_data;

	g_string_append_printf(ctx->hooks->events, "start:%s;", ctx->id);
	if (g_strcmp0(ctx->hooks->fail_start, ctx->id) == 0) {
		g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_FAILED, "no luck");
		return FALSE;
	}

	return TRUE;
}

static void
hook_stop(gpointer user_data)
{
	Ctx *ctx = user_data;

	g_string_append_printf(ctx->hooks->events, "stop:%s;", ctx->id);
}

static const gchar *const needs_core[] = { "core", NULL };
static const gchar *const needs_media[] = { "media", NULL };
static const gchar *const needs_missing[] = { "missing", NULL };
static const gchar *const needs_itself[] = { "loop", NULL };
static const gchar *const needs_movies[] = { "movies", NULL };

static gboolean
add(
	AxisModuleRegistry *registry,
	Ctx                *ctx,
	guint               flags,
	const gchar *const *depends_on,
	GError            **error
){
	AxisModuleInfo info;

	memset(&info, 0, sizeof(info));
	info.id = ctx->id;
	info.name = ctx->id;
	info.start = hook_start;
	info.stop = hook_stop;
	info.flags = flags;
	info.depends_on = depends_on;

	return axis_module_registry_register(registry, &info, ctx, error);
}

typedef struct
{
	Hooks               hooks;
	AxisModuleRegistry *registry;
	Ctx                 core;
	Ctx                 media;
	Ctx                 movies;
	Ctx                 music;
} Reg;

/* core (required) <- media <- movies, and music (needs core, off by default) */
static void
reg_setup(Reg *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->hooks.events = g_string_new(NULL);
	fx->registry = axis_module_registry_new();
	fx->core.hooks = fx->media.hooks = fx->movies.hooks = fx->music.hooks = &fx->hooks;
	fx->core.id = "core";
	fx->media.id = "media";
	fx->movies.id = "movies";
	fx->music.id = "music";

	g_assert_true(add(fx->registry, &fx->core, AXIS_MODULE_REQUIRED, NULL, &error));
	g_assert_true(add(fx->registry, &fx->media, 0, needs_core, &error));
	g_assert_true(add(fx->registry, &fx->movies, 0, needs_media, &error));
	g_assert_true(add(fx->registry, &fx->music, AXIS_MODULE_DISABLED_BY_DEFAULT, needs_core, &error));
	g_assert_no_error(error);
}

static void
reg_teardown(Reg *fx, gconstpointer data)
{
	axis_module_registry_free(fx->registry);
	g_string_free(fx->hooks.events, TRUE);
	g_free(fx->hooks.fail_start);
	axis_test_log_reset();
}

static void
assert_events(Reg *fx, const gchar *expected)
{
	g_assert_cmpstr(fx->hooks.events->str, ==, expected);
	g_string_truncate(fx->hooks.events, 0);
}

/* Spec: Unknown or cyclic dependency */
static void
test_dependencies_are_checked_at_registration(Reg *fx, gconstpointer data)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Ctx a = { &fx->hooks, "alpha" };
	Ctx loop = { &fx->hooks, "loop" };
	Ctx base = { &fx->hooks, "core" };
	Ctx req = { &fx->hooks, "req" };

	/* Unknown: the message names both modules. */
	g_assert_false(add(registry, &a, 0, needs_missing, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_nonnull(strstr(error->message, "alpha"));
	g_assert_nonnull(strstr(error->message, "missing"));
	g_clear_error(&error);
	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 0);

	/* A module that needs itself is a cycle of one. */
	g_assert_false(add(registry, &loop, 0, needs_itself, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_nonnull(strstr(error->message, "loop"));
	g_clear_error(&error);

	/* Longer cycles need a forward reference, which is the unknown case:
	 * "beta" needs "alpha" before "alpha" exists. */
	g_assert_false(add(registry, &fx->media, 0, needs_movies, &error));
	g_assert_nonnull(strstr(error->message, "movies"));
	g_clear_error(&error);

	/* A required module may not be off by default, nor need an optional one. */
	g_assert_true(add(registry, &base, 0, NULL, &error));
	g_assert_false(add(registry, &req, AXIS_MODULE_REQUIRED, needs_core, &error));
	g_assert_nonnull(strstr(error->message, "req"));
	g_clear_error(&error);
	g_assert_false(add(registry, &req, AXIS_MODULE_REQUIRED | AXIS_MODULE_DISABLED_BY_DEFAULT, NULL, &error));
	g_clear_error(&error);
	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 1);
}

/* Spec: Required module; Enable blocked; Disable blocked */
static void
test_dependency_rules_both_ways(Reg *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	/* Starting state: core, media and movies on; music off. */
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "movies"));
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "music"));
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "nothing"));

	/* Required: refused, stays on. */
	g_assert_false(axis_module_registry_set_enabled(fx->registry, "core", FALSE, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_nonnull(strstr(error->message, "required"));
	g_clear_error(&error);
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "core"));

	/* Disable blocked: names the dependent. */
	g_assert_false(axis_module_registry_set_enabled(fx->registry, "media", FALSE, &error));
	g_assert_nonnull(strstr(error->message, "movies"));
	g_clear_error(&error);
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "media"));

	/* Top down works, and then enabling is blocked from the bottom up. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "media", FALSE, &error));
	g_assert_no_error(error);
	g_assert_false(axis_module_registry_set_enabled(fx->registry, "movies", TRUE, &error));
	g_assert_nonnull(strstr(error->message, "media"));
	g_clear_error(&error);
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "movies"));

	/* check_enabled changes nothing. */
	g_assert_true(axis_module_registry_check_enabled(fx->registry, "media", TRUE, &error));
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "media"));
	g_assert_false(axis_module_registry_check_enabled(fx->registry, "nothing", TRUE, &error));
	g_clear_error(&error);

	/* Bottom up. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "media", TRUE, &error));
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", TRUE, &error));
	g_assert_no_error(error);

	/* Setting the state a module is in is fine, even for a required one. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "core", TRUE, &error));
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "music", FALSE, &error));
	g_assert_no_error(error);
}

/* Module start and stop hooks are called on enable and disable */
static void
test_hooks_follow_the_switch(Reg *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	/* Not started: a switch calls nothing. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	assert_events(fx, "");
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", TRUE, &error));
	assert_events(fx, "");

	/* Started: the ones that are on, in order; music is off and not started. */
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	assert_events(fx, "start:core;start:media;start:movies;");

	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	assert_events(fx, "stop:movies;");
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "music", TRUE, &error));
	assert_events(fx, "start:music;");
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", TRUE, &error));
	assert_events(fx, "start:movies;");

	/* A start that fails leaves the module off, with the module's reason. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "music", FALSE, &error));
	assert_events(fx, "stop:music;");
	fx->hooks.fail_start = g_strdup("music");
	g_assert_false(axis_module_registry_set_enabled(fx->registry, "music", TRUE, &error));
	g_assert_nonnull(strstr(error->message, "music"));
	g_assert_nonnull(strstr(error->message, "no luck"));
	g_clear_error(&error);
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "music"));
	assert_events(fx, "start:music;");

	/* Stopping stops what is on, newest first, and not what was switched off. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	assert_events(fx, "stop:movies;");
	axis_module_registry_stop_all(fx->registry);
	assert_events(fx, "stop:media;stop:core;");
}

static void
on_switch(
	AxisModuleRegistry *registry,
	const gchar        *id,
	gboolean            enabled,
	gpointer            user_data
){
	g_string_append_printf(user_data, "%s=%d;", id, enabled);
}

static void
test_listeners(Reg *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	GString *seen = g_string_new(NULL);
	guint id = axis_module_registry_add_listener(fx->registry, on_switch, seen);

	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	g_assert_false(axis_module_registry_set_enabled(fx->registry, "core", FALSE, &error));
	g_clear_error(&error);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", TRUE, &error));
	g_assert_cmpstr(seen->str, ==, "movies=0;movies=1;");

	axis_module_registry_remove_listener(fx->registry, id);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "movies", FALSE, &error));
	g_assert_cmpstr(seen->str, ==, "movies=0;movies=1;");
	g_string_free(seen, TRUE);
}

static void
test_stored_states_are_applied(Reg *fx, gconstpointer data)
{
	static const gchar *const ids[] = { "movies", "music", "core", "retired", "media" };
	static const gboolean states[] = { TRUE, TRUE, FALSE, FALSE, FALSE };
	g_autoptr(GError) error = NULL;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_module_registry_load_states(fx->registry, ids, states, G_N_ELEMENTS(ids));

	/* The required module ignores "off"; the unknown id is only warned about;
	 * media is off, so movies (stored "on") is switched off with it. */
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "core"));
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "media"));
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "movies"));
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "music"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'core'*required*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'retired'*not registered*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'movies'*switched off*'media'*"), ==, 1);

	/* Only the ones that are on are started. */
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	assert_events(fx, "start:core;start:music;");
}

/* ── The table ──────────────────────────────────────────────────── */

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
} Db;

static void
db_setup(Db *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	fx->db = axis_test_db_new();
	if (fx->db != NULL)
		fx->pool = axis_test_jobs_pool(fx->db, 4);
}

static void
db_teardown(Db *fx, gconstpointer data)
{
	if (fx->pool != NULL)
		axis_db_pool_free(fx->pool);
	if (fx->db != NULL)
		axis_test_db_free(fx->db);
	axis_test_log_reset();
}

#define NEEDS_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

/* Spec: State persists */
static void
test_state_persists_across_restart(Db *fx, gconstpointer data)
{
	Hooks hooks = { NULL, NULL };
	Ctx media = { &hooks, "media" };
	Ctx music = { &hooks, "music" };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *rows = NULL;

	NEEDS_DB(fx);
	/* Only once the test is known to run: a skipped one would leak it. */
	hooks.events = g_string_new(NULL);

	{
		g_autoptr(AxisModuleRegistry) first = axis_module_registry_new();

		g_assert_true(add(first, &media, 0, NULL, &error));
		g_assert_true(add(first, &music, AXIS_MODULE_DISABLED_BY_DEFAULT, NULL, &error));
		g_assert_true(axis_module_state_load(first, fx->pool, &error));
		g_assert_true(axis_module_registry_is_enabled(first, "media"));
		g_assert_false(axis_module_registry_is_enabled(first, "music"));

		/* What the Modules page does: switch, then store. */
		g_assert_true(axis_module_registry_set_enabled(first, "media", FALSE, &error));
		g_assert_true(axis_module_state_store(fx->pool, "media", FALSE, &error));
		g_assert_true(axis_module_registry_set_enabled(first, "music", TRUE, &error));
		g_assert_true(axis_module_state_store(fx->pool, "music", TRUE, &error));
		g_assert_no_error(error);
	}

	/* "Restart": a new registry, the same database. */
	{
		g_autoptr(AxisModuleRegistry) second = axis_module_registry_new();

		g_assert_true(add(second, &media, 0, NULL, &error));
		g_assert_true(add(second, &music, AXIS_MODULE_DISABLED_BY_DEFAULT, NULL, &error));
		g_assert_true(axis_module_state_load(second, fx->pool, &error));
		g_assert_false(axis_module_registry_is_enabled(second, "media"));
		g_assert_true(axis_module_registry_is_enabled(second, "music"));

		/* A state can be stored twice, and changes. */
		g_assert_true(axis_module_state_store(fx->pool, "media", TRUE, &error));
	}

	rows = axis_test_db_query(fx->db,
		"SELECT string_agg(module_id || '=' || enabled, ' ' ORDER BY module_id) FROM module_state");
	g_assert_cmpstr(rows, ==, "media=true music=true");

	/* A row for a module this build lacks is kept and only warned about. */
	axis_test_db_exec(fx->db, "INSERT INTO module_state (module_id, enabled) VALUES ('retired', false)");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*not registered*");
	{
		g_autoptr(AxisModuleRegistry) third = axis_module_registry_new();

		g_assert_true(add(third, &media, 0, NULL, &error));
		g_assert_true(axis_module_state_load(third, fx->pool, &error));
		g_assert_true(axis_module_registry_is_enabled(third, "media"));
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'retired'*"), ==, 1);
	g_free(rows);
	rows = axis_test_db_query(fx->db, "SELECT count(*) FROM module_state WHERE module_id = 'retired'");
	g_assert_cmpstr(rows, ==, "1");

	/* The table refuses a malformed id. */
	g_assert_false(axis_module_state_store(fx->pool, "Not Valid", TRUE, &error));
	g_clear_error(&error);

	g_string_free(hooks.events, TRUE);
}

/* ── Jobs ───────────────────────────────────────────────────────── */

typedef struct
{
	GMutex   lock;
	gboolean gate_open;
	guint    ran_a;
	guint    ran_b;
} Probe;

static gboolean
probe_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	Probe *probe = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *mode = json_object_get_string_member_with_default(payload, "mode", "ok");
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;

	g_mutex_lock(&probe->lock);
	if (strcmp(axis_job_context_get_kind(context), "alpha.work") == 0)
		probe->ran_a++;
	else
		probe->ran_b++;
	g_mutex_unlock(&probe->lock);

	while (strcmp(mode, "gate") == 0) {
		gboolean open;

		g_mutex_lock(&probe->lock);
		open = probe->gate_open;
		g_mutex_unlock(&probe->lock);
		if (open || g_get_monotonic_time() > deadline)
			break;
		axis_job_context_wait(context, 20);
	}

	return TRUE;
}

static const AxisJobHandlerInfo alpha_handlers[] = {
	{ "alpha.work", "mod", 1, probe_handler, "Alpha's work", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobHandlerInfo beta_handlers[] = {
	{ "beta.work", "mod", 1, probe_handler, "Beta's work", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo alpha_schedules[] = {
	{ "alpha-tick", "alpha.work", NULL, 3600 },
	{ NULL, NULL, NULL, 0 }
};

static const AxisJobScheduleInfo beta_schedules[] = {
	{ "beta-tick", "beta.work", NULL, 3600 },
	{ NULL, NULL, NULL, 0 }
};

enum
{
	WITH_SCHEDULES = 1 << 0
};

typedef struct
{
	Db                  db;
	Probe               probe;
	AxisModuleRegistry *modules;
	AxisJobRegistry    *registry;
	AxisJobQueue       *queue;
	gboolean            started;
} Jobs;

static void
jobs_setup(Jobs *fx, gconstpointer data)
{
	AxisModuleInfo alpha;
	AxisModuleInfo beta;
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	db_setup(&fx->db, NULL);
	if (fx->db.db == NULL)
		return;

	g_mutex_init(&fx->probe.lock);
	memset(&alpha, 0, sizeof(alpha));
	alpha.id = "alpha";
	alpha.name = "Alpha";
	alpha.job_handlers = alpha_handlers;
	memset(&beta, 0, sizeof(beta));
	beta.id = "beta";
	beta.name = "Beta";
	beta.job_handlers = beta_handlers;
	/* Schedules only where a test is about them: they would add jobs. */
	if (GPOINTER_TO_UINT(data) & WITH_SCHEDULES) {
		alpha.job_schedules = alpha_schedules;
		beta.job_schedules = beta_schedules;
	}

	fx->modules = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->modules, &alpha, &fx->probe, &error));
	g_assert_true(axis_module_registry_register(fx->modules, &beta, &fx->probe, &error));
	fx->registry = axis_job_registry_new();
	g_assert_true(axis_job_registry_add_modules(fx->registry, fx->modules, &error));
	g_assert_no_error(error);

	fx->queue = axis_job_queue_new(fx->db.pool, fx->registry);
	axis_job_queue_set_poll_interval(fx->queue, 100);
	axis_job_queue_set_lease(fx->queue, 1500);
	axis_job_queue_set_sweep_interval(fx->queue, 100);
	axis_job_queue_set_schedule_interval(fx->queue, 100);
}

static void
start_queue(Jobs *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
	fx->started = TRUE;
}

static void
jobs_teardown(Jobs *fx, gconstpointer data)
{
	if (fx->db.db != NULL) {
		g_mutex_lock(&fx->probe.lock);
		fx->probe.gate_open = TRUE;
		g_mutex_unlock(&fx->probe.lock);
		if (fx->started) {
			axis_job_queue_stop(fx->queue, 5000);
			g_assert_true(axis_job_queue_wait_threads(fx->queue, AXIS_TEST_WAIT_MS));
		}
		g_object_unref(fx->queue);
		while (g_main_context_iteration(NULL, FALSE))
			;
		/* The job registry outlives the module registry it came from. */
		axis_module_registry_free(fx->modules);
		axis_job_registry_unref(fx->registry);
		g_mutex_clear(&fx->probe.lock);
	}
	db_teardown(&fx->db, NULL);
}

static guint
ran(Jobs *fx, gboolean alpha)
{
	guint n;

	g_mutex_lock(&fx->probe.lock);
	n = alpha ? fx->probe.ran_a : fx->probe.ran_b;
	g_mutex_unlock(&fx->probe.lock);

	return n;
}

static gint64
enqueue(Jobs *fx, const gchar *kind, const gchar *mode)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	gint64 id = 0;

	json_object_set_string_member(payload, "mode", mode);
	g_assert_true(axis_job_queue_enqueue(fx->queue, kind, payload, NULL, &id, NULL, &error));
	g_assert_no_error(error);

	return id;
}

static gchar *
state_of(Jobs *fx, gint64 id)
{
	return axis_test_db_queryf(fx->db.db, "SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);
}

static void
wait_state(Jobs *fx, gint64 id, const gchar *state)
{
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	if (!axis_test_wait_query(fx->db.db, sql, state, AXIS_TEST_WAIT_MS))
		g_error("job %" G_GINT64_FORMAT " did not become %s", id, state);
}

/* Spec: Held, then released; Other kinds unaffected; Queued jobs wait */
static void
test_jobs_held_then_released(Jobs *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	gint64 held;
	gint64 other;
	gint64 held_again;

	NEEDS_DB(&fx->db);
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));
	start_queue(fx);

	held = enqueue(fx, "alpha.work", "ok");
	other = enqueue(fx, "beta.work", "ok");

	/* Another module's kind is claimed as usual; alpha's job is not. */
	wait_state(fx, other, "succeeded");
	axis_test_pump_for(600);
	state = state_of(fx, held);
	g_assert_cmpstr(state, ==, "queued");
	g_assert_cmpuint(ran(fx, TRUE), ==, 0);

	/* Held jobs keep their state, and a job queued meanwhile waits too. */
	held_again = enqueue(fx, "alpha.work", "ok");
	axis_test_pump_for(300);
	g_free(state);
	state = state_of(fx, held_again);
	g_assert_cmpstr(state, ==, "queued");

	/* Switched on: both start, without being queued again. */
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", TRUE, &error));
	axis_job_queue_wake(fx->queue);
	wait_state(fx, held, "succeeded");
	wait_state(fx, held_again, "succeeded");
	g_assert_cmpuint(ran(fx, TRUE), ==, 2);

	/* The very same rows ran, once each. */
	g_free(state);
	state = axis_test_db_query(fx->db.db,
		"SELECT count(*) FROM job WHERE kind = 'alpha.work' AND state = 'succeeded' AND attempts = 1");
	g_assert_cmpstr(state, ==, "2");
	g_assert_cmpint(held, <, held_again);
}

/* Without a nudge, the next poll picks the jobs up */
static void
test_jobs_released_by_polling(Jobs *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 held;

	NEEDS_DB(&fx->db);
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));
	start_queue(fx);

	held = enqueue(fx, "alpha.work", "ok");
	axis_test_pump_for(400);
	g_assert_cmpuint(ran(fx, TRUE), ==, 0);

	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", TRUE, &error));
	wait_state(fx, held, "succeeded");
}

/* Spec: Running job finishes */
static void
test_running_job_finishes(Jobs *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	gint64 running;
	gint64 queued;

	NEEDS_DB(&fx->db);
	start_queue(fx);

	running = enqueue(fx, "alpha.work", "gate");
	wait_state(fx, running, "running");

	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));
	queued = enqueue(fx, "alpha.work", "ok");

	/* Still running: switching off does not stop it. */
	axis_test_pump_for(300);
	state = state_of(fx, running);
	g_assert_cmpstr(state, ==, "running");

	g_mutex_lock(&fx->probe.lock);
	fx->probe.gate_open = TRUE;
	g_mutex_unlock(&fx->probe.lock);
	wait_state(fx, running, "succeeded");

	/* It finished; the next of its kind did not start. */
	axis_test_pump_for(400);
	g_free(state);
	state = state_of(fx, queued);
	g_assert_cmpstr(state, ==, "queued");
	g_assert_cmpuint(ran(fx, TRUE), ==, 1);
}

/* Spec: Schedules silent; Re-enable resumes */
static void
test_schedules_are_silent(Jobs *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *count = NULL;

	NEEDS_DB(&fx->db);
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));
	start_queue(fx);

	/* Both default schedules exist and are due; beta's fires, alpha's does not. */
	g_assert_true(axis_test_wait_query(fx->db.db,
		"SELECT count(*) FROM job WHERE kind = 'beta.work'", "1", AXIS_TEST_WAIT_MS));
	axis_test_pump_for(600);
	count = axis_test_db_query(fx->db.db, "SELECT count(*) FROM job WHERE kind = 'alpha.work'");
	g_assert_cmpstr(count, ==, "0");
	g_free(count);
	count = axis_test_db_query(fx->db.db, "SELECT count(*) FROM job_schedule");
	g_assert_cmpstr(count, ==, "2");

	/* The schedule did not move on: it is still due when the module is on. */
	g_free(count);
	count = axis_test_db_query(fx->db.db,
		"SELECT (next_run_at <= now())::text FROM job_schedule WHERE name = 'alpha-tick'");
	g_assert_cmpstr(count, ==, "true");

	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", TRUE, &error));
	g_assert_true(axis_test_wait_query(fx->db.db,
		"SELECT count(*) FROM job WHERE kind = 'alpha.work'", "1", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_wait_query(fx->db.db,
		"SELECT string_agg(state::text, ',') FROM job WHERE kind = 'alpha.work'", "succeeded", AXIS_TEST_WAIT_MS));

	/* And off again: due schedules do not fire. */
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));
	axis_test_db_exec(fx->db.db, "UPDATE job_schedule SET next_run_at = now() - interval '1 minute' "
		"WHERE name = 'alpha-tick'");
	axis_test_db_exec(fx->db.db, "DELETE FROM job WHERE kind = 'alpha.work'");
	axis_test_pump_for(600);
	g_free(count);
	count = axis_test_db_query(fx->db.db, "SELECT count(*) FROM job WHERE kind = 'alpha.work'");
	g_assert_cmpstr(count, ==, "0");
}

/* Spec: Disabling removes nothing */
static void
test_switching_removes_nothing(Jobs *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 held;

	NEEDS_DB(&fx->db);
	axis_test_db_exec(fx->db.db, "CREATE TABLE alpha_data (id int PRIMARY KEY, note text)");
	axis_test_db_exec(fx->db.db, "INSERT INTO alpha_data VALUES (1, 'one'), (2, 'two')");
	axis_test_db_exec(fx->db.db, "INSERT INTO setting (key, value) VALUES ('alpha.limit', '5')");
	start_queue(fx);
	held = enqueue(fx, "alpha.work", "gate");
	wait_state(fx, held, "running");
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));

	before = axis_test_db_query(fx->db.db,
		"SELECT (SELECT string_agg(id || note, ',' ORDER BY id) FROM alpha_data) || '/' || "
		"(SELECT value::text FROM setting WHERE key = 'alpha.limit') || '/' || "
		"(SELECT count(*) FROM job_schedule WHERE name = 'alpha-tick' AND enabled)");

	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", TRUE, &error));
	axis_test_pump_for(100);
	g_assert_true(axis_module_registry_set_enabled(fx->modules, "alpha", FALSE, &error));

	after = axis_test_db_query(fx->db.db,
		"SELECT (SELECT string_agg(id || note, ',' ORDER BY id) FROM alpha_data) || '/' || "
		"(SELECT value::text FROM setting WHERE key = 'alpha.limit') || '/' || "
		"(SELECT count(*) FROM job_schedule WHERE name = 'alpha-tick' AND enabled)");
	g_assert_cmpstr(before, ==, "1one,2two/5/1");
	g_assert_cmpstr(after, ==, before);
}

/* ── The settings the built-in module declares ──────────────────── */

/* "jobs.keep_finished" and "jobs.keep_failed" are what the hourly clean-up
 * deletes by, read at the start of each run. */
static void
test_prune_follows_the_settings(Db *fx, gconstpointer data)
{
	g_autoptr(AxisSettings) settings = NULL;
	g_autoptr(AxisModuleRegistry) modules = NULL;
	g_autoptr(AxisJobRegistry) registry = NULL;
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *left = NULL;
	gint64 id = 0;

	NEEDS_DB(fx);
	settings = axis_settings_new();
	modules = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(modules, axis_job_builtin_get_module(), settings, &error));
	g_assert_true(axis_settings_add_modules(settings, modules, &error));
	g_assert_true(axis_settings_load(settings, fx->pool, &error));
	g_assert_no_error(error);

	/* The defaults are the ones the queue always had. */
	g_assert_cmpint(axis_settings_get_duration(settings, AXIS_JOB_SETTING_KEEP_FINISHED), ==, 7 * 86400);
	g_assert_cmpint(axis_settings_get_duration(settings, AXIS_JOB_SETTING_KEEP_FAILED), ==, 30 * 86400);
	/* And they have limits: a day at least. */
	g_assert_false(axis_settings_set(settings, AXIS_JOB_SETTING_KEEP_FINISHED, "1h", &error));
	g_clear_error(&error);
	g_assert_true(axis_settings_set(settings, AXIS_JOB_SETTING_KEEP_FINISHED, "2d", &error));
	g_assert_true(axis_settings_set(settings, AXIS_JOB_SETTING_KEEP_FAILED, "10d", &error));

	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, state, max_attempts, attempts, finished_at, last_error) VALUES"
		" ('" AXIS_JOB_PRUNE_KIND "', 'maintenance', 'succeeded', 1, 1, now() - interval '3 days', 'a'),"
		" ('" AXIS_JOB_PRUNE_KIND "', 'maintenance', 'succeeded', 1, 1, now() - interval '30 hours', 'b'),"
		" ('" AXIS_JOB_PRUNE_KIND "', 'maintenance', 'failed', 1, 1, now() - interval '12 days', 'c'),"
		" ('" AXIS_JOB_PRUNE_KIND "', 'maintenance', 'failed', 1, 1, now() - interval '8 days', 'd')");

	registry = axis_job_registry_new();
	g_assert_true(axis_job_registry_add_modules(registry, modules, &error));
	queue = axis_job_queue_new(fx->pool, registry);
	axis_job_queue_set_poll_interval(queue, 100);
	axis_job_queue_set_schedule_interval(queue, 100);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* The default schedule runs it at once: older than the settings say goes. */
	g_assert_true(axis_test_wait_query(fx->db,
		"SELECT count(*) FROM job WHERE kind = '" AXIS_JOB_PRUNE_KIND "' AND last_error IS NULL AND state = 'succeeded'",
		"1", AXIS_TEST_WAIT_MS));
	left = axis_test_db_query(fx->db,
		"SELECT string_agg(last_error, '' ORDER BY last_error) FROM job WHERE last_error IS NOT NULL");
	g_assert_cmpstr(left, ==, "bd");

	/* A change applies to the next run, with no restart. */
	g_assert_true(axis_settings_set(settings, AXIS_JOB_SETTING_KEEP_FINISHED, "1d", &error));
	g_assert_true(axis_settings_set(settings, AXIS_JOB_SETTING_KEEP_FAILED, "7d", &error));
	g_assert_true(axis_job_queue_enqueue(queue, AXIS_JOB_PRUNE_KIND, NULL, NULL, &id, NULL, &error));
	g_assert_true(axis_test_wait_query(fx->db,
		"SELECT count(*) FROM job WHERE kind = '" AXIS_JOB_PRUNE_KIND "' AND last_error IS NULL AND state = 'succeeded'",
		"2", AXIS_TEST_WAIT_MS));
	g_free(left);
	left = axis_test_db_query(fx->db,
		"SELECT count(*) FROM job WHERE last_error IS NOT NULL");
	g_assert_cmpstr(left, ==, "0");

	axis_job_queue_stop(queue, 5000);
	g_assert_true(axis_job_queue_wait_threads(queue, AXIS_TEST_WAIT_MS));
}

#define ADD_REG(name, func) \
	g_test_add("/modules/registry/" name, Reg, NULL, reg_setup, func, reg_teardown)
#define ADD_JOBS(name, flags, func) \
	g_test_add("/modules/jobs/" name, Jobs, GUINT_TO_POINTER(flags), jobs_setup, func, jobs_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD_REG("dependencies-at-registration", test_dependencies_are_checked_at_registration);
	ADD_REG("dependency-rules", test_dependency_rules_both_ways);
	ADD_REG("hooks", test_hooks_follow_the_switch);
	ADD_REG("listeners", test_listeners);
	ADD_REG("stored-states", test_stored_states_are_applied);

	g_test_add("/modules/state/persists", Db, NULL, db_setup, test_state_persists_across_restart,
		db_teardown);
	g_test_add("/modules/settings/prune-follows-them", Db, NULL, db_setup,
		test_prune_follows_the_settings, db_teardown);

	ADD_JOBS("held-then-released", 0, test_jobs_held_then_released);
	ADD_JOBS("released-by-polling", 0, test_jobs_released_by_polling);
	ADD_JOBS("running-job-finishes", 0, test_running_job_finishes);
	ADD_JOBS("schedules-silent", WITH_SCHEDULES, test_schedules_are_silent);
	ADD_JOBS("removes-nothing", WITH_SCHEDULES, test_switching_removes_nothing);

	return g_test_run();
}
