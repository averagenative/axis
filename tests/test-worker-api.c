/*
 * test-worker-api.c - the worker API against PostgreSQL: the third class of
 * route (a worker's token and nothing else), every refusal of its gate,
 * the throttle, register, claim and what a worker is offered, leases
 * (heartbeat, fencing by token and by worker, silence, cancel, revoke),
 * fail and complete, the in-process queue never running a remote kind, and
 * the Workers page (administrators only, the token shown once and kept
 * nowhere)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The daemon's side runs on the test's main loop (axis-test-worker.h) with
 * a 2 s lease; nothing here starts an axis-worker or ffmpeg.
 */

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "axis-test-worker.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-store.h"
#include "jobs/axis-job-error.h"
#include "jobs/axis-job-lifecycle.h"
#include "jobs/axis-job-queue.h"
#include "web/axis-password.h"

#define UUID_A "3f2a5c1e-7b9d-4e2f-8a6b-1c3d5e7f9a0b"
#define UUID_B "6c1d2e3f-4a5b-4c6d-9e8f-0a1b2c3d4e5f"
#define UUID_C "9a8b7c6d-5e4f-4a3b-8c2d-1e0f9a8b7c6d"

typedef struct
{
	AxisTestDb     *db;
	AxisDbPool     *pool;
	AxisTestDaemon *d;
	gchar          *admin;
	gchar          *viewer;
	gchar          *token;		/* worker "garage" */
	gint64          worker;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the lease of worker*expired*");
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->d = axis_test_daemon_new(fx->pool, NULL, NULL, "127.0.0.0/8, ::1");
	fx->admin = axis_web_auth_open_test_session(fx->d->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->d->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->token = axis_test_daemon_add_worker(fx->d, "garage", &fx->worker);
	axis_test_db_exec(fx->db, "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at) VALUES"
		" ('" UUID_A "', 'Disk A', '/srv/a', false, 'online', now()),"
		" ('" UUID_B "', 'Disk B', '/srv/b', false, 'online', now())");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_test_daemon_free(fx->d);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_secret_free(fx->token);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* Every answer of the worker API: JSON, never a redirect, never a cookie. */
static void
assert_api_reply(AxisTestReply *reply)
{
	const gchar *type = axis_test_reply_header(reply, "Content-Type");

	g_assert_nonnull(type);
	g_assert_true(g_str_has_prefix(type, "application/json"));
	g_assert_null(axis_test_reply_header(reply, "Set-Cookie"));
	g_assert_null(axis_test_reply_header(reply, "Location"));
}

static AxisTestReply
api(
	Fixture     *fx,
	const gchar *request,
	const gchar *token,
	const gchar *body
){
	AxisTestReply reply = axis_test_daemon_post(fx->d, "POST", request, token, NULL, body, NULL);

	assert_api_reply(&reply);
	return reply;
}

/* The error word of @reply, which must have status @status. */
static void
assert_refused(
	AxisTestReply *reply,
	guint          status,
	const gchar   *word
){
	g_autofree gchar *got = NULL;
	g_autofree gchar *message = NULL;

	if (reply->status != status)
		g_error("expected %u, got %u: %s", status, reply->status, reply->body);
	assert_api_reply(reply);
	g_assert_true(axis_worker_error_parse(reply->body, reply->length, &got, &message, NULL, NULL));
	g_assert_cmpstr(got, ==, word);
	g_assert_nonnull(message);
}

static void
expect_refused(
	Fixture     *fx,
	const gchar *request,
	const gchar *token,
	const gchar *body,
	guint        status,
	const gchar *word
){
	AxisTestReply reply = api(fx, request, token, body);

	assert_refused(&reply, status, word);
	axis_test_reply_clear(&reply);
}

static gchar *
q(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->db, sql);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *got = q(fx, sql);

	if (g_strcmp0(got, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", got != NULL ? got : "NULL");
}

static void
do_register(
	Fixture            *fx,
	const gchar        *token,
	gboolean            no_x265,
	const gchar *const *volumes
){
	g_autofree gchar *json = axis_test_register_json(no_x265, volumes);
	AxisTestReply reply = api(fx, "register", token, json);

	if (reply.status != 200)
		g_error("register: %u %s", reply.status, reply.body);
	axis_test_reply_clear(&reply);
}

/* A claim; the job (NULL for none). */
static AxisWorkerJob *
do_claim(
	Fixture            *fx,
	const gchar        *token,
	const gchar *const *volumes
){
	g_autofree gchar *json = axis_test_claim_json(volumes);
	g_autofree gchar *problem = NULL;
	AxisTestReply reply = api(fx, "claim", token, json);
	AxisWorkerJob *job = NULL;

	if (reply.status != 200)
		g_error("claim: %u %s", reply.status, reply.body);
	if (!axis_worker_claimed_parse(reply.body, reply.length, &job, NULL, &problem))
		g_error("claim answer %s: %s", reply.body, problem);
	axis_test_reply_clear(&reply);
	return job;
}

static gchar *
heartbeat_json(
	gint64       job,
	const gchar *token,
	gdouble      progress,
	const gchar *message
){
	AxisWorkerHeartbeat beat;

	memset(&beat, 0, sizeof(beat));
	beat.job = job;
	beat.lease_token = (gchar *)token;
	beat.has_progress = message != NULL;
	beat.progress = progress;
	beat.message = (gchar *)message;
	return axis_worker_heartbeat_to_json(&beat);
}

static gchar *
fail_json(
	gint64       job,
	const gchar *token,
	const gchar *error,
	gboolean     retry,
	gboolean     stopping
){
	AxisWorkerFail fail;

	memset(&fail, 0, sizeof(fail));
	fail.job = job;
	fail.lease_token = (gchar *)token;
	fail.error = (gchar *)error;
	fail.retry = retry;
	fail.stopping = stopping;
	return axis_worker_fail_to_json(&fail);
}

static AxisEncoderCapabilities *
caps_new(const gchar *version)
{
	AxisEncoderCapabilities *caps = axis_encoder_capabilities_new();

	caps->tool_version = g_strdup(version);
	caps->tool_digest = g_strdup("fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210");
	caps->aac = TRUE;
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx264"), TRUE, FALSE, FALSE, NULL);
	axis_encoder_capabilities_set(caps, axis_convert_encoder_named("libx265"), FALSE, FALSE, FALSE, "it failed");
	return caps;
}

static gchar *
complete_caps_json(
	gint64       job,
	const gchar *token,
	const gchar *version
){
	g_autoptr(AxisEncoderCapabilities) caps = caps_new(version);
	AxisWorkerComplete complete;

	memset(&complete, 0, sizeof(complete));
	complete.job = job;
	complete.lease_token = (gchar *)token;
	complete.kind = (gchar *)AXIS_WORKER_KIND_CAPABILITIES;
	complete.caps = caps;
	return axis_worker_complete_to_json(&complete);
}

/* A remote job put straight in the table (as the daemon's own code would):
 * @payload is its payload's JSON. */
static gint64
enqueue(
	Fixture     *fx,
	const gchar *kind,
	const gchar *payload,
	guint        max_attempts
){
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonObject) object = NULL;
	AxisDbConn *conn;
	AxisJobSpec spec;
	gint64 id = 0;

	g_assert_true(json_parser_load_from_data(parser, payload, -1, &error));
	object = json_node_dup_object(json_parser_get_root(parser));
	memset(&spec, 0, sizeof(spec));
	spec.kind = kind;
	spec.queue = AXIS_WORKER_QUEUE;
	spec.payload = object;
	spec.max_attempts = max_attempts;
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_job_enqueue(conn, &spec, &id, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	return id;
}

static gchar *
job_q(
	Fixture     *fx,
	const gchar *column,
	gint64       id
){
	return axis_test_db_queryf(fx->db, "SELECT %s FROM job WHERE id = %" G_GINT64_FORMAT, column, id);
}

static void
assert_job(
	Fixture     *fx,
	gint64       id,
	const gchar *column,
	const gchar *expected
){
	g_autofree gchar *got = job_q(fx, column, id);

	if (g_strcmp0(got, expected) != 0)
		g_error("job %" G_GINT64_FORMAT " %s: expected %s, got %s", id, column, expected != NULL ? expected : "NULL",
			got != NULL ? got : "NULL");
}

static gboolean
wait_job_state(
	Fixture     *fx,
	gint64       id,
	const gchar *state
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_wait_query(fx->db, sql, state, AXIS_TEST_WAIT_MS);
}

static AxisTestReply
page_get(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->d->session, fx->d->port, "GET", path, cookie, NULL, NULL);
}

static AxisTestReply
page_post(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply page = page_get(fx, fx->admin, "/workers");
	g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
	const gchar *extra[3];

	g_assert_nonnull(csrf);
	axis_test_reply_clear(&page);
	if (cookie != fx->admin) {
		/* Another session's own token, from a page it can see. */
		AxisTestReply other = page_get(fx, cookie, "/");

		g_free(csrf);
		csrf = axis_test_web_csrf_token(other.body);
		axis_test_reply_clear(&other);
		if (csrf == NULL)
			csrf = g_strdup("none");
	}
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf;
	extra[2] = NULL;
	return axis_test_web_request(fx->d->session, fx->d->port, "POST", path, cookie, extra, form);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

/* No row of any table, and nothing logged, holds @secret. */
static void
assert_secret_nowhere(
	Fixture     *fx,
	const gchar *secret
){
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING
	};
	g_autofree gchar *pattern = g_strdup_printf("*%s*", secret);
	g_autofree gchar *count = NULL;
	guint i;

	count = axis_test_db_queryf(fx->db,
		"SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace"
		" WHERE c.relkind = 'r' AND n.nspname = 'public'"
		" AND (xpath('/row/c/text()', query_to_xml(format('SELECT count(*) AS c FROM %%I.%%I t"
		" WHERE strpos(t::text, %%L) > 0', n.nspname, c.relname, '%s'), false, true, '')))[1]::text::int > 0",
		secret);
	g_assert_cmpstr(count, ==, "0");
	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		g_assert_cmpuint(axis_test_log_count(levels[i], pattern), ==, 0);
}

/* ── The registry ───────────────────────────────────────────────── */

static gboolean
never_runs(AxisJobContext *context, gpointer user_data, GError **error)
{
	g_assert_not_reached();
	return FALSE;
}

static void
test_registry(void)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	AxisJobHandlerInfo info;

	g_assert_true(axis_worker_api_register_kinds(registry, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_registry_is_remote(registry, AXIS_WORKER_KIND_CAPABILITIES));
	g_assert_true(axis_job_registry_is_remote(registry, AXIS_WORKER_KIND_SAMPLE));
	g_assert_true(axis_job_registry_kind_claimable(registry, AXIS_WORKER_KIND_SAMPLE));
	g_assert_false(axis_job_registry_is_remote(registry, "probe.file"));

	/* An in-process kind cannot share the remote queue... */
	memset(&info, 0, sizeof(info));
	info.kind = "test.local";
	info.queue = AXIS_WORKER_QUEUE;
	info.max_attempts = 1;
	info.handler = never_runs;
	g_assert_false(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	/* ...nor a remote kind an in-process queue. */
	info.queue = "local";
	g_assert_true(axis_job_registry_add(registry, &info, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(axis_job_registry_add_remote(registry, "test.remote", "local", 1, "Remote work",
		AXIS_JOB_CLASS_WORK, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
	g_clear_error(&error);
	/* A kind is registered once, remote or not. */
	g_assert_false(axis_job_registry_add_remote(registry, "test.local", AXIS_WORKER_QUEUE, 1, "Local work",
		AXIS_JOB_CLASS_WORK, NULL, &error));
	g_assert_error(error, AXIS_JOB_ERROR, AXIS_JOB_ERROR_INVALID);
}

/* ── The gate ───────────────────────────────────────────────────── */

static void
test_gate(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *claim = axis_test_claim_json(NULL);
	g_autofree gchar *bearer = NULL;
	g_autofree gchar *big = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	bearer = g_strconcat("Bearer ", fx->token, NULL);

	/* No token, a wrong one, a malformed header. */
	reply = api(fx, "claim", NULL, claim);
	assert_refused(&reply, 401, "unauthorized");
	g_assert_cmpstr(axis_test_reply_header(&reply, "WWW-Authenticate"), ==, "Bearer");
	axis_test_reply_clear(&reply);
	expect_refused(fx, "claim", "not-a-token", claim, 401, "unauthorized");
	{
		const gchar *const twice[] = { "Authorization", bearer, NULL };

		/* Two Authorization headers make no token. */
		reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, NULL, claim, twice);
		assert_refused(&reply, 401, "unauthorized");
		axis_test_reply_clear(&reply);
	}
	{
		g_autofree gchar *basic = g_strconcat("Basic ", fx->token, NULL);
		const gchar *const extra[] = { "Authorization", basic, NULL };

		reply = axis_test_daemon_post(fx->d, "POST", "claim", NULL, NULL, claim, extra);
		assert_refused(&reply, 401, "unauthorized");
		axis_test_reply_clear(&reply);
	}

	/* The right token: POST with JSON only. */
	reply = axis_test_daemon_post(fx->d, "GET", "claim", fx->token, NULL, NULL, NULL);
	assert_refused(&reply, 405, "method");
	g_assert_cmpstr(axis_test_reply_header(&reply, "Allow"), ==, "POST");
	axis_test_reply_clear(&reply);
	reply = axis_test_daemon_post(fx->d, "PUT", "claim", fx->token, NULL, claim, NULL);
	assert_refused(&reply, 405, "method");
	axis_test_reply_clear(&reply);
	reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, "text/plain", claim, NULL);
	assert_refused(&reply, 415, "content-type");
	axis_test_reply_clear(&reply);
	reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, "application/x-www-form-urlencoded", "a=b",
		NULL);
	assert_refused(&reply, 415, "content-type");
	axis_test_reply_clear(&reply);
	/* Through the gate (the API itself answers: not registered yet). */
	reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, "application/json; charset=utf-8", claim, NULL);
	assert_refused(&reply, 409, "unregistered");
	axis_test_reply_clear(&reply);

	/* Nothing a browser sends, even with the right token. */
	{
		const gchar *const origin[] = { "Origin", "http://127.0.0.1", NULL };
		const gchar *const site[] = { "Sec-Fetch-Site", "same-origin", NULL };
		const gchar *const mode[] = { "Sec-Fetch-Mode", "cors", NULL };
		const gchar *const cookie[] = { "Cookie", fx->admin, NULL };
		const gchar *const *each[] = { origin, site, mode, cookie };
		guint i;

		for (i = 0; i < G_N_ELEMENTS(each); i++) {
			reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, NULL, claim, each[i]);
			assert_refused(&reply, 403, "forbidden");
			axis_test_reply_clear(&reply);
		}
	}

	/* No such request. */
	expect_refused(fx, "nope", fx->token, claim, 404, "not-found");
	expect_refused(fx, "/api/v1/worker", fx->token, claim, 404, "not-found");
	expect_refused(fx, "/api/v1/worker/claim/more", fx->token, claim, 404, "not-found");

	/* Malformed, or too large. */
	expect_refused(fx, "claim", fx->token, "{\"kinds\":", 400, "malformed");
	expect_refused(fx, "claim", fx->token, "{\"kinds\":[],\"volumes\":[],\"extra\":1}", 400, "malformed");
	big = g_strnfill(AXIS_WORKER_MAX_REQUEST_BYTES + 1, ' ');
	reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, NULL, big, NULL);
	g_assert_cmpuint(reply.status, ==, 413);
	g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
	axis_test_reply_clear(&reply);

	/* A session is nothing here: an administrator's cookie and CSRF token
	 * without a worker's token are refused like no token. */
	{
		AxisTestReply page = axis_test_web_request(fx->d->session, fx->d->port, "GET", "/workers", fx->admin, NULL,
			NULL);
		g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
		const gchar *extra[] = { "Cookie", fx->admin, "X-CSRF-Token", csrf, NULL };

		g_assert_cmpuint(page.status, ==, 200);
		g_assert_nonnull(csrf);
		axis_test_reply_clear(&page);
		reply = axis_test_daemon_post(fx->d, "POST", "claim", NULL, NULL, claim, extra);
		assert_refused(&reply, 401, "unauthorized");
		axis_test_reply_clear(&reply);
	}

	/* And a worker's token is nothing outside the worker API. */
	{
		const gchar *const extra[] = { "Authorization", bearer, NULL };
		const gchar *const paths[] = { "/workers", "/", "/jobs", "/api/v1/workers" };
		guint i;

		for (i = 0; i < G_N_ELEMENTS(paths); i++) {
			reply = axis_test_web_request(fx->d->session, fx->d->port, "GET", paths[i], NULL, extra, NULL);
			g_assert_cmpuint(reply.status, !=, 200);
			assert_lacks(reply.body, "garage");
			axis_test_reply_clear(&reply);
		}
		reply = axis_test_web_request(fx->d->session, fx->d->port, "POST", "/workers", NULL, extra, "name=evil");
		g_assert_cmpuint(reply.status, !=, 200);
		axis_test_reply_clear(&reply);
		assert_q(fx, "SELECT count(*) FROM worker", "1");
	}
	assert_secret_nowhere(fx, fx->token);
}

static void
test_throttle(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *claim = axis_test_claim_json(NULL);
	const gchar *const from_a[] = { "X-Forwarded-For", "203.0.113.7", NULL };
	const gchar *const from_b[] = { "X-Forwarded-For", "198.51.100.4", NULL };
	AxisTestReply reply;
	guint refused = 0;
	gboolean throttled = FALSE;
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; i < 16 && !throttled; i++) {
		g_autofree gchar *wrong = g_strdup_printf("wrong-token-%u", i);

		reply = axis_test_daemon_post(fx->d, "POST", "claim", wrong, NULL, claim, from_a);
		assert_api_reply(&reply);
		if (reply.status == 429) {
			throttled = TRUE;
			g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
		} else {
			g_assert_cmpuint(reply.status, ==, 401);
			refused++;
		}
		axis_test_reply_clear(&reply);
	}
	/* Ten wrong tokens are free; the eleventh starts the wait. */
	g_assert_true(throttled);
	g_assert_cmpuint(refused, ==, 11);
	/* The address waits, whatever it sends now; another does not. */
	reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, NULL, claim, from_a);
	assert_refused(&reply, 429, "throttled");
	axis_test_reply_clear(&reply);
	reply = axis_test_daemon_post(fx->d, "POST", "claim", fx->token, NULL, claim, from_b);
	assert_refused(&reply, 409, "unregistered");
	axis_test_reply_clear(&reply);
	/* What was logged names the address, never what was sent. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*from 203.0.113.7 carried a token that is not*"), ==,
		11);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*wrong-token*"), ==, 0);
}

/* ── register ───────────────────────────────────────────────────── */

