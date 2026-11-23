/*
 * axis-web-jobs-activity.c - the Activity page: what Axis is doing now, in a
 * person's words
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   GET /activity            the work under way, the downloads at their
 *                            clients, the work waiting for its turn, and
 *                            what ended in the last hour
 *   GET /activity?part=list  the same lists alone (what the page fetches
 *                            again when the hub says work changed)
 *
 * The Jobs pages' data, worded for a person: each job by what it does to
 * what ("Searching for Lantern Bay (2025), season 1"), in the words its
 * module gives (axis_web_add_activity_words()), else the kind's label.
 * Anyone logged in sees it; who asked and the job's number are for
 * administrators.  Background upkeep is never listed.  Three statements a
 * view, and one more a module that names its jobs.
 */

#include "web/axis-web-jobs-private.h"

#include <string.h>

#include "core/axis-size.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-private.h"
#include "web/axis-web-storage-private.h"

#define ADD axis_web_storage_add

/* Work shown at most, of each list. */
#define SHOWN 100

/* What ended within this long is listed. */
#define ENDED_S 3600

#define US(column) "(extract(epoch FROM " column ") * 1000000)::bigint"

typedef struct
{
	AxisWebActivityItem  item;	/* the module's words */
	gchar               *kind;
	gchar               *key;
	gchar               *label;
	gchar               *user;
	gchar               *message;
	gchar               *error;
	AxisJobState         state;
	gint64               queued_us;
	gint64               started_us;
	gint64               run_at_us;
	gint64               finished_us;
	gdouble              progress;
	guint                ahead;
	gboolean             admin_href;	/* item.href is an administrator's page */
} Entry;

typedef struct
{
	gchar  *title;
	gchar  *client;
	gchar  *state;
	gint64  size;
	gint64  done;
} Download;

typedef struct
{
	AxisWeb   *web;
	gboolean   part;
	GPtrArray *live;	/* Entry: running first, then in their turn */
	GPtrArray *ended;	/* Entry: the latest first */
	GPtrArray *downloads;	/* Download */
	gint64     now_us;
	GError    *error;
} Work;

static void
entry_free(Entry *entry)
{
	g_free(entry->item.words);
	g_free(entry->item.href);
	g_clear_pointer(&entry->item.payload, json_object_unref);
	g_free(entry->kind);
	g_free(entry->key);
	g_free(entry->label);
	g_free(entry->user);
	g_free(entry->message);
	g_free(entry->error);
	g_free(entry);
}

static void
download_free(Download *download)
{
	g_free(download->title);
	g_free(download->client);
	g_free(download->state);
	g_free(download);
}

static void
work_free(gpointer data)
{
	Work *work = data;

	g_clear_pointer(&work->live, g_ptr_array_unref);
	g_clear_pointer(&work->ended, g_ptr_array_unref);
	g_clear_pointer(&work->downloads, g_ptr_array_unref);
	g_clear_error(&work->error);
	g_free(work);
}

/* The upkeep kinds, quoted, as a text[] literal. */
static gchar *
upkeep_literal(AxisJobRegistry *kinds)
{
	g_auto(GStrv) upkeep = kinds != NULL ? axis_job_registry_list_class(kinds, AXIS_JOB_CLASS_UPKEEP) : NULL;
	GString *literal = g_string_new("{");
	guint i;

	for (i = 0; upkeep != NULL && upkeep[i] != NULL; i++)
		g_string_append_printf(literal, "%s\"%s\"", i > 0 ? "," : "", upkeep[i]);
	g_string_append_c(literal, '}');
	return g_string_free(literal, FALSE);
}

static gchar *
text_of(
	AxisDbResult *rows,
	guint         row,
	guint         column
){
	return axis_db_result_is_null(rows, row, column) ? NULL : g_strdup(axis_db_result_get_text(rows, row, column));
}

static gint64
int_of(
	AxisDbResult *rows,
	guint         row,
	guint         column
){
	gint64 value = 0;

	if (!axis_db_result_is_null(rows, row, column))
		axis_db_result_get_int64(rows, row, column, &value);
	return value;
}

