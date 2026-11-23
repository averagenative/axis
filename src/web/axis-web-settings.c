/*
 * axis-web-settings.c - the "web" module: the Settings and Modules pages'
 * state, their database work, and the session idle setting
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-settings-private.h"
#include "web/axis-session-store.h"
#include "web/axis-web-theme.h"

#define SESSION_IDLE_KEY   "web.session_idle"
#define DEFAULT_THEME_KEY  "web.default_theme"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebSettings         *owner;
	AxisWebSettingsWorkFunc  work;
	AxisWebSettingsDoneFunc  done;
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
	g_object_unref(task->request);
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

	task->work(task->data);
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
axis_web_settings_dispatch(
	AxisWebSettings         *self,
	HtmxRequest             *request,
	AxisWebSettingsWorkFunc  work,
	AxisWebSettingsDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
){
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_SETTINGS_MAX_PENDING) {
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

	/* The answer is delivered on the main loop, never from this call. */
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);

	return returned;
}

/* ── Helpers for the pages ──────────────────────────────────────── */

AxisModuleRegistry *
axis_web_settings_registry(AxisWebSettings *self)
{
	return self->web != NULL ? axis_web_get_registry(self->web) : NULL;
}

void
axis_web_settings_add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

HtmxNode *
axis_web_settings_card(const gchar *heading)
{
	HtmxSection *section = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();

	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	htmx_node_set_text_content(HTMX_NODE(title), heading);
	axis_web_settings_add(section, title);

	return HTMX_NODE(section);
}

/* ── The session idle setting ───────────────────────────────────── */

void
axis_web_settings_sync(AxisWebSettings *self)
{
	gint64 seconds;
	gchar *theme;

	g_return_if_fail(self != NULL);

	seconds = axis_settings_get_duration(self->settings, SESSION_IDLE_KEY);
	axis_session_store_set_idle_seconds((guint)seconds);

	theme = axis_settings_get_string(self->settings, DEFAULT_THEME_KEY);
	axis_web_theme_set_default(theme);
	g_free(theme);
}

static void
on_setting_changed(
	AxisSettings *settings,
	const gchar  *key,
	gpointer      user_data
){
	axis_web_settings_sync(user_data);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebSettings *
axis_web_settings_new(
	AxisSettings *settings,
	AxisDbPool   *pool
){
	AxisWebSettings *self;

	g_return_val_if_fail(settings != NULL, NULL);

	self = g_new0(AxisWebSettings, 1);
	self->settings = settings;
	self->pool = pool;
	self->subscription = axis_settings_subscribe(settings, SESSION_IDLE_KEY, on_setting_changed,
		self);
	self->theme_subscription = axis_settings_subscribe(settings, DEFAULT_THEME_KEY,
		on_setting_changed, self);

	return self;
}

void
axis_web_settings_stop(AxisWebSettings *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_settings_free(AxisWebSettings *self)
{
	if (self == NULL)
		return;

	axis_web_settings_stop(self);
	axis_settings_unsubscribe(self->settings, self->subscription);
	axis_settings_unsubscribe(self->settings, self->theme_subscription);
	g_free(self);
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebSettings *self = user_data;

	self->web = web;
	axis_web_settings_register_settings_page(self, web);
	axis_web_modules_register_page(self, web);
	axis_web_outbound_register_page(self, web);
}

static const AxisSettingInfo setting_table[] = {
	{
		SESSION_IDLE_KEY, AXIS_SETTING_DURATION,
		"Session idle limit",
		"How long a browser may go unused before its session ends and the user has to log "
		"in again. A session also ends 90 days after it was opened, however busy.",
		0, "30d",
		3600, 90 * 24 * 3600, 0, 0, NULL
	},
	{
		DEFAULT_THEME_KEY, AXIS_SETTING_CHOICE,
		"Default theme",
		"The colour theme of the login and setup pages, and of every user who has not chosen "
		"one on their account page. Users who have chosen a theme keep it.",
		0, AXIS_WEB_THEME_FALLBACK,
		0, 0, 0, 0, axis_web_theme_ids
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

/* The Users page is the shell's own (it comes with the accounts). */
static const AxisNavEntry nav_entries[] = {
	{ "Settings", "/admin/settings", 10, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ "Modules", "/admin/modules", 30, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ "Outbound network", "/admin/network", 31, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const AxisModuleInfo module = {
	"web",
	"Web interface",
	NULL,
	NULL,
	NULL,
	NULL,
	register_routes,
	nav_entries,
	"The pages you are looking at: accounts, sessions, settings, modules and the outbound network.",
	setting_table,
	AXIS_MODULE_REQUIRED,
	NULL,
	NULL
};

const AxisModuleInfo *
axis_web_settings_get_module(void)
{
	axis_setting_set_choice_labels(DEFAULT_THEME_KEY, axis_web_theme_labels);
	return &module;
}