static void
test_register(Fixture *fx, gconstpointer data)
{
	const gchar *const volumes[] = { UUID_A, "/mnt/a", UUID_C, "/mnt/c", NULL };
	g_autofree gchar *json = axis_test_register_json(FALSE, volumes);
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisWorkerRegistered) answer = NULL;
	AxisTestReply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	/* Nothing is offered before it registers. */
	{
		g_autofree gchar *claim = axis_test_claim_json(NULL);

		expect_refused(fx, "claim", fx->token, claim, 409, "unregistered");
	}

	reply = api(fx, "register", fx->token, json);
	g_assert_cmpuint(reply.status, ==, 200);
	answer = axis_worker_registered_parse(reply.body, reply.length, &problem);
	if (answer == NULL)
		g_error("%s: %s", reply.body, problem);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(answer->worker, ==, "garage");
	g_assert_cmpuint(answer->lease_ms, ==, 2000);
	g_assert_cmpuint(answer->heartbeat_ms, ==, 500);
	g_assert_cmpuint(answer->poll_ms, ==, 200);
	g_assert_cmpuint(answer->volumes->len, ==, 2);
	for (i = 0; i < answer->volumes->len; i++) {
		const AxisWorkerVolumeAnswer *v = g_ptr_array_index(answer->volumes, i);

		/* Only a volume the daemon has. */
		if (strcmp(v->uuid, UUID_A) == 0) {
			g_assert_true(v->accepted);
		} else {
			g_assert_cmpstr(v->uuid, ==, UUID_C);
			g_assert_false(v->accepted);
			g_assert_nonnull(v->reason);
		}
	}
	assert_q(fx, "SELECT worker_version || ' ' || ffmpeg_version || ' ' || max_jobs || ' ' ||"
		" (registered_at IS NOT NULL) || ' ' || last_address FROM worker WHERE name = 'garage'",
		"0.1.0 8.1.3 1 true 127.0.0.1");
	assert_q(fx, "SELECT string_agg(path, ',') FROM worker_volume", "/mnt/a");
	/* Its capabilities, under its own machine. */
	assert_q(fx, "SELECT tool_version FROM encoder_tool WHERE machine = 'worker-garage'", "8.1.3");
	assert_q(fx, "SELECT usable::text FROM encoder_capability WHERE machine = 'worker-garage' AND encoder = 'libx265'",
		"true");

	/* A register says it all again: the volumes it does not name go. */
	do_register(fx, fx->token, TRUE, NULL);
	assert_q(fx, "SELECT count(*) FROM worker_volume", "0");
	assert_q(fx, "SELECT usable::text FROM encoder_capability WHERE machine = 'worker-garage' AND encoder = 'libx265'",
		"false");

	/* Malformed: nothing changes. */
	expect_refused(fx, "register", fx->token, "{\"protocol\":3}", 400, "malformed");
	{
		g_autofree gchar *upgraded = g_strdup(json);
		gchar *at = strstr(upgraded, "\"0.1.0\"");

		g_assert_nonnull(at);
		memcpy(at, "\"0 1;0\"", 7);
		expect_refused(fx, "register", fx->token, upgraded, 400, "malformed");
	}
	assert_q(fx, "SELECT worker_version FROM worker WHERE name = 'garage'", "0.1.0");
}

