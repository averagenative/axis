/*
 * axis-web-settings-presets.c - the quick choices on the History settings card
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * One row of buttons ("Keep history for": 1 month ... Forever) above the
 * card's four fields.  A button saves the same time to all four keeping
 * times in one step; the button that matches all four current values is
 * marked as the current one, and the row says "Custom" when the four differ
 * or match none.  Only the values in the table below are accepted: the
 * request names one of them, never a duration of its own.
 */

#include "web/axis-web-settings-private.h"
#include "core/axis-error.h"
#include "jobs/axis-history.h"
#include "web/axis-web-place.h"

#include <string.h>

#define ADD axis_web_settings_add

typedef struct
{
	const gchar *value;	/* what the form carries and what is stored */
	const gchar *label;
	gint64       seconds;
} Preset;

#define DAY (24 * 3600)

static const Preset presets[] = {
	{ "30d",  "1 month",  30 * (gint64)DAY },
	{ "90d",  "3 months", 90 * (gint64)DAY },
	{ "180d", "6 months", 180 * (gint64)DAY },
	{ "365d", "1 year",   365 * (gint64)DAY },
	{ "0",    "Forever",  0 },
};

static const gchar *const keys[] = {
	AXIS_HISTORY_SETTING_KEEP_EVENTS,
	AXIS_HISTORY_SETTING_KEEP_GRABS,
	AXIS_HISTORY_SETTING_KEEP_DOWNLOADS,
	AXIS_HISTORY_SETTING_KEEP_IMPORTS,
};

/* The preset all four settings are at, or -1. */
static gint
current_preset(AxisSettings *settings)
{
	guint i;
	guint k;

	for (i = 0; i < G_N_ELEMENTS(presets); i++) {
		gboolean all = TRUE;

		for (k = 0; k < G_N_ELEMENTS(keys) && all; k++)
			all = axis_settings_get_duration(settings, keys[k]) == presets[i].seconds;
		if (all)
			return (gint)i;
	}
	return -1;
}

HtmxNode *
axis_web_settings_presets_row(
	AxisSettings *settings,
	HtmxRequest  *request,
	const gchar  *module_id
){
	HtmxDiv *row;
	HtmxP *label;
	HtmxDiv *buttons;
	gint current;
	guint i;

	if (g_strcmp0(module_id, AXIS_HISTORY_MODULE_ID) != 0)
		return NULL;

	current = current_preset(settings);
	row = htmx_div_new();
	label = htmx_p_new();
	buttons = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(row), "preset-row");
	htmx_element_add_class(HTMX_ELEMENT(label), "help");
	htmx_node_set_text_content(HTMX_NODE(label), current < 0 ?
		"Keep history for: Custom. The four times below are not all the same. Choose a time to set all four." :
		"Keep history for: choose a time to set all four times below at once.");
	ADD(row, label);

	htmx_element_add_class(HTMX_ELEMENT(buttons), "actions");
	for (i = 0; i < G_N_ELEMENTS(presets); i++) {
		HtmxNode *form = axis_web_auth_post_form(request, "/admin/settings/history/keep");
		HtmxNode *button = axis_web_auth_button(presets[i].label,
			(gint)i == current ? "button-primary" : "button-quiet");

		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		htmx_element_add_class(HTMX_ELEMENT(button), "button-small");
		if ((gint)i == current) {
			htmx_element_add_class(HTMX_ELEMENT(button), "preset-current");
			htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-current", "true");
		}
		axis_web_auth_add(form, axis_web_auth_hidden("preset", presets[i].value));
		axis_web_auth_add(form, button);
		ADD(buttons, form);
	}
	ADD(row, buttons);
	return HTMX_NODE(row);
}

/* ── Saving ─────────────────────────────────────────────────────── */

typedef struct
{
	AxisWebSettings *self;
	const Preset    *preset;
	GStrv            changed;	/* out */
	GError          *error;		/* out */
} Job;

static void
job_free(gpointer data)
{
	Job *job = data;

	g_strfreev(job->changed);
	g_clear_error(&job->error);
	g_free(job);
}

static void
job_run(gpointer data)
{
	Job *job = data;
	AxisSettingChange changes[G_N_ELEMENTS(keys)];
	guint i;

	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		changes[i].key = keys[i];
		changes[i].text = job->preset->value;
	}
	axis_settings_set_many(job->self->settings, changes, G_N_ELEMENTS(keys), &job->changed, NULL,
		&job->error);
}

static HtmxResponse *
job_done(
	AxisWebSettings *self,
	HtmxRequest     *request,
	gpointer         data
){
	Job *job = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	guint i;

	if (job->error != NULL)
		return axis_web_internal_error(request, job->error);
	for (i = 0; job->changed != NULL && job->changed[i] != NULL; i++)
		axis_web_auth_audit("setting changed: key=%s by=%s client=%s", job->changed[i], state->username,
			state->client_address);
	return axis_web_place_redirect(request, "/admin/settings",
		job->changed == NULL || job->changed[0] == NULL ? "unchanged" : "saved", "module-" AXIS_HISTORY_MODULE_ID);
}

static HtmxResponse *
handle_keep(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebSettings *self = user_data;
	AxisModuleRegistry *registry = axis_web_settings_registry(self);
	const gchar *choice = htmx_request_get_form_value(request, "preset");
	const Preset *preset = NULL;
	Job *job;
	guint i;

	if (registry == NULL || !axis_module_registry_is_enabled(registry, AXIS_HISTORY_MODULE_ID))
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");
	for (i = 0; choice != NULL && i < G_N_ELEMENTS(presets); i++) {
		if (strcmp(choice, presets[i].value) == 0)
			preset = &presets[i];
	}
	if (preset == NULL)
		return axis_web_error_respond(request, 400, "Unknown choice",
			"That is not one of the times to choose from. Nothing was changed.");

	job = g_new0(Job, 1);
	job->self = self;
	job->preset = preset;
	return axis_web_settings_dispatch(self, request, job_run, job_done, job, job_free);
}

void
axis_web_settings_register_presets(
	AxisWebSettings *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/settings/history/keep", AXIS_WEB_ROUTE_ADMIN,
		handle_keep, self);
}
