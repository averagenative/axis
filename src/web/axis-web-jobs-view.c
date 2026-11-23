/*
 * axis-web-jobs-view.c - the elements of the Jobs pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-jobs-view.h"
#include "web/axis-web-table.h"

#include <string.h>

/* The most of a message or error shown in a table cell. */
#define CELL_TEXT_CHARS 100

/* ── Small helpers ──────────────────────────────────────────────── */

/* Adds @child to @parent and drops the caller's reference to it. */
static void
add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

static HtmxSpan *
span_with(
	const gchar *css_class,
	const gchar *text
){
	HtmxSpan *span = htmx_span_new();

	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(span), css_class);
	if (text != NULL)
		htmx_node_set_text_content(HTMX_NODE(span), text);

	return span;
}

/* One line of valid UTF-8, cut to @max characters. */
static gchar *
one_line(
	const gchar *text,
	glong        max
){
	g_autofree gchar *valid = g_utf8_make_valid(text != NULL ? text : "", -1);

	g_strdelimit(valid, "\r\n\t", ' ');
	if (max > 0 && g_utf8_strlen(valid, -1) > max) {
		gchar *end = g_utf8_offset_to_pointer(valid, max);

		*end = '\0';
		return g_strconcat(valid, "...", NULL);
	}

	return g_steal_pointer(&valid);
}

static gchar *
format_time(gint64 us)
{
	return axis_web_format_time(us, "-");
}

/* The columns of the jobs list, with and without the actions.  A job is
 * what it does in plain words, as the dashboard names it, linking to its
 * page, with its kind and number beside; its queue is on its page and in
 * the filter. */
static const AxisWebColumn job_columns[] = {
	{ "Job", AXIS_WEB_COL_NAME },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Progress", AXIS_WEB_COL_WIDE },
	{ "Attempts", AXIS_WEB_COL_NUMBER },
	{ "Created", AXIS_WEB_COL_DATE },
	{ "Finished", AXIS_WEB_COL_DATE },
	{ NULL, 0 }
};

static const AxisWebColumn admin_job_columns[] = {
	{ "Job", AXIS_WEB_COL_NAME },
	{ "State", AXIS_WEB_COL_STATE },
	{ "Progress", AXIS_WEB_COL_WIDE },
	{ "Attempts", AXIS_WEB_COL_NUMBER },
	{ "Created", AXIS_WEB_COL_DATE },
	{ "Finished", AXIS_WEB_COL_DATE },
	{ "Actions", AXIS_WEB_COL_ACTIONS },
	{ NULL, 0 }
};

static HtmxNode *
time_cell(gint64 us)
{
	HtmxTd *cell = htmx_td_new();
	g_autofree gchar *text = format_time(us);

	if (us > 0) {
		g_autoptr(GDateTime) time = g_date_time_new_from_unix_utc(us / G_USEC_PER_SEC);
		g_autofree gchar *iso = time != NULL ? g_date_time_format(time, "%Y-%m-%dT%H:%M:%SZ") : NULL;

		add(cell, htmx_time_new_with_datetime(text, iso != NULL ? iso : ""));
	} else {
		htmx_node_set_text_content(HTMX_NODE(cell), text);
	}

	return HTMX_NODE(cell);
}

gchar *
axis_web_jobs_topic_for_job(
	gint64    id,
	gboolean  admin
){
	return g_strdup_printf(admin ? "job:%" G_GINT64_FORMAT ":admin" : "job:%" G_GINT64_FORMAT, id);
}

/* ── Pieces ─────────────────────────────────────────────────────── */

HtmxNode *
axis_web_jobs_name_new(const AxisJobRow *row)
{
	HtmxSpan *name = htmx_span_new();
	g_autofree gchar *href = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, row->id);
	g_autofree gchar *number = g_strdup_printf("#%" G_GINT64_FORMAT, row->id);
	HtmxA *link = htmx_a_new_with_href(href);
	HtmxSpan *detail = htmx_span_new();

	htmx_element_add_class(HTMX_ELEMENT(name), "job-name");
	/* A kind the registry does not name is named by itself. */
	if (row->label != NULL)
		htmx_node_set_text_content(HTMX_NODE(link), row->label);
	else
		add(link, htmx_code_new_with_text(row->kind));
	add(name, link);

	/* The technical kind and the number, as a quiet detail. */
	htmx_element_add_class(HTMX_ELEMENT(detail), "job-kind");
	if (row->label != NULL) {
		add(detail, htmx_code_new_with_text(row->kind));
		add(detail, span_with("job-number", number));
	} else {
		htmx_node_set_text_content(HTMX_NODE(detail), number);
	}
	add(name, detail);

	return HTMX_NODE(name);
}

