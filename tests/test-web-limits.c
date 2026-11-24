/*
 * test-web-limits.c - what a request and a client may cost: the body
 * limit before the daemon serves, the deadlines for a request's head and
 * body and for a kept-alive connection, the caps on connections in all and
 * per address, a trusted proxy's clients, and event streams under their
 * own limits
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The shell runs on an ephemeral loopback port with an auth that has no
 * database and test sessions; the limits are installed before the shell,
 * with short times.  Clients are plain sockets that pump the test's main
 * loop while they wait, so what the server does is seen byte for byte.
 * The last case runs the real axisd (AXISD_PATH) against a database that
 * does not answer.  Names in here are made up.
 */

#include <gio/gio.h>
#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "api/axis-worker-proto.h"
#include "api/axis-worker-tokens.h"
#include "core/axis-config.h"
#include "core/axis-module.h"
#include "web/axis-password.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-health.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-limits.h"

#ifndef AXISD_PATH
#error "AXISD_PATH must be defined by the build"
#endif

/* Short deadlines, so that a test waits seconds, not minutes. */
#define HEAD_MS 600
#define BODY_MS 600
#define IDLE_MS 600

enum
{
	TWO_PER_ADDRESS = 1 << 0,
	TWO_IN_ALL      = 1 << 1,
	PROXY           = 1 << 2,	/* 127.0.0.1 is a trusted proxy */
	NO_BODIES       = 1 << 3,
	LOCAL_PUBLIC    = 1 << 4,	/* loopback held to the per-address cap */
	LONG_BODY       = 1 << 5,	/* a minute for a body */
	ONE_OVERFLOW    = 1 << 6	/* one connection over a cap let in */
};

typedef struct
{
	HtmxServer         *server;
	AxisModuleRegistry *registry;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisWebHub         *hub;
	AxisWebLimits      *limits;
	AxisWorkerTokens   *tokens;
	gchar              *worker_token;	/* a worker's, known to the auth */
	SoupSession        *session;
	guint16             port;
	GPtrArray          *held;	/* deferred answers of /hold, not yet given */
} Fixture;

static Fixture *current;

/* ── A module with a page and a request that waits ──────────────── */

static HtmxResponse *
page_plain(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "Plain page.");
	return axis_web_page_respond(request, "Plain", HTMX_NODE(text));
}

/* Answered only when the test says so: a request in flight. */
static HtmxResponse *
page_hold(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	HtmxResponse *deferred = htmx_response_new_deferred();

	g_ptr_array_add(current->held, g_object_ref(deferred));
	return deferred;
}

static HtmxResponse *
page_poke(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "Poked.");
	return axis_web_page_respond(request, "Poke", HTMX_NODE(text));
}

static HtmxResponse *
page_livez(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return axis_web_health_probe_response(200, "alive");
}

/* A worker's request answered at once, and one held in flight. */
static HtmxResponse *
worker_echo(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	return axis_web_health_probe_response(200, "echo");
}

static void
pages_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_get(web, "/plain", page_plain, NULL);
	axis_web_route_full(web, HTMX_METHOD_GET, "/login", AXIS_WEB_ROUTE_PUBLIC, page_plain, NULL);
	axis_web_route_full(web, HTMX_METHOD_GET, "/livez", AXIS_WEB_ROUTE_PUBLIC, page_livez, NULL);
	axis_web_get(web, "/hold", page_hold, NULL);
	axis_web_post(web, "/poke", page_poke, NULL);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "echo", AXIS_WEB_ROUTE_WORKER, worker_echo, NULL);
	axis_web_route_full(web, HTMX_METHOD_POST, AXIS_WORKER_API_PREFIX "hold", AXIS_WEB_ROUTE_WORKER, page_hold, NULL);
}

