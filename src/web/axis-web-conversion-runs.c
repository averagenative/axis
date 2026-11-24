/*
 * axis-web-conversion-runs.c - the pages of the conversion runs: the list of
 * runs, planning one, a run's page (its review before it starts, where it is
 * and why while it runs, its list a page at a time, what is not in it), a
 * movie or series of its list, the files left out for a reason, the actions,
 * and the dashboard's piece
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every page and form is an administrator's (AXIS_WEB_ROUTE_ADMIN), every
 * POST carries the CSRF token, and what a request does is decided again on
 * the server.  A run asks for each of its files through the single
 * conversion's request and gates; replacing originals needs a person to tick
 * the same sentence a single replacement's confirmation shows.  No page
 * shows a path of the server: names are shown as stored, as text.
 */

#include "web/axis-web-conversion-runs.h"

#include <string.h>

#include "core/axis-size.h"
#include "library/axis-library-name.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-conversions.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add
#define MAX_PENDING 32
#define RUNS_LIMIT 50
#define RECENT 20
#define STATUS_POLL "every 15s"

struct _AxisWebRuns
{
	AxisConversionRuns *runs;
	guint               pending;
	gboolean            stopping;
};

typedef void (*WorkFunc)(AxisWebRuns *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisWebRuns *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisWebRuns    *owner;
	WorkFunc        work;
	DoneFunc        done;
	HtmxRequest    *request;
	HtmxResponse   *deferred;
	gpointer        data;
	GDestroyNotify  free_data;
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
	g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebRuns    *self,
	HtmxRequest    *request,
	WorkFunc        work,
	DoneFunc        done,
	gpointer        data,
	GDestroyNotify  free_data
){
	HtmxResponse *returned;
	GTask *gtask;
	Task *task;

	if (self->stopping || self->pending >= MAX_PENDING) {
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

/* ── Small pieces ───────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *h = level == 1 ? htmx_h1_new() : htmx_h2_new();

	htmx_node_set_text_content(HTMX_NODE(h), text);
	return HTMX_NODE(h);
}

static HtmxNode *
cell_text(const gchar *text)
{
	return HTMX_NODE(htmx_td_new_with_text(text != NULL ? text : ""));
}

static HtmxNode *
cell_node(HtmxNode *node)
{
	HtmxTd *cell = htmx_td_new();

	if (node != NULL)
		ADD(cell, node);
	return HTMX_NODE(cell);
}

static HtmxNode *
cell_size(gint64 bytes)
{
	g_autofree gchar *text = bytes >= 0 ? axis_size_format((guint64)bytes) : g_strdup("");

	return cell_text(text);
}

static HtmxNode *
cell_count(gint64 n)
{
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, n);

	return cell_text(text);
}

static gchar *
size_text(gint64 bytes)
{
	return axis_size_format((guint64)MAX(bytes, 0));
}

static HtmxNode *
link_text(
	const gchar *href,
	const gchar *text
){
	return axis_web_browser_link(href, text);
}

static HtmxNode *
small_post(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	const gchar *css
){
	HtmxNode *form = axis_web_auth_post_form(request, action);
	g_autofree gchar *classes = g_strdup_printf("button-small%s%s", css != NULL ? " " : "", css != NULL ? css : "");

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_button(label, classes));
	return form;
}

static HtmxNode *
state_badge(const gchar *state)
{
	const gchar *css = g_strcmp0(state, "finished") == 0 ? "badge-succeeded" :
		g_strcmp0(state, "failed") == 0 ? "badge-failed" : g_strcmp0(state, "cancelled") == 0 ? "badge-note" :
		g_strcmp0(state, "paused") == 0 || g_strcmp0(state, "ready") == 0 ? "badge-warn" : "badge-running";

	return axis_web_browser_badge(axis_conversion_run_state_label(state), css);
}

static const gchar *
entry_state_label(const AxisConversionRunEntry *e)
{
	if (g_strcmp0(e->state, "waiting") == 0)
		return e->trial_id > 0 ? "Waiting (its trial passed)" : "Waiting";
	if (g_strcmp0(e->state, "asked") == 0)
		return axis_conversion_state_label(e->conversion_state);
	if (g_strcmp0(e->state, "done") == 0)
		return g_strcmp0(e->outcome, "trial") == 0 ? "Trial done" : "Replaced";
	if (g_strcmp0(e->state, "skipped") == 0)
		return "Skipped";
	if (g_strcmp0(e->state, "failed") == 0)
		return "Failed";
	return "Taken out";
}

static HtmxNode *
entry_badge(const AxisConversionRunEntry *e)
{
	const gchar *css = g_strcmp0(e->state, "done") == 0 ? "badge-succeeded" : g_strcmp0(e->state, "failed") == 0 ?
		"badge-failed" : g_strcmp0(e->state, "asked") == 0 ? "badge-running" : g_strcmp0(e->state, "waiting") == 0 ?
		"badge-warn" : "badge-note";

	return axis_web_browser_badge(entry_state_label(e), css);
}

static gchar *
run_title(gint64 id)
{
	return g_strdup_printf("Conversion run %" G_GINT64_FORMAT, id);
}

static gchar *
profile_text(const AxisConversionRunInfo *info)
{
	return g_strdup_printf("%s, version %d (%s)", info->profile_name != NULL ? info->profile_name : "?",
		info->revision_number, info->proven ? "proven" : "locked: trials only");
}

/* What a unit is called: its title (and year), else its folder or file. */
static HtmxNode *
unit_name(const AxisConversionRunUnit *u)
{
	if (u->title != NULL) {
		g_autofree gchar *text = u->year > 0 ? g_strdup_printf("%s (%d)", u->title, u->year) : g_strdup(u->title);
		HtmxNode *bdi = HTMX_NODE(htmx_bdi_new());

		ADD(bdi, axis_web_breakable(text));
		return bdi;
	}
	if (g_strcmp0(u->kind, "file") == 0 || g_strcmp0(u->kind, "movie") == 0) {
		const gchar *slash = u->rel_path != NULL ? strrchr(u->rel_path, '/') : NULL;

		return axis_web_browser_name_node(slash != NULL ? slash + 1 : u->rel_path);
	}
	return axis_web_browser_name_node(u->rel_path);
}

static gchar *
unit_kind_text(const AxisConversionRunUnit *u)
{
	if (g_strcmp0(u->kind, "series") == 0)
		return g_strdup("Series");
	if (g_strcmp0(u->kind, "folder") == 0)
		return g_strdup("Folder (taken as one series)");
	if (g_strcmp0(u->kind, "movie") == 0)
		return g_strdup("Movie");
	return g_strdup("File");
}

/* "23 of 62 episodes done" */
static gchar *
unit_progress(const AxisConversionRunUnit *u)
{
	gboolean series = g_strcmp0(u->kind, "series") == 0 || g_strcmp0(u->kind, "folder") == 0;

	if (u->files == 1)
		return g_strdup(u->done > 0 ? "Done" : u->asked > 0 ? "In flight" : u->failed > 0 ? "Failed" :
			u->skipped > 0 ? "Skipped" : u->removed ? "Taken out" : "Waiting");
	return g_strdup_printf("%d of %d %s done%s", u->done, u->files, series ? "episodes" : "files",
		u->asked > 0 ? ", one in flight" : "");
}

/* A file of a unit: its episode, or its name. */
static HtmxNode *
entry_name(
	const AxisConversionRunEntry *e,
	gboolean                      link
){
	const gchar *slash = e->rel_path != NULL ? strrchr(e->rel_path, '/') : NULL;
	HtmxSpan *span = htmx_span_new();

	if (e->season_number >= 0 && e->episode_number >= 0) {
		g_autofree gchar *code = g_strdup_printf("S%02dE%02d ", e->season_number, e->episode_number);

		ADD(span, htmx_strong_new_with_text(code));
	}
	if (link && e->file_id > 0) {
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, e->file_id);
		HtmxA *a = htmx_a_new_with_href(href);

		ADD(a, axis_web_browser_name_node(slash != NULL ? slash + 1 : e->rel_path));
		ADD(span, a);
	} else {
		ADD(span, axis_web_browser_name_node(slash != NULL ? slash + 1 : e->rel_path));
	}
	return HTMX_NODE(span);
}

static const gchar *
done_text(const gchar *key)
{
	if (g_strcmp0(key, "created") == 0)
		return "The run is made: its list is being made now (this page follows it). Nothing is converted until you "
			"start it.";
	if (g_strcmp0(key, "exists") == 0)
		return "Nothing new was made: this run, of the same selection with the same profile version, has not "
			"started yet. Start it, or cancel it before making another.";
	if (g_strcmp0(key, "started") == 0)
		return "The run is started: it asks for its first files within a minute.";
	if (g_strcmp0(key, "paused") == 0)
		return "The run is paused: it asks for nothing more; what is in flight finishes.";
	if (g_strcmp0(key, "resumed") == 0)
		return "The run goes on.";
	if (g_strcmp0(key, "cancelled") == 0)
		return "The run is cancelled: what waited for a worker or was being converted is given back; what was being "
			"checked or put in place finishes.";
	if (g_strcmp0(key, "replacing") == 0)
		return "From now on the run replaces originals, with the same list. The files it converted as trials are "
			"converted again, as replacements.";
	if (g_strcmp0(key, "limits") == 0)
		return "The limits are saved.";
	if (g_strcmp0(key, "removed") == 0)
		return "Taken out of the list.";
	if (g_strcmp0(key, "kept") == 0)
		return "The files Axis keeps are being added to the list (this page follows it): each is listed, or left out "
			"for another reason of its own. A file that is no longer in the library is not added, so fewer can be "
			"added than the row said. They stay kept. Nothing is converted until the run is started or resumed.";
	return NULL;
}

static HtmxNode *
crumbs(gint64 run_id)
{
	HtmxP *p = htmx_p_new();

	ADD(p, axis_web_browser_link("/conversions", "Conversions"));
	ADD(p, axis_web_storage_text(NULL, " / "));
	ADD(p, axis_web_browser_link("/conversions/runs", "Conversion runs"));
	if (run_id > 0) {
		g_autofree gchar *href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, run_id);
		g_autofree gchar *title = run_title(run_id);

		ADD(p, axis_web_storage_text(NULL, " / "));
		ADD(p, axis_web_browser_link(href, title));
	}
	return HTMX_NODE(p);
}

static HtmxResponse *
error_page(
	HtmxRequest  *request,
	const GError *error
){
	guint status;

	if (error->domain != AXIS_CONVERSION_ERROR)
		return axis_web_internal_error(request, error);
	status = error->code == AXIS_CONVERSION_ERROR_NOT_FOUND ? 404 : error->code == AXIS_CONVERSION_ERROR_BUSY ? 503 :
		409;
	return axis_web_error_respond(request, status, "Not done", error->message);
}

static HtmxResponse *
fragment_response(HtmxNode *node)
{
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *markup = NULL;

	axis_web_table_apply_roles(node);
	markup = htmx_element_render(HTMX_ELEMENT(node));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_add_header(response, "Vary", "HX-Request, HX-History-Restore-Request");
	g_object_unref(node);
	return response;
}

