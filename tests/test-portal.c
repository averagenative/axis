/*
 * test-portal.c - axis-portal's pages, against a stand-in daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3; the `portal` specification.)  Two servers
 * run on the test's own main loop: the portal's own code (libaxis-portal.a,
 * what the program axis-portal is made of) on a port the system gave, and a
 * stand-in for the daemon's portal listener that writes down every request
 * it is sent, with its headers, and answers what the test told it to.  No
 * database, no daemon: the portal has neither.
 *
 * Every test here is an attack or a failure, and what is asserted is what
 * the stand-in was asked: mostly, that it was asked nothing.
 */

#define _GNU_SOURCE

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "../clients/portal/axis-portal-app.h"
#include "../clients/portal/axis-portal-client.h"
#include "../clients/portal/axis-portal-config.h"
#include "../clients/portal/axis-portal-form.h"
#include "../clients/portal/axis-portal-log.h"
#include "../clients/portal/axis-portal-view.h"
#include "../clients/portal/axis-portal-wire.h"
#include "api/axis-portal-proto.h"
#include "axis-test-web.h"
#include "web/axis-web-headers.h"

#define ROBIN_PASSWORD "lantern-orchard-pass"
#define OUTSIDE        "203.0.113.5"

/* The fixture's modes. */
#define MODE_PROXY    0	/* behind a trusted proxy that reports https */
#define MODE_PLAIN    1	/* nobody is trusted, no override: plain http */
#define MODE_INSECURE 2	/* the test-only override: plain http is served */

typedef struct
{
	gchar *op;
	gchar *body;
	gchar *headers;		/* "Name: value\n" for each */
	gboolean valid;		/* the body is one the daemon's own reader takes */
} Call;

typedef struct
{
	guint  status;
	gchar *type;
	gchar *body;
	gsize  length;
} Answer;

typedef struct
{
	SoupServer       *daemon;
	guint16           daemon_port;
	HtmxServer       *server;
	guint16           port;
	AxisPortalApp    *app;
	AxisPortalConfig *config;
	SoupSession      *session;
	GPtrArray        *calls;	/* Call* */
	GHashTable       *script;	/* op -> Answer* */
	gint              mode;
	gchar            *token;
	gchar            *robin;	/* robin's proof */
	gchar            *sam;		/* sam's proof */
	gboolean          robin_ended;	/* the daemon no longer knows robin's proof */
} Fixture;

/* ── The log ────────────────────────────────────────────────────── */

static GString *log_text;

static void
capture(
	const gchar    *domain,
	GLogLevelFlags  level,
	const gchar    *message,
	gpointer        user_data
){
	g_string_append_printf(log_text, "%s\n", message);
}

/* A warning of the portal is part of what is tested (a daemon that is away
 * is one); a critical is a bug. */
static gboolean
fatal(
	const gchar    *domain,
	GLogLevelFlags  level,
	const gchar    *message,
	gpointer        user_data
){
	return !((level & G_LOG_LEVEL_WARNING) != 0 && g_strcmp0(domain, "axis-portal") == 0);
}

/* ── The stand-in daemon ────────────────────────────────────────── */

static void
call_free(gpointer data)
{
	Call *call = data;

	g_free(call->op);
	g_free(call->body);
	g_free(call->headers);
	g_free(call);
}

static void
answer_free(gpointer data)
{
	Answer *answer = data;

	g_free(answer->type);
	g_free(answer->body);
	g_free(answer);
}

/* From now on the stand-in answers @op with this. */
static void
script(
	Fixture     *fx,
	const gchar *op,
	guint        status,
	const gchar *body
){
	Answer *answer = g_new0(Answer, 1);

	answer->status = status;
	answer->type = g_strdup("application/json");
	answer->body = g_strdup(body);
	answer->length = strlen(body);
	g_hash_table_replace(fx->script, g_strdup(op), answer);
}

static gchar *
fill43(const gchar *prefix)
{
	GString *text = g_string_new(prefix);

	while (text->len < 43)
		g_string_append_c(text, 'A');
	return g_string_free(text, FALSE);
}

#define WHO_ROBIN "{\"name\":\"robin\",\"movies\":{\"limit\":5,\"left\":3,\"window_ends\":1900000000}," \
	"\"seasons\":{\"limit\":10,\"left\":7,\"window_ends\":1900000000}}"

#define TITLE_MOVIE "{\"id\":3,\"kind\":\"movie\",\"name\":\"The Lantern Orchard\",\"year\":2019," \
	"\"overview\":\"A made-up film about lanterns.\",\"has_poster\":true,\"state\":\"open\"}"

#define TITLE_SERIES "{\"id\":7,\"kind\":\"series\",\"name\":\"Harbour of Glass\",\"year\":2020," \
	"\"overview\":\"A made-up series.\",\"has_poster\":false,\"state\":\"partly_available\",\"asked_by\":\"sam\"," \
	"\"seasons\":[{\"number\":0,\"episodes\":2,\"aired\":2,\"state\":\"open\"}," \
	"{\"number\":1,\"episodes\":8,\"aired\":8,\"state\":\"available\"}," \
	"{\"number\":2,\"episodes\":8,\"aired\":8,\"state\":\"requested\"}," \
	"{\"number\":3,\"episodes\":8,\"aired\":5,\"state\":\"open\"}," \
	"{\"number\":4,\"episodes\":8,\"aired\":0,\"state\":\"declined\"}]}"

/* What the stand-in answers when the test scripted nothing: a small daemon
 * that knows robin and sam, one movie (3) and one series (7). */
static void
default_answer(
	Fixture      *fx,
	const gchar  *op,
	const gchar  *body,
	guint        *status,
	const gchar **json
){
	static gchar login[512];
	gboolean robin = strstr(body, fx->robin) != NULL && !fx->robin_ended;
	gboolean sam = strstr(body, fx->sam) != NULL;

	*status = 200;
	if (strcmp(op, "hello") == 0) {
		*json = "{\"protocol\":1,\"kinds\":[\"movie\",\"series\"],\"lists\":[\"popular\",\"trending\"],"
			"\"show_names\":true}";
	} else if (strcmp(op, "login") == 0) {
		if (strstr(body, "\"username\":\"robin\"") != NULL && strstr(body, "\"" ROBIN_PASSWORD "\"") != NULL) {
			g_snprintf(login, sizeof(login), "{\"proof\":\"%s\",\"name\":\"robin\",\"expires_in\":2592000,"
				"\"device\":\"zzdev1.robin-device-value\"}", fx->robin);
			*json = login;
		} else {
			*status = 401;
			*json = "{\"error\":\"login\",\"message\":\"the username or password is incorrect\"}";
		}
	} else if (!robin && !sam) {
		*status = 401;
		*json = "{\"error\":\"proof\",\"message\":\"a valid session proof is required\"}";
	} else if (strcmp(op, "whoami") == 0) {
		*json = robin ? WHO_ROBIN : "{\"name\":\"sam\",\"movies\":{\"limit\":0,\"left\":0,\"window_ends\":1900000000},"
			"\"seasons\":{\"limit\":0,\"left\":0,\"window_ends\":1900000000}}";
	} else if (strcmp(op, "logout") == 0 || strcmp(op, "withdraw") == 0) {
		*json = "{}";
	} else if (strcmp(op, "search") == 0) {
		*json = "{\"titles\":[" TITLE_MOVIE "],\"page\":1,\"more\":false,\"stale\":false}";
	} else if (strcmp(op, "browse") == 0) {
		*json = "{\"titles\":[" TITLE_MOVIE "],\"page\":1,\"more\":false}";
	} else if (strcmp(op, "title") == 0) {
		if (strstr(body, "\"id\":7") != NULL) {
			*json = "{\"title\":" TITLE_SERIES "}";
		} else if (strstr(body, "\"id\":3") != NULL) {
			*json = "{\"title\":" TITLE_MOVIE "}";
		} else {
			*status = 404;
			*json = "{\"error\":\"not-found\",\"message\":\"there is no such thing\"}";
		}
	} else if (strcmp(op, "request") == 0) {
		*json = "{\"outcome\":\"made\",\"request\":{\"id\":\"reqAAAAAAAAAAAAAAAAAAA\",\"title\":3,\"kind\":\"movie\","
			"\"name\":\"The Lantern Orchard\",\"year\":2019,\"seasons\":[],\"state\":\"waiting\",\"mine\":\"made\","
			"\"asked_at\":1800000000,\"changed_at\":1800000000},\"left_out\":[],\"followed\":0,"
			"\"movies\":{\"limit\":5,\"left\":2,\"window_ends\":1900000000},"
			"\"seasons\":{\"limit\":10,\"left\":7,\"window_ends\":1900000000}}";
	} else if (strcmp(op, "requests") == 0) {
		*json = "{\"requests\":[{\"id\":\"reqAAAAAAAAAAAAAAAAAAA\",\"title\":3,\"kind\":\"movie\","
			"\"name\":\"The Lantern Orchard\",\"year\":2019,\"seasons\":[],\"state\":\"waiting\",\"mine\":\"made\","
			"\"asked_at\":1800000000,\"changed_at\":1800000000}],\"page\":1,\"more\":false}";
	} else {
		*status = 404;
		*json = "{\"error\":\"not-found\",\"message\":\"there is no such thing\"}";
	}
}

static void
on_daemon_request(
	SoupServer        *server,
	SoupServerMessage *message,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	Fixture *fx = user_data;
	SoupMessageBody *request_body = soup_server_message_get_request_body(message);
	g_autoptr(GBytes) bytes = soup_message_body_flatten(request_body);
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;
	GString *headers = g_string_new(NULL);
	Call *call = g_new0(Call, 1);
	const Answer *answer;
	AxisPortalOp op;
	gsize length = 0;
	const gchar *data = g_bytes_get_data(bytes, &length);

	/* Everything of the request is kept: the method, the whole path with
	 * its query, every header. */
	call->op = g_str_has_prefix(path, AXIS_PORTAL_API_PREFIX) ? g_strdup(path + strlen(AXIS_PORTAL_API_PREFIX)) :
		g_strdup(path);
	call->body = g_strndup(data != NULL ? data : "", length);
	g_string_append_printf(headers, "%s %s%s%s\n", soup_server_message_get_method(message), path,
		g_uri_get_query(soup_server_message_get_uri(message)) != NULL ? "?" : "",
		g_uri_get_query(soup_server_message_get_uri(message)) != NULL ?
		g_uri_get_query(soup_server_message_get_uri(message)) : "");
	soup_message_headers_iter_init(&iter, soup_server_message_get_request_headers(message));
	while (soup_message_headers_iter_next(&iter, &name, &value))
		g_string_append_printf(headers, "%s: %s\n", name, value);
	call->headers = g_string_free(headers, FALSE);
	/* Is the body one the daemon's own reader of bodies takes whole? */
	if (axis_portal_op_from_name(call->op, &op)) {
		g_autoptr(AxisPortalRequest) parsed = axis_portal_request_parse(op, call->body, strlen(call->body), NULL);

		call->valid = parsed != NULL && parsed->valid;
	}
	g_ptr_array_add(fx->calls, call);

	/* No connection is kept: a stand-in that goes away is gone at once. */
	soup_message_headers_replace(soup_server_message_get_response_headers(message), "Connection", "close");
	answer = g_hash_table_lookup(fx->script, call->op);
	if (answer != NULL) {
		soup_server_message_set_status(message, answer->status, NULL);
		soup_server_message_set_response(message, answer->type, SOUP_MEMORY_COPY, answer->body, answer->length);
	} else {
		const gchar *json = "{}";
		guint status = 200;

		default_answer(fx, call->op, call->body, &status, &json);
		soup_server_message_set_status(message, status, NULL);
		soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
	}
}

static guint16
soup_port(SoupServer *server)
{
	GSList *uris = soup_server_get_uris(server);
	guint16 port = (guint16)g_uri_get_port(uris->data);

	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	return port;
}

/* How many times the stand-in was asked for @op (%NULL: for anything). */
static guint
asked(
	Fixture     *fx,
	const gchar *op
){
	guint n = 0;
	guint i;

	for (i = 0; i < fx->calls->len; i++) {
		const Call *call = g_ptr_array_index(fx->calls, i);

		if (op == NULL || strcmp(call->op, op) == 0)
			n++;
	}
	return n;
}

static const Call *
last_call(
	Fixture     *fx,
	const gchar *op
){
	guint i;

	for (i = fx->calls->len; i > 0; i--) {
		const Call *call = g_ptr_array_index(fx->calls, i - 1);

		if (strcmp(call->op, op) == 0)
			return call;
	}
	return NULL;
}

/* ── The fixture ────────────────────────────────────────────────── */

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	fx->mode = GPOINTER_TO_INT(data);
	if (log_text == NULL) {
		log_text = g_string_new(NULL);
		g_log_set_handler("axis-portal", G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, capture, NULL);
	}
	g_string_truncate(log_text, 0);
	g_test_log_set_fatal_handler(fatal, NULL);

	fx->token = fill43("zzPortalToken");
	fx->robin = fill43("zzProofRobin");
	fx->sam = fill43("zzProofSam");
	fx->calls = g_ptr_array_new_with_free_func(call_free);
	fx->script = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, answer_free);

	fx->daemon = soup_server_new(NULL, NULL);
	soup_server_add_handler(fx->daemon, NULL, on_daemon_request, fx, NULL);
	g_assert_true(soup_server_listen_local(fx->daemon, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
	g_assert_no_error(error);
	fx->daemon_port = soup_port(fx->daemon);

	fx->config = g_new0(AxisPortalConfig, 1);
	fx->config->url = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->daemon_port);
	fx->config->host = g_strdup("127.0.0.1");
	fx->config->loopback = TRUE;
	fx->config->token = g_strdup(fx->token);
	fx->config->request_timeout_ms = 5000;
	fx->config->listen_host = g_strdup("127.0.0.1");
	fx->config->trusted_proxies = g_strdup(fx->mode == MODE_PROXY ? "127.0.0.1" : "");
	fx->config->insecure_browser_http = fx->mode == MODE_INSECURE;
	fx->config->max_connections = 256;
	fx->config->max_per_address = 64;

	fx->app = axis_portal_app_new(fx->config, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fx->app);
	fx->server = htmx_server_new();
	axis_portal_app_install(fx->app, fx->server);
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	g_object_unref(fx->session);
	axis_portal_app_stop(fx->app);
	htmx_server_stop(fx->server);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_portal_app_free(fx->app);
	g_object_unref(fx->server);
	soup_server_disconnect(fx->daemon);
	g_object_unref(fx->daemon);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_portal_config_free(fx->config);
	g_ptr_array_unref(fx->calls);
	g_hash_table_unref(fx->script);
	g_free(fx->token);
	g_free(fx->robin);
	g_free(fx->sam);
}

/* ── Requests of the portal ─────────────────────────────────────── */

#define assert_status(reply, expected) G_STMT_START { \
	if ((reply)->status != (expected)) \
		g_error("%s:%d: expected %u, got %u:\n%s", __FILE__, __LINE__, (guint)(expected), (reply)->status, \
			(reply)->body); \
} G_STMT_END

#define assert_has(text, needle) G_STMT_START { \
	if (strstr((text), (needle)) == NULL) \
		g_error("%s:%d: \"%s\" is not in:\n%s", __FILE__, __LINE__, (needle), (text)); \
} G_STMT_END

#define assert_lacks(text, needle) G_STMT_START { \
	if (strstr((text), (needle)) != NULL) \
		g_error("%s:%d: \"%s\" is in:\n%s", __FILE__, __LINE__, (needle), (text)); \
} G_STMT_END

/* How many times @needle is in @text. */
static guint
count_of(
	const gchar *text,
	const gchar *needle
){
	guint n = 0;

	while ((text = strstr(text, needle)) != NULL) {
		n++;
		text += strlen(needle);
	}
	return n;
}

/* A request as a browser behind the fixture's proxy sends it (in the other
 * modes: as it is), from @from (%NULL: OUTSIDE), with @more headers. */
static AxisTestReply
send_from(
	Fixture            *fx,
	const gchar        *method,
	const gchar        *path,
	const gchar        *cookie,
	const gchar        *form,
	const gchar        *from,
	const gchar *const *more
){
	const gchar *extra[32];
	guint n = 0;
	guint i;

	if (fx->mode == MODE_PROXY) {
		extra[n++] = "X-Forwarded-Proto";
		extra[n++] = "https";
		extra[n++] = "X-Forwarded-For";
		extra[n++] = from != NULL ? from : OUTSIDE;
	}
	for (i = 0; more != NULL && more[i] != NULL && n + 2 < G_N_ELEMENTS(extra); i++)
		extra[n++] = more[i];
	extra[n] = NULL;
	return axis_test_web_request(fx->session, fx->port, method, path, cookie, extra, form);
}

static AxisTestReply
get(
	Fixture     *fx,
	const gchar *path,
	const gchar *cookie
){
	return send_from(fx, "GET", path, cookie, NULL, NULL, NULL);
}

static AxisTestReply
post(
	Fixture     *fx,
	const gchar *path,
	const gchar *cookie,
	const gchar *form
){
	return send_from(fx, "POST", path, cookie, form, NULL, NULL);
}

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
} Typed;

static void
on_typed(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Typed *typed = user_data;

	typed->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, NULL);
	g_main_loop_quit(typed->loop);
}

static void assert_headers(AxisTestReply *reply);

/* A post whose body is declared @type; the status.  Whatever it is answered
 * with carries the security headers. */
static guint
post_typed(
	Fixture     *fx,
	const gchar *path,
	const gchar *cookie,
	const gchar *type,
	const gchar *body
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)fx->port, path);
	g_autoptr(SoupMessage) message = soup_message_new("POST", url);
	g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));
	SoupMessageHeaders *headers = soup_message_get_request_headers(message);
	Typed typed;

	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(headers, "Cookie", cookie);
	soup_message_headers_append(headers, "X-Forwarded-Proto", "https");
	soup_message_headers_append(headers, "X-Forwarded-For", OUTSIDE);
	soup_message_set_request_body_from_bytes(message, type, payload);
	typed.loop = g_main_loop_new(NULL, FALSE);
	typed.bytes = NULL;
	soup_session_send_and_read_async(fx->session, message, G_PRIORITY_DEFAULT, NULL, on_typed, &typed);
	g_main_loop_run(typed.loop);
	g_main_loop_unref(typed.loop);
	g_assert_nonnull(typed.bytes);
	g_bytes_unref(typed.bytes);
	{
		AxisTestReply view;

		memset(&view, 0, sizeof(view));
		view.message = message;
		assert_headers(&view);
	}
	return soup_message_get_status(message);
}

