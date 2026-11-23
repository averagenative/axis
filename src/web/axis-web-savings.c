/*
 * axis-web-savings.c - the estimate module: its jobs, its settings, the
 * work its pages hand to threads, the file page's card
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-savings-private.h"

#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-analysis.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "storage/axis-storage.h"
#include "web/axis-web-browser.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebSavings         *owner;
	AxisWebSavingsWorkFunc  work;
	AxisWebSavingsDoneFunc  done;
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
axis_web_savings_dispatch(
	AxisWebSavings         *self,
	HtmxRequest            *request,
	AxisWebSavingsWorkFunc  work,
	AxisWebSavingsDoneFunc  done,
	gpointer                data,
	GDestroyNotify          free_data
){
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_SAVINGS_MAX_PENDING) {
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

AxisWebSavings *
axis_web_savings_new(AxisEstimator *estimator)
{
	AxisWebSavings *self;

	g_return_val_if_fail(estimator != NULL, NULL);

	self = g_new0(AxisWebSavings, 1);
	self->estimator = axis_estimator_ref(estimator);
	return self;
}

void
axis_web_savings_set_compat(
	AxisWebSavings *self,
	AxisCompat     *compat
){
	g_return_if_fail(self != NULL);

	g_clear_pointer(&self->compat, axis_compat_unref);
	if (compat != NULL)
		self->compat = axis_compat_ref(compat);
}

void
axis_web_savings_stop(AxisWebSavings *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_savings_free(AxisWebSavings *self)
{
	if (self == NULL)
		return;

	axis_web_savings_stop(self);
	g_clear_pointer(&self->compat, axis_compat_unref);
	axis_estimator_unref(self->estimator);
	g_free(self);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

gchar *
axis_web_savings_range(
	gint64 mid,
	gint64 low,
	gint64 high
){
	g_autofree gchar *m = axis_web_browser_format_size((guint64)MAX(mid, 0));
	g_autofree gchar *l = axis_web_browser_format_size((guint64)MAX(low, 0));
	g_autofree gchar *h = axis_web_browser_format_size((guint64)MAX(high, 0));

	return g_strdup_printf("about %s (%s to %s)", m, l, h);
}

static HtmxNode *
cell_text(
	const gchar *css,
	const gchar *text
){
	HtmxTd *cell = htmx_td_new();

	if (css != NULL)
		htmx_element_add_class(HTMX_ELEMENT(cell), css);
	htmx_node_set_text_content(HTMX_NODE(cell), text);
	return HTMX_NODE(cell);
}

HtmxNode *
axis_web_savings_file_card(GArray *estimates)
{
	static const AxisWebColumn columns[] = {
		{ "Conversion profile", AXIS_WEB_COL_NAME },
		{ "Decision", AXIS_WEB_COL_WIDE },
		{ "Size after", AXIS_WEB_COL_RANGE },
		{ "Saving", AXIS_WEB_COL_RANGE },
		{ "Accuracy", AXIS_WEB_COL_TEXT },
		{ NULL, 0 }
	};
	HtmxNode *card = axis_web_storage_card("Space saved if converted");
	HtmxNode *table;
	HtmxTbody *body;
	gboolean uncalibrated = FALSE;
	guint i;

	if (estimates == NULL || estimates->len == 0) {
		BROWSER_ADD(card, axis_web_storage_text("help", "No conversion profile applies to this kind of file."));
		return card;
	}
	table = axis_web_table_new("file-estimates", columns);
	body = htmx_tbody_new();
	for (i = 0; i < estimates->len; i++) {
		const AxisFileEstimate *e = &g_array_index(estimates, AxisFileEstimate, i);
		const AxisEstimateResult *r = &e->result;
		g_autofree gchar *href = g_strdup_printf("/savings/target/%s", e->target.id);
		g_autofree gchar *name = axis_web_analysis_profile_label(e->target.family_name, e->target.name);
		g_autofree gchar *decision = NULL;
		HtmxTr *row = htmx_tr_new();
		HtmxTd *target = htmx_td_new();

		BROWSER_ADD(target, axis_web_browser_link(href, name));
		BROWSER_ADD(row, target);
		if (r->reason == AXIS_ESTIMATE_ELIGIBLE)
			decision = g_strdup("Would be converted");
		else if (r->reason == AXIS_ESTIMATE_ALBUM && r->album_file > 0)
			decision = g_strdup_printf("Skipped: %s (another file of its album: %s)",
				axis_estimate_reason_label(r->reason), axis_estimate_reason_label(r->album_reason));
		else if (r->reason == AXIS_ESTIMATE_OFFLINE)
			decision = g_strdup("Would be converted once its disk is back online");
		else
			decision = g_strdup_printf("Skipped: %s", axis_estimate_reason_label(r->reason));
		{
			HtmxTd *cell = htmx_td_new();
			HtmxSpan *why = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(why), decision);
			BROWSER_ADD(cell, why);
			if (!axis_estimate_result_converts(r))
				BROWSER_ADD(cell, axis_web_storage_text("help", axis_estimate_reason_help(r->reason)));
			BROWSER_ADD(row, cell);
		}
		if (r->sized) {
			g_autofree gchar *after = axis_web_savings_range(r->mid, r->low, r->high);
			g_autofree gchar *saving = axis_web_savings_range(r->now - r->mid, r->now - r->high, r->now - r->low);

			BROWSER_ADD(row, cell_text("num", after));
			BROWSER_ADD(row, cell_text("num", saving));
		} else {
			BROWSER_ADD(row, cell_text("num", "not estimated"));
			BROWSER_ADD(row, cell_text("num", ""));
		}
		if (r->sized) {
			g_autofree gchar *state = e->samples > 0 ?
				g_strdup_printf("%s (%u sample%s)", axis_calibration_state_label(e->state), e->samples,
					e->samples == 1 ? "" : "s") :
				g_strdup(axis_calibration_state_label(e->state));

			if (e->state != AXIS_CALIBRATION_FULL)
				uncalibrated = TRUE;
			BROWSER_ADD(row, cell_text(NULL, state));
		} else {
			BROWSER_ADD(row, cell_text(NULL, ""));
		}
		BROWSER_ADD(body, row);
	}
	BROWSER_ADD(table, body);
	BROWSER_ADD(card, table);
	if (uncalibrated)
		BROWSER_ADD(card, axis_web_storage_text("help", "Figures not based on measurements are informed "
			"guesses, so the ranges are wide on purpose. Nothing is converted here; a conversion keeps the "
			"original when the result is not smaller."));
	return card;
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
module_start(
	gpointer   user_data,
	GError   **error
){
	AxisWebSavings *self = user_data;

	axis_estimator_set_enabled(self->estimator, TRUE);
	/* What a killed daemon's measurement left behind. */
	axis_estimator_clean_scratch(self->estimator);
	return TRUE;
}

