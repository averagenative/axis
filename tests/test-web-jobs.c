/*
 * test-web-jobs.c - the Jobs pages: listing, filters, paging, failure
 * detail, live updates over a real event stream, cancel and retry, the
 * activity count in the header, and that all of it is escaped
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test makes its own database, migrates it and runs the real shell,
 * a real hub, the real Jobs pages and (where a test needs jobs that run)
 * a real job queue with a probe handler, on an ephemeral loopback port.
 * Sessions are opened in an auth without a database.  Jobs the listing
 * tests need are inserted straight into the table, in whatever state the
 * test wants, so that they hold still.  Every wait is for a condition,
 * bounded.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-jobs-data.h"

#define MARKUP_ERROR  "<script>alert(1)</script> & \"quoted\" 'single' <b>bold</b>"
#define MARKUP_KIND   "<img src=x onerror=alert(2)>"
#define MARKUP_QUEUE  "<i>q</i>"

enum
{
	WITH_QUEUE  = 1 << 0,	/* a started job queue, attached to the pages */
	NO_HUB      = 1 << 1,	/* no hub: pages are only ever static */
	TWO_WORKERS = 1 << 2	/* two workers on the "test" queue */
};

/* What the kind whose owner asks for its work again says. */
#define ELSEWHERE_TEXT "Ask for the test work again on its own page."

/*
 * The probe: one handler for the test kind, doing what the payload's
 * "mode" says.
 */
typedef struct
{
	GMutex   lock;
	gboolean gate_open;
	gdouble  progress;	/* what "gate" jobs report; < 0 for nothing yet */
	gchar   *message;
	guint    started;
} Probe;

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisJobRegistry    *job_registry;
	AxisJobQueue       *queue;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisWebHub         *hub;
	AxisWebJobs        *jobs;
	SoupSession        *session;
	Probe               probe;
	guint16             port;
	gchar              *admin;	/* "axis_session=..." */
	gchar              *viewer;
	gboolean            started;
} Fixture;

static gboolean
probe_handler(
	AxisJobContext  *context,
	gpointer         user_data,
	GError         **error
){
	Probe *probe = user_data;
	JsonObject *payload = axis_job_context_get_payload(context);
	const gchar *mode = json_object_get_string_member_with_default(payload, "mode", "ok");
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;
	gdouble reported = -1;

	g_mutex_lock(&probe->lock);
	probe->started++;
	g_mutex_unlock(&probe->lock);

	if (strcmp(mode, "ok") == 0)
		return TRUE;

	if (strcmp(mode, "fail") == 0) {
		g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, MARKUP_ERROR);
		return FALSE;
	}

	/* "gate": runs until the gate opens or it is cancelled, passing on
	 * the progress the test sets; "stubborn" does not hear a cancel. */
	for (;;) {
		gboolean open;
		gdouble progress;
		g_autofree gchar *message = NULL;

		g_mutex_lock(&probe->lock);
		open = probe->gate_open;
		progress = probe->progress;
		message = g_strdup(probe->message);
		g_mutex_unlock(&probe->lock);

		if (open)
			return TRUE;
		if (strcmp(mode, "gate") == 0 && axis_job_context_is_cancelled(context)) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_CANCELLED, "stopped");
			return FALSE;
		}
		if (g_get_monotonic_time() > deadline) {
			g_set_error_literal(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_PERMANENT, "test timed out");
			return FALSE;
		}
		if (progress >= 0 && progress != reported) {
			axis_job_context_set_progress(context, progress, message);
			reported = progress;
		}
		axis_job_context_wait(context, 20);
	}
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);
	AxisJobHandlerInfo handler;
	GError *error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 16);
	g_mutex_init(&fx->probe.lock);
	fx->probe.progress = -1;

	fx->job_registry = axis_job_registry_new();
	memset(&handler, 0, sizeof(handler));
	handler.kind = "test.probe";
	handler.queue = "test";
	handler.max_attempts = 1;
	handler.handler = probe_handler;
	g_assert_true(axis_job_registry_add(fx->job_registry, &handler, &fx->probe, &error));
	g_assert_no_error(error);
	/* Two that say what they are: one a person would watch, one upkeep. */
	handler.kind = "test.scan";
	handler.label = "Scanning a test folder";
	handler.job_class = AXIS_JOB_CLASS_WORK;
	g_assert_true(axis_job_registry_add(fx->job_registry, &handler, &fx->probe, &error));
	handler.kind = "test.upkeep";
	handler.label = "Checking the test clients";
	handler.job_class = AXIS_JOB_CLASS_UPKEEP;
	g_assert_true(axis_job_registry_add(fx->job_registry, &handler, &fx->probe, &error));
	g_assert_no_error(error);
	/* One whose owner asks for its work again, as an import's does. */
	handler.kind = "test.owned";
	handler.label = "Doing a test owner's work";
	handler.job_class = AXIS_JOB_CLASS_WORK;
	g_assert_true(axis_job_registry_add(fx->job_registry, &handler, &fx->probe, &error));
	g_assert_true(axis_job_registry_set_retry_elsewhere(fx->job_registry, "test.owned", ELSEWHERE_TEXT, &error));
	g_assert_no_error(error);

	fx->queue = axis_job_queue_new(fx->pool, fx->job_registry);
	axis_job_queue_set_poll_interval(fx->queue, 100);
	axis_job_queue_set_lease(fx->queue, 1500);
	axis_job_queue_set_sweep_interval(fx->queue, 100);
	axis_job_queue_set_schedule_interval(fx->queue, 100);
	if (flags & TWO_WORKERS)
		axis_job_queue_set_concurrency(fx->queue, "test", 2);

	if (!(flags & NO_HUB))
		fx->hub = axis_web_hub_new(NULL);
	fx->jobs = axis_web_jobs_new(fx->pool, fx->hub);
	axis_web_jobs_set_kinds(fx->jobs, fx->job_registry);
	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_jobs_get_module(), fx->jobs,
		&error));
	g_assert_no_error(error);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	if (fx->hub != NULL)
		axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);

	if (flags & WITH_QUEUE) {
		g_assert_true(axis_job_queue_start(fx->queue, &error));
		g_assert_no_error(error);
		fx->started = TRUE;
		g_assert_true(axis_web_jobs_attach(fx->jobs, fx->queue, &error));
		g_assert_no_error(error);
	}

	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}

	g_mutex_lock(&fx->probe.lock);
	fx->probe.gate_open = TRUE;
	g_mutex_unlock(&fx->probe.lock);

	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_jobs_stop(fx->jobs);
	if (fx->started) {
		axis_job_queue_stop(fx->queue, 5000);
		g_assert_true(axis_job_queue_wait_threads(fx->queue, AXIS_TEST_WAIT_MS));
	}
	g_object_unref(fx->queue);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	if (fx->hub != NULL)
		axis_web_hub_free(fx->hub);
	axis_web_jobs_free(fx->jobs);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_job_registry_unref(fx->job_registry);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_mutex_clear(&fx->probe.lock);
	g_free(fx->probe.message);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(Fixture *fx, const gchar *cookie, const gchar *path)
{
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

static void
assert_contains(const gchar *haystack, const gchar *needle)
{
	if (strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack);
}

static void
assert_lacks(const gchar *haystack, const gchar *needle)
{
	if (strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

/* The job ids of the rows in @html, in the order they appear. */
static GArray *
row_ids(const gchar *html)
{
	GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	const gchar *p = html;

	while ((p = strstr(p, "<tr id=\"job-")) != NULL) {
		gint64 id;

		p += strlen("<tr id=\"job-");
		id = (gint64)g_ascii_strtoll(p, NULL, 10);
		g_array_append_val(ids, id);
	}

	return ids;
}

static gboolean
has_row(const gchar *html, gint64 id)
{
	g_autofree gchar *needle = g_strdup_printf("<tr id=\"job-%" G_GINT64_FORMAT "\"", id);

	return strstr(html, needle) != NULL;
}

/* Runs @sql, which ends in RETURNING id, and returns the id. */
static gint64
insert(Fixture *fx, const gchar *sql)
{
	g_autofree gchar *id = axis_test_db_query(fx->db, sql);

	g_assert_nonnull(id);
	return (gint64)g_ascii_strtoll(id, NULL, 10);
}

static gint64
insert_queued(Fixture *fx, const gchar *kind, const gchar *queue)
{
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO job (kind, queue, max_attempts) VALUES ($k$%s$k$, $q$%s$q$, 3) RETURNING id",
		kind, queue);

	return insert(fx, sql);
}

static gint64
insert_running(Fixture *fx, const gchar *kind, const gchar *queue, gdouble progress, const gchar *message)
{
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO job (kind, queue, max_attempts, state, attempts, lease_token, lease_expires_at, "
		"started_at, progress, progress_message) VALUES ($k$%s$k$, $q$%s$q$, 3, 'running', 1, "
		"gen_random_uuid(), now() + interval '1 hour', now(), %f, $m$%s$m$) RETURNING id",
		kind, queue, progress, message);

	return insert(fx, sql);
}

static gint64
insert_finished(Fixture *fx, const gchar *kind, const gchar *queue, const gchar *state, const gchar *error)
{
	g_autofree gchar *error_sql = error != NULL ? g_strdup_printf("$e$%s$e$", error) : g_strdup("NULL");
	g_autofree gchar *sql = g_strdup_printf(
		"INSERT INTO job (kind, queue, max_attempts, state, attempts, started_at, finished_at, "
		"last_error, payload) VALUES ($k$%s$k$, $q$%s$q$, 3, '%s', %d, now() - interval '1 minute', "
		"now(), %s, '{\"path\": \"/media/<movies>\"}') RETURNING id",
		kind, queue, state, strcmp(state, "failed") == 0 ? 3 : 1, error_sql);

	return insert(fx, sql);
}

/* Fails the test unless the first column of the first row of @sql is @expected. */
static void
assert_query(Fixture *fx, const gchar *sql, const gchar *expected)
{
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	g_assert_cmpstr(value, ==, expected);
}

static gchar *
job_state(Fixture *fx, gint64 id)
{
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_db_query(fx->db, sql);
}

static gboolean
state_is(Fixture *fx, gint64 id, const gchar *expected)
{
	g_autofree gchar *state = job_state(fx, id);

	return g_strcmp0(state, expected) == 0;
}

typedef struct
{
	Fixture     *fx;
	gint64       id;
	const gchar *state;
} StateWait;

static gboolean
state_reached(gpointer data)
{
	StateWait *wait = data;

	return state_is(wait->fx, wait->id, wait->state);
}

static void
wait_for_state(Fixture *fx, gint64 id, const gchar *state)
{
	StateWait wait = { fx, id, state };

	if (!axis_test_pump_until(state_reached, &wait, AXIS_TEST_WAIT_MS))
		g_error("job %" G_GINT64_FORMAT " did not become %s", id, state);
}

static gint64
enqueue(Fixture *fx, const gchar *mode)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	gint64 id = 0;

	json_object_set_string_member(payload, "mode", mode);
	g_assert_true(axis_job_queue_enqueue(fx->queue, "test.probe", payload, NULL, &id, NULL, &error));
	g_assert_no_error(error);

	return id;
}

static void
open_gate(Fixture *fx)
{
	g_mutex_lock(&fx->probe.lock);
	fx->probe.gate_open = TRUE;
	g_mutex_unlock(&fx->probe.lock);
}

static void
set_progress(Fixture *fx, gdouble progress, const gchar *message)
{
	g_mutex_lock(&fx->probe.lock);
	fx->probe.progress = progress;
	g_free(fx->probe.message);
	fx->probe.message = g_strdup(message);
	g_mutex_unlock(&fx->probe.lock);
}

/* An attribute value with its "&amp;" undone. */
static gchar *
unescape_attribute(const gchar *raw)
{
	g_auto(GStrv) parts = g_strsplit(raw, "&amp;", -1);

	return g_strjoinv("&", parts);
}

/* The stream a page asks its browser to open: its path and query, with
 * the markup of the attribute undone. */
static gchar *
stream_path_of(const gchar *html)
{
	static const gchar marker[] = "sse-connect=\"";
	const gchar *start = strstr(html, marker);
	const gchar *end;
	g_autofree gchar *raw = NULL;

	g_assert_nonnull(start);
	start += strlen(marker);
	end = strchr(start, '"');
	g_assert_nonnull(end);
	raw = g_strndup(start, (gsize)(end - start));

	return unescape_attribute(raw);
}

/* A stream opened the way the page's browser would. */
static AxisTestStream *
stream_for_page(Fixture *fx, const gchar *cookie, const gchar *page_path)
{
	AxisTestReply page = get_as(fx, cookie, page_path);
	g_autofree gchar *path = stream_path_of(page.body);
	AxisTestStream *stream = axis_test_stream_open(fx->port, path, cookie, 0);

	g_assert_cmpuint(axis_test_stream_get_status(stream), ==, 200);
	g_assert_true(axis_test_stream_wait_for(stream, ": open", AXIS_TEST_WAIT_MS));
	axis_test_reply_clear(&page);

	return stream;
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *csrf,
	gboolean     htmx,
	const gchar *target
){
	const gchar *extra[8];
	guint n = 0;

	if (csrf != NULL) {
		extra[n++] = "X-CSRF-Token";
		extra[n++] = csrf;
	}
	if (htmx) {
		extra[n++] = "HX-Request";
		extra[n++] = "true";
	}
	if (target != NULL) {
		extra[n++] = "HX-Target";
		extra[n++] = target;
	}
	extra[n] = NULL;

	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, extra, NULL);
}

