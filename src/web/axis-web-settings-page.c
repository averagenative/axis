/*
 * axis-web-settings-page.c - the Settings page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Generated from the modules' declarations: one card and one form per
 * module that is switched on and has settings.  A form is validated as a
 * whole before anything is stored.  A secret is shown as set or not set and
 * is never put back in a form; the submitted value is used once and wiped.
 * Messages come from fixed keys in the redirect, never from the request.
 */

#include "web/axis-web-settings-private.h"
#include "core/axis-error.h"
#include "web/axis-password.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-setup-path.h"

#include <string.h>

#define ADD axis_web_settings_add

/* ── Pieces ─────────────────────────────────────────────────────── */

static const gchar *
done_message(const gchar *key)
{
	static const struct
	{
		const gchar *key;
		const gchar *text;
	} messages[] = {
		{ "saved",         "The settings have been saved." },
		{ "saved-restart", "The settings have been saved. At least one of them takes effect "
		                   "after the next restart." },
		{ "unchanged",     "Nothing needed saving: the values are as they were." },
		{ "cleared",       "The stored value has been removed." },
		{ NULL, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0)
			return messages[i].text;
	}

	return NULL;
}

/* An element id for a setting key: letters, digits, '-' and '_' only. */
static gchar *
field_id(
	const gchar *key,
	const gchar *suffix
){
	g_autofree gchar *base = g_strdelimit(g_strdup(key), ".", '-');

	return g_strconcat("setting-", base, suffix, NULL);
}

static HtmxNode *
text_paragraph(
	const gchar *css_class,
	const gchar *id,
	const gchar *text
){
	HtmxP *p = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(p), css_class);
	if (id != NULL)
		htmx_element_set_id(HTMX_ELEMENT(p), id);
	htmx_node_set_text_content(HTMX_NODE(p), text);

	return HTMX_NODE(p);
}

/* The editing control of one setting. */
static HtmxNode *
control(
	const AxisSettingInfo *info,
	const gchar           *id,
	const gchar           *described_by,
	const gchar           *value
){
	HtmxNode *node;
	const gchar *text = value != NULL ? value : "";

	if (info->type == AXIS_SETTING_BOOLEAN || info->type == AXIS_SETTING_CHOICE) {
		HtmxSelect *select = htmx_select_new_with_name(info->key);
		guint i;

		if (info->type == AXIS_SETTING_BOOLEAN) {
			HtmxOption *on = htmx_option_new_with_value("true", "On");
			HtmxOption *off = htmx_option_new_with_value("false", "Off");

			htmx_option_set_selected(on, strcmp(text, "true") == 0);
			htmx_option_set_selected(off, strcmp(text, "true") != 0);
			ADD(select, on);
			ADD(select, off);
		}
		for (i = 0; info->type == AXIS_SETTING_CHOICE && info->choices[i] != NULL; i++) {
			HtmxOption *option = htmx_option_new_with_value(info->choices[i],
				axis_setting_choice_label(info, info->choices[i]));

			htmx_option_set_selected(option, strcmp(text, info->choices[i]) == 0);
			ADD(select, option);
		}
		node = HTMX_NODE(select);
	} else if (info->type == AXIS_SETTING_STRING_LIST) {
		HtmxTextarea *area = htmx_textarea_new_with_name(info->key);

		htmx_textarea_set_rows(area, 5);
		htmx_node_set_text_content(HTMX_NODE(area), text);
		node = HTMX_NODE(area);
	} else if (info->flags & AXIS_SETTING_SECRET) {
		HtmxInput *input = htmx_input_new(HTMX_INPUT_PASSWORD);

		/* Never a value: only the way to replace it. */
		htmx_input_set_name(input, info->key);
		htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "new-password");
		htmx_input_set_placeholder(input, "Type a new value to replace it");
		node = HTMX_NODE(input);
	} else {
		HtmxInput *input = htmx_input_new(HTMX_INPUT_TEXT);

		htmx_input_set_name(input, info->key);
		htmx_input_set_value(input, text);
		htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "off");
		if (info->type == AXIS_SETTING_INTEGER)
			htmx_element_set_attribute(HTMX_ELEMENT(input), "inputmode", "numeric");
		else if (info->type == AXIS_SETTING_DECIMAL)
			htmx_element_set_attribute(HTMX_ELEMENT(input), "inputmode", "decimal");
		node = HTMX_NODE(input);
	}

	htmx_element_set_id(HTMX_ELEMENT(node), id);
	if (described_by != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(node), "aria-describedby", described_by);
	return node;
}

