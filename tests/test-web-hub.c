/*
 * test-web-hub.c - server-sent events: who may open a stream, delivery to
 * the right subscribers, the limits, keep-alive, and how the pages open a
 * stream
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test runs the real shell (with an auth that has no database and
 * test sessions) and a real hub on an ephemeral loopback port.  Streams
 * are read with a plain socket client that pumps the test's main loop.
 * Times are shortened through the hub's options, and every wait is for a
 * condition, bounded.  No database is needed.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "web/axis-password.h"
#include "web/axis-web.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-hub-topic.h"

enum
{
	NO_HUB      = 1 << 0,	/* the hub is made but not installed */
	SHORT_KEEP  = 1 << 1,	/* keep-alive every 100 ms */
	SHORT_STALL = 1 << 2,	/* a client that stops reading is dropped after 300 ms */
	SMALL_QUEUE = 1 << 3,	/* at most 128 KiB queued for a client */
	TWO_PER_SESSION = 1 << 4,
	TWO_IN_ALL  = 1 << 5,
	THREE_PER_USER = 1 << 6,
	FOUR_TWO_KEPT = 1 << 7	/* four in all, two of them for administrators */
};

typedef struct
{
	HtmxServer         *server;
	AxisModuleRegistry *registry;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisWebHub         *hub;
	SoupSession        *session;
	guint16             port;
	gchar              *alice;	/* "axis_session=..." */
	gchar              *bob;
	gchar              *alice_second;	/* a second session of alice's */
} Fixture;

/* ── A module with pages that follow topics ─────────────────────── */

static HtmxResponse *
page_live(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	axis_web_page_add_topic(request, "news");
	htmx_node_set_text_content(HTMX_NODE(text), "Live page.");
	return axis_web_page_respond(request, "Live", HTMX_NODE(text));
}

static HtmxResponse *
page_plain(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "Plain page.");
	return axis_web_page_respond(request, "Plain", HTMX_NODE(text));
}

static HtmxNode *
header_widget(HtmxRequest *request, gpointer user_data)
{
	HtmxSpan *span = htmx_span_new();

	axis_web_page_add_topic(request, "widget");
	htmx_element_set_id(HTMX_ELEMENT(span), "widget");
	htmx_node_set_text_content(HTMX_NODE(span), "Widget");
	return HTMX_NODE(span);
}

static void
pages_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_get(web, "/live", page_live, NULL);
	axis_web_get(web, "/plain", page_plain, NULL);
	axis_web_set_header_widget(web, header_widget, NULL);
}

/* ── Fixture ────────────────────────────────────────────────────── */

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);
	AxisWebHubOptions options;
	AxisModuleInfo info;
	GError *error = NULL;

	memset(fx, 0, sizeof(*fx));
	memset(&options, 0, sizeof(options));
	axis_test_log_init();
	axis_test_log_reset();
	if (flags & SHORT_KEEP)
		options.keepalive_ms = 100;
	if (flags & SHORT_STALL) {
		options.stall_ms = 300;
		options.max_pending_bytes = 64 * 1024 * 1024;
	}
	if (flags & SMALL_QUEUE)
		options.max_pending_bytes = 128 * 1024;
	if (flags & TWO_PER_SESSION)
		options.max_per_session = 2;
	if (flags & TWO_IN_ALL)
		options.max_streams = 2;
	if (flags & THREE_PER_USER)
		options.max_per_user = 3;
	if (flags & FOUR_TWO_KEPT) {
		options.max_streams = 4;
		options.admin_reserved = 2;
	}

	fx->registry = axis_module_registry_new();
	memset(&info, 0, sizeof(info));
	info.id = "pages";
	info.name = "Pages";
	info.register_routes = pages_routes;
	g_assert_true(axis_module_registry_register(fx->registry, &info, NULL, &error));
	g_assert_no_error(error);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	fx->hub = axis_web_hub_new(&options);
	if (!(flags & NO_HUB))
		axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);

	fx->alice = axis_web_auth_open_test_session(fx->auth, "alice", AXIS_USER_ROLE_USER);
	fx->alice_second = axis_web_auth_open_test_session(fx->auth, "alice", AXIS_USER_ROLE_USER);
	fx->bob = axis_web_auth_open_test_session(fx->auth, "bob", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	g_free(fx->alice);
	g_free(fx->alice_second);
	g_free(fx->bob);
	axis_test_log_reset();
}