static gchar *
csrf_for(Fixture *fx, const gchar *cookie)
{
	AxisTestReply page = get_as(fx, cookie, "/jobs");
	gchar *token = axis_test_web_csrf_token(page.body);

	g_assert_nonnull(token);
	axis_test_reply_clear(&page);

	return token;
}

/* The page has no inline script, no style element and no style attribute,
 * and no markup that came from the database. */
static void
assert_escaped(const gchar *html)
{
	assert_lacks(html, "<script>alert");
	assert_lacks(html, "<img src=x");
	assert_lacks(html, "<b>bold</b>");
	assert_lacks(html, "<i>q</i>");
	assert_lacks(html, "onerror=alert(2)>");
	assert_lacks(html, "<style");
	assert_lacks(html, " style=");
}

/* ── Listing ────────────────────────────────────────────────────── */

static void
test_listing(Fixture *fx, gconstpointer data)
{
	gint64 queued;
	gint64 running;
	gint64 failed;
	gint64 done;
	AxisTestReply page;
	g_autoptr(GArray) ids = NULL;

	SKIP_WITHOUT_DB(fx);
	queued = insert_queued(fx, "media.scan", "media");
	running = insert_running(fx, "media.transcode", "convert", 0.4, "encoding frames");
	done = insert_finished(fx, "media.scan", "media", "succeeded", NULL);
	failed = insert_finished(fx, "media.probe", "media", "failed", "no such file");

	page = get_as(fx, fx->viewer, "/jobs");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>Jobs</h1>");

	/* Running and queued jobs first, then the finished ones, newest
	 * first in each group. */
	ids = row_ids(page.body);
	g_assert_cmpuint(ids->len, ==, 4);
	g_assert_cmpint(g_array_index(ids, gint64, 0), ==, running);
	g_assert_cmpint(g_array_index(ids, gint64, 1), ==, queued);
	g_assert_cmpint(g_array_index(ids, gint64, 2), ==, failed);
	g_assert_cmpint(g_array_index(ids, gint64, 3), ==, done);

	assert_contains(page.body, "<code>media.transcode</code>");
	assert_contains(page.body, "badge-running");
	assert_contains(page.body, "badge-queued");
	assert_contains(page.body, "badge-failed");
	assert_contains(page.body, "badge-succeeded");
	assert_contains(page.body, "<progress");
	assert_contains(page.body, "40%");
	assert_contains(page.body, "encoding frames");
	assert_contains(page.body, "3 / 3");
	assert_contains(page.body, "no such file");
	/* The table declares its kind and roles; the attempts are a figure. */
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 1);
	assert_contains(page.body, "<td class=\"col-number\" data-label=\"Attempts\">3 / 3</td>");

	/* Viewing is for everyone, acting is not: no buttons, no column. */
	assert_lacks(page.body, "hx-post");
	assert_lacks(page.body, "Actions");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/jobs");
	assert_contains(page.body, "Actions");
	assert_contains(page.body, "hx-post");
	/* The actions are a column of their own at the right edge. */
	assert_contains(page.body, "<th class=\"col-actions\" scope=\"col\">Actions</th>");
	assert_contains(page.body, "<td class=\"job-actions col-actions\" data-label=\"Actions\">");
	axis_test_web_assert_tables(page.body);
	axis_test_reply_clear(&page);
}

/* How many job rows a page lists. */
static guint
rows_listed(const gchar *html)
{
	guint count = 0;
	const gchar *p = html;

	while ((p = strstr(p, "<tr id=\"job-")) != NULL) {
		count++;
		p++;
	}
	return count;
}

/* Spec (web-shell): Figures agree.  The dashboard's Now card: its running
 * and queued figures are the lengths of the lists they link to; the
 * running jobs with their progress; the last failures, linked. */