/* ── Fixture ────────────────────────────────────────────────────── */

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);
	AxisWebLimitsOptions options;
	AxisModuleInfo info;
	GError *error = NULL;

	memset(fx, 0, sizeof(*fx));
	current = fx;
	axis_test_log_init();
	axis_test_log_reset();
	fx->held = g_ptr_array_new_with_free_func(g_object_unref);

	fx->registry = axis_module_registry_new();
	memset(&info, 0, sizeof(info));
	info.id = "pages";
	info.name = "Pages";
	info.register_routes = pages_routes;
	g_assert_true(axis_module_registry_register(fx->registry, &info, NULL, &error));
	g_assert_no_error(error);

	memset(&options, 0, sizeof(options));
	options.head_timeout_ms = HEAD_MS;
	options.body_timeout_ms = (flags & LONG_BODY) ? 60000 : BODY_MS;
	options.idle_timeout_ms = IDLE_MS;
	if (flags & TWO_PER_ADDRESS)
		options.max_per_address = 2;
	if (flags & TWO_IN_ALL)
		options.max_connections = 2;
	if (flags & ONE_OVERFLOW)
		options.max_overflow = 1;
	options.local_is_public = (flags & LOCAL_PUBLIC) != 0;

	fx->server = htmx_server_new();
	fx->limits = axis_web_limits_install(fx->server, &options);
	if (flags & NO_BODIES)
		axis_web_limits_set_bodies(fx->limits, FALSE);
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(NULL, (flags & PROXY) ? "127.0.0.1" : "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->tokens = axis_worker_tokens_new();
	fx->worker_token = axis_random_token();
	{
		g_autofree gchar *hash = axis_token_hash(fx->worker_token);

		axis_worker_tokens_put(fx->tokens, 1, "garage", hash, FALSE);
	}
	axis_web_auth_set_worker_tokens(fx->auth, fx->tokens);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	fx->hub = axis_web_hub_new(NULL);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	guint i;

	for (i = 0; i < fx->held->len; i++) {
		g_autoptr(HtmxResponse) answer = htmx_response_new();

		htmx_response_set_content(answer, "late");
		htmx_response_complete(g_ptr_array_index(fx->held, i), answer);
	}
	g_ptr_array_unref(fx->held);
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_auth_free(fx->auth);
	axis_worker_tokens_free(fx->tokens);
	axis_secret_free(fx->worker_token);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_test_log_reset();
	current = NULL;
}

/* ── A raw client ───────────────────────────────────────────────── */

static GSocketConnection *
raw_open(Fixture *fx)
{
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *where = g_strdup_printf("127.0.0.1:%u", (guint)fx->port);
	GSocketConnection *connection = g_socket_client_connect_to_host(client, where, 0, NULL, &error);

	g_assert_no_error(error);
	g_socket_set_blocking(g_socket_connection_get_socket(connection), FALSE);
	return connection;
}

static void
raw_send(
	GSocketConnection *connection,
	const gchar       *text
){
	GSocket *socket = g_socket_connection_get_socket(connection);
	gsize done = 0;

	while (done < strlen(text)) {
		g_autoptr(GError) error = NULL;
		gssize n = g_socket_send(socket, text + done, strlen(text) - done, NULL, &error);

		if (n < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
			axis_test_pump_for(5);
			continue;
		}
		g_assert_no_error(error);
		done += (gsize)n;
	}
}

/*
 * Reads what has arrived into @got (nullable) and says whether the server
 * has ended the connection (the end of the stream, or a reset).
 */
static gboolean
raw_ended(
	GSocketConnection *connection,
	GString           *got
){
	GSocket *socket = g_socket_connection_get_socket(connection);

	for (;;) {
		g_autoptr(GError) error = NULL;
		gchar buffer[4096];
		gssize n = g_socket_receive(socket, buffer, sizeof(buffer), NULL, &error);

		if (n > 0) {
			if (got != NULL)
				g_string_append_len(got, buffer, n);
			continue;
		}
		if (n == 0)
			return TRUE;
		return !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
	}
}

/* Pumps for up to @ms until the server ends @connection. */
static gboolean
raw_wait_ended(
	GSocketConnection *connection,
	guint              ms,
	GString           *got
){
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline) {
		if (raw_ended(connection, got))
			return TRUE;
		axis_test_pump_for(20);
	}
	return raw_ended(connection, got);
}

static gboolean
connections_are(gpointer data)
{
	gpointer *pair = data;

	return axis_web_limits_count_connections(pair[0]) == GPOINTER_TO_UINT(pair[1]);
}

static void
wait_for_connections(
	Fixture *fx,
	guint    n
){
	gpointer pair[2] = { fx->limits, GUINT_TO_POINTER(n) };

	if (!axis_test_pump_until(connections_are, pair, AXIS_TEST_WAIT_MS))
		g_error("expected %u connections, the limits count %u", n, axis_web_limits_count_connections(fx->limits));
}

/* ── Deadlines ──────────────────────────────────────────────────── */

/* The defaults are what deploy/README.md says, and generous only so far:
 * nginx gives a whole head 60 s, Apache 20 to 40. */
static void
test_defaults(void)
{
	g_autoptr(AxisConfig) config = axis_config_new();

	g_assert_cmpuint(axis_config_get_head_timeout(config), ==, AXIS_WEB_LIMITS_HEAD_TIMEOUT_S);
	g_assert_cmpuint(axis_config_get_body_timeout(config), ==, AXIS_WEB_LIMITS_BODY_TIMEOUT_S);
	g_assert_cmpuint(axis_config_get_idle_timeout(config), ==, AXIS_WEB_LIMITS_IDLE_TIMEOUT_S);
	g_assert_cmpuint(axis_config_get_max_connections(config), ==, AXIS_WEB_LIMITS_MAX_CONNECTIONS);
	g_assert_cmpuint(axis_config_get_max_connections_per_address(config), ==, AXIS_WEB_LIMITS_MAX_PER_ADDRESS);
	g_assert_cmpuint(AXIS_WEB_LIMITS_HEAD_TIMEOUT_S, <=, 45);
	g_assert_cmpuint(AXIS_WEB_LIMITS_IDLE_TIMEOUT_S, <=, 120);
}