/* A job state in the words of the State filter; the raw state stays in the
 * badge's class. */
static const gchar *
state_label(AxisJobState state)
{
	switch (state) {
	case AXIS_JOB_STATE_QUEUED:
		return "Queued";
	case AXIS_JOB_STATE_RUNNING:
		return "Running";
	case AXIS_JOB_STATE_SUCCEEDED:
		return "Succeeded";
	case AXIS_JOB_STATE_FAILED:
		return "Failed";
	case AXIS_JOB_STATE_CANCELLED:
		return "Cancelled";
	default:
		return axis_job_state_to_string(state);
	}
}

/* A queue in plain words; a queue this list does not name is named by
 * itself.  The queue's own name is shown beside it as a quiet detail. */
static const gchar *
queue_label(const gchar *queue)
{
	static const struct
	{
		const gchar *queue;
		const gchar *label;
	} names[] = {
		{ "maintenance", "Background checks" },
		{ "scan", "Library scans and identification" },
		{ "probe", "Reading files" },
		{ "metadata", "Movie and TV details and Discover" },
		{ "convert", "Placing converted files" },
		{ "sample", "Savings measurements" },
		{ "remote", "On workers" },
		{ "compat", "Media servers" },
		{ "searches", "Searches" },
		{ "downloads", "Downloads and imports" }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		if (g_strcmp0(names[i].queue, queue) == 0)
			return names[i].label;
	}

	return queue;
}

static HtmxSpan *
state_badge(const AxisJobRow *row)
{
	g_autofree gchar *css = g_strdup_printf("badge badge-%s", axis_job_state_to_string(row->state));

	return span_with(css, state_label(row->state));
}

/* The progress bar and, beside it, what the job says it is doing. */
static HtmxNode *
progress_content(const AxisJobRow *row)
{
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *text = NULL;

	htmx_element_add_class(HTMX_ELEMENT(box), "job-progress");

	if (row->state == AXIS_JOB_STATE_RUNNING) {
		if (row->progress >= 0) {
			HtmxProgress *bar = htmx_progress_new_with_value(row->progress, 1.0);
			g_autofree gchar *percent = g_strdup_printf("%d%%", (gint)(row->progress * 100 + 0.5));

			htmx_element_set_attribute(HTMX_ELEMENT(bar), "aria-label", "Progress");
			add(box, bar);
			add(box, span_with("job-percent", percent));
		}
		if (row->progress_message != NULL && row->progress_message[0] != '\0') {
			text = one_line(row->progress_message, CELL_TEXT_CHARS);
			add(box, span_with("job-message", text));
		}
	} else if (row->state == AXIS_JOB_STATE_QUEUED && row->attempts > 0 && row->last_error != NULL) {
		g_autofree gchar *line = one_line(row->last_error, CELL_TEXT_CHARS);

		text = g_strconcat("Will try again after this error: ", line, NULL);
		add(box, span_with("job-message", text));
	} else if (row->state == AXIS_JOB_STATE_FAILED && row->last_error != NULL) {
		text = one_line(row->last_error, CELL_TEXT_CHARS);
		add(box, span_with("job-message job-error", text));
	} else if (row->state == AXIS_JOB_STATE_SUCCEEDED) {
		add(box, span_with("job-percent", "100%"));
	}

	return HTMX_NODE(box);
}

static HtmxNode *
action_button(
	const gchar *label,
	const gchar *css_class,
	const gchar *url,
	const gchar *target,
	const gchar *swap,
	const gchar *confirm
){
	HtmxButton *button = htmx_button_new_with_label(label);

	htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "button");
	htmx_element_add_class(HTMX_ELEMENT(button), css_class);
	htmx_element_set_attribute(HTMX_ELEMENT(button), "hx-post", url);
	if (target != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(button), "hx-target", target);
	htmx_element_set_attribute(HTMX_ELEMENT(button), "hx-swap", swap);
	if (confirm != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(button), "hx-confirm", confirm);

	return HTMX_NODE(button);
}

