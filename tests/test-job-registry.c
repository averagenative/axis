/*
 * test-job-registry.c - job kinds, default schedules, and the module tables
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * No database: what the registry accepts and refuses, and how it reads
 * the job tables of registered modules.
 */

#include <glib.h>
#include <string.h>

#include "core/axis-error.h"
#include "core/axis-module.h"
#include "jobs/axis-history.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-private.h"
#include "jobs/axis-job-registry.h"
#include "acquire/axis-acquire.h"
#include "api/axis-worker-api.h"
#include "download/axis-download.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-arr.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-compat.h"
#include "web/axis-web-conversions.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-library.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-profiles.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-savings.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-workers.h"

static gboolean
noop(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	return TRUE;
}

static void
test_add_and_refuse(void)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	AxisJobHandlerInfo info;

	memset(&info, 0, sizeof(info));
	info.kind = "scan.volume";
	info.queue = "scan";
	info.handler = noop;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_registry_contains(registry, "scan.volume"));
	g_assert_false(axis_job_registry_contains(registry, "scan.other"));
	g_assert_false(axis_job_registry_contains(registry, NULL));

	/* Twice. */
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "scan.volume"));
	g_clear_error(&error);

	/* No handler, bad names, too many attempts. */
	info.kind = "scan.b";
	info.handler = NULL;
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.handler = noop;
	info.queue = "Scan";
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.queue = "scan";
	info.kind = "scan b";
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.kind = "scan.b";
	info.max_attempts = AXIS_JOB_MAX_ATTEMPTS_LIMIT + 1;
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_job_registry_contains(registry, "scan.b"));
}

/* Spec (job-queue): a kind that follows a hold is not claimable while it
 * is on, and is again when it is released; other kinds are unaffected; no
 * hold is set once the queue has started. */