/* Columns: id, kind, dedupe_key, state, queued, started, run_at, finished,
 * progress, message, error, payload, ahead. */
static Entry *
entry_from(
	AxisWebJobs  *jobs,
	AxisDbResult *rows,
	guint         row
){
	Entry *entry = g_new0(Entry, 1);
	g_autofree gchar *payload = text_of(rows, row, 11);
	const gchar *progress = axis_db_result_get_text(rows, row, 8);

	entry->item.job_id = int_of(rows, row, 0);
	entry->kind = text_of(rows, row, 1);
	entry->key = text_of(rows, row, 2);
	entry->item.kind = entry->kind;
	entry->item.key = entry->key;
	if (!axis_job_state_from_string(axis_db_result_get_text(rows, row, 3), &entry->state))
		entry->state = AXIS_JOB_STATE_QUEUED;
	entry->queued_us = int_of(rows, row, 4);
	entry->started_us = int_of(rows, row, 5);
	entry->run_at_us = int_of(rows, row, 6);
	entry->finished_us = int_of(rows, row, 7);
	entry->progress = progress != NULL ? CLAMP(g_ascii_strtod(progress, NULL), 0.0, 1.0) : -1.0;
	entry->message = text_of(rows, row, 9);
	entry->error = text_of(rows, row, 10);
	entry->ahead = (guint)MAX(int_of(rows, row, 12), 0);
	entry->label = jobs->kinds != NULL ? g_strdup(axis_job_registry_get_label(jobs->kinds, entry->kind)) : NULL;
	if (payload != NULL) {
		g_autoptr(JsonParser) parser = json_parser_new();

		if (json_parser_load_from_data(parser, payload, -1, NULL) && json_parser_get_root(parser) != NULL &&
		    JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
			JsonNode *user;

			entry->item.payload = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
			user = json_object_get_member(entry->item.payload, "user");
			if (user != NULL && JSON_NODE_HOLDS_VALUE(user) && json_node_get_value_type(user) == G_TYPE_STRING)
				entry->user = g_utf8_make_valid(json_node_get_string(user), -1);
		}
	}
	return entry;
}

/* ── Words the shell knows itself ───────────────────────────────── */

static gint64
payload_id(
	const Entry *entry,
	const gchar *member
){
	JsonNode *node = entry->item.payload != NULL ? json_object_get_member(entry->item.payload, member) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return 0;
	return json_node_get_int(node);
}

/* The scans of a disk, the imports and Identify's look at a folder: one
 * statement each for all of them. */
static void
own_words(
	AxisDbPool *pool,
	GPtrArray  *entries
){
	static const struct
	{
		const gchar *kind;
		const gchar *member;
		const gchar *sql;
		const gchar *words;
		const gchar *href;
		gboolean     admin;	/* the page is an administrator's */
	} known[] = {
		{ "library.scan", "volume_id", "SELECT id, name FROM volume WHERE id = ANY ($1::bigint[])",
		  "Scanning the disk ", "/storage/volumes/", FALSE },
		{ "import.run", "import", "SELECT id, coalesce(title_label, release_name, 'a download') FROM import"
		  " WHERE id = ANY ($1::bigint[])", "Importing ", "/imports/", FALSE },
		{ "identify.root", "root", "SELECT id, name FROM library_root WHERE id = ANY ($1::bigint[])",
		  "Identifying the movies and series in the library folder ", "/identify/roots/", TRUE }
	};
	guint k;
	guint i;

	for (k = 0; k < G_N_ELEMENTS(known); k++) {
		g_autoptr(GString) ids = g_string_new("{");
		g_autoptr(AxisDbResult) rows = NULL;
		const gchar *params[1];
		guint n = 0;

		for (i = 0; i < entries->len; i++) {
			const Entry *entry = g_ptr_array_index(entries, i);
			gint64 id = payload_id(entry, known[k].member);

			if (g_strcmp0(entry->kind, known[k].kind) != 0 || id <= 0)
				continue;
			g_string_append_printf(ids, "%s%" G_GINT64_FORMAT, n++ > 0 ? "," : "", id);
		}
		if (n == 0)
			continue;
		g_string_append_c(ids, '}');
		params[0] = ids->str;
		if (!axis_db_pool_exec(pool, known[k].sql, 1, params, &rows, NULL))
			continue;
		for (i = 0; i < entries->len; i++) {
			Entry *entry = g_ptr_array_index(entries, i);
			gint64 id = payload_id(entry, known[k].member);
			guint r;

			if (g_strcmp0(entry->kind, known[k].kind) != 0 || id <= 0)
				continue;
			for (r = 0; r < axis_db_result_get_n_rows(rows); r++) {
				if (int_of(rows, r, 0) != id)
					continue;
				entry->item.words = g_strconcat(known[k].words, axis_db_result_get_text(rows, r, 1), NULL);
				entry->item.href = g_strdup_printf("%s%" G_GINT64_FORMAT, known[k].href, id);
				entry->admin_href = known[k].admin;
			}
		}
	}
}

