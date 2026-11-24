/*
 * axis-web-conversions.c - the conversions module: its lifetime, jobs and
 * settings, the work it hands to threads, the file page's card, and the
 * pages under Media management -> Conversions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every page and form is an administrator's (AXIS_WEB_ROUTE_ADMIN), every
 * POST carries the CSRF token, and what a request does is decided again on
 * the server: the confirmation page shows the gates, the POST evaluates them
 * once more, whatever the form said.  A file's name is set as text (its
 * stored form, axis_web_browser_name_node()); nothing of a name is in a URL.
 */

#include "web/axis-web-conversions.h"

#include <string.h>

#include "compat/axis-compat.h"
#include "compat/axis-compat-report.h"
#include "core/axis-size.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library-name.h"
#include "web/axis-web.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-conversion-runs.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add
#define MAX_PENDING 32
#define LIST_LIMIT 200
/* How often a block that follows a conversion in flight asks again. */
#define PROGRESS_POLL "every 5s"
#define LATEST_POLL "every 15s"

struct _AxisWebConversions
{
	AxisConversions    *conversions;
	AxisConversionRuns *runs;
	AxisWebRuns        *runs_ui;
	guint               pending;
	gboolean            stopping;
	guint               listener;
	gint                enabled;	/* atomic: the module is on */
};

typedef void (*WorkFunc)(AxisWebConversions *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisWebConversions *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisWebConversions *owner;
	WorkFunc            work;
	DoneFunc            done;
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
	g_object_unref(response);
	if (task->free_data != NULL)
		task->free_data(task->data);
	g_clear_object(&task->request);
	g_clear_object(&task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebConversions *self,
	HtmxRequest        *request,
	WorkFunc            work,
	DoneFunc            done,
	gpointer            data,
	GDestroyNotify      free_data
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
cell_time(gint64 us)
{
	g_autofree gchar *text = axis_web_format_time(us, "");

	return cell_text(text);
}

static HtmxNode *
link_node(
	const gchar *href,
	HtmxNode    *inside
){
	HtmxA *a = htmx_a_new_with_href(href);

	ADD(a, inside);
	return HTMX_NODE(a);
}

/* A file's name as a link to its page (the stored form's last part). */
static HtmxNode *
file_name(
	gint64       file_id,
	const gchar *rel_path,
	gboolean     whole
){
	const gchar *slash = rel_path != NULL ? strrchr(rel_path, '/') : NULL;
	g_autofree gchar *shown = axis_library_name_display(whole || slash == NULL ? rel_path : slash + 1);
	HtmxNode *bdi = HTMX_NODE(htmx_bdi_new());

	ADD(bdi, axis_web_breakable(shown));
	if (file_id <= 0)
		return bdi;
	{
		g_autofree gchar *href = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, file_id);

		return link_node(href, bdi);
	}
}

static HtmxNode *
state_badge(const gchar *state)
{
	const gchar *css = g_strcmp0(state, "done") == 0 ? "badge-succeeded" :
		g_strcmp0(state, "rejected") == 0 || g_strcmp0(state, "failed") == 0 || g_strcmp0(state, "refused") == 0 ?
		"badge-failed" : g_strcmp0(state, "cancelled") == 0 ? "badge-note" :
		g_strcmp0(state, "queued") == 0 ? "badge-warn" : "badge-running";

	return axis_web_browser_badge(axis_conversion_state_label(state), css);
}

static gchar *
profile_label(
	const gchar *name,
	gint         number
){
	return g_strdup_printf("%s, version %d", name != NULL ? name : "?", number);
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

/* ── Progress ───────────────────────────────────────────────────── */

/* "about 40 seconds", "about 12 minutes", "about 3 hours 20 minutes" */
static gchar *
left_text(gint64 seconds)
{
	gint64 minutes;

	if (seconds < 60)
		return g_strdup("less than a minute");
	minutes = (seconds + 30) / 60;
	if (minutes < 60)
		return g_strdup_printf("about %" G_GINT64_FORMAT " minute%s", minutes, minutes == 1 ? "" : "s");
	if (minutes % 60 == 0)
		return g_strdup_printf("about %" G_GINT64_FORMAT " hour%s", minutes / 60, minutes / 60 == 1 ? "" : "s");
	return g_strdup_printf("about %" G_GINT64_FORMAT " hour%s %" G_GINT64_FORMAT " minute%s", minutes / 60,
		minutes / 60 == 1 ? "" : "s", minutes % 60, minutes % 60 == 1 ? "" : "s");
}

gchar *
axis_web_conversions_progress_text(const AxisConversionProgress *progress)
{
	GString *text;

	g_return_val_if_fail(progress != NULL, NULL);

	if (progress->stage == NULL)
		return g_strdup("Not in flight");
	text = g_string_new(axis_conversion_progress_stage_label(progress->stage));
	if (progress->fraction < 0.0) {
		if (g_strcmp0(progress->stage, "encoding") == 0 || g_strcmp0(progress->stage, "checking") == 0)
			g_string_append(text, ": no progress reported yet");
		return g_string_free(text, FALSE);
	}
	g_string_append_printf(text, ": %d%%", (gint)(progress->fraction * 100.0 + 0.5));
	if (progress->speed > 0.0)
		g_string_append_printf(text, ", %.1f times real time", progress->speed);
	if (progress->left_s >= 0 && progress->fraction < 1.0) {
		g_autofree gchar *left = left_text(progress->left_s);

		g_string_append_printf(text, ", %s left", left);
	}
	return g_string_free(text, FALSE);
}

HtmxNode *
axis_web_conversions_progress_node(const AxisConversionProgress *progress)
{
	g_autofree gchar *text = axis_web_conversions_progress_text(progress);
	HtmxDiv *box = htmx_div_new();
	HtmxSpan *words = htmx_span_new();
	HtmxProgress *bar;

	htmx_element_add_class(HTMX_ELEMENT(box), "conversion-progress");
	/* No value: the bar is indeterminate (the element's own way of saying
	 * that work goes on and how far is not known). */
	if (progress->fraction < 0.0)
		bar = htmx_progress_new();
	else
		bar = htmx_progress_new_with_value((gdouble)(gint)(progress->fraction * 100.0 + 0.5), 100.0);
	/* The same words beside the bar and as its name, for a reader that
	 * does not show it. */
	htmx_element_set_attribute(HTMX_ELEMENT(bar), "aria-label", text);
	htmx_element_add_class(HTMX_ELEMENT(words), "conversion-progress-text");
	htmx_node_set_text_content(HTMX_NODE(words), text);
	ADD(box, bar);
	ADD(box, words);
	return HTMX_NODE(box);
}

HtmxNode *
axis_web_conversions_bar(
	gdouble      value,
	gdouble      max,
	const gchar *text
){
	HtmxDiv *box = htmx_div_new();
	HtmxSpan *words = htmx_span_new();
	HtmxProgress *bar = htmx_progress_new_with_value(CLAMP(value, 0.0, MAX(max, 0.0)), MAX(max, 0.0));

	htmx_element_add_class(HTMX_ELEMENT(box), "conversion-progress");
	htmx_element_set_attribute(HTMX_ELEMENT(bar), "aria-label", text);
	htmx_element_add_class(HTMX_ELEMENT(words), "conversion-progress-text");
	htmx_node_set_text_content(HTMX_NODE(words), text);
	ADD(box, bar);
	ADD(box, words);
	return HTMX_NODE(box);
}

static void
record_progress(
	const AxisConversionRecord *r,
	AxisConversionProgress     *progress
){
	axis_conversion_progress(r->state, r->outcome, r->job_state, r->job_fraction, r->job_running_s, r->verify_state,
		r->verify_fraction, r->verify_running_s, r->duration_ms, progress);
}

static gboolean
in_flight(const gchar *state)
{
	return g_strcmp0(state, "queued") == 0 || g_strcmp0(state, "encoding") == 0 ||
		g_strcmp0(state, "verifying") == 0 || g_strcmp0(state, "placing") == 0;
}

/*
 * The block of a conversion's page that follows it while it is in flight:
 * the stage in words, the bar, and what the worker says.  It asks for itself
 * again (htmx, every PROGRESS_POLL) for as long as the conversion is in
 * flight; the block of one that ended asks for nothing and points at the
 * page, whose checks and result are read when it is loaded.
 */
static HtmxNode *
progress_block(
	const AxisConversionRecord *r,
	gboolean                    polled
){
	HtmxDiv *block = htmx_div_new();
	AxisConversionProgress progress;

	htmx_element_set_id(HTMX_ELEMENT(block), "conversion-progress");
	record_progress(r, &progress);
	if (progress.stage == NULL) {
		/* Only a block that was following a conversion says that it ended. */
		if (polled) {
			g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, r->id);
			g_autofree gchar *text = g_strdup_printf("The conversion is no longer in flight (%s). ",
				axis_conversion_state_label(r->state));
			HtmxP *line = htmx_p_new();
			HtmxSpan *words = htmx_span_new();

			htmx_element_add_class(HTMX_ELEMENT(line), "notice");
			htmx_node_set_text_content(HTMX_NODE(words), text);
			ADD(line, words);
			ADD(line, axis_web_browser_link(href, "Load the page again to see its checks and result"));
			ADD(block, line);
		}
		return HTMX_NODE(block);
	}
	{
		g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/progress", r->id);

		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-get", href);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-trigger", PROGRESS_POLL);
		htmx_element_set_attribute(HTMX_ELEMENT(block), "hx-swap", "outerHTML");
	}
	ADD(block, axis_web_conversions_progress_node(&progress));
	/* What the worker says of it, as it says it. */
	if (g_strcmp0(progress.stage, "encoding") == 0 && g_strcmp0(r->job_state, "running") == 0 &&
	    r->job_message != NULL && r->job_message[0] != '\0')
		ADD(block, axis_web_storage_text("help", r->job_message));
	else if (g_strcmp0(progress.stage, "checking") == 0 && g_strcmp0(r->verify_state, "running") == 0 &&
	    r->verify_message != NULL && r->verify_message[0] != '\0')
		ADD(block, axis_web_storage_text("help", r->verify_message));
	return HTMX_NODE(block);
}