/*
 * The buttons that apply to @row: cancel for a job that is queued or
 * running (and not already being cancelled), retry for one that failed or
 * was cancelled.  They are htmx requests, so the CSRF token comes from
 * the page's body.  The server checks the role again.
 */
static HtmxNode *
actions(
	const AxisJobRow *row,
	const gchar      *target
){
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *cancel_url = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", row->id);
	g_autofree gchar *retry_url = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", row->id);
	g_autofree gchar *confirm = g_strdup_printf("Cancel job %" G_GINT64_FORMAT
		"? It stops, and runs again only if you retry it.", row->id);

	htmx_element_add_class(HTMX_ELEMENT(box), "actions");

	if (row->state == AXIS_JOB_STATE_QUEUED ||
	    (row->state == AXIS_JOB_STATE_RUNNING && !row->cancel_requested))
		add(box, action_button("Cancel job", "button-danger", cancel_url, target, "outerHTML", confirm));
	/* Done, the browser goes to the job (HX-Redirect); refused (the work is
	 * being done by another job, or its owner asks for it again), the
	 * reason takes the page's content. */
	if (row->state == AXIS_JOB_STATE_FAILED || row->state == AXIS_JOB_STATE_CANCELLED)
		add(box, action_button("Retry job", "button-small", retry_url, "#content", "innerHTML", NULL));

	return HTMX_NODE(box);
}

/* ── Activity indicator ─────────────────────────────────────────── */

typedef struct
{
	gint         n;
	const gchar *one;
	const gchar *many;
} Kind;

/* "2 searches, 1 download": what is under way, by kind, the most first. */
static gchar *
activity_kinds(const AxisJobsActivity *a)
{
	Kind kinds[6] = {
		{ 0, "search", "searches" }, { 0, "download", "downloads" }, { 0, "import", "imports" },
		{ 0, "scan", "scans" }, { 0, "conversion", "conversions" }, { 0, "other job", "other jobs" }
	};
	GString *text = g_string_new(NULL);
	guint i;
	guint k;

	kinds[0].n = a->searches;
	kinds[1].n = a->downloads;
	kinds[2].n = a->imports;
	kinds[3].n = a->scans;
	kinds[4].n = a->conversions;
	kinds[5].n = MAX(a->running - a->searches - a->imports - a->scans - a->conversions, 0);
	for (i = 0; i + 1 < G_N_ELEMENTS(kinds); i++) {
		for (k = i + 1; k < G_N_ELEMENTS(kinds); k++) {
			if (kinds[k].n > kinds[i].n) {
				Kind swap = kinds[i];

				kinds[i] = kinds[k];
				kinds[k] = swap;
			}
		}
	}
	for (i = 0; i < G_N_ELEMENTS(kinds) && kinds[i].n > 0; i++)
		g_string_append_printf(text, "%s%d %s", i > 0 ? ", " : "", kinds[i].n, kinds[i].n == 1 ? kinds[i].one :
			kinds[i].many);
	return g_string_free(text, FALSE);
}

HtmxNode *
axis_web_jobs_activity_new(const AxisJobsActivity *activity)
{
	HtmxA *link = htmx_a_new_with_href(AXIS_JOBS_ACTIVITY_PATH);
	gint now = activity->running + activity->downloads;
	g_autofree gchar *count = g_strdup_printf("%d", now);
	g_autofree gchar *kinds = activity_kinds(activity);
	g_autofree gchar *waiting = NULL;
	g_autofree gchar *title = NULL;

	htmx_element_set_id(HTMX_ELEMENT(link), "activity");
	htmx_element_add_class(HTMX_ELEMENT(link), "activity");
	if (now > 0)
		htmx_element_add_class(HTMX_ELEMENT(link), "activity-busy");
	/* Work only: Axis's own background upkeep is never counted here.  The
	 * kinds again, whole: on a tight row they are cut. */
	waiting = activity->queued > 0 ? g_strdup_printf("; %d more %s for %s turn", activity->queued,
		activity->queued == 1 ? "waits" : "wait", activity->queued == 1 ? "its" : "their") : g_strdup("");
	title = g_strdup_printf("What Axis is doing now%s%s%s (upkeep jobs are not counted)",
		kinds[0] != '\0' ? ": " : "", kinds, waiting);
	htmx_element_set_attribute(HTMX_ELEMENT(link), "title", title);
	add(link, span_with("activity-count", count));
	add(link, span_with(NULL, now == 1 ? " job running" : " jobs running"));
	if (kinds[0] != '\0') {
		g_autofree gchar *what = g_strdup_printf(": %s", kinds);

		add(link, span_with("activity-kinds", what));
	}

	return HTMX_NODE(link);
}