/*
 * One setting's field: label, control, help, and what a person should
 * know (the default, a restart, a secret's state).  @reason is the error
 * to show beside it; @submitted the text to show instead of the stored
 * value.
 */
static HtmxNode *
setting_field(
	AxisSettings          *settings,
	const AxisSettingInfo *info,
	const gchar           *reason,
	const gchar           *submitted
){
	HtmxDiv *field = htmx_div_new();
	g_autofree gchar *id = field_id(info->key, "");
	HtmxLabel *label = htmx_label_new_for(id);
	g_autofree gchar *help_id = field_id(info->key, "-help");
	g_autofree gchar *error_id = field_id(info->key, "-error");
	g_autofree gchar *described_by = reason != NULL ?
		(info->help != NULL ? g_strdup_printf("%s %s", help_id, error_id) : g_strdup(error_id)) :
		(info->help != NULL ? g_strdup(help_id) : NULL);
	g_autofree gchar *current = NULL;
	g_autofree gchar *fallback = NULL;
	GString *meta = g_string_new(NULL);
	gboolean secret = (info->flags & AXIS_SETTING_SECRET) != 0;

	htmx_node_set_text_content(HTMX_NODE(label), info->label);
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_element_add_class(HTMX_ELEMENT(field), "setting");
	if (reason != NULL)
		htmx_element_add_class(HTMX_ELEMENT(field), "setting-invalid");

	if (!secret) {
		current = submitted != NULL ? g_strdup(submitted) : axis_settings_format(settings, info->key);
		fallback = axis_settings_format_default(settings, info->key);
	}

	ADD(field, label);
	ADD(field, control(info, id, described_by, current));
	if (info->help != NULL)
		ADD(field, text_paragraph("help", help_id, info->help));

	if (reason != NULL) {
		HtmxNode *error = text_paragraph("field-error", error_id, reason);

		htmx_element_set_attribute(HTMX_ELEMENT(error), "role", "alert");
		ADD(field, error);
	}

	if (secret) {
		g_string_append(meta, axis_settings_is_set(settings, info->key) ?
			"A value is stored. Type a new one to replace it; leave this empty to keep it." :
			"No value is stored.");
	} else if (fallback != NULL && info->type != AXIS_SETTING_STRING_LIST) {
		const gchar *shown = fallback;

		if (info->type == AXIS_SETTING_BOOLEAN)
			shown = strcmp(fallback, "true") == 0 ? "On" : "Off";
		else if (info->type == AXIS_SETTING_CHOICE)
			shown = axis_setting_choice_label(info, fallback);
		else if (fallback[0] == '\0')
			shown = "empty";
		g_string_append_printf(meta, "Default: %s.", shown);
	}
	if (info->flags & AXIS_SETTING_RESTART) {
		g_string_append(meta, meta->len > 0 ? " " : "");
		g_string_append(meta, axis_settings_restart_pending(settings, info->key) ?
			"The stored value takes effect after the next restart." :
			"A change takes effect after a restart.");
	}
	if (meta->len > 0)
		ADD(field, text_paragraph("setting-meta", NULL, meta->str));
	g_string_free(meta, TRUE);

	return HTMX_NODE(field);
}

/*
 * A text setting offered as a list to pick one of: a group of radio
 * buttons, each with its name and a line about it, the stored value (or the
 * one submitted) picked.  It posts the value as the field would.
 */