static const gchar *
session_name(Fixture *fx)
{
	return fx->mode == MODE_INSECURE ? AXIS_PORTAL_SESSION_COOKIE_PLAIN : AXIS_PORTAL_SESSION_COOKIE;
}

static const gchar *
form_name(Fixture *fx)
{
	return fx->mode == MODE_INSECURE ? AXIS_PORTAL_FORM_COOKIE_PLAIN : AXIS_PORTAL_FORM_COOKIE;
}

/* "name=value" of the session cookie that the portal sets for @proof: the
 * proof, sealed with the fixture's token. */
static gchar *
sealed(
	Fixture     *fx,
	const gchar *proof
){
	g_autofree gchar *value = axis_portal_session_cookie(fx->token, proof);

	return g_strdup_printf("%s=%s", session_name(fx), value);
}

/* The whole Set-Cookie header that sets @name, or %NULL. */
static gchar *
set_cookie_line(
	AxisTestReply *reply,
	const gchar   *name
){
	g_autofree gchar *prefix = g_strconcat(name, "=", NULL);
	SoupMessageHeadersIter iter;
	const gchar *header;
	const gchar *value;

	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(reply->message));
	while (soup_message_headers_iter_next(&iter, &header, &value)) {
		if (g_ascii_strcasecmp(header, "Set-Cookie") == 0 && g_str_has_prefix(value, prefix))
			return g_strdup(value);
	}
	return NULL;
}

/* "name=value" of the cookie the reply sets, as a browser sends it back. */
static gchar *
set_cookie(
	AxisTestReply *reply,
	const gchar   *name
){
	g_autofree gchar *line = set_cookie_line(reply, name);

	return line != NULL ? g_strndup(line, strcspn(line, ";")) : NULL;
}

/* The value of the first hidden field called @name in @html. */
static gchar *
hidden_value(
	const gchar *html,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("name=\"%s\" value=\"", name);
	const gchar *start = strstr(html, needle);

	if (start == NULL)
		return NULL;
	start += strlen(needle);
	return g_strndup(start, strcspn(start, "\""));
}

/* Posts a login as a browser does: the form first, then its fields.  The
 * reply (the caller clears it); @cookie_out: the session cookie, or %NULL. */
static AxisTestReply
login_from(
	Fixture      *fx,
	const gchar  *username,
	const gchar  *password,
	const gchar  *from,
	const gchar  *next,
	gchar       **cookie_out
){
	AxisTestReply page = send_from(fx, "GET", "/login", NULL, NULL, from, NULL);
	g_autofree gchar *form_cookie = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *user = g_uri_escape_string(username, NULL, FALSE);
	g_autofree gchar *pass = g_uri_escape_string(password, NULL, FALSE);
	g_autofree gchar *where = next != NULL ? g_uri_escape_string(next, NULL, FALSE) : NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	assert_status(&page, 200);
	form_cookie = set_cookie(&page, form_name(fx));
	token = hidden_value(page.body, "_csrf");
	g_assert_nonnull(form_cookie);
	g_assert_nonnull(token);
	axis_test_reply_clear(&page);
	form = g_strdup_printf("_csrf=%s&username=%s&password=%s%s%s", token, user, pass, where != NULL ? "&next=" : "",
		where != NULL ? where : "");
	reply = send_from(fx, "POST", "/login", form_cookie, form, from, NULL);
	if (cookie_out != NULL)
		*cookie_out = reply.status == 303 ? set_cookie(&reply, session_name(fx)) : NULL;
	return reply;
}

/* Logs robin in; the session cookie. */
static gchar *
login_robin(Fixture *fx)
{
	gchar *cookie = NULL;
	AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, NULL, NULL, &cookie);

	assert_status(&reply, 303);
	g_assert_nonnull(cookie);
	axis_test_reply_clear(&reply);
	return cookie;
}

/* The CSRF token of the session @cookie, from a page's form. */
static gchar *
csrf_of(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply reply = get(fx, "/requests", cookie);
	gchar *token;

	assert_status(&reply, 200);
	token = hidden_value(reply.body, "_csrf");
	g_assert_nonnull(token);
	axis_test_reply_clear(&reply);
	return token;
}

/* ── Configuration ──────────────────────────────────────────────── */

static AxisPortalConfig *
load(
	const gchar  *file_text,
	const gchar  *env_name,
	const gchar  *env_value,
	GError      **error
){
	g_autofree gchar *path = NULL;
	g_auto(GStrv) envp = g_new0(gchar *, 1);
	AxisPortalConfig *config;
	gint fd;

	if (env_name != NULL)
		envp = g_environ_setenv(envp, env_name, env_value, TRUE);
	if (file_text == NULL)
		return axis_portal_config_load(NULL, envp, TRUE, error);
	fd = g_file_open_tmp("axis-portal-test-XXXXXX.conf", &path, NULL);
	g_assert_cmpint(fd, >=, 0);
	g_close(fd, NULL);
	g_assert_true(g_file_set_contents(path, file_text, -1, NULL));
	config = axis_portal_config_load(path, envp, TRUE, error);
	g_unlink(path);
	return config;
}

/* 43 characters, as the daemon's tokens have. */
#define TOKEN_LINE "token = zzPortalToken" "AAAAAAAAAA" "AAAAAAAAAA" "AAAAAAAAAA" "\n"

/* A text and its length, for the readers. */
#define LIT(text) (text), strlen(text)

/* Spec (portal): An unknown key -- a configuration with database_url is
 * refused, and the key is named; so is every other key of the daemon's. */