static gboolean
query_number(
	HtmxRequest *request,
	const gchar *name,
	gint64      *value
){
	const gchar *text = htmx_request_get_query_param(request, name);
	guint64 n = 0;

	*value = 0;
	if (text == NULL || text[0] == '\0')
		return TRUE;
	if (!g_ascii_string_to_unsigned(text, 10, 0, (guint64)G_MAXINT64, &n, NULL))
		return FALSE;
	*value = (gint64)n;
	return TRUE;
}

/* ── The runs ───────────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *runs;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_clear_pointer(&work->runs, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebRuns *self,
	gpointer     data
){
	ListWork *work = data;

	axis_conversion_runs_list(self->runs, RUNS_LIMIT, &work->runs, &work->error);
}

static HtmxResponse *
list_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	static const AxisWebColumn columns[] = {
		{ "Run", AXIS_WEB_COL_NAME },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Conversion profile", AXIS_WEB_COL_TEXT },
		{ "Trials or replacements", AXIS_WEB_COL_TEXT },
		{ "Files done", AXIS_WEB_COL_NUMBER },
		{ "Size after (files done)", AXIS_WEB_COL_SIZE },
		{ "Made", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, crumbs(0));
	ADD(content, heading(1, "Conversion runs"));
	ADD(content, axis_web_page_intro("A conversion run works through a list of a library's files for you: the files "
		"that would save the most come first, and a series is kept together so that it is finished as a whole. It "
		"converts one file at a time, each an ordinary conversion with its ten checks, first as trials and, once you "
		"have proven the profile, replacing originals."));
	{
		HtmxNode *paused = axis_web_conversions_paused_notice(request, axis_conversion_runs_get_conversions(self->runs));

		if (paused != NULL)
			ADD(content, paused);
	}
	{
		HtmxA *a = htmx_a_new_with_href("/conversions/runs/new");

		htmx_element_add_class(HTMX_ELEMENT(a), "button");
		htmx_element_add_class(HTMX_ELEMENT(a), "button-primary");
		htmx_node_set_text_content(HTMX_NODE(a), "Plan a conversion run");
		ADD(content, a);
	}
	card = axis_web_storage_card("Every run");
	if (work->runs->len == 0) {
		ADD(card, axis_web_storage_text("help", "There is no conversion run yet. Plan one with Plan a "
			"conversion run above."));
	} else {
		HtmxNode *table = axis_web_table_new("conversion-runs", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->runs->len; i++) {
			const AxisConversionRunInfo *r = g_ptr_array_index(work->runs, i);
			g_autofree gchar *href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, r->id);
			g_autofree gchar *name = g_strdup_printf("Run %" G_GINT64_FORMAT ": %s", r->id, r->scope_text);
			g_autofree gchar *profile = g_strdup_printf("%s, version %d", r->profile_name, r->revision_number);
			g_autofree gchar *done = g_strdup_printf("%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT, r->done,
				r->waiting + r->asked + r->done + r->skipped + r->failed);
			g_autofree gchar *made = axis_web_format_time(r->created_us, "");
			HtmxTr *row = htmx_tr_new();

			ADD(row, cell_node(link_text(href, name)));
			ADD(row, cell_node(state_badge(r->state)));
			ADD(row, cell_text(profile));
			ADD(row, cell_text(g_strcmp0(r->outcome, "replace") == 0 ? "Replaces originals" : "Trials"));
			ADD(row, cell_text(done));
			ADD(row, cell_size(r->done_after));
			ADD(row, cell_text(made));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	ADD(content, card);
	return axis_web_page_respond(request, "Conversion runs", HTMX_NODE(content));
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return dispatch(user_data, request, list_run, list_done, g_new0(ListWork, 1), list_work_free);
}

/* ── Planning a run ─────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *roots;	/* AxisRootInfo */
	GPtrArray *series;	/* AxisConversionRunSeries */
	GPtrArray *revisions;	/* AxisProfileRevision */
	GError    *error;
} PlanWork;

static void
plan_work_free(gpointer data)
{
	PlanWork *work = data;

	g_clear_pointer(&work->roots, g_ptr_array_unref);
	g_clear_pointer(&work->series, g_ptr_array_unref);
	g_clear_pointer(&work->revisions, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
plan_run(
	AxisWebRuns *self,
	gpointer     data
){
	PlanWork *work = data;
	AxisConversions *conversions = axis_conversion_runs_get_conversions(self->runs);

	if (!axis_storage_list_roots(axis_conversions_get_storage(conversions), 0, &work->roots, &work->error))
		return;
	if (!axis_conversion_runs_series_on_disk(self->runs, 5000, &work->series, &work->error))
		return;
	axis_conversions_offer(conversions, &work->revisions, &work->error);
}

static HtmxNode *
radio(
	const gchar *name,
	const gchar *value,
	const gchar *text,
	gboolean     checked
){
	HtmxDiv *div = htmx_div_new();
	HtmxInput *input = htmx_input_new(HTMX_INPUT_RADIO);
	g_autofree gchar *id = g_strdup_printf("run-%s-%s", name, value);
	HtmxLabel *label = htmx_label_new_for(id);

	htmx_element_add_class(HTMX_ELEMENT(div), "field-check");
	htmx_input_set_name(input, name);
	htmx_input_set_value(input, value);
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	if (checked)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "checked", "checked");
	htmx_node_set_text_content(HTMX_NODE(label), text);
	ADD(div, input);
	ADD(div, label);
	return HTMX_NODE(div);
}

static HtmxNode *
fieldset(const gchar *legend)
{
	HtmxFieldset *set = htmx_fieldset_new();

	htmx_element_add_class(HTMX_ELEMENT(set), "profile-group");
	ADD(set, htmx_legend_new_with_text(legend));
	return HTMX_NODE(set);
}

static HtmxResponse *
plan_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	PlanWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	HtmxNode *card;
	HtmxNode *form;
	HtmxNode *set;
	guint video_roots = 0;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, crumbs(0));
	ADD(content, heading(1, "Plan a conversion run"));
	ADD(content, axis_web_page_intro("Choose what to look at and the profile to convert with. Axis then makes the "
		"list: every file the profile would convert, the largest estimated saving first, a series kept together, and "
		"every file left out with its reason. Nothing is converted until you have looked at the list and started the "
		"run."));
	card = axis_web_storage_card("Files and conversion profile for the run");
	form = axis_web_auth_post_form(request, "/conversions/runs");
	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");

	set = fieldset("Which files");
	ADD(set, radio("scope", "movies", "Every movie library folder", TRUE));
	ADD(set, radio("scope", "tv", "Every TV library folder", FALSE));
	ADD(set, radio("scope", "roots", "The library folders ticked below", FALSE));
	for (i = 0; work->roots != NULL && i < work->roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(work->roots, i);
		g_autofree gchar *id = g_strdup_printf("run-root-%" G_GINT64_FORMAT, root->id);
		g_autofree gchar *name = g_strdup_printf("root_%" G_GINT64_FORMAT, root->id);
		g_autofree gchar *text = g_strdup_printf("%s (%s)", root->name, axis_media_type_label(root->media_type));

		if (root->media_type != AXIS_MEDIA_MOVIES && root->media_type != AXIS_MEDIA_TV &&
		    root->media_type != AXIS_MEDIA_OTHER)
			continue;
		video_roots++;
		ADD(set, axis_web_storage_checkbox(id, text, name, FALSE));
	}
	if (video_roots == 0)
		ADD(set, axis_web_storage_text("help", "There is no movie, TV or other video library folder "
			"yet. Add one under Media management, Storage."));
	ADD(set, radio("scope", "series", "One series (choose it below)", FALSE));
	if (work->series != NULL && work->series->len > 0) {
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("run-series");
		HtmxSelect *select = htmx_select_new_with_name("series");

		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Series on disk");
		htmx_element_set_id(HTMX_ELEMENT(select), "run-series");
		for (i = 0; i < work->series->len; i++) {
			const AxisConversionRunSeries *s = g_ptr_array_index(work->series, i);
			g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, s->title_id);
			g_autofree gchar *text = s->year > 0 ? g_strdup_printf("%s (%d)", s->title, s->year) : g_strdup(s->title);

			ADD(select, htmx_option_new_with_value(value, text));
		}
		ADD(field, label);
		ADD(field, select);
		ADD(set, field);
	} else {
		ADD(set, axis_web_storage_text("help", "No series on disk is identified yet. Identify your TV "
			"library folder under Media management, Identify."));
	}
	ADD(form, set);

	set = fieldset("Conversion profile");
	if (work->revisions != NULL && work->revisions->len > 0) {
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("run-revision");
		HtmxSelect *select = htmx_select_new_with_name("revision");

		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Convert with");
		htmx_element_set_id(HTMX_ELEMENT(select), "run-revision");
		for (i = 0; i < work->revisions->len; i++) {
			const AxisProfileRevision *r = g_ptr_array_index(work->revisions, i);
			g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, r->id);
			g_autofree gchar *text = g_strdup_printf("%s (%s)", r->name, axis_profile_revision_is_unlocked(r) ?
				"proven" : "locked: trials only");

			ADD(select, htmx_option_new_with_value(value, text));
		}
		ADD(field, label);
		ADD(field, select);
		ADD(set, field);
		ADD(set, axis_web_storage_text("help", "With a locked profile the run makes trials until enough have passed to "
			"prove it; then you play them, prove it, and switch the run to replacing originals, with the same list."));
	} else {
		ADD(set, axis_web_storage_text("help", "There is no usable video conversion profile. Copy a "
			"built-in one under Conversion profiles."));
	}
	ADD(form, set);

	/* Off unless ticked: the run then lists the files Axis keeps as well. */
	set = fieldset("Files Axis keeps");
	ADD(set, axis_web_storage_checkbox("run-convert-kept", AXIS_WEB_CONVERT_KEPT_LABEL, "convert_kept", FALSE));
	ADD(set, axis_web_storage_text("help", AXIS_WEB_CONVERT_KEPT_HELP " Files are kept when you keep them by hand, "
		"and nearly every file is after a migration from Radarr or Sonarr. Without this, a run leaves kept files out; "
		"you can still add them from the run's page."));
	ADD(form, set);
	ADD(form, axis_web_auth_button("Make the list", "button-primary"));
	ADD(card, form);
	ADD(content, card);
	return axis_web_page_respond(request, "Plan a conversion run", HTMX_NODE(content));
}

static HtmxResponse *
handle_new(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return dispatch(user_data, request, plan_run, plan_done, g_new0(PlanWork, 1), plan_work_free);
}

typedef struct
{
	AxisConversionRunScope  scope;
	GArray                 *roots;
	gint64                  title_id;
	gint64                  revision_id;
	gboolean                convert_kept;	/* the tick "Also convert files Axis keeps" */
	gchar                  *user;
	gint64                  run_id;
	gboolean                created;
	GError                 *error;
} CreateWork;

static void
create_work_free(gpointer data)
{
	CreateWork *work = data;

	if (work->roots != NULL)
		g_array_unref(work->roots);
	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
create_run(
	AxisWebRuns *self,
	gpointer     data
){
	CreateWork *work = data;

	axis_conversion_runs_create_full(self->runs, work->scope, (const gint64 *)work->roots->data, work->roots->len,
		work->title_id, work->revision_id, work->convert_kept, work->user, &work->run_id, &work->created, &work->error);
}

static HtmxResponse *
create_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	CreateWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL)
		return error_page(request, work->error);
	/* A second click gets the run the first made (add-activity-feedback). */
	if (!work->created) {
		location = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, work->run_id);
		return axis_web_place_redirect(request, location, "exists", "run-controls");
	}
	axis_web_storage_audit(request, "conversion run %" G_GINT64_FORMAT " made%s", work->run_id,
		work->convert_kept ? ", also converting the files Axis keeps" : "");
	location = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "?done=created", work->run_id);
	return axis_web_auth_redirect(request, location);
}