/*
 * Found by the stage 2 hunt of the web layer (W-3): a request that sends
 * part of its head and then nothing was held for as long as the client
 * liked.  It is closed once the head deadline passes, and a connection that
 * sends nothing at all is counted from the moment it opens.
 */
static void
test_slow_head_closed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) partial = raw_open(fx);
	g_autoptr(GSocketConnection) silent = raw_open(fx);
	gint64 start = g_get_monotonic_time();

	raw_send(partial, "GET /login HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Slow: ");
	/* Seen before a byte is read from it. */
	wait_for_connections(fx, 2);

	g_assert_true(raw_wait_ended(partial, HEAD_MS * 10, NULL));
	g_assert_true(raw_wait_ended(silent, HEAD_MS * 10, NULL));
	g_assert_cmpint(g_get_monotonic_time() - start, >=, (gint64)HEAD_MS * 1000);
	g_assert_cmpuint(axis_web_limits_count_timed_out(fx->limits), ==, 2);
	wait_for_connections(fx, 0);
}

/* A head that arrives in time is answered, whatever the deadline. */
static void
test_head_in_time_answered(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) client = raw_open(fx);
	g_autoptr(GString) got = g_string_new(NULL);

	raw_send(client, "GET /login HTTP/1.1\r\nHost: 127.0.0.1\r\n");
	axis_test_pump_for(HEAD_MS / 3);
	raw_send(client, "Connection: close\r\n\r\n");
	g_assert_true(raw_wait_ended(client, AXIS_TEST_WAIT_MS, got));
	g_assert_true(g_str_has_prefix(got->str, "HTTP/1.1 200"));
	g_assert_cmpuint(axis_web_limits_count_timed_out(fx->limits), ==, 0);
}

/* A body that stops arriving is cut off at its own deadline. */
static void
test_slow_body_closed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) client = raw_open(fx);
	gint64 start;

	raw_send(client, "POST /poke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
		"Content-Length: 100\r\n\r\nx=1");
	start = g_get_monotonic_time();
	g_assert_true(raw_wait_ended(client, BODY_MS * 10, NULL));
	g_assert_cmpint(g_get_monotonic_time() - start, >=, (gint64)BODY_MS * 1000 * 3 / 4);
	g_assert_cmpuint(axis_web_limits_count_timed_out(fx->limits), ==, 1);
}

/* A kept-alive connection that asks nothing more is let go; one that
 * asks again in time is served again. */
static void
test_idle_keep_alive_closed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) client = raw_open(fx);
	g_autoptr(GString) got = g_string_new(NULL);

	raw_send(client, "GET /login HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	axis_test_pump_for(IDLE_MS / 3);
	g_assert_false(raw_ended(client, got));
	raw_send(client, "GET /login HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	axis_test_pump_for(IDLE_MS / 3);
	g_assert_false(raw_ended(client, got));
	g_assert_true(raw_wait_ended(client, IDLE_MS * 10, got));
	g_assert_nonnull(strstr(strstr(got->str, "HTTP/1.1 200") + 1, "HTTP/1.1 200"));
	g_assert_cmpuint(axis_web_limits_count_timed_out(fx->limits), ==, 1);
}

/* An event stream is not idle, and has no body: it outlives every
 * deadline, under the hub's own limits. */
static void
test_event_stream_outlives_deadlines(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_USER);
	g_autoptr(AxisTestStream) stream = axis_test_stream_open(fx->port, "/events?topic=news", carol, 0);

	g_assert_cmpuint(axis_test_stream_get_status(stream), ==, 200);
	g_assert_true(axis_test_stream_wait_for(stream, ": open", AXIS_TEST_WAIT_MS));
	axis_test_pump_for(MAX(HEAD_MS, MAX(BODY_MS, IDLE_MS)) * 3);
	g_assert_false(axis_test_stream_is_closed(stream));
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "<p>still</p>"), ==, 1);
	g_assert_true(axis_test_stream_wait_for(stream, "data: <p>still</p>", AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_web_limits_count_timed_out(fx->limits), ==, 0);
}

/* ── Caps ───────────────────────────────────────────────────────── */

/* A connection whose request is in flight (GET /hold): it holds its place
 * for as long as the test likes, no deadline running. */