/* ── Row ────────────────────────────────────────────────────────── */

HtmxNode *
axis_web_jobs_row_new(
	const AxisJobRow *row,
	gboolean          admin
){
	HtmxTr *tr = htmx_tr_new();
	g_autofree gchar *id = g_strdup_printf("job-%" G_GINT64_FORMAT, row->id);
	g_autofree gchar *attempts = g_strdup_printf("%u / %u", row->attempts, row->max_attempts);
	g_autofree gchar *target = g_strdup_printf("#job-%" G_GINT64_FORMAT, row->id);
	HtmxTd *cell;

	htmx_element_set_id(HTMX_ELEMENT(tr), id);
	htmx_element_add_class(HTMX_ELEMENT(tr), "job-row");

	/* What it does, linking to the job, as the dashboard names it. */
	cell = htmx_td_new();
	add(cell, axis_web_jobs_name_new(row));
	add(tr, cell);

	cell = htmx_td_new();
	add(cell, state_badge(row));
	if (row->state == AXIS_JOB_STATE_RUNNING && row->cancel_requested)
		add(cell, span_with("badge badge-note", "Cancelling"));
	add(tr, cell);

	cell = htmx_td_new();
	add(cell, progress_content(row));
	add(tr, cell);

	add(tr, htmx_td_new_with_text(attempts));
	add(tr, time_cell(row->created_us));
	add(tr, time_cell(row->finished_us));

	if (admin) {
		cell = htmx_td_new();
		htmx_element_add_class(HTMX_ELEMENT(cell), "job-actions");
		add(cell, actions(row, target));
		add(tr, cell);
	}

	/* A row is also sent on its own, as a live update. */
	axis_web_table_row_roles(HTMX_NODE(tr), admin ? admin_job_columns : job_columns);

	return HTMX_NODE(tr);
}

/* ── Detail ─────────────────────────────────────────────────────── */

static void
fact(
	HtmxNode    *list,
	const gchar *label,
	HtmxNode    *value
){
	HtmxDd *dd = htmx_dd_new();

	add(list, htmx_dt_new_with_text(label));
	add(dd, value);
	add(list, dd);
}

static void
fact_text(
	HtmxNode    *list,
	const gchar *label,
	const gchar *text
){
	add(list, htmx_dt_new_with_text(label));
	add(list, htmx_dd_new_with_text(text));
}

/* How the job ended before each retry, newest first: "Failed after 2
 * attempts, <when>. Retried by <who>, <when>." and its error; who retried
 * it is for administrators only (anyone logged in may open a job). */
HtmxNode *
axis_web_jobs_earlier_new(
	const AxisJobRow *row,
	gboolean          admin
){
	HtmxSection *box;
	HtmxHeading *title;
	HtmxOl *list;
	guint i;

	if (row->earlier == NULL || row->earlier->len == 0)
		return NULL;

	box = htmx_section_new();
	title = htmx_h2_new();
	list = htmx_ol_new();
	htmx_element_set_id(HTMX_ELEMENT(box), "job-earlier-runs");
	htmx_element_add_class(HTMX_ELEMENT(box), "card");
	htmx_node_set_text_content(HTMX_NODE(title), "Earlier runs of this job");
	add(box, title);
	htmx_element_add_class(HTMX_ELEMENT(list), "earlier-runs");
	htmx_element_set_attribute(HTMX_ELEMENT(list), "reversed", "reversed");
	for (i = row->earlier->len; i > 0; i--) {
		const AxisJobEarlierRun *run = g_ptr_array_index(row->earlier, i - 1);
		HtmxLi *item = htmx_li_new();
		HtmxP *said = htmx_p_new();
		g_autofree gchar *ended = format_time(run->finished_us);
		g_autofree gchar *retried = format_time(run->retried_us);
		gboolean named = admin && run->retried_by != NULL;
		g_autofree gchar *line = g_strdup_printf("%s after %u attempt%s, %s. Retried%s%s, %s.",
			run->state == AXIS_JOB_STATE_CANCELLED ? "Cancelled" : "Failed", run->attempts,
			run->attempts == 1 ? "" : "s", ended, named ? " by " : "", named ? run->retried_by : "", retried);

		htmx_node_set_text_content(HTMX_NODE(said), line);
		add(item, said);
		if (run->error != NULL && run->error[0] != '\0')
			add(item, htmx_pre_new_with_text(run->error));
		add(list, item);
	}
	add(box, list);

	return HTMX_NODE(box);
}