static void
test_dashboard_now(Fixture *fx, gconstpointer data)
{
	AxisTestReply board;
	AxisTestReply list;
	gint64 failed;
	g_autofree gchar *failure = NULL;

	SKIP_WITHOUT_DB(fx);

	/* Nothing yet: zero and zero, and it says so. */
	board = get_as(fx, fx->viewer, "/");
	assert_contains(board.body, "id=\"dash-now\"");
	assert_contains(board.body, "<a href=\"/jobs\">Running now</a>");
	assert_contains(board.body, "No jobs are running or queued.");
	axis_test_reply_clear(&board);

	insert_running(fx, "media.transcode", "convert", 0.4, "encoding frames");
	insert_running(fx, "library.scan", "scan", 0.1, "walking");
	insert_queued(fx, "probe.file", "probe");
	insert_queued(fx, "probe.file", "probe");
	insert_queued(fx, "probe.file", "probe");
	failed = insert_finished(fx, "probe.file", "probe", "failed", "no such file");
	insert_finished(fx, "probe.file", "probe", "succeeded", NULL);

	board = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(board.status, ==, 200);
	assert_contains(board.body, "<span class=\"figure-value\">2</span><span class=\"figure-label\">jobs running</span>");
	assert_contains(board.body, "<span class=\"figure-value\">3</span><span class=\"figure-label\">jobs queued</span>");
	assert_contains(board.body, "href=\"/jobs?state=running&amp;class=work\"");
	assert_contains(board.body, "href=\"/jobs?state=queued&amp;class=work\"");
	assert_contains(board.body, "<code>media.transcode</code>");
	assert_contains(board.body, "40%");
	assert_contains(board.body, "Recently failed jobs");
	/* A kind the registry does not name is named by itself, with its number. */
	failure = g_strdup_printf("<a href=\"/jobs/%" G_GINT64_FORMAT "\"><code>probe.file</code></a>"
		"<span class=\"job-kind\">#%" G_GINT64_FORMAT "</span>", failed, failed);
	assert_contains(board.body, failure);
	assert_lacks(board.body, "No jobs are running or queued.");
	assert_lacks(board.body, " style=");
	axis_test_reply_clear(&board);

	/* The pages behind the figures list as many. */
	list = get_as(fx, fx->viewer, "/jobs?state=running&class=work");
	g_assert_cmpuint(rows_listed(list.body), ==, 2);
	axis_test_reply_clear(&list);
	list = get_as(fx, fx->viewer, "/jobs?state=queued&class=work");
	g_assert_cmpuint(rows_listed(list.body), ==, 3);
	axis_test_reply_clear(&list);
}

static void
test_filters(Fixture *fx, gconstpointer data)
{
	gint64 queued;
	gint64 running;
	gint64 failed;
	gint64 failed_other;
	gint64 done;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	queued = insert_queued(fx, "media.scan", "media");
	running = insert_running(fx, "media.transcode", "convert", 0.4, "working");
	done = insert_finished(fx, "media.scan", "media", "succeeded", NULL);
	failed = insert_finished(fx, "media.probe", "media", "failed", "no such file");
	failed_other = insert_finished(fx, "media.convert", "convert", "failed", "disk full");

	/* Only failed jobs. */
	page = get_as(fx, fx->viewer, "/jobs?state=failed");
	g_assert_cmpuint(page.status, ==, 200);
	g_assert_true(has_row(page.body, failed));
	g_assert_true(has_row(page.body, failed_other));
	g_assert_false(has_row(page.body, queued));
	g_assert_false(has_row(page.body, running));
	g_assert_false(has_row(page.body, done));
	/* The form shows what is selected. */
	assert_contains(page.body, "<option selected=\"selected\" value=\"failed\"");
	axis_test_reply_clear(&page);

	/* A queue. */
	page = get_as(fx, fx->viewer, "/jobs?queue=convert");
	g_assert_true(has_row(page.body, running));
	g_assert_true(has_row(page.body, failed_other));
	g_assert_false(has_row(page.body, queued));
	g_assert_false(has_row(page.body, failed));
	assert_contains(page.body, "<option selected=\"selected\" value=\"convert\"");
	/* (tidy-labels) A queue is named in plain words, its own name beside it. */
	assert_contains(page.body, ">Placing converted files (convert)</option>");
	assert_contains(page.body, ">Work or upkeep</label>");
	axis_test_reply_clear(&page);

	/* Both together. */
	page = get_as(fx, fx->viewer, "/jobs?state=failed&queue=media");
	g_assert_true(has_row(page.body, failed));
	g_assert_false(has_row(page.body, failed_other));
	g_assert_false(has_row(page.body, done));
	axis_test_reply_clear(&page);

	/* Active: queued and running. */
	page = get_as(fx, fx->viewer, "/jobs?state=active");
	g_assert_true(has_row(page.body, queued));
	g_assert_true(has_row(page.body, running));
	g_assert_false(has_row(page.body, failed));
	g_assert_false(has_row(page.body, done));
	axis_test_reply_clear(&page);

	/* Nothing matches. */
	page = get_as(fx, fx->viewer, "/jobs?state=cancelled");
	assert_contains(page.body, "No work jobs match these filters.");
	assert_contains(page.body, "No upkeep jobs match these filters.");
	assert_contains(page.body, ">Show all jobs</a>");
	assert_lacks(page.body, "<table");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/jobs?state=cancelled&class=upkeep");
	assert_contains(page.body, "No jobs match these filters.");
	assert_contains(page.body, "<option selected=\"selected\" value=\"upkeep\"");
	axis_test_reply_clear(&page);
}

/* With AXIS_TEST_JOBS_PAGES set to a directory, a page is also saved there
 * for looking at in a browser. */
static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_JOBS_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gint64
enqueue_kind(Fixture *fx, const gchar *kind, const gchar *mode)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	gint64 id = 0;

	json_object_set_string_member(payload, "mode", mode);
	g_assert_true(axis_job_queue_enqueue(fx->queue, kind, payload, NULL, &id, NULL, &error));
	g_assert_no_error(error);

	return id;
}

/*
 * Spec (web-shell): Only work a person would watch counts as running.  With
 * only upkeep running, the header reads 0 and the dashboard says nothing
 * is running; the Jobs page lists the upkeep under its own heading, by its
 * plain name with its kind beside.  A job of work counts, by its name.
 */
static void
test_upkeep_is_not_counted(Fixture *fx, gconstpointer data)
{
	static const gchar *const htmx_headers[] = { "HX-Request", "true", NULL };
	g_autoptr(AxisTestStream) list_stream = NULL;
	g_autoptr(AxisTestStream) home_stream = NULL;
	AxisTestReply page;
	gint64 upkeep;
	gint64 work;

	SKIP_WITHOUT_DB(fx);
	list_stream = stream_for_page(fx, fx->viewer, "/jobs?class=upkeep");

	/* Upkeep runs: the page with it follows, and the count has been taken
	 * again by then (both come from the same read). */
	upkeep = enqueue_kind(fx, "test.upkeep", "gate");
	wait_for_state(fx, upkeep, "running");
	{
		g_autofree gchar *row = g_strdup_printf("<tr id=\"job-%" G_GINT64_FORMAT "\"", upkeep);

		g_assert_true(axis_test_stream_wait_for(list_stream, row, 5000));
		g_assert_true(axis_test_stream_wait_for(list_stream, "badge-running", 5000));
	}
	g_assert_cmpint(axis_web_jobs_get_running(fx->jobs), ==, 0);

	page = get_as(fx, fx->viewer, "/");
	assert_contains(page.body, "<span class=\"activity-count\">0</span><span> jobs running</span>");
	assert_contains(page.body, "<span class=\"figure-value\">0</span><span class=\"figure-label\">jobs running</span>");
	assert_contains(page.body, "No jobs are running or queued.");
	assert_lacks(page.body, "Checking the test clients");
	save_page("dashboard-upkeep.html", page.body);
	axis_test_reply_clear(&page);

	/* The Jobs page: work first, the upkeep under its heading, closed. */
	page = get_as(fx, fx->viewer, "/jobs");
	assert_contains(page.body, "<h2>Work jobs</h2>");
	assert_contains(page.body, "No work jobs yet.");
	assert_contains(page.body, "<details id=\"jobs-upkeep\" class=\"card upkeep\">");
	assert_contains(page.body, "<span id=\"upkeep-counts\" class=\"upkeep-counts\">1 running</span>");
	assert_contains(page.body, ">Checking the test clients</a>");
	assert_contains(page.body, "<span class=\"job-kind\"><code>test.upkeep</code>");
	g_assert_true(has_row(page.body, upkeep));
	g_assert_true(strstr(page.body, "id=\"jobs-list\"") < strstr(page.body, "id=\"jobs-upkeep\""));
	axis_test_reply_clear(&page);

	/* By class. */
	page = get_as(fx, fx->viewer, "/jobs?class=work");
	g_assert_false(has_row(page.body, upkeep));
	assert_lacks(page.body, "id=\"jobs-upkeep\"");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/jobs?class=upkeep");
	g_assert_true(has_row(page.body, upkeep));
	assert_lacks(page.body, "id=\"jobs-upkeep\"");
	axis_test_reply_clear(&page);

	/* The brief list's refresh brings its heading's counts along. */
	page = axis_test_web_request(fx->session, fx->port, "GET", "/jobs/list?class=upkeep&brief=1", fx->viewer,
		htmx_headers, NULL);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<div id=\"upkeep-list\"");
	assert_contains(page.body, "hx-swap-oob=\"true\"");
	assert_contains(page.body, "1 running");
	g_assert_true(has_row(page.body, upkeep));
	axis_test_reply_clear(&page);

	/* Its page names it in plain words and says it is upkeep. */
	{
		g_autofree gchar *path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, upkeep);

		page = get_as(fx, fx->viewer, path);
		assert_contains(page.body, "<h2>Checking the test clients</h2>");
		assert_contains(page.body, "<code>test.upkeep</code>");
		assert_contains(page.body, "Upkeep: a check or clean-up Axis repeats by itself");
		save_page("job-upkeep.html", page.body);
		axis_test_reply_clear(&page);
	}

	open_gate(fx);
	wait_for_state(fx, upkeep, "succeeded");

	/* Work counts, by its name. */
	g_mutex_lock(&fx->probe.lock);
	fx->probe.gate_open = FALSE;
	g_mutex_unlock(&fx->probe.lock);
	home_stream = stream_for_page(fx, fx->viewer, "/");
	work = enqueue_kind(fx, "test.scan", "gate");
	g_assert_true(axis_test_stream_wait_for(home_stream,
		"<span class=\"activity-count\">1</span><span> job running</span>", 5000));
	g_assert_cmpint(axis_web_jobs_get_running(fx->jobs), ==, 1);
	page = get_as(fx, fx->viewer, "/");
	assert_contains(page.body, ">Scanning a test folder</a>");
	assert_contains(page.body, "<code>test.scan</code>");
	assert_lacks(page.body, "No jobs are running or queued.");
	save_page("dashboard-work.html", page.body);
	axis_test_reply_clear(&page);
	open_gate(fx);
	wait_for_state(fx, work, "succeeded");

	/* For looking at: the page with work and upkeep of every sort. */
	if (g_getenv("AXIS_TEST_JOBS_PAGES") != NULL) {
		guint i;

		insert_running(fx, "test.scan", "test", 0.62, "Movies/Some Film (2019)");
		insert_finished(fx, "test.scan", "test", "failed", "the volume Disk two went offline");
		for (i = 0; i < 6; i++)
			insert_finished(fx, "test.upkeep", "maintenance", "succeeded", NULL);
		insert_running(fx, "test.upkeep", "downloads", 0.3, "Following 1 download client");
		page = get_as(fx, fx->admin, "/jobs");
		save_page("jobs.html", page.body);
		axis_test_reply_clear(&page);
		page = get_as(fx, fx->admin, "/jobs?class=upkeep");
		save_page("jobs-upkeep.html", page.body);
		axis_test_reply_clear(&page);
		page = get_as(fx, fx->viewer, "/jobs");
		save_page("jobs-viewer.html", page.body);
		axis_test_reply_clear(&page);
	}
}

