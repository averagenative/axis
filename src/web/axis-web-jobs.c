/*
 * axis-web-jobs.c - the Jobs pages: routes, database work and actions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A request reads the database on a worker thread (a pooled connection is
 * acquired and released inside one call on that thread) and the page is
 * built on the main loop from the plain rows that come back.  Cancel and
 * retry are done on a worker as well; the queue's functions block.
 */

#include "web/axis-web-jobs-private.h"
#include "web/axis-web-auth-private.h"

#include <string.h>

#include "jobs/axis-job-error.h"

/* Tasks waiting for a thread beyond this are answered 503. */
#define MAX_PENDING_TASKS 32

/* ── Tasks off the main loop ────────────────────────────────────── */

typedef struct
{
	AxisWebJobs      *jobs;
	AxisJobsWorkFunc  work;
	AxisJobsDoneFunc  done;
	gpointer          data;
	GDestroyNotify    free_data;
} Task;

static void
task_free(Task *task)
{
	if (task->free_data != NULL)
		task->free_data(task->data);
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

	task->work(task->jobs, task->data);
	g_task_return_boolean(gtask, TRUE);
}

static void
task_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Task *task = user_data;

	task->jobs->pending--;
	task->done(task->jobs, task->data);
	task_free(task);
}

gboolean
axis_web_jobs_run(
	AxisWebJobs      *jobs,
	AxisJobsWorkFunc  work,
	AxisJobsDoneFunc  done,
	gpointer          data,
	GDestroyNotify    free_data
){
	Task *task;
	GTask *gtask;

	if (jobs->stopping || jobs->pending >= MAX_PENDING_TASKS) {
		if (free_data != NULL)
			free_data(data);
		return FALSE;
	}

	task = g_new0(Task, 1);
	task->jobs = jobs;
	task->work = work;
	task->done = done;
	task->data = data;
	task->free_data = free_data;

	jobs->pending++;
	gtask = g_task_new(NULL, NULL, task_finished, task);
	g_task_set_task_data(gtask, task, NULL);
	g_task_run_in_thread(gtask, task_thread);
	g_object_unref(gtask);

	return TRUE;
}

/* A request answered once its task is done. */
typedef struct
{
	AxisWebJobs  *jobs;
	HtmxRequest  *request;
	HtmxResponse *deferred;
	gpointer      data;
	GDestroyNotify free_data;
	void        (*work)(AxisWebJobs *jobs, gpointer data);
	HtmxResponse *(*build)(AxisWebJobs *jobs, HtmxRequest *request, gpointer data);
} Job;

static void
job_work(
	AxisWebJobs *jobs,
	gpointer     data
){
	Job *job = data;

	job->work(jobs, job->data);
}

static void
job_done(
	AxisWebJobs *jobs,
	gpointer     data
){
	Job *job = data;
	HtmxResponse *response = job->build(jobs, job->request, job->data);

	htmx_response_complete(job->deferred, response);
	g_object_unref(response);
}

static void
job_free(gpointer data)
{
	Job *job = data;

	if (job->free_data != NULL)
		job->free_data(job->data);
	g_object_unref(job->request);
	g_clear_object(&job->deferred);
	g_free(job);
}

static HtmxResponse *
busy(HtmxRequest *request)
{
	HtmxResponse *response = axis_web_error_respond(request, 503, "Busy",
		"The server is busy. Try again in a few seconds.");

	htmx_response_add_header(response, "Retry-After", "5");
	return response;
}

/*
 * Runs @work(jobs, @data) on a worker thread, then answers @request with
 * @build(jobs, request, @data) on the main loop.  Returns the deferred
 * response for the handler to return.
 */
static HtmxResponse *
dispatch(
	AxisWebJobs    *jobs,
	HtmxRequest    *request,
	void          (*work)(AxisWebJobs *jobs, gpointer data),
	HtmxResponse *(*build)(AxisWebJobs *jobs, HtmxRequest *request, gpointer data),
	gpointer        data,
	GDestroyNotify  free_data
){
	Job *job = g_new0(Job, 1);

	job->jobs = jobs;
	job->request = g_object_ref(request);
	job->data = data;
	job->free_data = free_data;
	job->work = work;
	job->build = build;

	/* Refused: job_free() has run, and there is no placeholder to
	 * answer (one finalized unanswered would be a 500). */
	if (!axis_web_jobs_run(jobs, job_work, job_done, job, job_free))
		return busy(request);

	/* The task finishes on the main loop, which is not running yet. */
	job->deferred = htmx_response_new_deferred();

	return g_object_ref(job->deferred);
}

