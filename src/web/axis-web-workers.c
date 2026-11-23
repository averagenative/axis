/*
 * axis-web-workers.c - the Workers page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-workers.h"

#include <string.h>

#include "api/axis-worker-api.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "transcode/axis-convert.h"
#include "transcode/axis-profile-store.h"
#include "web/axis-password.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-browser-private.h"
#include "web/axis-web-conversions.h"
#include "web/axis-web-notice.h"
#include "web/axis-web-place.h"
#include "web/axis-web-profiles-private.h"
#include "web/axis-web-storage-private.h"

#define MAX_PENDING 16

/* A worker counts as connected when heard from within this, or three
 * heartbeats if that is longer (last-seen times are written at most every
 * ten seconds). */
#define CONNECTED_MS 30000

struct _AxisWebWorkers
{
	AxisDbPool       *pool;
	AxisWorkerTokens *tokens;
	AxisEstimator    *estimator;	/* owned reference, or NULL */
	AxisJobRegistry  *kinds;	/* owned reference, or NULL */
	AxisConversions  *conversions;	/* owned reference, or NULL: where checks run, the pause */
	guint             heartbeat_ms;
	guint             pending;
	gboolean          stopping;
	gint64            started_us;	/* when these pages were made: the daemon's start */
};

#define ADD axis_web_storage_add

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef void (*WorkFunc)(AxisWebWorkers *self, gpointer data);
typedef HtmxResponse *(*DoneFunc)(AxisWebWorkers *self, HtmxRequest *request, gpointer data);

typedef struct
{
	AxisWebWorkers *owner;
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
	g_object_unref(task->request);
	g_object_unref(task->deferred);
	g_free(task);
}

static HtmxResponse *
dispatch(
	AxisWebWorkers *self,
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

/* ── Pieces ─────────────────────────────────────────────────────── */

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

	ADD(cell, child);
	return HTMX_NODE(cell);
}

static HtmxNode *
yes_no(gboolean yes)
{
	return axis_web_browser_badge(yes ? "Yes" : "No", yes ? "badge-succeeded" : "badge-note");
}

/* Never seen, connected, silent since, revoked. */
static HtmxNode *
state_badge(
	AxisWebWorkers       *self,
	const AxisWorkerInfo *worker
){
	guint connected = MAX((guint)CONNECTED_MS, 3 * self->heartbeat_ms + 10000);

	if (worker->revoked_us > 0)
		return axis_web_browser_badge("Revoked", "badge-failed");
	if (worker->paused_us > 0)
		return axis_web_browser_badge("Paused", "badge-failed");
	if (worker->seen_ago_ms < 0)
		return axis_web_browser_badge("Never seen", "badge-note");
	if (worker->seen_ago_ms <= (gint64)connected)
		return axis_web_browser_badge("Connected", "badge-succeeded");
	return axis_web_browser_badge("Not heard from", "badge-warn");
}

static HtmxNode *
encoders_table(const AxisEncoderCapabilities *caps)
{
	static const AxisWebColumn columns[] = {
		{ "Encoder", AXIS_WEB_COL_TEXT },
		{ "Usable", AXIS_WEB_COL_STATE },
		{ "10-bit", AXIS_WEB_COL_STATE },
		{ "HDR10", AXIS_WEB_COL_STATE },
		{ "Note", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	const AxisConvertEncoder *table = axis_convert_encoders();
	HtmxNode *node = axis_web_table_new("encoders", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < AXIS_CONVERT_N_ENCODERS; i++) {
		const AxisEncoderCapability *cap = &caps->encoders[i];
		HtmxTr *row = htmx_tr_new();
		HtmxCode *name = htmx_code_new_with_text(table[i].name);

		ADD(row, cell_node(HTMX_NODE(name)));
		if (!cap->measured) {
			ADD(row, cell_text("not measured"));
			ADD(row, cell_text(""));
			ADD(row, cell_text(""));
		} else {
			ADD(row, cell_node(yes_no(cap->usable)));
			ADD(row, cell_node(yes_no(cap->ten_bit)));
			ADD(row, cell_node(yes_no(cap->hdr10)));
		}
		ADD(row, cell_text(cap->reason));
		ADD(body, row);
	}
	ADD(node, body);
	return node;
}

static HtmxNode *
volumes_table(GPtrArray *volumes)
{
	static const AxisWebColumn columns[] = {
		{ "Disk", AXIS_WEB_COL_NAME },
		{ "Identifier", AXIS_WEB_COL_WIDE },
		{ "Where the worker sees it", AXIS_WEB_COL_NAME },
		{ "How the worker checks a file", AXIS_WEB_COL_WIDE },
		{ "Last proved", AXIS_WEB_COL_DATE },
		{ NULL, 0 }
	};
	HtmxNode *node = axis_web_table_new("worker-volumes", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < volumes->len; i++) {
		const AxisWorkerVolumeRow *v = g_ptr_array_index(volumes, i);
		g_autofree gchar *proved = axis_web_storage_format_time(v->proved_us, "never");
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_text(v->volume_name));
		ADD(row, cell_node(HTMX_NODE(htmx_code_new_with_text(v->uuid != NULL ? v->uuid : ""))));
		ADD(row, cell_node(HTMX_NODE(htmx_code_new_with_text(v->path != NULL ? v->path : ""))));
		if (v->keeps_identity)
			ADD(row, cell_text("By its size, time, inode and samples of its content"));
		else
			ADD(row, cell_text("Sees this disk through a network mount that does not keep file identities: by "
				"its size, its time to the second and samples of its content"));
		ADD(row, cell_text(proved));
		ADD(body, row);
	}
	ADD(node, body);
	return node;
}

/* A job a worker holds: named in plain words, linking to its page, with its
 * number beside; a kind with no label is named by itself. */
static HtmxNode *
job_name_new(
	AxisWebWorkers         *self,
	const AxisWorkerJobRow *job
){
	const gchar *label = self->kinds != NULL && job->kind != NULL ?
		axis_job_registry_get_label(self->kinds, job->kind) : NULL;
	g_autofree gchar *href = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, job->id);
	g_autofree gchar *number = g_strdup_printf("#%" G_GINT64_FORMAT, job->id);
	HtmxSpan *name = htmx_span_new();
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxSpan *detail = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(name), "job-name");
	if (label != NULL)
		htmx_node_set_text_content(HTMX_NODE(link), label);
	else
		ADD(link, htmx_code_new_with_text(job->kind != NULL ? job->kind : ""));
	ADD(name, link);
	htmx_element_add_class(HTMX_ELEMENT(detail), "job-kind");
	htmx_node_set_text_content(HTMX_NODE(detail), number);
	ADD(name, detail);
	return HTMX_NODE(name);
}

