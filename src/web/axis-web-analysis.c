/*
 * axis-web-analysis.c - the analysis module: its jobs, its settings, the
 * work its pages hand to threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-analysis-private.h"

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "storage/axis-storage.h"
#include "web/axis-web-browser.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebAnalysis         *owner;
	AxisWebAnalysisWorkFunc  work;
	AxisWebAnalysisDoneFunc  done;
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
axis_web_analysis_dispatch(
	AxisWebAnalysis         *self,
	HtmxRequest             *request,
	AxisWebAnalysisWorkFunc  work,
	AxisWebAnalysisDoneFunc  done,
	gpointer                 data,
	GDestroyNotify           free_data
){
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_ANALYSIS_MAX_PENDING) {
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

AxisWebAnalysis *
axis_web_analysis_new(AxisAnalysis *analysis)
{
	AxisWebAnalysis *self;

	g_return_val_if_fail(analysis != NULL, NULL);

	self = g_new0(AxisWebAnalysis, 1);
	self->analysis = axis_analysis_ref(analysis);
	return self;
}

void
axis_web_analysis_stop(AxisWebAnalysis *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_analysis_free(AxisWebAnalysis *self)
{
	if (self == NULL)
		return;

	axis_web_analysis_stop(self);
	axis_analysis_unref(self->analysis);
	g_free(self);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
refresh_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebAnalysis *self = user_data;

	return axis_analysis_run_refresh_job(self->analysis, context, error);
}

static gboolean
due_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebAnalysis *self = user_data;

	return axis_analysis_run_due_job(self->analysis, context, error);
}

/* ── The dashboard: the collection's size ───────────────────────── */

/* On a worker thread: the latest snapshot's totals (no report's facts but
 * the overall one). */
static gpointer
dashboard_collect(gpointer user_data)
{
	AxisWebAnalysis *self = user_data;
	static const gchar *const reports[] = { "overall", NULL };
	g_autoptr(GError) error = NULL;
	AxisAnalysisSnapshot *snapshot = NULL;

	if (!axis_analysis_load_snapshot(self->analysis, 0, reports, &snapshot, &error)) {
		g_debug("dashboard: cannot read the analysis snapshot: %s", error->message);
		return NULL;
	}
	return snapshot;
}

static HtmxNode *
figure(
	const gchar *value,
	const gchar *label
){
	HtmxDiv *box = htmx_div_new();
	HtmxSpan *number = htmx_span_new();
	HtmxSpan *text = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "figure");
	htmx_element_add_class(HTMX_ELEMENT(number), "figure-value");
	htmx_node_set_text_content(HTMX_NODE(number), value);
	htmx_element_add_class(HTMX_ELEMENT(text), "figure-label");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	BROWSER_ADD(box, number);
	BROWSER_ADD(box, text);
	return HTMX_NODE(box);
}

/* The collection's files and size, as the Analysis overview's Totals show
 * them, with when they were taken. */
static void
dashboard_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          figures,
	gpointer          user_data
){
	const AxisAnalysisSnapshot *snapshot = figures;
	HtmxDiv *piece = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	if (snapshot == NULL) {
		BROWSER_ADD(piece, axis_web_storage_text("help", "The library has not been measured yet: Disk usage fills "
			"in once scans have finished and file details have been read."));
	} else {
		const AxisAnalysisFact *overall = axis_analysis_snapshot_fact(snapshot, "overall", "", NULL);
		g_autofree gchar *files = axis_web_browser_format_count((guint64)MAX(overall != NULL ? overall->paths : 0, 0));
		g_autofree gchar *size = axis_web_browser_format_size((guint64)MAX(overall != NULL ? overall->bytes : 0, 0));
		HtmxDiv *row = htmx_div_new();
		HtmxP *line = htmx_p_new();
		g_autofree gchar *when = axis_web_format_time(snapshot->taken_us, "unknown");
		g_autofree gchar *text = g_strdup_printf("Library as of %s. ", when);

		htmx_element_add_class(HTMX_ELEMENT(row), "figure-row");
		BROWSER_ADD(row, figure(size, "in the library"));
		BROWSER_ADD(row, figure(files, files[0] == '1' && files[1] == '\0' ? "file" : "files"));
		BROWSER_ADD(piece, row);
		htmx_element_add_class(HTMX_ELEMENT(line), "as-of");
		htmx_node_set_text_content(HTMX_NODE(line), text);
		BROWSER_ADD(line, axis_web_browser_link("/analysis", "Disk usage"));
		BROWSER_ADD(piece, line);
	}
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SPACE, 0, NULL, HTMX_NODE(piece));
}

static void
dashboard_free(gpointer figures)
{
	axis_analysis_snapshot_free(figures);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_analysis_register_pages(user_data, web);
	axis_web_add_dashboard_part(web, dashboard_collect, dashboard_render, dashboard_free, user_data);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_SPACE);
}

static const AxisJobHandlerInfo handlers[] = {
	/* One attempt: a refresh that fails is tried again by the next check. */
	{ AXIS_ANALYSIS_REFRESH_KIND, AXIS_JOB_MAINTENANCE_QUEUE, AXIS_ANALYSIS_REFRESH_ATTEMPTS, refresh_handler,
	  "Refreshing the Disk usage reports", AXIS_JOB_CLASS_WORK },
	{ AXIS_ANALYSIS_DUE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, due_handler,
	  "Checking whether the Disk usage reports need updating", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_ANALYSIS_DUE_SCHEDULE, AXIS_ANALYSIS_DUE_KIND, NULL, AXIS_ANALYSIS_DUE_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Disk usage", "/analysis", 20, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

/* The Library pages too: every figure is a link to their file list. */
static const gchar *const depends_on[] = {
	AXIS_STORAGE_MODULE_ID, AXIS_LIBRARY_MODULE_ID, AXIS_PROBE_MODULE_ID, AXIS_WEB_BROWSER_MODULE_ID, NULL
};

const AxisModuleInfo *
axis_web_analysis_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_ANALYSIS_MODULE_ID;
		info.name = "Disk usage";
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.settings = axis_analysis_get_setting_table();
		info.description = "Reports about the library, computed from the index after scans and file reads have "
			"settled: where the space went, what the files are made of, what is large for its class, what "
			"is likely duplicated or split across disks, and what looks like debris. It only reads the "
			"index; it never opens a media file and changes nothing.";
		info.depends_on = depends_on;
	}

	return &info;
}
