/*
 * test-web-health.c - the probes: what /healthz says to whom, and that a
 * flood of probes asks the database one question at a time
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The shell runs on an ephemeral loopback port with an auth that has no
 * database; the probes' questions are fakes that count how often they are
 * asked and can be held, as a slow database would hold them.  No database
 * is needed.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "axis-version.h"
#include "core/axis-module.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-health.h"
#include "web/axis-web-metrics.h"

#define METRICS_TOKEN "made-up-metrics-token-0123456789"
#define REUSE_MS      300
#define BURST         20

typedef struct
{
	GMutex   lock;
	GCond    cond;
	gboolean open;		/* (lock) the questions may answer */
	gboolean reachable;	/* (lock) what they answer */
	gboolean held;		/* the lock, for /readyz */
} Database;

typedef struct
{
	HtmxServer         *server;
	AxisModuleRegistry *registry;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisWebMetrics     *metrics;
	AxisWebHealth      *health;
	SoupSession        *session;
	guint16             port;
	Database            database;
} Fixture;

/* A question to the fake database: waits while it is closed (at most ten
 * seconds), then says whether it is reachable. */
static gboolean
ask_database(
	gpointer  data,
	GError  **error
){
	Database *database = data;
	gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	gboolean ok;

	g_mutex_lock(&database->lock);
	while (!database->open && g_cond_wait_until(&database->cond, &database->lock, until))
		;
	ok = database->reachable;
	g_mutex_unlock(&database->lock);

	if (!ok)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "made-up refusal");
	return ok;
}

static gboolean
lock_held(gpointer data)
{
	Database *database = data;

	return database->held;
}

static void
database_set(
	Database *database,
	gboolean  open,
	gboolean  reachable
){
	g_mutex_lock(&database->lock);
	database->open = open;
	database->reachable = reachable;
	g_cond_broadcast(&database->cond);
	g_mutex_unlock(&database->lock);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	AxisWebHealthHooks hooks;
	GError *error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	g_mutex_init(&fx->database.lock);
	g_cond_init(&fx->database.cond);
	fx->database.open = TRUE;
	fx->database.reachable = TRUE;
	fx->database.held = TRUE;

	fx->registry = axis_module_registry_new();
	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->metrics = axis_web_metrics_new(METRICS_TOKEN);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);

	memset(&hooks, 0, sizeof(hooks));
	hooks.ping = ask_database;
	hooks.current = ask_database;
	hooks.held = lock_held;
	hooks.data = &fx->database;
	hooks.metrics = fx->metrics;
	hooks.reuse_ms = REUSE_MS;
	fx->health = axis_web_health_new(&hooks);
	axis_web_health_register(fx->health, fx->web);

	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, "max-conns", 64,
		"max-conns-per-host", 64, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	database_set(&fx->database, TRUE, TRUE);
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_health_free(fx->health);
	axis_web_free(fx->web);
	axis_web_metrics_free(fx->metrics);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	g_mutex_clear(&fx->database.lock);
	g_cond_clear(&fx->database.cond);
	axis_test_log_reset();
}

static AxisTestReply
probe(
	Fixture     *fx,
	const gchar *path,
	const gchar *cookie,
	const gchar *token
){
	g_autofree gchar *bearer = token != NULL ? g_strconcat("Bearer ", token, NULL) : NULL;
	const gchar *const extra[] = { "Authorization", bearer, NULL };

	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, token != NULL ? extra : NULL, NULL);
}

/* The members of a JSON object body, in order, joined by commas. */
static gchar *
members_of(const gchar *body)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(GList) names = NULL;
	GString *out = g_string_new(NULL);
	GList *l;

	json_parser_load_from_data(parser, body, -1, &error);
	g_assert_no_error(error);
	names = json_object_get_members(json_node_get_object(json_parser_get_root(parser)));
	for (l = names; l != NULL; l = l->next)
		g_string_append_printf(out, "%s%s", out->len > 0 ? "," : "", (const gchar *)l->data);
	return g_string_free(out, FALSE);
}

/* ── Who is told what ───────────────────────────────────────────── */

/*
 * Found by the stage 2 hunt of the web layer (W-6): /healthz told anyone
 * the exact version.  Without a session or the metrics token it says its
 * status word and nothing else.
 */
static void
test_healthz_says_little_to_strangers(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_USER);
	AxisTestReply anonymous = probe(fx, "/healthz", NULL, NULL);
	AxisTestReply wrong = probe(fx, "/healthz", NULL, "not-the-metrics-token-at-all-000");
	AxisTestReply with_session = probe(fx, "/healthz", carol, NULL);
	AxisTestReply with_token = probe(fx, "/healthz", NULL, METRICS_TOKEN);
	g_autofree gchar *full = NULL;

	g_assert_cmpuint(anonymous.status, ==, 200);
	g_assert_cmpstr(anonymous.body, ==, "{\"status\":\"ok\"}");
	g_assert_null(strstr(anonymous.body, AXIS_VERSION));
	g_assert_cmpstr(wrong.body, ==, "{\"status\":\"ok\"}");

	full = members_of(with_session.body);
	g_assert_cmpstr(full, ==, "status,version,database");
	g_assert_nonnull(strstr(with_session.body, AXIS_VERSION));
	g_assert_nonnull(strstr(with_token.body, AXIS_VERSION));

	axis_test_reply_clear(&anonymous);
	axis_test_reply_clear(&wrong);
	axis_test_reply_clear(&with_session);
	axis_test_reply_clear(&with_token);
}

/* A wrong token on /healthz counts as it does on /metrics, and the right
 * one always opens the version (found by the review of
 * fix-web-hunt-findings: a throttled address lost it).  An Authorization
 * header that is no bearer token of the metrics token's form (a
 * forward-auth proxy adds one to everything) is not looked at, nor
 * counted, nor logged. */