/* A worker's jobs, each named in plain words. */
static HtmxNode *
jobs_table(
	AxisWebWorkers *self,
	GPtrArray      *jobs
){
	static const AxisWebColumn columns[] = {
		{ "Job", AXIS_WEB_COL_NAME },
		{ "Progress", AXIS_WEB_COL_NUMBER },
		{ "Status", AXIS_WEB_COL_WIDE },
		{ NULL, 0 }
	};
	HtmxNode *node = axis_web_table_new("worker-jobs", columns);
	HtmxTbody *body = htmx_tbody_new();
	guint i;

	for (i = 0; i < jobs->len; i++) {
		const AxisWorkerJobRow *job = g_ptr_array_index(jobs, i);
		g_autofree gchar *progress = job->progress >= 0 ? g_strdup_printf("%.0f%%", job->progress * 100.0) :
			g_strdup("");
		HtmxTr *row = htmx_tr_new();

		ADD(row, cell_node(job_name_new(self, job)));
		ADD(row, cell_text(progress));
		ADD(row, cell_text(job->message));
		ADD(body, row);
	}
	ADD(node, body);
	return node;
}

static HtmxNode *
action_form(
	HtmxRequest *request,
	const gchar *action,
	const gchar *label,
	HtmxNode    *extra
){
	HtmxNode *form = axis_web_auth_post_form(request, action);

	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	if (extra != NULL)
		ADD(form, extra);
	ADD(form, axis_web_auth_button(label, NULL));
	return form;
}