static gboolean
form_id(
	HtmxRequest *request,
	const gchar *name,
	gint64      *value
){
	guint64 n = 0;

	*value = 0;
	if (!axis_web_storage_form_u64(request, name, &n) || n == 0 || n > (guint64)G_MAXINT64)
		return FALSE;
	*value = (gint64)n;
	return TRUE;
}

static HtmxResponse *
handle_create(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	CreateWork *work = g_new0(CreateWork, 1);
	GHashTable *form = htmx_request_get_form_data(request);

	work->roots = g_array_new(FALSE, FALSE, sizeof(gint64));
	if (!axis_conversion_run_scope_from_string(htmx_request_get_form_value(request, "scope"), &work->scope) ||
	    !form_id(request, "revision", &work->revision_id)) {
		create_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "Choose what to look at and a profile.");
	}
	if (work->scope == AXIS_CONVERSION_RUN_ROOTS && form != NULL) {
		GHashTableIter iter;
		gpointer key;
		gpointer value;

		g_hash_table_iter_init(&iter, form);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			guint64 id = 0;

			if (g_str_has_prefix(key, "root_") && g_strcmp0(value, "yes") == 0 &&
			    g_ascii_string_to_unsigned((const gchar *)key + strlen("root_"), 10, 1, (guint64)G_MAXINT64, &id, NULL)) {
				gint64 root = (gint64)id;

				g_array_append_val(work->roots, root);
			}
		}
		if (work->roots->len == 0) {
			create_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "Tick at least one library folder.");
		}
	}
	if (work->scope == AXIS_CONVERSION_RUN_SERIES && !form_id(request, "series", &work->title_id)) {
		create_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "Choose a series.");
	}
	/* Only an explicit tick: anything else is no choice. */
	work->convert_kept = axis_web_storage_form_yes(request, "convert_kept");
	work->user = g_strdup(axis_web_request_get_username(request));
	return dispatch(user_data, request, create_run, create_done, work, create_work_free);
}

/* ── A run ──────────────────────────────────────────────────────── */

typedef struct
{
	gint64                 id;
	gint                   after;
	gboolean               status_only;
	AxisConversionRunInfo *info;
	GPtrArray             *units;
	gboolean               more;
	GPtrArray             *reasons;
	GPtrArray             *in_flight;
	GPtrArray             *recent;
	GError                *error;
} RunWork;

static void
run_work_free(gpointer data)
{
	RunWork *work = data;

	axis_conversion_run_info_free(work->info);
	g_clear_pointer(&work->units, g_ptr_array_unref);
	g_clear_pointer(&work->reasons, g_ptr_array_unref);
	g_clear_pointer(&work->in_flight, g_ptr_array_unref);
	g_clear_pointer(&work->recent, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
run_read(
	AxisWebRuns *self,
	gpointer     data
){
	RunWork *work = data;

	if (!axis_conversion_runs_get(self->runs, work->id, &work->info, &work->error))
		return;
	if (!axis_conversion_runs_in_flight(self->runs, work->id, &work->in_flight, &work->error))
		return;
	if (work->status_only)
		return;
	if (!axis_conversion_runs_units(self->runs, work->id, work->after, AXIS_CONVERSION_RUN_PAGE, &work->units,
	        &work->more, &work->error))
		return;
	if (!axis_conversion_runs_reasons(self->runs, work->id, &work->reasons, &work->error))
		return;
	axis_conversion_runs_recent(self->runs, work->id, RECENT, &work->recent, &work->error);
}

/* "about 1.2 TiB, from 3.4 TiB to 2.2 TiB" */
static gchar *
saving_sentence(
	gint64 before,
	gint64 after
){
	g_autofree gchar *saving = size_text(before - after);
	g_autofree gchar *from = size_text(before);
	g_autofree gchar *to = size_text(after);

	return g_strdup_printf("%s (from %s to %s)", saving, from, to);
}

static gchar *
duration_text(gint64 seconds)
{
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " day%s", seconds / 86400, seconds == 86400 ? "" : "s");
	if (seconds % 3600 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT " hour%s", seconds / 3600, seconds == 3600 ? "" : "s");
	return g_strdup_printf("%" G_GINT64_FORMAT " minutes", MAX(seconds / 60, 1));
}

/* The files settled (done, skipped or failed) of the files within the run's
 * caps (of the whole list when it has none), as a bar with its words. */
static HtmxNode *
files_bar(const AxisConversionRunInfo *info)
{
	gint64 listed = info->waiting + info->asked + info->done + info->skipped + info->failed;
	gboolean capped = (info->cap_files > 0 || info->cap_bytes > 0) && info->cap_files_in > 0;
	gint64 of = capped ? MIN(info->cap_files_in, listed) : listed;
	gint64 settled = MIN(info->done + info->skipped + info->failed, of);
	g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " files %s (%d%%)", settled, of,
		capped ? "within its caps settled" : "settled", of > 0 ? (gint)(settled * 100 / of) : 0);

	return axis_web_conversions_bar((gdouble)settled, (gdouble)of, text);
}

/* The bytes saved so far against the saving estimated for the files within
 * the caps; NULL while nothing is done or nothing is estimated. */
static HtmxNode *
saved_bar(const AxisConversionRunInfo *info)
{
	gboolean capped = info->cap_files > 0 || info->cap_bytes > 0;
	gint64 estimate = capped ? info->cap_bytes_in - info->cap_mid_in : info->bytes - info->mid;
	gint64 saved = MAX(info->done_before - info->done_after, 0);
	gint64 part = MIN(saved, estimate);
	g_autofree gchar *saved_text = NULL;
	g_autofree gchar *estimate_text = NULL;
	g_autofree gchar *text = NULL;
	gint percent;

	if (info->done <= 0 || estimate <= 0)
		return NULL;
	/* In whole percents, without a product that a gint64 cannot hold. */
	percent = (gint)(part > G_MAXINT64 / 100 ? part / (estimate / 100) : part * 100 / estimate);
	saved_text = size_text(saved);
	estimate_text = size_text(estimate);
	text = g_strdup_printf("%s saved of about %s estimated%s%s", saved_text, estimate_text,
		capped ? " within its caps" : "", g_strcmp0(info->outcome, "replace") == 0 || info->done_trials == 0 ?
		"" : " (as trials, nothing replaced)");
	/* As a percent: the sizes are too long for a bar's value to carry. */
	return axis_web_conversions_bar((gdouble)percent, 100.0, text);
}

/* Where the run is and why: the block the page polls. */
static HtmxNode *
status_block(
	const AxisConversionRunInfo *info,
	GPtrArray                   *in_flight
){
	HtmxDiv *block = htmx_div_new();
	HtmxNode *card = axis_web_storage_card("Progress of this run");
	HtmxNode *facts = axis_web_browser_facts_new();
	gboolean live = g_strcmp0(info->state, "listing") == 0 || g_strcmp0(info->state, "running") == 0 ||
		(g_strcmp0(info->state, "paused") == 0 && info->asked > 0) || info->asked > 0;
	gint64 listed = info->waiting + info->asked + info->done + info->skipped + info->failed;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(block), "run-status");
	if (live) {
		g_autofree gchar *href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/status", info->id);

		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", href);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", STATUS_POLL);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
	}
	axis_web_browser_fact_node(facts, "State", state_badge(info->state));
	if (g_strcmp0(info->state, "listing") == 0) {
		g_autofree gchar *text = g_strdup_printf("%s%s%s", info->list_job_state != NULL &&
			g_strcmp0(info->list_job_state, "running") == 0 ? "Being made" : "Waiting for its turn",
			info->list_message != NULL ? ": " : "", info->list_message != NULL ? info->list_message : "");

		axis_web_browser_fact(facts, info->relist_state != NULL ? "Adding the files Axis keeps" : "List of files", text);
	}
	if (g_strcmp0(info->state, "failed") == 0 && info->list_text != NULL)
		ADD(card, axis_web_storage_text("notice notice-error", info->list_text));
	/* Kept files that could not be added: the run is as it was, and says so. */
	if (g_strcmp0(info->state, "failed") != 0 && g_strcmp0(info->state, "listing") != 0 && info->list_text != NULL)
		ADD(card, axis_web_storage_text("notice notice-error", info->list_text));
	if (g_strcmp0(info->state, "paused") == 0 && info->pause_text != NULL) {
		g_autofree gchar *text = g_strcmp0(info->pause_code, "person") == 0 ? g_strdup(info->pause_text) :
			g_strdup_printf("The run paused itself. %s", info->pause_text);

		ADD(card, axis_web_storage_text("notice notice-warning", text));
	}
	if (g_strcmp0(info->state, "running") == 0 && info->wait_text != NULL) {
		g_autofree gchar *text = g_strdup_printf("Waiting: %s", info->wait_text);

		ADD(card, axis_web_storage_text("notice", text));
	}
	if ((g_strcmp0(info->state, "finished") == 0 || g_strcmp0(info->state, "cancelled") == 0) &&
	    info->finish_text != NULL)
		axis_web_browser_fact(facts, "How it ended", info->finish_text);
	if (listed > 0 || info->done > 0) {
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " files done; %"
			G_GINT64_FORMAT " skipped, %" G_GINT64_FORMAT " failed, %" G_GINT64_FORMAT " waiting%s", info->done, listed,
			info->skipped, info->failed, info->waiting, info->asked > 0 ? ", the rest in flight" : "");
		g_autofree gchar *units = g_strdup_printf("%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT, info->units_done,
			info->units);

		axis_web_browser_fact(facts, "Files", text);
		axis_web_browser_fact(facts, "Movies and series finished", units);
		if (listed > 0)
			ADD(card, files_bar(info));
	}
	if (info->done > 0) {
		g_autofree gchar *real = saving_sentence(info->done_before, info->done_after);
		g_autofree gchar *estimated = saving_sentence(info->done_before, info->done_estimate);
		g_autofree gchar *text = g_strdup_printf("%s%s; estimated %s", g_strcmp0(info->outcome, "replace") == 0 ||
			info->done_trials == 0 ? "" : "(as trials, nothing replaced) ", real, estimated);

		axis_web_browser_fact(facts, "Saved so far", text);
	}
	{
		HtmxNode *saved = saved_bar(info);

		if (saved != NULL)
			ADD(card, saved);
	}
	ADD(card, facts);
	if (in_flight != NULL && in_flight->len > 0) {
		HtmxUl *list = htmx_ul_new();

		ADD(card, heading(2, "Files being converted or checked now"));
		for (i = 0; i < in_flight->len; i++) {
			const AxisConversionRunEntry *e = g_ptr_array_index(in_flight, i);
			HtmxLi *item = htmx_li_new();

			ADD(item, entry_name(e, TRUE));
			if (e->conversion_id > 0) {
				g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, e->conversion_id);
				g_autofree gchar *text = g_strdup_printf("Conversion %" G_GINT64_FORMAT ": %s",
					e->conversion_id, axis_conversion_state_label(e->conversion_state));

				ADD(item, link_text(href, text));
			}
			/* How far the file in flight is. */
			{
				AxisConversionProgress progress;

				axis_conversion_progress(e->conversion_state, e->outcome, e->job_state, e->job_fraction, e->job_running_s,
					e->verify_state, e->verify_fraction, e->verify_running_s, e->duration_ms, &progress);
				if (progress.stage != NULL)
					ADD(item, axis_web_conversions_progress_node(&progress));
			}
			ADD(list, item);
		}
		ADD(card, list);
	}
	if (info->waits != NULL && info->waits->len > 0) {
		HtmxUl *list = htmx_ul_new();

		ADD(card, heading(2, "Files that wait for their disk"));
		for (i = 0; i < info->waits->len; i++) {
			const AxisConversionRunWait *w = g_ptr_array_index(info->waits, i);
			g_autofree gchar *text = g_strdup_printf("%s: %s", w->volume_name, w->text);
			HtmxLi *item = htmx_li_new();

			htmx_node_set_text_content(HTMX_NODE(item), text);
			ADD(list, item);
		}
		ADD(card, list);
	}
	ADD(block, card);
	return HTMX_NODE(block);
}

