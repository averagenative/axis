/*
 * axis-web-modules-page.c - the Modules page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Lists every registered module with its state and what it needs, and
 * switches modules on and off.  A switch the dependencies forbid has no
 * button; the reason is shown instead.  The same rules are enforced on the
 * POST, so a forged request for a required module, or for a switch the
 * page does not offer, is refused with the same reason.  A switch is made
 * in memory first (the module's start or stop hook runs) and then stored;
 * if storing fails, the switch is undone.
 */

#include "web/axis-web-settings-private.h"
#include "core/axis-module-state.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"

#include <string.h>

#define ADD axis_web_settings_add

static const gchar *
done_message(const gchar *key)
{
	if (g_strcmp0(key, "enabled") == 0)
		return "The module has been switched on.";
	if (g_strcmp0(key, "disabled") == 0)
		return "The module has been switched off. Its data and settings are kept.";

	return NULL;
}

static HtmxNode *
cell(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

/* The names of @index's dependencies, comma separated; "None" for none. */
static gchar *
needs_text(
	AxisModuleRegistry *registry,
	guint               index
){
	const gchar *const *ids = axis_module_registry_get_depends_on(registry, index);
	GString *text = g_string_new(NULL);
	guint i;

	for (i = 0; ids[i] != NULL; i++) {
		gint other = axis_module_registry_find(registry, ids[i]);

		g_string_append_printf(text, "%s%s", i > 0 ? ", " : "",
			other >= 0 ? axis_module_registry_get_name(registry, (guint)other) : ids[i]);
	}
	if (text->len == 0)
		g_string_append(text, "None");

	return g_string_free(text, FALSE);
}

static HtmxNode *
action_cell(
	HtmxRequest        *request,
	AxisModuleRegistry *registry,
	guint               index
){
	HtmxTd *td = htmx_td_new();
	const gchar *id = axis_module_registry_get_id(registry, index);
	gboolean on = axis_module_registry_is_enabled(registry, id);
	g_autoptr(GError) error = NULL;

	if (axis_module_registry_is_required(registry, index)) {
		htmx_node_set_text_content(HTMX_NODE(td), "Always on");
	} else if (!axis_module_registry_check_enabled(registry, id, !on, &error)) {
		HtmxSpan *reason = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(reason), "blocked");
		htmx_node_set_text_content(HTMX_NODE(reason), error->message);
		ADD(td, reason);
	} else {
		g_autofree gchar *action = g_strdup_printf("/admin/modules/%s/%s", id,
			on ? "disable" : "enable");
		HtmxNode *form = axis_web_auth_post_form(request, action);
		g_autofree gchar *words = g_strdup_printf("Switch %s %s",
			axis_module_registry_get_name(registry, index), on ? "off" : "on");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		axis_web_auth_add(form, axis_web_auth_button(words, on ? "button-danger" : NULL));
		ADD(td, form);
	}

	return HTMX_NODE(td);
}