static HtmxNode *
worker_card(
	AxisWebWorkers       *self,
	HtmxRequest          *request,
	const AxisWorkerInfo *worker,
	AxisWebNotice        *placed
){
	HtmxNode *card = axis_web_storage_card(worker->name);
	HtmxNode *facts = axis_web_browser_facts_new();
	g_autofree gchar *card_id = g_strdup_printf("worker-%s", worker->name);
	g_autofree gchar *created = axis_web_storage_format_time(worker->created_us, "?");
	g_autofree gchar *created_line = g_strdup_printf("%s by %s", created,
		worker->created_by != NULL ? worker->created_by : "?");
	g_autofree gchar *seen = axis_web_storage_format_time(worker->last_seen_us, "never");
	g_autofree gchar *seen_line = worker->last_seen_us > 0 ?
		g_strdup_printf("%s from %s", seen, worker->last_address != NULL ? worker->last_address : "?") :
		g_strdup(seen);
	g_autofree gchar *registered = axis_web_storage_format_time(worker->registered_us, "never");
	g_autofree gchar *jobs_at_once = worker->max_jobs > 0 ? g_strdup_printf("%d", worker->max_jobs) : g_strdup("");

	htmx_element_set_id(HTMX_ELEMENT(card), card_id);
	axis_web_notice_put(placed, card, card_id);
	axis_web_browser_fact_node(facts, "State", state_badge(self, worker));
	axis_web_browser_fact(facts, "Created", created_line);
	if (worker->revoked_us > 0) {
		g_autofree gchar *revoked = axis_web_storage_format_time(worker->revoked_us, "?");
		g_autofree gchar *revoked_line = g_strdup_printf("%s by %s", revoked,
			worker->revoked_by != NULL ? worker->revoked_by : "?");

		axis_web_browser_fact(facts, "Revoked", revoked_line);
	}
	axis_web_browser_fact(facts, "Last heard from", seen_line);
	axis_web_browser_fact(facts, "Registered", registered);
	axis_web_browser_fact(facts, "axis-worker version", worker->worker_version != NULL ? worker->worker_version : "");
	axis_web_browser_fact(facts, "ffmpeg version", worker->ffmpeg_version != NULL ? worker->ffmpeg_version : "");
	axis_web_browser_fact(facts, "Jobs at once", jobs_at_once);
	ADD(card, facts);

	if (worker->revoked_us == 0 && worker->paused_us > 0) {
		g_autofree gchar *since = axis_web_storage_format_time(worker->paused_us, "?");
		g_autofree gchar *text = g_strdup_printf("Paused since %s: its encoder crashed on job %" G_GINT64_FORMAT
			" (%s). The conversion it was making is reported failed; this worker takes no more work until it is "
			"resumed here or restarted, so that an encoder or a GPU in a bad state is not given the next file. Other "
			"workers are not affected. Look at the worker's machine (its log, the GPU's driver) before resuming it.",
			since, worker->paused_job, worker->paused_reason != NULL ? worker->paused_reason : "?");
		g_autofree gchar *resume = g_strdup_printf("/workers/%s/resume", worker->name);
		g_autofree gchar *job = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, worker->paused_job);

		ADD(card, axis_web_storage_text("notice notice-warning", text));
		ADD(card, axis_web_browser_link(job, "The job that crashed"));
		ADD(card, action_form(request, resume, "Resume this worker", NULL));
	}
	if (worker->revoked_us == 0 && worker->refused_protocol >= 0) {
		g_autofree gchar *when = axis_web_storage_format_time(worker->refused_protocol_us, "?");
		g_autofree gchar *text = g_strdup_printf("Its axis-worker speaks worker protocol %d and this server %d (last "
			"seen %s): its requests are refused and it is given no work. Run the axis-worker of this server's version "
			"(the same image or release).", worker->refused_protocol, AXIS_WORKER_PROTOCOL, when);

		ADD(card, axis_web_storage_text("notice notice-warning", text));
	}

	ADD(card, heading(3, "Encoders this worker can use"));
	if (worker->caps != NULL)
		ADD(card, encoders_table(worker->caps));
	else
		ADD(card, axis_web_storage_text("help", "It has not reported a measurement yet."));

	ADD(card, heading(3, "Disks this worker can reach"));
	if (worker->volumes->len > 0)
		ADD(card, volumes_table(worker->volumes));
	else
		ADD(card, axis_web_storage_text("help", "None: this worker is offered no work until it proves a disk. Mount "
			"the library disks where the worker can read their markers."));

	ADD(card, heading(3, "Jobs this worker is running"));
	if (worker->jobs->len > 0)
		ADD(card, jobs_table(self, worker->jobs));
	else
		ADD(card, axis_web_storage_text("help", "Nothing."));

	if (worker->revoked_us == 0) {
		g_autofree gchar *measure = g_strdup_printf("/workers/%s/measure", worker->name);
		g_autofree gchar *revoke = g_strdup_printf("/workers/%s/revoke", worker->name);
		g_autofree gchar *confirm_id = g_strdup_printf("revoke-%s", worker->name);

		ADD(card, action_form(request, measure, "Measure this worker's encoders again", NULL));
		ADD(card, action_form(request, revoke, "Revoke this worker",
			axis_web_storage_checkbox(confirm_id, "Revoke its token for good", "confirm", TRUE)));
	}
	return card;
}

/* ── The page ───────────────────────────────────────────────────── */

typedef struct
{
	gchar            *done;
	GPtrArray        *workers;
	GPtrArray        *profiles;
	GPtrArray        *unmarked;	/* names of the volumes without a marker yet */
	AxisSampleStatus  sample;
	GError           *error;
} PageWork;

static void
page_work_free(gpointer data)
{
	PageWork *work = data;

	g_free(work->done);
	g_clear_pointer(&work->workers, g_ptr_array_unref);
	g_clear_pointer(&work->profiles, g_ptr_array_unref);
	g_clear_pointer(&work->unmarked, g_ptr_array_unref);
	axis_sample_status_clear(&work->sample);
	g_clear_error(&work->error);
	g_free(work);
}

static void
page_run(
	AxisWebWorkers *self,
	gpointer        data
){
	PageWork *work = data;
	g_autoptr(AxisDbResult) unmarked = NULL;
	AxisDbConn *conn;
	guint i;

	if (!axis_worker_store_list(self->pool, &work->workers, &work->error))
		return;
	/* No worker can prove these: they have no marker yet. */
	if (!axis_db_pool_exec(self->pool, "SELECT name FROM volume WHERE identity = 'filesystem' ORDER BY name", 0,
	                       NULL, &unmarked, &work->error))
		return;
	work->unmarked = g_ptr_array_new_with_free_func(g_free);
	for (i = 0; i < axis_db_result_get_n_rows(unmarked); i++)
		g_ptr_array_add(work->unmarked, g_strdup(axis_db_result_get_text(unmarked, i, 0)));
	if (self->estimator == NULL)
		return;
	conn = axis_db_pool_acquire(self->pool, &work->error);
	if (conn == NULL)
		return;
	axis_profile_store_list(conn, FALSE, &work->profiles, &work->error);
	axis_db_pool_release(self->pool, conn);
	if (work->error == NULL)
		axis_estimator_get_sample_status(self->estimator, &work->sample, &work->error);
}