static void
test_bad_filters_are_refused(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad[] = {
		"state=exploded", "state=failed%27%20OR%201%3D1", "queue=Bad%20Queue", "queue=%3Cb%3E",
		"before=12", "before=a", "before=a0", "before=x5", "before=a1%27", "before=a99999999999999999999",
		NULL
	};
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; bad[i] != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/jobs?%s", bad[i]);
		AxisTestReply page = get_as(fx, fx->viewer, path);

		if (page.status != 400)
			g_error("'%s' answered %u", bad[i], page.status);
		axis_test_reply_clear(&page);
	}
}

static void
test_paging(Fixture *fx, gconstpointer data)
{
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *path = g_strdup("/jobs");
	guint pages = 0;
	guint total = 0;

	SKIP_WITHOUT_DB(fx);

	/* 30 unfinished jobs and 90 finished ones: the keyset crosses from
	 * one group to the other in the middle of a page. */
	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, max_attempts, state, attempts, started_at, finished_at) "
		"SELECT 'media.scan', 'media', 3, 'succeeded', 1, now(), now() FROM generate_series(1, 90)");
	axis_test_db_exec(fx->db,
		"INSERT INTO job (kind, queue, max_attempts) SELECT 'media.scan', 'media', 3 "
		"FROM generate_series(1, 30)");

	for (;;) {
		AxisTestReply page = get_as(fx, fx->viewer, path);
		g_autoptr(GArray) ids = row_ids(page.body);
		const gchar *older = strstr(page.body, "Older jobs");
		guint i;

		g_assert_cmpuint(page.status, ==, 200);
		pages++;
		for (i = 0; i < ids->len; i++) {
			gint64 *key = g_new(gint64, 1);

			*key = g_array_index(ids, gint64, i);
			if (g_hash_table_contains(seen, key))
				g_error("job %" G_GINT64_FORMAT " listed twice", *key);
			g_hash_table_add(seen, key);
		}
		total += ids->len;

		if (pages < 3) {
			g_assert_cmpuint(ids->len, ==, AXIS_JOBS_PAGE_SIZE);
			g_assert_nonnull(older);
		}
		if (pages > 1)
			assert_contains(page.body, "Newest jobs");

		if (older == NULL) {
			axis_test_reply_clear(&page);
			break;
		}
		{
			/* The link is an ordinary href, with the markup undone. */
			const gchar *href = older;
			g_autofree gchar *raw = NULL;
			const gchar *start = older;

			while (start > page.body && strncmp(start, "href=\"", 6) != 0)
				start--;
			href = start + 6;
			raw = g_strndup(href, (gsize)(strchr(href, '"') - href));
			g_free(path);
			path = unescape_attribute(raw);
		}
		axis_test_reply_clear(&page);
		g_assert_cmpuint(pages, <, 10);
	}

	g_assert_cmpuint(pages, ==, 3);
	g_assert_cmpuint(total, ==, 120);
	g_assert_cmpuint(g_hash_table_size(seen), ==, 120);
}

static void
test_failure_detail(Fixture *fx, gconstpointer data)
{
	gint64 failed;
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	failed = insert_finished(fx, "media.probe", "media", "failed", MARKUP_ERROR);
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, failed);

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<title>Job ");
	assert_contains(page.body, "id=\"job-detail\"");
	assert_contains(page.body, "<code>media.probe</code>");
	assert_contains(page.body, "badge-failed");
	assert_contains(page.body, "3 of 3");
	assert_contains(page.body, "Last error");

	/* The whole error text, as text. */
	assert_contains(page.body,
		"&lt;script&gt;alert(1)&lt;/script&gt; &amp; &quot;quoted&quot; &#39;single&#39; &lt;b&gt;bold&lt;/b&gt;");
	assert_escaped(page.body);

	/* A payload may hold secrets: only administrators see it. */
	assert_lacks(page.body, "/media/");
	assert_lacks(page.body, "Job input (payload)");
	assert_lacks(page.body, "hx-post");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, path);
	assert_contains(page.body, "id=\"job-payload\"");
	assert_contains(page.body, "/media/&lt;movies&gt;");
	assert_contains(page.body, "hx-post=\"/jobs/");
	assert_contains(page.body, "Retry job");
	assert_escaped(page.body);
	axis_test_reply_clear(&page);
}

static void
test_markup_from_the_database_is_escaped(Fixture *fx, gconstpointer data)
{
	gint64 running;
	gint64 failed;
	g_autofree gchar *detail = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	running = insert_running(fx, MARKUP_KIND, MARKUP_QUEUE, 0.5,
		"<script>alert(3)</script> step");
	failed = insert_finished(fx, MARKUP_KIND, MARKUP_QUEUE, "failed", MARKUP_ERROR);

	page = get_as(fx, fx->admin, "/jobs");
	g_assert_true(has_row(page.body, running));
	g_assert_true(has_row(page.body, failed));
	assert_contains(page.body, "&lt;img src=x onerror=alert(2)&gt;");
	assert_contains(page.body, "&lt;script&gt;alert(3)&lt;/script&gt; step");
	assert_lacks(page.body, "<script>alert");
	assert_escaped(page.body);
	axis_test_reply_clear(&page);

	/* The queue name is also a filter, and the filter is checked. */
	page = get_as(fx, fx->admin, "/jobs?queue=%3Ci%3Eq%3C%2Fi%3E");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);

	detail = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, running);
	page = get_as(fx, fx->admin, detail);
	assert_contains(page.body, "&lt;img src=x onerror=alert(2)&gt;");
	/* The queue is one of its facts. */
	assert_contains(page.body, "&lt;i&gt;q&lt;/i&gt;");
	assert_escaped(page.body);
	axis_test_reply_clear(&page);
}

