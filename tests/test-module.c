/*
 * test-module.c - module registry
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>

#include "core/axis-error.h"
#include "core/axis-module.h"
#include "jobs/axis-job-registry.h"

/* Each module appends "start:<id>" / "stop:<id>" to the shared log. */
typedef struct
{
	GString  *events;
	gboolean  fail;
} Probe;

typedef struct
{
	Probe       *probe;
	const gchar *id;
} Ctx;

static gboolean
probe_start(gpointer user_data, GError **error)
{
	Ctx *ctx = user_data;

	g_string_append_printf(ctx->probe->events, "start:%s;", ctx->id);
	if (ctx->probe->fail && strcmp(ctx->id, "bad") == 0) {
		g_set_error_literal(error, AXIS_ERROR, AXIS_ERROR_FAILED, "boom");
		return FALSE;
	}

	return TRUE;
}

static void
probe_stop(gpointer user_data)
{
	Ctx *ctx = user_data;

	g_string_append_printf(ctx->probe->events, "stop:%s;", ctx->id);
}

static gboolean
register_probe(
	AxisModuleRegistry *registry,
	Ctx                *ctx,
	GError            **error
){
	AxisModuleInfo info;

	memset(&info, 0, sizeof(info));
	info.id = ctx->id;
	info.name = "Probe";
	info.start = probe_start;
	info.stop = probe_stop;

	return axis_module_registry_register(registry, &info, ctx, error);
}

static void
test_empty_registry(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;

	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 0);
	g_assert_true(axis_module_registry_start_all(registry, &error));
	g_assert_no_error(error);
	axis_module_registry_stop_all(registry);
}

static void
test_register(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { NULL, FALSE };
	Ctx a = { &probe, "movies" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_no_error(error);
	g_assert_true(axis_module_registry_contains(registry, "movies"));
	g_assert_false(axis_module_registry_contains(registry, "tv"));
	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 1);
}

static void
test_info_is_copied(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;
	gchar *id = g_strdup("copied");

	memset(&info, 0, sizeof(info));
	info.id = id;
	info.name = "Copied";
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
	g_free(id);

	g_assert_true(axis_module_registry_contains(registry, "copied"));
}

static void
test_duplicate_refused(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx first = { &probe, "movies" };
	Ctx second = { &probe, "movies" };

	g_assert_true(register_probe(registry, &first, &error));
	g_assert_false(register_probe(registry, &second, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_nonnull(strstr(error->message, "movies"));

	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 1);
	g_assert_true(axis_module_registry_contains(registry, "movies"));

	/* The first one is the one that runs. */
	g_clear_error(&error);
	g_assert_true(axis_module_registry_start_all(registry, &error));
	g_assert_cmpstr(probe.events->str, ==, "start:movies;");

	axis_module_registry_stop_all(registry);
	g_string_free(probe.events, TRUE);
}

static void
test_malformed_ids_refused(void)
{
	static const gchar *const bad[] = {
		NULL, "", "Movies", "1movies", "-movies", "movie_s", "movie s",
		"movies!", "m\xc3\xb6vies",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", /* 33 characters */
		NULL
	};
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	guint i;

	/* index 0 is the NULL id; the array also ends in NULL */
	for (i = 0; i < G_N_ELEMENTS(bad) - 1; i++) {
		g_autoptr(GError) error = NULL;
		AxisModuleInfo info;

		memset(&info, 0, sizeof(info));
		info.id = bad[i];
		info.name = "x";
		g_assert_false(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	}

	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 0);
}

static void
test_valid_ids_accepted(void)
{
	static const gchar *const good[] = {
		"a", "movies", "tv-2", "music-library-v2",
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", /* exactly 32 */
		NULL
	};
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	guint i;

	for (i = 0; good[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		AxisModuleInfo info;

		memset(&info, 0, sizeof(info));
		info.id = good[i];
		info.name = good[i];
		g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_no_error(error);
	}
}

static void
test_start_order_and_reverse_stop(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };
	Ctx b = { &probe, "b" };
	Ctx c = { &probe, "c" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(register_probe(registry, &b, &error));
	g_assert_true(register_probe(registry, &c, &error));

	g_assert_true(axis_module_registry_start_all(registry, &error));
	g_assert_cmpstr(probe.events->str, ==, "start:a;start:b;start:c;");

	axis_module_registry_stop_all(registry);
	g_assert_cmpstr(probe.events->str, ==,
		"start:a;start:b;start:c;stop:c;stop:b;stop:a;");

	g_string_free(probe.events, TRUE);
}

static void
test_stop_all_twice(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(axis_module_registry_start_all(registry, &error));
	axis_module_registry_stop_all(registry);
	axis_module_registry_stop_all(registry);
	g_assert_cmpstr(probe.events->str, ==, "start:a;stop:a;");

	g_string_free(probe.events, TRUE);
}

static void
test_stop_without_start_does_nothing(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };

	g_assert_true(register_probe(registry, &a, &error));
	axis_module_registry_stop_all(registry);
	g_assert_cmpstr(probe.events->str, ==, "");

	g_string_free(probe.events, TRUE);
}

static void
test_free_stops_started_modules(void)
{
	AxisModuleRegistry *registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };
	Ctx b = { &probe, "b" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(register_probe(registry, &b, &error));
	g_assert_true(axis_module_registry_start_all(registry, &error));
	axis_module_registry_free(registry);

	g_assert_cmpstr(probe.events->str, ==, "start:a;start:b;stop:b;stop:a;");
	g_string_free(probe.events, TRUE);
}

static void
test_rollback_on_failed_start(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), TRUE };
	Ctx a = { &probe, "a" };
	Ctx b = { &probe, "b" };
	Ctx bad = { &probe, "bad" };
	Ctx never = { &probe, "never" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(register_probe(registry, &b, &error));
	g_assert_true(register_probe(registry, &bad, &error));
	g_assert_true(register_probe(registry, &never, &error));

	g_assert_false(axis_module_registry_start_all(registry, &error));
	g_assert_nonnull(error);
	g_assert_true(g_str_has_prefix(error->message, "bad: "));
	g_assert_nonnull(strstr(error->message, "boom"));

	/* Earlier modules stopped in reverse order; the failed module and the
	 * later one were never stopped, and the later one never started. */
	g_assert_cmpstr(probe.events->str, ==, "start:a;start:b;start:bad;stop:b;stop:a;");

	/* The registry is not started, so a later stop does nothing... */
	axis_module_registry_stop_all(registry);
	g_assert_cmpstr(probe.events->str, ==, "start:a;start:b;start:bad;stop:b;stop:a;");

	/* ... and a retry starts from the beginning. */
	probe.fail = FALSE;
	g_clear_error(&error);
	g_string_truncate(probe.events, 0);
	g_assert_true(axis_module_registry_start_all(registry, &error));
	g_assert_cmpstr(probe.events->str, ==, "start:a;start:b;start:bad;start:never;");

	axis_module_registry_stop_all(registry);
	g_string_free(probe.events, TRUE);
}