HtmxResponse *
axis_web_jobs_dispatch(
	AxisWebJobs    *jobs,
	HtmxRequest    *request,
	void          (*work)(AxisWebJobs *jobs, gpointer data),
	HtmxResponse *(*build)(AxisWebJobs *jobs, HtmxRequest *request, gpointer data),
	gpointer        data,
	GDestroyNotify  free_data
){
	return dispatch(jobs, request, work, build, data, free_data);
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* The :id route parameter as a job id, or 0. */
static gint64
param_id(GHashTable *params)
{
	const gchar *text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	guint64 value = 0;
	const gchar *p;

	if (text == NULL || text[0] == '\0' || strlen(text) > 18 || text[0] == '0')
		return 0;
	for (p = text; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return 0;
		value = value * 10 + (guint64)(*p - '0');
	}

	return (gint64)value;
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is nothing at this address.");
}

static HtmxResponse *
fragment_response(HtmxNode *node)
{
	g_autoptr(HtmxResponse) response = htmx_response_new();
	g_autofree gchar *markup = NULL;

	axis_web_table_apply_roles(node);
	markup = htmx_element_render(HTMX_ELEMENT(node));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");

	return (HtmxResponse *)g_steal_pointer(&response);
}

/* A copy of a request's query parameter, or %NULL. */
static gchar *
query_param(
	HtmxRequest *request,
	const gchar *name
){
	const gchar *value = htmx_request_get_query_param(request, name);

	return value != NULL ? g_strdup(value) : NULL;
}

/* ── List ───────────────────────────────────────────────────────── */

/*
 * The list, in one of three shapes.  Work and upkeep (no class asked for,
 * the first page): the work, paged, and below it the upkeep under its own
 * heading, closed, its newest few with a link to the rest.  Work only or
 * upkeep only: that list, paged.  A refresh of the brief upkeep list
 * (brief=1) also brings its heading's counts up to date.
 */
typedef struct
{
	gchar     *state;
	gchar     *queue;
	gchar     *cursor;
	gchar     *job_class;
	gboolean   brief;	/* the upkeep under its heading */
	GPtrArray *rows;
	gchar     *next;
	GPtrArray *upkeep_rows;	/* the brief list, for work and upkeep */
	gchar     *upkeep_next;
	gint64     upkeep_running;
	gint64     upkeep_queued;
	GStrv      queues;
	GError    *error;
	gboolean   admin;
	gboolean   fragment;	/* only the list, for an htmx refresh */
} ListWork;

static void
list_work_free(gpointer data)
{
	ListWork *work = data;

	g_free(work->state);
	g_free(work->queue);
	g_free(work->cursor);
	g_free(work->job_class);
	g_clear_pointer(&work->rows, g_ptr_array_unref);
	g_free(work->next);
	g_clear_pointer(&work->upkeep_rows, g_ptr_array_unref);
	g_free(work->upkeep_next);
	g_strfreev(work->queues);
	g_clear_error(&work->error);
	g_free(work);
}

/* Both classes, shown as work with the upkeep below it. */
static gboolean
list_both(const ListWork *work)
{
	return work->job_class == NULL || work->job_class[0] == '\0';
}

static void
list_filter(
	const ListWork *work,
	AxisJobFilter  *filter
){
	memset(filter, 0, sizeof(*filter));
	filter->state = work->state;
	filter->queue = work->queue;
	filter->cursor = work->cursor;
	filter->job_class = list_both(work) ? "work" : work->job_class;
	if (work->brief) {
		filter->job_class = "upkeep";
		filter->cursor = NULL;
		filter->limit = AXIS_JOBS_BRIEF_SIZE;
	}
}

static void
list_work_run(
	AxisWebJobs *jobs,
	gpointer     data
){
	ListWork *work = data;
	AxisJobFilter filter;

	list_filter(work, &filter);
	if (!axis_web_jobs_list(jobs->pool, jobs->kinds, &filter, &work->rows, &work->next,
	                        work->fragment ? NULL : &work->queues, &work->error))
		return;
	if (work->brief) {
		axis_web_jobs_count_active(jobs->pool, jobs->kinds, "upkeep", &work->upkeep_running,
			&work->upkeep_queued, &work->error);
		return;
	}
	/* The upkeep under its heading, on the first page of both. */
	if (!work->fragment && list_both(work) && work->cursor == NULL) {
		filter.job_class = "upkeep";
		filter.limit = AXIS_JOBS_BRIEF_SIZE;
		if (axis_web_jobs_list(jobs->pool, jobs->kinds, &filter, &work->upkeep_rows, &work->upkeep_next, NULL,
		                       &work->error))
			axis_web_jobs_count_active(jobs->pool, jobs->kinds, "upkeep", &work->upkeep_running,
				&work->upkeep_queued, &work->error);
	}
}

/* The brief upkeep list, and its heading's counts swapped in beside it. */
static HtmxResponse *
brief_response(
	ListWork            *work,
	const AxisJobFilter *filter
){
	g_autoptr(HtmxFragment) fragment = htmx_fragment_new();
	g_autoptr(HtmxResponse) response = htmx_response_new();
	g_autoptr(HtmxNode) list = axis_web_jobs_list_new(filter, work->rows, work->next, work->admin, TRUE);
	g_autoptr(HtmxNode) counts = axis_web_jobs_upkeep_counts_new(work->upkeep_running, work->upkeep_queued);
	g_autofree gchar *markup = NULL;

	htmx_element_set_attribute(HTMX_ELEMENT(counts), "hx-swap-oob", "true");
	htmx_node_add_child(HTMX_NODE(fragment), list);
	htmx_node_add_child(HTMX_NODE(fragment), counts);
	axis_web_table_apply_roles(HTMX_NODE(fragment));
	markup = htmx_element_render(HTMX_ELEMENT(fragment));
	htmx_response_set_content(response, markup);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");

	return (HtmxResponse *)g_steal_pointer(&response);
}

static HtmxResponse *
list_build(
	AxisWebJobs *jobs,
	HtmxRequest *request,
	gpointer     data
){
	ListWork *work = data;
	AxisJobFilter filter;
	AxisJobFilter shown;
	g_autoptr(HtmxDiv) content = NULL;
	g_autoptr(HtmxHeading) title = NULL;
	g_autoptr(HtmxSection) filters = NULL;
	g_autoptr(HtmxNode) list = NULL;
	g_autoptr(HtmxNode) form = NULL;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	list_filter(work, &filter);
	if (work->brief)
		return brief_response(work, &filter);
	list = axis_web_jobs_list_new(&filter, work->rows, work->next, work->admin, FALSE);

	if (work->fragment)
		return fragment_response(list);

	content = htmx_div_new();
	title = htmx_h1_new();
	filters = htmx_section_new();
	/* The form shows what was asked for, both classes included. */
	shown = filter;
	shown.job_class = work->job_class;
	form = axis_web_jobs_filter_form_new(&shown, (const gchar *const *)work->queues);

	htmx_node_set_text_content(HTMX_NODE(title), "Jobs");
	htmx_element_add_class(HTMX_ELEMENT(filters), "card");
	htmx_node_add_child(HTMX_NODE(filters), form);
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(filters));
	if (list_both(work)) {
		HtmxHeading *heading = htmx_h2_new();
		HtmxP *intro = htmx_p_new();

		htmx_node_set_text_content(HTMX_NODE(heading), "Work jobs");
		htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(heading));
		g_object_unref(heading);
		htmx_element_add_class(HTMX_ELEMENT(intro), "help");
		htmx_node_set_text_content(HTMX_NODE(intro), "Searches, imports, scans, conversions and the other work "
			"Axis does for a movie, a series, a file or a folder.");
		htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(intro));
		g_object_unref(intro);
	}
	htmx_node_add_child(HTMX_NODE(content), list);
	if (work->upkeep_rows != NULL) {
		g_autoptr(HtmxNode) upkeep = axis_web_jobs_upkeep_new(&filter, work->upkeep_rows, work->upkeep_next,
			work->admin, work->upkeep_running, work->upkeep_queued);

		htmx_node_add_child(HTMX_NODE(content), upkeep);
	}

	axis_web_page_add_topic(request, work->admin ? AXIS_JOBS_TOPIC_LIST_ADMIN : AXIS_JOBS_TOPIC_LIST);

	return axis_web_page_respond(request, "Jobs", HTMX_NODE(content));
}