static void
test_detail_refetches_when_the_stream_opens(Fixture *fx, gconstpointer data)
{
	gint64 failed;
	g_autofree gchar *path = NULL;
	g_autofree gchar *refresh = NULL;
	g_autofree gchar *attribute = NULL;
	static const gchar *const htmx_headers[] = { "HX-Request", "true", NULL };
	AxisTestReply page;
	AxisTestReply fragment;
	AxisTestReply plain;

	SKIP_WITHOUT_DB(fx);
	failed = insert_finished(fx, "media.probe", "media", "failed", MARKUP_ERROR);
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, failed);
	refresh = g_strdup_printf("%s/detail", path);
	attribute = g_strdup_printf("hx-get=\"%s\"", refresh);

	/* After a break in the stream the page fetches what it missed. */
	page = get_as(fx, fx->viewer, path);
	assert_contains(page.body, attribute);
	assert_contains(page.body, "hx-trigger=\"htmx:sseOpen from:body\"");
	axis_test_reply_clear(&page);

	fragment = axis_test_web_request(fx->session, fx->port, "GET", refresh, fx->viewer,
		htmx_headers, NULL);
	g_assert_cmpuint(fragment.status, ==, 200);
	g_assert_true(g_str_has_prefix(fragment.body, "<section id=\"job-detail\""));
	assert_lacks(fragment.body, "<html");
	assert_contains(fragment.body, "3 of 3");
	assert_escaped(fragment.body);
	axis_test_reply_clear(&fragment);

	plain = get_as(fx, fx->viewer, refresh);
	g_assert_cmpuint(plain.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&plain, "Location"), ==, path);
	axis_test_reply_clear(&plain);

	plain = get_as(fx, NULL, refresh);
	g_assert_cmpuint(plain.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&plain, "Location"), !=, path);
	axis_test_reply_clear(&plain);
}

static void
test_unknown_job(Fixture *fx, gconstpointer data)
{
	static const gchar *const paths[] = {
		"/jobs/987654", "/jobs/abc", "/jobs/0", "/jobs/007", "/jobs/-1", "/jobs/99999999999999999999",
		"/jobs/1%27", NULL
	};
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; paths[i] != NULL; i++) {
		AxisTestReply page = get_as(fx, fx->viewer, paths[i]);

		if (page.status != 404)
			g_error("'%s' answered %u", paths[i], page.status);
		axis_test_reply_clear(&page);
	}
}

static void
test_pages_need_a_session(Fixture *fx, gconstpointer data)
{
	AxisTestReply list;
	AxisTestReply fragment;
	AxisTestReply detail;

	SKIP_WITHOUT_DB(fx);
	insert_queued(fx, "media.scan", "media");
	list = get_as(fx, NULL, "/jobs");
	fragment = get_as(fx, NULL, "/jobs/list");
	detail = get_as(fx, NULL, "/jobs/1");

	g_assert_cmpuint(list.status, ==, 303);
	g_assert_cmpuint(fragment.status, ==, 303);
	g_assert_cmpuint(detail.status, ==, 303);
	assert_lacks(list.body, "media.scan");

	axis_test_reply_clear(&list);
	axis_test_reply_clear(&fragment);
	axis_test_reply_clear(&detail);
}

static void
test_correct_without_a_stream(Fixture *fx, gconstpointer data)
{
	gint64 running;
	AxisTestReply page;
	AxisTestReply refresh;
	static const gchar *const htmx_headers[] = { "HX-Request", "true", NULL };

	SKIP_WITHOUT_DB(fx);
	running = insert_running(fx, "media.transcode", "convert", 0.4, "encoding frames");
	insert_finished(fx, "media.probe", "media", "failed", "no such file");

	/* No hub, no events: the page is the state of the jobs as of load. */
	page = get_as(fx, fx->viewer, "/jobs");
	g_assert_cmpuint(page.status, ==, 200);
	g_assert_true(has_row(page.body, running));
	assert_contains(page.body, "40%");
	assert_contains(page.body, "no such file");
	assert_lacks(page.body, "sse-connect");
	assert_lacks(page.body, "sse-swap");
	axis_test_reply_clear(&page);

	/* The fragment a refresh would fetch is the same table. */
	refresh = axis_test_web_request(fx->session, fx->port, "GET", "/jobs/list", fx->viewer,
		htmx_headers, NULL);
	g_assert_cmpuint(refresh.status, ==, 200);
	assert_contains(refresh.body, "<div id=\"jobs-list\"");
	assert_lacks(refresh.body, "<html");
	assert_lacks(refresh.body, "<title");
	g_assert_true(has_row(refresh.body, running));
	axis_test_reply_clear(&refresh);
}

/* ── Live updates ───────────────────────────────────────────────── */

static void
test_pages_follow_the_right_topics(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *detail = NULL;
	AxisTestReply page;
	AxisTestReply admin_page;
	AxisTestReply other;

	SKIP_WITHOUT_DB(fx);
	id = insert_queued(fx, "media.scan", "media");
	detail = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, id);

	page = get_as(fx, fx->viewer, "/jobs");
	admin_page = get_as(fx, fx->admin, "/jobs");
	assert_contains(page.body, "sse-connect=\"/events?topic=jobs&amp;topic=activity\"");
	assert_contains(admin_page.body, "sse-connect=\"/events?topic=jobs:admin&amp;topic=activity\"");
	/* The list fetches itself again when told to and when the stream opens. */
	assert_contains(page.body, "hx-trigger=\"sse:jobs-changed throttle:1s, htmx:sseOpen from:body\"");
	assert_contains(page.body, "hx-get=\"/jobs/list?class=work\"");
	assert_contains(page.body, "hx-get=\"/jobs/list?class=upkeep&amp;brief=1\"");
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&admin_page);

	page = get_as(fx, fx->viewer, detail);
	admin_page = get_as(fx, fx->admin, detail);
	{
		g_autofree gchar *viewer_topic = g_strdup_printf("topic=job:%" G_GINT64_FORMAT "&amp;", id);
		g_autofree gchar *admin_topic = g_strdup_printf("topic=job:%" G_GINT64_FORMAT ":admin&amp;", id);

		assert_contains(page.body, viewer_topic);
		assert_contains(admin_page.body, admin_topic);
	}
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&admin_page);

	/* Every page, not only these, carries the activity stream. */
	other = get_as(fx, fx->viewer, "/");
	assert_contains(other.body, "sse-connect=\"/events?topic=activity\"");
	axis_test_reply_clear(&other);
}

static void
test_live_progress(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autoptr(AxisTestStream) viewer = NULL;
	g_autoptr(AxisTestStream) admin = NULL;
	g_autofree gchar *row = NULL;
	gint64 started;
	gint64 elapsed_ms;

	SKIP_WITHOUT_DB(fx);
	id = enqueue(fx, "gate");
	wait_for_state(fx, id, "running");
	row = g_strdup_printf("<tr id=\"job-%" G_GINT64_FORMAT "\"", id);

	viewer = stream_for_page(fx, fx->viewer, "/jobs");
	admin = stream_for_page(fx, fx->admin, "/jobs");

	/* The job reports progress while both pages are open. */
	started = g_get_monotonic_time();
	set_progress(fx, 0.5, "halfway there");
	g_assert_true(axis_test_stream_wait_for(viewer, "halfway there", 5000));
	elapsed_ms = (g_get_monotonic_time() - started) / 1000;
	g_assert_true(axis_test_stream_wait_for(admin, "halfway there", 5000));

	assert_contains(axis_test_stream_get_text(viewer), "event: oob\n");
	assert_contains(axis_test_stream_get_text(viewer), row);
	assert_contains(axis_test_stream_get_text(viewer), "hx-swap-oob=\"true\"");
	assert_contains(axis_test_stream_get_text(viewer), "50%");
	assert_contains(axis_test_stream_get_text(viewer), "<progress");
	/* What reaches an administrator carries the buttons; the other does not. */
	assert_contains(axis_test_stream_get_text(admin), "hx-post=\"/jobs/");
	assert_lacks(axis_test_stream_get_text(viewer), "hx-post");
	/* The row, and not only a message: nothing but the row's own markup. */
	assert_lacks(axis_test_stream_get_text(viewer), "<html");

	g_test_message("progress reached the page %" G_GINT64_FORMAT " ms after it was reported", elapsed_ms);
	g_assert_cmpint(elapsed_ms, <, 2000);

	/* And again. */
	set_progress(fx, 0.75, "nearly");
	g_assert_true(axis_test_stream_wait_for(viewer, "nearly", 5000));
	assert_contains(axis_test_stream_get_text(viewer), "75%");
}

static void
test_live_detail(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *path = NULL;
	g_autoptr(AxisTestStream) stream = NULL;

	SKIP_WITHOUT_DB(fx);
	id = enqueue(fx, "gate");
	wait_for_state(fx, id, "running");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, id);
	stream = stream_for_page(fx, fx->viewer, path);

	set_progress(fx, 0.25, "quarter");
	g_assert_true(axis_test_stream_wait_for(stream, "quarter", 5000));
	assert_contains(axis_test_stream_get_text(stream), "<section id=\"job-detail\"");
	assert_contains(axis_test_stream_get_text(stream), "25%");

	/* The end of the job arrives too. */
	open_gate(fx);
	g_assert_true(axis_test_stream_wait_for(stream, "badge-succeeded", 5000));
}