static AxisTestReply
get(Fixture *fx, const gchar *path, const gchar *cookie)
{
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

static AxisTestStream *
open_stream(Fixture *fx, const gchar *query, const gchar *cookie)
{
	g_autofree gchar *path = g_strdup_printf("/events?%s", query);
	AxisTestStream *stream = axis_test_stream_open(fx->port, path, cookie, 0);

	g_assert_cmpuint(axis_test_stream_get_status(stream), ==, 200);
	g_assert_true(axis_test_stream_wait_for(stream, ": open", AXIS_TEST_WAIT_MS));

	return stream;
}

static gboolean
streams_are(gpointer data)
{
	gpointer *pair = data;

	return axis_web_hub_get_n_streams(pair[0]) == GPOINTER_TO_UINT(pair[1]);
}

/* Pumps until the hub has exactly @n streams. */
static void
wait_for_streams(Fixture *fx, guint n)
{
	gpointer pair[2] = { fx->hub, GUINT_TO_POINTER(n) };

	if (!axis_test_pump_until(streams_are, pair, AXIS_TEST_WAIT_MS))
		g_error("expected %u streams, the hub has %u", n, axis_web_hub_get_n_streams(fx->hub));
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

/* ── Who may open a stream ──────────────────────────────────────── */

static void
test_unauthenticated_refused(Fixture *fx, gconstpointer data)
{
	static const gchar *const accept[] = { "Accept", "text/event-stream", NULL };
	AxisTestReply reply = get(fx, "/events?topic=news", NULL);
	AxisTestReply with_accept = axis_test_web_request(fx->session, fx->port, "GET",
		"/events?topic=news", NULL, accept, NULL);

	/* A plain 401, not the redirect to the login page every other page
	 * gets: an EventSource could not make sense of that. */
	g_assert_cmpuint(reply.status, ==, 401);
	g_assert_null(axis_test_reply_header(&reply, "Location"));
	g_assert_cmpuint(with_accept.status, ==, 401);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 0);
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 0);

	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&with_accept);
}

static void
test_admin_topic_refused_to_others(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_ADMIN);
	AxisTestReply reply = get(fx, "/events?topic=news:admin", fx->alice);
	AxisTestReply mixed = get(fx, "/events?topic=news&topic=job:7:admin", fx->alice);

	g_assert_cmpuint(reply.status, ==, 403);
	g_assert_cmpuint(mixed.status, ==, 403);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 0);
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 0);

	{
		g_autoptr(AxisTestStream) stream = open_stream(fx, "topic=news:admin", carol);

		g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news:admin"), ==, 1);
	}

	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&mixed);
}

static void
test_stream_headers(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) stream = open_stream(fx, "topic=news", fx->alice);
	g_autofree gchar *type = axis_test_stream_get_header(stream, "Content-Type");
	g_autofree gchar *csp = axis_test_stream_get_header(stream, "Content-Security-Policy");
	g_autofree gchar *nosniff = axis_test_stream_get_header(stream, "X-Content-Type-Options");
	g_autofree gchar *frames = axis_test_stream_get_header(stream, "X-Frame-Options");
	g_autofree gchar *referrer = axis_test_stream_get_header(stream, "Referrer-Policy");
	g_autofree gchar *cache = axis_test_stream_get_header(stream, "Cache-Control");

	/* The shell's security headers reach a streaming response too. */
	g_assert_cmpstr(type, ==, "text/event-stream");
	g_assert_nonnull(csp);
	g_assert_true(g_str_has_prefix(csp, "default-src 'self'"));
	assert_lacks(csp, "unsafe-inline");
	g_assert_cmpstr(nosniff, ==, "nosniff");
	g_assert_cmpstr(frames, ==, "DENY");
	g_assert_cmpstr(referrer, ==, "same-origin");
	g_assert_cmpstr(cache, ==, "no-store");
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 1);
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 1);
}

static void
test_bad_topics_refused(Fixture *fx, gconstpointer data)
{
	static const gchar *const bad[] = {
		"", "x=1", "topic=", "topic=NEWS", "topic=-news", "topic=a%0d%0aevent", "topic=a%00b",
		"topic=a%zz", "topic=a%", "topic=a b",
		"topic=1&topic=2&topic=3&topic=4&topic=5&topic=6&topic=7&topic=8&topic=9",
		NULL
	};
	AxisTestReply post;
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/events?%s", bad[i]);
		AxisTestReply reply = get(fx, path, fx->alice);

		if (reply.status != 400)
			g_error("'%s' answered %u", bad[i], reply.status);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 0);

	/* A stream is read, not posted to. */
	post = axis_test_web_request(fx->session, fx->port, "POST", "/events?topic=news", fx->alice, NULL, NULL);
	g_assert_cmpuint(post.status, ==, 405);
	axis_test_reply_clear(&post);
}