static HtmxNode *
choice_field(
	AxisSettings              *settings,
	const AxisSettingInfo     *info,
	const AxisWebSettingsForm *form,
	const gchar               *reason,
	const gchar               *submitted
){
	HtmxFieldset *field = htmx_fieldset_new();
	HtmxDiv *list = htmx_div_new();
	g_autofree gchar *current = submitted != NULL ? g_strdup(submitted) : axis_settings_format(settings, info->key);
	g_autofree gchar *base = field_id(info->key, "");
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_element_add_class(HTMX_ELEMENT(field), "setting");
	htmx_element_add_class(HTMX_ELEMENT(field), "setting-choice");
	if (reason != NULL)
		htmx_element_add_class(HTMX_ELEMENT(field), "setting-invalid");
	ADD(field, htmx_legend_new_with_text(info->label));
	htmx_element_add_class(HTMX_ELEMENT(list), "choice-list");
	for (i = 0; i < form->n_choices; i++) {
		const AxisWebSettingsChoice *choice = &form->choices[i];
		g_autofree gchar *id = g_strdup_printf("%s-%u", base, i);
		HtmxLabel *row = htmx_label_new_for(id);
		HtmxInput *input = htmx_input_new(HTMX_INPUT_RADIO);
		HtmxSpan *text = htmx_span_new();
		HtmxSpan *name = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(row), "choice");
		htmx_element_set_id(HTMX_ELEMENT(input), id);
		htmx_input_set_name(input, info->key);
		htmx_input_set_value(input, choice->value);
		if (g_strcmp0(current, choice->value) == 0)
			htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
		htmx_element_add_class(HTMX_ELEMENT(text), "choice-text");
		htmx_element_add_class(HTMX_ELEMENT(name), "choice-name");
		htmx_node_set_text_content(HTMX_NODE(name), choice->label);
		ADD(text, name);
		if (choice->detail != NULL) {
			HtmxSpan *detail = htmx_span_new();

			htmx_element_add_class(HTMX_ELEMENT(detail), "choice-detail");
			htmx_node_set_text_content(HTMX_NODE(detail), choice->detail);
			ADD(text, detail);
		}
		ADD(row, input);
		ADD(row, text);
		ADD(list, row);
	}
	ADD(field, list);
	if (reason != NULL) {
		g_autofree gchar *error_id = field_id(info->key, "-error");
		HtmxNode *error = text_paragraph("field-error", error_id, reason);

		htmx_element_set_attribute(HTMX_ELEMENT(error), "role", "alert");
		ADD(field, error);
	}
	return HTMX_NODE(field);
}

static gboolean
form_shows(
	const AxisWebSettingsForm *form,
	const gchar               *key
){
	return form == NULL || form->keys == NULL || g_strv_contains(form->keys, key);
}

HtmxNode *
axis_web_settings_form_new(
	AxisSettings              *settings,
	HtmxRequest               *request,
	const gchar               *module_id,
	const gchar               *module_name,
	const AxisSettingInfo     *table,
	const AxisWebSettingsForm *form
){
	g_autofree gchar *action = NULL;
	g_autofree gchar *button = NULL;
	HtmxNode *node;
	HtmxNode *back;
	guint i;

	g_return_val_if_fail(settings != NULL && module_id != NULL && table != NULL, NULL);

	action = g_strdup_printf("/admin/settings/%s", module_id);
	button = form != NULL && form->button != NULL ? g_strdup(form->button) :
		g_strdup_printf("Save %s settings", module_name != NULL ? module_name : module_id);
	node = axis_web_auth_post_form(request, action);
	htmx_element_add_class(HTMX_ELEMENT(node), "form");
	for (i = 0; table[i].key != NULL; i++) {
		const gchar *reason = form != NULL && form->reasons != NULL ?
			g_hash_table_lookup(form->reasons, table[i].key) : NULL;
		const gchar *submitted = form != NULL && form->texts != NULL ?
			g_hash_table_lookup(form->texts, table[i].key) : NULL;

		if (!form_shows(form, table[i].key))
			continue;
		if (form != NULL && form->choice_key != NULL && strcmp(form->choice_key, table[i].key) == 0 &&
		    form->n_choices > 0 && (table[i].flags & AXIS_SETTING_SECRET) == 0) {
			htmx_element_add_class(HTMX_ELEMENT(node), "form-choices");
			axis_web_auth_add(node, choice_field(settings, &table[i], form, reason, submitted));
		} else
			axis_web_auth_add(node, setting_field(settings, &table[i], reason, submitted));
	}
	back = axis_web_setup_return_field(form != NULL ? form->return_path : NULL);
	if (back != NULL)
		axis_web_auth_add(node, back);
	axis_web_auth_add(node, axis_web_auth_button(button, "button-primary"));
	return node;
}

