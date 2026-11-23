/*
 * axis-web-arr.c - "Migrate from Radarr or Sonarr": the module, its jobs,
 * and the pages of the connections (the list, one connection, its test, its
 * reading).  A connection's pages name its own app and what that app holds
 * (movies, or series); a Radarr's page never names Sonarr, nor the reverse.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Administrators only.  A connection's API key is never put back in a page:
 * its field is a password field without a value, typed into only to replace
 * the stored key, and what was typed is wiped once it has been used.  A
 * refused form is shown again with what was typed (but never the key) and
 * the layer's reason, which is fixed words naming the field, never its
 * value.  Messages after a redirect come from fixed keys.
 */

#include "web/axis-web-arr-private.h"

#include <string.h>

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

#define LIST_TITLE "Migrate from Radarr or Sonarr"

#define LIST_INTRO "Read what your Radarr or Sonarr has (the movies and series you want, their quality profile, their " \
	"folder, and the files you already have) and see what Axis would migrate. Axis only reads from Radarr and " \
	"Sonarr: it changes nothing there."

/* Where the TheTVDB key is entered. */
#define METADATA_PAGE "/metadata"

/* What a connection's page opens with: the app's own words. */
static gchar *
intro_text(AxisArrApp app)
{
	const gchar *label = axis_arr_app_label(app);

	if (app == AXIS_ARR_SONARR)
		return g_strdup_printf("Read what your %s has (the series you want, each season's and episode's monitoring, "
			"their quality profile, their folder, and the episode files you already have) and see what Axis would "
			"migrate. Axis only reads from %s: it changes nothing there.", label, label);
	return g_strdup_printf("Read what your %s has (the movies you want, their quality profile, their folder, and the "
		"files you already have) and see what Axis would migrate. Axis only reads from %s: it changes nothing there.",
		label, label);
}

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebArr         *owner;
	AxisWebArrWorkFunc  work;
	AxisWebArrDoneFunc  done;
	HtmxRequest        *request;
	HtmxResponse       *deferred;
	gpointer            data;
	GDestroyNotify      free_data;
} Task;

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
	if (response != NULL)
		g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

HtmxResponse *
axis_web_arr_dispatch_limited(
	AxisWebArr         *self,
	HtmxRequest        *request,
	guint               limit,
	AxisWebArrWorkFunc  work,
	AxisWebArrDoneFunc  done,
	gpointer            data,
	GDestroyNotify      free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= limit) {
		if (free_data != NULL)
			free_data(data);
		returned = axis_web_error_respond(request, 503, "Busy", "The server is busy. Try again in a few seconds.");
		htmx_response_add_header(returned, "Retry-After", "5");
		return returned;
	}
	task = g_new0(Task, 1);
	task->owner = self;
	task->work = work;
	task->done = done;
	task->data = data;
	task->free_data = free_data;
	task->request = g_object_ref(request);
	task->deferred = htmx_response_new_deferred();
	returned = g_object_ref(task->deferred);
	self->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);
	return returned;
}

HtmxResponse *
axis_web_arr_dispatch(
	AxisWebArr         *self,
	HtmxRequest        *request,
	AxisWebArrWorkFunc  work,
	AxisWebArrDoneFunc  done,
	gpointer            data,
	GDestroyNotify      free_data
){
	return axis_web_arr_dispatch_limited(self, request, AXIS_WEB_ARR_MAX_PENDING, work, done, data, free_data);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

gchar *
axis_web_arr_form_text(
	HtmxRequest *request,
	const gchar *name,
	gsize        max_bytes
){
	const gchar *value = htmx_request_get_form_value(request, name);

	if (value == NULL || strlen(value) > max_bytes || !g_utf8_validate(value, -1, NULL))
		return NULL;
	return g_strdup(value);
}

HtmxNode *
axis_web_arr_select(
	const gchar        *id,
	const gchar        *label,
	const gchar        *name,
	const gchar *const *values,
	const gchar *const *labels,
	const gchar        *current
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxSelect *select = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(select), id);
	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels[i]);

		if (g_strcmp0(values[i], current) == 0)
			htmx_option_set_selected(option, TRUE);
		ADD(select, option);
	}
	ADD(field, text);
	ADD(field, select);
	return HTMX_NODE(field);
}

HtmxNode *
axis_web_arr_checkbox(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gboolean     checked
){
	HtmxDiv *field = htmx_div_new();
	HtmxInput *box = htmx_input_new(HTMX_INPUT_CHECKBOX);
	HtmxLabel *text = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(field), "field-check");
	htmx_element_set_id(HTMX_ELEMENT(box), id);
	htmx_input_set_name(box, name);
	htmx_input_set_value(box, "yes");
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(box), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	ADD(field, box);
	ADD(field, text);
	return HTMX_NODE(field);
}

gchar *
axis_web_arr_path(
	gint64       source_id,
	const gchar *rest
){
	return g_strdup_printf("/migrate/%" G_GINT64_FORMAT "%s", source_id, rest != NULL ? rest : "");
}

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *node = level == 1 ? htmx_h1_new() : htmx_h3_new();

	htmx_node_set_text_content(HTMX_NODE(node), text);
	return HTMX_NODE(node);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *child)
{
	HtmxTd *cell = htmx_td_new();

	if (child != NULL)
		ADD(cell, child);
	return HTMX_NODE(cell);
}

/* What a form held, shown again when it is refused (never the key). */
typedef struct
{
	gchar    *app;
	gchar    *name;
	gchar    *address;
	gboolean  accept_untrusted;
} Values;

static void
values_clear(Values *values)
{
	g_clear_pointer(&values->app, g_free);
	g_clear_pointer(&values->name, g_free);
	g_clear_pointer(&values->address, g_free);
}

static void
values_from_source(
	Values              *values,
	const AxisArrSource *source
){
	memset(values, 0, sizeof(*values));
	values->app = g_strdup(axis_arr_app_to_string(source->app));
	values->name = g_strdup(source->name);
	values->address = g_strconcat(source->origin, source->path_prefix != NULL ? source->path_prefix : "", NULL);
	values->accept_untrusted = source->accept_untrusted;
}