static void
test_logout_closes_streams(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) mine = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) mine_too = open_stream(fx, "topic=news&topic=other", fx->alice);
	g_autoptr(AxisTestStream) other_session = open_stream(fx, "topic=news", fx->alice_second);
	g_autoptr(AxisTestStream) other_user = open_stream(fx, "topic=news", fx->bob);
	AxisTestReply page = get(fx, "/plain", fx->alice);
	g_autofree gchar *token = axis_test_web_csrf_token(page.body);
	const gchar *const extra[] = { "X-CSRF-Token", token, NULL };
	AxisTestReply logout;

	g_assert_nonnull(token);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 4);

	logout = axis_test_web_request(fx->session, fx->port, "POST", "/logout", fx->alice, extra, NULL);
	g_assert_cmpuint(logout.status, ==, 303);

	/* That session's streams are closed by the daemon; nobody else's. */
	g_assert_true(axis_test_stream_wait_closed(mine, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_closed(mine_too, AXIS_TEST_WAIT_MS));
	wait_for_streams(fx, 2);
	axis_test_pump_for(100);
	g_assert_false(axis_test_stream_is_closed(other_session));
	g_assert_false(axis_test_stream_is_closed(other_user));
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 2);
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "other"), ==, 0);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&logout);
}

static void
test_revocation_and_disabling_close_streams(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) revoked = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) disabled = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) kept = open_stream(fx, "topic=news", fx->alice_second);
	const gchar *token = strchr(fx->alice, '=') + 1;
	g_autofree gchar *hash = axis_token_hash(token);
	const AxisUser *bob = axis_session_store_find_user(fx->auth->sessions, "bob");
	g_autoptr(AxisUser) changed = axis_user_copy(bob);

	/* Revoked, as the account page does it. */
	g_assert_true(axis_session_store_revoke(fx->auth->sessions, hash));
	g_assert_true(axis_test_stream_wait_closed(revoked, AXIS_TEST_WAIT_MS));

	/* The account is disabled. */
	changed->disabled = TRUE;
	axis_session_store_put_user(fx->auth->sessions, changed);
	g_assert_true(axis_test_stream_wait_closed(disabled, AXIS_TEST_WAIT_MS));

	wait_for_streams(fx, 1);
	g_assert_false(axis_test_stream_is_closed(kept));
}

/* Gives the account @name @role, as /admin/users/:id/role does. */
static void
set_role(
	Fixture      *fx,
	const gchar  *name,
	AxisUserRole  role
){
	g_autoptr(AxisUser) changed = axis_user_copy(axis_session_store_find_user(fx->auth->sessions, name));

	changed->role = role;
	axis_session_store_put_user(fx->auth->sessions, changed);
}

/*
 * Found by the stage 2 hunt of the web layer (W-4): an administrator made
 * an ordinary user kept the streams opened as one, ":admin" topics and
 * all, because the role was checked only when a stream opened.  A role
 * change now closes every stream of the account; the browser opens them
 * again, and that request is judged by the role the account has now.
 */
static void
test_role_change_closes_streams(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_ADMIN);
	g_autoptr(AxisTestStream) admin_topic = open_stream(fx, "topic=jobs:admin", carol);
	g_autoptr(AxisTestStream) plain_topic = open_stream(fx, "topic=news", carol);
	g_autoptr(AxisTestStream) promoted = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) unaffected = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) again = NULL;
	AxisTestReply refused;

	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "jobs:admin"), ==, 1);

	/* Made an ordinary user: both of carol's streams end ... */
	set_role(fx, "carol", AXIS_USER_ROLE_USER);
	g_assert_true(axis_test_stream_wait_closed(admin_topic, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_closed(plain_topic, AXIS_TEST_WAIT_MS));
	wait_for_streams(fx, 2);
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "jobs:admin", "oob", "<p id=\"for-admins\">x</p>"), ==, 0);

	/* ... and opened again, the session is an ordinary user's. */
	refused = get(fx, "/events?topic=jobs:admin", carol);
	g_assert_cmpuint(refused.status, ==, 403);
	axis_test_reply_clear(&refused);
	again = open_stream(fx, "topic=news", carol);

	/* Made an administrator: alice's streams end too, to be opened with
	 * what the new role allows; bob's are not touched. */
	set_role(fx, "alice", AXIS_USER_ROLE_ADMIN);
	g_assert_true(axis_test_stream_wait_closed(promoted, AXIS_TEST_WAIT_MS));
	wait_for_streams(fx, 2);
	g_assert_false(axis_test_stream_is_closed(unaffected));
	g_assert_false(axis_test_stream_is_closed(again));
	{
		g_autoptr(AxisTestStream) now_admin = open_stream(fx, "topic=jobs:admin", fx->alice);

		g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "jobs:admin"), ==, 1);
	}

	/* The same role again is no change. */
	set_role(fx, "bob", AXIS_USER_ROLE_USER);
	axis_test_pump_for(100);
	g_assert_false(axis_test_stream_is_closed(unaffected));
}