HtmxNode *
axis_web_jobs_detail_new(
	const AxisJobRow *row,
	gboolean          admin
){
	HtmxSection *section = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();
	HtmxDl *facts = htmx_dl_new();
	g_autofree gchar *heading = g_strdup_printf("Job %" G_GINT64_FORMAT, row->id);
	g_autofree gchar *number = g_strdup_printf("%" G_GINT64_FORMAT, row->id);
	g_autofree gchar *attempts = g_strdup_printf("%u of %u", row->attempts, row->max_attempts);
	g_autofree gchar *created = format_time(row->created_us);
	g_autofree gchar *started = format_time(row->started_us);
	g_autofree gchar *finished = format_time(row->finished_us);
	HtmxCode *kind = htmx_code_new_with_text(row->kind);
	HtmxDiv *state = htmx_div_new();

	g_autofree gchar *refresh_url = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/detail", row->id);

	htmx_element_set_id(HTMX_ELEMENT(section), "job-detail");
	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	/* After a break in the stream, whatever was missed is fetched. */
	htmx_element_set_attribute(HTMX_ELEMENT(section), "hx-get", refresh_url);
	htmx_element_set_attribute(HTMX_ELEMENT(section), "hx-trigger", "htmx:sseOpen from:body");
	htmx_element_set_attribute(HTMX_ELEMENT(section), "hx-swap", "outerHTML");
	/* What it does, in plain words; the number and kind are facts. */
	htmx_node_set_text_content(HTMX_NODE(title), row->label != NULL ? row->label : heading);
	add(section, title);

	htmx_element_add_class(HTMX_ELEMENT(facts), "facts");
	if (row->label != NULL)
		fact_text(HTMX_NODE(facts), "Job", number);
	fact(HTMX_NODE(facts), "Job type", HTMX_NODE(kind));
	fact_text(HTMX_NODE(facts), "Work or upkeep", row->upkeep ?
		"Upkeep: a check or clean-up Axis repeats by itself" : "Work");
	{
		HtmxDd *dd = htmx_dd_new();
		HtmxCode *queue_code = htmx_code_new_with_text(row->queue);

		add(facts, htmx_dt_new_with_text("Queue"));
		if (g_strcmp0(queue_label(row->queue), row->queue) != 0) {
			HtmxSpan *label = htmx_span_new();
			g_autofree gchar *text = g_strdup_printf("%s ", queue_label(row->queue));

			htmx_node_set_text_content(HTMX_NODE(label), text);
			add(dd, label);
		}
		add(dd, queue_code);
		add(facts, dd);
	}

	add(state, state_badge(row));
	if (row->state == AXIS_JOB_STATE_RUNNING && row->cancel_requested)
		add(state, span_with("badge badge-note", "Cancelling"));
	fact(HTMX_NODE(facts), "State", HTMX_NODE(state));

	if (row->state == AXIS_JOB_STATE_RUNNING || row->state == AXIS_JOB_STATE_SUCCEEDED)
		fact(HTMX_NODE(facts), "Progress", progress_content(row));

	fact_text(HTMX_NODE(facts), "Attempts", attempts);
	fact_text(HTMX_NODE(facts), "Created", created);
	fact_text(HTMX_NODE(facts), "Started", started);
	fact_text(HTMX_NODE(facts), "Finished", finished);
	if (row->state == AXIS_JOB_STATE_QUEUED && row->run_at_us > 0) {
		g_autofree gchar *run_at = format_time(row->run_at_us);

		fact_text(HTMX_NODE(facts), "Runs at the earliest", run_at);
	}
	add(section, facts);

	if (row->last_error != NULL && row->last_error[0] != '\0') {
		HtmxHeading *error_title = htmx_h3_new();
		g_autofree gchar *error_text = g_utf8_make_valid(row->last_error, -1);

		htmx_node_set_text_content(HTMX_NODE(error_title), "Last error");
		add(section, error_title);
		add(section, htmx_pre_new_with_text(error_text));
	}

	if (admin)
		add(section, actions(row, "#job-detail"));

	return HTMX_NODE(section);
}