static gboolean
silent_failing_start(gpointer user_data, GError **error)
{
	return FALSE;
}

static void
test_failed_start_without_error_set(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;

	memset(&info, 0, sizeof(info));
	info.id = "quiet";
	info.name = "Quiet";
	info.start = silent_failing_start;
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));

	g_assert_false(axis_module_registry_start_all(registry, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_true(g_str_has_prefix(error->message, "quiet: "));
}

static void
test_module_without_callbacks(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };
	AxisModuleInfo bare;

	memset(&bare, 0, sizeof(bare));
	bare.id = "bare";
	bare.name = "Bare";

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(axis_module_registry_register(registry, &bare, NULL, &error));
	g_assert_true(axis_module_registry_start_all(registry, &error));
	axis_module_registry_stop_all(registry);
	g_assert_cmpstr(probe.events->str, ==, "start:a;stop:a;");

	g_string_free(probe.events, TRUE);
}

static void
test_register_after_start_refused(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };
	Ctx b = { &probe, "b" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(axis_module_registry_start_all(registry, &error));

	g_assert_false(register_probe(registry, &b, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_false(axis_module_registry_contains(registry, "b"));
	g_assert_cmpuint(axis_module_registry_get_count(registry), ==, 1);

	/* After stopping, registration is open again. */
	axis_module_registry_stop_all(registry);
	g_clear_error(&error);
	g_assert_true(register_probe(registry, &b, &error));
	g_assert_no_error(error);

	g_string_free(probe.events, TRUE);
}

static void
test_start_twice_is_an_error(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	Probe probe = { g_string_new(NULL), FALSE };
	Ctx a = { &probe, "a" };

	g_assert_true(register_probe(registry, &a, &error));
	g_assert_true(axis_module_registry_start_all(registry, &error));
	g_assert_false(axis_module_registry_start_all(registry, &error));
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_MODULE);
	g_assert_cmpstr(probe.events->str, ==, "start:a;");

	axis_module_registry_stop_all(registry);
	g_string_free(probe.events, TRUE);
}

static gboolean
noop_job(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return TRUE;
}

static const AxisJobHandlerInfo job_handlers[] = {
	{ "scan.volume", "scan", 0, noop_job, "Scanning a volume", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo job_schedules[] = {
	{ "rescan", "scan.volume", NULL, 60 },
	{ NULL, NULL, NULL, 0 }
};

/* The job tables are kept as given, and are NULL for a module without. */
static void
test_job_tables(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;
	gint marker = 0;

	memset(&info, 0, sizeof(info));
	info.id = "bare";
	info.name = "Bare";
	g_assert_true(axis_module_registry_register(registry, &info, &marker, &error));
	info.id = "scan";
	info.job_handlers = job_handlers;
	info.job_schedules = job_schedules;
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
	g_assert_no_error(error);

	g_assert_cmpstr(axis_module_registry_get_id(registry, 0), ==, "bare");
	g_assert_cmpstr(axis_module_registry_get_id(registry, 1), ==, "scan");
	g_assert_true(axis_module_registry_get_user_data(registry, 0) == &marker);
	g_assert_null(axis_module_registry_get_user_data(registry, 1));
	g_assert_null(axis_module_registry_get_job_handlers(registry, 0));
	g_assert_null(axis_module_registry_get_job_schedules(registry, 0));
	g_assert_true(axis_module_registry_get_job_handlers(registry, 1) == job_handlers);
	g_assert_true(axis_module_registry_get_job_schedules(registry, 1) == job_schedules);

	/* Modules with job tables start and stop like any other. */
	g_assert_true(axis_module_registry_start_all(registry, &error));
	axis_module_registry_stop_all(registry);

	if (g_test_undefined()) {
		g_test_expect_message("axis", G_LOG_LEVEL_CRITICAL, "*index < self->modules->len*");
		g_assert_null(axis_module_registry_get_id(registry, 2));
		g_test_assert_expected_messages();
	}
}

/* An entry that states no group: written as the tables before groups were,
 * the rest of its fields zero. */
static const AxisNavEntry plain_nav[] = {
	{ "Zeta", "/zeta", 5, 0, FALSE, NULL, 0 },
	{ "Alpha", "/alpha", 5, 0, FALSE, NULL, 0 },
	{ "First", "/first", 1, 0, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const AxisNavEntry grouped_nav[] = {
	{ "Disks", "/disks", 10, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 0 },
	{ "Secret", "/secret", 20, AXIS_NAV_GROUP_MANAGEMENT, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static guint
nav_count(
	AxisModuleRegistry *registry,
	AxisNavGroup        group,
	gboolean            admin
){
	g_autoptr(GArray) entries = axis_module_registry_collect_nav(registry, group, admin);

	return entries->len;
}

/* Spec (daemon-core): Default group.  And the group, the administrators'
 * pages, the order within a group and a module that is off. */
static void
test_nav_groups(void)
{
	g_autoptr(AxisModuleRegistry) registry = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) system = NULL;
	AxisModuleInfo info;

	memset(&info, 0, sizeof(info));
	info.id = "plain";
	info.nav_entries = plain_nav;
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
	info.id = "grouped";
	info.nav_entries = grouped_nav;
	g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
	g_assert_no_error(error);

	/* No group stated: the system group, by order then label. */
	system = axis_module_registry_collect_nav(registry, AXIS_NAV_GROUP_SYSTEM, TRUE);
	g_assert_cmpuint(system->len, ==, 3);
	g_assert_cmpstr(g_array_index(system, AxisNavEntry, 0).label, ==, "First");
	g_assert_cmpstr(g_array_index(system, AxisNavEntry, 1).label, ==, "Alpha");
	g_assert_cmpstr(g_array_index(system, AxisNavEntry, 2).label, ==, "Zeta");
	g_assert_cmpint(AXIS_NAV_GROUP_SYSTEM, ==, 0);

	/* The administrators' page only for an administrator. */
	g_assert_cmpuint(nav_count(registry, AXIS_NAV_GROUP_MANAGEMENT, TRUE), ==, 2);
	g_assert_cmpuint(nav_count(registry, AXIS_NAV_GROUP_MANAGEMENT, FALSE), ==, 1);
	g_assert_cmpuint(nav_count(registry, AXIS_NAV_GROUP_ACTIVITY, TRUE), ==, 0);

	/* A module that is off has no entry. */
	g_assert_true(axis_module_registry_set_enabled(registry, "grouped", FALSE, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(nav_count(registry, AXIS_NAV_GROUP_MANAGEMENT, TRUE), ==, 0);
	g_assert_cmpuint(nav_count(registry, AXIS_NAV_GROUP_SYSTEM, TRUE), ==, 3);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/module/nav-groups", test_nav_groups);
	g_test_add_func("/module/empty", test_empty_registry);
	g_test_add_func("/module/register", test_register);
	g_test_add_func("/module/info-is-copied", test_info_is_copied);
	g_test_add_func("/module/duplicate-refused", test_duplicate_refused);
	g_test_add_func("/module/malformed-ids-refused", test_malformed_ids_refused);
	g_test_add_func("/module/valid-ids-accepted", test_valid_ids_accepted);
	g_test_add_func("/module/start-order-reverse-stop", test_start_order_and_reverse_stop);
	g_test_add_func("/module/stop-all-twice", test_stop_all_twice);
	g_test_add_func("/module/stop-without-start", test_stop_without_start_does_nothing);
	g_test_add_func("/module/free-stops-started", test_free_stops_started_modules);
	g_test_add_func("/module/rollback-on-failed-start", test_rollback_on_failed_start);
	g_test_add_func("/module/failed-start-without-error", test_failed_start_without_error_set);
	g_test_add_func("/module/no-callbacks", test_module_without_callbacks);
	g_test_add_func("/module/register-after-start-refused", test_register_after_start_refused);
	g_test_add_func("/module/start-twice", test_start_twice_is_an_error);
	g_test_add_func("/module/job-tables", test_job_tables);

	return g_test_run();
}