/* ── claim ──────────────────────────────────────────────────────── */

static void
test_claim(Fixture *fx, gconstpointer data)
{
	const gchar *const at_a[] = { UUID_A, "/mnt/a", NULL };
	const gchar *const proved_a[] = { UUID_A, NULL };
	const gchar *const proved_ab[] = { UUID_A, UUID_B, NULL };
	g_autofree gchar *attic = NULL;
	g_autofree gchar *fail = NULL;
	gint64 attic_id = 0;
	gint64 for_attic;
	gint64 for_x265;
	gint64 for_b;
	gint64 other;
	AxisWorkerJob *job;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	attic = axis_test_daemon_add_worker(fx->d, "attic", &attic_id);
	do_register(fx, fx->token, FALSE, at_a);	/* libx265, volume A */
	do_register(fx, attic, TRUE, at_a);		/* no libx265, volume A */

	/* A job for one worker. */
	for_attic = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{\"requires\":{\"worker\":\"attic\"}}", 1);
	g_assert_null(do_claim(fx, fx->token, proved_a));
	job = do_claim(fx, attic, proved_a);
	g_assert_nonnull(job);
	g_assert_cmpint(job->id, ==, for_attic);
	g_assert_cmpstr(job->kind, ==, AXIS_WORKER_KIND_CAPABILITIES);
	g_assert_cmpuint(job->attempt, ==, 1);
	g_assert_cmpuint(job->lease_ms, ==, 2000);
	g_assert_cmpuint(job->heartbeat_ms, ==, 500);
	g_assert_nonnull(job->lease_token);
	assert_job(fx, for_attic, "state || ' ' || worker_id", "running worker:attic");
	assert_job(fx, for_attic, "lease_token::text", job->lease_token);
	fail = fail_json(job->id, job->lease_token, "done with it", FALSE, FALSE);
	axis_worker_job_free(job);

	/* A job that needs libx265 and volume A. */
	for_x265 = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES,
		"{\"requires\":{\"encoder\":\"libx265\",\"volumes\":[\"" UUID_A "\"]}}", 1);
	/* attic holds one job, all it said it runs. */
	g_assert_null(do_claim(fx, attic, proved_a));
	reply = api(fx, "fail", attic, fail);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	assert_job(fx, for_attic, "state", "failed");
	/* attic cannot run libx265. */
	g_assert_null(do_claim(fx, attic, proved_a));
	/* garage can, only with volume A proved now. */
	g_assert_null(do_claim(fx, fx->token, NULL));
	job = do_claim(fx, fx->token, proved_a);
	g_assert_nonnull(job);
	g_assert_cmpint(job->id, ==, for_x265);
	assert_job(fx, for_x265, "worker_id", "worker:garage");
	/* One at a time, as it said. */
	g_assert_null(do_claim(fx, fx->token, proved_a));
	g_free(fail);
	fail = fail_json(job->id, job->lease_token, "done with it", FALSE, FALSE);
	axis_worker_job_free(job);
	reply = api(fx, "fail", fx->token, fail);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);

	/* A volume it did not register is not one it has, whatever it claims. */
	for_b = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{\"requires\":{\"volumes\":[\"" UUID_B "\"]}}", 1);
	g_assert_null(do_claim(fx, fx->token, proved_ab));
	assert_job(fx, for_b, "state", "queued");

	/* Never a kind that is not remote, whatever it asks for. */
	other = enqueue(fx, "probe.file", "{}", 1);
	{
		AxisWorkerClaim request;
		const gchar *const kinds[] = { "probe.file", NULL };
		g_autofree gchar *json = NULL;

		memset(&request, 0, sizeof(request));
		request.kinds = (gchar **)kinds;
		request.volumes = (gchar **)proved_ab;
		json = axis_worker_claim_to_json(&request);
		reply = api(fx, "claim", fx->token, json);
		g_assert_cmpuint(reply.status, ==, 200);
		job = NULL;
		g_assert_true(axis_worker_claimed_parse(reply.body, reply.length, &job, NULL, NULL));
		g_assert_null(job);
		axis_test_reply_clear(&reply);
	}
	assert_job(fx, other, "state", "queued");
	axis_secret_free(g_steal_pointer(&attic));
}

/* A volume added without a marker (from a disk mounted read-only) is never
 * offered to a worker, which proves a disk by its marker; once its marker is
 * written, it is. */