static void
values_from_form(
	Values      *values,
	HtmxRequest *request
){
	memset(values, 0, sizeof(*values));
	values->app = axis_web_arr_form_text(request, "app", 16);
	values->name = axis_web_arr_form_text(request, "name", 256);
	values->address = axis_web_arr_form_text(request, "address", 1024);
	values->accept_untrusted = axis_web_storage_form_yes(request, "accept_untrusted");
}

/* Before a refused form is shown again: an address that holds a user name,
 * a password or a query is not put back in a page, since that is where a
 * key pasted with the address would be. */
static void
values_for_page(Values *values)
{
	if (values->address != NULL && strpbrk(values->address, "@?#") != NULL)
		g_clear_pointer(&values->address, g_free);
}

/* The key's field: never a value, only the way to replace it.  @app is the
 * app's name, or "the app" where the form still asks which. */
static HtmxNode *
key_field(
	const gchar *id,
	gboolean     set,
	const gchar *app
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *label = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(HTMX_INPUT_PASSWORD);
	g_autofree gchar *placeholder = g_strdup_printf("%s's API key", app);
	g_autofree gchar *where = g_strdup_printf("In %s: Settings, General, Security. It is never shown again, never "
		"written to a log, and sent only to this connection's address.", app);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), set ? "API key (stored; type a new one to replace it)" : "API key");
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, "secret");
	htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", "new-password");
	htmx_input_set_placeholder(input, set ? "Type a new key to replace it" : placeholder);
	ADD(field, label);
	ADD(field, input);
	ADD(field, axis_web_storage_text("help", set ?
		"A key is stored. Leave this empty to keep it. It is never shown again, never written to a log, and sent "
		"only to this connection's address." : where));
	return HTMX_NODE(field);
}

static HtmxNode *
source_form(
	HtmxRequest  *request,
	const gchar  *action,
	const Values *values,
	gboolean      creating,
	gboolean      key_set,
	const gchar  *button,
	const gchar  *prefix
){
	static const gchar *const app_values[] = { "radarr", "sonarr", NULL };
	static const gchar *const app_labels[] = { "Radarr", "Sonarr", NULL };
	HtmxNode *form = axis_web_auth_post_form(request, action);
	g_autofree gchar *app_id = g_strdup_printf("%s-app", prefix);
	g_autofree gchar *name_id = g_strdup_printf("%s-name", prefix);
	g_autofree gchar *address_id = g_strdup_printf("%s-address", prefix);
	g_autofree gchar *key_id = g_strdup_printf("%s-key", prefix);
	g_autofree gchar *untrusted_id = g_strdup_printf("%s-untrusted", prefix);
	g_autofree gchar *address_help = NULL;
	AxisArrApp app = AXIS_ARR_RADARR;
	/* A connection's own form names its app; the form that makes one still
	 * asks which. */
	gboolean known = !creating && axis_arr_app_from_string(values->app, &app);
	const gchar *label = known ? axis_arr_app_label(app) : "the app";

	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	if (creating)
		ADD(form, axis_web_arr_select(app_id, "App", "app", app_values, app_labels,
			values->app != NULL ? values->app : "radarr"));
	else
		ADD(form, axis_web_auth_hidden("app", values->app != NULL ? values->app : "radarr"));
	address_help = known ? g_strdup_printf("%s's own address as Axis reaches it: http or https, its host and port, and "
		"its URL base if it has one (http://media.example/%s). An address on your home network or the name of the "
		"next container is fine.", label, app == AXIS_ARR_SONARR ? "sonarr" : "radarr") :
		g_strdup("The app's own address as Axis reaches it: http or https, its host and port (Radarr's is usually "
		"7878, Sonarr's 8989), and its URL base if it has one (http://media.example/radarr). An address on your home "
		"network or the name of the next container is fine.");
	ADD(form, axis_web_storage_text_field(name_id, "Name", "name", values->name,
		known && app == AXIS_ARR_SONARR ? "Sonarr" : "Radarr", "The name Axis shows on its pages."));
	ADD(form, axis_web_storage_text_field(address_id, "Address", "address", values->address,
		known && app == AXIS_ARR_SONARR ? "http://sonarr:8989" : "http://radarr:7878", address_help));
	ADD(form, key_field(key_id, key_set, label));
	ADD(form, axis_web_arr_checkbox(untrusted_id, "Accept a self-signed certificate", "accept_untrusted",
		values->accept_untrusted));
	ADD(form, axis_web_auth_button(button, "button-primary"));
	return form;
}

static HtmxNode *
post_button(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *css_class
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_button(label, css_class));
	return form;
}

static HtmxNode *
test_badge(
	AxisArrTest test,
	AxisArrApp  app
){
	switch (test) {
	case AXIS_ARR_TEST_OK:
		return axis_web_browser_badge("Answers", "badge-succeeded");
	case AXIS_ARR_TEST_NONE:
		return axis_web_browser_badge("Not tested", "badge-note");
	case AXIS_ARR_TEST_REFUSED:
		return axis_web_browser_badge("Key refused", "badge-warn");
	case AXIS_ARR_TEST_NOT_APP:
		return axis_web_browser_badge(app == AXIS_ARR_SONARR ? "Not Sonarr" : "Not Radarr", "badge-warn");
	case AXIS_ARR_TEST_CERTIFICATE:
		return axis_web_browser_badge("Certificate", "badge-warn");
	default:
		return axis_web_browser_badge("No answer", "badge-warn");
	}
}

/* The message of a fixed key, after a redirect; "%s" in one is the app's
 * name (on the list, where no connection is meant, "Radarr or Sonarr"). */