static const gchar *
done_message(
	const gchar *key,
	gboolean    *error
){
	static const struct
	{
		const gchar *key;
		gboolean     error;
		const gchar *text;
	} messages[] = {
		{ "revoked", FALSE, "The worker was revoked: its token is refused from now on. A job it was running is "
		  "given back to the queue when its lease runs out, which it can no longer renew." },
		{ "already-revoked", FALSE, "That worker was revoked before." },
		{ "resumed", FALSE, "The worker is resumed: it takes work again the next time it asks for it." },
		{ "not-paused", FALSE, "That worker was not paused." },
		{ "measure-requested", FALSE, "The worker will measure its encoders again the next time it asks for work." },
		{ "measure-queued", FALSE, "A measurement of that worker's encoders is queued already." },
		{ "sample-requested", FALSE, "A sample measurement has been queued for a worker. It runs on the first worker "
		  "that can encode for the profile and has proved every disk it needs; its progress is on the Jobs page "
		  "and on the Space savings page's calibration tab." },
		{ "sample-exists", FALSE, "A sample measurement is queued or running already (on a worker or on this server)." },
		{ "invalid-name", TRUE, "A worker's name is 1 to 48 lower-case letters, digits and '-', starting with a "
		  "letter." },
		{ "exists", TRUE, "There is a worker of that name already (a revoked worker keeps its name)." },
		{ "no-worker", TRUE, "There is no such worker, or it is revoked." },
		{ "nothing-to-measure", TRUE, "No file of the disks that are online would be converted by that profile, "
		  "so there is nothing to measure." },
		{ "not-usable", TRUE, "That profile's definition is not usable, so it cannot be measured." },
		{ NULL, FALSE, NULL }
	};
	guint i;

	for (i = 0; key != NULL && messages[i].key != NULL; i++) {
		if (strcmp(key, messages[i].key) == 0) {
			*error = messages[i].error;
			return messages[i].text;
		}
	}
	return NULL;
}

static HtmxNode *
sample_card(
	HtmxRequest   *request,
	PageWork      *work,
	AxisWebNotice *placed
){
	HtmxNode *card = axis_web_storage_card("Measure samples on a worker");
	HtmxNode *form;
	HtmxDiv *field;
	HtmxLabel *label;
	HtmxSelect *select;
	guint i;

	htmx_element_set_id(HTMX_ELEMENT(card), "worker-sample");
	axis_web_notice_put(placed, card, "worker-sample");
	ADD(card, axis_web_storage_text(NULL, "The sample measurement of the Space savings page, run by a worker instead "
		"of this server: it encodes three short excerpts of a few files the profile would convert and reports only "
		"how many bytes came out. This server checks each figure against the plan and the file before keeping it. "
		"Nothing is written to any disk."));
	if (work->sample.job_id > 0) {
		g_autofree gchar *profile = NULL;
		g_autofree gchar *state_word = g_strdup(work->sample.state != NULL ? work->sample.state : "?");
		g_autofree gchar *line = NULL;

		for (i = 0; work->profiles != NULL && work->sample.target != NULL && i < work->profiles->len; i++) {
			const AxisProfileInfo *p = g_ptr_array_index(work->profiles, i);

			if (p->current != NULL && g_strcmp0(p->slug, work->sample.target) == 0)
				profile = axis_web_profiles_label(p->current);
		}
		if (profile == NULL)
			profile = g_strdup(work->sample.target != NULL ? work->sample.target : "?");
		state_word[0] = g_ascii_toupper(state_word[0]);
		line = g_strdup_printf("The latest measurement (job %" G_GINT64_FORMAT ", %s): %s%s%s.",
			work->sample.job_id, profile, state_word,
			work->sample.message != NULL ? ": " : "", work->sample.message != NULL ? work->sample.message : "");

		ADD(card, axis_web_storage_text("help", line));
	}
	form = axis_web_auth_post_form(request, "/workers/sample");
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	field = htmx_div_new();
	label = htmx_label_new_for("worker-sample-target");
	select = htmx_select_new_with_name("target");
	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(label), "Conversion profile");
	htmx_element_set_id(HTMX_ELEMENT(select), "worker-sample-target");
	for (i = 0; work->profiles != NULL && i < work->profiles->len; i++) {
		const AxisProfileInfo *profile = g_ptr_array_index(work->profiles, i);
		g_autofree gchar *name = NULL;

		if (profile->current == NULL || !profile->current->usable)
			continue;
		/* The value is the slug (fixed characters); the name is text. */
		name = axis_web_profiles_label(profile->current);
		ADD(select, htmx_option_new_with_value(profile->slug, name));
	}
	ADD(field, label);
	ADD(field, select);
	ADD(form, field);
	ADD(form, axis_web_auth_button("Measure on a worker", NULL));
	ADD(card, form);
	return card;
}

static HtmxNode *
create_card(
	HtmxRequest   *request,
	AxisWebNotice *placed
){
	HtmxNode *card = axis_web_storage_card("Add a worker");
	HtmxNode *form = axis_web_auth_post_form(request, "/workers");

	htmx_element_set_id(HTMX_ELEMENT(card), "worker-add");
	axis_web_notice_put(placed, card, "worker-add");
	ADD(card, axis_web_storage_text(NULL, "A worker is the axis-worker program, on this machine or another, doing "
		"work this server gives it. Name it here and you are shown its token once: give the token to the worker "
		"(a file it reads, never its command line). A worker can do nothing but ask for work through its own "
		"part of the API, and is only given work on a disk after it has read that disk's marker where it is "
		"told the disk is."));
	htmx_element_add_class(HTMX_ELEMENT(form), "inline-form");
	ADD(form, axis_web_storage_text_field("worker-name", "Name", "name", NULL, "garage-pc",
		"Lower-case letters, digits and '-'."));
	ADD(form, axis_web_auth_button("Add the worker", NULL));
	ADD(card, form);
	return card;
}