static GSocketConnection *
raw_hold(Fixture *fx)
{
	GSocketConnection *connection = raw_open(fx);
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_USER);
	g_autofree gchar *request = g_strdup_printf("GET /hold HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: %s\r\n\r\n", carol);
	guint held = fx->held->len;
	gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;

	raw_send(connection, request);
	while (fx->held->len == held && g_get_monotonic_time() < deadline)
		axis_test_pump_for(10);
	g_assert_cmpuint(fx->held->len, ==, held + 1);
	return connection;
}

/* GET @path on a new connection, Connection: close; what came back. */
static gchar *
raw_get(
	Fixture     *fx,
	const gchar *path
){
	g_autoptr(GSocketConnection) client = raw_open(fx);
	g_autofree gchar *request = g_strdup_printf("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
	GString *got = g_string_new(NULL);

	raw_send(client, request);
	g_assert_true(raw_wait_ended(client, AXIS_TEST_WAIT_MS, got));
	return g_string_free(got, FALSE);
}

/*
 * From a public address (loopback taken for one here), a connection over
 * the per-address cap is answered 503 with Retry-After once its head has
 * arrived (found by the review of fix-web-hunt-findings: it was closed
 * without an answer), and the hint says what the cap refused.
 */
static void
test_per_address_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) first = raw_hold(fx);
	g_autoptr(GSocketConnection) second = raw_hold(fx);
	g_autofree gchar *third = NULL;
	g_autofree gchar *later = NULL;
	g_autofree gchar *hint = NULL;

	wait_for_connections(fx, 2);

	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*refused a request from 127.0.0.1*");
	third = raw_get(fx, "/login");
	g_assert_true(g_str_has_prefix(third, "HTTP/1.1 503"));
	g_assert_nonnull(strstr(third, "Retry-After: 5"));
	g_assert_nonnull(strstr(third, "too many connections open right now"));
	g_assert_cmpuint(axis_web_limits_count_refused(fx->limits), ==, 1);
	hint = axis_web_auth_forwarding_hint(fx->auth);
	g_assert_nonnull(hint);
	g_assert_nonnull(strstr(hint, "turned away 1 connection or request"));
	g_assert_nonnull(strstr(hint, "AXIS_TRUSTED_PROXIES"));

	/* Room is made when one ends. */
	{
		g_autoptr(HtmxResponse) answer = htmx_response_new();

		htmx_response_set_content(answer, "done");
		htmx_response_complete(g_ptr_array_index(fx->held, 0), answer);
		g_ptr_array_remove_index(fx->held, 0);
	}
	g_clear_object(&first);
	wait_for_connections(fx, 1);
	later = raw_get(fx, "/login");
	g_assert_true(g_str_has_prefix(later, "HTTP/1.1 200"));
}

/*
 * Found by the review of fix-web-hunt-findings: behind a reverse proxy that
 * is not trusted (a usual first setup), every client is the proxy's one
 * address, and 64 connections for the whole site filled at once.  A
 * loopback, private or link-local address is held to the total only.
 */
static void
test_local_not_capped_per_address(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) a = raw_hold(fx);
	g_autoptr(GSocketConnection) b = raw_hold(fx);
	g_autoptr(GSocketConnection) c = raw_hold(fx);
	g_autoptr(GSocketConnection) d = raw_hold(fx);
	g_autofree gchar *fifth = NULL;

	wait_for_connections(fx, 4);
	fifth = raw_get(fx, "/login");
	g_assert_true(g_str_has_prefix(fifth, "HTTP/1.1 200"));
	g_assert_cmpuint(axis_web_limits_count_refused(fx->limits), ==, 0);
	g_assert_false(raw_ended(a, NULL));
}

/*
 * The total: over it, a connection is answered 503, except a probe from
 * beside the daemon (loopback, private), which is served however full the
 * daemon is (found by the review: a full daemon failed its liveness probe
 * and would be restarted); and beyond the connections over a cap let in at
 * once, one is closed as soon as it is seen.
 */
static void
test_total_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) first = raw_hold(fx);
	g_autoptr(GSocketConnection) second = raw_hold(fx);
	g_autoptr(GSocketConnection) waiting = NULL;
	g_autoptr(GSocketConnection) beyond = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *probe = NULL;

	wait_for_connections(fx, 2);
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*refused a * from 127.0.0.1*");
	page = raw_get(fx, "/login");
	g_assert_true(g_str_has_prefix(page, "HTTP/1.1 503"));
	g_assert_nonnull(strstr(page, "Retry-After: 5"));
	probe = raw_get(fx, "/livez");
	g_assert_true(g_str_has_prefix(probe, "HTTP/1.1 200"));
	g_assert_nonnull(strstr(probe, "{\"status\":\"alive\"}"));
	g_assert_false(raw_ended(second, NULL));

	/* One over the cap waits for its head (the one let in); the next is
	 * closed at once, unanswered. */
	waiting = raw_open(fx);
	wait_for_connections(fx, 3);
	beyond = raw_open(fx);
	g_assert_true(raw_wait_ended(beyond, AXIS_TEST_WAIT_MS, NULL));
	g_assert_false(raw_ended(waiting, NULL));
	/* And the one let in is closed when its two seconds are up. */
	g_assert_true(raw_wait_ended(waiting, AXIS_TEST_WAIT_MS, NULL));
}