static void
test_volume_without_marker(Fixture *fx, gconstpointer data)
{
	const gchar *const at_ac[] = { UUID_A, "/mnt/a", UUID_C, "/mnt/c", NULL };
	const gchar *const proved_c[] = { UUID_C, NULL };
	AxisWorkerJob *job;
	AxisTestReply page;
	gint64 for_c;

	SKIP_WITHOUT_DB(fx);
	axis_test_db_exec(fx->db, "INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at,"
		" identity, ident_magic, ident_fsid, ident_ino, ident_bytes) VALUES"
		" ('" UUID_C "', 'Disk C', '/srv/c', true, 'read_only', NULL, 'filesystem', 61267, '03350eabfcdf1e9a', 2,"
		" 2040373248)");
	/* Even when a worker says it proved it (it cannot have read a marker
	 * that is not there yet). */
	do_register(fx, fx->token, FALSE, at_ac);
	for_c = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{\"requires\":{\"volumes\":[\"" UUID_C "\"]}}", 1);
	g_assert_null(do_claim(fx, fx->token, proved_c));
	assert_job(fx, for_c, "state", "queued");

	/* The Workers page says which volumes no worker can use, and why. */
	page = page_get(fx, fx->admin, "/workers");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Disk &#39;Disk C&#39; has no marker yet");
	assert_contains(page.body, "A worker recognises a disk only by its marker");
	assert_lacks(page.body, "Disk A&#39; has no marker");
	axis_test_reply_clear(&page);

	/* Its marker written: offered. */
	axis_test_db_exec(fx->db, "UPDATE volume SET identity = 'marker', marker_created_at = now() WHERE uuid = '"
		UUID_C "'");
	job = do_claim(fx, fx->token, proved_c);
	g_assert_nonnull(job);
	g_assert_cmpint(job->id, ==, for_c);
	axis_worker_job_free(job);
	page = page_get(fx, fx->admin, "/workers");
	assert_lacks(page.body, "has no marker yet");
	axis_test_reply_clear(&page);
}

/* The in-process queue never claims a remote kind, but sweeps it. */
static void
test_not_in_process(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisJobQueue *queue;
	gint64 id = 0;

	SKIP_WITHOUT_DB(fx);
	queue = axis_job_queue_new(fx->pool, fx->d->registry);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	g_assert_true(axis_worker_store_request_measure(fx->pool, "garage", &id, NULL, &error));
	g_assert_no_error(error);
	axis_test_pump_for(600);
	assert_job(fx, id, "state || ' ' || attempts", "queued 0");
	g_assert_true(axis_job_queue_stop(queue, 1000));
	g_clear_object(&queue);
}

/* ── Leases ─────────────────────────────────────────────────────── */

static void
test_heartbeat(Fixture *fx, gconstpointer data)
{
	const gchar *const at_a[] = { UUID_A, "/mnt/a", NULL };
	g_autofree gchar *attic = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *stale = NULL;
	g_autofree gchar *problem = NULL;
	g_autoptr(AxisWorkerBeat) beat = NULL;
	AxisWorkerJob *job;
	AxisTestReply reply;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	attic = axis_test_daemon_add_worker(fx->d, "attic", NULL);
	do_register(fx, fx->token, FALSE, at_a);
	do_register(fx, attic, FALSE, at_a);
	id = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 3);
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	g_assert_cmpint(job->id, ==, id);
	g_assert_cmpuint(axis_worker_api_count_leases(fx->d->api), ==, 1);
	before = job_q(fx, "extract(epoch FROM lease_expires_at)::text", id);

	/* A heartbeat renews it, and records the progress. */
	axis_test_pump_for(100);
	json = heartbeat_json(id, job->lease_token, 0.5, "half way");
	reply = api(fx, "heartbeat", fx->token, json);
	g_assert_cmpuint(reply.status, ==, 200);
	beat = axis_worker_beat_parse(reply.body, reply.length, &problem);
	g_assert_nonnull(beat);
	g_assert_false(beat->stop);
	g_assert_cmpuint(beat->lease_ms, ==, 2000);
	axis_test_reply_clear(&reply);
	after = job_q(fx, "extract(epoch FROM lease_expires_at)::text", id);
	g_assert_cmpfloat(g_ascii_strtod(after, NULL), >, g_ascii_strtod(before, NULL));
	assert_job(fx, id, "progress || ' ' || progress_message", "0.5 half way");

	/* The Conversion workers page: what the worker is doing, in plain words,
	 * its kind and number beside. */
	{
		g_autofree gchar *number = g_strdup_printf("#%" G_GINT64_FORMAT, id);
		g_autofree gchar *link = g_strdup_printf("href=\"/jobs/%" G_GINT64_FORMAT "\"", id);
		AxisTestReply page = page_get(fx, fx->admin, "/workers");

		g_assert_cmpuint(page.status, ==, 200);
		assert_contains(page.body, "<h1>Conversion workers</h1><p class=\"page-intro\">A worker is a separate program");
		assert_contains(page.body, ">Measuring what a worker");
		assert_contains(page.body, "s encoders can do</a>");
		/* The kind by its label, not as code. */
		assert_lacks(page.body, "<code>" AXIS_WORKER_KIND_CAPABILITIES "</code>");
		assert_contains(page.body, number);
		assert_contains(page.body, link);
		assert_contains(page.body, "half way");
		axis_test_reply_clear(&page);
	}

	/* Another token, or another worker with the right one: refused, and
	 * nothing changes. */
	stale = heartbeat_json(id, "00000000-0000-4000-8000-000000000000", 0.9, "stale");
	expect_refused(fx, "heartbeat", fx->token, stale, 409, "lease");
	expect_refused(fx, "heartbeat", attic, json, 409, "lease");
	{
		g_autofree gchar *done = complete_caps_json(id, job->lease_token, "8.1.4");

		expect_refused(fx, "complete", attic, done, 409, "lease");
	}
	{
		g_autofree gchar *other = fail_json(id, job->lease_token, "not mine", FALSE, FALSE);

		expect_refused(fx, "fail", attic, other, 409, "lease");
	}
	g_free(before);
	before = job_q(fx, "extract(epoch FROM lease_expires_at)::text", id);
	g_assert_cmpstr(before, ==, after);
	assert_job(fx, id, "state || ' ' || progress || ' ' || progress_message || ' ' || worker_id",
		"running 0.5 half way worker:garage");

	/* Silence: the lease runs out, the job goes back (an attempt used). */
	g_assert_true(wait_job_state(fx, id, "queued"));
	assert_job(fx, id, "attempts || ' ' || coalesce(lease_token::text, '-') || ' ' || last_error",
		"1 - the lease of worker worker:garage expired: it stopped renewing it");
	g_assert_cmpuint(axis_worker_api_count_leases(fx->d->api), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*the lease of worker worker:garage expired*"), ==, 1);

	/* Too late for anything under the old lease. */
	expect_refused(fx, "heartbeat", fx->token, json, 409, "lease");
	{
		g_autofree gchar *done = complete_caps_json(id, job->lease_token, "8.1.4");
		g_autofree gchar *gave = fail_json(id, job->lease_token, "late", TRUE, TRUE);

		expect_refused(fx, "complete", fx->token, done, 409, "lease");
		expect_refused(fx, "fail", fx->token, gave, 409, "lease");
	}
	assert_job(fx, id, "state || ' ' || attempts", "queued 1");
	assert_q(fx, "SELECT count(*) FROM encoder_tool WHERE tool_version = '8.1.4'", "0");

	/* Claimed again: a new lease token, the next attempt. */
	{
		AxisWorkerJob *again = do_claim(fx, attic, NULL);

		g_assert_nonnull(again);
		g_assert_cmpint(again->id, ==, id);
		g_assert_cmpuint(again->attempt, ==, 2);
		g_assert_cmpstr(again->lease_token, !=, job->lease_token);
		/* The old holder's token is nothing now. */
		expect_refused(fx, "heartbeat", fx->token, json, 409, "lease");
		axis_worker_job_free(again);
	}
	axis_worker_job_free(job);
	axis_secret_free(g_steal_pointer(&attic));
}

static void
test_cancel(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(AxisWorkerBeat) beat = NULL;
	AxisJobCancelOutcome outcome;
	AxisWorkerJob *job;
	AxisTestReply reply;
	AxisDbConn *conn;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	do_register(fx, fx->token, FALSE, NULL);
	id = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 3);
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	conn = axis_db_pool_acquire(fx->pool, &error);
	g_assert_true(axis_job_cancel(conn, id, &outcome, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->pool, conn);
	g_assert_cmpint(outcome, ==, AXIS_JOB_CANCEL_REQUESTED);

	/* The next heartbeat says stop, and the job ends. */
	json = heartbeat_json(id, job->lease_token, 0.1, "working");
	reply = api(fx, "heartbeat", fx->token, json);
	g_assert_cmpuint(reply.status, ==, 200);
	beat = axis_worker_beat_parse(reply.body, reply.length, NULL);
	g_assert_nonnull(beat);
	g_assert_true(beat->stop);
	g_assert_cmpstr(beat->reason, ==, "cancelled");
	axis_test_reply_clear(&reply);
	assert_job(fx, id, "state", "cancelled");
	expect_refused(fx, "heartbeat", fx->token, json, 409, "lease");
	g_assert_cmpuint(axis_worker_api_count_leases(fx->d->api), ==, 0);
	axis_worker_job_free(job);
}