/* ── Delivery ───────────────────────────────────────────────────── */

static void
test_delivery_is_per_topic(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) first = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) second = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) elsewhere = open_stream(fx, "topic=sports", fx->alice);
	g_autoptr(AxisTestStream) both = open_stream(fx, "topic=sports&topic=news", fx->alice_second);

	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 3);
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "<p id=\"a\">hello</p>"), ==, 3);

	g_assert_true(axis_test_stream_wait_for(first, "event: oob\ndata: <p id=\"a\">hello</p>\n\n", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(second, "event: oob\ndata: <p id=\"a\">hello</p>\n\n", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(both, "event: oob\ndata: <p id=\"a\">hello</p>\n\n", AXIS_TEST_WAIT_MS));

	/* The one subscribed elsewhere hears nothing of it. */
	axis_test_pump_for(200);
	assert_lacks(axis_test_stream_get_text(elsewhere), "hello");

	/* And a topic nobody follows reaches nobody. */
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "nobody", "oob", "x"), ==, 0);

	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "sports", "oob", "<p>goal</p>"), ==, 2);
	g_assert_true(axis_test_stream_wait_for(elsewhere, "data: <p>goal</p>", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(both, "data: <p>goal</p>", AXIS_TEST_WAIT_MS));
	axis_test_pump_for(100);
	assert_lacks(axis_test_stream_get_text(first), "goal");
	assert_lacks(axis_test_stream_get_text(second), "goal");
}

static void
test_closed_stream_is_forgotten(Fixture *fx, gconstpointer data)
{
	AxisTestStream *gone = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) stays = open_stream(fx, "topic=news", fx->alice);

	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "one"), ==, 2);

	/* A closed tab leaves nothing behind. */
	axis_test_stream_free(gone);
	wait_for_streams(fx, 1);
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 1);
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "two"), ==, 1);
	g_assert_true(axis_test_stream_wait_for(stays, "data: two", AXIS_TEST_WAIT_MS));
}

static void
test_event_text_cannot_forge_fields(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) stream = open_stream(fx, "topic=news", fx->alice);

	/* Escaped for HTML, a carriage return still ends a line in an event
	 * stream: it is sent as a line feed so that it cannot start one. */
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "x\revent: evil\rdata: y\r\nz"), ==, 1);
	g_assert_true(axis_test_stream_wait_for(stream, "data: z\n\n", AXIS_TEST_WAIT_MS));
	assert_contains(axis_test_stream_get_text(stream),
		"event: oob\ndata: x\ndata: event: evil\ndata: data: y\ndata: \ndata: z\n\n");
	assert_lacks(axis_test_stream_get_text(stream), "\nevent: evil");
	assert_lacks(axis_test_stream_get_text(stream), "x\r");
}

static void
test_oversized_fragment_is_not_sent(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) stream = open_stream(fx, "topic=news", fx->alice);
	g_autofree gchar *big = g_strnfill(AXIS_WEB_HUB_MAX_EVENT_BYTES + 1, 'x');

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*not publishing a fragment*");
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", big), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*not publishing a fragment*"), ==, 1);
	g_assert_cmpuint(axis_web_hub_get_pending_bytes(fx->hub), <, 100);
}

/* ── Limits ─────────────────────────────────────────────────────── */