/* What the run does and with what. */
static HtmxNode *
what_card(
	AxisWebRuns                 *self,
	const AxisConversionRunInfo *info
){
	HtmxNode *card = axis_web_storage_card("What this run converts, and how");
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *profile = profile_text(info);
	g_autofree gchar *made = axis_web_format_time(info->created_us, "");
	g_autofree gchar *who = g_strdup_printf("%s, %s", info->created_by, made);
	g_autofree gchar *at_once = g_strdup_printf("%d at a time (never more than the connected workers can take)",
		info->at_once);

	axis_web_browser_fact(facts, "Files it looks at", info->scope_text);
	axis_web_browser_fact(facts, "Conversion profile", profile);
	axis_web_browser_fact(facts, "Trials or replacements", g_strcmp0(info->outcome, "replace") == 0 ?
		"Replacements: each converted file takes its original's place once it has passed every check" :
		"Trials: each converted file is kept aside to be played; the library is not touched");
	if (info->confirmed_by != NULL) {
		g_autofree gchar *when = axis_web_format_time(info->confirmed_us, "");
		g_autofree gchar *text = g_strdup_printf("%s, %s", info->confirmed_by, when);

		axis_web_browser_fact(facts, "Replacing confirmed by", text);
	}
	if (info->convert_kept) {
		g_autofree gchar *when = axis_web_format_time(info->convert_kept_us, "");
		g_autofree gchar *text = g_strdup_printf("Included: a kept file is converted in place after every check passes, "
			"and stays kept. Chosen by %s, %s.", info->convert_kept_by != NULL ? info->convert_kept_by : "?", when);

		axis_web_browser_fact(facts, "Files Axis keeps", text);
	} else {
		axis_web_browser_fact(facts, "Files Axis keeps", "Left out: the run converts no file that Axis keeps (kept by "
			"hand, or brought over from Radarr or Sonarr).");
	}
	axis_web_browser_fact(facts, "Made by", who);
	if (info->started_by != NULL) {
		g_autofree gchar *when = axis_web_format_time(info->started_us, "");
		g_autofree gchar *text = g_strdup_printf("%s, %s", info->started_by, when);

		axis_web_browser_fact(facts, "Started by", text);
	}
	axis_web_browser_fact(facts, "Files at once", at_once);
	if (info->cap_files > 0 || info->cap_bytes > 0) {
		g_autofree gchar *saving = size_text(info->cap_bytes_in - info->cap_mid_in);
		g_autofree gchar *text = g_strdup_printf("The first %" G_GINT64_FORMAT " movies and series (%" G_GINT64_FORMAT
			" files, estimated to save %s): %s%s%s", info->cap_units_in, info->cap_files_in, saving,
			info->cap_files > 0 ? "a cap of files" : "", info->cap_files > 0 && info->cap_bytes > 0 ? " and " : "",
			info->cap_bytes > 0 ? "a cap of saving" : "");

		axis_web_browser_fact(facts, "Within the run's caps", text);
	}
	if (info->cap_seconds > 0) {
		g_autofree gchar *budget = duration_text(info->cap_seconds);
		g_autofree gchar *text = g_strdup_printf("%s from its start", budget);

		axis_web_browser_fact(facts, "Time budget", text);
	}
	if (info->window_start >= 0) {
		g_autofree gchar *from = axis_conversion_run_clock_text(info->window_start);
		g_autofree gchar *to = axis_conversion_run_clock_text(info->window_end);
		g_autofree gchar *text = g_strdup_printf("From %s to %s (this server's local time)", from, to);

		axis_web_browser_fact(facts, "Hours it asks for files", text);
	}
	if (g_strcmp0(info->outcome, "trial") == 0) {
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " of the %" G_GINT64_FORMAT " trials needed to "
			"prove it by hand%s", info->passed_trials, info->needed_trials, info->bad_trials > 0 ?
			"; a trial was rejected by a check of the converted file, so it cannot be proven by hand" : "");

		axis_web_browser_fact(facts, "Passed trials of the version", text);
	}
	ADD(card, facts);
	(void)self;
	return card;
}

static HtmxNode *
number_field(
	const gchar *id,
	const gchar *label,
	const gchar *name,
	gint64       value,
	const gchar *help
){
	g_autofree gchar *text = value > 0 ? g_strdup_printf("%" G_GINT64_FORMAT, value) : g_strdup("");
	HtmxNode *field = axis_web_auth_field(id, label, name, HTMX_INPUT_NUMBER, "off", text);

	if (help != NULL)
		ADD(field, axis_web_storage_text("help", help));
	return field;
}

static HtmxNode *
limits_form(
	HtmxRequest                 *request,
	const AxisConversionRunInfo *info
){
	g_autofree gchar *action = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/limits", info->id);
	g_autofree gchar *from = info->window_start >= 0 ? axis_conversion_run_clock_text(info->window_start) : g_strdup("");
	g_autofree gchar *to = info->window_end >= 0 ? axis_conversion_run_clock_text(info->window_end) : g_strdup("");
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxNode *set = fieldset("Limits");

	htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
	ADD(set, number_field("run-at-once", "Files at once", "at_once", info->at_once, "1 to 8. Never more than the "
		"connected workers can take."));
	ADD(set, number_field("run-cap-files", "Stop after this many files", "cap_files", info->cap_files, "Leave empty for "
		"every file. A series is kept whole: the list ends after the series in which the cap is reached."));
	ADD(set, number_field("run-cap-gib", "Only until this many GiB are saved (estimated)", "cap_gib",
		info->cap_bytes > 0 ? (info->cap_bytes + 1073741823) / 1073741824 : 0, "Leave empty for no cap."));
	ADD(set, number_field("run-cap-hours", "A time budget, in hours", "cap_hours",
		info->cap_seconds > 0 ? (info->cap_seconds + 3599) / 3600 : 0, "From its start: it asks for nothing after "
		"that, and what is in flight finishes. Leave empty for none."));
	ADD(set, axis_web_auth_field("run-window-from", "Only from (HH:MM)", "window_from", HTMX_INPUT_TIME, "off", from));
	ADD(set, axis_web_auth_field("run-window-to", "Until (HH:MM)", "window_to", HTMX_INPUT_TIME, "off", to));
	ADD(set, axis_web_storage_text("help", "Hours in this server's local time, for example 22:00 to 06:00 for the "
		"night. Leave both empty for any time."));
	ADD(form, set);
	ADD(form, axis_web_auth_button("Save the limits", NULL));
	return form;
}

/* The sentence a single replacement's confirmation shows, said of each
 * file, with its box. */
static void
replace_confirmation(
	HtmxNode        *form,
	AxisConversions *conversions
){
	g_autofree gchar *sentence = axis_web_conversions_keep_sentence(axis_conversions_keep_original(conversions));
	g_autofree gchar *text = g_strdup_printf("For each file: %s", sentence);

	ADD(form, axis_web_storage_text("notice notice-warning", text));
	ADD(form, axis_web_storage_checkbox("run-confirm", "I understand: the originals will be deleted", "confirm", TRUE));
}

static HtmxNode *
controls_card(
	AxisWebRuns                 *self,
	HtmxRequest                 *request,
	const AxisConversionRunInfo *info,
	AxisWebNotice               *placed
){
	HtmxNode *card = axis_web_storage_card("Start, pause, cancel or limit this run");
	AxisConversions *conversions = axis_conversion_runs_get_conversions(self->runs);
	g_autofree gchar *base = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, info->id);
	gboolean ready = g_strcmp0(info->state, "ready") == 0;
	gboolean running = g_strcmp0(info->state, "running") == 0;
	gboolean paused = g_strcmp0(info->state, "paused") == 0;
	gboolean may_replace = info->proven && info->current;

	htmx_element_set_id(HTMX_ELEMENT(card), "run-controls");
	axis_web_notice_put(placed, card, "run-controls");
	if (ready) {
		g_autofree gchar *action = g_strdup_printf("%s/start", base);
		HtmxNode *form = axis_web_auth_post_form(request, action);

		ADD(card, axis_web_storage_text("help", "Look at the list below first: take out what you do not want converted, "
			"or cap it. Then start the run."));
		ADD(form, axis_web_auth_hidden("outcome", "trial"));
		ADD(form, axis_web_storage_text("help", "Trials: each file is converted and checked, and the converted file is "
			"kept aside in its disk's work area to be played. The library is not touched."));
		ADD(form, axis_web_auth_button("Start with trials", "button-primary"));
		ADD(card, form);
		if (may_replace) {
			HtmxNode *replace = axis_web_auth_post_form(request, action);

			ADD(replace, axis_web_auth_hidden("outcome", "replace"));
			ADD(replace, axis_web_storage_text("help", "Replacing originals: each file's conversion takes its "
				"original's place once it has passed every check, while every condition a single replacement needs holds "
				"(a safe playback verdict for its library folder, among them)."));
			replace_confirmation(replace, conversions);
			ADD(replace, axis_web_auth_button("Start replacing originals", "button-danger"));
			ADD(card, replace);
		} else {
			ADD(card, axis_web_storage_text("help", info->proven ? "Replacing originals needs the profile's current "
				"version: this run's is not, so it can only make trials." : "Replacing originals needs the version "
				"proven: a run with a locked version makes trials until enough have passed, then pauses for you to "
				"prove it."));
		}
	}
	if (running || paused) {
		g_autofree gchar *pause = g_strdup_printf("%s/pause", base);
		g_autofree gchar *resume = g_strdup_printf("%s/resume", base);

		if (running) {
			ADD(card, small_post(request, pause, "Pause the run", NULL));
		} else if (g_strcmp0(info->outcome, "replace") == 0 && info->waiting_in > 0) {
			/* What resuming does to the library, in numbers: the files that
			 * wait within its caps, each replaced once every check passes. */
			g_autofree gchar *label = g_strdup_printf("Resume the run, replacing up to %" G_GINT64_FORMAT " original%s",
				info->waiting_in, info->waiting_in == 1 ? "" : "s");
			g_autofree gchar *text = g_strdup_printf("The run replaces originals: resuming it replaces up to %"
				G_GINT64_FORMAT " original%s, %s of the list that wait%s within its caps, each once every check passes.",
				info->waiting_in, info->waiting_in == 1 ? "" : "s", info->waiting_in == 1 ? "the file" : "the files",
				info->waiting_in == 1 ? "s" : "");

			ADD(card, axis_web_storage_text("help", text));
			ADD(card, small_post(request, resume, label, "button-danger"));
		} else {
			ADD(card, small_post(request, resume, "Resume the run", "button-primary"));
		}
	}
	if ((ready || running || paused) && g_strcmp0(info->outcome, "trial") == 0 && may_replace && !ready) {
		g_autofree gchar *action = g_strdup_printf("%s/replace", base);
		HtmxNode *form = axis_web_auth_post_form(request, action);
		g_autofree gchar *count = g_strdup_printf("This run will replace up to %" G_GINT64_FORMAT " original%s, %"
			G_GINT64_FORMAT " of them %s Axis keeps, each once every check of its converted file passes. A replaced "
			"original cannot be brought back.", info->replace_in, info->replace_in == 1 ? "" : "s",
			info->replace_kept_in, info->replace_kept_in == 1 ? "a file" : "files");
		g_autofree gchar *seen = g_strdup_printf("%" G_GINT64_FORMAT, info->replace_kept_in);

		ADD(form, axis_web_storage_text("help", "The version is proven: the run can replace originals from now on, with "
			"the same list. The files it converted as trials are converted again as replacements."));
		ADD(form, axis_web_storage_text("notice notice-warning", count));
		ADD(form, axis_web_auth_hidden("kept_seen", seen));
		replace_confirmation(form, conversions);
		ADD(form, axis_web_auth_button("Replace originals from now on", "button-danger"));
		ADD(card, form);
	}
	if (ready || running || paused || g_strcmp0(info->state, "listing") == 0) {
		g_autofree gchar *cancel = g_strdup_printf("%s/cancel", base);

		ADD(card, small_post(request, cancel, "Cancel the run", "button-danger"));
	}
	if (ready || running || paused)
		ADD(card, limits_form(request, info));
	return card;
}