static void
test_revoke(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *claim = axis_test_claim_json(NULL);
	g_autofree gchar *again = axis_test_register_json(FALSE, NULL);
	AxisWorkerJob *job;
	gboolean revoked = FALSE;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	do_register(fx, fx->token, FALSE, NULL);
	id = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 3);
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	g_assert_true(axis_worker_store_revoke(fx->pool, fx->d->tokens, "garage", "root", &revoked, &error));
	g_assert_no_error(error);
	g_assert_true(revoked);

	/* Its very next request is refused, whatever it is. */
	json = heartbeat_json(id, job->lease_token, 0.2, "still here");
	expect_refused(fx, "heartbeat", fx->token, json, 401, "unauthorized");
	expect_refused(fx, "claim", fx->token, claim, 401, "unauthorized");
	expect_refused(fx, "register", fx->token, again, 401, "unauthorized");
	assert_q(fx, "SELECT (revoked_at IS NOT NULL) || ' ' || revoked_by FROM worker WHERE name = 'garage'", "true root");
	/* Its job goes back when the lease it can no longer renew runs out. */
	g_assert_true(wait_job_state(fx, id, "queued"));
	g_assert_true(axis_worker_store_revoke(fx->pool, fx->d->tokens, "garage", "root", &revoked, &error));
	g_assert_false(revoked);
	axis_worker_job_free(job);
}

/* ── fail, complete ─────────────────────────────────────────────── */

static gboolean
job_claimable(gpointer data)
{
	Fixture *fx = ((gpointer *)data)[0];
	gint64 id = *(gint64 *)((gpointer *)data)[1];
	g_autofree gchar *due = job_q(fx, "(run_at <= now())::text", id);

	return g_strcmp0(due, "true") == 0;
}

static void
wait_claimable(
	Fixture *fx,
	gint64   id
){
	gpointer data[2];

	data[0] = fx;
	data[1] = &id;
	g_assert_true(axis_test_pump_until(job_claimable, data, AXIS_TEST_WAIT_MS));
}

static void
test_fail(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *state = NULL;
	AxisWorkerJob *job;
	AxisTestReply reply;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	do_register(fx, fx->token, FALSE, NULL);
	id = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 3);

	/* A failure to retry: queued again after a delay, its error kept with
	 * the worker's name. */
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	{
		g_autofree gchar *json = fail_json(id, job->lease_token, "the encoder crashed", TRUE, FALSE);

		reply = api(fx, "fail", fx->token, json);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_true(axis_worker_failed_parse(reply.body, reply.length, &state, NULL));
		g_assert_cmpstr(state, ==, "queued");
		axis_test_reply_clear(&reply);
		/* Once only. */
		expect_refused(fx, "fail", fx->token, json, 409, "lease");
	}
	assert_job(fx, id, "attempts || ' ' || last_error", "1 worker garage: the encoder crashed");
	axis_worker_job_free(job);

	/* Given back while stopping: no attempt counted. */
	wait_claimable(fx, id);
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	g_assert_cmpuint(job->attempt, ==, 2);
	{
		g_autofree gchar *json = fail_json(id, job->lease_token, "the worker is stopping", TRUE, TRUE);

		reply = api(fx, "fail", fx->token, json);
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
	}
	assert_job(fx, id, "state || ' ' || attempts", "queued 1");
	axis_worker_job_free(job);

	/* A failure not worth retrying ends it. */
	wait_claimable(fx, id);
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	{
		g_autofree gchar *json = fail_json(id, job->lease_token, "the volume is gone", FALSE, FALSE);

		reply = api(fx, "fail", fx->token, json);
		g_assert_cmpuint(reply.status, ==, 200);
		g_free(state);
		state = NULL;
		g_assert_true(axis_worker_failed_parse(reply.body, reply.length, &state, NULL));
		g_assert_cmpstr(state, ==, "failed");
		axis_test_reply_clear(&reply);
	}
	assert_job(fx, id, "state || ' ' || last_error", "failed worker garage: the volume is gone");
	axis_worker_job_free(job);
}

static void
test_complete(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisWorkerCompleted completed;
	AxisWorkerJob *job;
	AxisTestReply reply;
	gint64 id = 0;

	SKIP_WITHOUT_DB(fx);
	do_register(fx, fx->token, FALSE, NULL);
	g_assert_true(axis_worker_store_request_measure(fx->pool, "garage", &id, NULL, &error));
	g_assert_no_error(error);
	job = do_claim(fx, fx->token, NULL);
	g_assert_nonnull(job);
	g_assert_cmpint(job->id, ==, id);

	/* A result of another kind: refused, the job still held. */
	{
		g_autoptr(AxisSampleResult) sample = axis_sample_result_new();
		g_autofree gchar *json = NULL;
		AxisWorkerComplete complete;
		const guint64 bytes[] = { 1000 };

		axis_sample_result_add(sample, 0, bytes, 1, NULL);
		memset(&complete, 0, sizeof(complete));
		complete.job = id;
		complete.lease_token = job->lease_token;
		complete.kind = (gchar *)AXIS_WORKER_KIND_SAMPLE;
		complete.sample = sample;
		json = axis_worker_complete_to_json(&complete);
		expect_refused(fx, "complete", fx->token, json, 400, "malformed");
	}
	assert_job(fx, id, "state", "running");

	/* Its measurement is kept as the worker's capabilities. */
	{
		g_autofree gchar *json = complete_caps_json(id, job->lease_token, "8.1.4");

		reply = api(fx, "complete", fx->token, json);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_true(axis_worker_completed_parse(reply.body, reply.length, &completed, NULL));
		g_assert_cmpuint(completed.recorded, ==, 0);
		axis_test_reply_clear(&reply);
		expect_refused(fx, "complete", fx->token, json, 409, "lease");
	}
	assert_job(fx, id, "state", "succeeded");
	assert_q(fx, "SELECT tool_version FROM encoder_tool WHERE machine = 'worker-garage'", "8.1.4");
	assert_q(fx, "SELECT ffmpeg_version FROM worker WHERE name = 'garage'", "8.1.4");
	assert_q(fx, "SELECT usable::text FROM encoder_capability WHERE machine = 'worker-garage' AND encoder = 'libx265'",
		"false");
	g_assert_cmpuint(axis_worker_api_count_leases(fx->d->api), ==, 0);
	axis_worker_job_free(job);
}

/* ── The Workers page ───────────────────────────────────────────── */

/* The token on the page that shows it once. */
static gchar *
token_on_page(const gchar *html)
{
	const gchar *at = strstr(html, "class=\"worker-token\"");
	const gchar *start;
	const gchar *end;

	g_assert_nonnull(at);
	start = strstr(at, "<code>");
	g_assert_nonnull(start);
	start += strlen("<code>");
	end = strstr(start, "</code>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

static void
test_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *claim = axis_test_claim_json(NULL);
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	/* Administrators only. */
	reply = page_get(fx, fx->viewer, "/workers");
	g_assert_cmpuint(reply.status, ==, 403);
	assert_lacks(reply.body, "garage");
	axis_test_reply_clear(&reply);
	reply = page_post(fx, fx->viewer, "/workers", "name=intruder");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = page_post(fx, fx->viewer, "/workers/garage/revoke", "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	assert_q(fx, "SELECT count(*) || ' ' || count(revoked_at) FROM worker", "1 0");

	reply = page_get(fx, fx->admin, "/workers");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "garage");
	axis_test_reply_clear(&reply);

	/* Created: the token is shown this once, and works. */
	reply = page_post(fx, fx->admin, "/workers", "name=%20Shed-PC%20");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Worker shed-pc");
	token = token_on_page(reply.body);
	g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(strlen(token), >=, 32);
	expect_refused(fx, "claim", token, claim, 409, "unregistered");
	hash = axis_token_hash(token);
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT name FROM worker WHERE token_hash = '%s'", hash);

		assert_q(fx, sql, "shed-pc");
	}

	/* Never again: not on the page, in no table, in no log. */
	reply = page_get(fx, fx->admin, "/workers");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "shed-pc");
	assert_lacks(reply.body, token);
	assert_lacks(reply.body, hash);
	axis_test_reply_clear(&reply);
	assert_secret_nowhere(fx, token);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: worker shed-pc created by root*"), ==, 1);

	/* A name taken, a name that is not one. */
	reply = page_post(fx, fx->admin, "/workers", "name=shed-pc");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=exists");
	axis_test_reply_clear(&reply);
	reply = page_post(fx, fx->admin, "/workers", "name=9lives");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=invalid-name");
	axis_test_reply_clear(&reply);

	/* Measure again: a job only that worker can claim. */
	reply = page_post(fx, fx->admin, "/workers/shed-pc/measure", "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=measure-requested");
	axis_test_reply_clear(&reply);
	assert_q(fx, "SELECT payload->'requires'->>'worker' FROM job WHERE kind = 'worker.capabilities'", "shed-pc");

	/* Revoke: only when confirmed; then the token is refused at once. */
	reply = page_post(fx, fx->admin, "/workers/shed-pc/revoke", "");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	expect_refused(fx, "claim", token, claim, 409, "unregistered");
	reply = page_post(fx, fx->admin, "/workers/shed-pc/revoke", "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=revoked");
	axis_test_reply_clear(&reply);
	expect_refused(fx, "claim", token, claim, 401, "unauthorized");
	reply = page_get(fx, fx->admin, "/workers");
	assert_contains(reply.body, "Revoked");
	axis_test_reply_clear(&reply);
	axis_secret_free(g_steal_pointer(&token));
}