static void
test_config_unknown_key(void)
{
	static const gchar *const keys[] = {
		"database_url", "data_dir", "media_path", "tmdb_api_key", "indexer", "download_client", "listen_port", NULL
	};
	g_autoptr(AxisPortalConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	for (i = 0; keys[i] != NULL; i++) {
		g_autofree gchar *text = g_strdup_printf("[portal]\nurl = http://127.0.0.1:7801\n" TOKEN_LINE
			"%s = postgresql://axis@db/axis\n", keys[i]);

		g_clear_error(&error);
		g_assert_null(load(text, NULL, NULL, &error));
		g_assert_nonnull(error);
		assert_has(error->message, keys[i]);
		assert_has(error->message, "does not know");
		assert_lacks(error->message, "zzPortalToken");
	}
	/* A group that is not the portal's, and a variable that is not a key. */
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://127.0.0.1:7801\n" TOKEN_LINE "[database]\nurl = x\n", NULL, NULL, &error));
	assert_has(error->message, "group other than [portal]");
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://127.0.0.1:7801\n" TOKEN_LINE, "AXIS_PORTAL_DATABASE_URL", "x", &error));
	assert_has(error->message, "AXIS_PORTAL_DATABASE_URL");
	/* The daemon's own setting for its portal listener is not the portal's. */
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://127.0.0.1:7801\n" TOKEN_LINE, "AXIS_PORTAL_LISTEN_PORT", "7801", &error));
	assert_has(error->message, "setting of the daemon");

	/* What it does know. */
	g_clear_error(&error);
	config = load("[portal]\nurl = http://127.0.0.1:7801/\n" TOKEN_LINE "web_listen_port = 7999\n"
		"trusted_proxies = traefik, 10.0.0.0/8\nlog_format = json\n", NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(config->url, ==, "http://127.0.0.1:7801");
	g_assert_cmpuint(config->listen_port, ==, 7999);
	g_assert_cmpstr(config->listen_host, ==, "127.0.0.1");
	g_assert_false(config->insecure_browser_http);
	{
		g_autofree gchar *described = axis_portal_config_describe(config);

		/* The settings are logged at start, without the token. */
		assert_has(described, "url=http://127.0.0.1:7801");
		assert_lacks(described, "zzPortalToken");
	}
}

/* An environment with the portal's own settings and @pairs (name, value,
 * %NULL-ended). */
static AxisPortalConfig *
load_env(
	const gchar *const  *pairs,
	GError             **error
){
	g_auto(GStrv) envp = g_new0(gchar *, 1);
	guint i;

	envp = g_environ_setenv(envp, "AXIS_PORTAL_URL", "http://127.0.0.1:7801", TRUE);
	envp = g_environ_setenv(envp, "AXIS_PORTAL_TOKEN", "zzPortalToken" "AAAAAAAAAA" "AAAAAAAAAA" "AAAAAAAAAA", TRUE);
	for (i = 0; pairs[i] != NULL; i += 2)
		envp = g_environ_setenv(envp, pairs[i], pairs[i + 1], TRUE);
	return axis_portal_config_load(NULL, envp, TRUE, error);
}

/* Found by the review of P3 (S4): Kubernetes puts variables into every pod
 * of a namespace for each Service there, so a Service called axis-portal
 * stopped the portal from starting, and for most of them the message did not
 * name the variable.  They are left aside, and named in one line; any other
 * unknown variable still stops it, by its name. */
static void
test_config_service_links(void)
{
	static const gchar *const links[] = {
		"AXIS_PORTAL_SERVICE_HOST", "10.0.0.7",
		"AXIS_PORTAL_SERVICE_PORT", "7802",
		"AXIS_PORTAL_SERVICE_PORT_HTTP", "7802",
		"AXIS_PORTAL_PORT", "tcp://10.0.0.7:7802",
		"AXIS_PORTAL_PORT_7802_TCP", "tcp://10.0.0.7:7802",
		"AXIS_PORTAL_PORT_7802_TCP_PROTO", "tcp",
		"AXIS_PORTAL_PORT_7802_TCP_PORT", "7802",
		"AXIS_PORTAL_PORT_7802_TCP_ADDR", "10.0.0.7",
		"AXIS_PORTAL_PORT_53_UDP", "udp://[fd00::7]:53",
		NULL
	};
	static const struct
	{
		const gchar *name;
		const gchar *value;
		const gchar *said;
	} refused[] = {
		/* A name of a link, with a value that is not one. */
		{ "AXIS_PORTAL_PORT", "7802", "AXIS_PORTAL_PORT, which axis-portal does not know" },
		{ "AXIS_PORTAL_PORT_7802_TCP", "8125", "AXIS_PORTAL_PORT_7802_TCP, which axis-portal does not know" },
		{ "AXIS_PORTAL_SERVICE_HOST", "portal.example.net", "AXIS_PORTAL_SERVICE_HOST, which" },
		{ "AXIS_PORTAL_PORT_7802_TCP_PROTO", "http", "AXIS_PORTAL_PORT_7802_TCP_PROTO, which" },
		{ "AXIS_PORTAL_PORT_99999_TCP_PORT", "7802", "AXIS_PORTAL_PORT_99999_TCP_PORT, which" },
		/* Another Service's links, and a setting that is not one. */
		{ "AXIS_PORTAL_API_SERVICE_HOST", "10.0.0.9", "enableServiceLinks: false" },
		{ "AXIS_PORTAL_DATABASE_URL", "postgresql://axis@db/axis", "the environment has AXIS_PORTAL_DATABASE_URL" },
		{ "AXIS_PORTAL_TIMEOUT_2", "zz-five", "the environment has AXIS_PORTAL_TIMEOUT_2" },
		/* A name that might be a token pasted by mistake is not printed. */
		{ "AXIS_PORTAL_zzSecretLooking-Value", "zz-its-value", "not printed in case it holds a secret" }
	};
	g_autoptr(AxisPortalConfig) config = NULL;
	guint i;

	{
		g_autoptr(GError) error = NULL;

		config = load_env(links, &error);
		g_assert_no_error(error);
		g_assert_nonnull(config);
		for (i = 0; links[i] != NULL; i += 2)
			assert_has(config->service_links, links[i]);
		/* None of them is taken for a setting. */
		g_assert_cmpuint(config->listen_port, ==, AXIS_PORTAL_DEFAULT_PORT);
	}
	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		g_autoptr(GError) error = NULL;
		const gchar *pairs[3];

		pairs[0] = refused[i].name;
		pairs[1] = refused[i].value;
		pairs[2] = NULL;
		g_assert_null(load_env(pairs, &error));
		g_assert_nonnull(error);
		assert_has(error->message, refused[i].said);
		assert_lacks(error->message, "zzSecretLooking");
		assert_lacks(error->message, refused[i].value);
	}
}

/* Spec (portal): Plain HTTP refused -- an unencrypted address that is not
 * loopback, without the override, stops the portal with the reason. */
static void
test_config_plain_http(void)
{
	g_autoptr(AxisPortalConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_null(load("[portal]\nurl = http://192.0.2.10:7801\n" TOKEN_LINE, NULL, NULL, &error));
	assert_has(error->message, "refusing to send the portal's token over plain http to 192.0.2.10");
	assert_has(error->message, "allow_insecure_http");
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://axis:7801\n" TOKEN_LINE, NULL, NULL, &error));
	assert_has(error->message, "plain http");

	/* With the override; to this machine; over https. */
	g_clear_error(&error);
	config = load("[portal]\nurl = http://axis:7801\nallow_insecure_http = true\n" TOKEN_LINE, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(config);
	g_clear_pointer(&config, axis_portal_config_free);
	config = load("[portal]\nurl = http://localhost:7801\n" TOKEN_LINE, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(config);
	g_clear_pointer(&config, axis_portal_config_free);
	config = load("[portal]\nurl = https://axis.example.net\n" TOKEN_LINE, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(config);
	g_clear_pointer(&config, axis_portal_config_free);

	/* No token, a token that is not one, a token twice, an address with a
	 * password in it. */
	g_assert_null(load("[portal]\nurl = http://127.0.0.1:7801\n", NULL, NULL, &error));
	assert_has(error->message, "token is required");
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://127.0.0.1:7801\ntoken = zz-short-secret\n", NULL, NULL, &error));
	assert_has(error->message, "not in the form");
	assert_lacks(error->message, "zz-short-secret");
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://127.0.0.1:7801\n" TOKEN_LINE "token_file = /nonexistent\n", NULL, NULL,
		&error));
	assert_has(error->message, "given twice");
	g_clear_error(&error);
	g_assert_null(load("[portal]\nurl = http://zzuser:zzpass@127.0.0.1:7801\n" TOKEN_LINE, NULL, NULL, &error));
	assert_has(error->message, "no user name, password");

	/* Found by the review of P3 (M10): the test-only override, which turns
	 * the cookies' Secure off, is refused on a listener that anything but
	 * this machine can reach. */
	{
		static const gchar *const open[] = {
			"AXIS_PORTAL_INSECURE_BROWSER_HTTP", "true", "AXIS_PORTAL_WEB_LISTEN_HOST", "0.0.0.0", NULL
		};
		static const gchar *const here[] = {
			"AXIS_PORTAL_INSECURE_BROWSER_HTTP", "true", "AXIS_PORTAL_WEB_LISTEN_HOST", "::1", NULL
		};

		g_clear_error(&error);
		g_assert_null(load_env(open, &error));
		assert_has(error->message, "AXIS_PORTAL_INSECURE_BROWSER_HTTP is for tests only");
		assert_has(error->message, "0.0.0.0");
		g_clear_error(&error);
		config = load_env(here, &error);
		g_assert_no_error(error);
		g_assert_true(config->insecure_browser_http);
		g_clear_pointer(&config, axis_portal_config_free);
	}
	/* (M9) A token file that every user may read is said at start. */
	{
		g_autofree gchar *path = NULL;
		g_autofree gchar *text = NULL;
		gint fd = g_file_open_tmp("axis-portal-test-XXXXXX.token", &path, NULL);

		g_assert_cmpint(fd, >=, 0);
		g_close(fd, NULL);
		g_assert_true(g_file_set_contents(path, "zzPortalToken" "AAAAAAAAAA" "AAAAAAAAAA" "AAAAAAAAAA\n", -1, NULL));
		text = g_strdup_printf("[portal]\nurl = http://127.0.0.1:7801\ntoken_file = %s\n", path);
		g_assert_cmpint(g_chmod(path, 0644), ==, 0);
		config = load(text, NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_true(config->token_file_readable_by_all);
		g_clear_pointer(&config, axis_portal_config_free);
		g_assert_cmpint(g_chmod(path, 0600), ==, 0);
		config = load(text, NULL, NULL, &error);
		g_assert_no_error(error);
		g_assert_false(config->token_file_readable_by_all);
		g_clear_pointer(&config, axis_portal_config_free);
		g_unlink(path);
	}
}

/* ── The messages ───────────────────────────────────────────────── */

/* The operations' names are the daemon's own list, and every body the
 * portal's writer makes is one the daemon's reader takes whole. */
static void
test_wire_bodies(void)
{
	g_autofree gchar *proof = fill43("zzProofRobin");
	gint picked[3] = { 0, 2, 5 };
	gint twice[2] = { 1, 1 };
	struct
	{
		AxisPortalOp  op;
		gchar        *body;
	} bodies[12];
	guint n = 0;
	guint i;

	for (i = 0; i < AXIS_PORTAL_N_OPS; i++)
		g_assert_cmpstr(axis_portal_client_op_name((AxisPortalOp)i), ==, axis_portal_op_name((AxisPortalOp)i));
	g_assert_cmpstr(axis_portal_client_op_name((AxisPortalOp)AXIS_PORTAL_N_OPS), ==, "?");

	bodies[n].op = AXIS_PORTAL_LOGIN;
	bodies[n++].body = axis_portal_wire_login("robin", "a \"quoted\" pass\\word", "203.0.113.5", "a browser", NULL);
	bodies[n].op = AXIS_PORTAL_LOGIN;
	bodies[n++].body = axis_portal_wire_login("robin", "pass", "2001:db8::7", "a browser", "zzdev1.value");
	bodies[n].op = AXIS_PORTAL_WHOAMI;
	bodies[n++].body = axis_portal_wire_proof(proof);
	bodies[n].op = AXIS_PORTAL_LOGOUT;
	bodies[n++].body = axis_portal_wire_proof(proof);
	bodies[n].op = AXIS_PORTAL_SEARCH;
	bodies[n++].body = axis_portal_wire_search(proof, "lantern \"orchard\"", -1, 1);
	bodies[n].op = AXIS_PORTAL_SEARCH;
	bodies[n++].body = axis_portal_wire_search(proof, "harbour", 1, 2);
	bodies[n].op = AXIS_PORTAL_BROWSE;
	bodies[n++].body = axis_portal_wire_browse(proof, "top_rated", AXIS_PORTAL_KIND_IS_SERIES, 3);
	bodies[n].op = AXIS_PORTAL_TITLE;
	bodies[n++].body = axis_portal_wire_title(proof, 7);
	bodies[n].op = AXIS_PORTAL_REQUEST;
	bodies[n++].body = axis_portal_wire_request(proof, 3, AXIS_PORTAL_KIND_IS_MOVIE, AXIS_PORTAL_ASK_ALL, NULL, 0);
	bodies[n].op = AXIS_PORTAL_REQUEST;
	bodies[n++].body = axis_portal_wire_request(proof, 7, AXIS_PORTAL_KIND_IS_SERIES, AXIS_PORTAL_ASK_PICKED, picked, 3);
	bodies[n].op = AXIS_PORTAL_REQUESTS;
	bodies[n++].body = axis_portal_wire_requests(proof, 2);
	bodies[n].op = AXIS_PORTAL_WITHDRAW;
	bodies[n++].body = axis_portal_wire_withdraw(proof, "reqAAAAAAAAAAAAAAAAAAA");
	for (i = 0; i < n; i++) {
		g_autoptr(AxisPortalRequest) parsed = NULL;

		g_assert_nonnull(bodies[i].body);
		parsed = axis_portal_request_parse(bodies[i].op, bodies[i].body, strlen(bodies[i].body), NULL);
		if (parsed == NULL || !parsed->valid)
			g_error("the daemon's reader refuses the portal's body of %s: %s (%s)",
				axis_portal_op_name(bodies[i].op), bodies[i].body, parsed != NULL ? parsed->problem : "not JSON");
		axis_portal_wire_free(bodies[i].body);
	}

	/* What the writer does not write: nothing is sent then. */
	g_assert_null(axis_portal_wire_proof("short"));
	g_assert_null(axis_portal_wire_proof(NULL));
	g_assert_null(axis_portal_wire_title(proof, 0));
	g_assert_null(axis_portal_wire_title(proof, -4));
	g_assert_null(axis_portal_wire_title("zz\",\"id\":1,\"x\":\"", 7));
	g_assert_null(axis_portal_wire_search(proof, "", -1, 1));
	g_assert_null(axis_portal_wire_search(proof, "a\nb", -1, 1));
	g_assert_null(axis_portal_wire_search(proof, "lantern", -1, 3));
	g_assert_null(axis_portal_wire_search(proof, "lantern", 2, 1));
	g_assert_null(axis_portal_wire_search(proof, "\xff\xfe", -1, 1));
	g_assert_null(axis_portal_wire_browse(proof, "Popular", AXIS_PORTAL_KIND_IS_MOVIE, 1));
	g_assert_null(axis_portal_wire_browse(proof, "popular\",\"kind\":\"x", AXIS_PORTAL_KIND_IS_MOVIE, 1));
	g_assert_null(axis_portal_wire_browse(proof, "popular", AXIS_PORTAL_KIND_IS_MOVIE, 0));
	g_assert_null(axis_portal_wire_request(proof, 7, AXIS_PORTAL_KIND_IS_SERIES, AXIS_PORTAL_ASK_PICKED, NULL, 0));
	g_assert_null(axis_portal_wire_request(proof, 7, AXIS_PORTAL_KIND_IS_SERIES, AXIS_PORTAL_ASK_PICKED, twice, 2));
	g_assert_null(axis_portal_wire_request(proof, 7, (AxisPortalKind)5, AXIS_PORTAL_ASK_ALL, NULL, 0));
	g_assert_null(axis_portal_wire_withdraw(proof, "not-an-identifier"));
	g_assert_null(axis_portal_wire_withdraw(proof, "req/../../../AAAAAAAAA"));
	g_assert_null(axis_portal_wire_login("", "pass", "203.0.113.5", "a browser", NULL));
	g_assert_null(axis_portal_wire_login("robin", "", "203.0.113.5", "a browser", NULL));
	g_assert_null(axis_portal_wire_login("robin", "pass", "not an address", "a browser", NULL));
	g_assert_null(axis_portal_wire_login("robin", "pass", "203.0.113.5", "a\nbrowser", NULL));
	g_assert_null(axis_portal_wire_login("robin", "pass", "203.0.113.5", "a browser", "a device; value"));
	g_assert_null(axis_portal_wire_login("robin", "\xff", "203.0.113.5", "a browser", NULL));
	{
		g_autofree gchar *long_name = g_strnfill(AXIS_PORTAL_USERNAME_MAX_BYTES + 1, 'a');
		g_autofree gchar *long_pass = g_strnfill(AXIS_PORTAL_PASSWORD_MAX_BYTES + 1, 'a');

		g_assert_null(axis_portal_wire_login(long_name, "pass", "203.0.113.5", "a browser", NULL));
		g_assert_null(axis_portal_wire_login("robin", long_pass, "203.0.113.5", "a browser", NULL));
	}
}

#define TITLE(extra) "{\"title\":{\"id\":7,\"kind\":\"series\",\"name\":\"Harbour of Glass\",\"year\":2020," \
	"\"overview\":\"x\",\"has_poster\":false,\"state\":\"open\"" extra "}}"

/* An answer is exactly what its operation answers, or it is refused whole. */
static void
test_wire_answers(void)
{
	static const gchar *const refused[] = {
		/* A member the operation does not define; one twice; one missing. */
		TITLE(",\"path\":\"/media/zz\""),
		TITLE(",\"id\":8"),
		"{\"title\":{\"id\":7,\"kind\":\"series\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\",\"state\":\"open\"}}",
		/* A word that is not one of the fixed ones; a wrong type; a range. */
		"{\"title\":{\"id\":7,\"kind\":\"series\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\",\"has_poster\":false,"
			"\"state\":\"<b>open</b>\"}}",
		"{\"title\":{\"id\":7,\"kind\":\"game\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\",\"has_poster\":false,"
			"\"state\":\"open\"}}",
		"{\"title\":{\"id\":\"7\",\"kind\":\"series\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\","
			"\"has_poster\":false,\"state\":\"open\"}}",
		"{\"title\":{\"id\":0,\"kind\":\"series\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\",\"has_poster\":false,"
			"\"state\":\"open\"}}",
		"{\"title\":{\"id\":7,\"kind\":\"series\",\"name\":7,\"year\":2020,\"overview\":\"x\",\"has_poster\":false,"
			"\"state\":\"open\"}}",
		"{\"title\":{\"id\":7,\"kind\":\"series\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\",\"has_poster\":1,"
			"\"state\":\"open\"}}",
		/* A name that is not an account's; seasons of a movie; a season twice. */
		TITLE(",\"asked_by\":\"<script>\""),
		TITLE(",\"asked_by\":\"Robin Of The Orchard\""),
		"{\"title\":{\"id\":3,\"kind\":\"movie\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\",\"has_poster\":false,"
			"\"state\":\"open\",\"seasons\":[]}}",
		TITLE(",\"seasons\":[{\"number\":1,\"episodes\":1,\"aired\":1,\"state\":\"open\"},"
			"{\"number\":1,\"episodes\":1,\"aired\":1,\"state\":\"open\"}]"),
		TITLE(",\"seasons\":[{\"number\":1,\"episodes\":1,\"aired\":1,\"state\":\"open\",\"file\":\"zz\"}]"),
		/* Not one object; two; nothing. */
		"[]", "{\"title\":{}}", "{}", "", "{\"title\":" TITLE_MOVIE "}{}", "{\"title\":null}", NULL
	};
	g_autoptr(AxisPortalTitleInfo) title = NULL;
	g_autoptr(AxisPortalTitles) titles = NULL;
	g_autoptr(AxisPortalWho) who = NULL;
	g_autoptr(AxisPortalHello) hello = NULL;
	g_autoptr(AxisPortalLogin) login = NULL;
	g_autoptr(AxisPortalAsked) made = NULL;
	g_autoptr(AxisPortalRequests) requests = NULL;
	g_autofree gchar *proof = fill43("zzProofRobin");
	g_autofree gchar *text = NULL;
	AxisPortalRefusal refusal;
	guint i;

	for (i = 0; refused[i] != NULL; i++) {
		AxisPortalTitleInfo *read = axis_portal_wire_read_title(refused[i], strlen(refused[i]));

		if (read != NULL)
			g_error("this answer of title was read: %s", refused[i]);
	}
	title = axis_portal_wire_read_title("{\"title\":" TITLE_SERIES "}", strlen("{\"title\":" TITLE_SERIES "}"));
	g_assert_nonnull(title);
	g_assert_cmpint(title->id, ==, 7);
	g_assert_cmpint(title->kind, ==, AXIS_PORTAL_KIND_IS_SERIES);
	g_assert_cmpint(title->state, ==, AXIS_PORTAL_TITLE_PARTLY);
	g_assert_cmpstr(title->asked_by, ==, "sam");
	g_assert_cmpuint(title->seasons->len, ==, 5);
	g_clear_pointer(&title, axis_portal_title_info_free);

	/* A text of a provider is cut to its length, and a control character or
	 * one that turns the text around is a space. */
	text = g_strdup_printf("{\"title\":{\"id\":7,\"kind\":\"movie\",\"name\":\"a\\u0007b\\u202ec\\nd\",\"year\":0,"
		"\"overview\":\"%s\",\"has_poster\":true,\"state\":\"closed\"}}", "0123456789");
	title = axis_portal_wire_read_title(text, strlen(text));
	g_assert_nonnull(title);
	g_assert_cmpstr(title->name, ==, "a b c d");
	g_clear_pointer(&title, axis_portal_title_info_free);
	{
		g_autofree gchar *long_overview = g_strnfill(3000, 'o');
		g_autofree gchar *with = g_strdup_printf("{\"title\":{\"id\":7,\"kind\":\"movie\",\"name\":\"x\",\"year\":0,"
			"\"overview\":\"%s\",\"has_poster\":true,\"state\":\"closed\"}}", long_overview);

		title = axis_portal_wire_read_title(with, strlen(with));
		g_assert_nonnull(title);
		g_assert_cmpuint(strlen(title->overview), ==, AXIS_PORTAL_WIRE_OVERVIEW_MAX_CHARS);
	}

	/* A list of a search has "stale"; one of browse must not; a title of a
	 * list carries no seasons. */
#define LIST "{\"titles\":[" TITLE_MOVIE "],\"page\":1,\"more\":true"
	titles = axis_portal_wire_read_titles(LIST ",\"stale\":true}", strlen(LIST ",\"stale\":true}"), TRUE, 1);
	g_assert_nonnull(titles);
	g_assert_true(titles->stale && titles->more);
	g_assert_null(axis_portal_wire_read_titles(LIST "}", strlen(LIST "}"), TRUE, 1));
	g_assert_null(axis_portal_wire_read_titles(LIST ",\"stale\":true}", strlen(LIST ",\"stale\":true}"), FALSE, 1));
	g_assert_null(axis_portal_wire_read_titles("{\"titles\":[" TITLE_SERIES "],\"page\":1,\"more\":true}",
		strlen("{\"titles\":[" TITLE_SERIES "],\"page\":1,\"more\":true}"), FALSE, 1));
	g_assert_null(axis_portal_wire_read_titles(LIST ",\"total\":9}", strlen(LIST ",\"total\":9}"), FALSE, 1));
	/* Found by the review of P3 (M16): the answer for another page than the
	 * one asked for is refused, and no page links past the last page that
	 * its route takes. */
	g_assert_null(axis_portal_wire_read_titles(LIST ",\"stale\":true}", strlen(LIST ",\"stale\":true}"), TRUE, 2));
	{
#define LIST_2 "{\"titles\":[" TITLE_MOVIE "],\"page\":2,\"more\":true,\"stale\":false}"
		g_autoptr(AxisPortalTitles) second = axis_portal_wire_read_titles(LIST_2, strlen(LIST_2), TRUE, 2);
		g_autofree gchar *page = NULL;

		g_assert_nonnull(second);
		g_assert_true(second->more);
		page = axis_portal_view_search(NULL, "lantern", -1, second);
		assert_has(page, "The Lantern Orchard");
		assert_lacks(page, "page=3");
		assert_lacks(page, ">More<");
	}

	/* whoami, hello, login. */
	who = axis_portal_wire_read_whoami(WHO_ROBIN, strlen(WHO_ROBIN));
	g_assert_nonnull(who);
	g_assert_cmpuint(who->movies.left, ==, 3);
#define WHO_ROLE "{\"name\":\"robin\",\"role\":\"admin\",\"movies\":{\"limit\":5,\"left\":3,\"window_ends\":1}," \
	"\"seasons\":{\"limit\":1,\"left\":1,\"window_ends\":1}}"
	g_assert_null(axis_portal_wire_read_whoami(WHO_ROLE, strlen(WHO_ROLE)));
#define HELLO "{\"protocol\":1,\"kinds\":[\"movie\"],\"lists\":[\"popular\"],\"show_names\":false}"
	hello = axis_portal_wire_read_hello(HELLO, strlen(HELLO));
	g_assert_nonnull(hello);
	g_assert_true(hello->movies && !hello->series);
#define HELLO_2 "{\"protocol\":2,\"kinds\":[\"movie\"],\"lists\":[\"popular\"],\"show_names\":false}"
	g_assert_null(axis_portal_wire_read_hello(HELLO_2, strlen(HELLO_2)));
#define HELLO_LIST "{\"protocol\":1,\"kinds\":[\"movie\"],\"lists\":[\"<a href=x>\"],\"show_names\":false}"
	g_assert_null(axis_portal_wire_read_hello(HELLO_LIST, strlen(HELLO_LIST)));
#define HELLO_VERSION "{\"protocol\":1,\"kinds\":[],\"lists\":[],\"show_names\":false,\"version\":\"0.1.0\"}"
	g_assert_null(axis_portal_wire_read_hello(HELLO_VERSION, strlen(HELLO_VERSION)));
	g_clear_pointer(&text, g_free);
	text = g_strdup_printf("{\"proof\":\"%s\",\"name\":\"robin\",\"expires_in\":60,\"device\":\"\"}", proof);
	login = axis_portal_wire_read_login(text, strlen(text));
	g_assert_nonnull(login);
	g_assert_null(login->device);
	g_clear_pointer(&login, axis_portal_login_free);
	g_clear_pointer(&text, g_free);
	text = g_strdup_printf("{\"proof\":\"%s\",\"name\":\"robin\",\"expires_in\":60,\"device\":\"a; Path=/x\"}", proof);
	g_assert_null(axis_portal_wire_read_login(text, strlen(text)));
	g_clear_pointer(&text, g_free);
	text = g_strdup_printf("{\"proof\":\"%s\",\"name\":\"robin\",\"expires_in\":0,\"device\":\"\"}", proof);
	g_assert_null(axis_portal_wire_read_login(text, strlen(text)));
	g_clear_pointer(&text, g_free);
	text = g_strdup("{\"proof\":\"short; HttpOnly\",\"name\":\"robin\",\"expires_in\":60,\"device\":\"\"}");
	g_assert_null(axis_portal_wire_read_login(text, strlen(text)));

	/* A request that was made, and a list of requests. */
#define ASKED "{\"outcome\":\"followed\",\"left_out\":[{\"season\":1,\"why\":\"available\"}," \
	"{\"season\":2,\"why\":\"requested\"}],\"followed\":1," \
	"\"movies\":{\"limit\":5,\"left\":2,\"window_ends\":1},\"seasons\":{\"limit\":10,\"left\":7,\"window_ends\":1}}"
	made = axis_portal_wire_read_asked(ASKED, strlen(ASKED));
	g_assert_nonnull(made);
	g_assert_cmpint(made->outcome, ==, AXIS_PORTAL_OUTCOME_FOLLOWED);
	g_assert_cmpuint(made->left_available->len, ==, 1);
	g_assert_cmpuint(made->left_requested->len, ==, 1);
#define ASKED_BAD "{\"outcome\":\"approved\",\"left_out\":[],\"followed\":0," \
	"\"movies\":{\"limit\":5,\"left\":2,\"window_ends\":1},\"seasons\":{\"limit\":10,\"left\":7,\"window_ends\":1}}"
	g_assert_null(axis_portal_wire_read_asked(ASKED_BAD, strlen(ASKED_BAD)));
#define REQUESTS(extra) "{\"requests\":[{\"id\":\"reqAAAAAAAAAAAAAAAAAAA\",\"title\":3,\"kind\":\"movie\"," \
	"\"name\":\"x\",\"year\":2019,\"seasons\":[],\"state\":\"failed\",\"cause\":\"download_failed\",\"mine\":\"made\"," \
	"\"asked_at\":1,\"changed_at\":2" extra "}],\"page\":1,\"more\":false}"
	requests = axis_portal_wire_read_requests(REQUESTS(""), strlen(REQUESTS("")), 1);
	g_assert_nonnull(requests);
	g_assert_cmpint(((const AxisPortalRequestInfo *)g_ptr_array_index(requests->requests, 0))->cause, ==,
		AXIS_PORTAL_CAUSE_DOWNLOAD_FAILED);
	g_assert_null(axis_portal_wire_read_requests(REQUESTS(""), strlen(REQUESTS("")), 2));
	g_assert_null(axis_portal_wire_read_requests(REQUESTS(",\"profile\":\"zz\""), strlen(REQUESTS(",\"profile\":\"zz\"")),
		1));
	g_assert_null(axis_portal_wire_read_requests(REQUESTS(",\"have\":1"), strlen(REQUESTS(",\"have\":1")), 1));
	g_assert_null(axis_portal_wire_read_requests(REQUESTS(",\"asked_by\":\"sam\""),
		strlen(REQUESTS(",\"asked_by\":\"sam\"")), 1));

	/* A refusal: the word, never the sentence. */
#define REFUSAL "{\"error\":\"quota\",\"message\":\"<b>zz</b>\",\"wait\":90}"
	g_assert_true(axis_portal_wire_read_refusal(REFUSAL, strlen(REFUSAL), &refusal));
	g_assert_cmpint(refusal.why, ==, AXIS_PORTAL_REFUSED_QUOTA);
	g_assert_cmpint(refusal.wait_s, ==, 90);
	g_assert_true(axis_portal_wire_read_refusal(LIT("{\"error\":\"zz-new\",\"message\":\"x\"}"), &refusal));
	g_assert_cmpint(refusal.why, ==, AXIS_PORTAL_REFUSED_OTHER);
	g_assert_false(axis_portal_wire_read_refusal(LIT("<html>502 Bad Gateway</html>"), &refusal));
	g_assert_false(axis_portal_wire_read_refusal(LIT("{\"error\":\"quota\",\"message\":\"x\",\"wait\":-5}"), &refusal));
	g_assert_cmpint(refusal.wait_s, ==, 0);
	g_assert_true(axis_portal_wire_read_empty(LIT("{}")));
	g_assert_false(axis_portal_wire_read_empty(LIT("{\"ok\":true}")));
	g_assert_false(axis_portal_wire_read_empty(LIT("[]")));
}

/* A path is one of the portal's own, written exactly; a form is pairs
 * within the limits. */
static void
test_form_reader(void)
{
	static const struct
	{
		const gchar     *path;
		AxisPortalRoute  route;
	} paths[] = {
		{ "/", AXIS_PORTAL_ROUTE_HOME },
		{ "/login", AXIS_PORTAL_ROUTE_LOGIN },
		{ "/title/7", AXIS_PORTAL_ROUTE_TITLE },
		{ "/title/999999999999999", AXIS_PORTAL_ROUTE_TITLE },
		{ "/title/7/request", AXIS_PORTAL_ROUTE_ASK },
		{ "/art/12", AXIS_PORTAL_ROUTE_ART },
		{ "/requests/reqAAAAAAAAAAAAAAAAAAA/withdraw", AXIS_PORTAL_ROUTE_WITHDRAW },
		{ "/static/portal.css", AXIS_PORTAL_ROUTE_STYLE },
		{ "/healthz", AXIS_PORTAL_ROUTE_HEALTH },
		{ "", AXIS_PORTAL_ROUTE_NONE },
		{ "//", AXIS_PORTAL_ROUTE_NONE },
		{ "/login/", AXIS_PORTAL_ROUTE_NONE },
		{ "/LOGIN", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/07", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/0", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/-7", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/7a", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/1000000000000000", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/7/", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/7/request/", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/7/../../admin/users", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/7%2frequest", AXIS_PORTAL_ROUTE_NONE },
		{ "/title/%37", AXIS_PORTAL_ROUTE_NONE },
		{ "/art/7.jpg", AXIS_PORTAL_ROUTE_NONE },
		{ "/requests/short/withdraw", AXIS_PORTAL_ROUTE_NONE },
		{ "/requests/reqAAAAAAAAAAAAAAAAAA./withdraw", AXIS_PORTAL_ROUTE_NONE },
		{ "/requests/reqAAAAAAAAAAAAAAAAAAA/approve", AXIS_PORTAL_ROUTE_NONE },
		{ "/static/axis.css", AXIS_PORTAL_ROUTE_NONE },
		{ "/static/../login", AXIS_PORTAL_ROUTE_NONE },
		{ "/admin/users", AXIS_PORTAL_ROUTE_NONE },
		{ "/api/v1/portal/requests", AXIS_PORTAL_ROUTE_NONE }
	};
	static const gchar *const bad_forms[] = {
		"a", "=1", "a=1&", "&a=1", "a=1&&b=2", "A=1", "a-b=1", "a=%", "a=%4", "a=%zz", "a=%00", "a=%ff", "a%3db=1",
		"a=1;b=2&", NULL
	};
	g_autoptr(AxisPortalForm) form = NULL;
	g_autofree gchar *long_value = g_strnfill(AXIS_PORTAL_FORM_VALUE_MAX + 1, 'v');
	g_autofree gchar *long_form = g_strconcat("a=", long_value, NULL);
	g_autofree gchar *no_request = NULL;
	gint picked[100];
	guint n_picked = 0;
	AxisPortalAsk ask;
	gint64 number = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(paths); i++) {
		g_autofree gchar *request_id = NULL;
		gint64 id = 0;

		if (axis_portal_route_parse(paths[i].path, &id, &request_id) != paths[i].route)
			g_error("the path %s is read as another route", paths[i].path);
	}
	g_assert_cmpint(axis_portal_route_parse(NULL, &number, &no_request), ==, AXIS_PORTAL_ROUTE_NONE);

	for (i = 0; bad_forms[i] != NULL; i++) {
		AxisPortalForm *read = axis_portal_form_parse(bad_forms[i], strlen(bad_forms[i]), 4096);

		if (read != NULL)
			g_error("the form %s was read", bad_forms[i]);
	}
	g_assert_null(axis_portal_form_parse(long_form, strlen(long_form), AXIS_PORTAL_FORM_MAX_BYTES));
	g_assert_null(axis_portal_form_parse(LIT("a=1&b=2"), 5));

	form = axis_portal_form_parse(LIT("q=lantern+orchard%21&kind=movie&season=1&season=2&x="), 4096);
	g_assert_nonnull(form);
	g_assert_cmpstr(axis_portal_form_get(form, "q"), ==, "lantern orchard!");
	g_assert_cmpstr(axis_portal_form_get(form, "x"), ==, "");
	/* A field given twice has no value. */
	g_assert_null(axis_portal_form_get(form, "season"));
	g_assert_cmpuint(axis_portal_form_count(form, "season"), ==, 2);
	g_assert_null(axis_portal_form_get(form, "absent"));
	g_clear_pointer(&form, axis_portal_form_free);

	g_assert_true(axis_portal_form_number("7", 1, 9, &number));
	g_assert_false(axis_portal_form_number("07", 1, 9, &number));
	g_assert_false(axis_portal_form_number("+7", 1, 9, &number));
	g_assert_false(axis_portal_form_number(" 7", 1, 9, &number));
	g_assert_false(axis_portal_form_number("7 ", 1, 9, &number));
	g_assert_false(axis_portal_form_number("10", 1, 9, &number));
	g_assert_false(axis_portal_form_number("9999999999999999999999", 1, G_MAXINT64, &number));
	g_assert_false(axis_portal_form_number("", 1, 9, &number));

	form = axis_portal_form_parse(LIT("what=picked&season=3&season=0"), 4096);
	g_assert_true(axis_portal_form_seasons(form, &ask, picked, &n_picked));
	g_assert_cmpint(ask, ==, AXIS_PORTAL_ASK_PICKED);
	g_assert_cmpuint(n_picked, ==, 2);
	g_clear_pointer(&form, axis_portal_form_free);
	form = axis_portal_form_parse(LIT("what=picked"), 4096);
	g_assert_false(axis_portal_form_seasons(form, &ask, picked, &n_picked));
	g_clear_pointer(&form, axis_portal_form_free);
	form = axis_portal_form_parse(LIT("what=picked&season=3&season=3"), 4096);
	g_assert_false(axis_portal_form_seasons(form, &ask, picked, &n_picked));
	g_clear_pointer(&form, axis_portal_form_free);
	form = axis_portal_form_parse(LIT("what=picked&season=1e1"), 4096);
	g_assert_false(axis_portal_form_seasons(form, &ask, picked, &n_picked));
	g_clear_pointer(&form, axis_portal_form_free);
	form = axis_portal_form_parse(LIT("what=episode&season=3"), 4096);
	g_assert_false(axis_portal_form_seasons(form, &ask, picked, &n_picked));
	g_clear_pointer(&form, axis_portal_form_free);
	form = axis_portal_form_parse(LIT("what=latest&season=3"), 4096);
	g_assert_true(axis_portal_form_seasons(form, &ask, picked, &n_picked));
	g_assert_cmpint(ask, ==, AXIS_PORTAL_ASK_LATEST);
	g_assert_cmpuint(n_picked, ==, 0);
}

/* ── Pages ──────────────────────────────────────────────────────── */

/* Spec (portal): Without a session -- every route of the portal's table but
 * the login, the stylesheet and the probe sends a client without a session
 * to /login (an htmx request is answered 401), and the daemon is asked
 * nothing. */
static void
test_without_session(Fixture *fx, gconstpointer data)
{
	static const gchar *const hx[] = { "HX-Request", "true", NULL };
	const gchar *const *patterns = axis_portal_route_patterns();
	guint walked = 0;
	guint i;

	for (i = 0; patterns[i] != NULL; i++) {
		gboolean posted = g_str_has_suffix(patterns[i], "/request") || g_str_has_suffix(patterns[i], "/withdraw") ||
			strcmp(patterns[i], "/logout") == 0;
		g_autofree gchar *path = NULL;
		g_auto(GStrv) parts = g_strsplit(patterns[i], "<id>", 2);
		AxisTestReply reply;

		if (strcmp(patterns[i], "/login") == 0 || strcmp(patterns[i], AXIS_PORTAL_STYLE_PATH) == 0 ||
		    strcmp(patterns[i], "/healthz") == 0)
			continue;
		path = parts[1] == NULL ? g_strdup(patterns[i]) : g_strconcat(parts[0],
			g_str_has_prefix(patterns[i], "/requests/") ? "reqAAAAAAAAAAAAAAAAAAA" : "7", parts[1], NULL);
		reply = send_from(fx, posted ? "POST" : "GET", path, NULL, posted ? "_csrf=zz" : NULL, NULL, NULL);
		assert_status(&reply, 303);
		g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
		g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
		axis_test_reply_clear(&reply);

		/* A cookie that is not in the form of a session is none. */
		reply = send_from(fx, posted ? "POST" : "GET", path, AXIS_PORTAL_SESSION_COOKIE "=zz-not-a-proof",
			posted ? "_csrf=zz" : NULL, NULL, NULL);
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);

		reply = send_from(fx, posted ? "POST" : "GET", path, NULL, posted ? "_csrf=zz" : NULL, NULL, hx);
		assert_status(&reply, 401);
		g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "HX-Redirect"), "/login"));
		axis_test_reply_clear(&reply);
		walked++;
	}
	g_assert_cmpuint(walked, ==, 8);
	g_assert_cmpuint(asked(fx, NULL), ==, 0);

	/* The page asked for is where the login goes back to, when it is one. */
	{
		AxisTestReply reply = get(fx, "/title/7", NULL);

		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login?next=%2Ftitle%2F7");
		axis_test_reply_clear(&reply);
		reply = get(fx, "/search?q=lantern", NULL);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login?next=%2Fsearch%3Fq%3Dlantern");
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(asked(fx, NULL), ==, 0);
}

/* Spec (portal): a request with two session cookies is treated as no
 * session (another site on the same domain may have set one of them). */
static void
test_two_session_cookies(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *sam = sealed(fx, fx->sam);
	g_autofree gchar *two = g_strdup_printf("%s; %s", cookie, sam);
	g_autofree gchar *twice = g_strdup_printf("%s; %s", cookie, cookie);
	AxisTestReply reply;
	guint before;

	reply = get(fx, "/requests", cookie);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	before = asked(fx, NULL);

	reply = get(fx, "/requests", two);
	assert_status(&reply, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login?next=%2Frequests");
	axis_test_reply_clear(&reply);
	reply = get(fx, "/requests", twice);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, NULL), ==, before);
}