static HtmxNode *
units_card(
	HtmxRequest                 *request,
	const AxisConversionRunInfo *info,
	GPtrArray                   *units,
	gboolean                     more,
	gint                         after,
	AxisWebNotice               *placed
){
	static const AxisWebColumn columns[] = {
		{ "Rank", AXIS_WEB_COL_COUNT },
		{ "Movie or series", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size now", AXIS_WEB_COL_SIZE },
		{ "After (estimated)", AXIS_WEB_COL_SIZE },
		{ "Saving", AXIS_WEB_COL_SIZE },
		{ "Progress", AXIS_WEB_COL_TEXT },
		{ "Action", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Movies and series in this run");
	gboolean changeable = g_strcmp0(info->state, "ready") == 0 || g_strcmp0(info->state, "running") == 0 ||
		g_strcmp0(info->state, "paused") == 0;
	guint i;

	if (g_strcmp0(info->state, "listing") == 0) {
		ADD(card, axis_web_storage_text("help", "The list is being made."));
		return card;
	}
	{
		g_autofree gchar *saving = saving_sentence(info->bytes, info->mid);
		g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " movies and series, %" G_GINT64_FORMAT " files of "
			"the %" G_GINT64_FORMAT " looked at, estimated to save %s. Ranked by their estimated saving, the largest "
			"first; a series' episodes are converted one after another, in order, before the next.", info->units,
			info->files, info->scope_files, saving);

		ADD(card, axis_web_storage_text("help", text));
	}
	if (units == NULL || units->len == 0) {
		ADD(card, axis_web_storage_text("help", after > 0 ? "There is nothing after this." :
			"Nothing would be converted: see what is not in the list, below."));
	} else {
		HtmxNode *table = axis_web_table_new("conversion-run-units", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < units->len; i++) {
			const AxisConversionRunUnit *u = g_ptr_array_index(units, i);
			g_autofree gchar *href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/units/%d", info->id,
				u->position);
			g_autofree gchar *progress = unit_progress(u);
			g_autofree gchar *kind = unit_kind_text(u);
			HtmxTr *row = htmx_tr_new();
			HtmxA *a = htmx_a_new_with_href(href);
			HtmxSpan *name = htmx_span_new();
			g_autofree gchar *row_id = g_strdup_printf("run-unit-%d", u->position);
			HtmxNode *here = axis_web_notice_take(placed, row_id);

			htmx_element_set_id(HTMX_ELEMENT(row), row_id);
			if (here != NULL)
				ADD(name, here);
			ADD(a, unit_name(u));
			ADD(name, a);
			{
				g_autofree gchar *what = g_strdup_printf(" %s%s%s", kind, u->removed ? ", taken out" : "",
					u->beyond_cap ? ", beyond the cap" : "");

				ADD(name, htmx_small_new_with_text(what));
			}
			ADD(row, cell_count(u->position));
			ADD(row, cell_node(HTMX_NODE(name)));
			ADD(row, cell_count(u->files));
			ADD(row, cell_size(u->bytes));
			ADD(row, cell_size(u->mid));
			ADD(row, cell_size(u->bytes - u->mid));
			ADD(row, cell_text(progress));
			if (changeable && !u->removed && u->waiting > 0) {
				g_autofree gchar *remove = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/units/%d/remove",
					info->id, u->position);
				HtmxNode *form = small_post(request, remove, "Take out of the run", NULL);
				g_autofree gchar *page = g_strdup_printf("%d", after);

				/* Back to this page of the list. */
				if (after > 0)
					ADD(form, axis_web_auth_hidden("after", page));
				ADD(row, cell_node(form));
			} else {
				ADD(row, cell_text(""));
			}
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	{
		g_autofree gchar *first = after > 0 ? g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, info->id) : NULL;
		g_autofree gchar *next = more && units != NULL && units->len > 0 ? g_strdup_printf("/conversions/runs/%"
			G_GINT64_FORMAT "?after=%d", info->id,
			((const AxisConversionRunUnit *)g_ptr_array_index(units, units->len - 1))->position) : NULL;

		ADD(card, axis_web_browser_pager(first, next));
	}
	return card;
}

/* Whether the files left out as kept can be added to the run now: it does
 * not convert kept files yet, asks for nothing (ready, paused, finished), and
 * has no file in flight.  The server decides it again at the POST. */
static gboolean
may_include_kept(const AxisConversionRunInfo *info)
{
	return !info->convert_kept && info->asked == 0 && (g_strcmp0(info->state, "ready") == 0 ||
		g_strcmp0(info->state, "paused") == 0 || g_strcmp0(info->state, "finished") == 0);
}

/*
 * "Add these N files to the run", with what it means for this run.  A run
 * that replaces originals replaces these too: the form says so, with the
 * sentence and the box of the switch to replacing, and the server refuses it
 * without the tick.  A run of trials adds them as trials; a run that has not
 * started asks how when it is started.  No run starts by itself after it.
 */
static HtmxNode *
include_kept_form(
	AxisWebRuns                 *self,
	HtmxRequest                 *request,
	const AxisConversionRunInfo *info,
	gint64                       files
){
	g_autofree gchar *action = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/include-kept", info->id);
	g_autofree gchar *label = g_strdup_printf("Add these %" G_GINT64_FORMAT " file%s to the run", files,
		files == 1 ? "" : "s");
	HtmxNode *form = axis_web_auth_post_form(request, action);
	HtmxDiv *box = htmx_div_new();

	if (g_strcmp0(info->outcome, "replace") == 0) {
		g_autofree gchar *text = NULL;

		if (files == 1)
			text = g_strdup("This run replaces originals. If you add this file, its original is replaced by its "
				"converted file once every check passes. It stays kept, so Axis still does not replace it with another "
				"release. The run waits, paused, until you resume it.");
		else
			text = g_strdup_printf("This run replaces originals. If you add these %" G_GINT64_FORMAT " files, their %"
				G_GINT64_FORMAT " originals are replaced by their converted files once every check passes. They stay "
				"kept, so Axis still does not replace them with another release. The run waits, paused, until you "
				"resume it.", files, files);

		ADD(form, axis_web_storage_text("notice notice-warning", text));
		replace_confirmation(form, axis_conversion_runs_get_conversions(self->runs));
		ADD(form, axis_web_auth_button(label, "button-small button-danger"));
	} else {
		gboolean for_proof = g_strcmp0(info->state, "paused") == 0 && (g_strcmp0(info->pause_code, "prove") == 0 ||
			g_strcmp0(info->pause_code, "proven") == 0);
		const gchar *help;

		if (g_strcmp0(info->state, "ready") == 0)
			help = "They join the list. Nothing is converted until you start the run: with trials, or replacing "
				"originals once you confirm that for every file of the list. They stay kept.";
		else if (for_proof)
			help = "They are added as trials now: each is converted and checked, and the converted file is kept aside "
				"to be played. The run waits, paused, for its profile to be proven. When you then switch the run to "
				"replacing originals, these files are replaced like the rest, and that form asks again, with the numbers.";
		else
			help = "They are added as trials: each is converted and checked, and the converted file is kept aside to be "
				"played. The library is not touched. The run waits, paused, until you resume it.";
		ADD(form, axis_web_storage_text("help", help));
		ADD(form, axis_web_auth_button(label, "button-small"));
	}
	ADD(box, form);
	return HTMX_NODE(box);
}

static HtmxNode *
reasons_card(
	AxisWebRuns                 *self,
	HtmxRequest                 *request,
	const AxisConversionRunInfo *info,
	GPtrArray                   *reasons,
	AxisWebNotice               *placed
){
	static const AxisWebColumn columns[] = {
		{ "Why it is not in the list", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "What it means", AXIS_WEB_COL_WIDE },
		{ "Action", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Files left out, and why");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "run-left-out");
	axis_web_notice_put(placed, card, "run-left-out");
	if (reasons == NULL || reasons->len == 0) {
		ADD(card, axis_web_storage_text("help", g_strcmp0(info->state, "listing") == 0 ? "The list is being made." :
			"Every file looked at is in the list."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "The files looked at that the profile would not convert now, each with the "
		"one reason it is left out. A file on a disk that is offline now, but that would be converted, is in the "
		"list: it waits for its disk."));
	table = axis_web_table_new("conversion-run-reasons", columns);
	body = htmx_tbody_new();
	for (i = 0; i < reasons->len; i++) {
		const AxisConversionRunReason *r = g_ptr_array_index(reasons, i);
		g_autofree gchar *href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/left-out?reason=%s", info->id,
			r->reason);
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_node(link_text(href, axis_conversion_run_reason_label(r->reason))));
		ADD(row, cell_count(r->files));
		ADD(row, cell_size(r->bytes));
		ADD(row, cell_text(axis_conversion_run_reason_help(r->reason)));
		if (g_strcmp0(r->reason, "kept") == 0 && r->files > 0 && may_include_kept(info)) {
			ADD(row, cell_node(include_kept_form(self, request, info, r->files)));
		} else if (g_strcmp0(r->reason, "kept") == 0 && !info->convert_kept && info->asked > 0) {
			ADD(row, cell_text("You can add these files once nothing of the run is in flight."));
		} else if (g_strcmp0(r->reason, "kept") == 0 && !info->convert_kept &&
		    g_strcmp0(info->state, "running") == 0) {
			ADD(row, cell_text("Pause the run to add these files."));
		} else {
			ADD(row, cell_text(""));
		}
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxNode *
entries_table(
	HtmxRequest                 *request,
	const AxisConversionRunInfo *info,
	GPtrArray                   *entries,
	const gchar                 *kind,
	gboolean                     removable,
	AxisWebNotice               *placed
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Size now", AXIS_WEB_COL_SIZE },
		{ "After (estimated)", AXIS_WEB_COL_SIZE },
		{ "After (real)", AXIS_WEB_COL_SIZE },
		{ "Why", AXIS_WEB_COL_WIDE },
		{ "Action", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new(kind, columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < entries->len; i++) {
		const AxisConversionRunEntry *e = g_ptr_array_index(entries, i);
		HtmxTr *row = htmx_tr_new();
		HtmxSpan *state = htmx_span_new();
		g_autofree gchar *row_id = g_strdup_printf("run-entry-%" G_GINT64_FORMAT, e->id);
		HtmxNode *here = placed != NULL ? axis_web_notice_take(placed, row_id) : NULL;

		htmx_element_set_id(HTMX_ELEMENT(row), row_id);
		if (here != NULL) {
			HtmxNode *wrap = HTMX_NODE(htmx_div_new());

			ADD(wrap, here);
			ADD(wrap, entry_name(e, TRUE));
			ADD(row, cell_node(wrap));
		} else {
			ADD(row, cell_node(entry_name(e, TRUE)));
		}
		ADD(state, entry_badge(e));
		if (e->conversion_id > 0 || e->trial_id > 0) {
			gint64 conversion = e->conversion_id > 0 ? e->conversion_id : e->trial_id;
			g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, conversion);
			g_autofree gchar *text = g_strdup_printf("Conversion %" G_GINT64_FORMAT, conversion);

			ADD(state, htmx_small_new_with_text(" "));
			ADD(state, link_text(href, text));
		}
		ADD(row, cell_node(HTMX_NODE(state)));
		ADD(row, cell_size(e->size));
		ADD(row, cell_size(e->mid));
		ADD(row, cell_size(e->result_size));
		ADD(row, cell_text(e->reason_text));
		if (removable && g_strcmp0(e->state, "waiting") == 0) {
			g_autofree gchar *remove = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/entries/%" G_GINT64_FORMAT
				"/remove", info->id, e->id);
			g_autofree gchar *unit = g_strdup_printf("%d", e->unit_position);
			HtmxNode *form = small_post(request, remove, "Take out of the run", NULL);

			ADD(form, axis_web_auth_hidden("unit", unit));
			ADD(row, cell_node(form));
		} else {
			ADD(row, cell_text(""));
		}
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

static HtmxNode *
estimates_card(const AxisConversionRunInfo *info)
{
	HtmxNode *card = axis_web_storage_card("Estimates and real sizes");
	HtmxNode *facts = axis_web_browser_facts_new();

	ADD(card, axis_web_storage_text("help", "The size the converted files came to, beside the size estimated for them. "
		"Axis keeps the ratio; it does not correct its estimates with it yet."));
	if (info->done > 0) {
		g_autofree gchar *real = size_text(info->done_after);
		g_autofree gchar *estimated = size_text(info->done_estimate);
		g_autofree gchar *text = info->done_estimate > 0 ? g_strdup_printf("%s, estimated %s (%.2f times the estimate)",
			real, estimated, (gdouble)info->done_after / (gdouble)info->done_estimate) :
			g_strdup_printf("%s", real);

		axis_web_browser_fact(facts, "Size after, of this run's finished files", text);
	} else {
		axis_web_browser_fact(facts, "Size after, of this run's finished files", "None done yet");
	}
	if (info->seen_files > 0) {
		g_autofree gchar *real = size_text(info->seen_actual);
		g_autofree gchar *estimated = size_text(info->seen_estimated);
		g_autofree gchar *text = info->seen_estimated > 0 ? g_strdup_printf("%" G_GINT64_FORMAT " files: %s, estimated %s "
			"(%.2f times the estimate)", info->seen_files, real, estimated,
			(gdouble)info->seen_actual / (gdouble)info->seen_estimated) : g_strdup_printf("%" G_GINT64_FORMAT " files: %s",
			info->seen_files, real);

		axis_web_browser_fact(facts, "Every finished conversion with this version", text);
	}
	ADD(card, facts);
	return card;
}

static HtmxResponse *
run_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	RunWork *work = data;
	const AxisConversionRunInfo *info = work->info;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *title = NULL;
	const gchar *notice = done_text(htmx_request_get_query_param(request, "done"));
	AxisWebNotice placed;

	if (g_error_matches(work->error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion run.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->status_only)
		return fragment_response(status_block(info, work->in_flight));
	title = run_title(info->id);
	ADD(content, crumbs(0));
	ADD(content, heading(1, title));
	ADD(content, axis_web_page_intro(g_strcmp0(info->state, "ready") == 0 ? "The list is made: review it, take out "
		"what you do not want, cap it if you like, and start the run. Nothing has been converted yet." :
		"The run asks for one conversion of its list at a time (or as many as you allow), in the order below, through "
		"the same request, conditions and ten checks as a single conversion asked for from a file's page."));
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	{
		HtmxNode *paused = axis_web_conversions_paused_notice(request, axis_conversion_runs_get_conversions(self->runs));

		if (paused != NULL)
			ADD(content, paused);
	}
	ADD(content, status_block(info, work->in_flight));
	ADD(content, what_card(self, info));
	if (g_strcmp0(info->state, "finished") != 0 && g_strcmp0(info->state, "cancelled") != 0 &&
	    g_strcmp0(info->state, "failed") != 0)
		ADD(content, controls_card(self, request, info, &placed));
	ADD(content, units_card(request, info, work->units, work->more, work->after, &placed));
	ADD(content, reasons_card(self, request, info, work->reasons, &placed));
	if (work->recent != NULL && work->recent->len > 0) {
		HtmxNode *card = axis_web_storage_card("Files finished last");

		ADD(card, axis_web_storage_text("help", "The files whose conversion ended last, newest first: done, skipped (not "
			"eligible any more, changed, not worth it) or failed, each with why."));
		ADD(card, entries_table(request, info, work->recent, "conversion-run-entries", FALSE, NULL));
		ADD(content, card);
	}
	ADD(content, estimates_card(info));
	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static HtmxResponse *
run_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     status_only
){
	RunWork *work;
	gint64 id = axis_web_storage_param_id(params);
	gint64 after = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion run.");
	if (!query_number(request, "after", &after) || after > G_MAXINT32)
		return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
	work = g_new0(RunWork, 1);
	work->id = id;
	work->after = (gint)after;
	work->status_only = status_only;
	return dispatch(user_data, request, run_read, run_done, work, run_work_free);
}

static HtmxResponse *
handle_run(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return run_page(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_status(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return run_page(request, params, user_data, TRUE);
}

/* ── A unit ─────────────────────────────────────────────────────── */

typedef struct
{
	gint64                 id;
	gint                   position;
	gint                   after;
	AxisConversionRunInfo *info;
	AxisConversionRunUnit *unit;
	GPtrArray             *entries;
	gboolean               more;
	GError                *error;
} UnitWork;

static void
unit_work_free(gpointer data)
{
	UnitWork *work = data;

	axis_conversion_run_info_free(work->info);
	axis_conversion_run_unit_free(work->unit);
	g_clear_pointer(&work->entries, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
unit_read(
	AxisWebRuns *self,
	gpointer     data
){
	UnitWork *work = data;

	if (!axis_conversion_runs_get(self->runs, work->id, &work->info, &work->error))
		return;
	if (!axis_conversion_runs_unit(self->runs, work->id, work->position, &work->unit, &work->error))
		return;
	axis_conversion_runs_entries(self->runs, work->id, work->position, work->after, AXIS_CONVERSION_RUN_ENTRY_PAGE,
		&work->entries, &work->more, &work->error);
}

static HtmxResponse *
unit_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	UnitWork *work = data;
	const AxisConversionRunUnit *u = work->unit;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *progress = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *title = NULL;
	gboolean changeable;
	HtmxNode *card;
	HtmxNode *facts;
	const gchar *notice = done_text(htmx_request_get_query_param(request, "done"));
	AxisWebNotice placed;

	if (g_error_matches(work->error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", work->error->message);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	progress = unit_progress(u);
	kind = unit_kind_text(u);
	title = u->title != NULL ? (u->year > 0 ? g_strdup_printf("%s (%d)", u->title, u->year) : g_strdup(u->title)) :
		axis_library_name_display(u->rel_path);
	changeable = g_strcmp0(work->info->state, "ready") == 0 || g_strcmp0(work->info->state, "running") == 0 ||
		g_strcmp0(work->info->state, "paused") == 0;
	ADD(content, crumbs(work->id));
	{
		HtmxNode *h1 = HTMX_NODE(htmx_h1_new());

		ADD(h1, unit_name(u));
		ADD(content, h1);
	}
	ADD(content, axis_web_page_intro(g_strcmp0(u->kind, "series") == 0 || g_strcmp0(u->kind, "folder") == 0 ?
		"A series of the run's list: its episodes are converted one after another, in season and episode order, before "
		"the run goes on to the next, so that the series is finished as a whole." :
		"A movie or file of the run's list."));
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	card = axis_web_storage_card("Place in the run and progress");
	facts = axis_web_browser_facts_new();
	{
		g_autofree gchar *rank = g_strdup_printf("%d", u->position);
		g_autofree gchar *saving = saving_sentence(u->bytes, u->mid);

		axis_web_browser_fact(facts, "Rank in the list", rank);
		axis_web_browser_fact(facts, "What it is", kind);
		if (u->root_name != NULL)
			axis_web_browser_fact(facts, "Library folder", u->root_name);
		axis_web_browser_fact(facts, "Progress", progress);
		axis_web_browser_fact(facts, "Estimated saving", saving);
		if (u->done > 0) {
			g_autofree gchar *real = saving_sentence(u->done_before, u->done_after);

			axis_web_browser_fact(facts, "Saved so far", real);
		}
		if (u->beyond_cap)
			axis_web_browser_fact(facts, "Within the caps", "No: the run stops before it");
		if (u->removed)
			axis_web_browser_fact(facts, "Taken out", "Yes: what was not asked for yet is not converted");
	}
	ADD(card, facts);
	ADD(content, card);
	card = axis_web_storage_card(g_strcmp0(u->kind, "series") == 0 || g_strcmp0(u->kind, "folder") == 0 ? "Episodes" :
		"Files");
	if (work->entries == NULL || work->entries->len == 0)
		ADD(card, axis_web_storage_text("help", work->after > 0 ? "There is nothing after this." :
			"No file of this movie or series is in the list."));
	else
		ADD(card, entries_table(request, work->info, work->entries, "conversion-run-entries", changeable, &placed));
	{
		g_autofree gchar *base = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/units/%d", work->id, u->position);
		g_autofree gchar *first = work->after > 0 ? g_strdup(base) : NULL;
		g_autofree gchar *next = work->more && work->entries->len > 0 ? g_strdup_printf("%s?after=%d", base,
			((const AxisConversionRunEntry *)g_ptr_array_index(work->entries, work->entries->len - 1))->position) : NULL;

		ADD(card, axis_web_browser_pager(first, next));
	}
	ADD(content, card);
	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static gint64
param_number(
	GHashTable  *params,
	const gchar *name
){
	const gchar *text = params != NULL ? g_hash_table_lookup(params, name) : NULL;
	guint64 n = 0;

	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 1, (guint64)G_MAXINT32, &n, NULL))
		return 0;
	return (gint64)n;
}

static HtmxResponse *
handle_unit(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	UnitWork *work;
	gint64 id = axis_web_storage_param_id(params);
	gint64 position = param_number(params, "pos");
	gint64 after = 0;

	if (id <= 0 || position <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such movie or series in this run.");
	if (!query_number(request, "after", &after) || after > G_MAXINT32)
		return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
	work = g_new0(UnitWork, 1);
	work->id = id;
	work->position = (gint)position;
	work->after = (gint)after;
	return dispatch(user_data, request, unit_read, unit_done, work, unit_work_free);
}

/* ── What is not in the list ────────────────────────────────────── */

typedef struct
{
	gint64     id;
	gchar     *reason;
	gint64     after_size;
	gint64     after_file;
	gint64     files;
	GPtrArray *list;
	gboolean   more;
	GError    *error;
} LeftWork;

static void
left_work_free(gpointer data)
{
	LeftWork *work = data;

	g_free(work->reason);
	g_clear_pointer(&work->list, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
left_read(
	AxisWebRuns *self,
	gpointer     data
){
	LeftWork *work = data;

	axis_conversion_runs_excluded(self->runs, work->id, work->reason, work->after_size, work->after_file,
		AXIS_CONVERSION_RUN_ENTRY_PAGE, &work->list, &work->more, &work->error);
}

static HtmxResponse *
left_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "Library folder", AXIS_WEB_COL_TEXT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ NULL, 0 }
	};
	LeftWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = NULL;
	HtmxNode *card;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	label = g_strdup_printf("Files left out of conversion run %" G_GINT64_FORMAT ": %s", work->id,
		axis_conversion_run_reason_label(work->reason));
	ADD(content, crumbs(work->id));
	ADD(content, heading(1, label));
	ADD(content, axis_web_page_intro(axis_conversion_run_reason_help(work->reason)));
	card = axis_web_storage_card("Files left out for this reason");
	if (work->list->len == 0) {
		ADD(card, axis_web_storage_text("help", "There is no file left out for this reason."));
	} else {
		HtmxNode *table = axis_web_table_new("conversion-run-left-out", columns);
		HtmxTbody *body = htmx_tbody_new();

		for (i = 0; i < work->list->len; i++) {
			const AxisConversionRunExcluded *x = g_ptr_array_index(work->list, i);
			HtmxTr *row = htmx_tr_new();

			if (x->rel_path != NULL) {
				g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, x->file_id);
				HtmxA *a = htmx_a_new_with_href(href);

				ADD(a, axis_web_browser_name_node(x->rel_path));
				ADD(row, cell_node(HTMX_NODE(a)));
			} else {
				ADD(row, cell_text("(no longer in the library)"));
			}
			ADD(row, cell_text(x->root_name));
			ADD(row, cell_size(x->size));
			ADD(body, row);
		}
		ADD(table, body);
		ADD(card, table);
	}
	{
		const AxisConversionRunExcluded *last = work->list->len > 0 ?
			g_ptr_array_index(work->list, work->list->len - 1) : NULL;
		g_autofree gchar *first = work->after_file > 0 ? g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT
			"/left-out?reason=%s", work->id, work->reason) : NULL;
		g_autofree gchar *next = work->more && last != NULL ? g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT
			"/left-out?reason=%s&size=%" G_GINT64_FORMAT "&file=%" G_GINT64_FORMAT, work->id, work->reason, last->size,
			last->file_id) : NULL;

		ADD(card, axis_web_browser_pager(first, next));
	}
	ADD(content, card);
	return axis_web_page_respond(request, label, HTMX_NODE(content));
}

static gboolean
reason_valid(const gchar *reason)
{
	const gchar *p;

	if (reason == NULL || reason[0] == '\0' || strlen(reason) > 32)
		return FALSE;
	for (p = reason; *p != '\0'; p++) {
		if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && *p != '_')
			return FALSE;
	}
	return TRUE;
}

static HtmxResponse *
handle_left_out(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	LeftWork *work;
	gint64 id = axis_web_storage_param_id(params);
	const gchar *reason = htmx_request_get_query_param(request, "reason");
	gint64 size = 0;
	gint64 file = 0;

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion run.");
	if (!reason_valid(reason) || !query_number(request, "size", &size) || !query_number(request, "file", &file))
		return axis_web_error_respond(request, 400, "Bad request", "The address is not valid.");
	work = g_new0(LeftWork, 1);
	work->id = id;
	work->reason = g_strdup(reason);
	work->after_size = size;
	work->after_file = file;
	return dispatch(user_data, request, left_read, left_done, work, left_work_free);
}

/* ── Actions ────────────────────────────────────────────────────── */

typedef enum
{
	ACT_START,
	ACT_PAUSE,
	ACT_RESUME,
	ACT_CANCEL,
	ACT_REPLACE,
	ACT_LIMITS,
	ACT_REMOVE_UNIT,
	ACT_REMOVE_ENTRY,
	ACT_INCLUDE_KEPT
} Act;

typedef struct
{
	Act                      act;
	gint64                   id;
	gint64                   target;	/* a unit's position, an entry */
	gint                     unit;	/* the unit's page to go back to */
	gint                     after;	/* the page of the list to go back to */
	AxisConversionOutcome    outcome;
	gboolean                 confirmed;
	gint64                   kept_seen;	/* the files Axis keeps the page showed (a switch to replacing) */
	AxisConversionRunLimits  limits;
	gchar                   *user;
	GError                  *error;
} ActWork;

static void
act_work_free(gpointer data)
{
	ActWork *work = data;

	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
act_run(
	AxisWebRuns *self,
	gpointer     data
){
	ActWork *work = data;

	switch (work->act) {
	case ACT_START:
		axis_conversion_runs_start(self->runs, work->id, work->outcome, work->confirmed, work->user, &work->error);
		break;
	case ACT_PAUSE:
		axis_conversion_runs_pause(self->runs, work->id, work->user, &work->error);
		break;
	case ACT_RESUME:
		axis_conversion_runs_resume(self->runs, work->id, work->user, &work->error);
		break;
	case ACT_CANCEL:
		axis_conversion_runs_cancel(self->runs, work->id, work->user, &work->error);
		break;
	case ACT_REPLACE:
		axis_conversion_runs_replace(self->runs, work->id, work->confirmed, work->kept_seen, work->user, &work->error);
		break;
	case ACT_LIMITS:
		axis_conversion_runs_set_limits(self->runs, work->id, &work->limits, work->user, &work->error);
		break;
	case ACT_REMOVE_UNIT:
		axis_conversion_runs_remove_unit(self->runs, work->id, (gint)work->target, work->user, &work->error);
		break;
	case ACT_INCLUDE_KEPT:
		axis_conversion_runs_include_kept(self->runs, work->id, work->confirmed, work->user, &work->error);
		break;
	default:
		axis_conversion_runs_remove_entry(self->runs, work->id, work->target, work->user, &work->error);
		break;
	}
}

static HtmxResponse *
act_done(
	AxisWebRuns *self,
	HtmxRequest *request,
	gpointer     data
){
	static const gchar *const keys[] = {
		"started", "paused", "resumed", "cancelled", "replacing", "limits", "removed", "removed", "kept"
	};
	ActWork *work = data;
	g_autofree gchar *location = NULL;
	g_autofree gchar *place = NULL;

	if (work->error != NULL)
		return error_page(request, work->error);
	axis_web_storage_audit(request, "conversion run %" G_GINT64_FORMAT ": %s", work->id, keys[work->act]);
	if (work->act == ACT_REMOVE_ENTRY && work->unit > 0) {
		/* The row it was taken out of, on the unit's page. */
		location = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "/units/%d", work->id, work->unit);
		place = g_strdup_printf("run-entry-%" G_GINT64_FORMAT, work->target);
	} else if (work->act == ACT_REMOVE_UNIT) {
		/* The row stays in its page of the list, marked taken out. */
		location = work->after > 0 ? g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT "?after=%d", work->id,
			work->after) : g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, work->id);
		place = g_strdup_printf("run-unit-%d", (gint)work->target);
	} else if (work->act == ACT_INCLUDE_KEPT) {
		location = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, work->id);
		place = g_strdup("run-left-out");
	} else {
		location = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, work->id);
		place = g_strdup("run-controls");
	}
	return axis_web_place_redirect(request, location, work->act == ACT_REMOVE_ENTRY ? "removed" : keys[work->act],
		place);
}

/* A form's number (empty: 0); FALSE when it is not one. */
static gboolean
form_number(
	HtmxRequest *request,
	const gchar *name,
	gint64       max,
	gint64      *value
){
	const gchar *text = htmx_request_get_form_value(request, name);
	guint64 n = 0;

	*value = 0;
	if (text == NULL || text[0] == '\0')
		return TRUE;
	if (!g_ascii_string_to_unsigned(text, 10, 0, (guint64)max, &n, NULL))
		return FALSE;
	*value = (gint64)n;
	return TRUE;
}

/* "HH:MM" (empty: -1); FALSE when it is not a time. */
static gboolean
form_clock(
	HtmxRequest *request,
	const gchar *name,
	gint        *minute
){
	const gchar *text = htmx_request_get_form_value(request, name);
	guint64 hours = 0;
	guint64 minutes = 0;
	g_auto(GStrv) parts = NULL;

	*minute = -1;
	if (text == NULL || text[0] == '\0')
		return TRUE;
	parts = g_strsplit(text, ":", 3);
	if (g_strv_length(parts) != 2 || !g_ascii_string_to_unsigned(parts[0], 10, 0, 23, &hours, NULL) ||
	    !g_ascii_string_to_unsigned(parts[1], 10, 0, 59, &minutes, NULL))
		return FALSE;
	*minute = (gint)(hours * 60 + minutes);
	return TRUE;
}

static HtmxResponse *
act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	Act          what
){
	ActWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion run.");
	work = g_new0(ActWork, 1);
	work->act = what;
	work->id = id;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->confirmed = axis_web_storage_form_yes(request, "confirm");
	if (what == ACT_START) {
		if (!axis_conversion_outcome_from_string(htmx_request_get_form_value(request, "outcome"), &work->outcome)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "Choose trials or replacing originals.");
		}
		if (work->outcome == AXIS_CONVERSION_REPLACE && !work->confirmed) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "Replacing originals deletes them: tick the box to "
				"confirm.");
		}
	}
	if (what == ACT_REPLACE && !work->confirmed) {
		act_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "Replacing originals deletes them: tick the box to "
			"confirm.");
	}
	if (what == ACT_REPLACE) {
		guint64 seen = 0;

		/* A form without the count covers none of the files Axis keeps. */
		if (axis_web_storage_form_u64(request, "kept_seen", &seen) && seen <= G_MAXINT32)
			work->kept_seen = (gint64)seen;
	}
	if (what == ACT_LIMITS) {
		gint64 at_once = 0;
		gint64 gib = 0;
		gint64 hours = 0;

		if (!form_number(request, "at_once", AXIS_CONVERSION_RUN_MAX_AT_ONCE, &at_once) ||
		    !form_number(request, "cap_files", G_MAXINT32, &work->limits.cap_files) ||
		    !form_number(request, "cap_gib", G_GINT64_CONSTANT(1) << 33, &gib) ||
		    !form_number(request, "cap_hours", 24 * 366, &hours) ||
		    !form_clock(request, "window_from", &work->limits.window_start) ||
		    !form_clock(request, "window_to", &work->limits.window_end)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "A limit is not a number, or an hour is not HH:MM.");
		}
		work->limits.at_once = (gint)MAX(at_once, 1);
		work->limits.cap_bytes = gib * G_GINT64_CONSTANT(1073741824);
		work->limits.cap_seconds = hours * 3600;
	}
	if (what == ACT_REMOVE_UNIT) {
		gint64 after = 0;

		work->target = param_number(params, "pos");
		if (work->target <= 0) {
			act_work_free(work);
			return axis_web_error_respond(request, 404, "Not found", "There is no such movie or series in this run.");
		}
		/* The page of the list it was taken out on. */
		if (!form_number(request, "after", G_MAXINT32, &after)) {
			act_work_free(work);
			return axis_web_error_respond(request, 400, "Not done", "The page of the list is not a number.");
		}
		work->after = (gint)after;
	}
	if (what == ACT_REMOVE_ENTRY) {
		const gchar *text = params != NULL ? g_hash_table_lookup(params, "entry") : NULL;
		guint64 n = 0;
		guint64 unit = 0;

		if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 1, (guint64)G_MAXINT64, &n, NULL)) {
			act_work_free(work);
			return axis_web_error_respond(request, 404, "Not found", "There is no such file in the run.");
		}
		work->target = (gint64)n;
		/* Back to the unit's page it was taken out on. */
		if (axis_web_storage_form_u64(request, "unit", &unit) && unit > 0 && unit <= G_MAXINT32)
			work->unit = (gint)unit;
	}
	return dispatch(user_data, request, act_run, act_done, work, act_work_free);
}