/* A form's answer is a redirect to the element the person acted on, and the
 * page there draws its notice inside that element (the element comes before
 * the first notice in the page). */
static void
assert_back_to_place(
	Fixture       *fx,
	AxisTestReply *reply,
	const gchar   *place
){
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *path = NULL;
	const gchar *location = axis_test_reply_header(reply, "Location");
	const gchar *element;
	const gchar *notice;
	AxisTestReply got;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strlen(location) - strlen(tail));
	got = page_get(fx, fx->admin, path);
	g_assert_cmpuint(got.status, ==, 200);
	element = strstr(got.body, id);
	notice = strstr(got.body, "notice-here");
	if (element == NULL || notice == NULL || element > notice)
		g_error("expected the notice inside %s in:\n%s", place, got.body);
	axis_test_reply_clear(&got);
	axis_test_reply_clear(reply);
}

/* Spec: a form comes back to the card or row it was posted from */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	/* A name that is not one: the card that adds workers. */
	reply = page_post(fx, fx->admin, "/workers", "name=9lives");
	assert_back_to_place(fx, &reply, "worker-add");
	reply = page_post(fx, fx->admin, "/workers", "name=garage");
	assert_back_to_place(fx, &reply, "worker-add");

	/* Each worker's card, by its name. */
	reply = page_post(fx, fx->admin, "/workers/garage/measure", "");
	assert_back_to_place(fx, &reply, "worker-garage");
	reply = page_post(fx, fx->admin, "/workers/garage/resume", "");
	assert_back_to_place(fx, &reply, "worker-garage");
	reply = page_post(fx, fx->admin, "/workers/garage/revoke", "confirm=yes");
	assert_back_to_place(fx, &reply, "worker-garage");
}

/* ── Protocols 2 and 3: other versions, mounts, the pause ───────── */

/* Spec (worker-protocol): a worker of protocol 2 (the axis-worker before the
 * check job) or 1 is refused at every request in a way it can read, given no
 * work, and the Workers page says why; one of a newer protocol is told the
 * daemon is older. */
static void
test_old_protocol(Fixture *fx, gconstpointer data)
{
	static const gchar old_register[] = "{\"protocol\":2,\"worker_version\":\"0.0.9\",\"max_jobs\":1,"
		"\"ffmpeg_version\":\"8.1.3\",\"ffmpeg_digest\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
		"abcdef\",\"capabilities\":{\"measured_at\":0,\"aac\":true,\"mov_text\":false,\"subrip\":false,\"encoders\":[]},"
		"\"volumes\":[{\"uuid\":\"" UUID_A "\",\"path\":\"/mnt/a\",\"keeps_file_identity\":true}]}";
	static const gchar old_claim[] = "{\"protocol\":2,\"kinds\":[\"convert.encode\",\"worker.capabilities\"],"
		"\"volumes\":[\"" UUID_A "\"]}";
	static const gchar old_beat[] = "{\"protocol\":2,\"job\":1,\"lease_token\":\"0b8a7c1e-1d2e-4f3a-9b8c-7d6e5f4a3b2c\"}";
	static const gchar oldest_claim[] = "{\"protocol\":1,\"kinds\":[\"convert.encode\"],\"volumes\":[]}";
	static const gchar newer[] = "{\"protocol\":4,\"kinds\":[\"convert.encode\"],\"volumes\":[]}";
	const gchar *const at_a[] = { UUID_A, "/mnt/a", NULL };
	const gchar *const proved_a[] = { UUID_A, NULL };
	const gchar *const requests[][2] = {
		{ "register", old_register }, { "claim", old_claim }, { "heartbeat", old_beat }
	};
	AxisTestReply reply;
	gint64 job;
	guint i;

	SKIP_WITHOUT_DB(fx);
	job = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 1);
	for (i = 0; i < G_N_ELEMENTS(requests); i++) {
		g_autofree gchar *word = NULL;
		g_autofree gchar *message = NULL;
		guint version = 0;

		reply = api(fx, requests[i][0], fx->token, requests[i][1]);
		g_assert_cmpuint(reply.status, ==, 400);
		/* In protocol 2, with a word protocol 2 knows: the older axis-worker
		 * reads it and logs it. */
		assert_contains(reply.body, "{\"protocol\":2,\"error\":\"malformed\"");
		g_assert_true(axis_worker_error_parse(reply.body, reply.length, &word, &message, &version, NULL));
		g_assert_cmpuint(version, ==, 2);
		assert_contains(message, "this daemon speaks worker protocol 3 and this axis-worker 2");
		axis_test_reply_clear(&reply);
	}
	/* Protocol 1 likewise, in its own version. */
	{
		g_autofree gchar *word = NULL;
		g_autofree gchar *message = NULL;
		guint version = 0;

		reply = api(fx, "claim", fx->token, oldest_claim);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "{\"protocol\":1,\"error\":\"malformed\"");
		g_assert_true(axis_worker_error_parse(reply.body, reply.length, &word, &message, &version, NULL));
		g_assert_cmpuint(version, ==, 1);
		assert_contains(message, "this daemon speaks worker protocol 3 and this axis-worker 1");
		axis_test_reply_clear(&reply);
	}
	/* Nothing it said was taken; no work was given. */
	assert_q(fx, "SELECT (registered_at IS NULL) || ' ' || refused_protocol FROM worker WHERE name = 'garage'",
		"true 2");
	assert_q(fx, "SELECT count(*) FROM worker_volume", "0");
	assert_job(fx, job, "state", "queued");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*worker garage speaks worker protocol 2*"), ==, 1);
	reply = page_get(fx, fx->admin, "/workers");
	assert_contains(reply.body, "Its axis-worker speaks worker protocol 2 and this server 3");
	axis_test_reply_clear(&reply);

	/* Newer than the daemon: answered in the daemon's own version. */
	reply = api(fx, "claim", fx->token, newer);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "\"protocol\":3");
	assert_contains(reply.body, "the daemon is older");
	axis_test_reply_clear(&reply);

	/* The right version: registered, the note gone, work given. */
	do_register(fx, fx->token, FALSE, at_a);
	assert_q(fx, "SELECT refused_protocol IS NULL FROM worker WHERE name = 'garage'", "t");
	{
		AxisWorkerJob *got = do_claim(fx, fx->token, proved_a);

		g_assert_nonnull(got);
		g_assert_cmpint(got->id, ==, job);
		axis_worker_job_free(got);
	}
	reply = page_get(fx, fx->admin, "/workers");
	assert_lacks(reply.body, "speaks worker protocol 2");
	axis_test_reply_clear(&reply);
}

/* Ends a job a worker holds (fails it for good), so that it can claim
 * again. */
static void
let_go(
	Fixture       *fx,
	const gchar   *token,
	AxisWorkerJob *job
){
	g_autofree gchar *json = fail_json(job->id, job->lease_token, "done with it", FALSE, FALSE);
	AxisTestReply reply = api(fx, "fail", token, json);

	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	axis_worker_job_free(job);
}

/* Spec (job-queue, worker-protocol): a remote job that prefers another
 * worker than one is offered to the others until its time, then to that one
 * too; a time that is not a number counts as past. */