static const gchar *
done_text(const gchar *key)
{
	if (g_strcmp0(key, "requested") == 0)
		return "The conversion is queued: a worker that can reach the file's disk takes it, then this server "
			"checks the result. This page follows its progress; load it again to see each check as it is made.";
	if (g_strcmp0(key, "cancelled") == 0)
		return "The conversion was cancelled; whatever a worker made is removed.";
	if (g_strcmp0(key, "trial-removed") == 0)
		return "The trial's file was removed.";
	if (g_strcmp0(key, "set-aside") == 0)
		return "The trial is set aside: it counts neither for nor against proving its version. A conversion run of "
			"trials that paused for it can be resumed once nothing else counts against the version.";
	if (g_strcmp0(key, "proven") == 0)
		return "The version is proven by hand: files can now be replaced with it where the playback verdict is safe.";
	if (g_strcmp0(key, "resumed") == 0)
		return "Conversion work is resumed: encodes, checks and sample measurements are handed out and run again, and "
			"the checks that waited are queued.";
	if (g_strcmp0(key, "not-held") == 0)
		return "Conversion work was not paused.";
	return NULL;
}

HtmxNode *
axis_web_conversions_paused_notice(
	HtmxRequest     *request,
	AxisConversions *conversions
){
	g_autofree gchar *when = NULL;
	g_autofree gchar *text = NULL;
	HtmxDiv *box;
	HtmxNode *form;
	gint64 since = 0;

	if (conversions == NULL || !axis_conversions_is_held(conversions, &since))
		return NULL;
	when = axis_web_format_time(since, "");
	text = g_strdup_printf("Conversion work has been paused since Axis started (%s): nothing is encoded, checked or "
		"sample-measured for conversions, on this server or on a worker, until you resume it. The setting "
		"\"Conversion work waits after a restart\" (convert.wait_after_start) is on, so that a server that stopped "
		"under the load of a conversion does not start it again by itself. Downloads, imports and scans go on.",
		when);
	box = htmx_div_new();
	htmx_element_add_class(HTMX_ELEMENT(box), "notice");
	htmx_element_add_class(HTMX_ELEMENT(box), "notice-warning");
	ADD(box, axis_web_storage_text(NULL, text));
	form = axis_web_auth_post_form(request, "/conversions/resume");
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_auth_button("Resume conversion work", NULL));
	ADD(box, form);
	return HTMX_NODE(box);
}

/* "Checked on": where the measurements were made, and by whom. */
static gchar *
checked_on(const AxisConversionRecord *r)
{
	if (g_strcmp0(r->checked_where, "server") == 0)
		return g_strdup("This server");
	if (g_strcmp0(r->checked_where, "worker") != 0)
		return g_strdup("Not yet");
	if (r->verifier == NULL)
		return g_strdup("A worker (its measurements have not come yet)");
	if (g_strcmp0(r->verifier, r->worker) == 0)
		return g_strdup_printf("Worker %s, the worker that converted it (no other worker could)", r->verifier);
	return g_strdup_printf("Worker %s (it was converted on %s)", r->verifier, r->worker != NULL ? r->worker : "?");
}

static HtmxNode *
read_only_notice(AxisWebConversions *self)
{
	if (!axis_storage_is_read_only_mode(axis_conversions_get_storage(self->conversions)))
		return NULL;
	return axis_web_storage_text("notice notice-warning", "Storage is read-only (Settings, Read-only storage): "
		"nothing can be converted, and nothing in flight is placed, until an administrator switches it off.");
}

/* ── Conversions ────────────────────────────────────────────────── */

