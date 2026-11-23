/*
 * axis-web-quality.c - the quality pages module: its work off the main
 * loop, the pieces its pages share
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-quality-private.h"

#include <string.h>

#include "db/axis-db-error.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebQuality         *owner;
	AxisWebQualityWorkFunc  work;
	AxisWebQualityDoneFunc  done;
	HtmxRequest            *request;
	HtmxResponse           *deferred;
	gpointer                data;
	GDestroyNotify          free_data;
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
axis_web_quality_dispatch(
	AxisWebQuality         *self,
	HtmxRequest            *request,
	AxisWebQualityWorkFunc  work,
	AxisWebQualityDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
){
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_QUALITY_MAX_PENDING) {
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

AxisWebQuality *
axis_web_quality_new(AxisQualityStore *store)
{
	AxisWebQuality *self;

	g_return_val_if_fail(store != NULL, NULL);

	self = g_new0(AxisWebQuality, 1);
	self->store = axis_quality_store_ref(store);
	return self;
}

void
axis_web_quality_set_compat(
	AxisWebQuality *self,
	AxisCompat     *compat
){
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->compat, axis_compat_unref);
	if (compat != NULL)
		self->compat = axis_compat_ref(compat);
}

void
axis_web_quality_stop(AxisWebQuality *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_quality_free(AxisWebQuality *self)
{
	if (self == NULL)
		return;

	axis_web_quality_stop(self);
	g_clear_pointer(&self->compat, axis_compat_unref);
	axis_quality_store_unref(self->store);
	g_free(self);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

HtmxResponse *
axis_web_quality_error(
	HtmxRequest  *request,
	const GError *error
){
	if (error->domain == AXIS_QUALITY_ERROR) {
		switch (error->code) {
		case AXIS_QUALITY_ERROR_NOT_FOUND:
			return axis_web_error_respond(request, 404, "Not found", error->message);
		case AXIS_QUALITY_ERROR_BUILT_IN:
			return axis_web_error_respond(request, 403, "Built in", error->message);
		case AXIS_QUALITY_ERROR_INVALID:
			return axis_web_error_respond(request, 422, "Not saved", error->message);
		default:
			return axis_web_error_respond(request, 409, "Not done", error->message);
		}
	}
	if (g_error_matches(error, AXIS_DB_ERROR, AXIS_DB_ERROR_TIMEOUT))
		return axis_web_error_respond(request, 503, "Too slow", "The profiles could not be read in time. Try again "
			"in a moment.");
	return axis_web_internal_error(request, error);
}

gchar *
axis_web_quality_reflect(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text != NULL ? text : ""; *p != '\0' && out->len < 600; p++) {
		if (g_ascii_isalnum(*p) || strchr(".,_ -+", *p) != NULL)
			g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}

gchar *
axis_web_quality_href(const gchar *slug)
{
	return g_strdup_printf("/quality/%s", slug);
}

HtmxNode *
axis_web_quality_copy_form(
	HtmxRequest             *request,
	const AxisQualityStored *stored
){
	g_autofree gchar *action = g_strdup_printf("/quality/%s/copy", stored->slug);
	g_autofree gchar *id = g_strdup_printf("quality-copy-name-%s", stored->slug);
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "Name of the copy");
	htmx_input_set_name(input, "name");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "maxlength", "80");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "placeholder", "Copy of this one");
	BROWSER_ADD(field, label);
	BROWSER_ADD(field, input);
	axis_web_auth_add(form, field);
	axis_web_auth_add(form, axis_web_auth_button("Copy this profile", NULL));
	return form;
}

/* A labelled text input of the Try form. */
static HtmxNode *
try_field(
	const gchar *id,
	const gchar *label_text,
	const gchar *name,
	const gchar *value,
	const gchar *placeholder,
	const gchar *css
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);
	g_autofree gchar *safe = NULL;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(field), css);
	htmx_node_set_text_content(HTMX_NODE(label), label_text);
	htmx_input_set_name(input, name);
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "spellcheck", "false");
	if (placeholder != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "placeholder", placeholder);
	if (value != NULL && value[0] != '\0') {
		/* A release name goes back as typed, cut to the parser's limit,
		 * without < and > (no release name has them; the element builder
		 * escapes only quotes and ampersands in an attribute); the other
		 * fields are cut to their characters. */
		if (strcmp(name, "name") == 0) {
			gchar *p;

			safe = g_utf8_make_valid(value, MIN(strlen(value), AXIS_WEB_QUALITY_NAME_MAX));
			for (p = safe; *p != '\0'; p++) {
				if (*p == '<' || *p == '>' || (guchar)*p < 0x20)
					*p = ' ';
			}
		} else {
			safe = axis_web_quality_reflect(value);
		}
		htmx_input_set_value(input, safe);
	}
	BROWSER_ADD(field, label);
	BROWSER_ADD(field, input);
	return HTMX_NODE(field);
}

HtmxNode *
axis_web_quality_try_form(
	GPtrArray   *profiles,
	const gchar *name,
	const gchar *slug,
	const gchar *runtime,
	const gchar *size
){
	HtmxForm *form = htmx_form_new();
	HtmxDiv *row = htmx_div_new();
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for("quality-try-profile");
	HtmxSelect *select = htmx_select_new_with_name("profile");
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/quality/try");
	htmx_element_add_class(HTMX_ELEMENT(form), "quality-try");
	BROWSER_ADD(form, try_field("quality-try-name", "Release name", "name", name,
		"Some.Movie.2024.1080p.WEB-DL.DDP5.1.H.264-GROUP", "quality-try-name"));

	htmx_element_add_class(HTMX_ELEMENT(row), "quality-try-row");
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "Quality profile");
	htmx_element_set_id(HTMX_ELEMENT(select), "quality-try-profile");
	for (i = 0; profiles != NULL && i < profiles->len; i++) {
		const AxisQualityStored *p = g_ptr_array_index(profiles, i);
		HtmxOption *option;

		if (!p->usable)
			continue;
		option = htmx_option_new_with_value(p->slug, p->name);
		htmx_option_set_selected(option, g_strcmp0(slug, p->slug) == 0);
		BROWSER_ADD(select, option);
	}
	BROWSER_ADD(field, label);
	BROWSER_ADD(field, select);
	BROWSER_ADD(row, field);
	BROWSER_ADD(row, try_field("quality-try-runtime", "Runtime (minutes)", "runtime", runtime, "optional", NULL));
	BROWSER_ADD(row, try_field("quality-try-size", "Size", "size", size, "optional, such as 2.9 GiB", NULL));
	BROWSER_ADD(row, axis_web_auth_button("Try this name","button-primary"));
	BROWSER_ADD(form, row);
	BROWSER_ADD(form, axis_web_storage_text("help", "The name is read here and decided with the profile: nothing is "
		"searched, sent or downloaded. Without a runtime only the profile's limit for any release is checked; "
		"without a size, no size at all."));
	return HTMX_NODE(form);
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_quality_register_pages(user_data, web);
	axis_web_quality_register_form(user_data, web);
}

static const AxisNavEntry nav_entries[] = {
	{ "Quality profiles", "/quality", 16, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_quality_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_QUALITY_MODULE_ID;
		info.name = "Quality profiles";
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Quality profiles: which releases of a title are acceptable, which is best, and when one "
			"is worth fetching over the file on disk, with every decision's reasons; the built-in profiles and an "
			"administrator's copies, and a box to try a release name against one. It searches and downloads "
			"nothing.";
		info.settings = axis_quality_store_settings();
	}
	return &info;
}
