/*
 * axis-web-compat.c - the compat module: its jobs, its settings, and the
 * work its pages hand to threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-compat-private.h"

#include "compat/axis-compat-notify.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "storage/axis-storage.h"
#include "transcode/axis-estimator.h"
#include "web/axis-web-attention.h"

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebCompat         *owner;
	AxisWebCompatWorkFunc  work;
	AxisWebCompatDoneFunc  done;
	HtmxRequest           *request;
	HtmxResponse          *deferred;
	gpointer               data;
	GDestroyNotify         free_data;
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
axis_web_compat_dispatch(
	AxisWebCompat         *self,
	HtmxRequest           *request,
	AxisWebCompatWorkFunc  work,
	AxisWebCompatDoneFunc  done,
	gpointer               data,
	GDestroyNotify         free_data
){
	Task *task;
	GTask *gtask;
	HtmxResponse *returned;

	if (self->stopping || self->pending >= AXIS_WEB_COMPAT_MAX_PENDING) {
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

AxisWebCompat *
axis_web_compat_new(AxisCompat *compat)
{
	AxisWebCompat *self;

	g_return_val_if_fail(compat != NULL, NULL);

	self = g_new0(AxisWebCompat, 1);
	self->compat = axis_compat_ref(compat);
	return self;
}

void
axis_web_compat_stop(AxisWebCompat *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_compat_free(AxisWebCompat *self)
{
	if (self == NULL)
		return;

	axis_web_compat_stop(self);
	axis_compat_unref(self->compat);
	g_free(self);
}

/* ── The module ─────────────────────────────────────────────────── */

static gboolean
module_start(
	gpointer   user_data,
	GError   **error
){
	AxisWebCompat *self = user_data;

	axis_compat_set_enabled(self->compat, TRUE);
	return TRUE;
}

static void
module_stop(gpointer user_data)
{
	AxisWebCompat *self = user_data;

	axis_compat_set_enabled(self->compat, FALSE);
}

static gboolean
poll_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebCompat *self = user_data;

	return axis_compat_run_poll_job(self->compat, context, error);
}

static gboolean
history_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebCompat *self = user_data;

	return axis_compat_run_history_job(self->compat, context, error);
}

static gboolean
notify_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	AxisWebCompat *self = user_data;

	return axis_compat_run_notify_job(self->compat, context, error);
}

/* ── What needs attention: a media server not answering ─────────── */