static HtmxResponse *
handle_start(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_START);
}

static HtmxResponse *
handle_pause(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_PAUSE);
}

static HtmxResponse *
handle_resume(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_RESUME);
}

static HtmxResponse *
handle_cancel(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_CANCEL);
}

static HtmxResponse *
handle_replace(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_REPLACE);
}

static HtmxResponse *
handle_limits(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_LIMITS);
}

static HtmxResponse *
handle_remove_unit(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_REMOVE_UNIT);
}

static HtmxResponse *
handle_remove_entry(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_REMOVE_ENTRY);
}

static HtmxResponse *
handle_include_kept(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return act(request, params, user_data, ACT_INCLUDE_KEPT);
}

/* ── The dashboard ──────────────────────────────────────────────── */

static gpointer
dashboard_collect(gpointer user_data)
{
	AxisWebRuns *self = user_data;
	g_autoptr(GError) error = NULL;
	AxisConversionRunInfo *info = NULL;

	if (!axis_conversion_runs_active(self->runs, &info, &error)) {
		g_debug("dashboard: cannot read the conversion runs: %s", error->message);
		return NULL;
	}
	return info;
}

static void
dashboard_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          figures,
	gpointer          user_data
){
	const AxisConversionRunInfo *info = figures;
	g_autofree gchar *href = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *text = NULL;
	HtmxDiv *piece;
	HtmxP *line;
	gint64 listed;

	if (info == NULL || !axis_web_request_is_admin(request))
		return;
	href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, info->id);
	listed = info->waiting + info->asked + info->done + info->skipped + info->failed;
	saved = size_text(g_strcmp0(info->outcome, "replace") == 0 ? info->done_before - info->done_after : 0);
	text = g_strdup_printf("%s, %s with the conversion profile %s, version %d:%" G_GINT64_FORMAT " of %" G_GINT64_FORMAT " files done, %s "
		"saved.%s%s", g_strcmp0(info->state, "running") == 0 ? "Running" : "Paused",
		g_strcmp0(info->outcome, "replace") == 0 ? "replacing originals" : "trials", info->profile_name,
		info->revision_number, info->done, listed, saved, info->wait_text != NULL &&
		g_strcmp0(info->state, "running") == 0 ? " Waiting: " : "", info->wait_text != NULL &&
		g_strcmp0(info->state, "running") == 0 ? info->wait_text : "");
	piece = htmx_div_new();
	line = htmx_p_new();
	{
		g_autofree gchar *name = run_title(info->id);
		g_autofree gchar *label = g_strdup_printf("%s (%s)", name, info->scope_text);

		ADD(line, axis_web_browser_link(href, label));
	}
	ADD(piece, line);
	ADD(piece, axis_web_storage_text(NULL, text));
	if (listed > 0)
		ADD(piece, files_bar(info));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_NOW, 30, href, HTMX_NODE(piece));
	/* A paused run is the attention source's to say, below. */
	(void)user_data;
}