/* ── Reading ────────────────────────────────────────────────────── */

static void
work_run(
	AxisWebJobs *jobs,
	gpointer     data
){
	Work *work = data;
	g_autoptr(AxisDbResult) live = NULL;
	g_autoptr(AxisDbResult) ended = NULL;
	g_autoptr(AxisDbResult) downloads = NULL;
	g_autoptr(GPtrArray) items = g_ptr_array_new();
	g_autofree gchar *upkeep = upkeep_literal(jobs->kinds);
	g_autofree gchar *within = g_strdup_printf("%d", ENDED_S);
	const gchar *params[2];
	guint i;

	params[0] = upkeep;
	params[1] = within;
	if (!axis_db_pool_exec(jobs->pool, "SELECT j.id, j.kind, j.dedupe_key, j.state::text, " US("j.created_at") ","
	        " " US("j.started_at") ", " US("j.run_at") ", NULL::bigint, j.progress::text, j.progress_message, NULL,"
	        " j.payload::text, CASE WHEN j.state = 'queued' THEN (SELECT count(*) FROM job o WHERE o.queue = j.queue"
	        "   AND o.state = 'queued' AND o.id <> j.id AND (o.priority > j.priority OR (o.priority = j.priority"
	        "   AND (o.run_at < j.run_at OR (o.run_at = j.run_at AND o.id < j.id))))) ELSE 0 END,"
	        " (extract(epoch FROM now()) * 1000000)::bigint"
	        " FROM job j WHERE j.state IN ('queued', 'running') AND NOT (j.kind = ANY ($1::text[]))"
	        " ORDER BY j.state = 'running' DESC, j.priority DESC, j.run_at, j.id LIMIT 100", 1, params, &live,
	        &work->error) ||
	    !axis_db_pool_exec(jobs->pool, "SELECT j.id, j.kind, j.dedupe_key, j.state::text, " US("j.created_at") ","
	        " " US("j.started_at") ", " US("j.run_at") ", " US("j.finished_at") ", j.progress::text,"
	        " j.progress_message, j.last_error, j.payload::text, 0"
	        " FROM job j WHERE j.state IN ('succeeded', 'failed', 'cancelled') AND NOT (j.kind = ANY ($1::text[]))"
	        " AND j.finished_at > now() - $2::integer * interval '1 second' ORDER BY j.finished_at DESC LIMIT 100",
	        2, params, &ended, &work->error) ||
	    !axis_db_pool_exec(jobs->pool, "SELECT coalesce(title_label, release_name), client_name, state, size_bytes,"
	        " done_bytes FROM download WHERE state IN ('sending', 'queued', 'downloading', 'paused')"
	        " ORDER BY created_at, id LIMIT 100", 0, NULL, &downloads, &work->error))
		return;
	work->now_us = g_get_real_time();
	work->live = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
	work->ended = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
	work->downloads = g_ptr_array_new_with_free_func((GDestroyNotify)download_free);
	for (i = 0; i < axis_db_result_get_n_rows(live); i++) {
		g_ptr_array_add(work->live, entry_from(jobs, live, i));
		work->now_us = int_of(live, i, 13);
	}
	for (i = 0; i < axis_db_result_get_n_rows(ended); i++)
		g_ptr_array_add(work->ended, entry_from(jobs, ended, i));
	for (i = 0; i < axis_db_result_get_n_rows(downloads); i++) {
		Download *d = g_new0(Download, 1);

		d->title = text_of(downloads, i, 0);
		d->client = text_of(downloads, i, 1);
		d->state = text_of(downloads, i, 2);
		d->size = axis_db_result_is_null(downloads, i, 3) ? -1 : int_of(downloads, i, 3);
		d->done = axis_db_result_is_null(downloads, i, 4) ? -1 : int_of(downloads, i, 4);
		g_ptr_array_add(work->downloads, d);
	}
	/* The words: the shell's own, then each module's for its kinds. */
	own_words(jobs->pool, work->live);
	own_words(jobs->pool, work->ended);
	for (i = 0; i < work->live->len; i++)
		g_ptr_array_add(items, &((Entry *)g_ptr_array_index(work->live, i))->item);
	for (i = 0; i < work->ended->len; i++)
		g_ptr_array_add(items, &((Entry *)g_ptr_array_index(work->ended, i))->item);
	axis_web_activity_words(work->web, items);
}