static HtmxResponse *
handle_list_common(
	HtmxRequest *request,
	AxisWebJobs *jobs,
	gboolean     fragment
){
	ListWork *work;
	gchar *state = query_param(request, "state");
	gchar *queue = query_param(request, "queue");
	gchar *cursor = query_param(request, "before");
	gchar *job_class = query_param(request, "class");
	gboolean brief = fragment && g_strcmp0(htmx_request_get_query_param(request, "brief"), "1") == 0;

	if (!axis_web_jobs_filter_state_is_valid(state) ||
	    (queue != NULL && queue[0] != '\0' && !axis_job_name_is_valid(queue)) ||
	    (cursor != NULL && !axis_web_jobs_cursor_is_valid(cursor)) ||
	    !axis_web_jobs_filter_class_is_valid(job_class)) {
		g_free(state);
		g_free(queue);
		g_free(cursor);
		g_free(job_class);
		return axis_web_error_respond(request, 400, "Filter not valid",
			"This filter cannot be used. Choose the filters again on the Jobs page.");
	}

	/* Nothing to refresh without htmx: send the browser to the page. */
	if (fragment && !htmx_request_is_htmx(request)) {
		const gchar *query = htmx_request_get_query(request);
		g_autofree gchar *location = query != NULL ? g_strconcat("/jobs?", query, NULL) :
			g_strdup("/jobs");

		g_free(state);
		g_free(queue);
		g_free(cursor);
		g_free(job_class);
		return axis_web_auth_redirect(request, location);
	}

	work = g_new0(ListWork, 1);
	work->state = state;
	work->queue = queue;
	work->cursor = cursor;
	work->job_class = job_class;
	work->brief = brief;
	work->admin = axis_web_request_is_admin(request);
	work->fragment = fragment;

	return dispatch(jobs, request, list_work_run, list_build, work, list_work_free);
}