static void
test_list_refreshes_when_jobs_change(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) stream = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	stream = stream_for_page(fx, fx->viewer, "/jobs");

	/* A job that was not on the page is no row to replace: the page is
	 * told to fetch its list again. */
	id = enqueue(fx, "gate");
	g_assert_true(axis_test_stream_wait_for(stream, "event: jobs-changed\ndata: refresh\n\n", 5000));
	wait_for_state(fx, id, "running");
}

static void
test_progress_is_coalesced(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autoptr(AxisTestStream) stream = NULL;
	g_autofree gchar *row = NULL;
	gint64 until;
	guint emitted = 0;
	guint seen;

	SKIP_WITHOUT_DB(fx);
	id = insert_running(fx, "media.transcode", "convert", 0.1, "start");
	row = g_strdup_printf("<tr id=\"job-%" G_GINT64_FORMAT "\"", id);
	stream = stream_for_page(fx, fx->viewer, "/jobs");

	/* A job that reports progress a hundred times a second for a second
	 * and a half is shown no more than twice a second. */
	until = g_get_monotonic_time() + 1500 * 1000;
	while (g_get_monotonic_time() < until) {
		AxisJobEvent event;
		g_autofree gchar *sql = g_strdup_printf(
			"UPDATE job SET progress = %f WHERE id = %" G_GINT64_FORMAT, 0.1 + emitted / 1000.0, id);

		axis_test_db_exec(fx->db, sql);
		memset(&event, 0, sizeof(event));
		event.type = AXIS_JOB_EVENT_PROGRESS;
		event.id = id;
		event.kind = (gchar *)"media.transcode";
		event.queue = (gchar *)"convert";
		event.state = AXIS_JOB_STATE_RUNNING;
		event.attempts = 1;
		event.max_attempts = 3;
		event.progress = 0.1 + emitted / 1000.0;
		g_signal_emit_by_name(fx->queue, "job-event", &event);
		emitted++;
		axis_test_pump_for(10);
	}
	axis_test_pump_for(1200);

	seen = axis_test_stream_count(stream, row);
	g_test_message("%u progress events became %u fragments", emitted, seen);
	g_assert_cmpuint(emitted, >=, 60);
	g_assert_cmpuint(seen, >=, 2);
	g_assert_cmpuint(seen, <=, 6);
}

static void
test_activity_indicator(Fixture *fx, gconstpointer data)
{
	const gchar *none_running = "<span class=\"activity-count\">0</span><span> jobs running</span>";
	g_autoptr(AxisTestStream) stream = NULL;
	AxisTestReply page;
	gint64 until;
	gint64 id;
	guint told;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->viewer, "/");
	assert_contains(page.body, "id=\"activity\"");
	assert_contains(page.body, "<span class=\"activity-count\">0</span><span> jobs running</span>");
	axis_test_reply_clear(&page);

	stream = stream_for_page(fx, fx->viewer, "/");

	/* A job starts while a page is open. */
	id = enqueue(fx, "gate");
	g_assert_true(axis_test_stream_wait_for(stream,
		"<span class=\"activity-count\">1</span><span> job running</span>", 5000));
	assert_contains(axis_test_stream_get_text(stream), "id=\"activity\"");
	assert_contains(axis_test_stream_get_text(stream), "hx-swap-oob=\"true\"");
	g_assert_cmpint(axis_web_jobs_get_running(fx->jobs), ==, 1);

	/* A page loaded now has it in its header, wherever it is. */
	page = get_as(fx, fx->admin, "/account");
	assert_contains(page.body, "<span class=\"activity-count\">1</span><span> job running</span>");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/jobs");
	assert_contains(page.body, "<span class=\"activity-count\">1</span><span> job running</span>");
	/* (add-activity-feedback) Leading to what Axis is doing, in words. */
	assert_contains(page.body, "href=\"/activity\"");
	assert_contains(page.body, "<span class=\"activity-kinds\">: 1 other job</span>");
	axis_test_reply_clear(&page);

	/* And it finishes.  The stream may hold a fragment that says 0 already:
	 * on a busy machine the counts are read once while the job is queued
	 * and not yet running, and the header is told of that too.  The
	 * fragment that tells of the end is one more than those. */
	told = axis_test_stream_count(stream, none_running);
	open_gate(fx);
	wait_for_state(fx, id, "succeeded");
	until = g_get_monotonic_time() + 5000 * 1000;
	while (axis_test_stream_count(stream, none_running) <= told) {
		g_assert_cmpint(g_get_monotonic_time(), <, until);
		axis_test_pump_for(10);
	}
	g_assert_cmpint(axis_web_jobs_get_running(fx->jobs), ==, 0);
}

/*
 * Spec (web-live): A job's change is told to its key family's topic: a
 * page that follows "act:tv.search:7" hears "activity" when a job keyed
 * "tv.search:7:season:1" starts, and the Activity page's topic hears it
 * too; the header names what runs by kind.
 */