HtmxNode *
axis_web_jobs_payload_new(const AxisJobRow *row)
{
	HtmxSection *section = htmx_section_new();
	HtmxHeading *title = htmx_h2_new();
	g_autofree gchar *payload = g_utf8_make_valid(row->payload != NULL ? row->payload : "", -1);

	htmx_element_set_id(HTMX_ELEMENT(section), "job-payload");
	htmx_element_add_class(HTMX_ELEMENT(section), "card");
	htmx_node_set_text_content(HTMX_NODE(title), "Job input (payload)");
	add(section, title);
	add(section, htmx_pre_new_with_text(payload));

	return HTMX_NODE(section);
}

/* ── List ───────────────────────────────────────────────────────── */

/* "state=failed&queue=media&class=work" and the cursor, if any; the caller
 * frees. */
static gchar *
filter_query(
	const AxisJobFilter *filter,
	const gchar         *cursor
){
	GString *query = g_string_new("");

	if (filter->state != NULL && filter->state[0] != '\0') {
		g_autofree gchar *escaped = g_uri_escape_string(filter->state, NULL, FALSE);

		g_string_append_printf(query, "state=%s", escaped);
	}
	if (filter->queue != NULL && filter->queue[0] != '\0') {
		g_autofree gchar *escaped = g_uri_escape_string(filter->queue, NULL, FALSE);

		g_string_append_printf(query, "%squeue=%s", query->len > 0 ? "&" : "", escaped);
	}
	if (filter->job_class != NULL && filter->job_class[0] != '\0') {
		g_autofree gchar *escaped = g_uri_escape_string(filter->job_class, NULL, FALSE);

		g_string_append_printf(query, "%sclass=%s", query->len > 0 ? "&" : "", escaped);
	}
	if (cursor != NULL) {
		g_autofree gchar *escaped = g_uri_escape_string(cursor, NULL, FALSE);

		g_string_append_printf(query, "%sbefore=%s", query->len > 0 ? "&" : "", escaped);
	}

	return g_string_free(query, FALSE);
}

static gchar *
page_url(
	const gchar         *base,
	const AxisJobFilter *filter,
	const gchar         *cursor
){
	g_autofree gchar *query = filter_query(filter, cursor);

	return query[0] != '\0' ? g_strconcat(base, "?", query, NULL) : g_strdup(base);
}