/* Spec (portal): The probe -- answered only to this machine itself, asking
 * for nobody else, and it says nothing but that the program runs. */
static void
test_probe(Fixture *fx, gconstpointer data)
{
	static const gchar *const forwarded[] = { "X-Forwarded-For", OUTSIDE, NULL };
	AxisTestReply reply;

	reply = axis_test_web_request(fx->session, fx->port, "GET", "/healthz", NULL, NULL, NULL);
	assert_status(&reply, 200);
	g_assert_cmpstr(reply.body, ==, "ok\n");
	axis_test_reply_clear(&reply);
	/* Through the proxy, for someone else: there is no such page.  (A peer
	 * that is not loopback cannot be made here; what stands for it is a
	 * request that a proxy on this machine forwards.) */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/healthz", NULL, forwarded, NULL);
	assert_status(&reply, 404);
	assert_lacks(reply.body, "ok\n");
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/healthz", NULL, NULL, "a=1");
	assert_status(&reply, 405);
	axis_test_reply_clear(&reply);
	/* Found by the review of P3 (M7): a proxy on this machine that sends
	 * only some other header of its kind, or a Host that is not this
	 * machine's, is not this machine asking. */
	{
		static const gchar *const headers[] = {
			"X-Real-IP", "Via", "X-Forwarded-Proto", "X-Forwarded-Host", "Forwarded", NULL
		};
		static const gchar *const values[] = {
			OUTSIDE, "1.1 proxy", "https", "portal.example.net", "for=203.0.113.5", NULL
		};
		static const gchar *const other_host[] = { "Host", "portal.example.net", NULL };
		guint i;

		for (i = 0; headers[i] != NULL; i++) {
			const gchar *one[3];

			one[0] = headers[i];
			one[1] = values[i];
			one[2] = NULL;
			reply = axis_test_web_request(fx->session, fx->port, "GET", "/healthz", NULL, one, NULL);
			assert_status(&reply, 404);
			axis_test_reply_clear(&reply);
		}
		reply = axis_test_web_request(fx->session, fx->port, "GET", "/healthz", NULL, other_host, NULL);
		if (g_strcmp0(soup_message_headers_get_one(soup_message_get_request_headers(reply.message), "Host"),
			      "portal.example.net") == 0)
			assert_status(&reply, 404);
		else
			g_test_message("libsoup sent its own Host: the probe's Host check was not tried");
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(asked(fx, NULL), ==, 0);
}

/* Found by the review of P3 (M3): a listed proxy that sends no usable
 * X-Forwarded-For made every visitor one client, the proxy itself: one
 * login throttle for all, and the proxy's address told to the daemon.  Such
 * a visit is refused, the daemon is not asked, and the log says why. */
static void
test_proxy_says_nobody(Fixture *fx, gconstpointer data)
{
	static const gchar *const only_proto[] = { "X-Forwarded-Proto", "https", NULL };
	static const gchar *const only_proxies[] = { "X-Forwarded-Proto", "https", "X-Forwarded-For", "127.0.0.1", NULL };
	static const gchar *const with_port[] = {
		"X-Forwarded-Proto", "https", "X-Forwarded-For", "203.0.113.5:4711", NULL
	};
	const gchar *const *cases[3];
	guint i;

	cases[0] = only_proto;
	cases[1] = only_proxies;
	cases[2] = with_port;
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		AxisTestReply reply = axis_test_web_request(fx->session, fx->port, "GET", "/login", NULL, cases[i], NULL);

		assert_status(&reply, 503);
		assert_lacks(reply.body, "<form");
		assert_headers(&reply);
		axis_test_reply_clear(&reply);
	}
	assert_has(log_text->str, "refused a visit: the trusted proxy 127.0.0.1 did not say who the client is");
	g_assert_cmpuint(asked(fx, NULL), ==, 0);
	/* The stylesheet and the probe are nobody's. */
	{
		AxisTestReply reply = axis_test_web_request(fx->session, fx->port, "GET", AXIS_PORTAL_STYLE_PATH, NULL,
			only_proto, NULL);

		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
	}
}

/* Every address a page refers to (href, src, action) is a path of the
 * portal's own: it begins with one '/', and holds none of @secrets. */
static void
assert_own_addresses(
	const gchar        *html,
	const gchar *const *secrets
){
	static const gchar *const attributes[] = { " href=\"", " src=\"", " action=\"", NULL };
	guint i;
	guint j;

	for (i = 0; attributes[i] != NULL; i++) {
		const gchar *at = html;

		while ((at = strstr(at, attributes[i])) != NULL) {
			g_autofree gchar *value = NULL;

			at += strlen(attributes[i]);
			value = g_strndup(at, strcspn(at, "\""));
			if (value[0] != '/' || value[1] == '/' || strstr(value, "://") != NULL)
				g_error("a page refers to an address that is not the portal's own: %s", value);
			for (j = 0; secrets != NULL && secrets[j] != NULL; j++) {
				if (strstr(value, secrets[j]) != NULL)
					g_error("an address of a page holds a secret: %s", value);
			}
		}
	}
	assert_lacks(html, "<script");
	assert_lacks(html, " style=");
	assert_lacks(html, "http://");
	assert_lacks(html, "https://");
}

/* The security headers of the engine's interface, with the portal's own
 * content security policy, on any answer. */
static void
assert_headers(AxisTestReply *reply)
{
	g_assert_cmpstr(axis_test_reply_header(reply, "Content-Security-Policy"), ==, AXIS_PORTAL_CSP);
	g_assert_cmpstr(axis_test_reply_header(reply, "X-Content-Type-Options"), ==, "nosniff");
	g_assert_cmpstr(axis_test_reply_header(reply, "X-Frame-Options"), ==, "DENY");
	g_assert_cmpstr(axis_test_reply_header(reply, "Referrer-Policy"), ==, "same-origin");
	g_assert_nonnull(axis_test_reply_header(reply, "Cache-Control"));
}

/* Spec (portal): Headers -- every answer carries the engine interface's
 * content security policy and security headers (the list both take from
 * web/axis-web-headers.c), and a page refers to no address but the portal's
 * own. */