static gchar *
done_message(
	const gchar *key,
	const gchar *app,
	gboolean    *error
){
	static const struct
	{
		const gchar *key;
		gboolean     error;
		const gchar *text;
	} messages[] = {
		{ "added", FALSE, "The connection has been added. Test it, then read what %s has." },
		{ "saved", FALSE, "The connection has been saved." },
		{ "tested", FALSE, "The connection has been tested: the result is under State of this connection." },
		{ "forgotten", FALSE, "The connection, its API key and its readings have been forgotten. Nothing in %s "
			"was changed." },
		{ "reading", FALSE, "The reading has started. This page shows where it is." },
		{ "busy", TRUE, "A reading of this connection is already under way." },
		{ "running", FALSE, "The run has started. This page shows where it is. Nothing is searched for: every movie "
			"migrated is held until you start searching." },
		{ "running-start", FALSE, "The run has started. This page shows where it is. As you ticked, searching is "
			"started when the run ends, for what %s is not downloading then; if %s does not answer then, nothing is "
			"started and this page says so." },
		{ "started", FALSE, "Searching has been started for the movies migrated. Nothing was searched for just now: "
			"the scheduler takes them at its ordinary pace." },
		{ "released", FALSE, "The files migrated are no longer kept: Axis upgrades them by the ordinary rules of "
			"their quality profiles, where it searches for their titles." },
		{ "nothing-kept", TRUE, "No file migrated by this connection is kept: there was nothing "
			"to let go." },
		{ "nothing-held", TRUE, "No movie this connection migrated is held: there was nothing to start." },
		{ "no-key", TRUE, "No API key is stored for this connection: type it under Settings for this connection." },
		{ "no-tvdb-key", TRUE, "Nothing was read: Axis has no TheTVDB key. %s numbers its series as TheTVDB does. Set "
			"a TheTVDB key on the Movie and TV details page, then read." },
		{ NULL, FALSE, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0) {
			g_auto(GStrv) parts = g_strsplit(messages[i].text, "%s", -1);

			*error = messages[i].error;
			/* A Sonarr's page speaks of series where a Radarr's speaks of
			 * movies. */
			if (g_strcmp0(app, "Sonarr") == 0) {
				g_autofree gchar *joined = g_strjoinv(app, parts);
				GString *text = g_string_new(joined);

				g_string_replace(text, "every movie", "every series", 0);
				g_string_replace(text, "the movies", "the series", 0);
				g_string_replace(text, "No movie", "No series", 0);
				return g_string_free(text, FALSE);
			}
			return g_strjoinv(app != NULL ? app : "Radarr or Sonarr", parts);
		}
	}
	return NULL;
}

/* Whether @key is one of the fixed keys. */
static gboolean
done_known(const gchar *key)
{
	gboolean is_error = FALSE;
	g_autofree gchar *text = done_message(key, NULL, &is_error);

	return text != NULL;
}

/* ── The list ───────────────────────────────────────────────────── */

static const AxisWebColumn source_columns[] = {
	{ "Name", AXIS_WEB_COL_NAME },
	{ "App", AXIS_WEB_COL_TEXT },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Last tested", AXIS_WEB_COL_DATE },
	{ "", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static HtmxNode *
source_row(
	HtmxRequest         *request,
	const AxisArrSource *source
){
	HtmxTr *row = htmx_tr_new();
	HtmxTd *name = htmx_td_new();
	HtmxSpan *address = htmx_span_new();
	HtmxCode *code = htmx_code_new();
	HtmxBdi *text = htmx_bdi_new();
	g_autofree gchar *href = axis_web_arr_path(source->id, NULL);
	g_autofree gchar *test = axis_web_arr_path(source->id, "/test");
	g_autofree gchar *shown = g_strconcat(source->origin, source->path_prefix, NULL);
	g_autofree gchar *tested = axis_web_format_time(source->tested_us, "never");
	g_autofree gchar *aria = g_strdup_printf("Test %s", source->name);
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxNode *form = axis_web_auth_post_form(request, test);
	HtmxNode *button = axis_web_auth_button("Test", "button-small");

	htmx_node_set_text_content(HTMX_NODE(text), source->name);
	ADD(link, text);
	ADD(name, link);
	htmx_element_add_class(HTMX_ELEMENT(address), "range-detail");
	ADD(code, axis_web_breakable(shown));
	ADD(address, code);
	ADD(name, address);
	ADD(row, name);
	ADD(row, cell_text(axis_arr_app_label(source->app)));
	ADD(row, cell_node(source->secret_set ? test_badge(source->last_test, source->app) :
		axis_web_browser_badge("No API key", "badge-warn")));
	ADD(row, cell_text(tested));
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	htmx_element_set_attribute(HTMX_ELEMENT(button), "aria-label", aria);
	ADD(form, button);
	ADD(row, cell_node(form));
	return HTMX_NODE(row);
}

static HtmxResponse *
list_page(
	HtmxRequest  *request,
	GPtrArray    *sources,
	const gchar  *notice,
	gboolean      notice_error,
	const Values *submitted
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card;
	Values fresh;
	guint i;

	ADD(content, heading(1, LIST_TITLE));
	ADD(content, axis_web_page_intro(LIST_INTRO));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(notice_error, notice));
	card = axis_web_storage_card("Your connections");
	if (sources->len == 0) {
		ADD(card, axis_web_storage_text("help", "No connection has been added yet. Add your Radarr or your Sonarr "
			"below, under Add a connection."));
	} else {
		HtmxNode *table = axis_web_table_new("arr-sources", source_columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < sources->len; i++)
			ADD(body, source_row(request, g_ptr_array_index(sources, i)));
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);

	card = axis_web_storage_card("Add a connection");
	ADD(card, axis_web_storage_text(NULL, "Four steps: connect, read what the app has, look at the preview, "
		"migrate. Nothing changes in Axis before you confirm at the end of the preview, and nothing is searched for "
		"until you start searching afterwards."));
	ADD(card, axis_web_storage_text("help", "A Sonarr migrates the same way: its series, each season's and "
		"episode's monitoring, and its episode files. Sonarr numbers its series as TheTVDB does, so reading one and "
		"migrating its series need a TheTVDB key in Axis (the Movie and TV details page)."));
	memset(&fresh, 0, sizeof(fresh));
	ADD(card, source_form(request, "/migrate", submitted != NULL ? submitted : &fresh, TRUE, FALSE,
		"Add the connection", "new-arr"));
	ADD(content, card);
	return axis_web_page_respond(request, LIST_TITLE, HTMX_NODE(content));
}

typedef struct
{
	gchar     *done;
	GPtrArray *sources;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebArr *self,
	gpointer    data
){
	ListWork *work = data;

	axis_arr_list_sources(self->arr, &work->sources, &work->error);
}

static HtmxResponse *
list_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	ListWork *work = data;
	gboolean is_error = FALSE;
	g_autofree gchar *message = NULL;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	message = done_message(work->done, NULL, &is_error);
	return list_page(request, work->sources, message, is_error, NULL);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ListWork *work = g_new0(ListWork, 1);
	const gchar *done = htmx_request_get_query_param(request, "done");

	if (done_known(done))
		work->done = g_strdup(done);
	return axis_web_arr_dispatch(user_data, request, list_run, list_done, work, list_work_free);
}