typedef struct
{
	GPtrArray *records;
	GPtrArray *revisions;
	GPtrArray *leftovers;
	GError    *error;
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_clear_pointer(&work->records, g_ptr_array_unref);
	g_clear_pointer(&work->revisions, g_ptr_array_unref);
	g_clear_pointer(&work->leftovers, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
list_run(
	AxisWebConversions *self,
	gpointer            data
){
	ListWork *work = data;

	if (!axis_conversions_list(self->conversions, LIST_LIMIT, &work->records, &work->error))
		return;
	if (!axis_conversions_trial_revisions(self->conversions, &work->revisions, &work->error))
		return;
	axis_conversions_leftovers(self->conversions, &work->leftovers, &work->error);
}

static HtmxNode *
conversions_table(GPtrArray *records)
{
	static const AxisWebColumn columns[] = {
		{ "File", AXIS_WEB_COL_NAME },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Conversion profile", AXIS_WEB_COL_TEXT },
		{ "Trial or replacement", AXIS_WEB_COL_TEXT },
		{ "Before", AXIS_WEB_COL_SIZE },
		{ "After", AXIS_WEB_COL_SIZE },
		{ "Asked", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("conversions", columns);
	HtmxTbody *body = htmx_tbody_new();
	HtmxDiv *holder = htmx_div_new();
	gboolean any_in_flight = FALSE;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(holder), "conversions-latest");
	for (i = 0; i < records->len; i++) {
		const AxisConversionRecord *r = g_ptr_array_index(records, i);
		g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, r->id);
		g_autofree gchar *profile = profile_label(r->profile_name, r->revision_number);
		const gchar *slash = r->rel_path != NULL ? strrchr(r->rel_path, '/') : NULL;
		g_autofree gchar *shown = axis_library_name_display(slash != NULL ? slash + 1 : r->rel_path);
		HtmxTr *row = htmx_tr_new();
		HtmxNode *bdi = HTMX_NODE(htmx_bdi_new());

		ADD(bdi, axis_web_breakable(shown));
		ADD(row, cell_node(link_node(href, bdi)));
		if (in_flight(r->state)) {
			/* In flight: its state, and how far it is. */
			HtmxDiv *cell = htmx_div_new();
			AxisConversionProgress progress;

			any_in_flight = TRUE;
			record_progress(r, &progress);
			ADD(cell, state_badge(r->state));
			ADD(cell, axis_web_conversions_progress_node(&progress));
			ADD(row, cell_node(HTMX_NODE(cell)));
		} else {
			ADD(row, cell_node(state_badge(r->state)));
		}
		ADD(row, cell_text(profile));
		ADD(row, cell_text(g_strcmp0(r->outcome, "replace") == 0 ? "Replacement" : "Trial"));
		ADD(row, cell_size(r->src_size));
		ADD(row, cell_size(r->output_size > 0 ? r->output_size : -1));
		ADD(row, cell_time(r->requested_us));
		ADD(body, row);
	}
	ADD(table, body);
	/* While a conversion is in flight the table asks for itself again; once
	 * none is, what comes back asks for nothing. */
	if (any_in_flight) {
		htmx_element_set_attribute(HTMX_ELEMENT(holder), "hx-get", "/conversions/latest");
		htmx_element_set_attribute(HTMX_ELEMENT(holder), "hx-trigger", LATEST_POLL);
		htmx_element_set_attribute(HTMX_ELEMENT(holder), "hx-swap", "outerHTML");
	}
	ADD(holder, table);
	return HTMX_NODE(holder);
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

static void
latest_run(
	AxisWebConversions *self,
	gpointer            data
){
	ListWork *work = data;

	axis_conversions_list(self->conversions, LIST_LIMIT, &work->records, &work->error);
}

static HtmxResponse *
latest_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	ListWork *work = data;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	return fragment_response(conversions_table(work->records));
}

static HtmxNode *
revisions_card(
	HtmxRequest *request,
	GPtrArray   *revisions
){
	static const AxisWebColumn columns[] = {
		{ "Profile version", AXIS_WEB_COL_NAME },
		{ "State", AXIS_WEB_COL_STATE },
		{ "Passed trials", AXIS_WEB_COL_COUNT },
		{ "Trials needed", AXIS_WEB_COL_COUNT },
		{ "Next step", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Profile versions being proven");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	ADD(card, axis_web_storage_text("help", "A version is locked until it is proven: trials are allowed with it, "
		"replacing files is not. With enough trials on different files that passed every check (and none rejected "
		"by a check of the converted file), an administrator can prove it by hand, after playing the trial files."));
	if (revisions == NULL || revisions->len == 0) {
		ADD(card, axis_web_storage_text("help", "No trial has been made yet. Start one from a file's page: "
			"Media management, Files, a file, Convert this file."));
		return card;
	}
	table = axis_web_table_new("conversion-revisions", columns);
	body = htmx_tbody_new();
	for (i = 0; i < revisions->len; i++) {
		const AxisConversionTrialRevision *r = g_ptr_array_index(revisions, i);
		g_autofree gchar *label = profile_label(r->profile_name, r->revision_number);
		g_autofree gchar *passed = g_strdup_printf("%u", r->passed);
		g_autofree gchar *needed = g_strdup_printf("%u", r->needed);
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_text(label));
		ADD(row, cell_node(r->proven ? axis_web_browser_badge(g_strcmp0(r->proof_method, "trials") == 0 ?
			"Proven by hand" : "Proven", "badge-succeeded") : axis_web_browser_badge("Locked: trials only", "badge-warn")));
		ADD(row, cell_text(passed));
		ADD(row, cell_text(needed));
		if (!r->proven && r->passed >= r->needed && r->bad == 0) {
			g_autofree gchar *href = g_strdup_printf("/conversions/prove/%" G_GINT64_FORMAT, r->revision_id);
			HtmxA *a = htmx_a_new_with_href(href);

			htmx_element_add_class(HTMX_ELEMENT(a), "button");
			htmx_element_add_class(HTMX_ELEMENT(a), "button-small");
			htmx_node_set_text_content(HTMX_NODE(a), "Review and prove this version");
			ADD(row, cell_node(HTMX_NODE(a)));
		} else {
			/* What stands in its way, and what was set aside. */
			g_autofree gchar *bad = r->bad > 0 ? g_strdup_printf("%u trial%s rejected by a check of the converted file",
				r->bad, r->bad == 1 ? "" : "s") : NULL;
			g_autofree gchar *aside = r->set_aside > 0 ? g_strdup_printf("%u trial%s set aside", r->set_aside,
				r->set_aside == 1 ? "" : "s") : NULL;
			g_autofree gchar *text = g_strdup_printf("%s%s%s", bad != NULL ? bad : "", bad != NULL && aside != NULL ?
				"; " : "", aside != NULL ? aside : "");

			ADD(row, cell_text(text));
		}
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	(void)request;
	return card;
}

static HtmxNode *
leftovers_card(
	AxisWebConversions *self,
	GPtrArray          *leftovers
){
	static const AxisWebColumn columns[] = {
		{ "Left in the work area", AXIS_WEB_COL_NAME },
		{ "Disk", AXIS_WEB_COL_TEXT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "First seen", AXIS_WEB_COL_DATE },
		{ "Removed after", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Leftover files in work areas");
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	ADD(card, axis_web_storage_text("help", "Workers write only in .axis-work at the top of a disk. What is there that "
		"no conversion accounts for (a worker that died, an output that was not reported) is listed here and removed "
		"after the time set in Settings, \"Remove work-area leftovers after\"; a name that is not one of Axis's is never removed."));
	if (leftovers == NULL || leftovers->len == 0) {
		ADD(card, axis_web_storage_text("help", "Nothing is left in a work area."));
		return card;
	}
	table = axis_web_table_new("conversion-leftovers", columns);
	body = htmx_tbody_new();
	for (i = 0; i < leftovers->len; i++) {
		const AxisConversionLeftover *l = g_ptr_array_index(leftovers, i);
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_node(axis_web_browser_name_node(l->name)));
		ADD(row, cell_text(l->volume_name));
		ADD(row, cell_size(l->size));
		ADD(row, cell_time(l->first_seen_us));
		if (l->ours) {
			g_autofree gchar *wait = axis_web_storage_removal_wait_note(axis_conversions_get_storage(self->conversions),
				l->volume_id, l->remove_us);

			if (wait != NULL) {
				g_autofree gchar *when = axis_web_format_time(l->remove_us, "");
				g_autofree gchar *both = g_strdup_printf("%s. %s", when, wait);

				ADD(row, cell_text(both));
			} else {
				ADD(row, cell_time(l->remove_us));
			}
		} else
			ADD(row, cell_text("Never: not a name of Axis's"));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static HtmxResponse *
list_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	ListWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *notice = done_text(htmx_request_get_query_param(request, "done"));
	HtmxNode *card;
	AxisWebNotice placed;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, heading(1, "Conversions"));
	ADD(content, axis_web_page_intro("Every file conversion, asked for from a file's page or by a conversion run. "
		"A trial keeps the converted file aside so that you can play it; a replacement puts it in the original's "
		"place once this server has passed it through ten checks. To convert one file, open it under Media "
		"management, Files, and use Convert this file."));
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	{
		/* What holds conversions back, in one box that is always there: a
		 * resume's notice goes at its top, where the paused box was. */
		HtmxNode *state = HTMX_NODE(htmx_div_new());
		HtmxNode *warn = read_only_notice(self);
		HtmxNode *paused = axis_web_conversions_paused_notice(request, self->conversions);
		HtmxNode *unmarked = axis_web_storage_unmarked_notice_for(axis_conversions_get_storage(self->conversions));

		htmx_element_set_id(HTMX_ELEMENT(state), "conversions-state");
		axis_web_notice_put(&placed, state, "conversions-state");
		if (warn != NULL)
			ADD(state, warn);
		if (paused != NULL)
			ADD(state, paused);
		if (unmarked != NULL)
			ADD(state, unmarked);
		ADD(content, state);
	}
	{
		g_autofree gchar *where = g_strdup_printf("Converted files are checked: %s (Settings, \"Where converted files "
			"are checked\"). This server decides every check in every mode.",
			axis_conversion_checks_where_label(axis_conversions_checks_where(self->conversions)));

		ADD(content, axis_web_storage_text("help", where));
	}
	ADD(content, axis_web_runs_card());
	card = axis_web_storage_card("Latest conversions");
	if (work->records->len == 0) {
		HtmxP *none = htmx_p_new();
		HtmxSpan *lead = htmx_span_new();
		HtmxA *files_link = htmx_a_new_with_href("/library");
		HtmxSpan *tail = htmx_span_new();

		htmx_element_add_class(HTMX_ELEMENT(none), "help");
		htmx_node_set_text_content(HTMX_NODE(lead),
			"Nothing has been converted yet. To convert a file, open it under Library, ");
		htmx_node_set_text_content(HTMX_NODE(files_link), "Files");
		htmx_node_set_text_content(HTMX_NODE(tail), ", and use its Convert this file card.");
		ADD(none, lead);
		ADD(none, files_link);
		ADD(none, tail);
		ADD(card, none);
	} else
		ADD(card, conversions_table(work->records));
	ADD(content, card);
	ADD(content, revisions_card(request, work->revisions));
	ADD(content, leftovers_card(self, work->leftovers));
	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, "Conversions", HTMX_NODE(content));
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return dispatch(user_data, request, list_run, list_done, g_new0(ListWork, 1), list_work_free);
}

static HtmxResponse *
handle_latest(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return dispatch(user_data, request, latest_run, latest_done, g_new0(ListWork, 1), list_work_free);
}

/* ── One conversion ─────────────────────────────────────────────── */

typedef struct
{
	gint64                 id;
	gboolean               progress_only;
	AxisConversionRecord  *record;
	GPtrArray             *checks;
	GError                *error;
} OneWork;

static void
one_work_free(gpointer data)
{
	OneWork *work = data;

	axis_conversion_record_free(work->record);
	g_clear_pointer(&work->checks, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

static void
one_run(
	AxisWebConversions *self,
	gpointer            data
){
	OneWork *work = data;

	axis_conversions_get(self->conversions, work->id, &work->record, work->progress_only ? NULL : &work->checks,
		&work->error);
}

static HtmxNode *
checks_card(GPtrArray *checks)
{
	static const AxisWebColumn columns[] = {
		{ "Check", AXIS_WEB_COL_NAME },
		{ "Result", AXIS_WEB_COL_STATE },
		{ "What was found", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("The ten checks of the converted file");
	HtmxNode *table = axis_web_table_new("conversion-checks", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	ADD(card, axis_web_storage_text("help", "This server decides each check of the converted file, in this order, "
		"from its own measurements or, for checks 5 to 8 where a worker made them, from the worker's measurements "
		"once they are shown to be of these very files; the first that fails rejects the file, removes it, and leaves "
		"the original as it was."));
	for (i = 0; i < checks->len; i++) {
		const AxisConversionCheck *c = g_ptr_array_index(checks, i);
		g_autofree gchar *label = g_strdup_printf("%u. %s", c->number, axis_conversion_check_label(c->number));
		HtmxTr *row = htmx_tr_new();
		HtmxNode *badge = g_strcmp0(c->result, "passed") == 0 ? axis_web_browser_badge("Passed", "badge-succeeded") :
			g_strcmp0(c->result, "failed") == 0 ? axis_web_browser_badge("Failed", "badge-failed") :
			g_strcmp0(c->result, "skipped") == 0 ? axis_web_browser_badge("Not run", "badge-note") :
			axis_web_browser_badge("To come", "badge-note");

		ADD(row, cell_text(label));
		ADD(row, cell_node(badge));
		ADD(row, cell_text(c->detail != NULL ? c->detail : ""));
		ADD(body, row);
	}
	ADD(table, body);
	ADD(card, table);
	return card;
}

static const gchar *
step_text(const gchar *step)
{
	if (step == NULL)
		return "Not placed";
	if (strcmp(step, "placing") == 0)
		return "About to be placed";
	if (strcmp(step, "adopted") == 0)
		return "In the original's folder under a temporary name";
	if (strcmp(step, "linked") == 0)
		return "The original has its name in the recycle folder";
	if (strcmp(step, "swapped") == 0)
		return "The new file has the original's name";
	if (strcmp(step, "published") == 0)
		return "The new file has its new name";
	if (strcmp(step, "retired") == 0)
		return "The original is in the recycle folder";
	return "Recorded";
}

static HtmxResponse *
one_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	OneWork *work = data;
	const AxisConversionRecord *r = work->record;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *title = NULL;
	g_autofree gchar *profile = NULL;
	const gchar *notice = done_text(htmx_request_get_query_param(request, "done"));
	gboolean replace;
	HtmxNode *card;
	HtmxNode *facts;
	AxisWebNotice placed;

	if (g_error_matches(work->error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_NOT_FOUND))
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion.");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->progress_only)
		return fragment_response(progress_block(r, TRUE));
	replace = g_strcmp0(r->outcome, "replace") == 0;
	title = g_strdup_printf("Conversion %" G_GINT64_FORMAT, r->id);
	profile = profile_label(r->profile_name, r->revision_number);
	{
		HtmxP *crumbs = htmx_p_new();

		ADD(crumbs, axis_web_browser_link("/conversions", "Conversions"));
		ADD(content, crumbs);
	}
	ADD(content, heading(1, title));
	ADD(content, axis_web_page_intro(replace ? "A replacement: the converted file takes the original's place once "
		"it has passed every check, and the original goes into the recycle folder." :
		"A trial: the converted file is kept in the disk's work area to be played; the library is not touched."));
	axis_web_notice_begin(&placed, request, FALSE, g_strdup(notice), HTMX_NODE(content));
	{
		HtmxNode *paused = axis_web_conversions_paused_notice(request, self->conversions);

		if (paused != NULL)
			ADD(content, paused);
	}

	card = axis_web_storage_card("Request, state and sizes");
	htmx_element_set_id(HTMX_ELEMENT(card), "conversion-asked");
	axis_web_notice_put(&placed, card, "conversion-asked");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "File", file_name(r->file_id, r->rel_path, TRUE));
	if (g_strcmp0(r->new_rel_path, r->rel_path) != 0 && replace)
		axis_web_browser_fact_node(facts, "New name", file_name(0, r->new_rel_path, FALSE));
	if (r->root_name != NULL)
		axis_web_browser_fact(facts, "Library folder", r->root_name);
	axis_web_browser_fact(facts, "Conversion profile", profile);
	axis_web_browser_fact(facts, "Trial or replacement", replace ? "Replace the original" : "Trial");
	if (r->convert_kept)
		axis_web_browser_fact(facts, "Files Axis keeps", "Asked for with the choice to convert kept files: if the "
			"file is kept, it is converted in place and stays kept.");
	axis_web_browser_fact_node(facts, "State", state_badge(r->state));
	if (r->result_text != NULL)
		axis_web_browser_fact(facts, "Result", r->result_text);
	{
		g_autofree gchar *asked = axis_web_format_time(r->requested_us, "");
		g_autofree gchar *who = g_strdup_printf("%s, %s", r->requested_by, asked);

		axis_web_browser_fact(facts, "Asked by", who);
	}
	if (r->run_id > 0) {
		g_autofree gchar *href = g_strdup_printf("/conversions/runs/%" G_GINT64_FORMAT, r->run_id);
		g_autofree gchar *text = g_strdup_printf("Conversion run %" G_GINT64_FORMAT, r->run_id);

		axis_web_browser_fact_node(facts, "Part of", axis_web_browser_link(href, text));
	}
	axis_web_browser_fact(facts, "Playback verdict when asked", g_strcmp0(r->verdict, "safe") == 0 ? "Safe" :
		g_strcmp0(r->verdict, "risky") == 0 ? "Risky" : "Unknown");
	if (r->source_quality != NULL)
		axis_web_browser_fact(facts, "Quality of the original", r->source_quality);
	{
		g_autofree gchar *before = axis_size_format((guint64)MAX(r->src_size, 0));

		axis_web_browser_fact(facts, "Size before", before);
	}
	if (r->estimate_bytes >= 0) {
		g_autofree gchar *estimate = axis_size_format((guint64)r->estimate_bytes);

		axis_web_browser_fact(facts, "Estimated size after", estimate);
	}
	if (r->output_size > 0) {
		g_autofree gchar *after = axis_size_format((guint64)r->output_size);
		g_autofree gchar *text = r->src_size > 0 ? g_strdup_printf("%s (%.1f%% smaller)", after,
			100.0 * (gdouble)(r->src_size - r->output_size) / (gdouble)r->src_size) : g_strdup(after);

		axis_web_browser_fact(facts, "Size after", text);
	}
	if (r->worker != NULL) {
		g_autofree gchar *text = g_strdup_printf("%s, with %s (attempt %d)", r->worker,
			r->encoder != NULL ? r->encoder : "?", r->attempt);

		axis_web_browser_fact(facts, "Converted on", text);
	}
	if (r->job_state != NULL && (g_strcmp0(r->state, "queued") == 0 || g_strcmp0(r->state, "encoding") == 0)) {
		g_autofree gchar *state_word = g_strdup(r->job_state);
		g_autofree gchar *text = NULL;

		if (state_word[0] != '\0')
			state_word[0] = g_ascii_toupper(state_word[0]);
		text = g_strdup_printf("%s%s%s (%.0f%%)", state_word, r->job_message != NULL ? ": " : "",
			r->job_message != NULL ? r->job_message : "", r->job_progress * 100.0);
		axis_web_browser_fact(facts, "Conversion job", text);
	}
	{
		g_autofree gchar *text = checked_on(r);

		axis_web_browser_fact(facts, "Checked on", text);
	}
	axis_web_browser_fact(facts, "Where checks run now",
		axis_conversion_checks_where_label(axis_conversions_checks_where(self->conversions)));
	if (g_strcmp0(r->state, "verifying") == 0) {
		g_autofree gchar *text = NULL;

		if (r->checks_waiting != NULL)
			text = g_strdup(r->checks_waiting);
		else if (g_strcmp0(r->verify_state, "running") == 0)
			text = g_strdup_printf("Being measured on %s (%.0f%%)%s%s", r->verify_holder != NULL &&
				g_str_has_prefix(r->verify_holder, "worker:") ? r->verify_holder + strlen("worker:") : "a worker",
				r->verify_progress * 100.0, r->verify_message != NULL ? ": " : "",
				r->verify_message != NULL ? r->verify_message : "");
		else if (g_strcmp0(r->checked_where, "worker") == 0 && r->verifier != NULL)
			text = g_strdup_printf("Measured on worker %s; being judged by this server", r->verifier);
		else
			text = g_strdup("Being checked on this server");
		axis_web_browser_fact(facts, "Checks in progress", text);
	}
	ADD(card, facts);
	/* How far it is, followed while it is in flight. */
	ADD(card, progress_block(r, FALSE));
	if (g_strcmp0(r->state, "queued") == 0 || g_strcmp0(r->state, "encoding") == 0) {
		g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/cancel", r->id);

		ADD(card, small_post(request, href, "Cancel the conversion", "button-danger"));
	}
	ADD(content, card);
	/* A trial rejected by a check of the converted file: whether it counts
	 * against proving its version, and the way to set it aside. */
	if (!replace && g_strcmp0(r->state, "rejected") == 0 && r->failed_check >= 3 && r->failed_check <= 8) {
		HtmxNode *aside = axis_web_storage_card("Whether this trial counts toward proving");
		HtmxNode *list = axis_web_browser_facts_new();

		htmx_element_set_id(HTMX_ELEMENT(aside), "conversion-aside");
		axis_web_notice_put(&placed, aside, "conversion-aside");
		if (r->set_aside_us > 0) {
			g_autofree gchar *when = axis_web_format_time(r->set_aside_us, "");
			g_autofree gchar *text = g_strdup_printf("By %s, %s: %s", r->set_aside_by != NULL ? r->set_aside_by : "?", when,
				r->set_aside_reason != NULL ? r->set_aside_reason : "");

			axis_web_browser_fact(list, "Set aside", text);
			axis_web_browser_fact(list, "Counts toward proving", "Neither for nor against proving the profile version.");
			ADD(aside, list);
		} else if (r->cleared_by != NULL) {
			g_autofree gchar *text = g_strdup_printf("Neither for nor against proving the profile version: check %u was found "
				"wrongly strict for conversions judged before check rules version %d, and this one was judged by "
				"check rules version %d. Why: %s. What the later checks would have said of the file is not known.", r->cleared_by->check,
				r->cleared_by->before, r->checks_version, r->cleared_by->why != NULL ? r->cleared_by->why : "?");

			axis_web_browser_fact(list, "Counts toward proving", text);
			ADD(aside, list);
		} else {
			g_autofree gchar *action = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/set-aside", r->id);
			HtmxNode *form = axis_web_auth_post_form(request, action);

			axis_web_browser_fact(list, "Counts toward proving", "Against proving the profile version: while it does, "
				"the version cannot be proven by hand, and a conversion run of trials with it pauses.");
			ADD(aside, list);
			ADD(aside, axis_web_storage_text("help", "When the rejection was not the profile's fault (a check found wrong "
				"and fixed since, for example), an administrator can set this trial aside. It then counts neither for "
				"nor against proving the version, and this page keeps saying that it was set aside, by whom and why. "
				"This is recorded with your name."));
			ADD(form, axis_web_storage_text_field("set-aside-reason", "Why this trial is set aside", "reason", NULL,
				"for example: check 4 was wrong for untagged sources, fixed since", "Required; kept with the trial."));
			ADD(form, axis_web_auth_button("Set this trial aside", NULL));
			ADD(aside, form);
		}
		ADD(content, aside);
	}
	/* A trial that passed by a rule found too lenient since. */
	if (!replace && r->voided_by != NULL) {
		HtmxNode *aside = axis_web_storage_card("Whether this trial counts toward proving");
		HtmxNode *list = axis_web_browser_facts_new();
		g_autofree gchar *text = g_strdup_printf("Not towards proving the profile version: check %u was found too "
			"lenient for conversions judged before check rules version %d, and this one was judged by check rules "
			"version %d. Why: %s. "
			"A new trial of the file is judged by the rules of now.", r->voided_by->check, r->voided_by->before,
			r->checks_version, r->voided_by->why != NULL ? r->voided_by->why : "?");

		axis_web_browser_fact(list, "Counts toward proving", text);
		ADD(aside, list);
		ADD(content, aside);
	}
	ADD(content, checks_card(work->checks));

	if (replace) {
		HtmxNode *placement = axis_web_storage_card("Putting the new file in place");
		HtmxNode *list = axis_web_browser_facts_new();

		ADD(placement, axis_web_storage_text("help", "Each step is recorded before or after it is taken, so that "
			"if this server stops at any point, it finishes or undoes the step from these records."));
		axis_web_browser_fact(list, "Step reached", step_text(r->step));
		if (r->recycle_path != NULL) {
			HtmxNode *where = axis_web_browser_name_node(r->recycle_path);

			axis_web_browser_fact_node(list, "Original in the recycle folder", where);
			axis_web_browser_fact(list, "Original", g_strcmp0(r->recycle_state, "kept") == 0 ? "Kept until the date below" :
				g_strcmp0(r->recycle_state, "removed") == 0 ? "Removed" : g_strcmp0(r->recycle_state, "gone") == 0 ?
				"Gone (not where it was recorded)" : g_strcmp0(r->recycle_state, "changed") == 0 ?
				"Left alone (something else is there)" : "Not recorded yet");
			if (g_strcmp0(r->recycle_state, "kept") == 0) {
				g_autofree gchar *when = axis_web_format_time(r->recycle_remove_us, "");

				g_autofree gchar *wait = axis_web_storage_removal_wait_note(
					axis_conversions_get_storage(self->conversions), r->volume_id, r->recycle_remove_us);
				g_autofree gchar *both = wait != NULL ? g_strdup_printf("%s. %s", when, wait) : g_strdup(when);

				axis_web_browser_fact(list, "Removed after", both);
			}
		}
		ADD(placement, list);
		ADD(content, placement);
	} else if (r->trial_name != NULL) {
		HtmxNode *trial = axis_web_storage_card("The trial's converted file");
		HtmxNode *list = axis_web_browser_facts_new();
		g_autofree gchar *where = g_strdup_printf("%s/%s", ".axis-work", r->trial_name);

		htmx_element_set_id(HTMX_ELEMENT(trial), "conversion-trial");
		axis_web_notice_put(&placed, trial, "conversion-trial");
		ADD(trial, axis_web_storage_text("help", "Play it on the household's players: proving the version by hand "
			"rests on that."));
		axis_web_browser_fact(list, "In the work area", where);
		axis_web_browser_fact(list, "Kept", g_strcmp0(r->trial_state, "kept") == 0 ? "Yes" :
			g_strcmp0(r->trial_state, "removed") == 0 ? "Removed" : "No");
		if (g_strcmp0(r->trial_state, "kept") == 0) {
			g_autofree gchar *when = axis_web_format_time(r->trial_remove_us, "");
			g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "/remove-trial", r->id);

			axis_web_browser_fact(list, "Removed after", when);
			ADD(trial, list);
			ADD(trial, small_post(request, href, "Remove the trial file now", NULL));
		} else {
			ADD(trial, list);
		}
		ADD(content, trial);
	}
	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static HtmxResponse *
handle_one(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	OneWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion.");
	work = g_new0(OneWork, 1);
	work->id = id;
	return dispatch(user_data, request, one_run, one_done, work, one_work_free);
}

static HtmxResponse *
handle_progress(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	OneWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion.");
	work = g_new0(OneWork, 1);
	work->id = id;
	work->progress_only = TRUE;
	return dispatch(user_data, request, one_run, one_done, work, one_work_free);
}

/* ── Actions on one ─────────────────────────────────────────────── */

typedef struct
{
	gint64   id;
	gboolean trial;
	gchar   *user;
	GError  *error;
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
	AxisWebConversions *self,
	gpointer            data
){
	ActWork *work = data;

	if (work->trial)
		axis_conversions_remove_trial(self->conversions, work->id, work->user, &work->error);
	else
		axis_conversions_cancel(self->conversions, work->id, work->user, &work->error);
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
act_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	ActWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL)
		return error_page(request, work->error);
	axis_web_storage_audit(request, "conversion %" G_GINT64_FORMAT " %s", work->id, work->trial ? "trial removed" :
		"cancelled");
	location = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, work->trial ? "trial-removed" : "cancelled",
		work->trial ? "conversion-trial" : "conversion-asked");
}

static HtmxResponse *
act(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	gboolean     trial
){
	ActWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion.");
	work = g_new0(ActWork, 1);
	work->id = id;
	work->trial = trial;
	work->user = g_strdup(axis_web_request_get_username(request));
	return dispatch(user_data, request, act_run, act_done, work, act_work_free);
}

static HtmxResponse *
handle_cancel(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return act(request, params, user_data, FALSE);
}

static HtmxResponse *
handle_remove_trial(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return act(request, params, user_data, TRUE);
}

/* ── Setting a rejected trial aside ─────────────────────────────── */

typedef struct
{
	gint64  id;
	gchar  *user;
	gchar  *reason;
	GError *error;
} AsideWork;

static void
aside_work_free(gpointer data)
{
	AsideWork *work = data;

	g_free(work->user);
	g_free(work->reason);
	g_clear_error(&work->error);
	g_free(work);
}

static void
aside_run(
	AxisWebConversions *self,
	gpointer            data
){
	AsideWork *work = data;

	axis_conversions_set_aside(self->conversions, work->id, work->user, work->reason, &work->error);
}

static HtmxResponse *
aside_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	AsideWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL)
		return error_page(request, work->error);
	axis_web_storage_audit(request, "conversion %" G_GINT64_FORMAT " set aside as a trial", work->id);
	location = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, work->id);
	return axis_web_place_redirect(request, location, "set-aside", "conversion-aside");
}