/* ── What needs attention ───────────────────────────────────────── */

/*
 * The conversion runs that wait for a person (add-attention-notices, slice
 * N2), one item each (the newest five; more are one line), from one
 * statement over the few rows of conversion_run:
 *
 *   paused by itself  failures in a row, refusals, a crash or a bad trial
 *                     stopped it: only a person's Resume ends that, and
 *                     nothing of the run is converted meanwhile: "Waiting for
 *                     you".
 *   paused by a       the person knows: "Worth a look", so that a run paused
 *   person, or ready  for the night, or planned and not started, is not
 *   and not started   forgotten.
 *
 * A paused run has no wait: it does not end by itself.  A run that is ready
 * is a notice AXIS_ATTENTION_READY_WAIT_S after its list was finished: the
 * person who planned it is usually on its page then and starts it or throws
 * it away, and a notice for those minutes would only be noise.  "newest" is
 * the second the run was paused (0 for one that is ready), so a run resumed
 * and paused again is a new occurrence.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebRuns *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GArray) rows = g_array_new(FALSE, FALSE, sizeof(guint));
	guint n;
	guint k;

	if (!axis_conversion_runs_waiting(self->runs, reading->deadline_ms, &result, error))
		return FALSE;
	/* The rows that are notices now: every paused run, and a ready one whose
	 * list was finished long enough ago. */
	for (k = 0; k < axis_db_result_get_n_rows(result); k++) {
		gint64 listed = 0;

		axis_db_result_get_int64(result, k, 4, &listed);
		if (g_strcmp0(axis_db_result_get_text(result, k, 1), "ready") == 0 &&
		    reading->now_us - listed * G_USEC_PER_SEC < (gint64)AXIS_ATTENTION_READY_WAIT_S * G_USEC_PER_SEC)
			continue;
		g_array_append_val(rows, k);
	}
	n = rows->len;
	if (n > AXIS_ATTENTION_MAX_OF_A_KIND) {
		AxisAttentionItem *item = axis_attention_item_new("conversion.run", "", AXIS_ATTENTION_LOOK, n, 0);
		gint64 newest = 0;

		axis_db_result_get_int64(result, g_array_index(rows, guint, 0), 0, &newest);
		item->newest = newest;
		axis_attention_item_say(item, "More than 5 conversion runs are paused or ready and not started.", NULL, NULL);
		axis_attention_item_link(item, "/conversions/runs", "Open Conversion runs");
		g_ptr_array_add(items, item);
		return TRUE;
	}
	for (k = 0; k < n; k++) {
		guint i = g_array_index(rows, guint, k);
		const gchar *state = axis_db_result_get_text(result, i, 1);
		const gchar *code = axis_db_result_get_text(result, i, 2);
		gboolean paused = g_strcmp0(state, "paused") == 0;
		gboolean by_person = paused && g_strcmp0(code, "person") == 0;
		g_autofree gchar *subject = NULL;
		g_autofree gchar *href = NULL;
		g_autofree gchar *name = NULL;
		AxisAttentionItem *item;
		gint64 id = 0;
		gint64 since = 0;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_int64(result, i, 3, &since);
		subject = g_strdup_printf("%" G_GINT64_FORMAT, id);
		href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, id);
		name = run_title(id);
		item = axis_attention_item_new(paused ? "conversion.paused" : "conversion.ready", subject,
			paused && !by_person ? AXIS_ATTENTION_WAITING : AXIS_ATTENTION_LOOK, 1, since);
		axis_attention_item_say(item, "", name, !paused ? " is ready and was not started." : by_person ?
			" is paused: a person paused it." : " paused itself. Open it to see why, then resume it.");
		axis_attention_item_link(item, href, "Open the conversion run");
		item->since_us = since * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