static void
test_prefer_other(Fixture *fx, gconstpointer data)
{
	const gchar *const at_a[] = { UUID_A, "/mnt/a", NULL };
	const gchar *const proved_a[] = { UUID_A, NULL };
	g_autofree gchar *later = NULL;
	gchar *shed = NULL;
	AxisWorkerJob *got;
	gint64 first;
	gint64 second;
	gint64 garbled;

	SKIP_WITHOUT_DB(fx);
	shed = axis_test_daemon_add_worker(fx->d, "shed", NULL);
	do_register(fx, fx->token, FALSE, at_a);
	do_register(fx, shed, FALSE, at_a);
	later = g_strdup_printf("{\"requires\":{\"prefer_not\":\"garage\",\"prefer_not_until\":%" G_GINT64_FORMAT "}}",
		g_get_real_time() / G_USEC_PER_SEC + 120);
	first = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, later, 1);
	g_assert_null(do_claim(fx, fx->token, proved_a));
	got = do_claim(fx, shed, proved_a);
	g_assert_nonnull(got);
	g_assert_cmpint(got->id, ==, first);
	let_go(fx, shed, got);

	/* Its time is past: the worker it preferred not to have takes it. */
	second = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES,
		"{\"requires\":{\"prefer_not\":\"garage\",\"prefer_not_until\":1}}", 1);
	got = do_claim(fx, fx->token, proved_a);
	g_assert_nonnull(got);
	g_assert_cmpint(got->id, ==, second);
	let_go(fx, fx->token, got);
	garbled = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES,
		"{\"requires\":{\"prefer_not\":\"garage\",\"prefer_not_until\":\"soon\"}}", 1);
	got = do_claim(fx, fx->token, proved_a);
	g_assert_nonnull(got);
	g_assert_cmpint(got->id, ==, garbled);
	let_go(fx, fx->token, got);
	axis_secret_free(shed);
}

/* Spec (worker-protocol): the worker says per volume whether its mount keeps
 * file identities; the page shows it. */
static void
test_volume_identity(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisWorkerRegister) request = NULL;
	AxisEncoderCapabilities *caps;
	AxisWorkerVolumeSeen *seen;
	g_autofree gchar *json = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	request = axis_worker_register_new();
	caps = axis_encoder_capabilities_new();
	caps->tool_version = g_strdup("8.1.3");
	caps->tool_digest = g_strdup("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	request->worker_version = g_strdup("0.1.0");
	request->max_jobs = 1;
	request->caps = caps;
	seen = g_new0(AxisWorkerVolumeSeen, 1);
	seen->uuid = g_strdup(UUID_A);
	seen->path = g_strdup("/media/disk-a");
	seen->keeps_identity = TRUE;
	g_ptr_array_add(request->volumes, seen);
	seen = g_new0(AxisWorkerVolumeSeen, 1);
	seen->uuid = g_strdup(UUID_B);
	seen->path = g_strdup("/mnt/server/disk-b");
	seen->keeps_identity = FALSE;
	g_ptr_array_add(request->volumes, seen);
	json = axis_worker_register_to_json(request);
	reply = api(fx, "register", fx->token, json);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	assert_q(fx, "SELECT string_agg(path || '=' || keeps_identity, ',' ORDER BY path) FROM worker_volume",
		"/media/disk-a=true,/mnt/server/disk-b=false");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE,
		"*2 of 2 volumes accepted (1 through a mount that does not keep file identities)*"), ==, 1);

	reply = page_get(fx, fx->admin, "/workers");
	g_assert_cmpuint(reply.status, ==, 200);
	/* Once, for disk B's row; disk A's row says what is checked there. */
	g_assert_nonnull(strstr(reply.body, "Sees this disk through a network mount that does not keep file identities"));
	g_assert_null(strstr(strstr(reply.body, "does not keep file identities") + 1, "does not keep file identities"));
	assert_contains(reply.body, "By its size, time, inode and samples of its content");
	axis_test_reply_clear(&reply);
}

static gchar *
paused_claim_json(
	const gchar *const *volumes,
	gint64              job,
	const gchar        *reason
){
	static const gchar *const kinds[] = { AXIS_WORKER_KIND_CAPABILITIES, NULL };
	static const gchar *const none[] = { NULL };
	AxisWorkerClaim claim;

	memset(&claim, 0, sizeof(claim));
	claim.kinds = (gchar **)kinds;
	claim.volumes = (gchar **)(volumes != NULL ? volumes : none);
	claim.paused_job = job;
	claim.paused_reason = (gchar *)reason;
	return axis_worker_claim_to_json(&claim);
}

/* A claim's answer: the job (or NULL) and what it said of the pause. */
static AxisWorkerJob *
claim_paused(
	Fixture             *fx,
	const gchar         *token,
	const gchar         *json,
	gchar              **paused
){
	g_autofree gchar *problem = NULL;
	AxisTestReply reply = api(fx, "claim", token, json);
	AxisWorkerJob *job = NULL;

	if (reply.status != 200)
		g_error("claim: %u %s", reply.status, reply.body);
	if (!axis_worker_claimed_parse(reply.body, reply.length, &job, paused, &problem))
		g_error("claim answer %s: %s", reply.body, problem);
	axis_test_reply_clear(&reply);
	return job;
}

/* Spec (worker-protocol): a worker whose encoder crashed is held paused: no
 * work until a person resumes it, or it registers again as a new process;
 * other workers are not affected. */
static void
test_pause(Fixture *fx, gconstpointer data)
{
	const gchar *const at_a[] = { UUID_A, "/mnt/a", NULL };
	const gchar *const proved_a[] = { UUID_A, NULL };
	g_autofree gchar *crashed = paused_claim_json(proved_a, 4242, "signal 11, SIGSEGV");
	g_autofree gchar *crashed_again = paused_claim_json(proved_a, 4343, "signal 6, SIGABRT");
	g_autofree gchar *plain = paused_claim_json(proved_a, 0, NULL);
	g_autofree gchar *attic = NULL;
	g_autofree gchar *paused = NULL;
	AxisWorkerJob *job;
	AxisTestReply reply;
	gint64 first;
	gint64 second;
	gint64 attic_id = 0;

	SKIP_WITHOUT_DB(fx);
	attic = axis_test_daemon_add_worker(fx->d, "attic", &attic_id);
	do_register(fx, fx->token, FALSE, at_a);
	do_register(fx, attic, FALSE, at_a);
	first = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 1);

	/* The crash, said in a claim: paused, given nothing. */
	job = claim_paused(fx, fx->token, crashed, &paused);
	g_assert_null(job);
	g_assert_nonnull(paused);
	assert_contains(paused, "Its encoder crashed on job 4242 (signal 11, SIGSEGV)");
	g_clear_pointer(&paused, g_free);
	assert_q(fx, "SELECT paused_job || ' ' || paused_reason || ' ' || (paused_at IS NOT NULL) FROM worker"
		" WHERE name = 'garage'", "4242 signal 11, SIGSEGV true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: worker garage paused: its encoder crashed on job "
		"4242*"), ==, 1);
	/* Every claim, whatever it says, until it is resumed: still nothing. */
	g_assert_null(claim_paused(fx, fx->token, plain, &paused));
	g_assert_nonnull(paused);
	g_clear_pointer(&paused, g_free);
	g_assert_null(claim_paused(fx, fx->token, crashed, &paused));
	g_clear_pointer(&paused, g_free);
	assert_job(fx, first, "state", "queued");
	/* Another worker is not affected. */
	job = claim_paused(fx, attic, plain, &paused);
	g_assert_nonnull(job);
	g_assert_null(paused);
	g_assert_cmpint(job->id, ==, first);
	{
		g_autofree gchar *done = fail_json(job->id, job->lease_token, "done with it", FALSE, FALSE);

		reply = api(fx, "fail", attic, done);
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
	}
	axis_worker_job_free(job);

	/* The page says why, and offers to resume it; the dashboard counts it. */
	reply = page_get(fx, fx->admin, "/workers");
	assert_contains(reply.body, "Paused");
	assert_contains(reply.body, "its encoder crashed on job 4242 (signal 11, SIGSEGV)");
	assert_contains(reply.body, "/workers/garage/resume");
	assert_contains(reply.body, "/jobs/4242");
	axis_test_reply_clear(&reply);
	axis_test_web_read_attention(fx->d->web);
	reply = page_get(fx, fx->admin, "/");
	assert_contains(reply.body, "attention-level\">Waiting for you</span><a href=\"/workers\"><span>1 worker is paused "
		"after its encoder crashed: it converts nothing until you resume it.</span></a>");
	axis_test_reply_clear(&reply);

	/* Administrators only. */
	reply = page_post(fx, fx->viewer, "/workers/garage/resume", "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	assert_q(fx, "SELECT paused_at IS NOT NULL FROM worker WHERE name = 'garage'", "t");

	/* Resumed: the same crash said again does not pause it again; it is
	 * given work. */
	reply = page_post(fx, fx->admin, "/workers/garage/resume", "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=resumed");
	axis_test_reply_clear(&reply);
	assert_q(fx, "SELECT (paused_at IS NULL) || ' ' || resumed_job FROM worker WHERE name = 'garage'", "true 4242");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: worker garage resumed by root after its encoder "
		"crashed on job 4242*"), ==, 1);
	second = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 1);
	job = claim_paused(fx, fx->token, crashed, &paused);
	g_assert_nonnull(job);
	g_assert_null(paused);
	g_assert_cmpint(job->id, ==, second);
	{
		g_autofree gchar *done = fail_json(job->id, job->lease_token, "done with it", FALSE, FALSE);

		reply = api(fx, "fail", fx->token, done);
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
	}
	axis_worker_job_free(job);
	reply = page_post(fx, fx->admin, "/workers/garage/resume", "");
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=not-paused");
	axis_test_reply_clear(&reply);

	/* Another crash pauses it again; registering again clears it (a worker
	 * restarted has forgotten its crash; one that has not says so again). */
	g_assert_null(claim_paused(fx, fx->token, crashed_again, &paused));
	assert_contains(paused, "job 4343");
	g_clear_pointer(&paused, g_free);
	do_register(fx, fx->token, FALSE, at_a);
	assert_q(fx, "SELECT paused_at IS NULL FROM worker WHERE name = 'garage'", "t");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*worker garage registered again: the pause after its "
		"encoder crashed on job 4343 is lifted*"), ==, 1);
	g_assert_null(claim_paused(fx, fx->token, crashed_again, &paused));
	g_assert_nonnull(paused);
	g_clear_pointer(&paused, g_free);
	do_register(fx, fx->token, FALSE, at_a);
	g_assert_null(claim_paused(fx, fx->token, plain, &paused));
	g_assert_null(paused);
	axis_secret_free(g_steal_pointer(&attic));
}