static HtmxResponse *
handle_set_aside(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AsideWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such conversion.");
	work = g_new0(AsideWork, 1);
	work->id = id;
	work->user = g_strdup(axis_web_request_get_username(request));
	work->reason = g_strdup(htmx_request_get_form_value(request, "reason"));
	return dispatch(user_data, request, aside_run, aside_done, work, aside_work_free);
}

/* ── Resuming conversion work ───────────────────────────────────── */

typedef struct
{
	gchar    *user;
	gboolean  was_held;
	GError   *error;
} ResumeWork;

static void
resume_work_free(gpointer data)
{
	ResumeWork *work = data;

	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
resume_run(
	AxisWebConversions *self,
	gpointer            data
){
	ResumeWork *work = data;

	axis_conversions_resume(self->conversions, work->user, &work->was_held, &work->error);
}

static HtmxResponse *
resume_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	ResumeWork *work = data;

	if (work->error != NULL)
		return error_page(request, work->error);
	if (work->was_held)
		axis_web_storage_audit(request, "conversion work resumed");
	/* The paused box goes with the resume: the notice takes its place. */
	return axis_web_place_redirect(request, "/conversions", work->was_held ? "resumed" : "not-held",
		"conversions-state");
}

static HtmxResponse *
handle_resume(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ResumeWork *work = g_new0(ResumeWork, 1);

	work->user = g_strdup(axis_web_request_get_username(request));
	return dispatch(user_data, request, resume_run, resume_done, work, resume_work_free);
}