static HtmxResponse *
handle_list(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_list_common(request, user_data, FALSE);
}

static HtmxResponse *
handle_list_fragment(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_list_common(request, user_data, TRUE);
}

/* ── Detail and actions ─────────────────────────────────────────── */

typedef enum
{
	WORK_VIEW,
	WORK_VIEW_FRAGMENT,	/* only the detail, for an htmx refresh */
	WORK_CANCEL,
	WORK_RETRY
} WorkKind;

typedef struct
{
	WorkKind    kind;
	gint64      id;
	gboolean    admin;
	gchar      *by;		/* who asked, for a retry */
	AxisJobRow *row;
	guint       status;		/* nonzero: answer with this instead */
	const gchar *status_text;
	gchar      *status_owned;	/* a status text made for this answer */
	GError     *error;
} JobWork;

static void
job_work_free(gpointer data)
{
	JobWork *work = data;

	axis_job_row_free(work->row);
	g_free(work->by);
	g_free(work->status_owned);
	g_clear_error(&work->error);
	g_free(work);
}

static void
refuse(
	JobWork     *work,
	guint        status,
	gchar       *text
){
	work->status = status;
	g_free(work->status_owned);
	work->status_owned = text;
	work->status_text = text;
}

static void
do_cancel(
	AxisWebJobs *jobs,
	JobWork     *work
){
	AxisJobCancelOutcome outcome;

	if (jobs->queue == NULL) {
		work->status = 503;
		work->status_text = "Jobs cannot be cancelled while the job queue is starting or stopping. Try again in a moment.";
		return;
	}

	if (!axis_job_queue_cancel(jobs->queue, work->id, &outcome, &work->error)) {
		if (g_error_matches(work->error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND)) {
			g_clear_error(&work->error);
			work->status = 404;
		}
		return;
	}
}

/*
 * A retry is the same piece of work: the job itself queued again, with its
 * key, priority and payload (axis_job_queue_retry()), never a copy that
 * whoever looks for the work by its key or its job would not see.  Refused
 * while another job of its key does the work, and for a kind whose owner
 * asks for its work again (axis_job_registry_get_retry_elsewhere()).
 */
static void
do_retry(
	AxisWebJobs *jobs,
	JobWork     *work
){
	g_autoptr(AxisJobRow) original = NULL;
	AxisJobRetryOutcome outcome = AXIS_JOB_RETRY_NOT_ENDED;
	const gchar *elsewhere;
	gint64 live = 0;

	if (jobs->queue == NULL) {
		work->status = 503;
		work->status_text = "Jobs cannot be retried while the job queue is starting or stopping. Try again in a moment.";
		return;
	}

	if (!axis_web_jobs_get(jobs->pool, jobs->kinds, work->id, FALSE, &original, &work->error))
		return;
	if (original == NULL) {
		work->status = 404;
		return;
	}
	if (original->state != AXIS_JOB_STATE_FAILED && original->state != AXIS_JOB_STATE_CANCELLED) {
		work->status = 409;
		work->status_text = "Only a failed or cancelled job can be retried.";
		return;
	}
	elsewhere = jobs->kinds != NULL ? axis_job_registry_get_retry_elsewhere(jobs->kinds, original->kind) : NULL;
	if (elsewhere != NULL) {
		refuse(work, 409, g_strdup_printf("This job cannot be retried from the Jobs page. %s", elsewhere));
		return;
	}

	if (!axis_job_queue_retry(jobs->queue, work->id, work->by, &outcome, &live, &work->error)) {
		if (g_error_matches(work->error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_UNKNOWN_KIND)) {
			g_clear_error(&work->error);
			work->status = 409;
			work->status_text = "This version of Axis no longer has this type of job, so it cannot be retried.";
		} else if (g_error_matches(work->error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_NOT_FOUND)) {
			g_clear_error(&work->error);
			work->status = 404;
		}
		return;
	}
	if (outcome == AXIS_JOB_RETRY_LIVE && live == work->id) {
		refuse(work, 409, g_strdup("This job was retried by someone else just now, and is queued or running."));
	} else if (outcome == AXIS_JOB_RETRY_LIVE && live > 0) {
		refuse(work, 409, g_strdup_printf("Job %" G_GINT64_FORMAT " is doing this work now (it is queued or "
			"running), so nothing was retried. Open job %" G_GINT64_FORMAT " to follow it.", live, live));
	} else if (outcome == AXIS_JOB_RETRY_LIVE) {
		refuse(work, 409, g_strdup("Another job did this work just now, so nothing was retried."));
	} else if (outcome != AXIS_JOB_RETRY_DONE) {
		work->status = 409;
		work->status_text = "Only a failed or cancelled job can be retried.";
	}
}