/* Spec (worker-protocol, A second crash on the same job; hunt finding H3): a
 * worker resumed after a crash on job N, whose tool then crashes on job N
 * again (a check job or a sample measurement is tried again under the same
 * id, and may come back to the same worker), is paused again: the resume is
 * used up by the claim that acknowledges it, whether or not that claim
 * brought work. */
static void
test_second_crash_same_job(Fixture *fx, gconstpointer data)
{
	const gchar *const at_a[] = { UUID_A, "/mnt/a", NULL };
	const gchar *const proved_a[] = { UUID_A, NULL };
	g_autofree gchar *crashed = paused_claim_json(proved_a, 4242, "signal 11, SIGSEGV");
	g_autofree gchar *plain = paused_claim_json(proved_a, 0, NULL);
	g_autofree gchar *paused = NULL;
	AxisWorkerJob *job;
	AxisTestReply reply;
	gint64 again;

	SKIP_WITHOUT_DB(fx);
	do_register(fx, fx->token, FALSE, at_a);

	/* The first crash on job 4242, a person's resume, and the worker taking
	 * the resume (its claim still said the crash; the answer, not paused). */
	g_assert_null(claim_paused(fx, fx->token, crashed, &paused));
	g_assert_nonnull(paused);
	g_clear_pointer(&paused, g_free);
	reply = page_post(fx, fx->admin, "/workers/garage/resume", "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_null(claim_paused(fx, fx->token, crashed, &paused));
	g_assert_null(paused);
	/* It forgot the crash: its claims say nothing of it. */
	g_assert_null(claim_paused(fx, fx->token, plain, &paused));
	g_assert_null(paused);

	/* Job 4242 is tried again on it, and its tool crashes again. */
	g_assert_null(claim_paused(fx, fx->token, crashed, &paused));
	g_assert_nonnull(paused);
	assert_contains(paused, "Its encoder crashed on job 4242");
	g_clear_pointer(&paused, g_free);

	/* Resumed again; this time the claim that acknowledges it brings the
	 * job back at once, and its tool crashes on it again before any other
	 * claim. */
	reply = page_post(fx, fx->admin, "/workers/garage/resume", "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	again = enqueue(fx, AXIS_WORKER_KIND_CAPABILITIES, "{}", 1);
	job = claim_paused(fx, fx->token, crashed, &paused);
	g_assert_nonnull(job);
	g_assert_null(paused);
	g_assert_cmpint(job->id, ==, again);
	{
		g_autofree gchar *done = fail_json(job->id, job->lease_token, "its encoder crashed", TRUE, FALSE);

		reply = api(fx, "fail", fx->token, done);
		g_assert_cmpuint(reply.status, ==, 200);
		axis_test_reply_clear(&reply);
	}
	axis_worker_job_free(job);
	g_assert_null(claim_paused(fx, fx->token, crashed, &paused));
	g_assert_nonnull(paused);
	assert_q(fx, "SELECT paused_job || ' ' || (paused_at IS NOT NULL) FROM worker WHERE name = 'garage'", "4242 true");
}

/*
 * add-attention-notices, after the N2 review (S4): the workers' notices on
 * the page "Needs attention".  A worker is not said to be silent until this
 * daemon has been up for the wait (while it was down nobody could be heard);
 * then the one silent worker is named, with how long; a refused worker is
 * its own notice and not also a silent one; and one silent for more than a
 * day is taken as switched off.
 */
static void
test_attention(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *attic = NULL;
	AxisTestReply reply;
	gint64 attic_id = 0;

	SKIP_WITHOUT_DB(fx);
	attic = axis_test_daemon_add_worker(fx->d, "attic", &attic_id);
	axis_test_db_exec(fx->db, "UPDATE worker SET last_seen_at = now() - interval '25 minutes' WHERE name = 'garage'");
	axis_test_db_exec(fx->db, "UPDATE worker SET last_seen_at = now() WHERE name = 'attic'");

	/* The daemon has only just started: nothing is said of a worker last
	 * heard 25 minutes ago. */
	axis_test_web_read_attention(fx->d->web);
	reply = page_get(fx, fx->admin, "/attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Nothing needs attention.");
	axis_test_reply_clear(&reply);

	/* Eleven minutes on (the readings' clock), the other worker heard at
	 * that moment: the silent one is named, with how long. */
	axis_test_db_exec(fx->db, "UPDATE worker SET last_seen_at = now() + interval '11 minutes' WHERE name = 'attic'");
	axis_test_web_read_attention_at(fx->d->web, 11);
	reply = page_get(fx, fx->admin, "/attention");
	assert_contains(reply.body, "id=\"attention-look\"");
	assert_contains(reply.body, "<span>The worker </span><bdi>garage</bdi><span> has not been heard from for 36 minutes."
		"</span>");
	assert_contains(reply.body, "<a href=\"/workers\">Open Conversion workers</a>");
	assert_lacks(reply.body, "more than 10 minutes");
	axis_test_reply_clear(&reply);

	/* Refused for its version: that is its notice, and it is not counted
	 * as silent too. */
	axis_test_db_exec(fx->db, "UPDATE worker SET refused_protocol = 1, refused_protocol_at = now() WHERE name = 'garage'");
	axis_test_web_read_attention_at(fx->d->web, 11);
	reply = page_get(fx, fx->admin, "/attention");
	assert_contains(reply.body, "1 worker was refused: it runs another version of Axis than this server. Update it to "
		"the same release.");
	assert_lacks(reply.body, "has not been heard from");
	axis_test_reply_clear(&reply);

	/* Silent for two days: taken as switched off, no notice. */
	axis_test_db_exec(fx->db, "UPDATE worker SET refused_protocol = NULL, refused_protocol_at = NULL,"
		" last_seen_at = now() - interval '2 days' WHERE name = 'garage'");
	axis_test_web_read_attention_at(fx->d->web, 11);
	reply = page_get(fx, fx->admin, "/attention");
	assert_contains(reply.body, "Nothing needs attention.");
	axis_test_reply_clear(&reply);

	/* Both silent: their number, and the shorter of the two silences. */
	axis_test_db_exec(fx->db, "UPDATE worker SET last_seen_at = now() - interval '25 minutes' WHERE name = 'garage'");
	axis_test_db_exec(fx->db, "UPDATE worker SET last_seen_at = now() - interval '9 minutes' WHERE name = 'attic'");
	axis_test_web_read_attention_at(fx->d->web, 11);
	reply = page_get(fx, fx->admin, "/attention");
	assert_contains(reply.body, "2 workers have not been heard from for 20 minutes or longer.");
	axis_test_reply_clear(&reply);
}

#define ADD(name, func) g_test_add("/worker/api/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/worker/api/registry", test_registry);
	ADD("gate", test_gate);
	ADD("throttle", test_throttle);
	ADD("register", test_register);
	ADD("claim", test_claim);
	ADD("volume-without-marker", test_volume_without_marker);
	ADD("not-in-process", test_not_in_process);
	ADD("heartbeat", test_heartbeat);
	ADD("cancel", test_cancel);
	ADD("revoke", test_revoke);
	ADD("fail", test_fail);
	ADD("complete", test_complete);
	ADD("page", test_page);
	g_test_add("/web/workers/back-to-place", Fixture, NULL, fixture_setup, test_back_to_place, fixture_teardown);
	ADD("old-protocol", test_old_protocol);
	ADD("prefer-other", test_prefer_other);
	ADD("volume-identity", test_volume_identity);
	ADD("pause", test_pause);
	ADD("second-crash-same-job", test_second_crash_same_job);
	ADD("attention", test_attention);
	return g_test_run();
}