/* ── The confirmation and the request ───────────────────────────── */

static gboolean
query_id(
	HtmxRequest *request,
	const gchar *name,
	gboolean     form,
	gint64      *value
){
	const gchar *text = form ? htmx_request_get_form_value(request, name) : htmx_request_get_query_param(request, name);
	guint64 n = 0;

	*value = 0;
	if (text == NULL || !g_ascii_string_to_unsigned(text, 10, 1, (guint64)G_MAXINT64, &n, NULL))
		return FALSE;
	*value = (gint64)n;
	return TRUE;
}

typedef struct
{
	gint64                 file_id;
	gint64                 revision_id;
	AxisConversionOutcome  outcome;
	gboolean               confirmed;
	gboolean               convert_kept;	/* the tick "Also convert files Axis keeps" */
	gchar                 *user;
	AxisConversionPreview *preview;
	gint64                 conversion_id;
	GError                *error;
} AskWork;

static void
ask_work_free(gpointer data)
{
	AskWork *work = data;

	axis_conversion_preview_free(work->preview);
	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
preview_run(
	AxisWebConversions *self,
	gpointer            data
){
	AskWork *work = data;

	axis_conversions_preview(self->conversions, work->file_id, work->revision_id, work->outcome, &work->preview,
		&work->error);
}

static HtmxNode *
gates_table(const AxisConversionPreview *preview)
{
	static const AxisWebColumn columns[] = {
		{ "What must hold", AXIS_WEB_COL_NAME },
		{ "Now", AXIS_WEB_COL_STATE },
		{ "What was found", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *table = axis_web_table_new("conversion-gates", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < preview->gates->len; i++) {
		const AxisConversionGate *gate = g_ptr_array_index(preview->gates, i);
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_text(gate->label));
		ADD(row, cell_node(!gate->applies ? axis_web_browser_badge("Not needed", "badge-note") : gate->passed ?
			axis_web_browser_badge("Holds", "badge-succeeded") : axis_web_browser_badge("Refuses", "badge-failed")));
		ADD(row, cell_text(gate->detail != NULL ? gate->detail : ""));
		ADD(body, row);
	}
	ADD(table, body);
	return table;
}

gchar *
axis_web_conversions_keep_sentence(gint64 seconds)
{
	if (seconds <= 0)
		return g_strdup("The original will be deleted after the new file has passed every check; it cannot be "
			"recovered.");
	if (seconds % 86400 == 0)
		return g_strdup_printf("The original will be deleted after the new file has passed every check; it is kept in "
			"the disk's recycle folder for %" G_GINT64_FORMAT " day%s first (Settings, \"Keep replaced originals for\"), "
			"then it cannot be recovered.", seconds / 86400, seconds / 86400 == 1 ? "" : "s");
	return g_strdup_printf("The original will be deleted after the new file has passed every check; it is kept in the "
		"disk's recycle folder for %" G_GINT64_FORMAT " hours first (Settings, \"Keep replaced originals for\"), then "
		"it cannot be recovered.", MAX(seconds / 3600, 1));
}

static HtmxResponse *
preview_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	AskWork *work = data;
	const AxisConversionPreview *p = work->preview;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const AxisConversionGate *refusal;
	gboolean replace = work->outcome == AXIS_CONVERSION_REPLACE;
	gboolean kept_choice = FALSE;
	HtmxNode *card;
	HtmxNode *facts;

	if (work->error != NULL)
		return error_page(request, work->error);
	{
		HtmxP *crumbs = htmx_p_new();

		ADD(crumbs, axis_web_browser_link("/conversions", "Conversions"));
		ADD(content, crumbs);
		ADD(content, heading(1, replace ? "Replace a file with its conversion" : "Try a conversion"));
		ADD(content, axis_web_page_intro(replace ? "What will happen, and every condition worked out now. Nothing "
			"happens until you confirm, and each condition is checked again when a worker takes it and just before "
			"the original is replaced." : "What will happen, and every condition worked out now. A trial keeps the "
			"converted file in the disk's work area; the library is not touched."));
	}
	{
		HtmxNode *warn = read_only_notice(self);

		if (warn != NULL)
			ADD(content, warn);
	}

	card = axis_web_storage_card("What will happen");
	facts = axis_web_browser_facts_new();
	axis_web_browser_fact_node(facts, "File", file_name(p->file_id, p->rel_path, TRUE));
	{
		g_autofree gchar *size = axis_size_format((guint64)MAX(p->size, 0));

		axis_web_browser_fact(facts, "Size", size);
	}
	if (p->root_name != NULL)
		axis_web_browser_fact(facts, "Library folder", p->root_name);
	if (p->revision != NULL) {
		g_autofree gchar *label = profile_label(p->revision->name, p->revision->number);

		axis_web_browser_fact(facts, "Conversion profile", label);
		axis_web_browser_fact(facts, "Lock", axis_profile_revision_is_unlocked(p->revision) ? "Proven" :
			"Locked: trials only");
	}
	axis_web_browser_fact(facts, "Trial or replacement", replace ? "Replace the original" : "Trial: keep the converted file aside");
	if (p->estimated) {
		g_autofree gchar *mid = axis_size_format((guint64)p->estimate_mid);
		g_autofree gchar *low = axis_size_format((guint64)p->estimate_low);
		g_autofree gchar *high = axis_size_format((guint64)p->estimate_high);
		g_autofree gchar *text = g_strdup_printf("about %s (%s to %s)", mid, low, high);

		axis_web_browser_fact(facts, "Estimated size after", text);
	}
	axis_web_browser_fact(facts, "Playback verdict", p->verdict == AXIS_VERDICT_SAFE ? "Safe" :
		p->verdict == AXIS_VERDICT_RISKY ? "Risky" : "Unknown");
	if (p->encoder != NULL)
		axis_web_browser_fact(facts, "Encoder", p->encoder);
	if (replace && p->new_rel_path != NULL && g_strcmp0(p->new_rel_path, p->rel_path) != 0)
		axis_web_browser_fact_node(facts, "New name", file_name(0, p->new_rel_path, FALSE));
	ADD(card, facts);
	ADD(content, card);

	card = axis_web_storage_card("Conditions for this conversion, checked now");
	ADD(card, gates_table(p));
	ADD(content, card);

	refusal = axis_conversion_preview_refusal(p);
	/* A kept file that nothing else holds back: the administrator may choose
	 * to convert it, with a tick of its own (convert-kept-files).  The
	 * request works every condition out again with the choice; without the
	 * tick it is refused as before. */
	if (refusal != NULL && replace && p->kept) {
		gboolean other = FALSE;
		guint i;

		for (i = 0; i < p->gates->len; i++) {
			const AxisConversionGate *gate = g_ptr_array_index(p->gates, i);

			if (gate->applies && !gate->passed && g_strcmp0(gate->id, "kept") != 0)
				other = TRUE;
		}
		kept_choice = !other;
	}
	card = axis_web_storage_card(replace ? "Confirm the replacement" : "Start the trial");
	if (refusal != NULL && !kept_choice) {
		g_autofree gchar *text = g_strdup_printf("This conversion cannot start now: %s. %s", refusal->label,
			refusal->detail != NULL ? refusal->detail : "");

		ADD(card, axis_web_storage_text("notice notice-error", text));
	} else {
		HtmxNode *form = axis_web_auth_post_form(request, "/conversions");
		g_autofree gchar *file = g_strdup_printf("%" G_GINT64_FORMAT, p->file_id);
		g_autofree gchar *revision = g_strdup_printf("%" G_GINT64_FORMAT, work->revision_id);

		ADD(form, axis_web_auth_hidden("file", file));
		ADD(form, axis_web_auth_hidden("revision", revision));
		ADD(form, axis_web_auth_hidden("outcome", replace ? "replace" : "trial"));
		if (replace) {
			g_autofree gchar *sentence = axis_web_conversions_keep_sentence(p->keep_seconds);

			if (kept_choice) {
				ADD(form, axis_web_storage_text("notice notice-warning", "Axis keeps this file: it does not replace it "
					"with another release, and a conversion leaves it alone unless you choose otherwise here."));
				ADD(form, axis_web_storage_checkbox("conversion-convert-kept", AXIS_WEB_CONVERT_KEPT_LABEL, "convert_kept",
					FALSE));
				ADD(form, axis_web_storage_text("help", AXIS_WEB_CONVERT_KEPT_HELP));
			}
			ADD(form, axis_web_storage_text("notice notice-warning", sentence));
			ADD(form, axis_web_storage_checkbox("conversion-confirm", "I understand: the original will be deleted",
				"confirm", TRUE));
			ADD(form, axis_web_auth_button("Replace the original", "button-danger"));
		} else {
			ADD(form, axis_web_storage_text("help", "The converted file is kept in .axis-work at the top of the disk "
				"for the time set in Settings, \"Keep trial files for\", or until you remove it from the conversion's page."));
			ADD(form, axis_web_auth_button("Start the trial", "button-primary"));
		}
		ADD(card, form);
	}
	ADD(content, card);
	return axis_web_page_respond(request, replace ? "Replace a file with its conversion" : "Try a conversion", HTMX_NODE(content));
}

static HtmxResponse *
handle_new(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AskWork *work = g_new0(AskWork, 1);

	if (!query_id(request, "file", FALSE, &work->file_id) || !query_id(request, "revision", FALSE, &work->revision_id) ||
	    !axis_conversion_outcome_from_string(htmx_request_get_query_param(request, "outcome"), &work->outcome)) {
		ask_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "Choose a file, a profile and an outcome.");
	}
	return dispatch(user_data, request, preview_run, preview_done, work, ask_work_free);
}