static void
test_per_session_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) oldest = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) middle = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) other = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) newest = NULL;

	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 3);

	/* A third stream for the same session ends its oldest, not another
	 * session's, and is served. */
	newest = open_stream(fx, "topic=news", fx->alice);
	g_assert_true(axis_test_stream_wait_closed(oldest, AXIS_TEST_WAIT_MS));
	wait_for_streams(fx, 3);
	g_assert_false(axis_test_stream_is_closed(middle));
	g_assert_false(axis_test_stream_is_closed(other));

	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "still here"), ==, 3);
	g_assert_true(axis_test_stream_wait_for(newest, "data: still here", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(middle, "data: still here", AXIS_TEST_WAIT_MS));
}

static void
test_total_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) first = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) second = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) refused = axis_test_stream_open(fx->port, "/events?topic=news",
		fx->alice_second, 0);
	g_autofree gchar *retry = NULL;
	g_autoptr(AxisTestStream) later = NULL;

	g_assert_cmpuint(axis_test_stream_get_status(refused), ==, 503);
	retry = axis_test_stream_get_header(refused, "Retry-After");
	g_assert_nonnull(retry);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 2);

	/* Room is made when one ends. */
	axis_test_stream_free(g_steal_pointer(&first));
	wait_for_streams(fx, 1);
	later = open_stream(fx, "topic=news", fx->alice_second);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 2);
}

/*
 * Found by the stage 2 hunt of the web layer (W-5): one account with its
 * 64 sessions of 8 streams each could hold more than the whole total.  An
 * account has a cap over all its sessions.  At the cap a new stream is
 * refused, and none of the account's is ended (found by the review of the
 * fixes: ending the oldest made tabs end each other's streams without end):
 * the refused tab is told that its live updates are off, and to try again
 * in a minute.
 */
static void
test_per_account_cap(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) oldest = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) second = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) third = open_stream(fx, "topic=news", fx->alice_second);
	g_autoptr(AxisTestStream) other = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) refused = NULL;
	g_autoptr(AxisTestStream) later = NULL;
	g_autofree gchar *retry = g_strdup_printf("retry: %d", AXIS_WEB_HUB_REFUSED_RETRY_MS);

	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 4);

	/* A fourth for the account, from its other session: answered as a
	 * stream that says live updates are off and ends at once. */
	refused = axis_test_stream_open(fx->port, "/events?topic=news", fx->alice_second, 0);
	g_assert_cmpuint(axis_test_stream_get_status(refused), ==, 200);
	g_assert_true(axis_test_stream_wait_for(refused, "Live updates are off in this tab", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_stream_wait_for(refused, retry, AXIS_TEST_WAIT_MS));
	assert_contains(axis_test_stream_get_text(refused), "hx-swap-oob=\"true\"");
	g_assert_true(axis_test_stream_wait_closed(refused, AXIS_TEST_WAIT_MS));
	axis_test_pump_for(100);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 4);
	g_assert_false(axis_test_stream_is_closed(oldest));
	g_assert_false(axis_test_stream_is_closed(second));
	g_assert_false(axis_test_stream_is_closed(third));
	g_assert_false(axis_test_stream_is_closed(other));
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "after"), ==, 4);

	/* Once one of the account's ends, the next is served, and its first
	 * event hides the line again. */
	axis_test_stream_free(g_steal_pointer(&oldest));
	wait_for_streams(fx, 3);
	later = open_stream(fx, "topic=news", fx->alice_second);
	assert_contains(axis_test_stream_get_text(later), "id=\"" AXIS_WEB_HUB_STATUS_ID "\" hidden=\"hidden\"");
}

/* A page that opens a stream carries the line a refusal fills in. */
static void
test_page_has_status_line(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get(fx, "/live", fx->alice);

	assert_contains(page.body, "<p id=\"" AXIS_WEB_HUB_STATUS_ID "\" hidden=\"hidden\"></p>");
	axis_test_reply_clear(&page);
}

/* The last part of the total is for administrators: ordinary accounts
 * cannot take every stream. */
static void
test_admin_reserve(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *carol = axis_web_auth_open_test_session(fx->auth, "carol", AXIS_USER_ROLE_ADMIN);
	g_autoptr(AxisTestStream) first = open_stream(fx, "topic=news", fx->alice);
	g_autoptr(AxisTestStream) second = open_stream(fx, "topic=news", fx->bob);
	g_autoptr(AxisTestStream) refused = axis_test_stream_open(fx->port, "/events?topic=news", fx->alice_second, 0);
	g_autoptr(AxisTestStream) admin_one = NULL;
	g_autoptr(AxisTestStream) admin_two = NULL;
	g_autoptr(AxisTestStream) full = NULL;

	/* Two of the four are all the others may have ... */
	g_assert_cmpuint(axis_test_stream_get_status(refused), ==, 503);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 2);

	/* ... and an administrator still gets the rest. */
	admin_one = open_stream(fx, "topic=jobs:admin", carol);
	admin_two = open_stream(fx, "topic=news", carol);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 4);
	full = axis_test_stream_open(fx->port, "/events?topic=news", carol, 0);
	g_assert_cmpuint(axis_test_stream_get_status(full), ==, 503);
}