/* ── One connection ─────────────────────────────────────────────── */

/* The reading's state (id arr-reading): polled while a reading runs. */
static HtmxNode *
reading_block(
	HtmxRequest         *request,
	const AxisArrSource *source,
	const AxisArrPlan   *latest,
	const AxisArrPlan   *usable,
	const AxisArrRun    *run,
	gboolean             series_ready
){
	HtmxDiv *block = htmx_div_new();
	const gchar *app = axis_arr_app_label(source->app);
	g_autofree gchar *read_path = axis_web_arr_path(source->id, "/read");
	g_autofree gchar *status_path = axis_web_arr_path(source->id, "/status");
	g_autofree gchar *preview_path = axis_web_arr_path(source->id, "/preview");
	g_autofree gchar *read_label = g_strdup_printf("Read what %s has", app);
	gboolean running = latest != NULL && g_strcmp0(latest->state, "reading") == 0 && latest->job_live;

	htmx_element_set_id(HTMX_ELEMENT(block), "arr-reading");
	/* A run at work: where it is, polled as a reading is. */
	if (run != NULL && run->job_live) {
		g_autofree gchar *line = NULL;

		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", status_path);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", "every 2s");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
		if (run->progress >= 0)
			line = g_strdup_printf("Migrating from %s now: %s (%.0f%%).", app, run->progress_message != NULL ?
				run->progress_message : "at work", run->progress * 100.0);
		else
			line = g_strdup_printf("The migration from %s waits for its turn.", app);
		ADD(block, axis_web_storage_text("job-message", line));
		ADD(block, axis_web_storage_text("help", "You can leave this page and come back: the run goes on, and the "
			"Jobs page lists it, where it can be cancelled. What it has migrated stays."));
		return HTMX_NODE(block);
	}
	if (running) {
		g_autofree gchar *line = NULL;

		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", status_path);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", "every 2s");
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
		if (latest->progress >= 0)
			line = g_strdup_printf("Reading %s now: %s (%.0f%%).", app, latest->progress_message != NULL ?
				latest->progress_message : "at work", latest->progress * 100.0);
		else
			line = g_strdup_printf("The reading of %s waits for its turn.", app);
		ADD(block, axis_web_storage_text("job-message", line));
		ADD(block, axis_web_storage_text("help", "You can leave this page and come back: the reading goes on, and "
			"the Jobs page lists it."));
		return HTMX_NODE(block);
	}
	if (latest == NULL) {
		ADD(block, axis_web_storage_text(NULL, "Nothing has been read yet."));
	} else if (g_strcmp0(latest->state, "reading") == 0) {
		ADD(block, axis_web_browser_warning("The last reading stopped before it finished. Read again."));
	} else if (g_strcmp0(latest->state, "failed") == 0) {
		g_autofree gchar *when = axis_web_format_time(latest->finished_us, "");
		g_autofree gchar *line = g_strdup_printf("The last reading failed (%s): %s", when,
			latest->problem != NULL ? latest->problem : "it did not finish.");

		ADD(block, axis_web_browser_warning(line));
	}
	if (usable != NULL) {
		g_autofree gchar *when = axis_web_format_time(usable->started_us, "");
		g_autofree gchar *count = axis_web_format_count(usable->n_items);
		g_autofree gchar *line = g_strdup_printf("Read on %s%s%s: %s %s.", when,
			usable->started_by != NULL ? " by " : "", usable->started_by != NULL ? usable->started_by : "", count,
			axis_arr_app_noun(source->app, usable->n_items != 1));
		HtmxP *open = htmx_p_new();

		ADD(block, axis_web_storage_text(NULL, line));
		ADD(open, axis_web_browser_link(preview_path, "Open the preview"));
		ADD(block, open);
	} else if (latest != NULL && g_strcmp0(latest->state, "complete") == 0) {
		ADD(block, axis_web_storage_text(NULL, "The last reading is more than a day old: read again to see a "
			"preview."));
	}
	if (source->app == AXIS_ARR_SONARR && !series_ready) {
		/* No series could be brought, so none is read: said here, with the
		 * way to the key, and no button. */
		g_autofree gchar *why = g_strdup_printf("%s numbers its series as TheTVDB does. Set a TheTVDB key on the Movie "
			"and TV details page to migrate them: until then Axis does not read %s, and migrates no series.", app, app);
		HtmxP *where = htmx_p_new();

		htmx_element_set_id(HTMX_ELEMENT(where), "arr-needs-tvdb");
		ADD(block, axis_web_browser_warning(why));
		ADD(where, axis_web_browser_link(METADATA_PAGE, "Open Movie and TV details to set the TheTVDB key"));
		ADD(block, where);
	} else if (source->secret_set) {
		ADD(block, post_button(request, read_path, latest != NULL ? "Read again" : read_label, NULL));
	} else {
		ADD(block, axis_web_storage_text("help", "A reading needs the API key: type it under Settings for this "
			"connection."));
	}
	return HTMX_NODE(block);
}