static void
request_run(
	AxisWebConversions *self,
	gpointer            data
){
	AskWork *work = data;

	axis_conversions_request_full(self->conversions, work->file_id, work->revision_id, work->outcome, work->confirmed,
		work->convert_kept, work->user, &work->conversion_id, &work->error);
}

static HtmxResponse *
request_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	AskWork *work = data;
	g_autofree gchar *location = NULL;

	if (work->error != NULL)
		return error_page(request, work->error);
	axis_web_storage_audit(request, "conversion %" G_GINT64_FORMAT " requested (%s%s)", work->conversion_id,
		axis_conversion_outcome_to_string(work->outcome), work->convert_kept ? ", also converting a file Axis keeps" : "");
	location = g_strdup_printf("/conversions/%" G_GINT64_FORMAT "?done=requested", work->conversion_id);
	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
handle_request(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AskWork *work = g_new0(AskWork, 1);

	if (!query_id(request, "file", TRUE, &work->file_id) || !query_id(request, "revision", TRUE, &work->revision_id) ||
	    !axis_conversion_outcome_from_string(htmx_request_get_form_value(request, "outcome"), &work->outcome)) {
		ask_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "Choose a file, a profile and an outcome.");
	}
	work->confirmed = axis_web_storage_form_yes(request, "confirm");
	if (work->outcome == AXIS_CONVERSION_REPLACE && !work->confirmed) {
		ask_work_free(work);
		return axis_web_error_respond(request, 400, "Not done", "A replacement deletes the original: tick the box to "
			"confirm.");
	}
	/* Only an explicit tick: anything else is no choice. */
	work->convert_kept = axis_web_storage_form_yes(request, "convert_kept");
	work->user = g_strdup(axis_web_request_get_username(request));
	return dispatch(user_data, request, request_run, request_done, work, ask_work_free);
}

/* ── Proving by hand ────────────────────────────────────────────── */

typedef struct
{
	gint64                 revision_id;
	gchar                 *trials;	/* comma-separated ids, from the form */
	gboolean               confirmed;
	gchar                 *user;
	AxisConversionProving *proving;
	GError                *error;
} ProveWork;