static void
job_work_run(
	AxisWebJobs *jobs,
	gpointer     data
){
	JobWork *work = data;

	if (work->kind == WORK_CANCEL)
		do_cancel(jobs, work);
	else if (work->kind == WORK_RETRY)
		do_retry(jobs, work);
	if (work->error != NULL || work->status != 0 || work->kind == WORK_RETRY)
		return;

	axis_web_jobs_get(jobs->pool, jobs->kinds, work->id, FALSE, &work->row, &work->error);
	if (work->error == NULL && work->row == NULL)
		work->status = 404;
}

static HtmxResponse *
status_response(
	HtmxRequest *request,
	JobWork     *work
){
	if (work->status == 404)
		return not_found(request);
	if (work->status == 409)
		return axis_web_error_respond(request, 409, "Not possible", work->status_text);

	return axis_web_error_respond(request, work->status, "Busy", work->status_text);
}

static HtmxResponse *
job_build(
	AxisWebJobs *jobs,
	HtmxRequest *request,
	gpointer     data
){
	JobWork *work = data;
	g_autofree gchar *location = NULL;
	g_autoptr(HtmxNode) node = NULL;
	g_autoptr(HtmxDiv) content = NULL;
	g_autoptr(HtmxP) back = NULL;
	g_autoptr(HtmxA) link = NULL;
	g_autofree gchar *topic = NULL;
	g_autofree gchar *title = NULL;
	const gchar *target;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->status != 0)
		return status_response(request, work);

	/* The same job, queued again. */
	if (work->kind == WORK_RETRY) {
		location = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, work->id);
		return axis_web_auth_redirect(request, location);
	}

	if (work->kind == WORK_VIEW_FRAGMENT) {
		/* Nothing to refresh without htmx: send the browser to the page. */
		if (!htmx_request_is_htmx(request)) {
			location = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, work->id);
			return axis_web_auth_redirect(request, location);
		}

		node = axis_web_jobs_detail_new(work->row, work->admin);
		return fragment_response(node);
	}

	if (work->kind == WORK_CANCEL) {
		if (!htmx_request_is_htmx(request)) {
			location = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, work->id);
			return axis_web_auth_redirect(request, location);
		}

		/* The element that asked is replaced by its new self. */
		target = htmx_request_get_target_id(request);
		if (g_strcmp0(target, "job-detail") == 0)
			node = axis_web_jobs_detail_new(work->row, work->admin);
		else
			node = axis_web_jobs_row_new(work->row, work->admin);
		return fragment_response(node);
	}

	content = htmx_div_new();
	back = htmx_p_new();
	link = htmx_a_new_with_href("/jobs");
	htmx_node_set_text_content(HTMX_NODE(link), "All jobs");
	htmx_node_add_child(HTMX_NODE(back), HTMX_NODE(link));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(back));
	node = axis_web_jobs_detail_new(work->row, work->admin);
	htmx_node_add_child(HTMX_NODE(content), node);
	{
		g_autoptr(HtmxNode) earlier = axis_web_jobs_earlier_new(work->row, work->admin);

		if (earlier != NULL)
			htmx_node_add_child(HTMX_NODE(content), earlier);
	}
	if (work->admin && work->row->payload != NULL) {
		g_autoptr(HtmxNode) payload = axis_web_jobs_payload_new(work->row);

		htmx_node_add_child(HTMX_NODE(content), payload);
	}

	topic = axis_web_jobs_topic_for_job(work->id, work->admin);
	axis_web_page_add_topic(request, topic);
	title = work->row->label != NULL ? g_strdup_printf("Job %" G_GINT64_FORMAT ": %s", work->id, work->row->label) :
		g_strdup_printf("Job %" G_GINT64_FORMAT, work->id);

	return axis_web_page_respond(request, title, HTMX_NODE(content));
}