static HtmxResponse *
source_page(
	HtmxRequest         *request,
	const AxisArrSource *source,
	const AxisArrPlan   *latest,
	const AxisArrPlan   *usable,
	const AxisArrRun    *run,
	gboolean             series_ready,
	const gchar         *notice,
	gboolean             notice_error,
	const Values        *submitted,
	guint                status
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *app = axis_arr_app_label(source->app);
	gboolean sonarr = source->app == AXIS_ARR_SONARR;
	g_autofree gchar *action = axis_web_arr_path(source->id, NULL);
	g_autofree gchar *test = axis_web_arr_path(source->id, "/test");
	g_autofree gchar *forget = axis_web_arr_path(source->id, "/forget");
	g_autofree gchar *shown = g_strconcat(source->origin, source->path_prefix, NULL);
	g_autofree gchar *sentence = axis_arr_test_sentence(source->app, source->last_test, source->version);
	g_autofree gchar *tested = axis_web_format_time(source->tested_us, "never");
	g_autofree gchar *has = g_strdup_printf("What %s has", app);
	g_autofree gchar *intro = intro_text(source->app);
	g_autofree gchar *only = sonarr ? g_strdup_printf("Axis only reads from %s. It changes nothing there. A reading adds "
		"no series to Axis and links no file: it is kept for the preview, for a day. Series are migrated only when you "
		"confirm at the end of the preview.", app) :
		g_strdup_printf("Axis only reads from %s. It changes nothing there. A reading adds no "
		"movie to Axis and links no file: it is kept for the preview, for a day. Movies are migrated only when you "
		"confirm at the end of the preview.", app);
	g_autofree gchar *forgets = sonarr ? g_strdup_printf("Axis forgets the connection, its API key and what it read. "
		"Nothing in %s is changed, and nothing in your library.", app) :
		g_strdup_printf("Axis forgets the connection, its API key and what it read. Nothing in "
		"%s is changed, and nothing in your library: the movies it migrated stay, with their files' links. "
		"Axis also forgets which movies you removed after they were migrated, so a later connection to the "
		"same %s can migrate them again.", app, app);
	HtmxNode *card = axis_web_storage_card("State of this connection");
	HtmxNode *facts = axis_web_browser_facts_new();
	HtmxCode *code = htmx_code_new();
	HtmxResponse *response;
	HtmxNode *form;
	Values current;

	ADD(content, heading(1, source->name));
	ADD(content, axis_web_page_intro(intro));
	if (notice != NULL)
		ADD(content, axis_web_auth_notice(notice_error, notice));

	htmx_element_set_id(HTMX_ELEMENT(card), "arr-state");
	axis_web_browser_fact(facts, "App", app);
	ADD(code, axis_web_breakable(shown));
	axis_web_browser_fact_node(facts, "Address", HTMX_NODE(code));
	axis_web_browser_fact(facts, "API key", source->secret_set ? "stored (never shown)" : "not stored");
	axis_web_browser_fact_node(facts, "State", source->secret_set ? test_badge(source->last_test, source->app) :
		axis_web_browser_badge("No API key", "badge-warn"));
	if (sentence != NULL)
		axis_web_browser_fact(facts, "Last test", sentence);
	axis_web_browser_fact(facts, "Last tested", tested);
	if (source->made_by != NULL)
		axis_web_browser_fact(facts, "Added by", source->made_by);
	ADD(card, facts);
	if (source->secret_set)
		ADD(card, post_button(request, test, "Test the connection", NULL));
	ADD(content, card);

	card = axis_web_storage_card(has);
	ADD(card, axis_web_storage_text(NULL, only));
	ADD(card, reading_block(request, source, latest, usable, run, series_ready));
	ADD(content, card);

	/* What it brought, the holds, and the way to start searching. */
	if (run != NULL && (run->n_brought > 0 || (run->last_state != NULL && !run->job_live)))
		ADD(content, axis_web_arr_brought_card(source, run));

	card = axis_web_storage_card("Settings for this connection");
	htmx_element_set_id(HTMX_ELEMENT(card), "arr-settings");
	if (g_str_has_prefix(source->origin, "http://"))
		ADD(card, axis_web_storage_text("help", "This address is plain http: the API key crosses the network "
			"unencrypted. That is usual inside one machine or a compose network."));
	if (submitted == NULL)
		values_from_source(&current, source);
	ADD(card, source_form(request, action, submitted != NULL ? submitted : &current, FALSE, source->secret_set, "Save",
		"arr"));
	if (submitted == NULL)
		values_clear(&current);
	ADD(content, card);

	card = axis_web_storage_card("Forget this connection");
	ADD(card, axis_web_storage_text(NULL, forgets));
	if (run != NULL && run->n_held > 0)
		ADD(card, axis_web_storage_text("help", "Movies it migrated are still held. Without the connection they can "
			"only be started one by one, on each movie's page: start searching above first if you want them all "
			"started."));
	/* The files it brought stay kept, and the one control that lets them all
	 * go, goes with the connection. */
	ADD(card, axis_web_storage_text("help", "The files this connection migrated stay kept: Axis does not upgrade "
		"or replace them. After you forget the connection, Let Axis upgrade these files is gone with it, and only the "
		"control of each file, on its title's page, lets a file go. Let them go above first if you want Axis to "
		"upgrade them all."));
	form = axis_web_auth_post_form(request, forget);
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_storage_checkbox("arr-forget-confirm", "Forget it for good", "confirm", TRUE));
	ADD(form, axis_web_auth_button("Forget this connection", "button-danger"));
	ADD(card, form);
	ADD(content, card);
	ADD(content, axis_web_browser_link("/migrate", "Back to Migrate"));
	response = axis_web_page_respond(request, source->name, HTMX_NODE(content));
	if (status != 200)
		htmx_response_set_status(response, status);
	return response;
}

typedef struct
{
	gint64         id;
	gboolean       fragment;
	gchar         *done;
	AxisArrSource *source;
	AxisArrPlan   *latest;
	AxisArrPlan   *usable;
	AxisArrRun    *run;
	gboolean       series_ready;	/* Axis has a TheTVDB key (a Sonarr can be read) */
	GError        *error;
} SourceWork;