static void
prove_work_free(gpointer data)
{
	ProveWork *work = data;

	axis_conversion_proving_free(work->proving);
	g_free(work->trials);
	g_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

static void
proving_run(
	AxisWebConversions *self,
	gpointer            data
){
	ProveWork *work = data;

	axis_conversions_proving(self->conversions, work->revision_id, &work->proving, &work->error);
}

static HtmxResponse *
proving_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	static const AxisWebColumn columns[] = {
		{ "Trial", AXIS_WEB_COL_NAME },
		{ "Before", AXIS_WEB_COL_SIZE },
		{ "After", AXIS_WEB_COL_SIZE },
		{ "Done", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	ProveWork *work = data;
	const AxisConversionProving *p = work->proving;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *label = NULL;
	GString *ids = g_string_new(NULL);
	HtmxNode *card;
	HtmxNode *table;
	HtmxTbody *body;
	guint i;

	if (work->error != NULL) {
		g_string_free(ids, TRUE);
		return error_page(request, work->error);
	}
	label = profile_label(p->revision->name, p->revision->number);
	{
		HtmxP *crumbs = htmx_p_new();

		ADD(crumbs, axis_web_browser_link("/conversions", "Conversions"));
		ADD(content, crumbs);
	}
	ADD(content, heading(1, "Prove a conversion profile version by hand"));
	ADD(content, axis_web_page_intro("Proving a version lets it replace files: their originals are deleted once the "
		"converted file has passed every check. It rests on trials you have played on the household's players."));
	card = axis_web_storage_card(label);
	table = axis_web_table_new("conversion-trials", columns);
	body = htmx_tbody_new();
	for (i = 0; i < p->trials->len; i++) {
		const AxisConversionRecord *r = g_ptr_array_index(p->trials, i);
		HtmxTr *row = htmx_tr_new();

		g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, i > 0 ? "," : "", r->id);
		ADD(row, cell_node(file_name(r->file_id, r->rel_path, FALSE)));
		ADD(row, cell_size(r->src_size));
		ADD(row, cell_size(r->output_size));
		ADD(row, cell_time(r->finished_us));
		ADD(body, row);
	}
	ADD(table, body);
	if (p->trials->len > 0) {
		ADD(card, axis_web_storage_text("help", "The passed trials of this version, one per file:"));
		ADD(card, table);
	} else {
		/* The body is the table's already. */
		g_object_unref(table);
		ADD(card, axis_web_storage_text("help", "This version has no passed trial yet. Start one from a file's "
			"page: Media management, Files, a file, Convert this file."));
	}
	if (p->set_aside > 0) {
		g_autofree gchar *text = g_strdup_printf("%u rejected trial%s of this version %s set aside by an administrator: "
			"%s neither for nor against proving it (%s page%s say%s who did it and why).", p->set_aside,
			p->set_aside == 1 ? "" : "s", p->set_aside == 1 ? "was" : "were", p->set_aside == 1 ? "it counts" :
			"they count", p->set_aside == 1 ? "its" : "their", p->set_aside == 1 ? "" : "s", p->set_aside == 1 ? "s" : "");

		ADD(card, axis_web_storage_text("help", text));
	}
	if (!p->allowed) {
		ADD(card, axis_web_storage_text("notice notice-error", p->why_not));
	} else {
		g_autofree gchar *action = g_strdup_printf("/conversions/prove/%" G_GINT64_FORMAT, work->revision_id);
		g_autofree gchar *sentence = p->trials->len == 1 ?
			g_strdup_printf("I played this trial file on the household's players and it played correctly: %s may "
				"replace files, deleting their originals", label) :
			g_strdup_printf("I played these %u trial files on the household's players and they played correctly: %s "
				"may replace files, deleting their originals", p->trials->len, label);
		HtmxNode *form = axis_web_auth_post_form(request, action);

		ADD(form, axis_web_auth_hidden("trials", ids->str));
		ADD(form, axis_web_storage_text("notice notice-warning", "This is recorded with your name, the time and the "
			"trials it rests on. Nothing proves a version by itself; a replacement still needs a safe playback "
			"verdict for the file's library folder."));
		ADD(form, axis_web_storage_checkbox("prove-confirm", sentence, "confirm", TRUE));
		ADD(form, axis_web_auth_button("Prove this version", "button-danger"));
		ADD(card, form);
	}
	ADD(content, card);
	g_string_free(ids, TRUE);
	return axis_web_page_respond(request, "Prove a conversion profile version by hand", HTMX_NODE(content));
}

static HtmxResponse *
handle_proving(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ProveWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such profile version.");
	work = g_new0(ProveWork, 1);
	work->revision_id = id;
	return dispatch(user_data, request, proving_run, proving_done, work, prove_work_free);
}

static void
prove_run(
	AxisWebConversions *self,
	gpointer            data
){
	ProveWork *work = data;
	g_auto(GStrv) parts = g_strsplit(work->trials != NULL ? work->trials : "", ",", 101);
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	guint i;

	for (i = 0; parts[i] != NULL; i++) {
		guint64 n = 0;

		if (!g_ascii_string_to_unsigned(parts[i], 10, 1, (guint64)G_MAXINT64, &n, NULL)) {
			g_set_error_literal(&work->error, AXIS_CONVERSION_ERROR, AXIS_CONVERSION_ERROR_REFUSED,
				"The trials named are not readable; look at the confirmation again.");
			return;
		}
		g_array_append_val(ids, n);
	}
	axis_conversions_prove(self->conversions, work->revision_id, (const gint64 *)(gpointer)ids->data, ids->len,
		work->user, &work->error);
}

static HtmxResponse *
prove_done(
	AxisWebConversions *self,
	HtmxRequest        *request,
	gpointer            data
){
	ProveWork *work = data;

	if (work->error != NULL)
		return error_page(request, work->error);
	axis_web_storage_audit(request, "profile revision %" G_GINT64_FORMAT " proven by hand", work->revision_id);
	return axis_web_auth_redirect(request, "/conversions?done=proven");
}

static HtmxResponse *
handle_prove(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ProveWork *work;
	gint64 id = axis_web_storage_param_id(params);

	if (id <= 0)
		return axis_web_error_respond(request, 404, "Not found", "There is no such profile version.");
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not done", "Proving needs the confirmation ticked.");
	work = g_new0(ProveWork, 1);
	work->revision_id = id;
	work->trials = g_strdup(htmx_request_get_form_value(request, "trials"));
	work->user = g_strdup(axis_web_request_get_username(request));
	return dispatch(user_data, request, prove_run, prove_done, work, prove_work_free);
}

/* ── The file page's card ───────────────────────────────────────── */

typedef struct
{
	gboolean   video;
	GPtrArray *revisions;	/* AxisProfileRevision */
	GPtrArray *records;	/* AxisConversionRecord */
	gboolean   failed;
} FileFigures;

void
axis_web_conversions_file_free(gpointer data)
{
	FileFigures *figures = data;

	if (figures == NULL)
		return;
	g_clear_pointer(&figures->revisions, g_ptr_array_unref);
	g_clear_pointer(&figures->records, g_ptr_array_unref);
	g_free(figures);
}

gpointer
axis_web_conversions_file_collect(
	gint64       file_id,
	const gchar *kind,
	gpointer     user_data
){
	AxisWebConversions *self = user_data;
	FileFigures *figures = g_new0(FileFigures, 1);
	g_autoptr(GError) error = NULL;

	figures->video = g_strcmp0(kind, "video") == 0 && g_atomic_int_get(&self->enabled);
	if (!figures->video)
		return figures;
	if (!axis_conversions_offer(self->conversions, &figures->revisions, &error) ||
	    !axis_conversions_for_file(self->conversions, file_id, &figures->records, &error)) {
		g_debug("file page: the conversions cannot be read: %s", error->message);
		figures->failed = TRUE;
	}
	return figures;
}

HtmxNode *
axis_web_conversions_file_render(
	HtmxRequest *request,
	gint64       file_id,
	gpointer     data,
	gpointer     user_data
){
	FileFigures *figures = data;
	HtmxNode *card;
	guint i;

	if (figures == NULL || !figures->video || !axis_web_request_is_admin(request))
		return NULL;
	card = axis_web_storage_card("Convert this file");
	if (figures->failed) {
		ADD(card, axis_web_storage_text("help", "The conversions of this file cannot be read just now. "
			"Reload the page in a moment."));
		return card;
	}
	ADD(card, axis_web_storage_text("help", "A trial converts it and keeps the result aside to be played. A "
		"replacement puts the converted file in its place, once this server has checked it, and deletes the original; "
		"it needs a proven profile and a safe playback verdict. The next page shows exactly what will happen."));
	if (figures->revisions != NULL && figures->revisions->len > 0) {
		HtmxForm *form = htmx_form_new();
		HtmxDiv *field = htmx_div_new();
		HtmxLabel *label = htmx_label_new_for("convert-revision");
		HtmxSelect *select = htmx_select_new_with_name("revision");
		HtmxDiv *outcome = htmx_div_new();
		HtmxLabel *outcome_label = htmx_label_new_for("convert-outcome");
		HtmxSelect *outcomes = htmx_select_new_with_name("outcome");
		g_autofree gchar *file = g_strdup_printf("%" G_GINT64_FORMAT, file_id);

		htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/conversions/new");
		htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
		htmx_element_add_class(HTMX_ELEMENT(form), "film-form");
		ADD(form, axis_web_auth_hidden("file", file));
		htmx_element_add_class(HTMX_ELEMENT(field), "field");
		htmx_node_set_text_content(HTMX_NODE(label), "Conversion profile");
		htmx_element_set_id(HTMX_ELEMENT(select), "convert-revision");
		for (i = 0; i < figures->revisions->len; i++) {
			const AxisProfileRevision *r = g_ptr_array_index(figures->revisions, i);
			g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, r->id);
			g_autofree gchar *text = g_strdup_printf("%s (%s)", r->name, axis_profile_revision_is_unlocked(r) ?
				"proven" : "locked: trials only");

			ADD(select, htmx_option_new_with_value(value, text));
		}
		ADD(field, label);
		ADD(field, select);
		ADD(form, field);
		htmx_element_add_class(HTMX_ELEMENT(outcome), "field");
		htmx_node_set_text_content(HTMX_NODE(outcome_label), "Trial or replacement");
		htmx_element_set_id(HTMX_ELEMENT(outcomes), "convert-outcome");
		ADD(outcomes, htmx_option_new_with_value("trial", "Trial: the original stays"));
		ADD(outcomes, htmx_option_new_with_value("replace", "Replace the original"));
		ADD(outcome, outcome_label);
		ADD(outcome, outcomes);
		ADD(form, outcome);
		ADD(form, axis_web_auth_button("Review the conversion", NULL));
		ADD(card, form);
	} else {
		ADD(card, axis_web_storage_text("help", "There is no usable video conversion profile. Copy a "
			"built-in one under Conversion profiles."));
	}
	if (figures->records != NULL && figures->records->len > 0) {
		HtmxUl *list = htmx_ul_new();

		ADD(card, heading(2, "Conversions of this file"));
		for (i = 0; i < figures->records->len; i++) {
			const AxisConversionRecord *r = g_ptr_array_index(figures->records, i);
			g_autofree gchar *href = g_strdup_printf("/conversions/%" G_GINT64_FORMAT, r->id);
			g_autofree gchar *when = axis_web_format_time(r->requested_us, "");
			g_autofree gchar *text = g_strdup_printf("%s, %s: %s (%s)", g_strcmp0(r->outcome, "replace") == 0 ?
				"Replacement" : "Trial", r->profile_name, axis_conversion_state_label(r->state), when);
			HtmxLi *item = htmx_li_new();

			ADD(item, axis_web_browser_link(href, text));
			ADD(list, item);
		}
		ADD(card, list);
	}
	return card;
}