HtmxNode *
axis_web_jobs_list_new(
	const AxisJobFilter *filter,
	GPtrArray           *rows,
	const gchar         *next,
	gboolean             admin,
	gboolean             brief
){
	HtmxDiv *box = htmx_div_new();
	g_autofree gchar *base_url = page_url("/jobs/list", filter, filter->cursor);
	g_autofree gchar *list_url = brief ? g_strconcat(base_url, strchr(base_url, '?') != NULL ? "&" : "?",
		"brief=1", NULL) : g_strdup(base_url);
	guint i;

	/* A card like every other table's: it scrolls the table sideways on a
	 * narrow screen.  The brief list of upkeep is inside its own card. */
	if (!brief)
		htmx_element_add_class(HTMX_ELEMENT(box), "card");
	htmx_element_set_id(HTMX_ELEMENT(box), brief ? "upkeep-list" : "jobs-list");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-get", list_url);
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-trigger",
		"sse:" AXIS_JOBS_EVENT_CHANGED " throttle:1s, htmx:sseOpen from:body");
	htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-swap", "outerHTML");

	if (rows->len == 0) {
		HtmxP *none = htmx_p_new();

		gboolean filtered = (filter->state != NULL && filter->state[0] != '\0') ||
			(filter->queue != NULL && filter->queue[0] != '\0');

		if (brief)
			htmx_node_set_text_content(HTMX_NODE(none), "No upkeep jobs match these filters.");
		else if (g_strcmp0(filter->job_class, "work") == 0)
			htmx_node_set_text_content(HTMX_NODE(none), filtered ? "No work jobs match these filters." :
				"No work jobs yet.");
		else
			htmx_node_set_text_content(HTMX_NODE(none), "No jobs match these filters.");
		add(box, none);
		if (!brief && filtered) {
			HtmxA *all = htmx_a_new_with_href("/jobs");

			htmx_node_set_text_content(HTMX_NODE(all), "Show all jobs");
			add(box, all);
		}
	} else {
		HtmxNode *table = admin ? axis_web_table_new("jobs-admin", admin_job_columns) :
			axis_web_table_new("jobs", job_columns);
		HtmxTbody *body = htmx_tbody_new();

		htmx_element_add_class(HTMX_ELEMENT(table), "jobs-table");

		htmx_element_set_id(HTMX_ELEMENT(body), "job-rows");
		for (i = 0; i < rows->len; i++)
			add(body, axis_web_jobs_row_new(g_ptr_array_index(rows, i), admin));
		add(table, body);
		add(box, table);
	}

	if (brief) {
		/* The rest of the upkeep is on a page of its own. */
		if (next != NULL) {
			AxisJobFilter all = *filter;
			g_autofree gchar *url = NULL;
			HtmxP *line = htmx_p_new();
			HtmxA *more;

			all.job_class = "upkeep";
			all.cursor = NULL;
			url = page_url("/jobs", &all, NULL);
			more = htmx_a_new_with_href(url);
			htmx_element_add_class(HTMX_ELEMENT(line), "card-foot");
			htmx_node_set_text_content(HTMX_NODE(more), "All upkeep jobs");
			add(line, more);
			add(box, line);
		}
	} else if (filter->cursor != NULL || next != NULL) {
		HtmxNav *pager = htmx_nav_new();

		htmx_element_add_class(HTMX_ELEMENT(pager), "pager");
		htmx_element_set_attribute(HTMX_ELEMENT(pager), "aria-label", "Pages of jobs");
		if (filter->cursor != NULL) {
			g_autofree gchar *url = page_url("/jobs", filter, NULL);
			HtmxA *first = htmx_a_new_with_href(url);

			htmx_node_set_text_content(HTMX_NODE(first), "Newest jobs");
			add(pager, first);
		}
		if (next != NULL) {
			g_autofree gchar *url = page_url("/jobs", filter, next);
			HtmxA *older = htmx_a_new_with_href(url);

			htmx_node_set_text_content(HTMX_NODE(older), "Older jobs");
			add(pager, older);
		}
		add(box, pager);
	}

	return HTMX_NODE(box);
}

/* ── Upkeep ─────────────────────────────────────────────────────── */

HtmxNode *
axis_web_jobs_upkeep_counts_new(
	gint64 running,
	gint64 queued
){
	HtmxSpan *counts = htmx_span_new();
	g_autofree gchar *text = NULL;

	if (running == 0 && queued == 0)
		text = g_strdup("none running");
	else if (queued == 0)
		text = g_strdup_printf("%" G_GINT64_FORMAT " running", running);
	else
		text = g_strdup_printf("%" G_GINT64_FORMAT " running, %" G_GINT64_FORMAT " queued", running, queued);
	htmx_element_set_id(HTMX_ELEMENT(counts), "upkeep-counts");
	htmx_element_add_class(HTMX_ELEMENT(counts), "upkeep-counts");
	htmx_node_set_text_content(HTMX_NODE(counts), text);
	return HTMX_NODE(counts);
}

HtmxNode *
axis_web_jobs_upkeep_new(
	const AxisJobFilter *filter,
	GPtrArray           *rows,
	const gchar         *next,
	gboolean             admin,
	gint64               running,
	gint64               queued
){
	HtmxDetails *details = htmx_details_new();
	HtmxSummary *summary = htmx_summary_new();
	HtmxP *help = htmx_p_new();
	AxisJobFilter upkeep = *filter;

	htmx_element_set_id(HTMX_ELEMENT(details), "jobs-upkeep");
	htmx_element_add_class(HTMX_ELEMENT(details), "card");
	htmx_element_add_class(HTMX_ELEMENT(details), "upkeep");
	add(summary, span_with("upkeep-title", "Upkeep jobs"));
	add(summary, axis_web_jobs_upkeep_counts_new(running, queued));
	add(details, summary);
	htmx_element_add_class(HTMX_ELEMENT(help), "help");
	htmx_node_set_text_content(HTMX_NODE(help), "Checks and clean-ups Axis repeats by itself, such as "
		"looking at the download clients every minute. They are not counted as work running.");
	add(details, help);
	upkeep.job_class = "upkeep";
	upkeep.cursor = NULL;
	add(details, axis_web_jobs_list_new(&upkeep, rows, next, admin, TRUE));
	return HTMX_NODE(details);
}