static void
dashboard_free(gpointer figures)
{
	axis_conversion_run_info_free(figures);
}

/* ── The module's part ──────────────────────────────────────────── */

HtmxNode *
axis_web_runs_card(void)
{
	HtmxNode *card = axis_web_storage_card("Conversion runs");
	HtmxP *p = htmx_p_new();

	ADD(card, axis_web_storage_text("help", "To convert a whole library rather than one file: a conversion run lists "
		"what would save the most first, keeps a series together, and works through the list one file at a time."));
	ADD(p, axis_web_browser_link("/conversions/runs", "Conversion runs"));
	ADD(p, axis_web_storage_text(NULL, " "));
	ADD(p, axis_web_browser_link("/conversions/runs/new", "Plan a conversion run"));
	ADD(card, p);
	return card;
}

void
axis_web_runs_register(
	AxisWebRuns *self,
	AxisWeb     *web
){
	axis_web_add_dashboard_part(web, dashboard_collect, dashboard_render, dashboard_free, self);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_NOW);
	axis_web_add_attention_source(web, "the conversion runs", attention_source, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/runs", AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs", AXIS_WEB_ROUTE_ADMIN, handle_create, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/runs/new", AXIS_WEB_ROUTE_ADMIN, handle_new, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/runs/:id", AXIS_WEB_ROUTE_ADMIN, handle_run, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/runs/:id/status", AXIS_WEB_ROUTE_ADMIN, handle_status, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/runs/:id/units/:pos", AXIS_WEB_ROUTE_ADMIN, handle_unit,
		self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/runs/:id/left-out", AXIS_WEB_ROUTE_ADMIN, handle_left_out,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/start", AXIS_WEB_ROUTE_ADMIN, handle_start, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/pause", AXIS_WEB_ROUTE_ADMIN, handle_pause, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/resume", AXIS_WEB_ROUTE_ADMIN, handle_resume, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/cancel", AXIS_WEB_ROUTE_ADMIN, handle_cancel, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/replace", AXIS_WEB_ROUTE_ADMIN, handle_replace,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/limits", AXIS_WEB_ROUTE_ADMIN, handle_limits, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/units/:pos/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove_unit, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/entries/:entry/remove", AXIS_WEB_ROUTE_ADMIN,
		handle_remove_entry, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/runs/:id/include-kept", AXIS_WEB_ROUTE_ADMIN,
		handle_include_kept, self);
}

AxisWebRuns *
axis_web_runs_new(AxisConversionRuns *runs)
{
	AxisWebRuns *self = g_new0(AxisWebRuns, 1);

	self->runs = axis_conversion_runs_ref(runs);
	return self;
}

void
axis_web_runs_stop(AxisWebRuns *self)
{
	if (self == NULL)
		return;
	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_runs_free(AxisWebRuns *self)
{
	if (self == NULL)
		return;
	axis_web_runs_stop(self);
	axis_conversion_runs_unref(self->runs);
	g_free(self);
}