static void
test_holds(void)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(AxisJobHold) hold = axis_job_hold_new();
	g_autoptr(GError) error = NULL;
	AxisJobHandlerInfo info;

	memset(&info, 0, sizeof(info));
	info.kind = "convert.local";
	info.queue = "convert";
	info.handler = noop;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	info.kind = "scan.volume";
	info.queue = "scan";
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_true(axis_job_registry_add_remote(registry, "convert.remote", "remote", 1, "Doing a remote thing",
		AXIS_JOB_CLASS_WORK, NULL, &error));
	g_assert_no_error(error);

	g_assert_true(axis_job_registry_set_hold(registry, "convert.local", hold, &error));
	g_assert_true(axis_job_registry_set_hold(registry, "convert.remote", hold, &error));
	g_assert_false(axis_job_registry_set_hold(registry, "convert.none", hold, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND);
	g_clear_error(&error);

	/* Off when made: everything claimable. */
	g_assert_false(axis_job_hold_is_held(hold));
	g_assert_true(axis_job_registry_kind_claimable(registry, "convert.local"));
	g_assert_true(axis_job_registry_kind_claimable(registry, "convert.remote"));
	/* On: the held kinds wait, the others do not. */
	axis_job_hold_set(hold, TRUE);
	g_assert_false(axis_job_registry_kind_claimable(registry, "convert.local"));
	g_assert_false(axis_job_registry_kind_claimable(registry, "convert.remote"));
	g_assert_true(axis_job_registry_kind_claimable(registry, "scan.volume"));
	/* Released. */
	axis_job_hold_set(hold, FALSE);
	g_assert_true(axis_job_registry_kind_claimable(registry, "convert.local"));
	g_assert_true(axis_job_registry_kind_claimable(registry, "convert.remote"));

	axis_job_registry_freeze(registry);
	g_assert_false(axis_job_registry_set_hold(registry, "scan.volume", hold, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_STATE);
}

static void
test_schedules(void)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	AxisJobHandlerInfo info;
	AxisJobScheduleInfo schedule;

	memset(&info, 0, sizeof(info));
	info.kind = "scan.volume";
	info.queue = "scan";
	info.handler = noop;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));

	memset(&schedule, 0, sizeof(schedule));
	schedule.name = "rescan";
	schedule.kind = "scan.volume";
	schedule.payload = "{\"deep\": false}";
	schedule.interval_seconds = 600;
	g_assert_true(axis_job_registry_add_schedule(registry, &schedule, &error));
	g_assert_no_error(error);

	g_assert_false(axis_job_registry_add_schedule(registry, &schedule, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);

	schedule.name = "other";
	schedule.kind = "not.registered";
	g_assert_false(axis_job_registry_add_schedule(registry, &schedule, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "not.registered"));
	g_clear_error(&error);

	schedule.kind = "scan.volume";
	schedule.payload = "[1, 2]";
	g_assert_false(axis_job_registry_add_schedule(registry, &schedule, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);

	schedule.payload = NULL;
	schedule.interval_seconds = 0;
	g_assert_false(axis_job_registry_add_schedule(registry, &schedule, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
}

/* The AxisModuleInfo tables: NULL-safe, user data passed, schedules of
 * one module may name another's kinds, errors name the module. */
static const AxisJobHandlerInfo scan_handlers[] = {
	{ "scan.volume", "scan", 0, noop, "Scanning a volume", AXIS_JOB_CLASS_WORK },
	{ "scan.file", "scan", 2, noop, "Scanning a file", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo scan_schedules[] = {
	{ "rescan", "probe.file", NULL, 60 },
	{ NULL, NULL, NULL, 0 }
};

static const AxisJobHandlerInfo probe_handlers[] = {
	{ "probe.file", "probe", 0, noop, "Reading a file", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobHandlerInfo clashing_handlers[] = {
	{ "scan.file", "other", 0, noop, "Scanning a file", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static void
test_add_modules(void)
{
	g_autoptr(AxisModuleRegistry) modules = axis_module_registry_new();
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;
	gint marker = 0;

	memset(&info, 0, sizeof(info));
	info.id = "bare";
	info.name = "No jobs";
	g_assert_true(axis_module_registry_register(modules, &info, NULL, &error));
	info.id = "scan";
	info.job_handlers = scan_handlers;
	info.job_schedules = scan_schedules;
	g_assert_true(axis_module_registry_register(modules, &info, &marker, &error));
	memset(&info, 0, sizeof(info));
	info.id = "probe";
	info.job_handlers = probe_handlers;
	g_assert_true(axis_module_registry_register(modules, &info, NULL, &error));
	g_assert_true(axis_module_registry_register(modules, axis_job_builtin_get_module(), NULL, &error));
	g_assert_no_error(error);

	g_assert_true(axis_job_registry_add_modules(registry, modules, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_registry_contains(registry, "scan.volume"));
	g_assert_true(axis_job_registry_contains(registry, "scan.file"));
	g_assert_true(axis_job_registry_contains(registry, "probe.file"));
	g_assert_true(axis_job_registry_contains(registry, AXIS_JOB_PRUNE_KIND));

	/* A second module with a kind the first has. */
	{
		g_autoptr(AxisModuleRegistry) more = axis_module_registry_new();
		g_autoptr(AxisJobRegistry) again = axis_job_registry_new();

		memset(&info, 0, sizeof(info));
		info.id = "scan";
		info.job_handlers = scan_handlers;
		g_assert_true(axis_module_registry_register(more, &info, NULL, &error));
		info.id = "clash";
		info.job_handlers = clashing_handlers;
		g_assert_true(axis_module_registry_register(more, &info, NULL, &error));
		g_assert_false(axis_job_registry_add_modules(again, more, &error));
		g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
		g_assert_true(g_str_has_prefix(error->message, "module clash: "));
		g_assert_nonnull(strstr(error->message, "scan.file"));
	}
}

/* A kind's plain label and its class: kept, read back, listed; a label
 * that is not one line of text is refused; a kind without them is listed
 * as undescribed (which the daemon refuses). */
static void
test_descriptions(void)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) work = NULL;
	g_auto(GStrv) upkeep = NULL;
	g_auto(GStrv) undescribed = NULL;
	g_autofree gchar *long_label = g_strnfill(AXIS_JOB_LABEL_MAX_LEN + 1, 'a');
	AxisJobHandlerInfo info;

	memset(&info, 0, sizeof(info));
	info.kind = "scan.volume";
	info.queue = "scan";
	info.handler = noop;
	info.label = "Scanning a volume";
	info.job_class = AXIS_JOB_CLASS_WORK;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	info.kind = "scan.due";
	info.label = "Checking which volumes are due";
	info.job_class = AXIS_JOB_CLASS_UPKEEP;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	info.kind = "test.bare";
	info.label = NULL;
	info.job_class = AXIS_JOB_CLASS_UNSET;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	info.kind = "test.unlabelled";
	info.job_class = AXIS_JOB_CLASS_UPKEEP;
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_true(axis_job_registry_add_remote(registry, "remote.thing", "remote", 1, "Doing a remote thing",
		AXIS_JOB_CLASS_WORK, NULL, &error));
	g_assert_no_error(error);

	g_assert_cmpstr(axis_job_registry_get_label(registry, "scan.volume"), ==, "Scanning a volume");
	g_assert_cmpint(axis_job_registry_get_class(registry, "scan.volume"), ==, AXIS_JOB_CLASS_WORK);
	g_assert_cmpint(axis_job_registry_get_class(registry, "scan.due"), ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_cmpstr(axis_job_registry_get_label(registry, "remote.thing"), ==, "Doing a remote thing");
	g_assert_null(axis_job_registry_get_label(registry, "test.bare"));
	g_assert_null(axis_job_registry_get_label(registry, "not.registered"));
	g_assert_cmpint(axis_job_registry_get_class(registry, "not.registered"), ==, AXIS_JOB_CLASS_UNSET);
	g_assert_null(axis_job_registry_get_label(registry, NULL));

	work = axis_job_registry_list_class(registry, AXIS_JOB_CLASS_WORK);
	upkeep = axis_job_registry_list_class(registry, AXIS_JOB_CLASS_UPKEEP);
	undescribed = axis_job_registry_list_undescribed(registry);
	g_assert_cmpuint(g_strv_length(work), ==, 2);
	g_assert_cmpstr(work[0], ==, "remote.thing");
	g_assert_cmpstr(work[1], ==, "scan.volume");
	g_assert_cmpuint(g_strv_length(upkeep), ==, 2);
	g_assert_cmpstr(upkeep[0], ==, "scan.due");
	g_assert_cmpstr(upkeep[1], ==, "test.unlabelled");
	g_assert_cmpuint(g_strv_length(undescribed), ==, 2);
	g_assert_cmpstr(undescribed[0], ==, "test.bare");
	g_assert_cmpstr(undescribed[1], ==, "test.unlabelled");

	/* Not one line of text, or not a class. */
	info.kind = "scan.b";
	info.job_class = AXIS_JOB_CLASS_WORK;
	info.label = "";
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.label = "two\nlines";
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.label = long_label;
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.label = "\xff\xfe";
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	info.label = "Fine";
	info.job_class = (AxisJobClass)7;
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_job_registry_add_remote(registry, "remote.other", "remote", 1, "two\nlines",
		AXIS_JOB_CLASS_WORK, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_assert_false(axis_job_registry_contains(registry, "scan.b"));
	g_assert_false(axis_job_registry_contains(registry, "remote.other"));
}

/*
 * Every kind the daemon registers says what it does in plain words and
 * whether it is work or upkeep.  The modules here are every one the daemon
 * has whose description needs no instance; the metadata module's needs its
 * store, and is covered by the daemon itself, which refuses to start with
 * an undescribed kind (so every test of test-daemon would fail).
 */
static void
test_every_kind_described(void)
{
	const AxisModuleInfo *(*const getters[])(void) = {
		axis_job_builtin_get_module, axis_history_get_module, axis_web_settings_get_module, axis_web_jobs_get_module,
		axis_web_storage_get_module, axis_web_library_get_module, axis_web_probe_get_module,
		axis_web_browser_get_module, axis_web_analysis_get_module, axis_web_savings_get_module,
		axis_web_compat_get_module, axis_web_profiles_get_module, axis_web_workers_get_module,
		axis_web_downloads_get_module, axis_web_imports_get_module, axis_web_indexers_get_module,
		axis_web_quality_get_module, axis_web_identify_get_module, axis_movies_get_module, axis_tv_get_module,
		axis_web_arr_get_module
	};
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) undescribed = NULL;
	guint kinds = 0;
	guint upkeep = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(getters); i++) {
		const AxisModuleInfo *module = getters[i]();
		guint j;

		for (j = 0; module->job_handlers != NULL && module->job_handlers[j].kind != NULL; j++) {
			const AxisJobHandlerInfo *info = &module->job_handlers[j];

			if (info->label == NULL || info->job_class == AXIS_JOB_CLASS_UNSET)
				g_error("module %s: job kind %s has no %s", module->id, info->kind,
					info->label == NULL ? "label" : "class");
			g_assert_true(axis_job_registry_add(registry, info, NULL, &error));
			g_assert_no_error(error);
			kinds++;
			upkeep += info->job_class == AXIS_JOB_CLASS_UPKEEP;
		}
	}
	g_assert_true(axis_worker_api_register_kinds(registry, NULL, &error));
	g_assert_no_error(error);

	undescribed = axis_job_registry_list_undescribed(registry);
	if (undescribed[0] != NULL) {
		g_autofree gchar *list = g_strjoinv(", ", undescribed);

		g_error("job kinds without a label or a class: %s", list);
	}
	/* The polling ones are upkeep, the ones a person asks for are work. */
	g_assert_cmpint(axis_job_registry_get_class(registry, "download.track"), ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_cmpstr(axis_job_registry_get_label(registry, "download.track"), ==, "Checking the download clients");
	g_assert_cmpint(axis_job_registry_get_class(registry, "library.scan"), ==, AXIS_JOB_CLASS_WORK);
	g_assert_cmpint(axis_job_registry_get_class(registry, "movies.search"), ==, AXIS_JOB_CLASS_WORK);
	g_assert_cmpint(axis_job_registry_get_class(registry, "movies.follow"), ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_cmpint(axis_job_registry_get_class(registry, "import.run"), ==, AXIS_JOB_CLASS_WORK);
	g_assert_cmpint(axis_job_registry_get_class(registry, "jobs.prune"), ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_cmpint(axis_job_registry_get_class(registry, AXIS_HISTORY_PRUNE_KIND), ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_cmpstr(axis_job_registry_get_label(registry, AXIS_HISTORY_PRUNE_KIND), ==,
		"Deleting old history of movies, series, downloads and imports");
	g_test_message("%u kinds from the modules, %u of them upkeep", kinds, upkeep);
	g_assert_cmpuint(kinds, >=, 25);
}

/*
 * Spec: Finishing comes before starting.  The searches that adding a series
 * queues by the dozen are on a queue of their own, with a worker of its own:
 * following the downloads and importing what finished share none with them.
 * Found by the stage 2 hunt of the job queue (H-4): they shared the
 * downloads queue, the searches a person caused claimed first.
 */
static void
test_acquisition_queues(void)
{
	const AxisModuleInfo *(*const getters[])(void) = {
		axis_web_downloads_get_module, axis_web_imports_get_module, axis_movies_get_module, axis_tv_get_module,
		axis_web_conversions_get_module, axis_web_identify_get_module
	};
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GPtrArray) kinds = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(getters); i++) {
		const AxisModuleInfo *module = getters[i]();
		guint j;

		for (j = 0; module->job_handlers != NULL && module->job_handlers[j].kind != NULL; j++)
			g_assert_true(axis_job_registry_add(registry, &module->job_handlers[j], NULL, &error));
		g_assert_no_error(error);
	}

	g_assert_cmpstr(axis_job_registry_lookup(registry, "movies.search")->queue, ==, AXIS_ACQUIRE_SEARCH_QUEUE);
	g_assert_cmpstr(axis_job_registry_lookup(registry, "tv.search")->queue, ==, AXIS_ACQUIRE_SEARCH_QUEUE);
	g_assert_cmpstr(axis_job_registry_lookup(registry, "download.track")->queue, ==, AXIS_DOWNLOAD_QUEUE);
	g_assert_cmpstr(axis_job_registry_lookup(registry, "import.run")->queue, ==, AXIS_DOWNLOAD_QUEUE);
	g_assert_cmpstr(AXIS_ACQUIRE_SEARCH_QUEUE, !=, AXIS_DOWNLOAD_QUEUE);

	/* Nothing else waits behind the searches. */
	kinds = axis_job_registry_list_kinds(registry);
	for (i = 0; i < kinds->len; i++) {
		const AxisJobKind *kind = g_ptr_array_index(kinds, i);

		if (strcmp(kind->queue, AXIS_ACQUIRE_SEARCH_QUEUE) == 0)
			g_assert_true(strcmp(kind->kind, "movies.search") == 0 || strcmp(kind->kind, "tv.search") == 0);
	}
}

/*
 * Spec (job-ui): A kind its owner asks for again.  The modules' and the
 * worker API's kinds whose owner tries the work again say where; the rest
 * are retried as themselves.
 */
static void
test_retry_elsewhere(void)
{
	g_autoptr(AxisModuleRegistry) modules = axis_module_registry_new();
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo owner;
	AxisJobHandlerInfo handlers[3];
	static const gchar *const notes[] = { "test.owned", "Ask the owner.", NULL };
	static const gchar *const bad[] = { "test.missing", "Ask the owner.", NULL };
	guint i;

	memset(&owner, 0, sizeof(owner));
	memset(handlers, 0, sizeof(handlers));
	handlers[0].kind = "test.owned";
	handlers[0].queue = "test";
	handlers[0].handler = noop;
	handlers[1].kind = "test.plain";
	handlers[1].queue = "test";
	handlers[1].handler = noop;
	owner.id = "owner";
	owner.name = "Owner";
	owner.job_handlers = handlers;
	owner.job_retry_elsewhere = notes;
	g_assert_true(axis_module_registry_register(modules, &owner, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_registry_add_modules(registry, modules, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(axis_job_registry_get_retry_elsewhere(registry, "test.owned"), ==, "Ask the owner.");
	g_assert_null(axis_job_registry_get_retry_elsewhere(registry, "test.plain"));
	g_assert_false(axis_job_registry_set_retry_elsewhere(registry, "test.plain", "two\nlines", &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);

	/* A note for a kind the module does not have is refused. */
	{
		g_autoptr(AxisModuleRegistry) others = axis_module_registry_new();
		g_autoptr(AxisJobRegistry) fresh = axis_job_registry_new();

		owner.job_retry_elsewhere = bad;
		g_assert_true(axis_module_registry_register(others, &owner, NULL, &error));
		g_assert_false(axis_job_registry_add_modules(fresh, others, &error));
		g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND);
		g_clear_error(&error);
	}

	/* A kind without its sentence is refused, and nothing is read past the
	 * table's end (found by an independent review). */
	{
		static const gchar *const unsaid[] = { "test.owned", NULL };
		g_autoptr(AxisModuleRegistry) others = axis_module_registry_new();
		g_autoptr(AxisJobRegistry) fresh = axis_job_registry_new();

		owner.job_retry_elsewhere = unsaid;
		g_assert_true(axis_module_registry_register(others, &owner, NULL, &error));
		g_assert_false(axis_job_registry_add_modules(fresh, others, &error));
		g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
		g_assert_nonnull(strstr(error->message, "'test.owned'"));
		g_clear_error(&error);
	}

	/* The real ones. */
	{
		const AxisModuleInfo *(*const getters[])(void) = {
			axis_web_imports_get_module, axis_web_conversions_get_module, axis_movies_get_module,
			axis_tv_get_module, axis_web_savings_get_module
		};
		static const gchar *const elsewhere[] = {
			"import.run", "convert.run.list", "convert.encode", "convert.verify", "estimate.sample",
			"estimate.sample.remote", "worker.capabilities", NULL
		};
		static const gchar *const itself[] = {
			"movies.search", "tv.search", "import.cleanup", "convert.finish", "convert.tidy", NULL
		};
		g_autoptr(AxisJobRegistry) real = axis_job_registry_new();

		for (i = 0; i < G_N_ELEMENTS(getters); i++) {
			const AxisModuleInfo *module = getters[i]();
			guint j;

			for (j = 0; module->job_handlers[j].kind != NULL; j++)
				g_assert_true(axis_job_registry_add(real, &module->job_handlers[j], NULL, &error));
			for (j = 0; module->job_retry_elsewhere != NULL && module->job_retry_elsewhere[j] != NULL; j += 2)
				g_assert_true(axis_job_registry_set_retry_elsewhere(real, module->job_retry_elsewhere[j],
					module->job_retry_elsewhere[j + 1], &error));
			g_assert_no_error(error);
		}
		g_assert_true(axis_worker_api_register_kinds(real, NULL, &error));
		g_assert_no_error(error);
		for (i = 0; elsewhere[i] != NULL; i++)
			g_assert_nonnull(axis_job_registry_get_retry_elsewhere(real, elsewhere[i]));
		for (i = 0; itself[i] != NULL; i++)
			g_assert_null(axis_job_registry_get_retry_elsewhere(real, itself[i]));
	}
}

static void
test_names(void)
{
	static const gchar *const good[] = { "a", "scan", "scan.volume", "jobs.prune", "a-b_c.9", NULL };
	static const gchar *const bad[] = { "", "A", "9a", ".a", "a b", "a/b", "a:b", "ä", NULL };
	AxisJobState state;
	guint i;

	for (i = 0; good[i] != NULL; i++)
		g_assert_true(axis_job_name_is_valid(good[i]));
	for (i = 0; bad[i] != NULL; i++)
		g_assert_false(axis_job_name_is_valid(bad[i]));
	g_assert_false(axis_job_name_is_valid(NULL));

	for (i = AXIS_JOB_STATE_QUEUED; i <= AXIS_JOB_STATE_CANCELLED; i++) {
		g_assert_true(axis_job_state_from_string(axis_job_state_to_string((AxisJobState)i), &state));
		g_assert_cmpint(state, ==, (AxisJobState)i);
	}
	g_assert_false(axis_job_state_from_string("done", &state));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/job/registry/add-and-refuse", test_add_and_refuse);
	g_test_add_func("/job/registry/holds", test_holds);
	g_test_add_func("/job/registry/schedules", test_schedules);
	g_test_add_func("/job/registry/add-modules", test_add_modules);
	g_test_add_func("/job/registry/descriptions", test_descriptions);
	g_test_add_func("/job/registry/every-kind-described", test_every_kind_described);
	g_test_add_func("/job/registry/names", test_names);
	g_test_add_func("/job/registry/acquisition-queues", test_acquisition_queues);
	g_test_add_func("/job/registry/retry-elsewhere", test_retry_elsewhere);

	return g_test_run();
}