/* The defaults: no one account, nor a handful of them, can fill what the
 * others may use, and administrators keep a part. */
static void
test_default_caps(void)
{
	guint others = AXIS_WEB_HUB_MAX_STREAMS - AXIS_WEB_HUB_MAX_STREAMS / AXIS_WEB_HUB_ADMIN_RESERVED_PART;

	g_assert_cmpuint(AXIS_WEB_HUB_MAX_STREAMS_PER_USER, >=, AXIS_WEB_HUB_MAX_STREAMS_PER_SESSION);
	g_assert_cmpuint(5 * AXIS_WEB_HUB_MAX_STREAMS_PER_USER, <, others);
	g_assert_cmpuint(others, <, AXIS_WEB_HUB_MAX_STREAMS);
}

/*
 * Publishes @size-byte fragments to "news" until @done says stop or ten
 * seconds pass, a few milliseconds apart, and checks after each that the
 * hub holds no more than @limit bytes for clients.
 */
static gboolean
stream_gone(gpointer data)
{
	return axis_web_hub_get_n_streams(data) == 0;
}

static void
flood_until_dropped(
	Fixture *fx,
	gsize    size,
	gsize    limit,
	gsize   *peak,
	gsize   *published
){
	g_autofree gchar *html = g_strnfill(size, 'x');
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

	*peak = 0;
	*published = 0;
	while (axis_web_hub_get_n_streams(fx->hub) > 0 && g_get_monotonic_time() < deadline) {
		gsize pending;

		if (axis_web_hub_publish(fx->hub, "news", "oob", html) > 0)
			*published += size;
		pending = axis_web_hub_get_pending_bytes(fx->hub);
		*peak = MAX(*peak, pending);
		g_assert_cmpuint(pending, <=, limit);
		axis_test_pump_for(5);
	}

	g_assert_true(axis_test_pump_until(stream_gone, fx->hub, AXIS_TEST_WAIT_MS));
}

static void
test_stalled_client_is_dropped(Fixture *fx, gconstpointer data)
{
	AxisTestStream *stuck = axis_test_stream_open(fx->port, "/events?topic=news", fx->alice, 2048);
	gsize peak;
	gsize published;

	g_assert_cmpuint(axis_test_stream_get_status(stuck), ==, 200);
	g_assert_true(axis_test_stream_wait_for(stuck, ": open", AXIS_TEST_WAIT_MS));

	/* From here on it reads nothing, while events keep coming.  Its
	 * kernel buffers fill, the rest queues in the daemon, and after 300
	 * ms without a byte read the stream is closed. */
	axis_test_stream_set_paused(stuck, TRUE);
	flood_until_dropped(fx, 16 * 1024, 64 * 1024 * 1024, &peak, &published);

	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 0);
	g_assert_cmpuint(axis_web_hub_count_subscribers(fx->hub, "news"), ==, 0);
	g_assert_cmpuint(axis_web_hub_get_pending_bytes(fx->hub), ==, 0);
	g_assert_cmpuint(peak, >, 0);

	/* The connection is really closed, and what had queued in the daemon
	 * was discarded, not delivered late: the client gets the rest of its
	 * kernel's buffer and then the end. */
	axis_test_stream_set_paused(stuck, FALSE);
	g_assert_true(axis_test_stream_wait_closed(stuck, AXIS_TEST_WAIT_MS));
	g_assert_cmpuint(strlen(axis_test_stream_get_text(stuck)), <, published);
	axis_test_stream_free(stuck);
}