static void
test_headers(Fixture *fx, gconstpointer data)
{
	static const gchar *const paths[] = {
		"/", "/search", "/search?q=lantern", "/title/7", "/title/3", "/requests", "/login", "/art/3", "/title/99",
		"/nothing-here", "/static/portal.css", "/admin/users", NULL
	};
	static const gchar png[] = "\x89PNG\r\n\x1a\nzz-a-made-up-picture";
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	const gchar *const *names = axis_web_security_header_names();
	Answer *poster = g_new0(Answer, 1);
	AxisTestReply reply;
	guint i;

	g_assert_cmpuint(g_strv_length((gchar **)names), ==, 4);
	poster->status = 200;
	poster->type = g_strdup("image/png");
	poster->body = g_memdup2(png, sizeof(png));
	poster->length = sizeof(png) - 1;
	g_hash_table_replace(fx->script, g_strdup("art"), poster);

	for (i = 0; paths[i] != NULL; i++) {
		reply = get(fx, paths[i], cookie);
		assert_headers(&reply);
		/* (M5) The visit came over HTTPS: the browser is told to keep to it. */
		if (g_strcmp0(axis_test_reply_header(&reply, "Strict-Transport-Security"), AXIS_PORTAL_HSTS) != 0)
			g_error("%s has no Strict-Transport-Security", paths[i]);
		if (g_str_has_prefix(reply.body, "<!DOCTYPE html>"))
			assert_own_addresses(reply.body, NULL);
		if (strcmp(paths[i], "/art/3") == 0) {
			assert_status(&reply, 200);
			g_assert_cmpstr(axis_test_reply_header(&reply, "Content-Type"), ==, "image/png");
			g_assert_cmpstr(axis_test_reply_header(&reply, "Cache-Control"), ==, "private, max-age=600");
		} else if (strcmp(paths[i], "/static/portal.css") == 0) {
			assert_status(&reply, 200);
			g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Content-Type"), "text/css"));
		} else {
			g_assert_cmpstr(axis_test_reply_header(&reply, "Cache-Control"), ==, "no-store");
		}
		axis_test_reply_clear(&reply);
	}
	/* A redirect, a refusal, a post that was answered, a method that is
	 * not taken. */
	reply = get(fx, "/requests", NULL);
	assert_status(&reply, 303);
	assert_headers(&reply);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/title/3/request", cookie, "kind=movie");
	assert_status(&reply, 403);
	assert_headers(&reply);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 200);
	assert_headers(&reply);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Strict-Transport-Security"), ==, AXIS_PORTAL_HSTS);
	assert_own_addresses(reply.body, NULL);
	axis_test_reply_clear(&reply);
	reply = send_from(fx, "DELETE", "/requests", cookie, NULL, NULL, NULL);
	assert_status(&reply, 405);
	assert_headers(&reply);
	axis_test_reply_clear(&reply);
	/* (M6) The portal's policy is its own, narrower than the engine's: no
	 * script from anywhere, nothing but its stylesheet and its pictures. */
	g_assert_cmpstr(AXIS_PORTAL_CSP, !=, AXIS_WEB_CSP);
	assert_has(AXIS_PORTAL_CSP, "default-src 'none'");
	assert_lacks(AXIS_PORTAL_CSP, "script-src");
	/* (M15) The refusals by the limits (a body too large) and the
	 * portal's own (a body that is not a form) carry them too; a refusal
	 * for too many searches, over plain HTTP and a daemon that is away are
	 * asserted where they are tried. */
	{
		g_autofree gchar *large = g_strnfill(65 * 1024, 'a');

		reply = post(fx, "/title/3/request", cookie, large);
		assert_status(&reply, 413);
		assert_headers(&reply);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(post_typed(fx, "/title/3/request", cookie, "application/json", "{}"), ==, 415);
	}

	/* What is served as a picture is one: an answer of the daemon that is
	 * not, is not served. */
	g_free(poster->body);
	poster->body = g_strdup("<html><script>zz</script></html>");
	poster->length = strlen(poster->body);
	reply = get(fx, "/art/4", cookie);
	assert_status(&reply, 404);
	assert_lacks(reply.body, "zz</script>");
	axis_test_reply_clear(&reply);
	/* The poster that was served is kept, up to the fixed size, and given
	 * again to the person the daemon knew a moment ago without asking. */
	g_assert_cmpuint(axis_portal_app_get_poster_bytes(fx->app), ==, sizeof(png) - 1);
	i = asked(fx, "art");
	reply = get(fx, "/art/3", cookie);
	assert_status(&reply, 200);
	g_assert_cmpuint(reply.length, ==, sizeof(png) - 1);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, "art"), ==, i);
	/* Not to someone the daemon does not know: it is asked who that is. */
	{
		g_autofree gchar *stranger = fill43("zzProofNobody");
		g_autofree gchar *other = sealed(fx, stranger);

		reply = get(fx, "/art/3", other);
		assert_status(&reply, 303);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login");
		axis_test_reply_clear(&reply);
	}
}

/* Spec (portal): A title's state and the form -- a series that is partly
 * available shows its state and each season's, offers only the seasons that
 * can be requested, and says what is left of the quota for seasons. */
static void
test_state_and_form(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *sam = sealed(fx, fx->sam);
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	const Call *call;

	reply = get(fx, "/title/7", cookie);
	assert_status(&reply, 200);
	assert_has(reply.body, "Harbour of Glass");
	assert_has(reply.body, "Partly in the library");
	assert_has(reply.body, "Asked by sam");
	assert_has(reply.body, "In the library");
	assert_has(reply.body, "Requested");
	assert_has(reply.body, "Declined");
	/* Boxes for the seasons that can be asked for (0, 3 and 4), and for no
	 * other. */
	assert_has(reply.body, "name=\"season\" value=\"0\"");
	assert_has(reply.body, "name=\"season\" value=\"3\"");
	assert_has(reply.body, "name=\"season\" value=\"4\"");
	assert_lacks(reply.body, "name=\"season\" value=\"1\"");
	assert_lacks(reply.body, "name=\"season\" value=\"2\"");
	assert_has(reply.body, "value=\"all\"");
	assert_has(reply.body, "value=\"first\"");
	assert_has(reply.body, "value=\"latest\"");
	assert_has(reply.body, "value=\"picked\"");
	assert_has(reply.body, "You can ask for 7 more of 10 seasons");
	assert_has(reply.body, "Seasons: 7 of 10 left");
	assert_has(reply.body, "action=\"/title/7/request\"");
	csrf = hidden_value(reply.body, "_csrf");
	axis_test_reply_clear(&reply);

	/* The picked seasons go to the daemon as a list of numbers, with the
	 * person's proof and nothing else of the form. */
	form = g_strdup_printf("_csrf=%s&kind=series&what=picked&season=3&season=0", csrf);
	script(fx, "request", 200, "{\"outcome\":\"made\",\"left_out\":[{\"season\":1,\"why\":\"available\"}],"
		"\"followed\":0,\"movies\":{\"limit\":5,\"left\":3,\"window_ends\":1900000000},"
		"\"seasons\":{\"limit\":10,\"left\":5,\"window_ends\":1900000000}}");
	reply = post(fx, "/title/7/request", cookie, form);
	assert_status(&reply, 200);
	assert_has(reply.body, "Your request is made");
	assert_has(reply.body, "Left out, because in the library already: season 1");
	assert_has(reply.body, "Seasons: 5 of 10 left");
	axis_test_reply_clear(&reply);
	call = last_call(fx, "request");
	g_assert_nonnull(call);
	g_assert_true(call->valid);
	assert_has(call->body, "\"id\":7,\"kind\":\"series\",\"seasons\":[3,0]");
	assert_lacks(call->body, "_csrf");
	assert_lacks(call->body, csrf);

	/* A form that chooses nothing, an episode, a folder: refused here. */
	{
		static const gchar *const refused[] = {
			"kind=series", "kind=series&what=picked", "kind=series&what=episode&season=1",
			"kind=series&what=picked&season=1&season=1", "kind=series&what=all&folder=%2Fmedia",
			"kind=series&what=all&profile=4", "kind=movie&what=all", "kind=game", "kind=series&what=all&user=sam",
			"kind=series&what=all&note=please", NULL
		};
		guint before = asked(fx, "request");
		guint i;

		for (i = 0; refused[i] != NULL; i++) {
			g_autofree gchar *bad = g_strdup_printf("_csrf=%s&%s", csrf, refused[i]);

			reply = post(fx, "/title/7/request", cookie, bad);
			assert_status(&reply, 400);
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpuint(asked(fx, "request"), ==, before);
	}

	/* A person whose limit for a kind is none is offered nothing of it. */
	reply = get(fx, "/title/7", sam);
	assert_status(&reply, 200);
	assert_has(reply.body, "You cannot ask for series");
	assert_lacks(reply.body, "name=\"season\"");
	assert_lacks(reply.body, "<form class=\"ask\"");
	axis_test_reply_clear(&reply);

	/* A movie: one button, and what is left of movies. */
	reply = get(fx, "/title/3", cookie);
	assert_status(&reply, 200);
	assert_has(reply.body, "Request this movie");
	assert_has(reply.body, "<img class=\"poster\" src=\"/art/3\"");
	axis_test_reply_clear(&reply);
}

/* Spec (portal): Provider text -- an overview with markup and a quotation
 * mark is shown as text, wherever it is put. */
static void
test_provider_text(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	AxisTestReply reply;

#define EVIL_NAME "Harbour\\\" onmouseover=\\\"zz<img src=x onerror=zz>"
	script(fx, "title", 200, "{\"title\":{\"id\":7,\"kind\":\"movie\",\"name\":\"" EVIL_NAME "\",\"year\":2020,"
		"\"overview\":\"<script>alert('zz')</script> \\\"quoted\\\" & more\",\"has_poster\":true,\"state\":\"open\"}}");
	reply = get(fx, "/title/7", cookie);
	assert_status(&reply, 200);
	assert_lacks(reply.body, "<script>");
	assert_lacks(reply.body, "onerror=zz>");
	assert_lacks(reply.body, "\" onmouseover=\"");
	assert_has(reply.body, "&lt;script&gt;alert(&#39;zz&#39;)&lt;/script&gt; &quot;quoted&quot; &amp; more");
	/* In an attribute too (the picture's alternative text). */
	assert_has(reply.body, "alt=\"Harbour&quot; onmouseover=&quot;zz&lt;img src=x onerror=zz&gt;\"");
	axis_test_reply_clear(&reply);

	/* In a list, and what was typed into the search box. */
	script(fx, "search", 200, "{\"titles\":[{\"id\":7,\"kind\":\"movie\",\"name\":\"" EVIL_NAME "\",\"year\":2020,"
		"\"overview\":\"x\",\"has_poster\":false,\"state\":\"open\"}],\"page\":1,\"more\":true,\"stale\":false}");
	reply = get(fx, "/search?q=%22%3E%3Cscript%3Ezz%3C%2Fscript%3E", cookie);
	assert_status(&reply, 200);
	assert_lacks(reply.body, "<script>");
	assert_lacks(reply.body, "onerror=zz>");
	assert_has(reply.body, "value=\"&quot;&gt;&lt;script&gt;zz&lt;/script&gt;\"");
	/* The link to more results carries the query escaped twice over: for
	 * the address, then for the page. */
	assert_has(reply.body, "href=\"/search?q=%22%3E%3Cscript%3Ezz%3C%2Fscript%3E&amp;kind=any&amp;page=2\"");
	axis_test_reply_clear(&reply);

	/* A reason an administrator wrote, on My requests. */
	script(fx, "requests", 200, "{\"requests\":[{\"id\":\"reqAAAAAAAAAAAAAAAAAAA\",\"title\":3,\"kind\":\"movie\","
		"\"name\":\"<b>bold</b>\",\"year\":2019,\"seasons\":[],\"state\":\"declined\","
		"\"reason\":\"<a href=\\\"https://evil.example.net/\\\">no</a>\",\"mine\":\"followed\","
		"\"asked_at\":1800000000,\"changed_at\":1800000000}],\"page\":1,\"more\":false}");
	reply = get(fx, "/requests", cookie);
	assert_status(&reply, 200);
	assert_lacks(reply.body, "<b>bold</b>");
	assert_lacks(reply.body, "<a href=\"https://evil");
	assert_has(reply.body, "&lt;a href=&quot;https://evil.example.net/&quot;&gt;no&lt;/a&gt;");
	assert_has(reply.body, "you follow this request");
	/* A follower cannot withdraw: no form is offered. */
	assert_lacks(reply.body, "/withdraw");
	axis_test_reply_clear(&reply);

	/* A name that is not an account's is no answer at all: the page is the
	 * plain one. */
	script(fx, "title", 200, "{\"title\":{\"id\":7,\"kind\":\"movie\",\"name\":\"x\",\"year\":2020,\"overview\":\"x\","
		"\"has_poster\":true,\"state\":\"requested\",\"asked_by\":\"<script>zz</script>\"}}");
	reply = get(fx, "/title/7", cookie);
	assert_status(&reply, 503);
	assert_lacks(reply.body, "zz");
	axis_test_reply_clear(&reply);
	/* Nor is a title that is not the one asked for. */
	script(fx, "title", 200, "{\"title\":" TITLE_MOVIE "}");
	reply = get(fx, "/title/7", cookie);
	assert_status(&reply, 503);
	axis_test_reply_clear(&reply);
}

/* Spec (portal): A failure in plain words -- a request that failed because
 * a download failed says so on "My requests". */
static void
test_failure_words(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	AxisTestReply reply;

	script(fx, "requests", 200, "{\"requests\":["
		"{\"id\":\"reqAAAAAAAAAAAAAAAAAAA\",\"title\":3,\"kind\":\"movie\",\"name\":\"The Lantern Orchard\","
		"\"year\":2019,\"seasons\":[],\"state\":\"failed\",\"cause\":\"download_failed\",\"mine\":\"made\","
		"\"asked_at\":1800000000,\"changed_at\":1800000000},"
		"{\"id\":\"reqBBBBBBBBBBBBBBBBBBB\",\"title\":7,\"kind\":\"series\",\"name\":\"Harbour of Glass\","
		"\"year\":2020,\"seasons\":[1,2],\"state\":\"partly_available\",\"have\":5,\"of\":16,\"mine\":\"made\","
		"\"asked_at\":1800000000,\"changed_at\":1800000000},"
		"{\"id\":\"reqCCCCCCCCCCCCCCCCCCC\",\"title\":9,\"kind\":\"movie\",\"name\":\"Saltmarsh Ledger\","
		"\"year\":2015,\"seasons\":[],\"state\":\"waiting\",\"mine\":\"made\","
		"\"asked_at\":1800000000,\"changed_at\":1800000000}],\"page\":1,\"more\":true}");
	reply = get(fx, "/requests", cookie);
	assert_status(&reply, 200);
	assert_has(reply.body, "A download failed. Axis is looking again.");
	assert_has(reply.body, "5 of 16 episodes");
	assert_has(reply.body, "Series, seasons 1 and 2");
	assert_has(reply.body, "Waiting for approval");
	/* Only the waiting one can be withdrawn. */
	assert_has(reply.body, "action=\"/requests/reqCCCCCCCCCCCCCCCCCCC/withdraw\"");
	assert_lacks(reply.body, "action=\"/requests/reqAAAAAAAAAAAAAAAAAAA/withdraw\"");
	assert_has(reply.body, "href=\"/requests?page=2\"");
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(axis_portal_view_cause_words(AXIS_PORTAL_CAUSE_DOWNLOAD_FAILED), ==,
		"A download failed. Axis is looking again.");
}

/* Spec (portal): The engine's paths -- the portal answers a path it does
 * not serve with its own 404 and passes nothing on. */
static void
test_engine_paths(Fixture *fx, gconstpointer data)
{
	static const gchar *const paths[] = {
		"/admin/users", "/films", "/attention", "/api/v1/worker/claim", "/api/v1/portal/requests",
		"/api/v1/portal/hello", "/setup", "/static/axis.css", "/events", "/metrics", "/livez", "//login", "/login/",
		"/LOGIN", "/title/07", "/title/7/request/x", "/art/7.jpg", "/requests/all",
		"/requests/reqAAAAAAAAAAAAAAAAAAA/approve", "/title/7;x", "/title/7%20", "/login%3Fx", NULL
	};
	/* Paths that libsoup may answer itself before the portal sees them (an
	 * encoded slash, dot segments): whoever answers, it is not a page, and
	 * the daemon is not asked. */
	static const gchar *const odd[] = {
		"/title/7/../../admin/users", "/title/7%2F..%2Fadmin", "/%2e%2e/admin", "/static/..%2f..%2fadmin",
		"/title/7%00", NULL
	};
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s", csrf);
	guint before = asked(fx, NULL);
	AxisTestReply reply;
	guint i;

	for (i = 0; paths[i] != NULL; i++) {
		reply = get(fx, paths[i], cookie);
		if (reply.status != 404)
			g_error("GET %s is answered %u", paths[i], reply.status);
		assert_has(reply.body, "There is no page at this address.");
		axis_test_reply_clear(&reply);
		reply = post(fx, paths[i], cookie, form);
		if (reply.status != 404)
			g_error("POST %s is answered %u", paths[i], reply.status);
		axis_test_reply_clear(&reply);
		/* Without a session too: a path that is not one is not one. */
		reply = get(fx, paths[i], NULL);
		if (reply.status != 404)
			g_error("GET %s without a session is answered %u", paths[i], reply.status);
		axis_test_reply_clear(&reply);
	}
	for (i = 0; odd[i] != NULL; i++) {
		reply = get(fx, odd[i], cookie);
		if (reply.status != 404 && reply.status != 400)
			g_error("GET %s is answered %u", odd[i], reply.status);
		assert_lacks(reply.body, "Harbour of Glass");
		axis_test_reply_clear(&reply);
		reply = post(fx, odd[i], cookie, form);
		if (reply.status != 404 && reply.status != 400)
			g_error("POST %s is answered %u", odd[i], reply.status);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(asked(fx, NULL), ==, before);

	/* An escape in a path is undone by libsoup before the portal sees it (a
	 * known limit): the path is then the route it spells, with every check
	 * of that route, and not a way around one. */
	reply = get(fx, "/title/7%2Frequest", cookie);
	assert_status(&reply, 405);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/title/7%2Frequest", cookie, "kind=movie");
	assert_status(&reply, 403);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/title/7%2Frequest", NULL, form);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	reply = get(fx, "/log%69n", NULL);
	assert_status(&reply, 200);
	assert_has(reply.body, "name=\"password\"");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, NULL), ==, before);
}

/* Spec (portal): Headers are not passed on -- no request to the daemon
 * carries a browser's Authorization, forwarding, Host or Cookie values, and
 * the daemon's address is the configured one. */
static void
test_headers_not_passed_on(Fixture *fx, gconstpointer data)
{
	static const gchar *const evil[] = {
		"Authorization", "Bearer zz-browser-token", "X-Evil", "zz-evil-header", "X-Forwarded-Host", "zz-evil.example.net",
		"X-Real-IP", "198.51.100.77", "Forwarded", "for=198.51.100.78;host=zz-evil", "Accept-Language", "zz-language",
		"Referer", "https://zz-evil.example.net/", "X-Original-URL", "/admin/users", "X-Rewrite-URL", "/admin/users",
		NULL
	};
	static const gchar *const allowed[] = {
		"Host", "Authorization", "Accept", "Content-Type", "Content-Length", "User-Agent", "Connection",
		"Accept-Encoding", NULL
	};
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *with = g_strdup_printf("%s; zz_other=zz-cookie-value", cookie);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	g_autofree gchar *host = g_strdup_printf("Host: 127.0.0.1:%u\n", (guint)fx->daemon_port);
	g_autofree gchar *bearer = g_strdup_printf("Authorization: Bearer %s\n", fx->token);
	AxisTestReply reply;
	guint i;
	guint j;

	reply = send_from(fx, "GET", "/title/7?zz_query=zz-in-the-address", with, NULL, NULL, evil);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = send_from(fx, "GET", "/search?q=lantern&zz_field=zz-extra", with, NULL, NULL, evil);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = send_from(fx, "GET", "/?kind=series&list=zz-list", with, NULL, NULL, evil);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = send_from(fx, "POST", "/title/3/request", with, form, NULL, evil);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);

	g_assert_cmpuint(fx->calls->len, >, 8);
	for (i = 0; i < fx->calls->len; i++) {
		const Call *call = g_ptr_array_index(fx->calls, i);
		g_autofree gchar *line = g_strdup_printf("POST " AXIS_PORTAL_API_PREFIX "%s\n", call->op);
		g_auto(GStrv) lines = g_strsplit(call->headers, "\n", -1);
		AxisPortalOp op;

		/* One of the eleven, by POST, with no query; the portal's own token
		 * and the configured host. */
		g_assert_true(axis_portal_op_from_name(call->op, &op));
		g_assert_true(g_str_has_prefix(call->headers, line));
		assert_has(call->headers, host);
		assert_has(call->headers, bearer);
		assert_has(call->headers, "Content-Type: application/json\n");
		assert_lacks(call->headers, "zz-");
		assert_lacks(call->headers, "zz_");
		assert_lacks(call->headers, "198.51.100.7");
		assert_lacks(call->headers, OUTSIDE);
		assert_lacks(call->headers, "Cookie");
		assert_lacks(call->headers, "admin");
		for (j = 1; lines[j] != NULL && lines[j][0] != '\0'; j++) {
			g_autofree gchar *name = g_strndup(lines[j], strcspn(lines[j], ":"));

			if (!g_strv_contains(allowed, name))
				g_error("the daemon was sent the header %s", lines[j]);
		}
		/* The body: what the daemon's own reader takes whole, with nothing
		 * of the browser's but through the operation's members. */
		g_assert_true(call->valid);
		assert_lacks(call->body, "zz-");
		assert_lacks(call->body, "zz_");
		assert_lacks(call->body, csrf);
		assert_lacks(call->body, fx->token);
	}
	/* The client's address is in the body of a login only, and it is the
	 * one the trusted proxy reported. */
	assert_has(last_call(fx, "login")->body, "\"client_address\":\"" OUTSIDE "\"");
	/* A list the daemon did not name is not sent: the first it offers is. */
	assert_has(last_call(fx, "browse")->body, "\"list\":\"popular\",\"kind\":\"series\"");
}