static void
test_activity_topics(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) family = NULL;
	g_autoptr(AxisTestStream) all = NULL;
	g_autoptr(AxisTestStream) other = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	AxisJobEnqueueOptions options;
	AxisTestReply page;
	gint64 id = 0;

	SKIP_WITHOUT_DB(fx);
	family = axis_test_stream_open(fx->port, "/events?topic=act:tv.search:7", fx->viewer, 0);
	all = axis_test_stream_open(fx->port, "/events?topic=act:all", fx->viewer, 0);
	other = axis_test_stream_open(fx->port, "/events?topic=act:tv.search:8", fx->viewer, 0);
	g_assert_true(axis_test_stream_wait_for(family, ": open", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(all, ": open", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(other, ": open", AXIS_TEST_WAIT_MS));

	memset(&options, 0, sizeof(options));
	options.dedupe_key = "tv.search:7:season:1";
	json_object_set_string_member(payload, "mode", "gate");
	g_assert_true(axis_job_queue_enqueue(fx->queue, "test.probe", payload, &options, &id, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_test_stream_wait_for(family, "event: activity", 5000));
	g_assert_true(axis_test_stream_wait_for(all, "event: activity", 5000));
	/* Another series' page hears nothing of it. */
	axis_test_pump_for(700);
	assert_lacks(axis_test_stream_get_text(other), "event: activity");

	/* The header: what runs, by kind, linking to the Activity page. */
	g_assert_true(axis_test_stream_wait_for(all, "event: activity", 5000));
	page = get_as(fx, fx->viewer, "/account");
	assert_contains(page.body, "href=\"/activity\"");
	axis_test_reply_clear(&page);
	open_gate(fx);
	wait_for_state(fx, id, "succeeded");
}

/* ── Actions ────────────────────────────────────────────────────── */

static void
test_cancel_running_job(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *path = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *token = NULL;
	g_autoptr(AxisTestStream) stream = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	id = enqueue(fx, "gate");
	wait_for_state(fx, id, "running");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", id);
	target = g_strdup_printf("job-%" G_GINT64_FORMAT, id);
	token = csrf_for(fx, fx->admin);
	stream = stream_for_page(fx, fx->admin, "/jobs");

	reply = post_as(fx, fx->admin, path, token, TRUE, target);
	g_assert_cmpuint(reply.status, ==, 200);

	/* The answer is the row, to replace the one that asked. */
	assert_contains(reply.body, "<tr id=\"job-");
	assert_lacks(reply.body, "<html");
	axis_test_reply_clear(&reply);

	/* The handler notices and the job ends cancelled; the page hears. */
	wait_for_state(fx, id, "cancelled");
	g_assert_true(axis_test_stream_wait_for(stream, "badge-cancelled", 5000));
	assert_contains(axis_test_stream_get_text(stream), "/retry");
}

static void
test_cancel_is_shown_while_the_job_stops(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *path = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *token = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);

	/* A job that takes its time to hear it. */
	id = enqueue(fx, "stubborn");
	wait_for_state(fx, id, "running");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", id);
	target = g_strdup_printf("job-%" G_GINT64_FORMAT, id);
	token = csrf_for(fx, fx->admin);

	reply = post_as(fx, fx->admin, path, token, TRUE, target);
	g_assert_cmpuint(reply.status, ==, 200);

	/* It has been asked; the row says so and offers no second button. */
	assert_contains(reply.body, "<tr id=\"job-");
	assert_contains(reply.body, "badge-running");
	assert_contains(reply.body, "Cancelling");
	assert_lacks(reply.body, "/cancel");
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT cancel_requested::text FROM job ORDER BY id DESC LIMIT 1", "true");

	/* The page shows it too when loaded now. */
	reply = get_as(fx, fx->admin, "/jobs");
	assert_contains(reply.body, "Cancelling");
	axis_test_reply_clear(&reply);
}

static void
test_cancel_queued_job_and_other_shapes(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *path = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *detail_target = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);

	/* Queued for later, so that nothing runs it. */
	id = insert(fx, "INSERT INTO job (kind, queue, max_attempts, run_at) "
		"VALUES ('test.probe', 'test', 1, now() + interval '1 hour') RETURNING id");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", id);
	token = csrf_for(fx, fx->admin);

	/* From the detail page the section is replaced. */
	reply = post_as(fx, fx->admin, path, token, TRUE, "job-detail");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<section id=\"job-detail\"");
	assert_contains(reply.body, "badge-cancelled");
	assert_contains(reply.body, "/retry");
	assert_lacks(reply.body, "/cancel\"");
	axis_test_reply_clear(&reply);
	g_assert_true(state_is(fx, id, "cancelled"));

	/* Without htmx the browser is sent to the job. */
	reply = post_as(fx, fx->admin, path, token, FALSE, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	{
		g_autofree gchar *expected = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, id);

		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, expected);
	}
	axis_test_reply_clear(&reply);

	/* Too late is not an error: the row shows what the job is. */
	reply = post_as(fx, fx->admin, path, token, TRUE, "job-1");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "badge-cancelled");
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/jobs/987654/cancel", token, TRUE, "job-987654");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/* Spec (job-ui): Retry; the earlier run is kept and shown. */
static void
test_retry_failed_job(Fixture *fx, gconstpointer data)
{
	gint64 id;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page_path = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *location = NULL;
	AxisTestReply reply;
	AxisTestReply page;
	const gchar *redirect;

	SKIP_WITHOUT_DB(fx);
	id = insert(fx, "INSERT INTO job (kind, queue, max_attempts, state, attempts, started_at, finished_at, "
		"last_error, worker_id, payload) VALUES ('test.probe', 'test', 1, 'failed', 1, now(), now(), "
		"'the share was away', 'axisd:test/test/1', '{\"mode\": \"gate\", \"n\": 5, \"name\": \"a<b\"}') "
		"RETURNING id");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", id);
	location = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, id);
	token = csrf_for(fx, fx->admin);

	reply = post_as(fx, fx->admin, path, token, TRUE, "job-detail");
	g_assert_cmpuint(reply.status, ==, 200);

	/* The job itself, queued again; the browser goes back to it. */
	redirect = axis_test_reply_header(&reply, "HX-Redirect");
	g_assert_cmpstr(redirect, ==, location);
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM job", "1");
	wait_for_state(fx, id, "running");

	/* Its payload, its attempts counted again, no error of its own yet,
	 * and the run before kept with who retried it. */
	sql = g_strdup_printf("SELECT kind || '|' || queue || '|' || (payload->>'n') || '|' || (payload->>'name') "
		"|| '|' || attempts || '|' || coalesce(last_error, '-') || '|' || jsonb_array_length(earlier_runs) "
		"|| '|' || (earlier_runs->0->>'state') || '|' || (earlier_runs->0->>'attempts') || '|' || "
		"(earlier_runs->0->>'error') || '|' || (earlier_runs->0->>'worker') || '|' || "
		"(earlier_runs->0->>'retried_by') FROM job WHERE id = %" G_GINT64_FORMAT, id);
	assert_query(fx, sql, "test.probe|test|5|a<b|1|-|1|failed|1|the share was away|axisd:test/test/1|root");

	/* Its page says how it ended before; who retried it, to an
	 * administrator only. */
	page_path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, id);
	page = get_as(fx, fx->admin, page_path);
	assert_contains(page.body, "Earlier runs of this job");
	assert_contains(page.body, "Failed after 1 attempt, ");
	assert_contains(page.body, "Retried by root, ");
	assert_contains(page.body, "the share was away");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, page_path);
	assert_contains(page.body, "Earlier runs of this job");
	assert_contains(page.body, "the share was away");
	assert_contains(page.body, "Retried, ");
	assert_lacks(page.body, "Retried by");
	axis_test_reply_clear(&page);

	/* The live updates (up to 200 jobs a batch) read no earlier runs; one
	 * job's page does, apart from the detail they replace. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(AxisJobRow) one = NULL;

		g_array_append_val(ids, id);
		g_assert_true(axis_web_jobs_get_many(fx->pool, fx->job_registry, ids, &rows, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(rows->len, ==, 1);
		g_assert_null(((AxisJobRow *)g_ptr_array_index(rows, 0))->earlier);
		g_assert_true(axis_web_jobs_get(fx->pool, fx->job_registry, id, FALSE, &one, &error));
		g_assert_nonnull(one->earlier);
		g_assert_cmpuint(one->earlier->len, ==, 1);
	}

	/* Running now: not retried again, with or without htmx. */
	reply = post_as(fx, fx->admin, path, token, FALSE, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);

	/* Without htmx a retry is a redirect to the job. */
	open_gate(fx);
	wait_for_state(fx, id, "succeeded");
	axis_test_db_exec(fx->db, "UPDATE job SET state = 'failed' WHERE state = 'succeeded'");
	reply = post_as(fx, fx->admin, path, token, FALSE, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, location);
	axis_test_reply_clear(&reply);
	g_free(sql);
	sql = g_strdup_printf("SELECT jsonb_array_length(earlier_runs) FROM job WHERE id = %" G_GINT64_FORMAT, id);
	assert_query(fx, sql, "2");
}

/*
 * Spec (job-ui): A retry is the same work.  A job whose work is known by its
 * key (an import, a conversion's finish, a schedule's run) failed; a person
 * retries it.  Whoever asks for the same work by its key while it runs (the
 * import's requeue, the conversion tidy, the schedule's next tick, a module's
 * page) is given the retried job, and with two workers on the queue nothing
 * runs it twice.  Found by the stage 2 hunt of the job queue (H-1): the
 * retry was a copy without the key or the priority.
 */
static void
test_retry_is_the_same_work(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *path = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *location = NULL;
	AxisJobEnqueueOptions options;
	AxisTestReply reply;
	gint64 failed;
	gint64 again = 0;
	gboolean created = TRUE;
	guint started;

	SKIP_WITHOUT_DB(fx);
	failed = insert(fx, "INSERT INTO job (kind, queue, max_attempts, state, attempts, started_at, finished_at, "
		"last_error, payload, dedupe_key, priority) VALUES ('test.probe', 'test', 1, 'failed', 1, now(), now(), "
		"'the share was away', '{\"mode\": \"gate\"}', 'test.probe:item:7', 10) RETURNING id");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", failed);
	location = g_strdup_printf("/jobs/%" G_GINT64_FORMAT, failed);
	token = csrf_for(fx, fx->admin);

	reply = post_as(fx, fx->admin, path, token, TRUE, "job-detail");
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(axis_test_reply_header(&reply, "HX-Redirect"), ==, location);
	axis_test_reply_clear(&reply);
	wait_for_state(fx, failed, "running");
	sql = g_strdup_printf("SELECT dedupe_key || '|' || priority FROM job WHERE id = %" G_GINT64_FORMAT, failed);
	assert_query(fx, sql, "test.probe:item:7|10");

	/* The same work asked for again by its key while it runs. */
	json_object_set_string_member(payload, "mode", "gate");
	memset(&options, 0, sizeof(options));
	options.dedupe_key = "test.probe:item:7";
	g_assert_true(axis_job_queue_enqueue(fx->queue, "test.probe", payload, &options, &again, &created, &error));
	g_assert_no_error(error);
	g_assert_false(created);
	g_assert_cmpint(again, ==, failed);

	axis_test_pump_for(500);
	g_mutex_lock(&fx->probe.lock);
	started = fx->probe.started;
	g_mutex_unlock(&fx->probe.lock);
	g_assert_cmpuint(started, ==, 1);
	assert_query(fx, "SELECT count(*) FROM job", "1");
	open_gate(fx);
	wait_for_state(fx, failed, "succeeded");
}