/* ── Drawing ────────────────────────────────────────────────────── */

static HtmxNode *
heading(
	guint        level,
	const gchar *text
){
	HtmxHeading *h = level == 1 ? htmx_h1_new() : htmx_h2_new();

	htmx_node_set_text_content(HTMX_NODE(h), text);
	return HTMX_NODE(h);
}

/* One line of the list: what, then where it stands; a link to what it is
 * for; for an administrator who asked, and the job. */
static HtmxNode *
item_line(
	const Entry *entry,
	const gchar *state,
	const gchar *css,
	gboolean     admin
){
	HtmxLi *li = htmx_li_new();
	HtmxSpan *what = htmx_span_new();
	g_autofree gchar *name = g_strdup(entry->item.words != NULL ? entry->item.words : entry->label != NULL ?
		entry->label : entry->kind);
	g_autofree gchar *rest = g_strdup_printf(": %s.", state);

	htmx_element_add_class(HTMX_ELEMENT(li), "activity-item");
	htmx_element_add_class(HTMX_ELEMENT(li), css);
	htmx_element_add_class(HTMX_ELEMENT(what), "activity-what");
	/* No link a viewer cannot open. */
	if (entry->item.href != NULL && (admin || !entry->admin_href)) {
		HtmxA *link = htmx_a_new_with_href(entry->item.href);
		HtmxBdi *text = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(text), name);
		ADD(link, text);
		ADD(what, link);
	} else {
		HtmxBdi *text = htmx_bdi_new();

		htmx_node_set_text_content(HTMX_NODE(text), name);
		ADD(what, text);
	}
	ADD(li, what);
	ADD(li, axis_web_storage_text("activity-state", rest));
	if (admin) {
		g_autofree gchar *href = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, entry->item.job_id);
		g_autofree gchar *number = g_strdup_printf("job %" G_GINT64_FORMAT, entry->item.job_id);
		HtmxSpan *detail = htmx_span_new();
		HtmxA *link = htmx_a_new_with_href(href);

		htmx_element_add_class(HTMX_ELEMENT(detail), "activity-detail");
		if (entry->user != NULL) {
			g_autofree gchar *by = g_strdup_printf("asked by %s, ", entry->user);
			HtmxSpan *who = htmx_span_new();

			htmx_node_set_text_content(HTMX_NODE(who), by);
			ADD(detail, who);
		}
		htmx_node_set_text_content(HTMX_NODE(link), number);
		ADD(detail, link);
		ADD(li, detail);
	}
	return HTMX_NODE(li);
}

/* (The download client's name is an administrator's: a viewer gets the plain
 * state.) */