/* ── Sessions, cookies and forms ────────────────────────────────── */

/* Spec (portal): The cookie -- after a login the session cookie has the
 * __Host- prefix, HttpOnly, Secure, SameSite=Lax and Path=/. */
static void
test_cookie(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *device = NULL;
	g_autofree gchar *form_line = NULL;
	g_autofree gchar *value = axis_portal_session_cookie(fx->token, fx->robin);
	g_autofree gchar *expected = g_strdup_printf("%s=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax; Secure",
		AXIS_PORTAL_SESSION_COOKIE, value);
	AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, NULL, NULL, &cookie);

	assert_status(&reply, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/");
	line = set_cookie_line(&reply, AXIS_PORTAL_SESSION_COOKIE);
	g_assert_cmpstr(line, ==, expected);
	g_assert_true(g_str_has_prefix(line, "__Host-"));
	/* The value is the proof and its seal: a keyed hash under a key that
	 * only the portal's token gives, which is not the token itself. */
	g_assert_cmpuint(strlen(value), ==, AXIS_PORTAL_WIRE_SESSION_COOKIE_LEN);
	g_assert_true(g_str_has_prefix(value, fx->robin));
	assert_lacks(value, fx->token);
	/* The device value: kept by the browser, never sent to another site. */
	device = set_cookie_line(&reply, AXIS_PORTAL_DEVICE_COOKIE);
	g_assert_cmpstr(device, ==, AXIS_PORTAL_DEVICE_COOKIE "=zzdev1.robin-device-value; Path=/; Max-Age=7776000; "
		"HttpOnly; SameSite=Strict; Secure");
	/* The pre-login form's cookie is spent. */
	form_line = set_cookie_line(&reply, AXIS_PORTAL_FORM_COOKIE);
	g_assert_cmpstr(form_line, ==, AXIS_PORTAL_FORM_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict; Secure");
	g_assert_cmpuint(reply.length, ==, 0);
	axis_test_reply_clear(&reply);

	/* The form's own cookie: short-lived, Strict. */
	reply = get(fx, "/login", NULL);
	g_clear_pointer(&line, g_free);
	line = set_cookie_line(&reply, AXIS_PORTAL_FORM_COOKIE);
	g_assert_true(g_str_has_suffix(line, "; Path=/; Max-Age=1800; HttpOnly; SameSite=Strict; Secure"));
	axis_test_reply_clear(&reply);

	/* A device value the browser kept goes back with its next login, as it
	 * is; one that is not in the form of one is not sent at all. */
	{
		g_autofree gchar *form_cookie = NULL;
		g_autofree gchar *token = NULL;
		g_autofree gchar *cookies = NULL;
		g_autofree gchar *form = NULL;

		reply = get(fx, "/login", NULL);
		form_cookie = set_cookie(&reply, AXIS_PORTAL_FORM_COOKIE);
		token = hidden_value(reply.body, "_csrf");
		axis_test_reply_clear(&reply);
		form = g_strdup_printf("_csrf=%s&username=robin&password=" ROBIN_PASSWORD, token);
		cookies = g_strdup_printf("%s; %s=zzdev1.robin-device-value", form_cookie, AXIS_PORTAL_DEVICE_COOKIE);
		reply = post(fx, "/login", cookies, form);
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
		assert_has(last_call(fx, "login")->body, "\"device\":\"zzdev1.robin-device-value\"");
		g_assert_true(last_call(fx, "login")->valid);
		g_clear_pointer(&cookies, g_free);
		cookies = g_strdup_printf("%s; %s=\"zz device\"", form_cookie, AXIS_PORTAL_DEVICE_COOKIE);
		reply = post(fx, "/login", cookies, form);
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
		assert_lacks(last_call(fx, "login")->body, "device");
	}
}

/* Spec (portal): A post without the token -- a logged-in person's post
 * without the CSRF token, with another session's token, or with an Origin
 * of another site is answered 403, and the daemon is not asked to make a
 * request. */
static void
test_post_without_token(Fixture *fx, gconstpointer data)
{
	static const gchar *const other_origin[] = { "Origin", "https://evil.example.net", NULL };
	static const gchar *const cross_site[] = { "Sec-Fetch-Site", "cross-site", NULL };
	static const gchar *const same_site[] = { "Sec-Fetch-Site", "same-site", NULL };
	static const gchar *const own_origin[] = {
		"Origin", "https://portal.example.net", "X-Forwarded-Host", "portal.example.net", "Sec-Fetch-Site",
		"same-origin", NULL
	};
	static const gchar *const posts[] = {
		"/title/3/request", "/requests/reqAAAAAAAAAAAAAAAAAAA/withdraw", "/logout", NULL
	};
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *sam_cookie = sealed(fx, fx->sam);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *sam_csrf = csrf_of(fx, sam_cookie);
	g_autofree gchar *good = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	g_autofree gchar *anothers = g_strdup_printf("_csrf=%s&kind=movie", sam_csrf);
	g_autofree gchar *the_proof = g_strdup_printf("_csrf=%s&kind=movie", fx->robin);
	g_autofree gchar *twice = g_strdup_printf("_csrf=%s&_csrf=%s&kind=movie", csrf, csrf);
	AxisTestReply reply;
	guint before;
	guint i;

	g_assert_cmpstr(csrf, !=, sam_csrf);
	g_assert_cmpuint(strlen(csrf), ==, 43);
	before = asked(fx, NULL);

	for (i = 0; posts[i] != NULL; i++) {
		reply = post(fx, posts[i], cookie, "kind=movie");
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = post(fx, posts[i], cookie, anothers);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		/* The cookie's own value is not the token; nor is the token twice. */
		reply = post(fx, posts[i], cookie, the_proof);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = post(fx, posts[i], cookie, twice);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		/* The right token, from another site. */
		reply = send_from(fx, "POST", posts[i], cookie, good, NULL, other_origin);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = send_from(fx, "POST", posts[i], cookie, good, NULL, cross_site);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = send_from(fx, "POST", posts[i], cookie, good, NULL, same_site);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
	}
	/* Found by the review of P3 (M4): a proxy that rewrites Host made every
	 * post "from another site" and nothing said why.  The log says what was
	 * compared. */
	assert_has(log_text->str, "refused a post from " OUTSIDE " as from another site: its Origin is evil.example.net "
		"and the host is 127.0.0.1:");
	assert_has(log_text->str, "a proxy that rewrites Host must send X-Forwarded-Host");
	assert_has(log_text->str, "the browser says Sec-Fetch-Site: cross-site");
	assert_has(log_text->str, "the browser says Sec-Fetch-Site: same-site");
	/* The token in the address is not the token (and a post has no query). */
	{
		g_autofree gchar *path = g_strdup_printf("/title/3/request?_csrf=%s", csrf);

		reply = post(fx, path, cookie, "kind=movie");
		assert_status(&reply, 400);
		axis_test_reply_clear(&reply);
	}
	/* A body that is not an urlencoded form is not read as one, whatever it
	 * holds: the token in JSON, in a multipart form, in plain text. */
	{
		g_autofree gchar *json = g_strdup_printf("{\"_csrf\":\"%s\",\"kind\":\"movie\"}", csrf);
		g_autofree gchar *multipart = g_strdup_printf("--zz\r\nContent-Disposition: form-data; name=\"_csrf\"\r\n\r\n%s"
			"\r\n--zz\r\nContent-Disposition: form-data; name=\"kind\"\r\n\r\nmovie\r\n--zz--\r\n", csrf);

		g_assert_cmpuint(post_typed(fx, "/title/3/request", cookie, "application/json", json), ==, 415);
		g_assert_cmpuint(post_typed(fx, "/title/3/request", cookie, "multipart/form-data; boundary=zz", multipart), ==,
			415);
		g_assert_cmpuint(post_typed(fx, "/title/3/request", cookie, "text/plain", good), ==, 415);
		g_assert_cmpuint(post_typed(fx, "/title/3/request", cookie, "application/x-www-form-urlencoded-not", good), ==,
			415);
	}
	g_assert_cmpuint(asked(fx, NULL), ==, before);

	/* With the token, from the portal's own pages: made. */
	reply = send_from(fx, "POST", "/title/3/request", cookie, good, NULL, own_origin);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, "request"), ==, 1);

	/* The login form: without its token, with the token of another cookie,
	 * without its cookie. */
	{
		g_autofree gchar *first_cookie = NULL;
		g_autofree gchar *first_token = NULL;
		g_autofree gchar *second_cookie = NULL;
		g_autofree gchar *second_token = NULL;
		g_autofree gchar *form = NULL;
		guint logins = asked(fx, "login");

		reply = get(fx, "/login", NULL);
		first_cookie = set_cookie(&reply, AXIS_PORTAL_FORM_COOKIE);
		first_token = hidden_value(reply.body, "_csrf");
		axis_test_reply_clear(&reply);
		reply = get(fx, "/login", NULL);
		second_cookie = set_cookie(&reply, AXIS_PORTAL_FORM_COOKIE);
		second_token = hidden_value(reply.body, "_csrf");
		axis_test_reply_clear(&reply);
		g_assert_cmpstr(first_cookie, !=, second_cookie);
		g_assert_cmpstr(first_token, !=, second_token);
		/* The form's token is not the cookie's value, nor made from it
		 * alone: it needs a key only the portal has. */
		assert_lacks(first_cookie, first_token);

		reply = post(fx, "/login", first_cookie, "username=robin&password=" ROBIN_PASSWORD);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		form = g_strdup_printf("_csrf=%s&username=robin&password=" ROBIN_PASSWORD, second_token);
		reply = post(fx, "/login", first_cookie, form);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = post(fx, "/login", NULL, form);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = send_from(fx, "POST", "/login", second_cookie, form, NULL, other_origin);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, "login"), ==, logins);
		/* A form the same cookie asked for again has the same token. */
		reply = get(fx, "/login", second_cookie);
		g_clear_pointer(&first_token, g_free);
		first_token = hidden_value(reply.body, "_csrf");
		g_assert_cmpstr(first_token, ==, second_token);
		axis_test_reply_clear(&reply);
		reply = post(fx, "/login", second_cookie, form);
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
	}
}

/* Spec (portal): Nothing secret in an address -- no link, form action or
 * redirect a person walks through holds the proof, the CSRF token or the
 * portal's token; and no page holds the proof, the token or a device value
 * at all. */
static void
test_nothing_secret(Fixture *fx, gconstpointer data)
{
	static const gchar *const pages[] = {
		"/", "/?kind=series&list=trending", "/search", "/search?q=lantern&kind=movie", "/title/7", "/title/3",
		"/requests", "/requests?page=2", "/login", "/title/99", NULL
	};
	g_autofree gchar *cookie = NULL;
	g_autofree gchar *csrf = NULL;
	g_autofree gchar *form = NULL;
	const gchar *secrets[5];
	AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, NULL, "/requests", &cookie);
	guint i;

	assert_status(&reply, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/requests");
	axis_test_reply_clear(&reply);
	csrf = csrf_of(fx, cookie);
	secrets[0] = fx->robin;
	secrets[1] = fx->token;
	secrets[2] = csrf;
	secrets[3] = "zzdev1";
	secrets[4] = NULL;

	for (i = 0; pages[i] != NULL; i++) {
		reply = get(fx, pages[i], cookie);
		assert_own_addresses(reply.body, secrets);
		assert_lacks(reply.body, fx->robin);
		assert_lacks(reply.body, fx->token);
		assert_lacks(reply.body, "zzdev1");
		assert_lacks(reply.body, ROBIN_PASSWORD);
		/* The CSRF token: in hidden fields of forms, and nowhere else. */
		{
			g_autofree gchar *field = g_strdup_printf("name=\"_csrf\" value=\"%s\"", csrf);
			g_auto(GStrv) around = g_strsplit(reply.body, field, -1);
			g_autofree gchar *rest = g_strjoinv("", around);

			assert_lacks(rest, csrf);
		}
		g_assert_null(axis_test_reply_header(&reply, "Location"));
		axis_test_reply_clear(&reply);
	}
	/* The answers to posts, and the redirect of a logout. */
	form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 200);
	assert_own_addresses(reply.body, secrets);
	assert_lacks(reply.body, fx->robin);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("_csrf=%s", csrf);
	reply = post(fx, "/requests/reqAAAAAAAAAAAAAAAAAAA/withdraw", cookie, form);
	assert_status(&reply, 200);
	assert_has(reply.body, "The request is withdrawn.");
	assert_own_addresses(reply.body, secrets);
	axis_test_reply_clear(&reply);
	assert_has(last_call(fx, "withdraw")->body, "\"request\":\"reqAAAAAAAAAAAAAAAAAAA\"");
	reply = post(fx, "/logout", cookie, form);
	assert_status(&reply, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login");
	{
		g_autofree gchar *gone = set_cookie_line(&reply, AXIS_PORTAL_SESSION_COOKIE);

		g_assert_cmpstr(gone, ==, AXIS_PORTAL_SESSION_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax; Secure");
	}
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, "logout"), ==, 1);
	assert_has(last_call(fx, "logout")->body, fx->robin);
}

/* Spec (portal): An open redirect -- a `next` that is not a path of the
 * portal is ignored. */
static void
test_open_redirect(Fixture *fx, gconstpointer data)
{
	static const gchar *const ignored[] = {
		"https://evil.example.net/", "//evil.example.net/", "/\\evil.example.net", "http:evil.example.net",
		"/admin/users", "/login", "/logout", "/art/7", "/title/7/request", "/title/7#x", "/api/v1/portal/hello",
		"/static/portal.css", " /requests", "/requests\r\nSet-Cookie: zz=1", "javascript:alert(1)", "", NULL
	};
	static const gchar *const kept[] = { "/requests", "/title/7", "/search?q=lantern&kind=movie", "/?kind=series", NULL };
	guint i;

	for (i = 0; ignored[i] != NULL; i++) {
		AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, NULL, ignored[i], NULL);

		assert_status(&reply, 303);
		if (g_strcmp0(axis_test_reply_header(&reply, "Location"), "/") != 0)
			g_error("next=%s sends to %s", ignored[i], axis_test_reply_header(&reply, "Location"));
		axis_test_reply_clear(&reply);
	}
	for (i = 0; kept[i] != NULL; i++) {
		AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, NULL, kept[i], NULL);

		assert_status(&reply, 303);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, kept[i]);
		axis_test_reply_clear(&reply);
	}
	/* In the login page's own address too: an unsafe one is not put into
	 * the form. */
	{
		AxisTestReply reply = get(fx, "/login?next=https%3A%2F%2Fevil.example.net%2F", NULL);

		assert_status(&reply, 200);
		assert_lacks(reply.body, "evil.example.net");
		assert_lacks(reply.body, "name=\"next\"");
		axis_test_reply_clear(&reply);
		reply = get(fx, "/login?next=%2Ftitle%2F7", NULL);
		assert_has(reply.body, "name=\"next\" value=\"/title/7\"");
		axis_test_reply_clear(&reply);
	}
}

/* Spec (portal): Plain HTTP -- without the override no form is served, the
 * page says that the portal needs HTTPS, and a posted login is refused
 * before the daemon is asked.  And: Forwarded headers from elsewhere -- a
 * request that does not come from a listed trusted proxy has its
 * X-Forwarded-Proto and X-Forwarded-For ignored. */
static void
test_plain_http(Fixture *fx, gconstpointer data)
{
	static const gchar *const forwarded[] = {
		"X-Forwarded-Proto", "https", "X-Forwarded-For", OUTSIDE, "X-Forwarded-Host", "portal.example.net", NULL
	};
	g_autofree gchar *value = axis_portal_session_cookie(fx->token, fx->robin);
	g_autofree gchar *cookie = g_strdup_printf("%s=%s; %s=%s", AXIS_PORTAL_SESSION_COOKIE, value,
		AXIS_PORTAL_SESSION_COOKIE_PLAIN, value);
	AxisTestReply reply;

	reply = get(fx, "/login", NULL);
	assert_status(&reply, 403);
	assert_has(reply.body, "This portal answers over HTTPS only");
	assert_lacks(reply.body, "<form");
	assert_lacks(reply.body, "name=\"password\"");
	g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
	/* (M15) The headers; (M5) no HSTS over plain http; (M12) the page
	 * tells a stranger nothing of the program's settings. */
	assert_headers(&reply);
	g_assert_null(axis_test_reply_header(&reply, "Strict-Transport-Security"));
	assert_lacks(reply.body, "AXIS_PORTAL");
	assert_lacks(reply.body, "proxy");
	axis_test_reply_clear(&reply);
	reply = post(fx, "/login", NULL, "username=robin&password=" ROBIN_PASSWORD);
	assert_status(&reply, 403);
	g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
	axis_test_reply_clear(&reply);
	/* Nobody is trusted to say that it was https. */
	reply = send_from(fx, "GET", "/login", NULL, NULL, NULL, forwarded);
	assert_status(&reply, 403);
	axis_test_reply_clear(&reply);
	reply = send_from(fx, "POST", "/login", NULL, "username=robin&password=" ROBIN_PASSWORD, NULL, forwarded);
	assert_status(&reply, 403);
	axis_test_reply_clear(&reply);
	/* Nor is a page served to a session cookie that came over plain http. */
	reply = send_from(fx, "GET", "/requests", cookie, NULL, NULL, forwarded);
	assert_status(&reply, 403);
	axis_test_reply_clear(&reply);
	/* The stylesheet is nobody's secret. */
	reply = get(fx, AXIS_PORTAL_STYLE_PATH, NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, NULL), ==, 0);
	/* The administrator's sentence is in the log, once. */
	g_assert_cmpuint(count_of(log_text->str, "AXIS_PORTAL_TRUSTED_PROXIES"), ==, 1);
}

