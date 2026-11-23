/*
 * axis-web-profiles.c - the profile pages module: its work off the main
 * loop, the pieces its pages share
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-profiles-private.h"

#include "db/axis-db-error.h"
#include "library/axis-library.h"
#include "storage/axis-storage.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebProfiles         *owner;
	AxisWebProfilesWorkFunc  work;
	AxisWebProfilesDoneFunc  done;
	HtmxRequest             *request;
	HtmxResponse            *deferred;
	gpointer                 data;
	GDestroyNotify           free_data;
} Task;

static void
task_free(Task *task)
{
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

static void
task_thread(
	GTask        *gtask,
	gpointer      source,
	gpointer      task_data,
	GCancellable *cancellable
){
	Task *task = task_data;

	task->work(task->owner, task->data);
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;
	HtmxResponse *response;

	task->owner->pending--;
	response = task->done(task->owner, task->request, task->data);
	htmx_response_complete(task->deferred, response);
	g_object_unref(response);
	task_free(task);
}

HtmxResponse *
axis_web_profiles_dispatch(
	AxisWebProfiles         *self,
	HtmxRequest             *request,
	AxisWebProfilesWorkFunc  work,
	AxisWebProfilesDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
){
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_PROFILES_MAX_PENDING) {
		HtmxResponse *busy = axis_web_error_respond(request, 503, "Busy",
			"The server is busy. Try again in a few seconds.");

		if (free_data != NULL)
			free_data(data);
		htmx_response_add_header(busy, "Retry-After", "5");
		return busy;
	}

	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	task->data = data;
	task->free_data = free_data;
	returned = g_object_ref(task->deferred);

	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebProfiles *
axis_web_profiles_new(AxisEstimator *estimator)
{
	AxisWebProfiles *self;

	g_return_val_if_fail(estimator != NULL, NULL);

	self = g_new0(AxisWebProfiles, 1);
	self->estimator = axis_estimator_ref(estimator);
	return self;
}

void
axis_web_profiles_set_compat(
	AxisWebProfiles *self,
	AxisCompat      *compat
){
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->compat, axis_compat_unref);
	if (compat != NULL)
		self->compat = axis_compat_ref(compat);
}

void
axis_web_profiles_stop(AxisWebProfiles *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_profiles_free(AxisWebProfiles *self)
{
	if (self == NULL)
		return;

	axis_web_profiles_stop(self);
	g_clear_pointer(&self->compat, axis_compat_unref);
	axis_estimator_unref(self->estimator);
	g_free(self);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

AxisCompatReport *
axis_web_profiles_compat_report(AxisWebProfiles *self)
{
	g_autoptr(GError) error = NULL;
	AxisCompatReport *report = NULL;

	if (self->compat == NULL || !axis_compat_is_enabled(self->compat))
		return NULL;
	if (!axis_compat_compute_report(self->compat, &report, &error)) {
		g_message("profiles: the compatibility verdicts could not be worked out: %s", error->message);
		return NULL;
	}
	return report;
}

gchar *
axis_web_profiles_label(const AxisProfileRevision *revision)
{
	const AxisProfileBuiltin *builtin = revision->built_in ? axis_profile_builtin(revision->slug) : NULL;
	const gchar *family = builtin != NULL ? builtin->family : revision->usable ? axis_profile_family(&revision->def) :
		revision->kind == AXIS_PROFILE_KIND_AUDIO ? "Audio" : "Video";

	if (g_strcmp0(family, revision->name) == 0)
		return g_strdup(revision->name);
	return g_strdup_printf("%s: %s", family, revision->name);
}

HtmxNode *
axis_web_profiles_lock_badge(const AxisProfileRevision *revision)
{
	if (!revision->usable)
		return axis_web_browser_badge("Not usable", "badge-failed");
	if (axis_profile_revision_is_unlocked(revision))
		return axis_web_browser_badge("Proven", "badge-succeeded");
	return axis_web_browser_badge("Locked: trials only", "badge-warn");
}

const gchar *
axis_web_profiles_lock_sentence(const AxisProfileRevision *revision)
{
	if (!revision->usable)
		return "Its definition is not usable, so nothing is estimated or converted with it.";
	if (axis_profile_revision_is_unlocked(revision) && g_strcmp0(revision->proof_method, "trials") == 0)
		return "Proven by hand from passed trials: it may replace files where the playback verdict is safe.";
	if (axis_profile_revision_is_unlocked(revision))
		return "Proven on the household's devices: it may be used for bulk conversion.";
	return "Locked: this version can make trials of single files but cannot replace a file, alone or in a conversion "
		"run. An administrator proves it on the Conversions page once enough trials have passed.";
}

HtmxResponse *
axis_web_profiles_error(
	HtmxRequest  *request,
	const GError *error
){
	if (error->domain == AXIS_PROFILE_ERROR) {
		switch (error->code) {
		case AXIS_PROFILE_ERROR_NOT_FOUND:
			return axis_web_error_respond(request, 404, "Not found", error->message);
		case AXIS_PROFILE_ERROR_BUILT_IN:
			return axis_web_error_respond(request, 403, "Built in", error->message);
		case AXIS_PROFILE_ERROR_INVALID:
			return axis_web_error_respond(request, 400, "Bad request", error->message);
		default:
			return axis_web_error_respond(request, 409, "Not done", error->message);
		}
	}
	if (g_error_matches(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT))
		return axis_web_error_respond(request, 503, "Too slow", "The profiles could not be read in time. Try again "
			"in a moment.");
	return axis_web_internal_error(request, error);
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_profiles_register_pages(user_data, web);
	axis_web_profiles_register_form(user_data, web);
}

static const AxisNavEntry nav_entries[] = {
	{ "Conversion profiles", "/profiles", 35, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = {
	AXIS_STORAGE_MODULE_ID, AXIS_LIBRARY_MODULE_ID, AXIS_ESTIMATE_MODULE_ID, NULL
};

const AxisModuleInfo *
axis_web_profiles_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_PROFILES_MODULE_ID;
		info.name = "Conversion profiles";
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Conversion profiles: the built-in ones and an administrator's copies of them. Each "
			"change makes a new version that cannot be edited and stays locked until it is proven. Each library "
			"folder can be given a conversion profile. It converts nothing.";
		info.depends_on = depends_on;
	}
	return &info;
}