/* GET /hold for @client through the trusted proxy (127.0.0.1); the
 * request stays in flight until the test answers it. */
static GSocketConnection *
hold_through_proxy(
	Fixture     *fx,
	const gchar *client
){
	GSocketConnection *connection = raw_open(fx);
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_USER);
	g_autofree gchar *request = g_strdup_printf("GET /hold HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: %s\r\n"
		"X-Forwarded-For: %s\r\n\r\n", carol, client);

	raw_send(connection, request);
	return connection;
}

static gboolean
held_are(gpointer data)
{
	gpointer *pair = data;

	return ((Fixture *)pair[0])->held->len == GPOINTER_TO_UINT(pair[1]);
}

/*
 * Behind a trusted proxy every client comes from its address: the proxy is
 * not held to the per-address cap (which would lock everyone out), and each
 * client it forwards is held to that cap in requests in flight.
 */
static void
test_proxy_clients_capped_not_proxy(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) a1 = hold_through_proxy(fx, "203.0.113.7");
	g_autoptr(GSocketConnection) a2 = hold_through_proxy(fx, "203.0.113.7");
	g_autoptr(GSocketConnection) b1 = hold_through_proxy(fx, "203.0.113.8");
	g_autoptr(GSocketConnection) a3 = NULL;
	g_autoptr(GString) got = g_string_new(NULL);
	gpointer pair[2] = { fx, GUINT_TO_POINTER(3) };

	/* Three connections from the proxy, two more than one address may
	 * have: all are taken, and all three requests are in flight. */
	g_assert_true(axis_test_pump_until(held_are, pair, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_web_limits_count_connections(fx->limits), ==, 3);

	/* A third in flight for the same client is refused; the others are
	 * not touched. */
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*refused a request from 203.0.113.7 through 127.0.0.1*");
	a3 = hold_through_proxy(fx, "203.0.113.7");
	g_assert_true(raw_wait_ended(a3, AXIS_TEST_WAIT_MS, got));
	g_assert_true(g_str_has_prefix(got->str, "HTTP/1.1 429"));
	g_assert_nonnull(strstr(got->str, "Retry-After: 5"));
	g_assert_cmpuint(fx->held->len, ==, 3);
	g_assert_false(raw_ended(a1, NULL));
	g_assert_false(raw_ended(b1, NULL));
}

/* POSTs to the worker API's @request on a new connection with the
 * fixture's worker token (through the proxy for @client, when given). */
static GSocketConnection *
worker_post(
	Fixture     *fx,
	const gchar *request,
	const gchar *client
){
	GSocketConnection *connection = raw_open(fx);
	g_autofree gchar *forwarded = client != NULL ? g_strdup_printf("X-Forwarded-For: %s\r\n", client) : g_strdup("");
	g_autofree gchar *text = g_strdup_printf("POST " AXIS_WORKER_API_PREFIX "%s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
		"Authorization: Bearer %s\r\n%sContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}", request,
		fx->worker_token, forwarded);

	raw_send(connection, text);
	return connection;
}

/*
 * Found by the review of fix-web-hunt-findings: behind a trusted proxy the
 * requests in flight per client counted event streams and the worker API,
 * so a household's tabs could make a worker's report 429 (which the worker
 * took for a refusal, losing its encode).  Neither counts now.
 */