/* The test-only override: plain http is served, with cookies that are
 * neither Secure nor prefixed; and forwarding headers from someone who is
 * not a trusted proxy change nothing of who the client is. */
static void
test_insecure_override(Fixture *fx, gconstpointer data)
{
	static const gchar *const forwarded[] = {
		"X-Forwarded-Proto", "https", "X-Forwarded-For", OUTSIDE, "X-Forwarded-Host", "portal.example.net", NULL
	};
	g_autofree gchar *form_cookie = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *value = axis_portal_session_cookie(fx->token, fx->robin);
	g_autofree gchar *expected = g_strdup_printf("%s=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax",
		AXIS_PORTAL_SESSION_COOKIE_PLAIN, value);
	AxisTestReply reply;

	reply = send_from(fx, "GET", "/login", NULL, NULL, NULL, forwarded);
	assert_status(&reply, 200);
	form_cookie = set_cookie(&reply, AXIS_PORTAL_FORM_COOKIE_PLAIN);
	token = hidden_value(reply.body, "_csrf");
	g_assert_nonnull(form_cookie);
	/* The secure names are not used: a browser would refuse them here. */
	g_assert_null(set_cookie_line(&reply, AXIS_PORTAL_FORM_COOKIE));
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("_csrf=%s&username=robin&password=" ROBIN_PASSWORD, token);
	reply = send_from(fx, "POST", "/login", form_cookie, form, NULL, forwarded);
	assert_status(&reply, 303);
	line = set_cookie_line(&reply, AXIS_PORTAL_SESSION_COOKIE_PLAIN);
	g_assert_cmpstr(line, ==, expected);
	/* Plain http: no HSTS. */
	g_assert_null(axis_test_reply_header(&reply, "Strict-Transport-Security"));
	axis_test_reply_clear(&reply);
	/* The address the daemon is told is the peer's own, not the header's. */
	assert_has(last_call(fx, "login")->body, "\"client_address\":\"127.0.0.1\"");
	assert_lacks(last_call(fx, "login")->body, OUTSIDE);
	assert_has(log_text->str, "login: robin from 127.0.0.1");
}

/* ── Failures of the daemon ─────────────────────────────────────── */

/* Spec (portal): The daemon is away -- the pages say that requests are not
 * available now, with 503, and the portal keeps running.  A failure of the
 * daemon is a plain page, never its message. */
static void
test_daemon_away(Fixture *fx, gconstpointer data)
{
	static const gchar *const pages[] = { "/", "/search?q=lantern", "/title/7", "/requests", "/art/3", NULL };
	static const struct
	{
		guint        status;
		const gchar *body;
	} failures[] = {
		{ 500, "{\"error\":\"internal\",\"message\":\"zz-detail: relation meta_title does not exist\"}" },
		{ 503, "{\"error\":\"busy\",\"message\":\"zz-detail: the pool is exhausted\",\"wait\":5}" },
		{ 502, "<html><body>zz-detail: bad gateway from nginx</body></html>" },
		{ 200, "zz-detail: not json at all" },
		{ 200, "{\"zz-detail\":\"an answer of another shape\"}" },
		{ 200, "" },
		{ 401, "{\"error\":\"unauthorized\",\"message\":\"zz-detail: a portal token is required\"}" },
		{ 404, "{\"error\":\"not-found\",\"message\":\"zz-detail: there is no such operation\"}" },
		{ 422, "{\"error\":\"invalid\",\"message\":\"zz-detail: the body has a member twice\"}" },
		{ 409, "{\"error\":\"closed\",\"message\":\"zz-detail: closed by root\"}" },
		{ 429, "{\"error\":\"quota\",\"message\":\"zz-detail: 5 of 5 used by robin\",\"wait\":3600}" },
		{ 302, "{}" }
	};
	static const gchar *const ops[] = { "whoami", "hello", "browse", "search", "title", "requests", "request", NULL };
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;
	guint i;
	guint j;
	guint k;

	/* Whatever the daemon says when it fails, the page says none of it. */
	for (i = 0; i < G_N_ELEMENTS(failures); i++) {
		for (j = 0; ops[j] != NULL; j++) {
			g_hash_table_remove_all(fx->script);
			script(fx, ops[j], failures[i].status, failures[i].body);
			/* What `whoami` said a moment ago is not what is asked about,
			 * and the session's own limits are not what is tested. */
			axis_portal_app_set_time_offset(fx->app, (gint64)(i * 100 + j + 1) * 70 * G_USEC_PER_SEC);
			for (k = 0; pages[k] != NULL; k++) {
				reply = get(fx, pages[k], cookie);
				assert_lacks(reply.body, "zz-detail");
				assert_lacks(reply.body, "meta_title");
				assert_lacks(reply.body, "nginx");
				g_assert_cmpuint(reply.status, !=, 500);
				axis_test_reply_clear(&reply);
			}
			reply = post(fx, "/title/3/request", cookie, form);
			assert_lacks(reply.body, "zz-detail");
			g_assert_cmpuint(reply.status, !=, 500);
			axis_test_reply_clear(&reply);
		}
	}
	g_hash_table_remove_all(fx->script);
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(200000) * G_USEC_PER_SEC);

	/* A status and a plain page for each: not found, a limit with its time,
	 * a title that is closed. */
	script(fx, "title", 404, "{\"error\":\"not-found\",\"message\":\"there is no such thing\"}");
	reply = get(fx, "/title/99", cookie);
	assert_status(&reply, 404);
	assert_has(reply.body, "There is no such title or request.");
	axis_test_reply_clear(&reply);
	g_hash_table_remove_all(fx->script);
	script(fx, "request", 429, "{\"error\":\"portal-limit\",\"message\":\"x\",\"wait\":7200}");
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 429);
	assert_has(reply.body, "The limit of requests for today is reached for everyone. Try again in 2 hours.");
	g_assert_cmpstr(axis_test_reply_header(&reply, "Retry-After"), ==, "7200");
	axis_test_reply_clear(&reply);
	script(fx, "request", 409, "{\"error\":\"closed\",\"message\":\"x\"}");
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 409);
	assert_has(reply.body, "This title is closed to requests.");
	axis_test_reply_clear(&reply);
	g_hash_table_remove_all(fx->script);

	/* The daemon does not answer at all. */
	soup_server_disconnect(fx->daemon);
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(300000) * G_USEC_PER_SEC);
	for (k = 0; pages[k] != NULL; k++) {
		reply = get(fx, pages[k], cookie);
		assert_status(&reply, 503);
		assert_has(reply.body, "Requests are not available at the moment.");
		g_assert_cmpstr(axis_test_reply_header(&reply, "Retry-After"), ==, "30");
		assert_headers(&reply);
		axis_test_reply_clear(&reply);
	}
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 503);
	axis_test_reply_clear(&reply);
	{
		gchar *none = NULL;

		reply = login_from(fx, "robin", ROBIN_PASSWORD, NULL, NULL, &none);
		assert_status(&reply, 503);
		g_assert_null(none);
		g_assert_null(set_cookie_line(&reply, AXIS_PORTAL_SESSION_COOKIE));
		axis_test_reply_clear(&reply);
	}
	/* A logout that the daemon did not hear keeps the cookie: the person
	 * can try again, and is not told that the session has ended. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("_csrf=%s", csrf);
	reply = post(fx, "/logout", cookie, form);
	assert_status(&reply, 503);
	g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
	axis_test_reply_clear(&reply);
	/* Each failure is in the log, without the token. */
	assert_has(log_text->str, "the daemon: it could not be reached");
	assert_lacks(log_text->str, fx->token);

	/* The portal kept running: its own pages are served, and when the
	 * daemon is back, so are the others. */
	reply = get(fx, "/login", NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	if (!soup_server_listen_local(fx->daemon, fx->daemon_port, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
		/* (Another program took the port in the meantime: the test programs
		 * run side by side.  The rest needs the stand-in back on it.) */
		g_test_message("the stand-in could not listen on its port again: %s", error->message);
		return;
	}
	reply = get(fx, "/requests", cookie);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
}

/* A change at the daemon takes effect at once: a proof the daemon no longer
 * knows sends the person to /login at the next request, page or post, and
 * what `whoami` said is dropped. */
static void
test_session_ended(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	AxisTestReply reply;
	guint whoamis;

	reply = get(fx, "/title/7", cookie);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	/* `whoami` is kept for the header, and for 30 seconds only. */
	g_assert_cmpuint(axis_portal_app_get_who_kept(fx->app), ==, 1);
	whoamis = asked(fx, "whoami");
	reply = get(fx, "/title/3", cookie);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, "whoami"), ==, whoamis);
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(31) * G_USEC_PER_SEC);
	reply = get(fx, "/title/3", cookie);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, "whoami"), ==, whoamis + 1);

	/* Disabled (or its password or role changed): the daemon answers 401
	 * for the proof.  The post that was open makes no request. */
	fx->robin_ended = TRUE;
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login");
	{
		g_autofree gchar *gone = set_cookie_line(&reply, AXIS_PORTAL_SESSION_COOKIE);

		g_assert_true(g_str_has_prefix(gone, AXIS_PORTAL_SESSION_COOKIE "=; Path=/; Max-Age=0;"));
	}
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_portal_app_get_who_kept(fx->app), ==, 0);
	reply = get(fx, "/title/3", cookie);
	assert_status(&reply, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/login?next=%2Ftitle%2F3");
	axis_test_reply_clear(&reply);
	reply = get(fx, "/art/3", cookie);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	/* (The stand-in answered `request` with 401 as the daemon does: what is
	 * asserted of the portal is that it showed no page and kept nothing.) */
}

/* ── Limits ─────────────────────────────────────────────────────── */

/* Spec (portal): A flood of logins from one address -- of a hundred wrong
 * logins the daemon is asked for at most the free failures and one more,
 * and the others are answered 429 by the portal. */
static void
test_login_flood(Fixture *fx, gconstpointer data)
{
	guint refused = 0;
	guint failed = 0;
	guint i;

	for (i = 0; i < 100; i++) {
		AxisTestReply reply = login_from(fx, "robin", "a wrong password", "198.51.100.20", NULL, NULL);

		if (reply.status == 429) {
			g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
			assert_has(reply.body, "There were too many failed logins.");
			refused++;
		} else {
			assert_status(&reply, 401);
			assert_has(reply.body, "The username or password is incorrect.");
			failed++;
		}
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(asked(fx, "login"), <=, AXIS_PORTAL_LOGIN_FREE_FAILURES + 1);
	g_assert_cmpuint(failed, ==, asked(fx, "login"));
	g_assert_cmpuint(refused, >=, 100 - AXIS_PORTAL_LOGIN_FREE_FAILURES - 1);
	assert_has(log_text->str, "login: refused 198.51.100.20, which has too many failed logins");

	/* Even the right password, from that address; another address, and
	 * another /64, is asked about as before. */
	{
		gchar *cookie = NULL;
		guint before = asked(fx, "login");
		AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, "198.51.100.20", NULL, &cookie);

		assert_status(&reply, 429);
		g_assert_null(cookie);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, "login"), ==, before);
		reply = login_from(fx, "robin", ROBIN_PASSWORD, "198.51.100.21", NULL, &cookie);
		assert_status(&reply, 303);
		g_free(cookie);
		axis_test_reply_clear(&reply);
	}
	/* An IPv6 address counts by its /64. */
	for (i = 0; i < 40; i++) {
		g_autofree gchar *from = g_strdup_printf("2001:db8:7:7::%x", i + 1);
		AxisTestReply reply = login_from(fx, "robin", "a wrong password", from, NULL, NULL);

		axis_test_reply_clear(&reply);
	}
	{
		AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, "2001:db8:7:7::ffff", NULL, NULL);

		assert_status(&reply, 429);
		axis_test_reply_clear(&reply);
		reply = login_from(fx, "robin", ROBIN_PASSWORD, "2001:db8:7:8::1", NULL, NULL);
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
	}
	/* The delay ends. */
	axis_portal_app_set_time_offset(fx->app, AXIS_PORTAL_THROTTLE_MAX_US + G_USEC_PER_SEC);
	{
		AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, "198.51.100.20", NULL, NULL);

		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
	}
	/* Found by the review of P3 (M2): a login that worked forgets the
	 * address's failed ones, so that a mistake afterwards is not one more
	 * on top of a hundred. */
	for (i = 0; i < 2; i++) {
		AxisTestReply reply = login_from(fx, "robin", "a wrong password", "198.51.100.20", NULL, NULL);

		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
	}
	/* And the daemon's own limit (for a username that a stranger spent) is
	 * not counted against the address: it is asked every time. */
	{
		guint before = asked(fx, "login");

		script(fx, "login", 429, "{\"error\":\"throttled\",\"message\":\"x\",\"wait\":60}");
		for (i = 0; i < AXIS_PORTAL_LOGIN_FREE_FAILURES + 5; i++) {
			AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, "198.51.100.40", NULL, NULL);

			assert_status(&reply, 429);
			assert_has(reply.body, "There were too many failed logins.");
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpuint(asked(fx, "login") - before, ==, AXIS_PORTAL_LOGIN_FREE_FAILURES + 5);
		g_hash_table_remove(fx->script, "login");
	}

	/* A stranger who presents session cookies that the portal sealed and
	 * the daemon does not know (a session that was logged out, say) is
	 * refused here too, after as many, and the daemon is no longer asked. */
	{
		guint before = asked(fx, NULL);
		guint sent = 0;

		for (i = 0; i < 60; i++) {
			g_autofree gchar *made_up = g_strdup_printf("zzNobody%02u", i);
			g_autofree gchar *proof = fill43(made_up);
			g_autofree gchar *cookie = sealed(fx, proof);
			AxisTestReply reply = send_from(fx, "GET", "/requests", cookie, NULL, "198.51.100.90", NULL);

			if (reply.status == 303)
				sent++;
			else
				assert_status(&reply, 429);
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpuint(sent, <=, AXIS_PORTAL_PROOF_FREE_FAILURES + 1);
		g_assert_cmpuint(asked(fx, NULL) - before, <=, AXIS_PORTAL_PROOF_FREE_FAILURES + 1);
	}
}

/* Found by the review of P3 (S1): every cookie in the form of a session
 * cost one call to the daemon, so that strangers at many addresses could
 * spend the token's calls for everyone.  A cookie the portal did not seal is
 * none: it is sent to /login, goes, costs the daemon nothing and is counted
 * against nobody.  A cookie sealed under another token (the portal's token
 * was changed) is none either. */
static void
test_made_up_cookies(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *other_token = fill43("zzAnotherPortalToken");
	g_autofree gchar *under_other = axis_portal_session_cookie(other_token, fx->robin);
	g_autofree gchar *robin_value = axis_portal_session_cookie(fx->token, fx->robin);
	g_autofree gchar *wrong_seal = g_strdup(robin_value);
	g_autofree gchar *bare = g_strdup_printf("%s=%s", AXIS_PORTAL_SESSION_COOKIE, fx->robin);
	gchar *cookies[4];
	guint i;
	guint j;

	wrong_seal[strlen(wrong_seal) - 1] = wrong_seal[strlen(wrong_seal) - 1] == 'A' ? 'B' : 'A';
	cookies[0] = g_strdup(bare);
	cookies[1] = g_strdup_printf("%s=%s", AXIS_PORTAL_SESSION_COOKIE, under_other);
	cookies[2] = g_strdup_printf("%s=%s", AXIS_PORTAL_SESSION_COOKIE, wrong_seal);
	cookies[3] = g_strdup_printf("%s=%s.%s", AXIS_PORTAL_SESSION_COOKIE, fx->robin, fx->robin);
	g_assert_cmpstr(under_other, !=, robin_value);

	/* From one address, far more than the free failures of an address:
	 * none of them reaches the daemon, and the address is not delayed. */
	for (i = 0; i < 40; i++) {
		for (j = 0; j < G_N_ELEMENTS(cookies); j++) {
			AxisTestReply reply = send_from(fx, "GET", "/requests", cookies[j], NULL, "198.51.100.70", NULL);

			assert_status(&reply, 303);
			g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
			/* The browser is told to drop it. */
			{
				g_autofree gchar *gone = set_cookie_line(&reply, AXIS_PORTAL_SESSION_COOKIE);

				g_assert_nonnull(gone);
				g_assert_true(g_str_has_prefix(gone, AXIS_PORTAL_SESSION_COOKIE "=; Path=/; Max-Age=0;"));
			}
			axis_test_reply_clear(&reply);
		}
	}
	g_assert_cmpuint(asked(fx, NULL), ==, 0);
	/* Nor does a logout with such a cookie end anything at the daemon. */
	{
		AxisTestReply reply = post(fx, "/logout", cookies[2], "_csrf=zz");

		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, NULL), ==, 0);
	}
	/* The same proof, sealed with this portal's token, is robin's session:
	 * the address was not delayed. */
	{
		g_autofree gchar *good = sealed(fx, fx->robin);
		AxisTestReply reply = send_from(fx, "GET", "/requests", good, NULL, "198.51.100.70", NULL);

		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
	}
	for (j = 0; j < G_N_ELEMENTS(cookies); j++)
		g_free(cookies[j]);
}

/* Found by the review of P3 (S2): the address throttle of unknown cookies
 * refused every cookie from the address, so one device behind a home's
 * address locked everyone there out.  A session the daemon confirmed lately
 * is not held to it; one it has not confirmed is. */