static HtmxResponse *
handle_job_common(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data,
	WorkKind     kind
){
	AxisWebJobs *jobs = user_data;
	gint64 id = param_id(params);
	JobWork *work;

	if (id == 0)
		return not_found(request);

	work = g_new0(JobWork, 1);
	work->kind = kind;
	work->id = id;
	work->admin = axis_web_request_is_admin(request);
	work->by = g_strdup(axis_web_request_get_username(request));

	return dispatch(jobs, request, job_work_run, job_build, work, job_work_free);
}

static HtmxResponse *
handle_detail(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_job_common(request, params, user_data, WORK_VIEW);
}

static HtmxResponse *
handle_detail_fragment(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_job_common(request, params, user_data, WORK_VIEW_FRAGMENT);
}

static HtmxResponse *
handle_cancel(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_job_common(request, params, user_data, WORK_CANCEL);
}

static HtmxResponse *
handle_retry(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	return handle_job_common(request, params, user_data, WORK_RETRY);
}

/* ── The dashboard: what is running ─────────────────────────────── */

/* Jobs listed on the dashboard, of each sort. */
#define DASH_RUNNING  5
#define DASH_FAILURES 3

typedef struct
{
	gint64     running;
	gint64     queued;
	GPtrArray *active;	/* AxisJobRow: running ones first */
	GPtrArray *failed;	/* AxisJobRow: newest first */
	gboolean   ok;
	gint64     taken_us;	/* when they were read, not when they are drawn */
} DashFigures;

static void
dash_figures_free(gpointer data)
{
	DashFigures *figures = data;

	g_clear_pointer(&figures->active, g_ptr_array_unref);
	g_clear_pointer(&figures->failed, g_ptr_array_unref);
	g_free(figures);
}

static gpointer
dash_collect(gpointer user_data)
{
	AxisWebJobs *jobs = user_data;
	DashFigures *figures = g_new0(DashFigures, 1);
	AxisJobFilter running;
	AxisJobFilter failed;
	g_autoptr(GError) error = NULL;

	figures->taken_us = g_get_real_time();

	/* Work only: what Axis repeats by itself is on the Jobs page. */
	memset(&running, 0, sizeof(running));
	running.state = "running";
	running.job_class = "work";
	running.limit = DASH_RUNNING;
	memset(&failed, 0, sizeof(failed));
	failed.state = "failed";
	failed.job_class = "work";
	failed.limit = DASH_FAILURES;
	figures->ok = axis_web_jobs_count_active(jobs->pool, jobs->kinds, "work", &figures->running, &figures->queued,
	                                         &error) &&
		axis_web_jobs_list(jobs->pool, jobs->kinds, &running, &figures->active, NULL, NULL, &error) &&
		axis_web_jobs_list(jobs->pool, jobs->kinds, &failed, &figures->failed, NULL, NULL, &error);
	if (!figures->ok)
		g_debug("dashboard: cannot read the jobs: %s", error->message);
	return figures;
}

static HtmxNode *
dash_figure(
	gint64       value,
	const gchar *label,
	const gchar *href
){
	HtmxA *box = htmx_a_new_with_href(href);
	HtmxSpan *number = htmx_span_new();
	HtmxSpan *text = htmx_span_new();
	g_autofree gchar *figure = g_strdup_printf("%" G_GINT64_FORMAT, value);

	htmx_element_add_class(HTMX_ELEMENT(box), "figure");
	htmx_element_add_class(HTMX_ELEMENT(number), "figure-value");
	htmx_node_set_text_content(HTMX_NODE(number), figure);
	htmx_element_add_class(HTMX_ELEMENT(text), "figure-label");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(number));
	g_object_unref(number);
	htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(text));
	g_object_unref(text);
	return HTMX_NODE(box);
}

static void
dash_add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

static HtmxNode *
dash_text(
	const gchar *css,
	const gchar *text
){
	HtmxP *line = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(line), css);
	htmx_node_set_text_content(HTMX_NODE(line), text);
	return HTMX_NODE(line);
}

/* What the job does, linking to it, with its kind beside. */
static HtmxNode *
dash_job_link(const AxisJobRow *row)
{
	return axis_web_jobs_name_new(row);
}