/* The forms that remove stored secrets, after the module's own form (a
 * second submit button inside it would be what Enter presses). */
static HtmxNode *
clear_forms(
	AxisWebSettings       *self,
	HtmxRequest           *request,
	const gchar           *module_id,
	const AxisSettingInfo *table
){
	HtmxDiv *block = NULL;
	g_autofree gchar *action = g_strdup_printf("/admin/settings/%s/clear", module_id);
	guint i;

	for (i = 0; table[i].key != NULL; i++) {
		HtmxNode *form;
		HtmxNode *eraser;
		HtmxDiv *row;
		HtmxSpan *name;
		g_autofree gchar *text = NULL;
		g_autofree gchar *erase_text = NULL;

		if (!(table[i].flags & AXIS_SETTING_SECRET) ||
		    !axis_settings_is_set(self->settings, table[i].key))
			continue;

		if (block == NULL) {
			block = htmx_div_new();
			htmx_element_add_class(HTMX_ELEMENT(block), "secret-list");
		}
		row = htmx_div_new();
		name = htmx_span_new();
		form = axis_web_auth_post_form(request, action);
		text = g_strdup_printf("%s is stored.", table[i].label);
		erase_text = g_strdup_printf("Remove the stored %s", table[i].label);

		htmx_element_add_class(HTMX_ELEMENT(row), "secret-row");
		htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
		htmx_node_set_text_content(HTMX_NODE(name), text);
		axis_web_auth_add(form, axis_web_auth_hidden("key", table[i].key));
		eraser = axis_web_auth_button(erase_text, "button-danger");
		htmx_element_add_class(HTMX_ELEMENT(eraser), "button-small");
		axis_web_auth_add(form, eraser);
		ADD(row, name);
		ADD(row, form);
		ADD(block, row);
	}

	return block != NULL ? HTMX_NODE(block) : NULL;
}

/* ── The page ───────────────────────────────────────────────────── */

/*
 * @reasons: setting key -> what is wrong with the submitted value (nullable)
 * @texts: setting key -> the submitted text, shown again (nullable)
 * @back_module, @back: the module whose form came from the setup guide, and
 *   where in it (both nullable): that form goes back there when saved
 */