static gchar *
download_state(
	const Download *d,
	gboolean        admin
){
	g_autofree gchar *at = admin ? g_strdup_printf(" at %s", d->client != NULL ? d->client : "its client") :
		g_strdup("");

	if (g_strcmp0(d->state, "downloading") == 0 && d->size > 0 && d->done >= 0) {
		g_autofree gchar *done = axis_size_format((guint64)d->done);
		g_autofree gchar *size = axis_size_format((guint64)d->size);

		return g_strdup_printf("downloading%s, %d%%, %s of %s", at, (gint)((gdouble)d->done * 100.0 /
			(gdouble)d->size), done, size);
	}
	if (g_strcmp0(d->state, "sending") == 0)
		return g_strdup_printf("being sent to %s", admin && d->client != NULL ? d->client : "its client");
	if (g_strcmp0(d->state, "paused") == 0)
		return g_strdup_printf("paused%s", at);
	if (g_strcmp0(d->state, "queued") == 0)
		return g_strdup_printf("waiting for its turn%s", at);
	return g_strdup_printf("downloading%s", at);
}

/* Where a job in hand stands, in the words of the status lines; its own
 * progress message is an administrator's. */
static gchar *
live_state(
	const Entry *entry,
	gint64       now_us,
	gboolean     admin
){
	AxisJobLive live;

	memset(&live, 0, sizeof(live));
	live.running = entry->state == AXIS_JOB_STATE_RUNNING;
	live.queued_us = entry->queued_us;
	live.started_us = entry->started_us;
	live.run_at_us = entry->run_at_us;
	live.progress = entry->progress;
	live.message = admin ? entry->message : NULL;
	live.ahead = entry->ahead;
	return axis_web_activity_state(&live, now_us);
}

/* How it ended; what it said (its error, its last message) is an
 * administrator's. */
static gchar *
ended_state(
	const Entry *entry,
	gint64       now_us,
	gboolean     admin
){
	g_autofree gchar *ago = axis_web_activity_ago(entry->finished_us, now_us);
	const gchar *error = admin ? entry->error : NULL;
	const gchar *message = admin ? entry->message : NULL;

	if (entry->state == AXIS_JOB_STATE_FAILED)
		return g_strdup_printf("failed %s%s%s", ago, error != NULL ? ": " : "", error != NULL ? error : "");
	if (entry->state == AXIS_JOB_STATE_CANCELLED)
		return g_strdup_printf("cancelled %s", ago);
	return g_strdup_printf("done %s%s%s", ago, message != NULL && message[0] != '\0' ? ": " : "",
		message != NULL ? message : "");
}

static HtmxNode *
section(
	const gchar *title,
	HtmxNode    *list
){
	HtmxDiv *box = htmx_div_new();

	htmx_element_add_class(HTMX_ELEMENT(box), "activity-section");
	ADD(box, heading(2, title));
	ADD(box, list);
	return HTMX_NODE(box);
}