static HtmxResponse *
modules_page(
	AxisWebSettings *self,
	HtmxRequest     *request,
	guint            status,
	gboolean         error,
	const gchar     *notice
){
	AxisModuleRegistry *registry = axis_web_settings_registry(self);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	HtmxNode *card = axis_web_settings_card("Installed modules");
	HtmxP *intro = htmx_p_new();
	/* The last column holds a button, or why there is none. */
	static const AxisWebColumn columns[] = {
		{ "Module", AXIS_WEB_COL_NAME },
		{ "What it does", AXIS_WEB_COL_WIDE },
		{ "Needs", AXIS_WEB_COL_TEXT },
		{ "State", AXIS_WEB_COL_STATE },
		{ "", AXIS_WEB_COL_CONTROLS },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("modules", columns);
	HtmxTbody *body = htmx_tbody_new();
	HtmxResponse *response;
	AxisWebNotice place;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), "Modules");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_page_intro("The parts Axis is made of, each of which can be switched off "
		"when you do not use it: a module that is off has no pages and runs no work."));
	axis_web_notice_begin(&place, request, error, g_strdup(notice), HTMX_NODE(content));

	htmx_node_set_text_content(HTMX_NODE(intro),
		"Switching a module off hides its pages and pauses its work at once. Nothing is "
		"deleted: its data, settings and waiting jobs are kept, and switching it on again "
		"picks up where it stopped.");
	ADD(card, intro);

	for (i = 0; registry != NULL && i < axis_module_registry_get_count(registry); i++) {
		const gchar *id = axis_module_registry_get_id(registry, i);
		gboolean on = axis_module_registry_is_enabled(registry, id);
		HtmxTr *row = htmx_tr_new();
		HtmxTd *name = htmx_td_new();
		HtmxCode *code = htmx_code_new_with_text(id);
		HtmxSpan *state = htmx_span_new();
		HtmxTd *state_cell = htmx_td_new();
		g_autofree gchar *needs = needs_text(registry, i);
		g_autofree gchar *name_text = g_strdup_printf("%s ", axis_module_registry_get_name(registry, i));
		g_autofree gchar *row_id = g_strdup_printf("module-%s", id);

		htmx_node_set_text_content(HTMX_NODE(name), name_text);
		htmx_node_add_child(HTMX_NODE(name), HTMX_NODE(code));
		g_object_unref(code);
		htmx_element_set_id(HTMX_ELEMENT(row), row_id);
		axis_web_notice_put(&place, HTMX_NODE(name), row_id);

		htmx_element_add_class(HTMX_ELEMENT(state), "badge");
		if (on)
			htmx_element_add_class(HTMX_ELEMENT(state), "badge-running");
		htmx_node_set_text_content(HTMX_NODE(state), on ? "On" : "Off");
		ADD(state_cell, state);

		ADD(row, name);
		ADD(row, cell(axis_module_registry_get_description(registry, i)));
		ADD(row, cell(needs));
		ADD(row, state_cell);
		ADD(row, action_cell(request, registry, i));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	axis_web_auth_add(content, card);

	axis_web_notice_end(&place);
	response = axis_web_page_respond(request, "Modules", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
handle_modules(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return modules_page(user_data, request, 200, FALSE,
		done_message(htmx_request_get_query_param(request, "done")));
}

/* ── Switching ──────────────────────────────────────────────────── */

typedef struct
{
	AxisWebSettings *self;
	gchar           *id;
	gboolean         enabled;
	GError          *error;
} SwitchJob;

static void
switch_job_free(gpointer data)
{
	SwitchJob *job = data;

	g_free(job->id);
	g_clear_error(&job->error);
	g_free(job);
}

static void
switch_job_run(gpointer data)
{
	SwitchJob *job = data;

	axis_module_state_store(job->self->pool, job->id, job->enabled, &job->error);
}

static void
audit_switch(
	HtmxRequest *request,
	const gchar *id,
	gboolean     enabled
){
	const AxisAuthState *state = axis_web_auth_state(request);

	axis_web_auth_audit("module %s: module=%s by=%s client=%s", enabled ? "enabled" : "disabled",
		id, state->username, state->client_address);
}

/* Back to the module's row. */
static HtmxResponse *
switched(
	HtmxRequest *request,
	const gchar *id,
	gboolean     enabled
){
	g_autofree gchar *place = g_strdup_printf("module-%s", id);

	return axis_web_place_redirect(request, "/admin/modules", enabled ? "enabled" : "disabled", place);
}

static HtmxResponse *
switch_job_done(
	AxisWebSettings *self,
	HtmxRequest     *request,
	gpointer         data
){
	SwitchJob *job = data;
	g_autoptr(GError) undo = NULL;

	self->module_busy = FALSE;

	if (job->error != NULL) {
		/* Not stored: put the module back as it was, so that what runs is
		 * what a restart would bring. */
		axis_module_registry_set_enabled(axis_web_settings_registry(self), job->id,
			!job->enabled, &undo);
		return axis_web_internal_error(request, job->error);
	}

	audit_switch(request, job->id, job->enabled);
	return switched(request, job->id, job->enabled);
}

static HtmxResponse *
switch_module(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     enabled
){
	AxisWebSettings *self = user_data;
	AxisModuleRegistry *registry = axis_web_settings_registry(self);
	const gchar *id = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	g_autoptr(GError) error = NULL;
	SwitchJob *job;

	if (id == NULL || axis_module_registry_find(registry, id) < 0)
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");

	if (self->module_busy)
		return modules_page(self, request, 409, TRUE,
			"Another change is still being saved. Try again in a moment.");
	if (self->pool != NULL && (self->stopping || self->pending >= AXIS_WEB_SETTINGS_MAX_PENDING))
		return axis_web_error_respond(request, 503, "Axis is busy",
			"Axis is busy. Try again in a few seconds.");

	if (!axis_module_registry_check_enabled(registry, id, enabled, &error)) {
		axis_web_auth_audit("module change refused: module=%s by=%s client=%s", id,
			axis_web_request_get_username(request), axis_web_request_get_client_address(request));
		return modules_page(self, request, 409, TRUE, error->message);
	}

	if (axis_module_registry_is_enabled(registry, id) == enabled)
		return switched(request, id, enabled);

	if (!axis_module_registry_set_enabled(registry, id, enabled, &error)) {
		/* The module's own message may be technical: the log has it. */
		g_warning("could not switch module '%s' %s: %s", id, enabled ? "on" : "off", error->message);
		return modules_page(self, request, 500, TRUE,
			"Axis could not switch the module on or off. The reason is in the Axis log.");
	}

	if (self->pool == NULL) {
		audit_switch(request, id, enabled);
		return switched(request, id, enabled);
	}

	job = g_new0(SwitchJob, 1);
	job->self = self;
	job->id = g_strdup(id);
	job->enabled = enabled;
	self->module_busy = TRUE;

	return axis_web_settings_dispatch(self, request, switch_job_run, switch_job_done, job,
		switch_job_free);
}

static HtmxResponse *
handle_enable(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return switch_module(request, params, user_data, TRUE);
}

static HtmxResponse *
handle_disable(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return switch_module(request, params, user_data, FALSE);
}

void
axis_web_modules_register_page(
	AxisWebSettings *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/admin/modules", AXIS_WEB_ROUTE_ADMIN,
		handle_modules, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/modules/:id/enable", AXIS_WEB_ROUTE_ADMIN,
		handle_enable, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/modules/:id/disable", AXIS_WEB_ROUTE_ADMIN,
		handle_disable, self);
}