static HtmxResponse *
settings_page(
	AxisWebSettings *self,
	HtmxRequest     *request,
	guint            status,
	gboolean         error,
	const gchar     *notice,
	GHashTable      *reasons,
	GHashTable      *texts,
	const gchar     *back_module,
	const gchar     *back
){
	AxisModuleRegistry *registry = axis_web_settings_registry(self);
	AxisWebAuth *auth = axis_web_auth_from_request(request);
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	g_autofree gchar *hint = auth != NULL ? axis_web_auth_forwarding_hint(auth) : NULL;
	HtmxResponse *response;
	AxisWebNotice place;
	guint shown = 0;
	guint i;

	htmx_node_set_text_content(HTMX_NODE(title), "Settings");
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	axis_web_auth_add(content, axis_web_page_intro("Everything an administrator can set, part by part: keys for "
		"outside services, schedules, limits and defaults. Each setting says what it does and what it is now."));
	axis_web_notice_begin(&place, request, error, g_strdup(notice), HTMX_NODE(content));
	/* A proxy that should be trusted and is not: every client looks like
	 * it, so they share one login throttle; and what the connection limits
	 * refused. */
	if (hint != NULL)
		axis_web_auth_add(content, axis_web_auth_notice(TRUE, hint));

	for (i = 0; registry != NULL && i < axis_module_registry_get_count(registry); i++) {
		const AxisSettingInfo *table = axis_module_registry_get_settings(registry, i);
		const gchar *module_id = axis_module_registry_get_id(registry, i);
		const gchar *description = axis_module_registry_get_description(registry, i);
		HtmxNode *card;
		AxisWebSettingsForm form;
		g_autofree gchar *anchor = NULL;
		g_autofree gchar *card_title = NULL;
		HtmxNode *clears;

		if (table == NULL || table[0].key == NULL || !axis_module_registry_is_enabled(registry, module_id))
			continue;
		shown++;

		anchor = g_strdup_printf("module-%s", module_id);
		card_title = g_strdup_printf("%s settings", axis_module_registry_get_name(registry, i));
		card = axis_web_settings_card(card_title);
		htmx_element_set_id(HTMX_ELEMENT(card), anchor);
		axis_web_notice_put(&place, card, anchor);
		if (description != NULL)
			ADD(card, text_paragraph("help", NULL, description));

		memset(&form, 0, sizeof(form));
		form.reasons = reasons;
		form.texts = texts;
		if (g_strcmp0(back_module, module_id) == 0) {
			form.return_path = back;
			if (back != NULL)
				ADD(card, axis_web_setup_back_note(back));
		}
		axis_web_auth_add(card, axis_web_settings_form_new(self->settings, request, module_id,
			axis_module_registry_get_name(registry, i), table, &form));

		clears = clear_forms(self, request, module_id, table);
		if (clears != NULL)
			axis_web_auth_add(card, clears);

		axis_web_auth_add(content, card);
	}

	if (shown == 0) {
		HtmxP *none = htmx_p_new();
		HtmxSpan *lead = htmx_span_new();
		HtmxA *modules_link = htmx_a_new_with_href("/admin/modules");
		HtmxSpan *stop = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(none), "help");
		htmx_node_set_text_content(HTMX_NODE(lead),
			"None of the modules that are switched on has settings. Switch modules on under ");
		htmx_node_set_text_content(HTMX_NODE(modules_link), "Modules");
		htmx_node_set_text_content(HTMX_NODE(stop), ".");
		ADD(none, lead);
		ADD(none, modules_link);
		ADD(none, stop);
		axis_web_auth_add(content, none);
	}

	axis_web_notice_end(&place);
	response = axis_web_page_respond(request, "Settings", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	return response;
}

static HtmxResponse *
handle_settings(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return settings_page(user_data, request, 200, FALSE,
		done_message(htmx_request_get_query_param(request, "done")), NULL, NULL, NULL, NULL);
}

/* ── Saving ─────────────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *keys;	/* of gchar* */
	GPtrArray *texts;	/* of gchar*, wiped when freed */
	GStrv      changed;	/* out */
	GPtrArray *problems;	/* out */
	GError    *error;	/* out */
} SaveWork;

static void
save_free(gpointer data)
{
	SaveWork *work = data;

	g_ptr_array_unref(work->keys);
	g_ptr_array_unref(work->texts);
	g_strfreev(work->changed);
	if (work->problems != NULL)
		g_ptr_array_unref(work->problems);
	g_clear_error(&work->error);
	g_free(work);
}

/* The state the worker needs besides the work itself. */
typedef struct
{
	AxisWebSettings *self;
	SaveWork        *work;
	gchar           *module_id;
	const gchar     *back;	/* the setup guide's address (static), or NULL */
} SaveJob;

static void
save_job_free(gpointer data)
{
	SaveJob *job = data;

	save_free(job->work);
	g_free(job->module_id);
	g_free(job);
}