static HtmxNode *
lists(
	const Work *work,
	gboolean    admin
){
	HtmxDiv *box = htmx_div_new();
	HtmxUl *running = htmx_ul_new();
	HtmxUl *waiting = htmx_ul_new();
	HtmxUl *ended = htmx_ul_new();
	HtmxUl *downloads = htmx_ul_new();
	guint n_running = 0;
	guint n_waiting = 0;
	guint i;

	/* Fetched again, whole, when the hub says work or a download changed (the
	 * last of a burst winning), and by time while anything is in hand. */
	{
		g_autofree gchar *trigger = axis_web_activity_trigger(work->live->len > 0 || work->downloads->len > 0, 0);

		htmx_element_set_id(HTMX_ELEMENT(box), "activity-list");
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-get", AXIS_JOBS_ACTIVITY_PATH "?part=list");
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-trigger", trigger);
		htmx_element_set_attribute(HTMX_ELEMENT(box), "hx-swap", "outerHTML");
	}
	htmx_element_add_class(HTMX_ELEMENT(running), "activity-items");
	htmx_element_add_class(HTMX_ELEMENT(waiting), "activity-items");
	htmx_element_add_class(HTMX_ELEMENT(ended), "activity-items");
	htmx_element_add_class(HTMX_ELEMENT(downloads), "activity-items");
	for (i = 0; i < work->live->len && i < SHOWN; i++) {
		const Entry *entry = g_ptr_array_index(work->live, i);
		g_autofree gchar *state = live_state(entry, work->now_us, admin);

		if (entry->state == AXIS_JOB_STATE_RUNNING) {
			ADD(running, item_line(entry, state, "activity-running", admin));
			n_running++;
		} else {
			ADD(waiting, item_line(entry, state, "activity-waiting", admin));
			n_waiting++;
		}
	}
	for (i = 0; i < work->downloads->len && i < SHOWN; i++) {
		const Download *d = g_ptr_array_index(work->downloads, i);
		g_autofree gchar *state = download_state(d, admin);
		g_autofree gchar *text = g_strdup_printf(": %s.", state);
		HtmxLi *li = htmx_li_new();
		HtmxSpan *what = htmx_span_new();
		HtmxA *link = htmx_a_new_with_href("/downloads");
		HtmxBdi *name = htmx_bdi_new();

		htmx_element_add_class(HTMX_ELEMENT(li), "activity-item");
		htmx_element_add_class(HTMX_ELEMENT(li), "activity-running");
		htmx_element_add_class(HTMX_ELEMENT(what), "activity-what");
		htmx_node_set_text_content(HTMX_NODE(name), d->title != NULL ? d->title : "A download");
		ADD(link, name);
		ADD(what, link);
		ADD(li, what);
		ADD(li, axis_web_storage_text("activity-state", text));
		ADD(downloads, li);
	}
	for (i = 0; i < work->ended->len && i < SHOWN; i++) {
		const Entry *entry = g_ptr_array_index(work->ended, i);
		g_autofree gchar *state = ended_state(entry, work->now_us, admin);

		ADD(ended, item_line(entry, state, entry->state == AXIS_JOB_STATE_FAILED ? "activity-failed" :
			"activity-done", admin));
	}
	if (n_running == 0 && work->downloads->len == 0 && n_waiting == 0)
		ADD(box, axis_web_storage_text("all-clear", "Nothing is under way or waiting now."));
	if (n_running > 0)
		ADD(box, section("Under way", HTMX_NODE(running)));
	else
		g_object_unref(running);
	if (work->downloads->len > 0)
		ADD(box, section("Downloading", HTMX_NODE(downloads)));
	else
		g_object_unref(downloads);
	if (n_waiting > 0)
		ADD(box, section("Waiting for their turn", HTMX_NODE(waiting)));
	else
		g_object_unref(waiting);
	if (work->ended->len > 0)
		ADD(box, section("Ended in the last hour", HTMX_NODE(ended)));
	else
		g_object_unref(ended);
	return HTMX_NODE(box);
}

static HtmxResponse *
work_build(
	AxisWebJobs *jobs,
	HtmxRequest *request,
	gpointer     data
){
	Work *work = data;
	gboolean admin = axis_web_request_is_admin(request);
	g_autoptr(HtmxDiv) content = NULL;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);
	if (work->part) {
		g_autoptr(HtmxNode) box = lists(work, admin);
		g_autofree gchar *markup = htmx_element_render(HTMX_ELEMENT(box));
		HtmxResponse *response = htmx_response_new();

		htmx_response_set_content(response, markup);
		htmx_response_set_content_type(response, "text/html; charset=utf-8");
		htmx_response_add_header(response, "Cache-Control", "no-store");
		return response;
	}
	content = htmx_div_new();
	ADD(content, heading(1, "Activity"));
	ADD(content, axis_web_page_intro("What Axis is doing now, what waits for its turn, and what ended in the last "
		"hour, in plain words. Background upkeep is not listed; every job, upkeep included, is on the Jobs page."));
	axis_web_page_add_topic(request, AXIS_WEB_ACTIVITY_TOPIC_ALL);
	ADD(content, lists(work, admin));
	return axis_web_page_respond(request, "Activity", HTMX_NODE(content));
}

static HtmxResponse *
handle_activity(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	Work *work = g_new0(Work, 1);

	work->web = axis_web_from_request(request);
	work->part = g_strcmp0(htmx_request_get_query_param(request, "part"), "list") == 0;
	return axis_web_jobs_dispatch(user_data, request, work_run, work_build, work, work_free);
}

void
axis_web_jobs_activity_register(
	AxisWeb     *web,
	AxisWebJobs *jobs
){
	axis_web_get(web, AXIS_JOBS_ACTIVITY_PATH, handle_activity, jobs);
}