static void
source_work_free(gpointer data)
{
	SourceWork *work = data;

	g_clear_pointer(&work->run, axis_arr_run_free);
	g_free(work->done);
	g_clear_pointer(&work->source, axis_arr_source_free);
	g_clear_pointer(&work->latest, axis_arr_plan_free);
	g_clear_pointer(&work->usable, axis_arr_plan_free);
	g_clear_error(&work->error);
	g_free(work);
}

static void
source_run(
	AxisWebArr *self,
	gpointer    data
){
	SourceWork *work = data;

	work->series_ready = axis_arr_series_ready(self->arr);
	if (axis_arr_get_source(self->arr, work->id, &work->source, &work->error) &&
	    axis_arr_plans(self->arr, work->id, &work->latest, &work->usable, &work->error))
		axis_arr_run_state(self->arr, work->id, &work->run, &work->error);
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no such connection.");
}

static HtmxResponse *
source_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	SourceWork *work = data;
	gboolean is_error = FALSE;
	g_autofree gchar *message = NULL;

	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->fragment) {
		HtmxNode *block = reading_block(request, work->source, work->latest, work->usable, work->run,
			work->series_ready);
		HtmxResponse *response = htmx_response_new();
		g_autofree gchar *markup = htmx_element_render(HTMX_ELEMENT(block));
		gboolean running = (work->latest != NULL && g_strcmp0(work->latest->state, "reading") == 0 &&
			work->latest->job_live) || (work->run != NULL && work->run->job_live);

		htmx_response_set_content(response, markup);
		htmx_response_set_content_type(response, "text/html; charset=utf-8");
		htmx_response_add_header(response, "Vary", "HX-Request, HX-History-Restore-Request");
		/* The reading ended: the page is drawn again, with what it found. */
		if (!running)
			htmx_response_add_header(response, "HX-Refresh", "true");
		g_object_unref(block);
		return response;
	}
	message = done_message(work->done, axis_arr_app_label(work->source->app), &is_error);
	return source_page(request, work->source, work->latest, work->usable, work->run, work->series_ready, message,
		is_error, NULL, 200);
}

static HtmxResponse *
handle_source(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	const gchar *done = htmx_request_get_query_param(request, "done");
	SourceWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(SourceWork, 1);
	work->id = id;
	if (done_known(done))
		work->done = g_strdup(done);
	return axis_web_arr_dispatch(user_data, request, source_run, source_done, work, source_work_free);
}

static HtmxResponse *
handle_status(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);
	SourceWork *work;

	if (id <= 0)
		return not_found(request);
	work = g_new0(SourceWork, 1);
	work->id = id;
	work->fragment = TRUE;
	return axis_web_arr_dispatch(user_data, request, source_run, source_done, work, source_work_free);
}

/* ── Changes ────────────────────────────────────────────────────── */

typedef enum
{
	CHANGE_SAVE,
	CHANGE_TEST,
	CHANGE_READ,
	CHANGE_FORGET
} ChangeId;

typedef struct
{
	ChangeId       change;
	gint64         id;	/* the connection; 0 for a new one */
	Values         values;
	gchar         *secret;	/* wiped */
	gchar         *user;
	gint64         saved_id;
	AxisArrTest    tested;
	GError        *error;
	AxisArrSource *source;	/* to show the page again */
	AxisArrPlan   *latest;
	AxisArrPlan   *usable;
	gboolean       series_ready;
	GPtrArray     *sources;	/* the list, to show it again */
} ChangeWork;

static void
change_work_free(gpointer data)
{
	ChangeWork *work = data;

	values_clear(&work->values);
	axis_secret_free(work->secret);
	g_free(work->user);
	g_clear_error(&work->error);
	g_clear_pointer(&work->source, axis_arr_source_free);
	g_clear_pointer(&work->latest, axis_arr_plan_free);
	g_clear_pointer(&work->usable, axis_arr_plan_free);
	g_clear_pointer(&work->sources, g_ptr_array_unref);
	g_free(work);
}

static void
change_run(
	AxisWebArr *self,
	gpointer    data
){
	ChangeWork *work = data;
	g_autoptr(GError) reread = NULL;

	switch (work->change) {
	case CHANGE_SAVE: {
		AxisArrSourceSpec spec;

		memset(&spec, 0, sizeof(spec));
		spec.app = work->values.app;
		spec.name = work->values.name;
		spec.address = work->values.address;
		spec.accept_untrusted = work->values.accept_untrusted;
		spec.secret = work->secret;
		spec.user = work->user;
		axis_arr_save_source(self->arr, work->id, &spec, &work->saved_id, &work->error);
		break;
	}
	case CHANGE_TEST:
		axis_arr_test_source(self->arr, work->id, &work->tested, &work->error);
		break;
	case CHANGE_READ:
		axis_arr_request_read(self->arr, work->id, work->user, NULL, &work->error);
		break;
	default:
		axis_arr_delete_source(self->arr, work->id, &work->error);
		break;
	}
	/* The key is not needed beyond this. */
	g_clear_pointer(&work->secret, axis_secret_free);
	/* A refused form shows its page again: read what it shows. */
	if (work->change == CHANGE_SAVE && g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID)) {
		work->series_ready = axis_arr_series_ready(self->arr);
		if (work->id == 0)
			axis_arr_list_sources(self->arr, &work->sources, &reread);
		else if (axis_arr_get_source(self->arr, work->id, &work->source, &reread))
			axis_arr_plans(self->arr, work->id, &work->latest, &work->usable, &reread);
	}
}