static void
save_job_run(gpointer data)
{
	SaveJob *job = data;
	SaveWork *work = job->work;
	g_autofree AxisSettingChange *changes = g_new0(AxisSettingChange, work->keys->len + 1);
	guint i;

	for (i = 0; i < work->keys->len; i++) {
		changes[i].key = g_ptr_array_index(work->keys, i);
		changes[i].text = g_ptr_array_index(work->texts, i);
	}

	axis_settings_set_many(job->self->settings, changes, work->keys->len, &work->changed,
		&work->problems, &work->error);
}

static HtmxResponse *
save_job_done(
	AxisWebSettings *self,
	HtmxRequest     *request,
	gpointer         data
){
	SaveJob *job = data;
	SaveWork *work = job->work;
	const AxisAuthState *state = axis_web_auth_state(request);
	gboolean restart = FALSE;
	guint i;

	if (work->error != NULL && g_error_matches(work->error, AXIS_ERROR, AXIS_ERROR_SETTING)) {
		/* Only if the declarations changed under us: show what was refused. */
		g_autoptr(GHashTable) reasons = g_hash_table_new(g_str_hash, g_str_equal);

		for (i = 0; work->problems != NULL && i < work->problems->len; i++) {
			const AxisSettingProblem *problem = g_ptr_array_index(work->problems, i);

			g_hash_table_insert(reasons, problem->key, problem->reason);
		}
		return settings_page(self, request, 400, TRUE,
			"Nothing was saved. Correct the marked settings and try again.", reasons, NULL, job->module_id, job->back);
	}
	if (work->error != NULL) {
		/* The message names no value (see axis_settings_set_many()). */
		return axis_web_internal_error(request, work->error);
	}

	for (i = 0; work->changed != NULL && work->changed[i] != NULL; i++) {
		const AxisSettingInfo *info = axis_settings_lookup(self->settings, work->changed[i], NULL);

		if (info != NULL && (info->flags & AXIS_SETTING_RESTART))
			restart = TRUE;
		axis_web_auth_audit("setting changed: key=%s by=%s client=%s", work->changed[i],
			state->username, state->client_address);
	}

	if (job->back != NULL) {
		g_autofree gchar *location = axis_web_setup_return_location(job->back,
			work->changed == NULL || work->changed[0] == NULL ? "unchanged" : restart ? "saved-restart" : "saved");

		return axis_web_auth_redirect(request, location);
	}
	{
		g_autofree gchar *place = g_strdup_printf("module-%s", job->module_id);

		return axis_web_place_redirect(request, "/admin/settings",
			work->changed == NULL || work->changed[0] == NULL ? "unchanged" : restart ? "saved-restart" : "saved",
			place);
	}
}

static HtmxResponse *
handle_save(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebSettings *self = user_data;
	AxisModuleRegistry *registry = axis_web_settings_registry(self);
	const gchar *module_id = params != NULL ? g_hash_table_lookup(params, "module") : NULL;
	gint index = module_id != NULL ? axis_module_registry_find(registry, module_id) : -1;
	const AxisSettingInfo *table;
	g_autoptr(GHashTable) reasons = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	g_autoptr(GHashTable) shown = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
	SaveWork *work;
	SaveJob *job;
	guint i;

	if (index < 0 || !axis_module_registry_is_enabled(registry, module_id) ||
	    (table = axis_module_registry_get_settings(registry, (guint)index)) == NULL)
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");

	work = g_new0(SaveWork, 1);
	work->keys = g_ptr_array_new_with_free_func(g_free);
	work->texts = g_ptr_array_new_with_free_func((GDestroyNotify)axis_secret_free);

	for (i = 0; table[i].key != NULL; i++) {
		const gchar *text = htmx_request_get_form_value(request, table[i].key);
		AxisSettingProblem *problem = NULL;

		if (text == NULL)
			continue;
		/* A secret is replaced by typing a new value; empty keeps it. */
		if ((table[i].flags & AXIS_SETTING_SECRET) && text[0] == '\0')
			continue;

		if (!(table[i].flags & AXIS_SETTING_SECRET))
			g_hash_table_insert(shown, (gpointer)table[i].key, g_strdup(text));
		if (!axis_settings_check(self->settings, table[i].key, text, &problem)) {
			g_hash_table_insert(reasons, (gpointer)table[i].key, g_strdup(problem->reason));
			axis_setting_problem_free(problem);
			continue;
		}
		g_ptr_array_add(work->keys, g_strdup(table[i].key));
		g_ptr_array_add(work->texts, g_strdup(text));
	}

	if (g_hash_table_size(reasons) > 0) {
		HtmxResponse *response = settings_page(self, request, 400, TRUE,
			"Nothing was saved. Correct the marked settings and try again.", reasons, shown, module_id,
			axis_web_setup_request_return(request));

		save_free(work);
		return response;
	}

	job = g_new0(SaveJob, 1);
	job->self = self;
	job->work = work;
	job->module_id = g_strdup(module_id);
	job->back = axis_web_setup_request_return(request);

	return axis_web_settings_dispatch(self, request, save_job_run, save_job_done, job,
		save_job_free);
}