static void
test_queue_is_bounded(Fixture *fx, gconstpointer data)
{
	AxisTestStream *stuck = axis_test_stream_open(fx->port, "/events?topic=news", fx->alice, 2048);
	g_autoptr(AxisTestStream) healthy = NULL;
	gsize peak;

	g_assert_cmpuint(axis_test_stream_get_status(stuck), ==, 200);
	g_assert_true(axis_test_stream_wait_for(stuck, ": open", AXIS_TEST_WAIT_MS));
	healthy = open_stream(fx, "topic=news", fx->bob);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 2);

	/* No stall time needed: more than 128 KiB unread ends the stream, so
	 * whatever the client does the memory held for it stays bounded. */
	axis_test_stream_set_paused(stuck, TRUE);
	{
		g_autofree gchar *html = g_strnfill(16 * 1024, 'x');
		gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

		peak = 0;
		while (axis_web_hub_get_n_streams(fx->hub) > 1 && g_get_monotonic_time() < deadline) {
			axis_web_hub_publish(fx->hub, "news", "oob", html);
			peak = MAX(peak, axis_web_hub_get_pending_bytes(fx->hub));
			g_assert_cmpuint(axis_web_hub_get_pending_bytes(fx->hub), <=, 128 * 1024 * 2);
			axis_test_pump_for(5);
		}
	}
	wait_for_streams(fx, 1);

	/* The other client, which reads, is not affected. */
	g_assert_cmpuint(axis_web_hub_publish(fx->hub, "news", "oob", "after"), ==, 1);
	g_assert_true(axis_test_stream_wait_for(healthy, "data: after", AXIS_TEST_WAIT_MS));
	axis_test_stream_free(stuck);
}

static void
test_keep_alive(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisTestStream) stream = open_stream(fx, "topic=news", fx->alice);

	/* Idle for longer than the interval: a comment line, again and again. */
	g_assert_true(axis_test_stream_wait_for(stream, ": keep-alive\n\n", AXIS_TEST_WAIT_MS));
	axis_test_pump_for(450);
	g_assert_cmpuint(axis_test_stream_count(stream, ": keep-alive\n\n"), >=, 3);
	g_assert_cmpuint(axis_web_hub_get_n_streams(fx->hub), ==, 1);

	/* A stream that is busy is not sent comments it does not need. */
	g_assert_cmpuint(axis_web_hub_get_pending_bytes(fx->hub), <, 1024);
}

/* ── Pages ──────────────────────────────────────────────────────── */

static void
test_page_opens_one_stream(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get(fx, "/live", fx->alice);
	static const gchar *const htmx_headers[] = { "HX-Request", "true", NULL };
	AxisTestReply fragment = axis_test_web_request(fx->session, fx->port, "GET", "/live", fx->alice,
		htmx_headers, NULL);
	AxisTestReply anonymous = get(fx, "/login", NULL);

	g_assert_cmpuint(page.status, ==, 200);
	/* One stream for every topic the page and its header asked for; the
	 * extension and the sink that takes the events are attributes only. */
	assert_contains(page.body, "<body ");
	assert_contains(page.body, "hx-ext=\"sse\"");
	assert_contains(page.body, "sse-connect=\"/events?topic=news&amp;topic=widget\"");
	assert_contains(page.body, "sse-swap=\"oob\"");
	assert_contains(page.body, "id=\"widget\"");
	assert_contains(page.body, "src=\"/static/");
	assert_contains(page.body, "htmx-ext-sse.js");
	assert_lacks(page.body, "<script>");

	/* A fragment is only the content: no body to carry a stream. */
	g_assert_cmpuint(fragment.status, ==, 200);
	assert_lacks(fragment.body, "sse-connect");

	/* Someone not logged in is shown no stream, nor a widget. */
	assert_lacks(anonymous.body, "sse-connect");
	assert_lacks(anonymous.body, "id=\"widget\"");

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&fragment);
	axis_test_reply_clear(&anonymous);
}

static void
test_page_follows_only_what_it_asked(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get(fx, "/plain", fx->alice);

	/* The header widget follows a topic of its own, so every page of a
	 * logged-in user has a stream; what a page does not ask for it does
	 * not get. */
	assert_contains(page.body, "sse-connect=\"/events?topic=widget\"");
	assert_lacks(page.body, "topic=news");
	axis_test_reply_clear(&page);
}

static void
test_pages_work_without_a_hub(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get(fx, "/live", fx->alice);
	AxisTestReply events = get(fx, "/events?topic=news", fx->alice);

	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Live page.");
	assert_contains(page.body, "id=\"widget\"");
	assert_lacks(page.body, "sse-connect");
	assert_lacks(page.body, "sse-swap");

	/* Nothing serves the stream either. */
	g_assert_cmpuint(events.status, ==, 404);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&events);
}

/* ── Topic names ────────────────────────────────────────────────── */