static HtmxResponse *
page_done(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	gpointer        data
){
	PageWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	const gchar *message;
	gboolean is_error = FALSE;
	AxisWebNotice placed;
	guint i;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	ADD(content, heading(1, "Conversion workers"));
	{
		AxisConversionChecksWhere where = self->conversions != NULL ?
			axis_conversions_checks_where(self->conversions) : AXIS_CONVERSION_CHECKS_SERVER;
		g_autofree gchar *intro = g_strdup_printf("A worker is a separate program, on this machine or on another one "
			"with a better CPU or GPU. %s None is needed until conversions are used. Here you add a worker (and get "
			"the token it logs in with), see what each can encode and what it is doing, have it measure again, or "
			"revoke it.", axis_conversion_checks_where_sentence(where));
		g_autofree gchar *mode = g_strdup_printf("Where converted files are checked: %s (Settings, \"Where converted "
			"files are checked\"). Choosing a worker trusts it with the figures it measures; this server still "
			"decides every check.", axis_conversion_checks_where_label(where));

		ADD(content, axis_web_page_intro(intro));
		ADD(content, axis_web_storage_text("help", mode));
	}
	{
		HtmxNode *paused = axis_web_conversions_paused_notice(request, self->conversions);
		HtmxNode *unmarked = axis_web_storage_unmarked_notice(work->unmarked);

		if (paused != NULL)
			ADD(content, paused);
		if (unmarked != NULL)
			ADD(content, unmarked);
	}
	message = done_message(work->done, &is_error);
	axis_web_notice_begin(&placed, request, is_error, g_strdup(message), HTMX_NODE(content));
	if (work->workers->len == 0)
		ADD(content, axis_web_storage_text("help", "No worker has been added yet. Add one in Add a worker below."));
	for (i = 0; i < work->workers->len; i++)
		ADD(content, worker_card(self, request, g_ptr_array_index(work->workers, i), &placed));
	ADD(content, create_card(request, &placed));
	if (self->estimator != NULL)
		ADD(content, sample_card(request, work, &placed));
	axis_web_notice_end(&placed);
	return axis_web_page_respond(request, "Conversion workers", HTMX_NODE(content));
}

static HtmxResponse *
handle_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	PageWork *work = g_new0(PageWork, 1);
	const gchar *done = htmx_request_get_query_param(request, "done");
	gboolean is_error = FALSE;

	/* Only a known key is kept: nothing from the address is shown. */
	if (done_message(done, &is_error) != NULL)
		work->done = g_strdup(done);
	return dispatch(user_data, request, page_run, page_done, work, page_work_free);
}

/* ── Creating: the token, shown once ────────────────────────────── */

typedef struct
{
	gchar  *name;
	gchar  *by;
	gchar  *token;	/* secret */
	GError *error;
} CreateWork;

static void
create_work_free(gpointer data)
{
	CreateWork *work = data;

	g_free(work->name);
	g_free(work->by);
	axis_secret_free(work->token);
	g_clear_error(&work->error);
	g_free(work);
}

static void
create_run(
	AxisWebWorkers *self,
	gpointer        data
){
	CreateWork *work = data;
	gint64 id = 0;

	axis_worker_store_create(self->pool, self->tokens, work->name, work->by, &work->token, &id, &work->error);
}