/* ── Filter form ────────────────────────────────────────────────── */

/* A queue's option text: its plain name with its own name after it, as
 * "Searches (searches)"; @owned keeps the built text alive. */
static const gchar *
queue_option(
	const gchar *queue,
	GPtrArray   *owned
){
	const gchar *label = queue_label(queue);
	gchar *text;

	if (g_strcmp0(label, queue) == 0)
		return queue;
	text = g_strdup_printf("%s (%s)", label, queue);
	g_ptr_array_add(owned, text);

	return text;
}

static HtmxNode *
filter_select(
	const gchar         *id,
	const gchar         *label,
	const gchar         *name,
	const gchar *const  *values,
	const gchar *const  *labels,
	const gchar         *current
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxSelect *chooser = htmx_select_new_with_name(name);
	guint i;

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(chooser), id);

	for (i = 0; values[i] != NULL; i++) {
		HtmxOption *option = htmx_option_new_with_value(values[i], labels[i]);

		if (g_strcmp0(values[i], current != NULL ? current : "") == 0)
			htmx_option_set_selected(option, TRUE);
		add(chooser, option);
	}

	add(field, text);
	add(field, chooser);

	return HTMX_NODE(field);
}

HtmxNode *
axis_web_jobs_filter_form_new(
	const AxisJobFilter *filter,
	const gchar *const  *queues
){
	static const gchar *const state_values[] = {
		"", "active", "queued", "running", "succeeded", "failed", "cancelled", NULL
	};
	static const gchar *const state_labels[] = {
		"All", "Active (queued and running)", "Queued", "Running", "Succeeded", "Failed",
		"Cancelled", NULL
	};
	static const gchar *const class_values[] = { "", "work", "upkeep", NULL };
	static const gchar *const class_labels[] = { "Work, then upkeep", "Work only", "Upkeep only", NULL };
	HtmxForm *form = htmx_form_new();
	g_autoptr(GPtrArray) values = g_ptr_array_new();
	g_autoptr(GPtrArray) labels = g_ptr_array_new();
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
	HtmxDiv *buttons = htmx_div_new();
	HtmxButton *apply = htmx_button_new_submit();
	gboolean current_listed = FALSE;
	guint i;

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/jobs");
	htmx_element_set_attribute(HTMX_ELEMENT(form), "method", "get");
	htmx_element_add_class(HTMX_ELEMENT(form), "filters");

	add(form, filter_select("filter-class", "Work or upkeep", "class", class_values, class_labels,
		filter->job_class));
	add(form, filter_select("filter-state", "State", "state", state_values, state_labels,
		filter->state));

	g_ptr_array_add(values, (gpointer)"");
	g_ptr_array_add(labels, (gpointer)"All");
	for (i = 0; queues != NULL && queues[i] != NULL; i++) {
		g_ptr_array_add(values, (gpointer)queues[i]);
		g_ptr_array_add(labels, (gpointer)queue_option(queues[i], owned));
		if (g_strcmp0(queues[i], filter->queue) == 0)
			current_listed = TRUE;
	}
	if (filter->queue != NULL && filter->queue[0] != '\0' && !current_listed) {
		g_ptr_array_add(values, (gpointer)filter->queue);
		g_ptr_array_add(labels, (gpointer)queue_option(filter->queue, owned));
	}
	g_ptr_array_add(values, NULL);
	g_ptr_array_add(labels, NULL);
	add(form, filter_select("filter-queue", "Queue", "queue",
		(const gchar *const *)values->pdata, (const gchar *const *)labels->pdata, filter->queue));

	htmx_node_set_text_content(HTMX_NODE(apply), "Filter");
	add(buttons, apply);
	add(form, buttons);

	return HTMX_NODE(form);
}

/* ── Fragments ──────────────────────────────────────────────────── */

gchar *
axis_web_jobs_render_oob(HtmxNode *node)
{
	htmx_element_set_attribute(HTMX_ELEMENT(node), "hx-swap-oob", "true");

	return htmx_element_render(HTMX_ELEMENT(node));
}