/*
 * A media server whose connection checks fail (add-attention-notices, slice
 * N2), from the handful of rows of compat_state in one statement.  Axis
 * checks again by itself, and a server that restarts answers again within
 * minutes, so it is a notice only once it has gone unanswered for
 * AXIS_ATTENTION_SERVER_WAIT_S since the last check that worked (or since
 * collection began, when none ever did) and on a second failed check at
 * least.  Nothing arrives or is stored through a media server, so it is
 * "Worth a look".  A server whose connection is no longer set up has no
 * notice.  "newest" is the second of the last check that worked: an outage
 * after a later good check is a new occurrence.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebCompat *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	if (!axis_db_pool_query_bounded(axis_compat_get_pool(self->compat),
	        "SELECT id, server_kind, coalesce(server_name, ''), failures,"
	        " coalesce(floor(extract(epoch FROM coalesce(last_poll_ok_at, started_at)))::bigint, 0)"
	        " FROM compat_state WHERE last_error IS NOT NULL AND failures >= 2 ORDER BY id", reading->deadline_ms,
	        &result, error))
		return FALSE;
	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		const gchar *kind = axis_db_result_get_text(result, i, 1);
		const gchar *name = axis_db_result_get_text(result, i, 2);
		g_autofree gchar *subject = NULL;
		g_autofree gchar *how_long = NULL;
		g_autofree gchar *tail = NULL;
		AxisAttentionItem *item;
		gint64 id = 0;
		gint64 failures = 0;
		gint64 since = 0;

		axis_db_result_get_int64(result, i, 0, &id);
		axis_db_result_get_int64(result, i, 3, &failures);
		axis_db_result_get_int64(result, i, 4, &since);
		if (kind == NULL || !axis_compat_is_configured_kind(self->compat, kind))
			continue;
		if (since <= 0 || reading->now_us - since * G_USEC_PER_SEC < (gint64)AXIS_ATTENTION_SERVER_WAIT_S * G_USEC_PER_SEC)
			continue;
		subject = g_strdup_printf("%" G_GINT64_FORMAT, id);
		how_long = axis_attention_duration(reading->now_us - since * G_USEC_PER_SEC);
		tail = g_strdup_printf(" for %s (%" G_GINT64_FORMAT " failed checks in a row). Test the connection.", how_long,
			failures);
		item = axis_attention_item_new("compat.failing", subject, AXIS_ATTENTION_LOOK, 1, since);
		axis_attention_item_say(item, "Axis has not reached the media server ", name != NULL && name[0] != '\0' ? name :
			kind, tail);
		axis_attention_item_link(item, "/compat", "Open Player compatibility");
		item->since_us = since * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_compat_register_pages(user_data, web);
	axis_web_add_attention_source(web, "the media servers", attention_source, user_data);
}

static const AxisJobHandlerInfo handlers[] = {
	{ AXIS_COMPAT_POLL_KIND, AXIS_COMPAT_QUEUE, 1, poll_handler,
	  "Watching what the media servers play", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_COMPAT_HISTORY_KIND, AXIS_COMPAT_QUEUE, 1, history_handler,
	  "Reading the media servers' play history", AXIS_JOB_CLASS_UPKEEP },
	{ AXIS_COMPAT_NOTIFY_KIND, AXIS_COMPAT_QUEUE, 1, notify_handler,
	  "Telling the media servers which folders changed", AXIS_JOB_CLASS_UPKEEP },
	{ NULL, NULL, 0, NULL, NULL, AXIS_JOB_CLASS_UNSET }
};

static const AxisJobScheduleInfo schedules[] = {
	{ AXIS_COMPAT_POLL_SCHEDULE, AXIS_COMPAT_POLL_KIND, NULL, AXIS_COMPAT_POLL_INTERVAL_SECONDS },
	{ AXIS_COMPAT_HISTORY_SCHEDULE, AXIS_COMPAT_HISTORY_KIND, NULL, AXIS_COMPAT_HISTORY_INTERVAL_SECONDS },
	{ AXIS_COMPAT_NOTIFY_SCHEDULE, AXIS_COMPAT_NOTIFY_KIND, NULL, AXIS_COMPAT_NOTIFY_INTERVAL_SECONDS },
	{ NULL, NULL, NULL, 0 }
};

static const AxisNavEntry nav_entries[] = {
	/* Its pages are for administrators: what is played is personal. */
	{ "Player compatibility", "/compat", 40, AXIS_NAV_GROUP_MANAGEMENT, TRUE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const gchar *const depends_on[] = {
	AXIS_STORAGE_MODULE_ID, AXIS_LIBRARY_MODULE_ID, AXIS_PROBE_MODULE_ID, AXIS_ESTIMATE_MODULE_ID, NULL
};

const AxisModuleInfo *
axis_web_compat_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_COMPAT_MODULE_ID;
		info.name = "Player compatibility";
		info.start = module_start;
		info.stop = module_stop;
		info.job_handlers = handlers;
		info.job_schedules = schedules;
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.settings = axis_compat_get_setting_table();
		info.description = "Learns, read-only, what the household's players can play directly, from Plex, "
			"Jellyfin or both: the devices, the sessions seen while they play and which media server libraries "
			"they play. Gives each conversion profile a playback verdict for each media server library (safe, "
			"risky or unknown: unknown is never safe) and a recommendation, judging a library that two servers "
			"serve by the devices of both. It sends only read requests to the addresses in Settings, and one other: after an "
			"import it asks each server to scan the one folder that changed (Plex's refresh of a library section "
			"limited to the folder, Jellyfin's \"media updated\"), never a whole library. It changes nothing "
			"else on a server and nothing in the media files.";
		info.depends_on = depends_on;
	}
	return &info;
}