static HtmxResponse *
create_done(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	gpointer        data
){
	CreateWork *work = data;
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autofree gchar *title = NULL;
	g_autofree gchar *name = NULL;
	HtmxPre *pre;
	HtmxCode *code;
	HtmxNode *card;

	if (g_error_matches(work->error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_INVALID))
		return axis_web_place_redirect(request, "/workers", "invalid-name", "worker-add");
	if (g_error_matches(work->error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_EXISTS))
		return axis_web_place_redirect(request, "/workers", "exists", "worker-add");
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	name = g_ascii_strdown(g_strstrip(work->name), -1);
	axis_web_storage_audit(request, "worker %s created", name);
	title = g_strdup_printf("Worker %s", name);
	ADD(content, heading(1, title));
	ADD(content, axis_web_auth_notice(FALSE, "The worker has been added."));
	card = axis_web_storage_card("Token for this worker");
	ADD(card, axis_web_storage_text("notice notice-warning", "This is the only time this token is shown. Copy it "
		"now: only a hash of it is kept, so it cannot be shown again. If it is lost, revoke the worker and add "
		"another."));
	pre = htmx_pre_new();
	code = htmx_code_new_with_text(work->token);
	htmx_element_add_class(HTMX_ELEMENT(pre), "worker-token");
	ADD(pre, code);
	ADD(card, pre);
	ADD(card, axis_web_storage_text(NULL, "Put it in a file only the worker's user can read (or a container "
		"secret), and point the worker at it with token_file in its configuration or AXIS_WORKER_TOKEN_FILE in "
		"its environment. Never put it on the worker's command line, where every user of that machine can see "
		"it."));
	ADD(content, card);
	ADD(content, axis_web_browser_link("/workers", "Back to Conversion workers"));
	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static HtmxResponse *
handle_create(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *name = htmx_request_get_form_value(request, "name");
	CreateWork *work;

	if (name == NULL || strlen(name) > 4 * AXIS_WORKER_NAME_MAX)
		return axis_web_place_redirect(request, "/workers", "invalid-name", "worker-add");
	work = g_new0(CreateWork, 1);
	work->name = g_strdup(name);
	work->by = g_strdup(axis_web_request_get_username(request));
	return dispatch(user_data, request, create_run, create_done, work, create_work_free);
}

/* ── Revoking, measuring again, a sample measurement ────────────── */

typedef enum
{
	ACTION_REVOKE,
	ACTION_MEASURE,
	ACTION_SAMPLE,
	ACTION_RESUME
} ActionId;

typedef struct
{
	ActionId  action;
	gchar    *name;
	gchar    *by;
	gboolean  changed;
	gint64    job_id;
	GError   *error;
} ActionWork;

static void
action_work_free(gpointer data)
{
	ActionWork *work = data;

	g_free(work->name);
	g_free(work->by);
	g_clear_error(&work->error);
	g_free(work);
}

static void
action_run(
	AxisWebWorkers *self,
	gpointer        data
){
	ActionWork *work = data;

	switch (work->action) {
	case ACTION_REVOKE:
		axis_worker_store_revoke(self->pool, self->tokens, work->name, work->by, &work->changed, &work->error);
		break;
	case ACTION_MEASURE:
		axis_worker_store_request_measure(self->pool, work->name, &work->job_id, &work->changed, &work->error);
		break;
	case ACTION_RESUME:
		axis_worker_store_resume(self->pool, work->name, work->by, &work->changed, &work->error);
		break;
	default:
		if (self->estimator == NULL)
			g_set_error_literal(&work->error, AXIS_ESTIMATE_ERROR, AXIS_ESTIMATE_ERROR_FAILED, "no estimator");
		else
			axis_estimator_request_remote_sample(self->estimator, work->name, work->by, &work->job_id,
				&work->changed, &work->error);
		break;
	}
}

static HtmxResponse *
action_done(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	gpointer        data
){
	ActionWork *work = data;
	/* (A worker's name is letters, digits and '-' (axis_worker_name_is_valid()):
	 * always a place's id.) */
	g_autofree gchar *card = g_strdup_printf("worker-%s", work->name);
	const gchar *place = work->action == ACTION_SAMPLE ? "worker-sample" : card;

	if (g_error_matches(work->error, AXIS_WORKER_STORE_ERROR, AXIS_WORKER_STORE_ERROR_NOT_FOUND))
		return axis_web_place_redirect(request, "/workers", "no-worker", place);
	if (work->action == ACTION_SAMPLE && g_error_matches(work->error, AXIS_ESTIMATE_ERROR,
	        AXIS_ESTIMATE_ERROR_NOT_FOUND))
		return axis_web_place_redirect(request, "/workers", "nothing-to-measure", place);
	if (work->action == ACTION_SAMPLE && g_error_matches(work->error, AXIS_ESTIMATE_ERROR,
	        AXIS_ESTIMATE_ERROR_FAILED))
		return axis_web_place_redirect(request, "/workers", "not-usable", place);
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	switch (work->action) {
	case ACTION_REVOKE:
		axis_web_storage_audit(request, "worker %s revoked: %s", work->name, work->changed ? "yes" : "already");
		return axis_web_place_redirect(request, "/workers", work->changed ? "revoked" : "already-revoked", place);
	case ACTION_MEASURE:
		axis_web_storage_audit(request, "worker %s asked to measure again: job=%" G_GINT64_FORMAT, work->name,
			work->job_id);
		return axis_web_place_redirect(request, "/workers", work->changed ? "measure-requested" :
			"measure-queued", place);
	case ACTION_RESUME:
		axis_web_storage_audit(request, "worker %s resumed: %s", work->name, work->changed ? "yes" : "was not paused");
		return axis_web_place_redirect(request, "/workers", work->changed ? "resumed" : "not-paused", place);
	default:
		axis_web_storage_audit(request, "sample measurement on a worker requested: target=%s job=%" G_GINT64_FORMAT,
			work->name, work->job_id);
		return axis_web_place_redirect(request, "/workers", work->changed ? "sample-requested" :
			"sample-exists", place);
	}
}

static HtmxResponse *
action_start(
	AxisWebWorkers *self,
	HtmxRequest    *request,
	ActionId        action,
	const gchar    *name
){
	ActionWork *work = g_new0(ActionWork, 1);

	work->action = action;
	work->name = g_strdup(name);
	work->by = g_strdup(axis_web_request_get_username(request));
	return dispatch(self, request, action_run, action_done, work, action_work_free);
}

static HtmxResponse *
handle_revoke(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *name = params != NULL ? g_hash_table_lookup(params, "name") : NULL;

	if (!axis_worker_name_is_valid(name))
		return axis_web_error_respond(request, 404, "Not found", "There is no such worker.");
	if (!axis_web_storage_form_yes(request, "confirm"))
		return axis_web_error_respond(request, 400, "Not confirmed", "Tick the box to revoke the worker.");
	return action_start(user_data, request, ACTION_REVOKE, name);
}

static HtmxResponse *
handle_measure(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *name = params != NULL ? g_hash_table_lookup(params, "name") : NULL;

	if (!axis_worker_name_is_valid(name))
		return axis_web_error_respond(request, 404, "Not found", "There is no such worker.");
	return action_start(user_data, request, ACTION_MEASURE, name);
}

static HtmxResponse *
handle_resume(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *name = params != NULL ? g_hash_table_lookup(params, "name") : NULL;

	if (!axis_worker_name_is_valid(name))
		return axis_web_error_respond(request, 404, "Not found", "There is no such worker.");
	return action_start(user_data, request, ACTION_RESUME, name);
}

static HtmxResponse *
handle_sample(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	const gchar *target = htmx_request_get_form_value(request, "target");

	if (!axis_profile_slug_valid(target))
		return axis_web_error_respond(request, 400, "Bad request", "Choose one of the profiles.");
	return action_start(user_data, request, ACTION_SAMPLE, target);
}

/* ── What needs attention: the workers ──────────────────────────── */

static void
worker_item(
	GPtrArray          *items,
	const gchar        *kind,
	AxisAttentionLevel  level,
	gint64              n,
	gint64              newest,
	gchar              *text	/* taken */
){
	AxisAttentionItem *item;

	if (n > 0) {
		item = axis_attention_item_new(kind, "", level, (guint64)n, newest);
		axis_attention_item_say(item, text, NULL, NULL);
		axis_attention_item_link(item, "/workers", "Open Conversion workers");
		g_ptr_array_add(items, item);
	}
	g_free(text);
}

/*
 * The workers' notices (add-attention-notices, slice N2), from one statement
 * over the handful of rows of worker; a revoked worker has none.
 *
 *   worker.paused   paused after its encoder crashed: only a person's Resume
 *                   ends it, so it is a notice at once ("Waiting for you").
 *   worker.refused  refused because it speaks another version of the
 *                   workers' protocol: it ends when a matching worker
 *                   registers, which a person brings about; at once.
 *   worker.silent   not heard from, by the Workers page's own rule (the
 *                   "Not heard from" badge), for AXIS_ATTENTION_WORKER_WAIT_S
 *                   longer: a worker that restarts is back within the minute.
 *                   A worker never seen has none, nor one that is refused
 *                   (that is its notice: a refused worker is not "seen"), nor
 *                   one silent for more than AXIS_ATTENTION_WORKER_GONE_S,
 *                   which is taken as switched off for good.  The sentence
 *                   names the worker when there is one and says for how long.
 *
 * While the daemon was down no worker could be heard, so every worker looks
 * silent when it starts.  Until this daemon has been up for the wait, whether
 * a worker is silent is not known (axis_attention_not_known()): no notice is
 * made, and none that was dismissed before the start is ended.
 *
 * "newest" is the second the latest of them was paused, refused or last
 * heard from: a worker that came back and went again is a new occurrence.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebWorkers *self = user_data;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *sql = NULL;
	gint64 connected_ms = MAX((gint64)CONNECTED_MS, 3 * (gint64)self->heartbeat_ms + 10000);
	gint64 now_s = reading->now_us / G_USEC_PER_SEC;
	gint64 silent_before = now_s - connected_ms / 1000 - AXIS_ATTENTION_WORKER_WAIT_S;
	gint64 gone_before = now_s - AXIS_ATTENTION_WORKER_GONE_S;
	gint64 paused = 0;
	gint64 refused = 0;
	gint64 silent = 0;
	gint64 paused_at = 0;
	gint64 refused_at = 0;
	gint64 silent_at = 0;
	const gchar *silent_name;

#define SILENT "paused_at IS NULL AND refused_protocol IS NULL AND last_seen_at IS NOT NULL" \
	" AND last_seen_at < to_timestamp(%" G_GINT64_FORMAT ") AND last_seen_at >= to_timestamp(%" G_GINT64_FORMAT ")"
#define SECOND(of) "coalesce(floor(extract(epoch FROM " of "))::bigint, 0)"
	sql = g_strdup_printf("SELECT count(*) FILTER (WHERE paused_at IS NOT NULL),"
		" count(*) FILTER (WHERE refused_protocol IS NOT NULL), count(*) FILTER (WHERE " SILENT "),"
		" " SECOND("max(paused_at)") ", " SECOND("max(refused_protocol_at)") ","
		" " SECOND("max(last_seen_at) FILTER (WHERE " SILENT ")") ", min(name) FILTER (WHERE " SILENT ")"
		" FROM worker WHERE revoked_at IS NULL", silent_before, gone_before, silent_before, gone_before, silent_before,
		gone_before);
#undef SILENT
#undef SECOND
	if (!axis_db_pool_query_bounded(self->pool, sql, reading->deadline_ms, &result, error))
		return FALSE;
	axis_db_result_get_int64(result, 0, 0, &paused);
	axis_db_result_get_int64(result, 0, 1, &refused);
	axis_db_result_get_int64(result, 0, 2, &silent);
	axis_db_result_get_int64(result, 0, 3, &paused_at);
	axis_db_result_get_int64(result, 0, 4, &refused_at);
	axis_db_result_get_int64(result, 0, 5, &silent_at);
	silent_name = axis_db_result_get_text(result, 0, 6);

	worker_item(items, "worker.paused", AXIS_ATTENTION_WAITING, paused, paused_at, paused == 1 ?
		g_strdup("1 worker is paused after its encoder crashed: it converts nothing until you resume it.") :
		g_strdup_printf("%" G_GINT64_FORMAT " workers are paused after their encoders crashed: they convert nothing "
			"until you resume them.", paused));
	worker_item(items, "worker.refused", AXIS_ATTENTION_LOOK, refused, refused_at, refused == 1 ?
		g_strdup("1 worker was refused: it runs another version of Axis than this server. Update it to the same "
			"release.") :
		g_strdup_printf("%" G_GINT64_FORMAT " workers were refused: they run another version of Axis than this "
			"server. Update them to the same release.", refused));
	/* Up for less than the wait: no worker has had the time to be silent
	 * to this daemon. */
	if (reading->now_us - self->started_us < (gint64)AXIS_ATTENTION_WORKER_WAIT_S * G_USEC_PER_SEC) {
		axis_attention_not_known(items, "worker.silent", NULL);
		return TRUE;
	}
	if (silent > 0) {
		/* The latest heard from among them: the others have been silent for
		 * longer. */
		g_autofree gchar *how_long = axis_attention_duration(reading->now_us - silent_at * G_USEC_PER_SEC);
		AxisAttentionItem *item = axis_attention_item_new("worker.silent", "", AXIS_ATTENTION_LOOK, (guint64)silent,
			silent_at);

		if (silent == 1 && silent_name != NULL) {
			g_autofree gchar *tail = g_strdup_printf(" has not been heard from for %s.", how_long);

			axis_attention_item_say(item, "The worker ", silent_name, tail);
		} else {
			g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT " workers have not been heard from for %s or "
				"longer.", silent, how_long);

			axis_attention_item_say(item, text, NULL, NULL);
		}
		axis_attention_item_link(item, "/workers", "Open Conversion workers");
		item->since_us = silent_at * G_USEC_PER_SEC;
		g_ptr_array_add(items, item);
	}
	return TRUE;
}