static void
test_proxy_streams_and_workers_not_counted(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) a1 = hold_through_proxy(fx, "203.0.113.7");
	g_autoptr(GSocketConnection) a2 = hold_through_proxy(fx, "203.0.113.7");
	g_autoptr(GSocketConnection) worker = NULL;
	g_autoptr(AxisTestStream) stream = NULL;
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_USER);
	g_autofree gchar *events = NULL;
	gpointer pair[2] = { fx, GUINT_TO_POINTER(2) };

	g_assert_true(axis_test_pump_until(held_are, pair, AXIS_TEST_WAIT_MS));

	/* The client has as many requests in flight as it may: its event
	 * stream is still served ... */
	{
		g_autoptr(GSocketConnection) connection = raw_open(fx);
		g_autoptr(GString) got = g_string_new(NULL);

		events = g_strdup_printf("GET /events?topic=news HTTP/1.1\r\nHost: 127.0.0.1\r\nCookie: %s\r\n"
			"X-Forwarded-For: 203.0.113.7\r\n\r\n", carol);
		raw_send(connection, events);
		{
			gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;

			while (strstr(got->str, ": open") == NULL && g_get_monotonic_time() < deadline) {
				raw_ended(connection, got);
				axis_test_pump_for(20);
			}
		}
		g_assert_true(g_str_has_prefix(got->str, "HTTP/1.1 200"));
	}

	/* ... and so is a worker, with its token, through the same proxy for
	 * the same client. */
	worker = worker_post(fx, "hold", "203.0.113.7");
	pair[1] = GUINT_TO_POINTER(3);
	g_assert_true(axis_test_pump_until(held_are, pair, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(axis_web_limits_count_refused(fx->limits), ==, 0);
}

/* A worker with its token is not held to the per-address cap, and its
 * kept-alive connection is not closed for being idle: its next report goes
 * on the same connection. */
static void
test_worker_exempt(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) first = raw_hold(fx);
	g_autoptr(GSocketConnection) second = raw_hold(fx);
	g_autoptr(GSocketConnection) worker = NULL;
	g_autoptr(GString) got = g_string_new(NULL);

	wait_for_connections(fx, 2);
	worker = worker_post(fx, "echo", NULL);
	{
		gint64 deadline = g_get_monotonic_time() + AXIS_TEST_WAIT_MS * 1000;

		while (strstr(got->str, "echo") == NULL && g_get_monotonic_time() < deadline) {
			g_assert_false(raw_ended(worker, got));
			axis_test_pump_for(20);
		}
	}
	g_assert_true(g_str_has_prefix(got->str, "HTTP/1.1 200"));

	/* Past the idle limit, the worker's connection is still there. */
	axis_test_pump_for(IDLE_MS * 3);
	g_assert_false(raw_ended(worker, got));
	g_assert_cmpuint(axis_web_limits_count_refused(fx->limits), ==, 0);
}

/* ── Bodies ─────────────────────────────────────────────────────── */

/*
 * While bodies are refused (the daemon does not serve yet), any body is
 * answered what the start-up gate would say (503, Retry-After), and no byte
 * of it is read (found by the review of fix-web-hunt-findings: a bare 413
 * after reading it): a request that promises 100 MB and sends a little is
 * answered at once, long before its body deadline (a minute here).  A
 * request without a body is answered as ever.
 */
static void
test_bodies_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GSocketConnection) promised = raw_open(fx);
	g_autoptr(GSocketConnection) chunked = NULL;
	g_autoptr(GString) got = g_string_new(NULL);
	AxisTestReply page;
	gint64 start;

	start = g_get_monotonic_time();
	raw_send(promised, "POST /poke HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
		"Content-Length: 100000000\r\n\r\nx=1&y=2");
	g_assert_true(raw_wait_ended(promised, AXIS_TEST_WAIT_MS, got));
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5 * G_USEC_PER_SEC);
	if (!g_str_has_prefix(got->str, "HTTP/1.1 503"))
		g_error("expected a 503, got: %s", got->str);
	g_assert_nonnull(strstr(got->str, "Retry-After: 5"));
	g_assert_nonnull(strstr(got->str, "not ready for this request"));
	g_assert_nonnull(strstr(got->str, "X-Content-Type-Options: nosniff"));

	g_string_truncate(got, 0);
	chunked = raw_open(fx);
	raw_send(chunked, "POST /poke HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nx=1\r\n0\r\n\r\n");
	g_assert_true(raw_wait_ended(chunked, AXIS_TEST_WAIT_MS, got));
	g_assert_true(g_str_has_prefix(got->str, "HTTP/1.1 503"));

	page = axis_test_web_request(fx->session, fx->port, "GET", "/login", NULL, NULL, NULL);
	g_assert_cmpuint(page.status, ==, 200);
	axis_test_reply_clear(&page);

	/* Once they are accepted, a body within the limit gets to its route
	 * (here: no session, so the login page). */
	axis_web_limits_set_bodies(fx->limits, TRUE);
	page = axis_test_web_request(fx->session, fx->port, "POST", "/poke", NULL, NULL, "x=1");
	g_assert_cmpuint(page.status, !=, 413);
	axis_test_reply_clear(&page);
}

/* ── The real daemon, before its database is ready ──────────────── */

typedef struct
{
	GSubprocess *proc;
	gchar       *scratch;
	gchar       *data_dir;
	guint16      port;
} Daemon;

static void
child_setup(gpointer user_data)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
}

static guint16
pick_free_port(void)
{
	return axis_test_free_port();
}