static void
test_one_device_misbehaves(Fixture *fx, gconstpointer data)
{
	static const gchar *home = "198.51.100.80";
	g_autofree gchar *robin = NULL;
	g_autofree gchar *sam = sealed(fx, fx->sam);
	AxisTestReply reply;
	guint i;

	reply = login_from(fx, "robin", ROBIN_PASSWORD, home, NULL, &robin);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_portal_app_get_confirmed_kept(fx->app), ==, 1);

	/* A device at the same address presents cookies the daemon does not
	 * know until the address is delayed. */
	for (i = 0; i < 40; i++) {
		g_autofree gchar *made_up = g_strdup_printf("zzGuest%02u", i);
		g_autofree gchar *proof = fill43(made_up);
		g_autofree gchar *cookie = sealed(fx, proof);

		reply = send_from(fx, "GET", "/", cookie, NULL, home, NULL);
		axis_test_reply_clear(&reply);
	}
	reply = send_from(fx, "GET", "/", sam, NULL, home, NULL);
	assert_status(&reply, 429);
	axis_test_reply_clear(&reply);

	/* robin, at the same address, is not locked out: a page, a search, a
	 * post, again and again. */
	for (i = 0; i < 5; i++) {
		reply = send_from(fx, "GET", "/requests", robin, NULL, home, NULL);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
		/* (Past `whoami`'s 30 seconds: the confirmation lasts longer.) */
		axis_portal_app_set_time_offset(fx->app, (gint64)(i + 1) * 40 * G_USEC_PER_SEC);
	}
	/* Once the daemon no longer knows robin's proof, the confirmation is
	 * gone with it. */
	fx->robin_ended = TRUE;
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(300) * G_USEC_PER_SEC);
	reply = send_from(fx, "GET", "/requests", robin, NULL, home, NULL);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_portal_app_get_confirmed_kept(fx->app), ==, 0);
	/* And a confirmation runs out: sam, confirmed elsewhere, is served at
	 * the address while it lasts, and is held to the address's count once
	 * it has run out. */
	reply = get(fx, "/requests", sam);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = send_from(fx, "GET", "/requests", sam, NULL, home, NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(300 + AXIS_PORTAL_CONFIRMED_SECONDS + 1) *
		G_USEC_PER_SEC);
	{
		g_autofree gchar *proof = fill43("zzGuestAgain");
		g_autofree gchar *cookie = sealed(fx, proof);

		/* (The device misbehaves again: the address is in a delay.) */
		reply = send_from(fx, "GET", "/", cookie, NULL, home, NULL);
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
	}
	reply = send_from(fx, "GET", "/requests", sam, NULL, home, NULL);
	assert_status(&reply, 429);
	axis_test_reply_clear(&reply);
}

/* Found by the review of P3 (S1): calls for sessions the daemon has not
 * confirmed, and logins, from every address together, are a share of the
 * daemon's calls; a flood of them from many addresses leaves the rest to the
 * sessions it has confirmed. */
static void
test_shared_calls(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *robin = login_robin(fx);
	guint before = asked(fx, NULL);
	gint64 start = g_get_monotonic_time();
	guint refused = 0;
	guint i;

	/* (The portal's clock stands still at @start, so that no call comes
	 * back while the test runs.) */
	for (i = 0; i < AXIS_PORTAL_UNCONFIRMED_A_MINUTE + 60; i++) {
		g_autofree gchar *made_up = g_strdup_printf("zzStranger%03u", i);
		g_autofree gchar *proof = fill43(made_up);
		g_autofree gchar *cookie = sealed(fx, proof);
		g_autofree gchar *from = g_strdup_printf("2001:db8:%x::1", i + 1);
		AxisTestReply reply;

		axis_portal_app_set_time_offset(fx->app, start - g_get_monotonic_time());
		reply = send_from(fx, "GET", "/requests", cookie, NULL, from, NULL);

		if (reply.status == 429) {
			assert_has(reply.body, "There are too many requests at the moment.");
			g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
			refused++;
		} else {
			assert_status(&reply, 303);
		}
		axis_test_reply_clear(&reply);
	}
	/* (login_robin() spent one.) */
	g_assert_cmpuint(asked(fx, NULL) - before, <=, AXIS_PORTAL_UNCONFIRMED_A_MINUTE);
	g_assert_cmpuint(refused, >=, 60);
	assert_has(log_text->str, "that the daemon has not confirmed lately");

	/* robin, whom the daemon confirmed, is served throughout. */
	{
		AxisTestReply reply;

		axis_portal_app_set_time_offset(fx->app, start - g_get_monotonic_time());
		reply = get(fx, "/requests", robin);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
	}
	/* A login spends the same calls: refused here, the daemon not asked. */
	{
		guint logins = asked(fx, "login");
		AxisTestReply reply;

		axis_portal_app_set_time_offset(fx->app, start - g_get_monotonic_time());
		reply = login_from(fx, "robin", ROBIN_PASSWORD, "198.51.100.30", NULL, NULL);
		assert_status(&reply, 429);
		assert_has(reply.body, "There are too many logins at the moment.");
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, "login"), ==, logins);
	}
	/* A minute later there are calls again. */
	axis_portal_app_set_time_offset(fx->app, start - g_get_monotonic_time() + G_GINT64_CONSTANT(61) * G_USEC_PER_SEC);
	{
		AxisTestReply reply = login_from(fx, "robin", ROBIN_PASSWORD, "198.51.100.30", NULL, NULL);

		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
	}
}

/* Spec (portal): A flood of searches -- of 50 searches a session sends
 * within a minute the daemon is asked for at most 10.  The same for request
 * posts. */
static void
test_search_flood(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *sam = sealed(fx, fx->sam);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	AxisTestReply reply;
	guint refused = 0;
	guint i;

	/* Found by the review of P3 (M1): a session is counted once the daemon
	 * has said it is one, so that cookies nobody vouched for take no room in
	 * the counts. */
	{
		g_autofree gchar *proof = fill43("zzNobodySearches");
		g_autofree gchar *nobody = sealed(fx, proof);

		for (i = 0; i < 3; i++) {
			reply = get(fx, "/search?q=lantern", nobody);
			assert_status(&reply, 303);
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpuint(axis_portal_app_get_counted_sessions(fx->app), ==, 0);
		g_assert_cmpuint(asked(fx, "search"), ==, 0);
	}

	for (i = 0; i < 50; i++) {
		g_autofree gchar *path = g_strdup_printf("/search?q=lantern+%u", i);

		reply = get(fx, path, cookie);
		if (reply.status == 429) {
			g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
			assert_headers(&reply);
			/* (M17) A refusal for a person who is logged in does not tell
			 * them to log in. */
			assert_has(reply.body, "Back to the lists");
			assert_lacks(reply.body, "href=\"/login\"");
			refused++;
		} else {
			assert_status(&reply, 200);
		}
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(axis_portal_app_get_counted_sessions(fx->app), ==, 1);
	g_assert_cmpuint(asked(fx, "search"), ==, AXIS_PORTAL_SEARCHES_A_MINUTE_HERE);
	g_assert_cmpuint(refused, ==, 40);
	assert_has(log_text->str, "search: refused a session");
	/* The page without a search asks for none and is not counted; another
	 * session has its own count; and a minute later there is room again. */
	reply = get(fx, "/search", cookie);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = get(fx, "/search?q=lantern", sam);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(61) * G_USEC_PER_SEC);
	reply = get(fx, "/search?q=lantern", cookie);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);

	/* Found by the review of P3 (M13): a search is a GET and the cookie is
	 * Lax, so a link on another site could spend the person's searches.  One
	 * the browser says another site sent is refused; one from a bookmark or
	 * the portal's own pages is not. */
	{
		static const gchar *const cross_site[] = { "Sec-Fetch-Site", "cross-site", NULL };
		static const gchar *const same_site[] = { "Sec-Fetch-Site", "same-site", NULL };
		static const gchar *const typed[] = { "Sec-Fetch-Site", "none", NULL };
		static const gchar *const own[] = { "Sec-Fetch-Site", "same-origin", NULL };
		guint before = asked(fx, "search");

		reply = send_from(fx, "GET", "/search?q=lantern", cookie, NULL, NULL, cross_site);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		reply = send_from(fx, "GET", "/search?q=lantern", cookie, NULL, NULL, same_site);
		assert_status(&reply, 403);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, "search"), ==, before);
		/* The search page itself, without a query, may be linked to. */
		reply = send_from(fx, "GET", "/search", cookie, NULL, NULL, cross_site);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
		reply = send_from(fx, "GET", "/search?q=lantern", cookie, NULL, NULL, typed);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
		reply = send_from(fx, "GET", "/search?q=lantern", cookie, NULL, NULL, own);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, "search"), ==, before + 2);
	}

	/* A search that cannot be one is refused before it is counted or sent. */
	{
		g_autofree gchar *long_query = g_strnfill(101, 'q');
		g_autofree gchar *path = g_strconcat("/search?q=", long_query, NULL);
		guint before = asked(fx, "search");

		reply = get(fx, path, cookie);
		assert_status(&reply, 400);
		axis_test_reply_clear(&reply);
		reply = get(fx, "/search?q=a%0Ab", cookie);
		assert_status(&reply, 400);
		axis_test_reply_clear(&reply);
		reply = get(fx, "/search?q=lantern&page=3", cookie);
		assert_status(&reply, 400);
		axis_test_reply_clear(&reply);
		reply = get(fx, "/search?q=lantern&kind=episode", cookie);
		assert_status(&reply, 400);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(asked(fx, "search"), ==, before);
	}

	refused = 0;
	for (i = 0; i < 50; i++) {
		reply = post(fx, "/title/3/request", cookie, form);
		if (reply.status == 429)
			refused++;
		else
			assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(asked(fx, "request"), ==, AXIS_PORTAL_ASKS_A_MINUTE_HERE);
	g_assert_cmpuint(refused, ==, 40);
}

/* Spec (portal): A large body -- a post of 65 KiB is answered 413 and not
 * read. */
static void
test_large_body(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *padding = g_strnfill(65 * 1024, 'a');
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie&what=%s", csrf, padding);
	g_autofree gchar *fits = g_strnfill(60 * 1024, 'a');
	g_autofree gchar *long_form = g_strdup_printf("_csrf=%s&kind=movie&what=%s", csrf, fits);
	guint before = asked(fx, NULL);
	AxisTestReply reply;

	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 413);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/login", NULL, form);
	assert_status(&reply, 413);
	axis_test_reply_clear(&reply);
	/* One that fits the limit and not a field: refused as a form. */
	reply = post(fx, "/title/3/request", cookie, long_form);
	assert_status(&reply, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(asked(fx, NULL), ==, before);
}

/* ── The log ────────────────────────────────────────────────────── */

/* Spec (portal): Secrets absent from the log -- people log in, fail to log
 * in, search and ask, and no line holds a submitted password, the token, a
 * proof, a device value, a cookie value or a CSRF token.  And: A password
 * typed as a username -- the line for it holds the client address and not
 * that text. */
static void
test_log(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *cookie = login_robin(fx);
	g_autofree gchar *csrf = csrf_of(fx, cookie);
	g_autofree gchar *form = g_strdup_printf("_csrf=%s&kind=movie", csrf);
	g_autofree gchar *withdraw = g_strdup_printf("_csrf=%s", csrf);
	AxisTestReply reply;

	reply = login_from(fx, "zz-hunter2-typed-as-a-name", "zz-a-wrong-password", "198.51.100.44", NULL, NULL);
	assert_status(&reply, 401);
	/* The page does not say the name back either. */
	assert_lacks(reply.body, "zz-hunter2");
	axis_test_reply_clear(&reply);
	reply = login_from(fx, "robin", "zz-another-wrong-password", "198.51.100.45", NULL, NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	reply = get(fx, "/search?q=zz-what-was-searched", cookie);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/title/3/request", cookie, form);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/requests/reqAAAAAAAAAAAAAAAAAAA/withdraw", cookie, withdraw);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/title/3/request", cookie, "kind=movie");
	assert_status(&reply, 403);
	axis_test_reply_clear(&reply);
	script(fx, "title", 500, "{\"error\":\"internal\",\"message\":\"x\"}");
	reply = get(fx, "/title/7", cookie);
	assert_status(&reply, 503);
	axis_test_reply_clear(&reply);
	reply = post(fx, "/logout", cookie, withdraw);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);

	/* What is logged. */
	assert_has(log_text->str, "login: robin from " OUTSIDE "\n");
	assert_has(log_text->str, "login: failed from 198.51.100.44\n");
	assert_has(log_text->str, "login: failed from 198.51.100.45\n");
	assert_has(log_text->str, "request: robin asked for title 3: made\n");
	assert_has(log_text->str, "request: robin withdrew a request\n");
	assert_has(log_text->str, "logout: a session was ended from " OUTSIDE "\n");
	/* And what is not, at any level. */
	assert_lacks(log_text->str, "zz-hunter2");
	assert_lacks(log_text->str, "wrong-password");
	assert_lacks(log_text->str, ROBIN_PASSWORD);
	assert_lacks(log_text->str, fx->token);
	assert_lacks(log_text->str, fx->robin);
	assert_lacks(log_text->str, "zzProof");
	assert_lacks(log_text->str, "zzdev1");
	assert_lacks(log_text->str, csrf);
	assert_lacks(log_text->str, "zz-what-was-searched");
	assert_lacks(log_text->str, "__Host-");
}

/* Found by the review of P3 (S3): each refusal was a line of the log, so a
 * stranger refused a thousand times a second wrote that many lines.  Of one
 * kind, AXIS_PORTAL_LOG_LINES_A_MINUTE are written in a minute; then one line
 * says how many more there were, when the minute has ended, or when the
 * portal stops. */
static void
test_log_budget(Fixture *fx, gconstpointer data)
{
	static const gchar *const line = "refused 198.51.100.91, which presented too many session cookies";
	g_autofree gchar *summary = NULL;
	guint refused = 0;
	guint i;

	for (i = 0; i < 80; i++) {
		g_autofree gchar *made_up = g_strdup_printf("zzLogFlood%02u", i);
		g_autofree gchar *proof = fill43(made_up);
		g_autofree gchar *cookie = sealed(fx, proof);
		AxisTestReply reply = send_from(fx, "GET", "/requests", cookie, NULL, "198.51.100.91", NULL);

		if (reply.status == 429)
			refused++;
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(refused, >, AXIS_PORTAL_LOG_LINES_A_MINUTE + 10);
	g_assert_cmpuint(count_of(log_text->str, line), ==, AXIS_PORTAL_LOG_LINES_A_MINUTE);
	summary = g_strdup_printf("log: %u more lines of the kind \"refused an address with too many session cookies the "
		"daemon did not know\" were not written in the last minute", refused - AXIS_PORTAL_LOG_LINES_A_MINUTE);
	assert_lacks(log_text->str, "more lines of the kind");

	/* The minute ends: the next line of any kind says how many there were
	 * first. */
	axis_portal_app_set_time_offset(fx->app, G_GINT64_CONSTANT(61) * G_USEC_PER_SEC);
	{
		AxisTestReply reply = login_from(fx, "robin", "a wrong password", "198.51.100.92", NULL, NULL);

		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
	}
	assert_has(log_text->str, summary);
	g_assert_cmpuint(count_of(log_text->str, "more lines of the kind"), ==, 1);
	assert_has(log_text->str, "login: failed from 198.51.100.92");

	/* A new minute has its own lines; what is left out when the portal stops
	 * is said then. */
	for (i = 0; i < 30; i++) {
		AxisTestReply reply = login_from(fx, "robin", "a wrong password", "198.51.100.93", NULL, NULL);

		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(count_of(log_text->str, "login: failed from"), ==, AXIS_PORTAL_LOG_LINES_A_MINUTE);
	assert_lacks(log_text->str, "more lines of the kind \"login: failed\"");
	axis_portal_app_stop(fx->app);
	assert_has(log_text->str, "more lines of the kind \"login: failed\" were not written in the last minute");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/portal/config/unknown-key", test_config_unknown_key);
	g_test_add_func("/portal/config/plain-http-refused", test_config_plain_http);
	g_test_add_func("/portal/config/service-links", test_config_service_links);
	g_test_add_func("/portal/wire/bodies", test_wire_bodies);
	g_test_add_func("/portal/wire/answers", test_wire_answers);
	g_test_add_func("/portal/form/reader", test_form_reader);

#define ADD(path, mode, func) \
	g_test_add(path, Fixture, GINT_TO_POINTER(mode), fixture_setup, func, fixture_teardown)

	ADD("/portal/pages/daemon-away", MODE_PROXY, test_daemon_away);
	ADD("/portal/pages/without-session", MODE_PROXY, test_without_session);
	ADD("/portal/pages/two-session-cookies", MODE_PROXY, test_two_session_cookies);
	ADD("/portal/pages/probe-loopback-only", MODE_PROXY, test_probe);
	ADD("/portal/pages/proxy-says-nobody", MODE_PROXY, test_proxy_says_nobody);
	ADD("/portal/pages/headers", MODE_PROXY, test_headers);
	ADD("/portal/pages/state-and-form", MODE_PROXY, test_state_and_form);
	ADD("/portal/pages/provider-text", MODE_PROXY, test_provider_text);
	ADD("/portal/pages/failure-plain-words", MODE_PROXY, test_failure_words);
	ADD("/portal/pages/engine-paths", MODE_PROXY, test_engine_paths);
	ADD("/portal/pages/headers-not-passed-on", MODE_PROXY, test_headers_not_passed_on);
	ADD("/portal/pages/session-ended", MODE_PROXY, test_session_ended);
	ADD("/portal/session/cookie", MODE_PROXY, test_cookie);
	ADD("/portal/session/post-without-token", MODE_PROXY, test_post_without_token);
	ADD("/portal/session/nothing-secret-in-an-address", MODE_PROXY, test_nothing_secret);
	ADD("/portal/session/open-redirect", MODE_PROXY, test_open_redirect);
	ADD("/portal/session/plain-http", MODE_PLAIN, test_plain_http);
	ADD("/portal/session/forwarded-from-elsewhere", MODE_INSECURE, test_insecure_override);
	ADD("/portal/limits/login-flood", MODE_PROXY, test_login_flood);
	ADD("/portal/limits/made-up-cookies", MODE_PROXY, test_made_up_cookies);
	ADD("/portal/limits/one-device-misbehaves", MODE_PROXY, test_one_device_misbehaves);
	ADD("/portal/limits/shared-calls", MODE_PROXY, test_shared_calls);
	ADD("/portal/limits/search-flood", MODE_PROXY, test_search_flood);
	ADD("/portal/limits/large-body", MODE_PROXY, test_large_body);
	ADD("/portal/log/secrets-absent", MODE_PROXY, test_log);
	ADD("/portal/log/lines-a-minute", MODE_PROXY, test_log_budget);

	return g_test_run();
}