static HtmxResponse *
change_done(
	AxisWebArr  *self,
	HtmxRequest *request,
	gpointer     data
){
	ChangeWork *work = data;
	g_autofree gchar *location = NULL;

	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NOT_FOUND))
		return not_found(request);
	if (work->change == CHANGE_SAVE && g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID) &&
	    (work->sources != NULL || work->source != NULL)) {
		/* The layer's words name the field, never its value. */
		values_for_page(&work->values);
		if (work->id == 0) {
			HtmxResponse *response = list_page(request, work->sources, work->error->message, TRUE, &work->values);

			htmx_response_set_status(response, 400);
			return response;
		}
		return source_page(request, work->source, work->latest, work->usable, NULL, work->series_ready,
			work->error->message, TRUE, &work->values, 400);
	}
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_BUSY)) {
		location = axis_web_arr_path(work->id, "?done=busy");
		return axis_web_auth_redirect(request, location);
	}
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_TVDB_KEY)) {
		/* A reading of a Sonarr while Axis has no TheTVDB key: nothing was
		 * asked of Sonarr, and the page says where the key goes. */
		location = axis_web_arr_path(work->id, "?done=no-tvdb-key");
		return axis_web_auth_redirect(request, location);
	}
	if (g_error_matches(work->error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_INVALID)) {
		/* A test or a reading of a connection without a key. */
		location = axis_web_arr_path(work->id, "?done=no-key");
		return axis_web_auth_redirect(request, location);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (work->change) {
	case CHANGE_SAVE:
		/* Never the key, and no more of the address than that it changed. */
		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT " %s", work->saved_id,
			work->id == 0 ? "added" : "changed");
		location = axis_web_arr_path(work->saved_id, work->id == 0 ? "?done=added" : "?done=saved");
		break;
	case CHANGE_TEST:
		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT " tested", work->id);
		location = axis_web_arr_path(work->id, "?done=tested");
		break;
	case CHANGE_READ:
		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT ": a reading was started",
			work->id);
		location = axis_web_arr_path(work->id, "?done=reading");
		break;
	default:
		axis_web_storage_audit(request, "migrate: connection %" G_GINT64_FORMAT " forgotten", work->id);
		location = g_strdup("/migrate?done=forgotten");
		break;
	}
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
start_change(
	AxisWebArr  *self,
	HtmxRequest *request,
	ChangeId     change,
	gint64       id
){
	ChangeWork *work = g_new0(ChangeWork, 1);

	work->change = change;
	work->id = id;
	work->user = g_strdup(axis_web_request_get_username(request));
	if (change == CHANGE_SAVE) {
		const gchar *secret = htmx_request_get_form_value(request, "secret");

		values_from_form(&work->values, request);
		/* Longer than a key can be is not kept at all: the layer says so. */
		if (secret != NULL && secret[0] != '\0')
			work->secret = g_strndup(secret, AXIS_ARR_KEY_MAX_CHARS + 1);
	}
	return axis_web_arr_dispatch(self, request, change_run, change_done, work, change_work_free);
}

static HtmxResponse *
handle_add(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return start_change(user_data, request, CHANGE_SAVE, 0);
}

static HtmxResponse *
handle_save(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return not_found(request);
	return start_change(user_data, request, CHANGE_SAVE, id);
}

static HtmxResponse *
handle_test(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return not_found(request);
	return start_change(user_data, request, CHANGE_TEST, id);
}

static HtmxResponse *
handle_read(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return not_found(request);
	return start_change(user_data, request, CHANGE_READ, id);
}

static HtmxResponse *
handle_forget(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return not_found(request);
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to forget the connection.");
	return start_change(user_data, request, CHANGE_FORGET, id);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
read_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebArr *self = user_data;

	return axis_arr_run_read(self->arr, context, error);
}

static gboolean
cleanup_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebArr *self = user_data;

	return axis_arr_run_cleanup(self->arr, error);
}

static gboolean
apply_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebArr *self = user_data;

	return axis_arr_run_apply(self->arr, context, error);
}

/* The old address of the pages, "/bring-over...": a bookmark made before the
 * feature was called Migrate still works.  Pages only (GET): a form posted to
 * the old address is not forwarded. */