/* ── The module ─────────────────────────────────────────────────── */

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_attention_source(web, "the workers", attention_source, user_data);
	axis_web_route_full(web, HTMX_METHOD_GET, "/workers", AXIS_WEB_ROUTE_ADMIN, handle_page, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers", AXIS_WEB_ROUTE_ADMIN, handle_create, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/sample", AXIS_WEB_ROUTE_ADMIN, handle_sample, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/:name/revoke", AXIS_WEB_ROUTE_ADMIN, handle_revoke,
		user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/:name/measure", AXIS_WEB_ROUTE_ADMIN, handle_measure,
		user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/workers/:name/resume", AXIS_WEB_ROUTE_ADMIN, handle_resume,
		user_data);
}

static const AxisNavEntry nav_entries[] = {
	{ "Conversion workers", "/workers", 50, AXIS_NAV_GROUP_MANAGEMENT, TRUE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_workers_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_WORKERS_MODULE_ID;
		info.name = "Conversion workers";
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "The axis-worker programs, on this machine or others, that do the encoding, each "
			"with its own token. A worker is given work only on disks whose marker file it has read.";
		info.flags = AXIS_MODULE_REQUIRED;
	}
	return &info;
}

AxisWebWorkers *
axis_web_workers_new(
	AxisDbPool       *pool,
	AxisWorkerTokens *tokens,
	AxisEstimator    *estimator,
	guint             heartbeat_ms
){
	AxisWebWorkers *self;

	g_return_val_if_fail(pool != NULL && tokens != NULL, NULL);

	self = g_new0(AxisWebWorkers, 1);
	self->pool = pool;
	self->tokens = tokens;
	self->estimator = estimator != NULL ? axis_estimator_ref(estimator) : NULL;
	self->heartbeat_ms = heartbeat_ms;
	self->started_us = g_get_real_time();
	return self;
}

void
axis_web_workers_stop(AxisWebWorkers *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
}

void
axis_web_workers_free(AxisWebWorkers *self)
{
	if (self == NULL)
		return;
	axis_web_workers_stop(self);
	if (self->estimator != NULL)
		axis_estimator_unref(self->estimator);
	if (self->kinds != NULL)
		axis_job_registry_unref(self->kinds);
	if (self->conversions != NULL)
		axis_conversions_unref(self->conversions);
	g_free(self);
}

void
axis_web_workers_set_conversions(
	AxisWebWorkers  *self,
	AxisConversions *conversions
){
	g_return_if_fail(self != NULL);

	if (conversions != NULL)
		axis_conversions_ref(conversions);
	if (self->conversions != NULL)
		axis_conversions_unref(self->conversions);
	self->conversions = conversions;
}

void
axis_web_workers_set_kinds(
	AxisWebWorkers  *self,
	AxisJobRegistry *kinds
){
	g_return_if_fail(self != NULL);

	if (kinds != NULL)
		axis_job_registry_ref(kinds);
	if (self->kinds != NULL)
		axis_job_registry_unref(self->kinds);
	self->kinds = kinds;
}