/* Spec (job-ui): Retry while the work is being done. */
static void
test_retry_refused_while_the_work_runs(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autofree gchar *path = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *said = NULL;
	AxisJobEnqueueOptions options;
	AxisTestReply reply;
	gint64 failed;
	gint64 live = 0;

	SKIP_WITHOUT_DB(fx);
	failed = insert(fx, "INSERT INTO job (kind, queue, max_attempts, state, attempts, started_at, finished_at, "
		"last_error, payload, dedupe_key) VALUES ('test.probe', 'test', 1, 'failed', 1, now(), now(), "
		"'boom', '{\"mode\": \"gate\"}', 'test.probe:item:8') RETURNING id");
	json_object_set_string_member(payload, "mode", "gate");
	memset(&options, 0, sizeof(options));
	options.dedupe_key = "test.probe:item:8";
	g_assert_true(axis_job_queue_enqueue(fx->queue, "test.probe", payload, &options, &live, NULL, &error));
	g_assert_no_error(error);
	wait_for_state(fx, live, "running");

	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", failed);
	token = csrf_for(fx, fx->admin);
	reply = post_as(fx, fx->admin, path, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	said = g_strdup_printf("Job %" G_GINT64_FORMAT " is doing this work now", live);
	assert_contains(reply.body, said);
	axis_test_reply_clear(&reply);

	/* Nothing changed, nothing was made. */
	sql = g_strdup_printf("SELECT state || '|' || attempts || '|' || last_error || '|' || "
		"jsonb_array_length(earlier_runs) FROM job WHERE id = %" G_GINT64_FORMAT, failed);
	assert_query(fx, sql, "failed|1|boom|0");
	assert_query(fx, "SELECT count(*) FROM job", "2");
}

/* Spec (job-ui): A kind its owner asks for again. */
static void
test_retry_elsewhere(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *sql = NULL;
	AxisTestReply reply;
	gint64 failed;

	SKIP_WITHOUT_DB(fx);
	failed = insert_finished(fx, "test.owned", "test", "failed", "boom");
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", failed);
	token = csrf_for(fx, fx->admin);
	reply = post_as(fx, fx->admin, path, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "This job cannot be retried from the Jobs page. " ELSEWHERE_TEXT);
	axis_test_reply_clear(&reply);

	sql = g_strdup_printf("SELECT state || '|' || attempts FROM job WHERE id = %" G_GINT64_FORMAT, failed);
	assert_query(fx, sql, "failed|3");
	assert_query(fx, "SELECT count(*) FROM job", "1");
}

static void
test_retry_is_for_failed_or_cancelled_jobs(Fixture *fx, gconstpointer data)
{
	gint64 done;
	gint64 cancelled;
	gint64 orphan;
	g_autofree gchar *token = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	done = insert_finished(fx, "test.probe", "test", "succeeded", NULL);
	cancelled = insert(fx, "INSERT INTO job (kind, queue, max_attempts, state, finished_at, cancel_requested, "
		"payload) VALUES ('test.probe', 'test', 1, 'cancelled', now(), true, '{\"mode\": \"gate\"}') RETURNING id");
	orphan = insert_finished(fx, "gone.kind", "gone", "failed", "no handler");
	token = csrf_for(fx, fx->admin);

	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", done);
	reply = post_as(fx, fx->admin, path, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);

	/* A job whose kind nothing here can run cannot be run again. */
	g_free(path);
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", orphan);
	reply = post_as(fx, fx->admin, path, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/jobs/987654/retry", token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	assert_query(fx, "SELECT count(*) FROM job", "3");
	assert_query(fx, "SELECT string_agg(state::text, ' ' ORDER BY id) FROM job", "succeeded cancelled failed");

	/* A cancelled one can, as itself: its cancel request is gone, so it
	 * runs to the end. */
	g_free(path);
	path = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", cancelled);
	reply = post_as(fx, fx->admin, path, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(axis_test_reply_header(&reply, "HX-Redirect"));
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM job", "3");
	wait_for_state(fx, cancelled, "running");
	/* Past a lease renewal (every 500 ms here), which would pass on a cancel
	 * request left over. */
	axis_test_pump_for(1200);
	g_assert_true(state_is(fx, cancelled, "running"));
	open_gate(fx);
	wait_for_state(fx, cancelled, "succeeded");
}

static void
test_non_admin_cannot_act(Fixture *fx, gconstpointer data)
{
	gint64 queued;
	gint64 failed;
	g_autofree gchar *token = NULL;
	g_autofree gchar *cancel = NULL;
	g_autofree gchar *retry = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	queued = insert(fx, "INSERT INTO job (kind, queue, max_attempts, run_at) "
		"VALUES ('test.probe', 'test', 1, now() + interval '1 hour') RETURNING id");
	failed = insert_finished(fx, "test.probe", "test", "failed", "boom");
	cancel = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", queued);
	retry = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/retry", failed);
	token = csrf_for(fx, fx->viewer);

	/* With a valid token, by htmx or not: 403, and nothing changes. */
	reply = post_as(fx, fx->viewer, cancel, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, retry, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, cancel, token, FALSE, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, retry, token, FALSE, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	g_assert_true(state_is(fx, queued, "queued"));
	g_assert_true(state_is(fx, failed, "failed"));
	assert_query(fx, "SELECT count(*) FROM job", "2");

	/* Nor without being logged in at all. */
	reply = post_as(fx, NULL, cancel, NULL, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	axis_test_reply_clear(&reply);
}

static void
test_actions_are_csrf_protected(Fixture *fx, gconstpointer data)
{
	gint64 queued;
	g_autofree gchar *cancel = NULL;
	g_autofree gchar *token = NULL;
	AxisTestReply reply;
	const gchar *foreign[] = { "X-CSRF-Token", NULL, "Origin", "http://evil.example", NULL };

	SKIP_WITHOUT_DB(fx);
	queued = insert(fx, "INSERT INTO job (kind, queue, max_attempts, run_at) "
		"VALUES ('test.probe', 'test', 1, now() + interval '1 hour') RETURNING id");
	cancel = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", queued);
	token = csrf_for(fx, fx->admin);

	/* As an administrator, but with no token or somebody else's. */
	reply = post_as(fx, fx->admin, cancel, NULL, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, cancel, "not-the-token", TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* Or from another site. */
	foreign[1] = token;
	reply = axis_test_web_request(fx->session, fx->port, "POST", cancel, fx->admin, foreign, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	g_assert_true(state_is(fx, queued, "queued"));

	/* A GET changes nothing and is not a route. */
	reply = get_as(fx, fx->admin, cancel);
	g_assert_cmpuint(reply.status, ==, 405);
	axis_test_reply_clear(&reply);
	g_assert_true(state_is(fx, queued, "queued"));
}

static void
test_actions_need_the_queue(Fixture *fx, gconstpointer data)
{
	gint64 queued;
	g_autofree gchar *cancel = NULL;
	g_autofree gchar *token = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);

	/* The queue is not attached: viewing works, acting says so. */
	queued = insert_queued(fx, "media.scan", "media");
	cancel = g_strdup_printf("/jobs/%" G_GINT64_FORMAT "/cancel", queued);
	token = csrf_for(fx, fx->admin);

	reply = post_as(fx, fx->admin, cancel, token, TRUE, NULL);
	g_assert_cmpuint(reply.status, ==, 503);
	axis_test_reply_clear(&reply);
	g_assert_true(state_is(fx, queued, "queued"));
}

#define ADD(name, flags, func) \
	g_test_add("/web-jobs/" name, Fixture, GUINT_TO_POINTER(flags), fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("page/listing", 0, test_listing);
	ADD("page/filters", 0, test_filters);
	ADD("page/dashboard-now", 0, test_dashboard_now);
	ADD("page/bad-filters-are-refused", 0, test_bad_filters_are_refused);
	ADD("page/paging", 0, test_paging);
	ADD("page/failure-detail", 0, test_failure_detail);
	ADD("page/markup-from-the-database-is-escaped", 0, test_markup_from_the_database_is_escaped);
	ADD("page/detail-refetches-when-the-stream-opens", 0, test_detail_refetches_when_the_stream_opens);
	ADD("page/unknown-job", 0, test_unknown_job);
	ADD("page/needs-a-session", 0, test_pages_need_a_session);
	ADD("page/correct-without-a-stream", NO_HUB, test_correct_without_a_stream);

	ADD("live/pages-follow-the-right-topics", 0, test_pages_follow_the_right_topics);
	ADD("live/progress", WITH_QUEUE, test_live_progress);
	ADD("live/detail", WITH_QUEUE, test_live_detail);
	ADD("live/list-refreshes-when-jobs-change", WITH_QUEUE, test_list_refreshes_when_jobs_change);
	ADD("live/progress-is-coalesced", WITH_QUEUE, test_progress_is_coalesced);
	ADD("live/activity-indicator", WITH_QUEUE, test_activity_indicator);
	ADD("live/activity-topics", WITH_QUEUE, test_activity_topics);
	ADD("live/upkeep-is-not-counted", WITH_QUEUE, test_upkeep_is_not_counted);

	ADD("actions/cancel-running-job", WITH_QUEUE, test_cancel_running_job);
	ADD("actions/cancel-is-shown-while-the-job-stops", WITH_QUEUE, test_cancel_is_shown_while_the_job_stops);
	ADD("actions/cancel-queued-job-and-other-shapes", WITH_QUEUE, test_cancel_queued_job_and_other_shapes);
	ADD("actions/retry-failed-job", WITH_QUEUE, test_retry_failed_job);
	ADD("actions/retry-is-the-same-work", WITH_QUEUE | TWO_WORKERS, test_retry_is_the_same_work);
	ADD("actions/retry-refused-while-the-work-runs", WITH_QUEUE, test_retry_refused_while_the_work_runs);
	ADD("actions/retry-elsewhere", WITH_QUEUE, test_retry_elsewhere);
	ADD("actions/retry-is-for-failed-or-cancelled-jobs", WITH_QUEUE, test_retry_is_for_failed_or_cancelled_jobs);
	ADD("actions/non-admin-cannot-act", WITH_QUEUE, test_non_admin_cannot_act);
	ADD("actions/csrf-protected", WITH_QUEUE, test_actions_are_csrf_protected);
	ADD("actions/need-the-queue", 0, test_actions_need_the_queue);

	return g_test_run();
}