static void
daemon_setup(Daemon *fx, gconstpointer data)
{
	static const gchar *const clean[] = {
		"AXIS_LISTEN_HOST", "AXIS_LISTEN_PORT", "AXIS_DATABASE_URL", "AXIS_DATABASE_URL_FILE",
		"AXIS_DATABASE_POOL_SIZE", "AXIS_DATABASE_WAIT", "AXIS_LOG_LEVEL", "AXIS_LOG_FORMAT",
		"AXIS_JOBS_CONCURRENCY", "AXIS_JOBS_GRACE_PERIOD", "AXIS_TRUSTED_PROXIES", "AXIS_SECURE_COOKIES",
		"AXIS_WORKER_LEASE", "AXIS_DATA_DIR", "AXIS_ADMIN_USERNAME", "AXIS_ADMIN_PASSWORD",
		"AXIS_ADMIN_PASSWORD_FILE", "AXIS_METRICS_TOKEN", "AXIS_METRICS_TOKEN_FILE", "AXIS_HEAD_TIMEOUT",
		"AXIS_BODY_TIMEOUT", "AXIS_IDLE_TIMEOUT", "AXIS_MAX_CONNECTIONS", "AXIS_MAX_CONNECTIONS_PER_ADDRESS", NULL
	};
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *port = NULL;
	g_autofree gchar *url = NULL;
	const gchar *argv[] = { AXISD_PATH, NULL };
	guint i;

	memset(fx, 0, sizeof(*fx));
	fx->scratch = axis_test_storage_dir_new();
	fx->data_dir = g_build_filename(fx->scratch, "data", NULL);
	fx->port = pick_free_port();
	port = g_strdup_printf("%u", (guint)fx->port);
	/* Nothing listens there: the daemon waits for its database for ever. */
	url = g_strdup_printf("postgresql://axis:made-up-pw@127.0.0.1:%u/axis", (guint)pick_free_port());

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	for (i = 0; clean[i] != NULL; i++)
		g_subprocess_launcher_unsetenv(launcher, clean[i]);
	g_subprocess_launcher_setenv(launcher, "AXIS_DATA_DIR", fx->data_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "AXIS_LISTEN_HOST", "127.0.0.1", TRUE);
	g_subprocess_launcher_setenv(launcher, "AXIS_LISTEN_PORT", port, TRUE);
	g_subprocess_launcher_setenv(launcher, "AXIS_DATABASE_URL", url, TRUE);
	/* The shortest head deadline, so that the case below is quick. */
	g_subprocess_launcher_setenv(launcher, "AXIS_HEAD_TIMEOUT", "1", TRUE);
	g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
	fx->proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
}

static void
daemon_teardown(Daemon *fx, gconstpointer data)
{
	if (fx->proc != NULL) {
		g_subprocess_force_exit(fx->proc);
		g_subprocess_wait(fx->proc, NULL, NULL);
		g_object_unref(fx->proc);
	}
	g_free(fx->data_dir);
	if (fx->scratch != NULL)
		axis_test_storage_dir_free(fx->scratch);
}

/*
 * GET @path from the daemon: its status (0 when it could not be asked, or
 * when what answered is not the daemon this test started: its probe answers
 * name their process to a loopback client).
 */
static guint
daemon_get(
	Daemon      *fx,
	SoupSession *session,
	const gchar *path
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)fx->port, path);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autoptr(GBytes) reply = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *process;

	reply = soup_session_send_and_read(session, message, NULL, &error);
	if (error != NULL)
		return 0;
	process = soup_message_headers_get_one(soup_message_get_response_headers(message), AXIS_WEB_HEALTH_PROCESS_HEADER);
	if (g_strcmp0(process, g_subprocess_get_identifier(fx->proc)) != 0) {
		g_test_message("%s was answered by process %s, not the daemon this test started (%s)", path,
			process != NULL ? process : "(unknown)", g_subprocess_get_identifier(fx->proc));
		return 0;
	}
	return soup_message_get_status(message);
}

/* Sends @head and @bytes of body on a new connection, and reads what comes
 * back until the daemon ends the connection; how long that took. */
static gchar *
daemon_raw(
	Daemon      *fx,
	const gchar *head,
	gsize        bytes,
	gint64      *took_us
){
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *where = g_strdup_printf("127.0.0.1:%u", (guint)fx->port);
	g_autofree gchar *body = g_strnfill(bytes, 'a');
	GSocket *socket;
	GString *got = g_string_new(NULL);
	gint64 start = g_get_monotonic_time();
	gchar buffer[4096];
	gssize n;

	connection = g_socket_client_connect_to_host(client, where, 0, NULL, &error);
	g_assert_no_error(error);
	socket = g_socket_connection_get_socket(connection);
	g_socket_set_timeout(socket, 10);
	g_assert_cmpint(g_socket_send(socket, head, strlen(head), NULL, NULL), ==, (gssize)strlen(head));
	/* What the daemon takes of the body before it answers; the rest it
	 * may refuse. */
	g_socket_send(socket, body, bytes, NULL, NULL);
	while ((n = g_socket_receive(socket, buffer, sizeof(buffer), NULL, NULL)) > 0)
		g_string_append_len(got, buffer, n);
	*took_us = g_get_monotonic_time() - start;
	return g_string_free(got, FALSE);
}