static void
dash_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          data,
	gpointer          user_data
){
	DashFigures *figures = data;
	HtmxDiv *piece = htmx_div_new();
	HtmxDiv *row = htmx_div_new();
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(piece), "dash-piece");
	if (figures == NULL || !figures->ok) {
		dash_add(piece, dash_text("help", "The jobs could not be read just now. Reload the page to try again."));
		axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_NOW, 0, "/jobs", HTMX_NODE(piece));
		return;
	}

	/* The same figures as the lists they link to: work only. */
	htmx_element_add_class(HTMX_ELEMENT(row), "figure-row");
	dash_add(row, dash_figure(figures->running, "jobs running", "/jobs?state=running&class=work"));
	dash_add(row, dash_figure(figures->queued, "jobs queued", "/jobs?state=queued&class=work"));
	dash_add(piece, row);

	if (figures->active->len > 0) {
		HtmxUl *list = htmx_ul_new();

		htmx_element_add_class(HTMX_ELEMENT(list), "dash-list");
		for (i = 0; i < figures->active->len && i < DASH_RUNNING; i++) {
			const AxisJobRow *job = g_ptr_array_index(figures->active, i);
			HtmxLi *item = htmx_li_new();

			dash_add(item, dash_job_link(job));
			if (job->progress >= 0) {
				HtmxProgress *bar = htmx_progress_new_with_value(job->progress, 1.0);
				g_autofree gchar *percent = g_strdup_printf("%d%%", (gint)(job->progress * 100 + 0.5));
				HtmxSpan *text = htmx_span_new();

				htmx_element_set_attribute(HTMX_ELEMENT(bar), "aria-label", "Progress");
				htmx_element_add_class(HTMX_ELEMENT(text), "job-percent");
				htmx_node_set_text_content(HTMX_NODE(text), percent);
				dash_add(item, bar);
				dash_add(item, text);
			}
			dash_add(list, item);
		}
		dash_add(piece, list);
	} else if (figures->queued == 0) {
		dash_add(piece, dash_text("all-clear", "No jobs are running or queued."));
	}

	if (figures->failed->len > 0) {
		HtmxUl *list = htmx_ul_new();

		dash_add(piece, dash_text("dash-subhead", "Recently failed jobs"));
		htmx_element_add_class(HTMX_ELEMENT(list), "dash-list");
		for (i = 0; i < figures->failed->len && i < DASH_FAILURES; i++) {
			const AxisJobRow *job = g_ptr_array_index(figures->failed, i);
			HtmxLi *item = htmx_li_new();
			HtmxSpan *when = htmx_span_new();
			g_autofree gchar *text = axis_web_format_time(job->finished_us, "");

			htmx_element_add_class(HTMX_ELEMENT(when), "as-of");
			htmx_node_set_text_content(HTMX_NODE(when), text);
			dash_add(item, dash_job_link(job));
			dash_add(item, when);
			dash_add(list, item);
		}
		dash_add(piece, list);
		{
			HtmxP *line = htmx_p_new();
			HtmxA *all = htmx_a_new_with_href("/jobs?state=failed&class=work");

			htmx_element_add_class(HTMX_ELEMENT(line), "card-foot");
			htmx_node_set_text_content(HTMX_NODE(all), "All failed work jobs");
			dash_add(line, all);
			dash_add(piece, line);
		}
	}
	dash_add(piece, axis_web_dashboard_as_of("As of", figures->taken_us));
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_NOW, 0, "/jobs", HTMX_NODE(piece));
}

/* ── Module ─────────────────────────────────────────────────────── */

static HtmxNode *
header_widget(
	HtmxRequest *request,
	gpointer     user_data
){
	AxisWebJobs *jobs = user_data;

	axis_web_page_add_topic(request, AXIS_JOBS_TOPIC_ACTIVITY);

	return axis_web_jobs_activity_new(&jobs->activity);
}

/*
 * The jobs that failed for good (add-attention-notices, slice N2): work, not
 * upkeep (an upkeep job that fails is simply run again at its next turn),
 * that used up its attempts and is still kept as failed.  The number is the
 * number of jobs its link lists (the Jobs page, failed, work): a notice that
 * said 2 over a list of 9 would be doubted.  A person retries a job there, or
 * leaves it and dismisses the notice; a failed job is removed after its
 * keeping time (jobs.keep_failed), and the notice ends with the last of
 * them.  The rest still works, so it is "Worth a look".  "newest" is the
 * highest id among them: a job that fails later is a new occurrence.
 */