/* ── Removing a secret ──────────────────────────────────────────── */

typedef struct
{
	AxisWebSettings *self;
	gchar           *key;
	gchar           *module_id;
	GError          *error;
} ClearJob;

static void
clear_job_free(gpointer data)
{
	ClearJob *job = data;

	g_free(job->key);
	g_free(job->module_id);
	g_clear_error(&job->error);
	g_free(job);
}

static void
clear_job_run(gpointer data)
{
	ClearJob *job = data;

	axis_settings_clear(job->self->settings, job->key, &job->error);
}

static HtmxResponse *
clear_job_done(
	AxisWebSettings *self,
	HtmxRequest     *request,
	gpointer         data
){
	ClearJob *job = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autofree gchar *place = NULL;

	if (job->error != NULL)
		return axis_web_internal_error(request, job->error);

	axis_web_auth_audit("setting cleared: key=%s by=%s client=%s", job->key, state->username,
		state->client_address);
	place = g_strdup_printf("module-%s", job->module_id);
	return axis_web_place_redirect(request, "/admin/settings", "cleared", place);
}

static HtmxResponse *
handle_clear(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebSettings *self = user_data;
	AxisModuleRegistry *registry = axis_web_settings_registry(self);
	const gchar *module_id = params != NULL ? g_hash_table_lookup(params, "module") : NULL;
	const gchar *key = htmx_request_get_form_value(request, "key");
	gint index = module_id != NULL ? axis_module_registry_find(registry, module_id) : -1;
	const AxisSettingInfo *table = index >= 0 ?
		axis_module_registry_get_settings(registry, (guint)index) : NULL;
	ClearJob *job;
	guint i;

	if (table == NULL || key == NULL || !axis_module_registry_is_enabled(registry, module_id))
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");

	/* Only a secret of this module can be removed from here. */
	for (i = 0; table[i].key != NULL; i++) {
		if (strcmp(table[i].key, key) == 0 && (table[i].flags & AXIS_SETTING_SECRET))
			break;
	}
	if (table[i].key == NULL)
		return axis_web_error_respond(request, 404, "Not found",
			"There is no page at this address. The link may be old, or what it showed was removed.");

	job = g_new0(ClearJob, 1);
	job->self = self;
	job->key = g_strdup(key);
	job->module_id = g_strdup(module_id);

	return axis_web_settings_dispatch(self, request, clear_job_run, clear_job_done, job,
		clear_job_free);
}

void
axis_web_settings_register_settings_page(
	AxisWebSettings *self,
	AxisWeb         *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/admin/settings", AXIS_WEB_ROUTE_ADMIN,
		handle_settings, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/settings/:module", AXIS_WEB_ROUTE_ADMIN,
		handle_save, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/admin/settings/:module/clear",
		AXIS_WEB_ROUTE_ADMIN, handle_clear, self);
}