/*
 * Found by the stage 2 hunt of the web layer (W-1): the body limit was
 * connected only once the database was ready, so while the daemon waited
 * for it (or stood by for its lock) a body of any size was read in full
 * before the 503.  Now no body is taken before the daemon serves: it is
 * answered what the start-up gate says, at once, without reading it (the
 * review of the fixes asked for that); and the deadlines hold from the
 * first connection.
 */
static void
test_daemon_limits_while_starting(Daemon *fx, gconstpointer data)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 60, NULL);
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) slow = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *where = g_strdup_printf("127.0.0.1:%u", (guint)fx->port);
	g_autofree gchar *large = NULL;
	g_autofree gchar *chunked = NULL;
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	gint64 took = 0;
	guint status = 0;
	gchar byte;

	while (status != 200 && g_get_monotonic_time() < deadline) {
		status = daemon_get(fx, session, "/livez");
		if (status != 200)
			g_usleep(100 * 1000);
	}
	g_assert_cmpuint(status, ==, 200);
	/* Still waiting for its database. */
	g_assert_cmpuint(daemon_get(fx, session, "/readyz"), ==, 503);

	/* 96 MiB promised, 64 KiB sent: answered at once, the rest not read
	 * (the body deadline is a minute). */
	large = daemon_raw(fx, "POST /login HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
		"Content-Length: 100663296\r\n\r\n", 64 * 1024, &took);
	g_test_message("a body of 96 MiB promised was answered in %" G_GINT64_FORMAT " ms: %.60s", took / 1000, large);
	g_assert_true(g_str_has_prefix(large, "HTTP/1.1 503"));
	g_assert_nonnull(strstr(large, "Retry-After: 5"));
	g_assert_nonnull(strstr(large, "Axis is starting: it is waiting for its database."));
	g_assert_cmpint(took, <, 5 * G_USEC_PER_SEC);
	chunked = daemon_raw(fx, "POST /login HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n4\r\naaaa\r\n",
		0, &took);
	g_assert_true(g_str_has_prefix(chunked, "HTTP/1.1 503"));

	/* And a head that never ends is cut off (AXIS_HEAD_TIMEOUT=1). */
	slow = g_socket_client_connect_to_host(client, where, 0, NULL, &error);
	g_assert_no_error(error);
	g_socket_send(g_socket_connection_get_socket(slow), "GET /livez HTTP/1.1\r\nHost: x\r\n",
		strlen("GET /livez HTTP/1.1\r\nHost: x\r\n"), NULL, &error);
	g_assert_no_error(error);
	g_socket_set_timeout(g_socket_connection_get_socket(slow), 10);
	g_assert_cmpint(g_socket_receive(g_socket_connection_get_socket(slow), &byte, 1, NULL, &error), <=, 0);
	if (error != NULL)
		g_assert_false(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT));
}

#define ADD(name, flags, func) \
	g_test_add("/web-limits/" name, Fixture, GUINT_TO_POINTER(flags), fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-limits/defaults", test_defaults);
	ADD("head/slow-head-closed", 0, test_slow_head_closed);
	ADD("head/in-time-answered", 0, test_head_in_time_answered);
	ADD("body/slow-body-closed", 0, test_slow_body_closed);
	ADD("idle/keep-alive-closed", 0, test_idle_keep_alive_closed);
	ADD("stream/outlives-deadlines", 0, test_event_stream_outlives_deadlines);
	ADD("cap/per-address", TWO_PER_ADDRESS | LOCAL_PUBLIC, test_per_address_cap);
	ADD("cap/local-not-per-address", TWO_PER_ADDRESS, test_local_not_capped_per_address);
	ADD("cap/total", TWO_IN_ALL | ONE_OVERFLOW, test_total_cap);
	ADD("cap/proxy-clients", TWO_PER_ADDRESS | PROXY, test_proxy_clients_capped_not_proxy);
	ADD("cap/proxy-streams-and-workers", TWO_PER_ADDRESS | PROXY, test_proxy_streams_and_workers_not_counted);
	ADD("cap/worker-exempt", TWO_PER_ADDRESS | LOCAL_PUBLIC, test_worker_exempt);
	ADD("body/refused-before-serving", NO_BODIES | LONG_BODY, test_bodies_refused);
	g_test_add("/web-limits/daemon/limits-while-starting", Daemon, NULL,
		daemon_setup, test_daemon_limits_while_starting, daemon_teardown);

	return g_test_run();
}