static void
test_topic_names(void)
{
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) topics = NULL;

	g_assert_true(axis_web_hub_topic_is_valid("jobs"));
	g_assert_true(axis_web_hub_topic_is_valid("job:12:admin"));
	g_assert_true(axis_web_hub_topic_is_valid("a.b_c-d"));
	g_assert_false(axis_web_hub_topic_is_valid(""));
	g_assert_false(axis_web_hub_topic_is_valid(NULL));
	g_assert_false(axis_web_hub_topic_is_valid("Jobs"));
	g_assert_false(axis_web_hub_topic_is_valid(":jobs"));
	g_assert_false(axis_web_hub_topic_is_valid("a b"));
	g_assert_false(axis_web_hub_topic_is_valid("a\nb"));
	g_assert_true(axis_web_hub_topic_is_valid("a123456789012345678901234567890123456789012345678901234567890123"));
	g_assert_false(axis_web_hub_topic_is_valid("a1234567890123456789012345678901234567890123456789012345678901234"));

	g_assert_true(axis_web_hub_event_name_is_valid("oob"));
	g_assert_true(axis_web_hub_event_name_is_valid("jobs-changed"));
	g_assert_false(axis_web_hub_event_name_is_valid(""));
	g_assert_false(axis_web_hub_event_name_is_valid("1x"));
	g_assert_false(axis_web_hub_event_name_is_valid("a:b"));
	g_assert_false(axis_web_hub_event_name_is_valid("a\nevent"));

	topics = axis_web_hub_parse_topics("x=1&topic=jobs%3Aadmin&topic=jobs&topic=jobs%3aadmin", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_strv_length(topics), ==, 2);
	g_assert_cmpstr(topics[0], ==, "jobs:admin");
	g_assert_cmpstr(topics[1], ==, "jobs");

	g_assert_null(axis_web_hub_parse_topics("topic=a%00", &error));
	g_assert_error(error, AXIS_WEB_HUB_TOPIC_ERROR, AXIS_WEB_HUB_TOPIC_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_null(axis_web_hub_parse_topics(NULL, &error));
	g_assert_error(error, AXIS_WEB_HUB_TOPIC_ERROR, AXIS_WEB_HUB_TOPIC_ERROR_NONE);
	g_clear_error(&error);
}

#define ADD(name, flags, func) \
	g_test_add("/web-hub/" name, Fixture, GUINT_TO_POINTER(flags), fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web-hub/topic-names", test_topic_names);

	ADD("stream/unauthenticated-refused", 0, test_unauthenticated_refused);
	ADD("stream/admin-topic-refused-to-others", 0, test_admin_topic_refused_to_others);
	ADD("stream/headers", 0, test_stream_headers);
	ADD("stream/bad-topics-refused", 0, test_bad_topics_refused);
	ADD("stream/logout-closes-streams", 0, test_logout_closes_streams);
	ADD("stream/revocation-and-disabling-close-streams", 0, test_revocation_and_disabling_close_streams);
	ADD("stream/role-change-closes-streams", 0, test_role_change_closes_streams);

	ADD("delivery/per-topic", 0, test_delivery_is_per_topic);
	ADD("delivery/closed-stream-is-forgotten", 0, test_closed_stream_is_forgotten);
	ADD("delivery/text-cannot-forge-fields", 0, test_event_text_cannot_forge_fields);
	ADD("delivery/oversized-fragment-is-not-sent", 0, test_oversized_fragment_is_not_sent);

	ADD("limits/per-session-cap", TWO_PER_SESSION, test_per_session_cap);
	ADD("limits/total-cap", TWO_IN_ALL, test_total_cap);
	ADD("limits/per-account-cap", THREE_PER_USER, test_per_account_cap);
	ADD("limits/admin-reserve", FOUR_TWO_KEPT, test_admin_reserve);
	g_test_add_func("/web-hub/limits/default-caps", test_default_caps);
	ADD("limits/stalled-client-is-dropped", SHORT_STALL, test_stalled_client_is_dropped);
	ADD("limits/queue-is-bounded", SMALL_QUEUE, test_queue_is_bounded);
	ADD("limits/keep-alive", SHORT_KEEP, test_keep_alive);

	ADD("pages/opens-one-stream", 0, test_page_opens_one_stream);
	ADD("pages/status-line", 0, test_page_has_status_line);
	ADD("pages/follows-only-what-it-asked", 0, test_page_follows_only_what_it_asked);
	ADD("pages/work-without-a-hub", NO_HUB, test_pages_work_without_a_hub);

	return g_test_run();
}