static gboolean
attention_source(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisWebJobs *jobs = user_data;
	g_autofree gchar *text = NULL;
	AxisAttentionItem *item;
	gint64 n = 0;
	gint64 newest = 0;

	if (!axis_web_jobs_count_failed(jobs->pool, jobs->kinds, reading->deadline_ms, &n, &newest, error))
		return FALSE;
	if (n <= 0)
		return TRUE;
	text = n == 1 ? g_strdup("1 job failed for good and was not retried. Retry it, or leave it.") :
		g_strdup_printf("%" G_GINT64_FORMAT " jobs failed for good and were not retried. Retry them, or leave them.",
			n);
	item = axis_attention_item_new("job.failed", "", AXIS_ATTENTION_LOOK, (guint64)n, newest);
	axis_attention_item_say(item, text, NULL, NULL);
	axis_attention_item_link(item, "/jobs?state=failed&class=work", "Open Jobs");
	g_ptr_array_add(items, item);
	return TRUE;
}

static void
register_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	AxisWebJobs *jobs = user_data;

	axis_web_set_header_widget(web, header_widget, user_data);
	axis_web_add_dashboard_part(web, dash_collect, dash_render, dash_figures_free, user_data);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_NOW);
	axis_web_add_attention_source(web, "the jobs that failed", attention_source, user_data);
	axis_attention_ping_unref(jobs->attention);
	jobs->attention = axis_web_attention_ref_ping(web);

	axis_web_jobs_activity_register(web, user_data);
	axis_web_get(web, "/jobs", handle_list, user_data);
	axis_web_get(web, "/jobs/list", handle_list_fragment, user_data);
	axis_web_get(web, "/jobs/:id", handle_detail, user_data);
	axis_web_get(web, "/jobs/:id/detail", handle_detail_fragment, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/jobs/:id/cancel", AXIS_WEB_ROUTE_ADMIN,
		handle_cancel, user_data);
	axis_web_route_full(web, HTMX_METHOD_POST, "/jobs/:id/retry", AXIS_WEB_ROUTE_ADMIN,
		handle_retry, user_data);
}

static const AxisNavEntry nav_entries[] = {
	{ "Now", AXIS_JOBS_ACTIVITY_PATH, 1, AXIS_NAV_GROUP_ACTIVITY, FALSE, NULL, 0 },
	{ "Jobs", "/jobs", 10, AXIS_NAV_GROUP_ACTIVITY, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

const AxisModuleInfo *
axis_web_jobs_get_module(void)
{
	static AxisModuleInfo info;

	if (info.id == NULL) {
		info.id = AXIS_WEB_JOBS_MODULE_ID;
		info.name = "Jobs";
		info.register_routes = register_routes;
		info.nav_entries = nav_entries;
		info.description = "Pages that list background jobs, their progress and their errors.";
	}

	return &info;
}

/* ── Lifetime ───────────────────────────────────────────────────── */

AxisWebJobs *
axis_web_jobs_new(
	AxisDbPool *pool,
	AxisWebHub *hub
){
	AxisWebJobs *self;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisWebJobs, 1);
	self->pool = pool;
	self->hub = hub;
	self->dirty = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);

	return self;
}

gboolean
axis_web_jobs_attach(
	AxisWebJobs   *self,
	AxisJobQueue  *queue,
	GError       **error
){
	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(AXIS_IS_JOB_QUEUE(queue), FALSE);
	g_return_val_if_fail(self->queue == NULL, FALSE);

	if (!axis_web_jobs_count_activity(self->pool, self->kinds, &self->activity, error))
		return FALSE;

	self->queue = queue;
	axis_web_jobs_bridge_start(self);

	return TRUE;
}

gboolean
axis_web_jobs_count_now(
	AxisWebJobs  *self,
	GError      **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	return axis_web_jobs_count_activity(self->pool, self->kinds, &self->activity, error);
}

void
axis_web_jobs_stop(AxisWebJobs *self)
{
	g_return_if_fail(self != NULL);

	if (self->stopping)
		return;

	self->stopping = TRUE;
	axis_web_jobs_bridge_stop(self);
	while (self->pending > 0)
		g_main_context_iteration(NULL, TRUE);
	self->queue = NULL;
}

void
axis_web_jobs_free(AxisWebJobs *self)
{
	if (self == NULL)
		return;

	axis_web_jobs_stop(self);
	axis_attention_ping_unref(self->attention);
	g_hash_table_unref(self->dirty);
	g_clear_pointer(&self->kinds, axis_job_registry_unref);
	g_free(self);
}

void
axis_web_jobs_set_kinds(
	AxisWebJobs     *self,
	AxisJobRegistry *kinds
){
	g_return_if_fail(self != NULL);
	g_return_if_fail(self->kinds == NULL);

	self->kinds = kinds != NULL ? axis_job_registry_ref(kinds) : NULL;
}

gint
axis_web_jobs_get_running(AxisWebJobs *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->activity.running;
}