static void
test_healthz_token_guesses_counted(Fixture *fx, gconstpointer data)
{
	static const gchar *const basic[] = { "Authorization", "Basic YWxpY2U6c2VjcmV0", NULL };
	AxisTestReply reply;
	guint i;

	for (i = 0; i < 20; i++) {
		reply = axis_test_web_request(fx->session, fx->port, "GET", "/healthz", NULL, basic, NULL);
		g_assert_cmpstr(reply.body, ==, "{\"status\":\"ok\"}");
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "metrics: *"), ==, 0);

	for (i = 0; i < 11; i++) {
		reply = probe(fx, "/healthz", NULL, "not-the-metrics-token-at-all-000");
		g_assert_cmpstr(reply.body, ==, "{\"status\":\"ok\"}");
		axis_test_reply_clear(&reply);
	}
	reply = probe(fx, "/healthz", NULL, METRICS_TOKEN);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, AXIS_VERSION));
	axis_test_reply_clear(&reply);
}

static void
test_probe_words(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply = probe(fx, "/livez", NULL, NULL);

	g_assert_cmpstr(reply.body, ==, "{\"status\":\"alive\"}");
	axis_test_reply_clear(&reply);
	reply = probe(fx, "/readyz", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(reply.body, ==, "{\"status\":\"ready\"}");
	axis_test_reply_clear(&reply);

	fx->database.held = FALSE;
	reply = probe(fx, "/readyz", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 503);
	g_assert_cmpstr(reply.body, ==, "{\"status\":\"database lock lost\"}");
	axis_test_reply_clear(&reply);

	fx->database.held = TRUE;
	database_set(&fx->database, TRUE, FALSE);
	axis_test_pump_for(REUSE_MS + 50);
	reply = probe(fx, "/readyz", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 503);
	g_assert_cmpstr(reply.body, ==, "{\"status\":\"not ready\"}");
	axis_test_reply_clear(&reply);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*the database is unreachable*");
	reply = probe(fx, "/healthz", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 503);
	g_assert_cmpstr(reply.body, ==, "{\"status\":\"degraded\"}");
	axis_test_reply_clear(&reply);
}

/* ── One question at a time ─────────────────────────────────────── */

typedef struct
{
	guint        waiting;
	guint        ok;
	SoupMessage *messages[BURST];
} Burst;

static void
on_burst_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Burst *burst = user_data;
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);

	g_assert_no_error(error);
	if (soup_message_get_status(message) == 200)
		burst->ok++;
	burst->waiting--;
}

static gboolean
burst_finished(gpointer data)
{
	return ((Burst *)data)->waiting == 0;
}

/* Sends BURST of @path at once, and with @wait waits for every answer. */
static void
send_burst(
	Fixture     *fx,
	Burst       *burst,
	const gchar *path,
	gboolean     wait
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)fx->port, path);
	guint i;

	memset(burst, 0, sizeof(*burst));
	for (i = 0; i < BURST; i++) {
		burst->messages[i] = soup_message_new("GET", url);
		burst->waiting++;
	}
	for (i = 0; i < BURST; i++)
		soup_session_send_and_read_async(fx->session, burst->messages[i], G_PRIORITY_DEFAULT, NULL,
			on_burst_done, burst);
	if (wait)
		g_assert_true(axis_test_pump_until(burst_finished, burst, AXIS_TEST_WAIT_MS));
}

static void
burst_clear(Burst *burst)
{
	guint i;

	for (i = 0; i < BURST; i++)
		g_clear_object(&burst->messages[i]);
}

/*
 * Found by the stage 2 hunt of the web layer (W-6): each /healthz took a
 * pooled connection of its own, with no limit, so a flood could take the
 * pool.  Now the probes that arrive while a question is asked wait for its
 * answer, and the answer is reused for a moment.
 */
static void
test_flood_asks_once(Fixture *fx, gconstpointer data)
{
	Burst burst;
	Burst again;

	/* The database is slow: twenty probes, one question. */
	database_set(&fx->database, FALSE, TRUE);
	send_burst(fx, &burst, "/healthz", FALSE);
	axis_test_pump_for(300);
	g_assert_cmpuint(axis_web_health_count_pings(fx->health), ==, 1);
	database_set(&fx->database, TRUE, TRUE);
	g_assert_true(axis_test_pump_until(burst_finished, &burst, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(burst.ok, ==, BURST);
	g_assert_cmpuint(axis_web_health_count_pings(fx->health), ==, 1);
	burst_clear(&burst);

	/* Within the reuse, the answer is the last one; after it, a new
	 * question.  The readiness question is its own. */
	send_burst(fx, &again, "/healthz", TRUE);
	g_assert_cmpuint(again.ok, ==, BURST);
	g_assert_cmpuint(axis_web_health_count_pings(fx->health), ==, 1);
	burst_clear(&again);
	axis_test_pump_for(REUSE_MS + 50);
	send_burst(fx, &again, "/healthz", TRUE);
	g_assert_cmpuint(axis_web_health_count_pings(fx->health), ==, 2);
	burst_clear(&again);
	send_burst(fx, &again, "/readyz", TRUE);
	g_assert_cmpuint(again.ok, ==, BURST);
	g_assert_cmpuint(axis_web_health_count_checks(fx->health), ==, 1);
	burst_clear(&again);
}

#define ADD(name, func) \
	g_test_add("/web-health/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("healthz/says-little-to-strangers", test_healthz_says_little_to_strangers);
	ADD("healthz/token-guesses-counted", test_healthz_token_guesses_counted);
	ADD("probe-words", test_probe_words);
	ADD("flood-asks-once", test_flood_asks_once);

	return g_test_run();
}