/* ── What other parts call ──────────────────────────────────────── */

static gboolean
hook_describe(
	gpointer             data,
	AxisDbConn          *conn,
	const AxisJobClaim  *claim,
	const gchar         *worker,
	const gchar         *machine,
	AxisConvertWork    **work,
	gchar              **why_not,
	gboolean            *permanent,
	GError             **error
){
	return axis_conversions_describe(data, conn, claim, worker, machine, work, why_not, permanent, error);
}

static gboolean
hook_encoded(
	gpointer                  data,
	AxisDbConn               *conn,
	gint64                    job,
	const AxisConvertResult  *result,
	const gchar              *worker,
	GError                  **error
){
	return axis_conversions_encoded(data, conn, job, result, worker, error);
}

static gboolean
hook_describe_verify(
	gpointer             data,
	AxisDbConn          *conn,
	const AxisJobClaim  *claim,
	const gchar         *worker,
	AxisVerifyWork     **work,
	gchar              **why_not,
	gboolean            *permanent,
	GError             **error
){
	return axis_conversions_describe_verify(data, conn, claim, worker, work, why_not, permanent, error);
}

static gboolean
hook_verified(
	gpointer                 data,
	AxisDbConn              *conn,
	gint64                   job,
	const AxisVerifyResult  *result,
	const gchar             *worker,
	GError                 **error
){
	return axis_conversions_verified(data, conn, job, result, worker, error);
}

const AxisWorkerConvertHooks *
axis_web_conversions_worker_hooks(void)
{
	static const AxisWorkerConvertHooks hooks = { hook_describe, hook_encoded, hook_describe_verify, hook_verified };

	return &hooks;
}

AxisCompatVerdict
axis_web_conversions_compat_verdict(
	gpointer     compat,
	gint64       root_id,
	const gchar *slug
){
	g_autoptr(AxisCompatReport) report = NULL;
	g_autoptr(GError) error = NULL;

	if (compat == NULL || !axis_compat_is_enabled(compat))
		return AXIS_VERDICT_UNKNOWN;
	if (!axis_compat_compute_report(compat, &report, &error)) {
		g_message("conversions: the player check's verdict cannot be worked out: %s", error->message);
		return AXIS_VERDICT_UNKNOWN;
	}
	return axis_compat_report_root_verdict(report, root_id, slug, NULL);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
finish_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebConversions *self = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	gint64 id = payload != NULL ? json_object_get_int_member_with_default(payload, "conversion", 0) : 0;
	gboolean ok = axis_conversions_run_finish(self->conversions, context, error);

	/* A conversion of a run that ended: the run asks for its next file now
	 * rather than at its next minute. */
	if (id > 0) {
		g_autoptr(AxisConversionRecord) record = NULL;
		g_autoptr(GError) local = NULL;

		if (axis_conversions_get(self->conversions, id, &record, NULL, &local) && record->run_id > 0 &&
		    record->finished_us > 0 && !axis_conversion_runs_request_step(self->runs, &local))
			g_message("conversion runs: the step could not be queued: %s", local->message);
	}
	return ok;
}

static gboolean
run_list_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebConversions *self = user_data;

	return axis_conversion_runs_run_list_job(self->runs, context, error);
}

static gboolean
run_step_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebConversions *self = user_data;

	return axis_conversion_runs_run_step_job(self->runs, context, error);
}

static gboolean
tidy_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebConversions *self = user_data;

	return axis_conversions_run_tidy(self->conversions, context, error);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_CONVERSION_FINISH_KIND, AXIS_CONVERSION_QUEUE, AXIS_CONVERSION_FINISH_ATTEMPTS, finish_handler,
	  "Checking and placing a converted file", AXIS_JOB_CLASS_WORK },
	{ AXIS_CONVERSION_TIDY_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, tidy_handler,
	  "Tidying the conversions' work areas", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_CONVERSION_RUN_LIST_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, run_list_handler,
	  "Making the list of a conversion run", AXIS_JOB_CLASS_WORK },
	{ AXIS_CONVERSION_RUN_STEP_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, run_step_handler,
	  "Starting conversions for the next files of a conversion run", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_CONVERSION_TIDY_SCHEDULE, AXIS_CONVERSION_TIDY_KIND, NULL, AXIS_CONVERSION_TIDY_INTERVAL_S },
	{ AXIS_CONVERSION_RUN_STEP_SCHEDULE, AXIS_CONVERSION_RUN_STEP_KIND, NULL, AXIS_CONVERSION_RUN_STEP_INTERVAL_S },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Conversions", "/conversions", 55, AXIS_NAV_GROUP_MANAGEMENT, TRUE, NULL, 1 },
	{ "Conversion runs", "/conversions/runs", 56, AXIS_NAV_GROUP_MANAGEMENT, TRUE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = { "storage", "probe", "workers", NULL };

/* A run's list, retried as it was, finds its run failed and does nothing. */
static const gchar *const retry_elsewhere[] = {
	AXIS_CONVERSION_RUN_LIST_KIND, "A run whose list was not made does not go on: make a new run under Conversion "
		"runs.",
	NULL
};

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebConversions *self = user_data;

	/* The conversion runs first: "/conversions/runs" is no conversion's id. */
	axis_web_runs_register(self->runs_ui, web);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions", AXIS_WEB_ROUTE_ADMIN, handle_list, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions", AXIS_WEB_ROUTE_ADMIN, handle_request, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/new", AXIS_WEB_ROUTE_ADMIN, handle_new, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/resume", AXIS_WEB_ROUTE_ADMIN, handle_resume, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/prove/:id", AXIS_WEB_ROUTE_ADMIN, handle_proving, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/prove/:id", AXIS_WEB_ROUTE_ADMIN, handle_prove, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/latest", AXIS_WEB_ROUTE_ADMIN, handle_latest, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/:id", AXIS_WEB_ROUTE_ADMIN, handle_one, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/conversions/:id/progress", AXIS_WEB_ROUTE_ADMIN, handle_progress,
		self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/:id/cancel", AXIS_WEB_ROUTE_ADMIN, handle_cancel, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/:id/remove-trial", AXIS_WEB_ROUTE_ADMIN,
		handle_remove_trial, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/conversions/:id/set-aside", AXIS_WEB_ROUTE_ADMIN, handle_set_aside,
		self);
}

/* A volume that comes back: what was in flight on it is looked at again. */
static void
on_volume(
	AxisStorage          *storage,
	const AxisVolumeInfo *volume,
	gboolean              state_changed,
	gpointer              user_data
){
	AxisWebConversions *self = user_data;
	g_autoptr(GError) error = NULL;

	if (volume == NULL || !state_changed || volume->state != AXIS_VOLUME_ONLINE)
		return;
	/* Quick: one statement; the tidy job does the rest on its queue. */
	if (!axis_conversions_request_tidy(self->conversions, &error))
		g_message("conversions: the tidy job could not be queued: %s", error->message);
}

static gboolean
module_start(
	gpointer  user_data,
	GError  **error
){
	AxisWebConversions *self = user_data;
	g_autoptr(GError) local = NULL;

	/* At start-up: recovery of whatever was in flight. */
	if (!axis_conversions_request_tidy(self->conversions, &local))
		g_message("conversions: the tidy job could not be queued at start-up: %s", local->message);
	if (self->listener == 0)
		self->listener = axis_storage_add_listener(axis_conversions_get_storage(self->conversions), on_volume, self);
	g_atomic_int_set(&self->enabled, 1);
	return TRUE;
}

static void
module_stop(gpointer user_data)
{
	AxisWebConversions *self = user_data;

	g_atomic_int_set(&self->enabled, 0);
	if (self->listener != 0) {
		axis_storage_remove_listener(axis_conversions_get_storage(self->conversions), self->listener);
		self->listener = 0;
	}
}

const AxisModuleInfo *
axis_web_conversions_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_CONVERSION_MODULE_ID;
		info.name = "Conversions";
		info.start = module_start;
		info.stop = module_stop;
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Converts single files on request, on a worker: a trial keeps the result aside to be "
			"played; a replacement puts it in the original's place once this server has checked it ten ways, and "
			"deletes the original. Nothing is written while Read-only storage is on.";
		info.settings = axis_conversions_get_setting_table();
		info.depends_on = depends_on;
		info.job_retry_elsewhere = retry_elsewhere;
	}
	return &info;
}

AxisWebConversions *
axis_web_conversions_new(AxisConversions *conversions)
{
	AxisWebConversions *self;

	g_return_val_if_fail(conversions != NULL, NULL);

	self = g_new0(AxisWebConversions, 1);
	self->conversions = axis_conversions_ref(conversions);
	self->runs = axis_conversion_runs_new(conversions);
	self->runs_ui = axis_web_runs_new(self->runs);
	return self;
}

AxisConversionRuns *
axis_web_conversions_get_runs(AxisWebConversions *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->runs;
}

void
axis_web_conversions_stop(AxisWebConversions *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
	axis_web_runs_stop(self->runs_ui);
}

void
axis_web_conversions_free(AxisWebConversions *self)
{
	if (self == NULL)
		return;
	axis_web_conversions_stop(self);
	module_stop(self);
	axis_web_runs_free(self->runs_ui);
	axis_conversion_runs_unref(self->runs);
	axis_conversions_unref(self->conversions);
	g_free(self);
}