static HtmxResponse *
handle_old_address(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	static const gchar old_prefix[] = "/bring-over";
	const gchar *path = htmx_request_get_path(request);
	g_autofree gchar *location = NULL;
	HtmxResponse *response;

	if (path == NULL || !g_str_has_prefix(path, old_prefix))
		return not_found(request);
	location = g_strconcat("/migrate", path + sizeof(old_prefix) - 1, NULL);
	response = htmx_response_new();
	htmx_response_set_status(response, 301);
	htmx_response_add_header(response, "Location", location);
	return response;
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * Titles migrated from Radarr or Sonarr that "Start searching" would have
 * Axis search for (add-attention-notices): of the movies and series whose
 * hold is still 'brought', only those with something missing
 * (AXIS_ARR_MOVIE_WAITS_SQL, AXIS_ARR_SERIES_WAITS_SQL in axis-arr.h: a
 * monitored movie without a file, a series with a monitored episode without
 * one).  A held title that has its file is not counted: the file is kept,
 * starting changes nothing for it, and a notice for it would ask a person to
 * do something that does nothing.  With none that waits there is no notice.
 * One notice for each connection, in one statement that finds the held
 * through the partial indexes on search_hold.  Only a person ends it, so
 * there is no wait.  A title held because the other app has a file Axis does
 * not see, or was downloading it, is not counted: "Start searching" does not
 * end those.  "newest" is when the last of them was brought, so a later run
 * that brings more is a new occurrence.  The sentence names the app, the
 * connection where its name says more than the app's, and no title.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebArr *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_db_pool_query_bounded(axis_arr_get_pool(self->arr),
	        "SELECT s.id, s.app, s.name, count(*), coalesce(floor(max(extract(epoch FROM b.brought_at)))::bigint, 0)"
	        " FROM arr_brought b JOIN arr_source s ON s.id = b.source_id"
	        " LEFT JOIN movie m ON m.id = b.movie_id AND " AXIS_ARR_MOVIE_WAITS_SQL("m")
	        " LEFT JOIN series sr ON sr.id = b.series_id AND " AXIS_ARR_SERIES_WAITS_SQL("sr")
	        " WHERE m.id IS NOT NULL OR sr.id IS NOT NULL"
	        " GROUP BY s.id, s.app, s.name ORDER BY s.id", reading->deadline_ms, &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		gboolean series = g_strcmp0(axis_db_result_get_text(result, i, 1), "sonarr") == 0;
		const gchar *name = axis_db_result_get_text(result, i, 2);
		const gchar *app = axis_arr_app_label(series ? AXIS_ARR_SONARR : AXIS_ARR_RADARR);
		/* The connection's own name only where it says more than the app's
		 * (a connection named "radarr" reads "Radarr", as its page does). */
		gboolean named = name != NULL && name[0] != '\0' && g_ascii_strcasecmp(name, app) != 0;
		const gchar *what;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *tail = NULL;
		AxisAttentionItem *item;
		gint64 id = 0;
		gint64 count = 0;
		gint64 newest = 0;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_int64(result, i, 3, &count);
		axis_db_result_get_int64(result, i, 4, &newest);
		if (count <= 0)
			continue;
		subject = g_strdup_printf("%" G_GINT64_FORMAT, id);
		href = g_strdup_printf("/migrate/%" G_GINT64_FORMAT, id);
		what = series ? (count == 1 ? " has monitored episodes without a file. Axis does not search for them until "
			"you start searching." : " have monitored episodes without a file. Axis does not search for them until you "
			"start searching.") : (count == 1 ? " has no file. Axis does not search for it until you start searching." :
			" have no file. Axis does not search for them until you start searching.");
		text = g_strdup_printf("%" G_GINT64_FORMAT " %s migrated from %s%s", count,
			series ? "series" : count == 1 ? "movie" : "movies", app, named ? " (" : what);
		if (named)
			tail = g_strconcat(")", what, NULL);
		item = axis_attention_item_new("arr.waiting", subject, AXIS_ATTENTION_WAITING, (guint64)count, newest);
		axis_attention_item_say(item, text, named ? name : NULL, tail);
		axis_attention_item_link(item, href, "Open the migration");
		item->since_us = newest * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebArr *self = user_data;

	axis_web_add_attention_source(web, "the migrations", attention_source, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/bring-over", AXIS_WEB_ROUTE_ADMIN, handle_old_address, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/bring-over/:id", AXIS_WEB_ROUTE_ADMIN, handle_old_address, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/bring-over/:id/preview", AXIS_WEB_ROUTE_ADMIN, handle_old_address,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/bring-over/:id/release", AXIS_WEB_ROUTE_ADMIN, handle_old_address,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/bring-over/:id/brought", AXIS_WEB_ROUTE_ADMIN, handle_old_address,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/bring-over/:id/start", AXIS_WEB_ROUTE_ADMIN, handle_old_address,
		self);

	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate", AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate", AXIS_WEB_ROUTE_ADMIN, handle_add, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate/:id", AXIS_WEB_ROUTE_ADMIN, handle_source, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id", AXIS_WEB_ROUTE_ADMIN, handle_save, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/test", AXIS_WEB_ROUTE_ADMIN, handle_test, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/read", AXIS_WEB_ROUTE_ADMIN, handle_read, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/migrate/:id/status", AXIS_WEB_ROUTE_ADMIN, handle_status, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/migrate/:id/forget", AXIS_WEB_ROUTE_ADMIN, handle_forget, self);
	axis_web_arr_register_preview(self, web);
	axis_web_arr_register_run(self, web);
}

static const AxisJobHandlerInfo handlers[] = {
	/* One attempt: the person who asked is looking at the page, and "Read
	 * again" is the retry.  On the metadata queue: it waits for Radarr and
	 * for the details provider, and holds up no maintenance job. */
	{ AXIS_ARR_READ_KIND, AXIS_META_QUEUE, 1, read_handler,
	  "Reading what Radarr or Sonarr has", AXIS_JOB_CLASS_WORK },
	{ AXIS_ARR_CLEANUP_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, cleanup_handler,
	  "Forgetting readings of Radarr and Sonarr older than a day", AXIS_JOB_CLASS_UPKEEP },
	/* The run: each movie is a transaction of its own, so an attempt that a
	 * database error or a restart ends is taken up by the next, which finds
	 * what was brought already in Axis. */
	{ AXIS_ARR_APPLY_KIND, AXIS_META_QUEUE, 3, apply_handler,
	  "Migrating from Radarr or Sonarr", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_ARR_CLEANUP_SCHEDULE, AXIS_ARR_CLEANUP_KIND, NULL, AXIS_ARR_CLEANUP_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Migrate", "/migrate", 37, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = { AXIS_META_MODULE_ID, NULL };

static const gchar *const retry_elsewhere[] = {
	AXIS_ARR_READ_KIND, "Start the reading again on the Migrate page, with Read again.",
	AXIS_ARR_APPLY_KIND, "Open the connection's preview on the Migrate page and confirm again: what was migrated "
		"stays, and nothing is migrated twice.",
	NULL
};

const AxisModuleInfo *
axis_web_arr_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.name = "Migrate";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Reads a Radarr you already run (its movies, their quality profile and folder, and the "
			"files they have) through Radarr's own API with your API key, shows what Axis would migrate, and "
			"migrates the movies when you confirm: each with its profile and folder, its file on disk linked "
			"where it is, and nothing searched for until you start it. It only reads from Radarr: nothing there "
			"is changed, and no file is moved, renamed or deleted. The API key goes only to that Radarr's address. A "
			"Sonarr migrates the same way: its series under TheTVDB's aired order, each season's and episode's "
			"monitoring, and its episode files linked to the episodes Sonarr has them on.";
		info.depends_on = depends_on;
		info.job_retry_elsewhere = retry_elsewhere;
		info.id = AXIS_ARR_MODULE_ID;
	}
	return &info;
}

AxisWebArr *
axis_web_arr_new(AxisArr *arr)
{
	AxisWebArr *self;

	g_return_val_if_fail(arr != NULL, NULL);

	self = g_new0(AxisWebArr, 1);
	self->arr = axis_arr_ref(arr);
	return self;
}

void
axis_web_arr_stop(AxisWebArr *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_arr_free(AxisWebArr *self)
{
	if (self == NULL)
		return;
	axis_web_arr_stop(self);
	axis_arr_unref(self->arr);
	g_free(self);
}