static void
module_stop(gpointer user_data)
{
	AxisWebSavings *self = user_data;

	axis_estimator_set_enabled(self->estimator, FALSE);
}

static gboolean
refresh_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebSavings *self = user_data;

	return axis_estimator_run_refresh_job(self->estimator, context, error);
}

static gboolean
due_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebSavings *self = user_data;

	return axis_estimator_run_due_job(self->estimator, context, error);
}

static gboolean
sample_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebSavings *self = user_data;

	return axis_estimator_run_sample_job(self->estimator, context, error);
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_savings_register_pages(user_data, web);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_ESTIMATE_REFRESH_KIND, AXIS_JOB_MAINTENANCE_QUEUE, AXIS_ESTIMATE_REFRESH_ATTEMPTS, refresh_handler,
	  "Working out what converting would save", AXIS_JOB_CLASS_WORK },
	{ AXIS_ESTIMATE_DUE_KIND, AXIS_JOB_MAINTENANCE_QUEUE, 1, due_handler,
	  "Checking whether the savings need working out again", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_ESTIMATE_SAMPLE_KIND, AXIS_ESTIMATE_SAMPLE_QUEUE, 1, sample_handler,
	  "Converting samples to measure the savings", AXIS_JOB_CLASS_WORK },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_ESTIMATE_DUE_SCHEDULE, AXIS_ESTIMATE_DUE_KIND, NULL, AXIS_ESTIMATE_DUE_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	{ "Space savings", "/savings", 30, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = {
	AXIS_STORAGE_MODULE_ID, AXIS_LIBRARY_MODULE_ID, AXIS_PROBE_MODULE_ID, AXIS_WEB_BROWSER_MODULE_ID,
	AXIS_ANALYSIS_MODULE_ID, NULL
};

/* A measurement names the profile revision current when it was asked for,
 * which a later one may have replaced: retried as it was, it would measure
 * the old one. */
static const gchar *const retry_elsewhere[] = {
	AXIS_ESTIMATE_SAMPLE_KIND, "A measurement follows the conversion profile version it was asked for, which may be out of "
		"date now: ask for a new one on the Space savings page.",
	NULL
};

const AxisModuleInfo *
axis_web_savings_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_ESTIMATE_MODULE_ID;
		info.name = "Space savings";
		info.start = module_start;
		info.stop = module_stop;
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.settings = axis_estimator_get_setting_table();
		info.description = "Estimates what converting the library with each built-in conversion profile would "
			"save, per file, disk and library, with honest ranges and the reasons files would be skipped, and "
			"can measure short samples to make them more accurate. It converts nothing and changes no media "
			"file.";
		info.depends_on = depends_on;
		info.job_retry_elsewhere = retry_elsewhere;
	}
	return &info;
}
