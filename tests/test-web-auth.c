/*
 * test-web-auth.c - logging in: setup, passwords, sessions, throttling,
 * cookies, CSRF, roles and trusted proxies, against PostgreSQL
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test makes its own database (axis-test-db.h), migrates it, and runs
 * the real shell with a real AxisWebAuth on an ephemeral loopback port;
 * clients are libsoup messages with a cookie jar of their own (a table of
 * name -> value), so a test can be several browsers at once.  Passwords
 * are hashed with libsodium's minimum limits (axis_password_use_test_cost)
 * so that a test can log in dozens of times.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <sodium.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-resolver.h"
#include "core/axis-data-dir.h"
#include "core/axis-log.h"
#include "core/axis-module.h"
#include "db/axis-db-migrate.h"
#include "db/axis-db-pool.h"
#include "web/axis-password.h"
#include "web/axis-user-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-auth-portal.h"
#include "web/axis-web-auth-private.h"

#define ALICE_PASSWORD  "alice-secret-pw"
#define BOB_PASSWORD    "bob-secret-pw-1"
#define LOGIN_FAILED    "The username or password is incorrect."
#define DAY             (G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC)

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	const gchar        *trusted;	/* for the next start() */
	AxisSecureCookies   secure;
	guint               pokes;
} Fixture;

/* A browser: its cookies, and headers it sends every time. */
typedef struct
{
	GHashTable         *cookies;	/* name -> value */
	const gchar *const *extra;
} Client;

typedef struct
{
	guint        status;
	SoupMessage *message;
	gchar       *body;
} Reply;

static Fixture *current_fixture;

/* ── A module with a page and a form handler ────────────────────── */

static HtmxResponse *
page_movies(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "Movie list goes here.");
	return axis_web_page_respond(request, "Movies", HTMX_NODE(text));
}

static HtmxResponse *
page_poke(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	current_fixture->pokes++;
	htmx_node_set_text_content(HTMX_NODE(text), "Poked.");
	return axis_web_page_respond(request, "Poke", HTMX_NODE(text));
}

static void
movies_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_get(web, "/movies", page_movies, NULL);
	axis_web_post(web, "/movies/poke", page_poke, NULL);
}

/* ── Fixture ────────────────────────────────────────────────────── */

static void
start(Fixture *fx)
{
	GError *error = NULL;
	GSList *uris;
	const gchar *colon;

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(fx->pool, fx->trusted != NULL ? fx->trusted : "", fx->secure, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);

	uris = htmx_server_get_listen_uris(fx->server);
	colon = strrchr(uris->data, ':');
	fx->port = (guint16)g_ascii_strtoull(colon + 1, NULL, 10);
	g_slist_free_full(uris, g_free);
}

static void
stop(Fixture *fx)
{
	if (fx->server == NULL)
		return;

	htmx_server_stop(fx->server);
	axis_web_auth_stop(fx->auth);
	axis_web_free(fx->web);
	axis_web_auth_free(fx->auth);
	g_clear_object(&fx->server);
	fx->web = NULL;
	fx->auth = NULL;
}

/* The daemon going down and up again on the same database. */
static void
restart(Fixture *fx)
{
	stop(fx);
	start(fx);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	GError *error = NULL;
	AxisModuleInfo info;

	memset(fx, 0, sizeof(*fx));
	current_fixture = fx;
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "no account exists yet*");

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_db_pool_new(axis_test_db_get_url(fx->db), 4, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_migrate(fx->pool, &error));
	g_assert_no_error(error);

	fx->registry = axis_module_registry_new();
	memset(&info, 0, sizeof(info));
	info.id = "movies";
	info.name = "Movies";
	info.register_routes = movies_routes;
	g_assert_true(axis_module_registry_register(fx->registry, &info, NULL, &error));

	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	stop(fx);
	g_clear_object(&fx->session);
	if (fx->registry != NULL)
		axis_module_registry_free(fx->registry);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	current_fixture = NULL;
}

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

/* An account made straight in the database (before start()). */
static gint64
seed(
	Fixture      *fx,
	const gchar  *username,
	const gchar  *password,
	AxisUserRole  role,
	gboolean      disabled
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = axis_password_hash(password, &error);
	g_autoptr(AxisUser) user = NULL;
	gint64 id;

	g_assert_no_error(error);
	user = axis_user_store_create(fx->pool, username, hash, role, &error);
	g_assert_no_error(error);
	id = user->id;
	if (disabled) {
		g_autoptr(AxisUser) changed = axis_user_store_set_disabled(fx->pool, id, TRUE, &error);

		g_assert_no_error(error);
	}

	return id;
}

static gchar *
query(Fixture *fx, const gchar *sql)
{
	return axis_test_db_query(fx->db, sql);
}

static void
assert_query(Fixture *fx, const gchar *sql, const gchar *expected)
{
	g_autofree gchar *value = query(fx, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: got '%s', expected '%s'", sql, value, expected);
}

/* ── Clients and requests ───────────────────────────────────────── */

static Client *
client_new(const gchar *const *extra)
{
	Client *client = g_new0(Client, 1);

	client->cookies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	client->extra = extra;
	return client;
}

static void
client_free(Client *client)
{
	g_hash_table_unref(client->cookies);
	g_free(client);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Client, client_free)

static const gchar *
client_cookie(Client *client, const gchar *name)
{
	return g_hash_table_lookup(client->cookies, name);
}

static void
client_set_cookie(Client *client, const gchar *name, const gchar *value)
{
	g_hash_table_replace(client->cookies, g_strdup(name), g_strdup(value));
}

static void
reply_clear(Reply *reply)
{
	g_clear_object(&reply->message);
	g_clear_pointer(&reply->body, g_free);
}

static const gchar *
header(const Reply *reply, const gchar *name)
{
	return soup_message_headers_get_one(soup_message_get_response_headers(reply->message), name);
}

/* The Set-Cookie header of @reply that sets @name, or NULL. */
static gchar *
set_cookie_header(const Reply *reply, const gchar *name)
{
	SoupMessageHeadersIter iter;
	const gchar *header_name;
	const gchar *value;
	g_autofree gchar *prefix = g_strconcat(name, "=", NULL);

	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(reply->message));
	while (soup_message_headers_iter_next(&iter, &header_name, &value)) {
		if (g_ascii_strcasecmp(header_name, "Set-Cookie") == 0 && g_str_has_prefix(value, prefix))
			return g_strdup(value);
	}

	return NULL;
}

static void
take_cookies(Client *client, SoupMessage *message)
{
	SoupMessageHeadersIter iter;
	const gchar *header_name;
	const gchar *value;

	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(message));
	while (soup_message_headers_iter_next(&iter, &header_name, &value)) {
		g_autofree gchar *pair = NULL;
		gchar *eq;

		if (g_ascii_strcasecmp(header_name, "Set-Cookie") != 0)
			continue;
		pair = g_strndup(value, strcspn(value, ";"));
		eq = strchr(pair, '=');
		g_assert_nonnull(eq);
		*eq = '\0';
		if (strstr(value, "Max-Age=0") != NULL)
			g_hash_table_remove(client->cookies, pair);
		else
			client_set_cookie(client, pair, eq + 1);
	}
}

static SoupMessage *
build(
	Fixture            *fx,
	Client             *client,
	const gchar        *method,
	const gchar        *path,
	const gchar *const *form,
	const gchar *const *extra
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)fx->port, path);
	SoupMessage *message = soup_message_new(method, url);
	SoupMessageHeaders *headers;
	GHashTableIter iter;
	gpointer name;
	gpointer value;
	GString *cookies = g_string_new(NULL);
	guint i;

	g_assert_nonnull(message);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	g_hash_table_iter_init(&iter, client->cookies);
	while (g_hash_table_iter_next(&iter, &name, &value))
		g_string_append_printf(cookies, "%s%s=%s", cookies->len > 0 ? "; " : "",
			(const gchar *)name, (const gchar *)value);
	if (cookies->len > 0)
		soup_message_headers_append(headers, "Cookie", cookies->str);
	g_string_free(cookies, TRUE);

	for (i = 0; client->extra != NULL && client->extra[i] != NULL; i += 2)
		soup_message_headers_append(headers, client->extra[i], client->extra[i + 1]);
	for (i = 0; extra != NULL && extra[i] != NULL; i += 2)
		soup_message_headers_append(headers, extra[i], extra[i + 1]);

	if (form != NULL) {
		GString *body = g_string_new(NULL);
		GBytes *bytes;

		for (i = 0; form[i] != NULL; i += 2) {
			g_autofree gchar *key = g_uri_escape_string(form[i], NULL, FALSE);
			g_autofree gchar *val = g_uri_escape_string(form[i + 1], NULL, FALSE);

			g_string_append_printf(body, "%s%s=%s", i > 0 ? "&" : "", key, val);
		}
		bytes = g_bytes_new(body->str, body->len);
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
		g_bytes_unref(bytes);
		g_string_free(body, TRUE);
	}

	return message;
}

typedef struct
{
	GMainLoop *loop;
	guint      waiting;
	GBytes    *bytes[2];
	GError    *errors[2];
	SoupMessage *messages[2];
} Pending;

static void
on_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Pending *pending = user_data;
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	guint slot = message == pending->messages[0] ? 0 : 1;

	pending->bytes[slot] = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
		&pending->errors[slot]);
	if (--pending->waiting == 0)
		g_main_loop_quit(pending->loop);
}

static Reply
finish(
	Client      *client,
	SoupMessage *message,
	GBytes      *bytes,
	GError      *error
){
	Reply reply;
	gsize size = 0;
	gconstpointer data;

	g_assert_no_error(error);
	g_assert_nonnull(bytes);
	data = g_bytes_get_data(bytes, &size);
	reply.message = message;
	reply.status = soup_message_get_status(message);
	reply.body = g_strndup(data != NULL ? data : "", size);
	g_bytes_unref(bytes);
	take_cookies(client, message);

	return reply;
}

static Reply
send(
	Fixture            *fx,
	Client             *client,
	const gchar        *method,
	const gchar        *path,
	const gchar *const *form,
	const gchar *const *extra
){
	Pending pending;

	memset(&pending, 0, sizeof(pending));
	pending.messages[0] = build(fx, client, method, path, form, extra);
	pending.loop = g_main_loop_new(NULL, FALSE);
	pending.waiting = 1;
	soup_session_send_and_read_async(fx->session, pending.messages[0], G_PRIORITY_DEFAULT, NULL,
		on_done, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);

	return finish(client, pending.messages[0], pending.bytes[0], pending.errors[0]);
}

static Reply
get(Fixture *fx, Client *client, const gchar *path)
{
	return send(fx, client, "GET", path, NULL, NULL);
}

/* A redirect to @location: the address and word, then (as the pages come
 * back to a place) the place. */
static void
assert_redirect_to(const Reply *reply, const gchar *location)
{
	const gchar *got = header(reply, "Location");

	g_assert_nonnull(got);
	g_assert_true(g_str_has_prefix(got, location));
	g_assert_true(got[strlen(location)] == '\0' || g_str_has_prefix(got + strlen(location), "&at="));
}

/* ── Reading pages ──────────────────────────────────────────────── */

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

/* The value of the first input called @name, or NULL. */
static gchar *
input_value(const gchar *html, const gchar *name)
{
	g_autofree gchar *needle = g_strdup_printf("name=\"%s\"", name);
	const gchar *p = html;

	while ((p = strstr(p, "<input")) != NULL) {
		const gchar *end = strchr(p, '>');
		g_autofree gchar *tag = NULL;

		g_assert_nonnull(end);
		tag = g_strndup(p, end - p);
		if (strstr(tag, needle) != NULL) {
			const gchar *value = strstr(tag, " value=\"");

			if (value == NULL)
				return g_strdup("");
			value += strlen(" value=\"");
			return g_strndup(value, strchr(value, '"') - value);
		}
		p = end;
	}

	return NULL;
}

/* The CSRF token a page hands htmx (hx-headers on its body). */
static gchar *
page_csrf(const gchar *html)
{
	const gchar *marker = "X-CSRF-Token&quot;:&quot;";
	const gchar *start = strstr(html, marker);

	if (start == NULL)
		return NULL;
	start += strlen(marker);
	return g_strndup(start, strstr(start, "&quot;") - start);
}

/* The session CSRF token of a logged-in @client. */
static gchar *
csrf_of(Fixture *fx, Client *client)
{
	Reply page = get(fx, client, "/account");
	gchar *token;

	g_assert_cmpuint(page.status, ==, 200);
	token = page_csrf(page.body);
	g_assert_nonnull(token);
	reply_clear(&page);

	return token;
}

static gboolean
logged_in(Fixture *fx, Client *client)
{
	Reply page = get(fx, client, "/account");
	gboolean yes = page.status == 200;

	if (!yes)
		g_assert_cmpuint(page.status, ==, 303);
	reply_clear(&page);

	return yes;
}

/* The pre-session form token from @path (which sets its cookie). */
static gchar *
form_token(Fixture *fx, Client *client, const gchar *path)
{
	Reply page = get(fx, client, path);
	gchar *token = input_value(page.body, "_csrf");

	g_assert_cmpuint(page.status, ==, 200);
	g_assert_nonnull(token);
	reply_clear(&page);

	return token;
}

static Reply
login(
	Fixture     *fx,
	Client      *client,
	const gchar *username,
	const gchar *password,
	const gchar *next
){
	g_autofree gchar *token = form_token(fx, client, "/login");
	const gchar *form[] = {
		"_csrf", token, "username", username, "password", password,
		next != NULL ? "next" : NULL, next, NULL
	};

	return send(fx, client, "POST", "/login", form, NULL);
}

static void
login_ok(
	Fixture     *fx,
	Client      *client,
	const gchar *username,
	const gchar *password
){
	Reply reply = login(fx, client, username, password, NULL);

	if (reply.status != 303)
		g_error("login as %s: expected 303, got %u:\n%s", username, reply.status, reply.body);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/");
	reply_clear(&reply);
}

static gchar *
setup_token(void)
{
	g_autofree gchar *line = axis_test_log_find(G_LOG_LEVEL_WARNING, "no account exists yet*");

	g_assert_nonnull(line);
	return g_strdup(line + strlen(line) - AXIS_SETUP_TOKEN_LEN);
}

static Reply
post_setup(
	Fixture     *fx,
	Client      *client,
	const gchar *token,
	const gchar *username,
	const gchar *password,
	const gchar *confirm
){
	g_autofree gchar *csrf = form_token(fx, client, "/setup");
	const gchar *form[] = {
		"_csrf", csrf, "setup_token", token, "username", username,
		"password", password, "password_confirm", confirm, NULL
	};

	return send(fx, client, "POST", "/setup", form, NULL);
}

/* POST @path as a logged-in @client, with its CSRF field and @fields. */
static Reply
post_as(
	Fixture            *fx,
	Client             *client,
	const gchar        *path,
	const gchar *const *fields
){
	g_autofree gchar *token = csrf_of(fx, client);
	const gchar *form[16];
	guint n = 0;
	guint i;

	form[n++] = "_csrf";
	form[n++] = token;
	for (i = 0; fields != NULL && fields[i] != NULL; i++)
		form[n++] = fields[i];
	form[n] = NULL;

	return send(fx, client, "POST", path, form, NULL);
}

static gint64
user_id(Fixture *fx, const gchar *username)
{
	g_autofree gchar *sql = g_strdup_printf("SELECT id FROM app_user WHERE username = '%s'", username);
	g_autofree gchar *id = query(fx, sql);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

/* ── Setup ──────────────────────────────────────────────────────── */

/* Spec: Setup with the token */
static void
test_setup_with_token(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *token = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	start(fx);
	token = setup_token();
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "no account exists yet*"), ==, 1);

	/* With no account, everything leads to setup. */
	reply = get(fx, browser, "/movies");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/setup");
	reply_clear(&reply);
	reply = get(fx, browser, "/login");
	g_assert_cmpstr(header(&reply, "Location"), ==, "/setup");
	reply_clear(&reply);

	reply = post_setup(fx, browser, token, "Admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/");
	g_assert_nonnull(client_cookie(browser, "axis_session"));
	reply_clear(&reply);

	/* Logged in as the new administrator. */
	reply = get(fx, browser, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Dashboard");
	assert_contains(reply.body, ">admin</a>");
	assert_contains(reply.body, "href=\"/admin/users\"");
	reply_clear(&reply);

	assert_query(fx, "SELECT username || ' ' || role || ' ' || setup_admin FROM app_user", "admin admin true");

	/* The token is spent: setup is gone. */
	reply = get(fx, browser, "/setup");
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);
}

/* Spec (add-request-portal, slice P4): no first setup from one of the
 * portal's addresses.  The right setup token, from an address in
 * server.portal_peers, is answered as a wrong one is, however often: no
 * account is made, no count is touched (the same address, once it is no
 * longer the portal's, sets Axis up at the first try), and the log says
 * why, once. */
static void
test_setup_from_portal_address(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL;
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	start(fx);
	token = setup_token();
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "127.0.0.0/30", &error));
	g_assert_no_error(error);

	/* (More often than an address has free failures.) */
	for (i = 0; i < 25; i++) {
		reply = post_setup(fx, browser, token, "Admin", ALICE_PASSWORD, ALICE_PASSWORD);
		g_assert_cmpuint(reply.status, ==, 403);
		assert_contains(reply.body, "The setup token is not correct.");
		g_assert_null(client_cookie(browser, "axis_session"));
		reply_clear(&reply);
	}
	assert_query(fx, "SELECT count(*) FROM app_user", "0");
	g_assert_cmpuint(axis_web_auth_portal_logins_barred(fx->auth, NULL), ==, 25);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "audit: setup refused: the connection came from "
		"127.0.0.1, one of the request portal's addresses (server.portal_peers, AXIS_PORTAL_PEERS)*"), ==, 1);

	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "", &error));
	g_assert_no_error(error);
	reply = post_setup(fx, browser, token, "Admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_nonnull(client_cookie(browser, "axis_session"));
	reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM app_user", "1");
}

/* Spec: Setup without the token */
static void
test_setup_without_token(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *token = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	start(fx);
	token = setup_token();

	reply = post_setup(fx, browser, "", "admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 403);
	assert_contains(reply.body, "setup token is not correct");
	g_assert_null(client_cookie(browser, "axis_session"));
	reply_clear(&reply);

	reply = post_setup(fx, browser, "0123456789abcdef0123456789abcdef", "admin", ALICE_PASSWORD,
		ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);

	/* The right length but one digit off. */
	{
		g_autofree gchar *near = g_strdup(token);

		near[0] = near[0] == 'a' ? 'b' : 'a';
		reply = post_setup(fx, browser, near, "admin", ALICE_PASSWORD, ALICE_PASSWORD);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}

	/* No form token at all is refused before anything else. */
	{
		const gchar *form[] = { "setup_token", token, "username", "admin",
			"password", ALICE_PASSWORD, "password_confirm", ALICE_PASSWORD, NULL };

		reply = send(fx, browser, "POST", "/setup", form, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}

	assert_query(fx, "SELECT count(*) FROM app_user", "0");

	/* Failures do not spend the token. */
	reply = post_setup(fx, browser, token, "admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM app_user", "1");
}

/* Spec: Setup closed after first account */
static void
test_setup_closed(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *csrf = NULL;
	Reply reply;
	const gchar *form[11];

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	start(fx);

	/* No token is made or logged once an account exists. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "no account exists yet*"), ==, 0);

	reply = get(fx, browser, "/setup");
	g_assert_cmpuint(reply.status, ==, 404);
	assert_lacks(reply.body, "setup_token");
	reply_clear(&reply);

	/* A well-formed submission (its form token from the login page). */
	csrf = form_token(fx, browser, "/login");
	form[0] = "_csrf";
	form[1] = csrf;
	form[2] = "setup_token";
	form[3] = "0123456789abcdef0123456789abcdef";
	form[4] = "username";
	form[5] = "mallory";
	form[6] = "password";
	form[7] = BOB_PASSWORD;
	form[8] = "password_confirm";
	form[9] = BOB_PASSWORD;
	form[10] = NULL;
	reply = send(fx, browser, "POST", "/setup", form, NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);

	assert_query(fx, "SELECT string_agg(username, ',') FROM app_user", "alice");
}

/* Spec: Racing setup requests (through the server) */
static void
test_setup_race(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) first = client_new(NULL);
	g_autoptr(Client) second = client_new(NULL);
	g_autofree gchar *token = NULL;
	g_autofree gchar *csrf_first = NULL;
	g_autofree gchar *csrf_second = NULL;
	Pending pending;
	Reply a;
	Reply b;

	SKIP_WITHOUT_DB(fx);
	start(fx);
	token = setup_token();
	csrf_first = form_token(fx, first, "/setup");
	csrf_second = form_token(fx, second, "/setup");

	{
		const gchar *form_a[] = { "_csrf", csrf_first, "setup_token", token, "username", "anna",
			"password", ALICE_PASSWORD, "password_confirm", ALICE_PASSWORD, NULL };
		const gchar *form_b[] = { "_csrf", csrf_second, "setup_token", token, "username", "bert",
			"password", BOB_PASSWORD, "password_confirm", BOB_PASSWORD, NULL };

		memset(&pending, 0, sizeof(pending));
		pending.messages[0] = build(fx, first, "POST", "/setup", form_a, NULL);
		pending.messages[1] = build(fx, second, "POST", "/setup", form_b, NULL);
	}
	pending.loop = g_main_loop_new(NULL, FALSE);
	pending.waiting = 2;
	soup_session_send_and_read_async(fx->session, pending.messages[0], G_PRIORITY_DEFAULT, NULL,
		on_done, &pending);
	soup_session_send_and_read_async(fx->session, pending.messages[1], G_PRIORITY_DEFAULT, NULL,
		on_done, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);

	a = finish(first, pending.messages[0], pending.bytes[0], pending.errors[0]);
	b = finish(second, pending.messages[1], pending.bytes[1], pending.errors[1]);

	/* Exactly one won. */
	g_assert_true((a.status == 303 && b.status == 404) || (a.status == 404 && b.status == 303));
	assert_query(fx, "SELECT count(*) FROM app_user", "1");
	assert_query(fx, "SELECT count(*) FROM app_user WHERE role = 'admin'", "1");

	reply_clear(&a);
	reply_clear(&b);
}

typedef struct
{
	AxisDbPool *pool;
	gchar      *username;
	gint       *wins;
	gint       *closed;
} RaceArgs;

static gpointer
race_first_admin(gpointer data)
{
	RaceArgs *args = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisUser) user = NULL;

	user = axis_user_store_create_first_admin(args->pool, args->username,
		"$argon2id$v=19$m=8,t=1,p=1$c2FsdHNhbHQ$aGFzaGhhc2hoYXNoaGFzaA", &error);
	if (user != NULL)
		g_atomic_int_inc(args->wins);
	else if (g_error_matches(error, AXIS_USER_ERROR, AXIS_USER_ERROR_SETUP_CLOSED))
		g_atomic_int_inc(args->closed);
	else
		g_error("unexpected: %s", error->message);

	return NULL;
}

/* Spec: Racing setup requests (in the database, truly concurrent) */
static void
test_setup_race_in_database(Fixture *fx, gconstpointer data)
{
	GThread *threads[4];
	RaceArgs args[4];
	gint wins = 0;
	gint closed = 0;
	guint round;
	guint i;

	SKIP_WITHOUT_DB(fx);

	for (round = 0; round < 5; round++) {
		wins = 0;
		closed = 0;
		axis_test_db_exec(fx->db, "DELETE FROM app_user");

		for (i = 0; i < G_N_ELEMENTS(threads); i++) {
			args[i].pool = fx->pool;
			args[i].username = g_strdup_printf("racer%u", i);
			args[i].wins = &wins;
			args[i].closed = &closed;
			threads[i] = g_thread_new("racer", race_first_admin, &args[i]);
		}
		for (i = 0; i < G_N_ELEMENTS(threads); i++) {
			g_thread_join(threads[i]);
			g_free(args[i].username);
		}

		g_assert_cmpint(wins, ==, 1);
		g_assert_cmpint(closed, ==, (gint)G_N_ELEMENTS(threads) - 1);
		assert_query(fx, "SELECT count(*) FROM app_user", "1");
	}
}

/* ── Passwords ──────────────────────────────────────────────────── */

/* Spec: Stored form */
static void
test_password_stored_form(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *token = NULL;
	g_autofree gchar *stored = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	start(fx);
	token = setup_token();
	reply = post_setup(fx, browser, token, "admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);

	stored = query(fx, "SELECT password_hash FROM app_user");
	g_assert_true(g_str_has_prefix(stored, "$argon2id$v=19$"));
	g_assert_null(strstr(stored, ALICE_PASSWORD));
	g_assert_null(strstr(stored, "alice-secret"));
	/* And nothing else in the table holds it. */
	{
		g_autofree gchar *row = query(fx, "SELECT row_to_json(u)::text FROM app_user AS u");

		g_assert_null(strstr(row, ALICE_PASSWORD));
	}
}

/* Spec: Short password refused (setup, user creation, password change) */
static void
test_password_short_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *token = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	start(fx);
	token = setup_token();

	reply = post_setup(fx, browser, token, "admin", "short", "short");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "at least 10 characters");
	reply_clear(&reply);
	reply = post_setup(fx, browser, token, "admin", "123456789", "123456789");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "at least 10 characters");
	reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM app_user", "0");

	reply = post_setup(fx, browser, token, "admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);

	/* Creating an account. */
	{
		const gchar *fields[] = { "username", "erin", "password", "tiny", "role", "user", NULL };

		reply = post_as(fx, browser, "/admin/users", fields);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "at least 10 characters");
		reply_clear(&reply);
		assert_query(fx, "SELECT count(*) FROM app_user", "1");
	}

	/* Changing one's own. */
	{
		const gchar *fields[] = { "current_password", ALICE_PASSWORD, "new_password", "tiny",
			"new_password_confirm", "tiny", NULL };
		g_autofree gchar *before = query(fx, "SELECT password_hash FROM app_user");
		g_autofree gchar *after = NULL;

		reply = post_as(fx, browser, "/account/password", fields);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "at least 10 characters");
		reply_clear(&reply);
		after = query(fx, "SELECT password_hash FROM app_user");
		g_assert_cmpstr(before, ==, after);
	}

	/* Setting someone else's. */
	{
		const gchar *fields[] = { "password", "tiny", NULL };
		g_autofree gchar *path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/password",
			user_id(fx, "admin"));

		reply = post_as(fx, browser, path, fields);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "at least 10 characters");
		reply_clear(&reply);
	}
}

/* Whether any recorded log message contains @secret. */
static guint
log_mentions(const gchar *secret)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING,
		G_LOG_LEVEL_CRITICAL
	};
	g_autofree gchar *pattern = g_strdup_printf("*%s*", secret);
	guint count = 0;
	guint i;

	g_assert_null(strchr(secret, '*'));
	g_assert_null(strchr(secret, '?'));
	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		count += axis_test_log_count(levels[i], pattern);

	return count;
}

/* Spec: Passwords absent from logs */
static void
test_password_absent_from_logs(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autoptr(Client) other = client_new(NULL);
	g_autoptr(Client) failing = client_new(NULL);
	g_autofree gchar *token = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *session = NULL;
	g_autofree gchar *csrf = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	axis_log_set_level(AXIS_LOG_LEVEL_DEBUG);
	start(fx);
	token = setup_token();

	reply = post_setup(fx, browser, token, "admin", ALICE_PASSWORD, ALICE_PASSWORD);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);

	/* Success, a wrong password, an unknown name, a password typed into
	 * the name box. */
	login_ok(fx, other, "admin", ALICE_PASSWORD);
	reply = login(fx, failing, "admin", "wrong-password-1", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	reply = login(fx, failing, "nobody", "wrong-password-2", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	reply = login(fx, failing, "typedpassword9", "wrong-password-3", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);

	/* A password change too. */
	{
		const gchar *fields[] = { "current_password", ALICE_PASSWORD,
			"new_password", "brand-new-password", "new_password_confirm", "brand-new-password", NULL };

		reply = post_as(fx, browser, "/account/password", fields);
		g_assert_cmpuint(reply.status, ==, 303);
		reply_clear(&reply);
	}

	stored = query(fx, "SELECT password_hash FROM app_user");
	session = g_strdup(client_cookie(browser, "axis_session"));
	csrf = csrf_of(fx, browser);

	/* There were audit lines... */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "audit: login: user=admin*"), >=, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "audit: login failed*"), ==, 3);
	/* ... and none of them, nor anything else, holds a secret. */
	g_assert_cmpuint(log_mentions(ALICE_PASSWORD), ==, 0);
	g_assert_cmpuint(log_mentions("wrong-password-"), ==, 0);
	g_assert_cmpuint(log_mentions("brand-new-password"), ==, 0);
	g_assert_cmpuint(log_mentions("typedpassword9"), ==, 0);
	g_assert_cmpuint(log_mentions(stored), ==, 0);
	g_assert_cmpuint(log_mentions("$argon2id$"), ==, 0);
	g_assert_cmpuint(log_mentions(session), ==, 0);
	g_assert_cmpuint(log_mentions(csrf), ==, 0);
	/* The setup token, once, where it was meant to be. */
	g_assert_cmpuint(log_mentions(token), ==, 1);

	axis_log_set_level(AXIS_LOG_LEVEL_INFO);
}

/* ── Login ──────────────────────────────────────────────────────── */

/* Spec: Successful login, Unauthenticated page request (preserved path) */
static void
test_login_success_returns(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *next = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	reply = get(fx, browser, "/movies?sort=size");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/login?next=%2Fmovies%3Fsort%3Dsize");
	reply_clear(&reply);

	reply = get(fx, browser, "/login?next=%2Fmovies%3Fsort%3Dsize");
	g_assert_cmpuint(reply.status, ==, 200);
	next = input_value(reply.body, "next");
	g_assert_cmpstr(next, ==, "/movies?sort=size");
	reply_clear(&reply);

	reply = login(fx, browser, "alice", ALICE_PASSWORD, next);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/movies?sort=size");
	g_assert_nonnull(client_cookie(browser, "axis_session"));
	/* The pre-session cookie is done with. */
	g_assert_null(client_cookie(browser, "axis_presession"));
	reply_clear(&reply);

	reply = get(fx, browser, "/movies?sort=size");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Movie list goes here.");
	reply_clear(&reply);

	/* Logged in, the login page sends one on. */
	reply = get(fx, browser, "/login");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/");
	reply_clear(&reply);
}

/* Spec: Open redirect refused */
static void
test_login_open_redirect(Fixture *fx, gconstpointer data)
{
	static const gchar *const hostile[] = {
		"https://evil.example/", "//evil.example", "/\\evil.example", "\\\\evil.example",
		"http:evil.example", "/\t/evil.example", "javascript:alert(1)", " //evil.example",
		"/login", "/logout?x=1", NULL
	};
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	for (i = 0; hostile[i] != NULL; i++) {
		g_autoptr(Client) browser = client_new(NULL);
		g_autofree gchar *escaped = g_uri_escape_string(hostile[i], NULL, FALSE);
		g_autofree gchar *page = g_strconcat("/login?next=", escaped, NULL);
		Reply reply = get(fx, browser, page);
		g_autofree gchar *next = input_value(reply.body, "next");

		/* Not even carried in the form. */
		g_assert_null(next);
		reply_clear(&reply);

		reply = login(fx, browser, "alice", ALICE_PASSWORD, hostile[i]);
		if (reply.status != 303 || g_strcmp0(header(&reply, "Location"), "/") != 0)
			g_error("next '%s': expected 303 to /, got %u to %s", hostile[i], reply.status,
				header(&reply, "Location"));
		reply_clear(&reply);
	}
}

/* Spec: Indistinguishable failures */
static void
test_login_indistinguishable(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	static const gchar *const typed = "someone";
	const gchar *users[] = { "nobody", "dora", "alice" };
	const gchar *passwords[] = { ALICE_PASSWORD, BOB_PASSWORD, "not-the-password" };
	gchar *bodies[3];
	guint statuses[3];
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	seed(fx, "dora", BOB_PASSWORD, AXIS_USER_ROLE_USER, TRUE);
	start(fx);

	for (i = 0; i < 3; i++) {
		Reply reply = login(fx, browser, users[i], passwords[i], NULL);
		GString *body = g_string_new(reply.body);
		g_autofree gchar *echo = g_strdup_printf("value=\"%s\"", users[i]);
		g_autofree gchar *placeholder = g_strdup_printf("value=\"%s\"", typed);

		statuses[i] = reply.status;
		g_assert_null(set_cookie_header(&reply, "axis_session"));
		/* Only the name typed is echoed back; take it out to compare. */
		g_string_replace(body, echo, placeholder, 0);
		bodies[i] = g_string_free(body, FALSE);
		assert_contains(bodies[i], LOGIN_FAILED);
		reply_clear(&reply);
	}

	g_assert_cmpuint(statuses[0], ==, 401);
	g_assert_cmpuint(statuses[1], ==, 401);
	g_assert_cmpuint(statuses[2], ==, 401);
	g_assert_cmpstr(bodies[0], ==, bodies[1]);
	g_assert_cmpstr(bodies[1], ==, bodies[2]);
	for (i = 0; i < 3; i++)
		g_free(bodies[i]);

	g_assert_null(client_cookie(browser, "axis_session"));
}

/* Spec: Username case */
static void
test_login_username_case(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) first = client_new(NULL);
	g_autoptr(Client) second = client_new(NULL);
	g_autoptr(Client) third = client_new(NULL);
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	login_ok(fx, first, "Alice", ALICE_PASSWORD);
	login_ok(fx, second, "  ALICE ", ALICE_PASSWORD);
	g_assert_true(logged_in(fx, first));

	/* The password is not case-insensitive. */
	reply = login(fx, third, "alice", "ALICE-SECRET-PW", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
}

/* ── Throttling ─────────────────────────────────────────────────── */

/* Spec: Repeated failures are throttled */
static void
test_throttle_repeated_failures(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	const gchar *retry;
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	for (i = 0; i < 6; i++) {
		reply = login(fx, browser, "alice", "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}

	/* Now refused, even with the right password. */
	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	retry = header(&reply, "Retry-After");
	g_assert_nonnull(retry);
	g_assert_cmpint(g_ascii_strtoll(retry, NULL, 10), >=, 1);
	assert_contains(reply.body, "Too many failed attempts");
	g_assert_null(client_cookie(browser, "axis_session"));
	reply_clear(&reply);

	/* The same name in any case is the same account. */
	reply = login(fx, browser, "ALICE", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);

	/* Once the delay has passed, the right password works. */
	axis_web_auth_set_time_offset(fx->auth, 3 * G_USEC_PER_SEC);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
}

/* Spec: Success clears the counter */
static void
test_throttle_success_clears(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autoptr(Client) later = client_new(NULL);
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	for (i = 0; i < 4; i++) {
		reply = login(fx, browser, "alice", "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	/* Without the reset, the fifth of these would be over the limit. */
	for (i = 0; i < 5; i++) {
		reply = login(fx, later, "alice", "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}
	reply = login(fx, later, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
}

/* Many requests sent at once, each on a connection of its own. */
typedef struct
{
	guint        waiting;
	guint        statuses[16];
	SoupMessage *messages[16];
} Burst;

static void
on_burst_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Burst *burst = user_data;
	SoupMessage *message = soup_session_get_async_result_message(SOUP_SESSION(source), result);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	guint i;

	g_assert_no_error(error);
	for (i = 0; i < G_N_ELEMENTS(burst->messages); i++) {
		if (burst->messages[i] == message)
			burst->statuses[i] = soup_message_get_status(message);
	}
	if (--burst->waiting == 0)
		g_main_context_wakeup(NULL);
}

/* POSTs @form to @path @n times at once as @client; counts the answers
 * with @status_a and @status_b. */
static void
post_burst(
	Fixture            *fx,
	Client             *client,
	const gchar        *path,
	const gchar *const *form,
	guint               n,
	guint               status_a,
	guint              *count_a,
	guint               status_b,
	guint              *count_b
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30,
		"max-conns", 64, "max-conns-per-host", 64, NULL);
	gint64 deadline = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
	Burst burst;
	guint i;

	g_assert_cmpuint(n, <=, G_N_ELEMENTS(burst.messages));
	memset(&burst, 0, sizeof(burst));
	for (i = 0; i < n; i++) {
		burst.messages[i] = build(fx, client, "POST", path, form, NULL);
		burst.waiting++;
	}
	for (i = 0; i < n; i++)
		soup_session_send_and_read_async(session, burst.messages[i], G_PRIORITY_DEFAULT, NULL, on_burst_done,
			&burst);
	while (burst.waiting > 0 && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(burst.waiting, ==, 0);

	*count_a = 0;
	*count_b = 0;
	for (i = 0; i < n; i++) {
		if (burst.statuses[i] == status_a)
			(*count_a)++;
		else if (burst.statuses[i] == status_b)
			(*count_b)++;
		else
			g_error("an attempt of the burst answered %u", burst.statuses[i]);
		g_object_unref(burst.messages[i]);
	}
}

/* An account whose password takes tens of milliseconds to check, as in the
 * daemon (libsodium's interactive limits), so that attempts sent together
 * are all being checked at once. */
static void
seed_slow(
	Fixture     *fx,
	const gchar *username,
	const gchar *password
){
	gchar hash[crypto_pwhash_argon2id_STRBYTES];
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisUser) user = NULL;

	g_assert_cmpint(crypto_pwhash_argon2id_str(hash, password, strlen(password),
		crypto_pwhash_argon2id_OPSLIMIT_INTERACTIVE, crypto_pwhash_argon2id_MEMLIMIT_INTERACTIVE), ==, 0);
	user = axis_user_store_create(fx->pool, username, hash, AXIS_USER_ROLE_USER, &error);
	g_assert_no_error(error);
}

/*
 * Found by the stage 2 hunt of the web layer (W-2): the throttle was asked
 * when an attempt arrived and told of its failure when the check ended, so
 * attempts sent together all had their password checked.  An attempt in
 * flight now counts as the failure it may be: no more are checked at once
 * than one after another would be (five free failures and the one that
 * starts the delay).
 */
static void
test_throttle_burst(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *token = NULL;
	guint checked;
	guint refused;

	SKIP_WITHOUT_DB(fx);
	seed_slow(fx, "alice", ALICE_PASSWORD);
	start(fx);

	token = form_token(fx, browser, "/login");
	{
		const gchar *form[] = { "_csrf", token, "username", "alice", "password", "wrong-password", NULL };

		post_burst(fx, browser, "/login", form, 12, 401, &checked, 429, &refused);
	}
	g_test_message("of 12 wrong passwords sent at once, %u were checked and %u refused", checked, refused);
	g_assert_cmpuint(checked, <=, 6);
	g_assert_cmpuint(checked + refused, ==, 12);
	g_assert_cmpuint(axis_login_throttle_get_pending(fx->auth->by_user, "alice"), ==, 0);
	g_assert_cmpuint(axis_login_throttle_get_pending(fx->auth->by_address, "127.0.0.1"), ==, 0);

	/* Once they have ended, the delay they earned holds, even for the
	 * right password, and then passes as before. */
	{
		Reply reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);

		g_assert_cmpuint(reply.status, ==, 429);
		reply_clear(&reply);
	}
	axis_web_auth_set_time_offset(fx->auth, 3 * G_USEC_PER_SEC);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
}

/* The current password on the account page is guessed at the same way:
 * checks in flight count. */
static void
test_throttle_burst_password_change(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *token = NULL;
	guint checked;
	guint refused;

	SKIP_WITHOUT_DB(fx);
	seed_slow(fx, "alice", ALICE_PASSWORD);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	token = csrf_of(fx, browser);
	{
		const gchar *form[] = {
			"_csrf", token, "current_password", "not-the-password", "new_password", "a-new-password-1",
			"new_password_confirm", "a-new-password-1", NULL
		};

		post_burst(fx, browser, "/account/password", form, 12, 403, &checked, 429, &refused);
	}
	g_test_message("of 12 wrong current passwords sent at once, %u were checked and %u refused", checked, refused);
	g_assert_cmpuint(checked, <=, 6);
	{
		g_autofree gchar *hash = axis_token_hash(client_cookie(browser, "axis_session"));

		g_assert_cmpuint(axis_login_throttle_get_pending(fx->auth->by_session, hash), ==, 0);
	}
}

/*
 * Found by the review of fix-web-hunt-findings: the current password on
 * the account page was held to the username's count, so a stranger who
 * kept the username delayed stopped its owner, logged in, from changing the
 * password.  It is held to the session's own count.
 */
static void
test_password_change_not_locked_by_stranger(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) owner = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	const gchar *fields[] = {
		"current_password", ALICE_PASSWORD, "new_password", "a-new-password-1",
		"new_password_confirm", "a-new-password-1", NULL
	};
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, owner, "alice", ALICE_PASSWORD);
	for (i = 0; i < 7; i++) {
		reply = login(fx, stranger, "alice", "wrong-password", NULL);
		reply_clear(&reply);
	}
	reply = login(fx, stranger, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);

	reply = post_as(fx, owner, "/account/password", fields);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/account?done=password");
	reply_clear(&reply);
}

/* The Set-Cookie value of the remembered device in @reply, or NULL. */
static gchar *
device_cookie(const Reply *reply)
{
	return set_cookie_header(reply, "axis_device");
}

/*
 * Found by the stage 2 hunt of the web layer (W-7): one wrong attempt
 * every fifteen minutes kept a known username locked for its owner too.
 * A browser that logged in to the account before (its remembered device)
 * is held to a count of its own: a stranger's failures, on the username or
 * from the same address, do not lock it out.
 */
static void
test_remembered_device_not_locked_out(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) owner = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	g_autofree gchar *cookie = NULL;
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	/* The owner logs in once: the browser is remembered. */
	reply = login(fx, owner, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	cookie = device_cookie(&reply);
	g_assert_nonnull(cookie);
	assert_contains(cookie, "; HttpOnly");
	assert_contains(cookie, "; SameSite=Strict");
	assert_contains(cookie, "; Max-Age=7776000");
	reply_clear(&reply);
	reply = post_as(fx, owner, "/logout", NULL);
	reply_clear(&reply);

	/* A stranger fills the username's count and the address's (every
	 * client here is 127.0.0.1, as behind a proxy that is not trusted):
	 * six failures for alice, then one each for fifteen other names, 21
	 * from the address in all. */
	for (i = 0; i < 6 + 15; i++) {
		g_autofree gchar *name = i < 6 ? g_strdup("alice") : g_strdup_printf("stranger-%u", i);

		reply = login(fx, stranger, name, "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}
	reply = login(fx, stranger, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);
	reply = login(fx, stranger, "nobody-tried-this", "wrong-password", NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);

	/* The owner, on the remembered browser, still gets in. */
	reply = login(fx, owner, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "audit: login: user=alice*(remembered device)*"), ==, 1);

	/* ... and the stranger still waits: the owner's success did not undo
	 * what the stranger earned. */
	reply = login(fx, stranger, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);
}

/* A remembered device is throttled too, on its own count. */
static void
test_remembered_device_has_its_own_count(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) owner = client_new(NULL);
	g_autoptr(Client) other = client_new(NULL);
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, owner, "alice", ALICE_PASSWORD);
	reply = post_as(fx, owner, "/logout", NULL);
	reply_clear(&reply);

	for (i = 0; i < 6; i++) {
		reply = login(fx, owner, "alice", "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}
	reply = login(fx, owner, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);

	/* Another browser is not held to the device's count. */
	login_ok(fx, other, "alice", ALICE_PASSWORD);
}

/* A device cookie that is altered, or is another account's, is no
 * remembered device. */
static void
test_remembered_device_must_be_signed_and_own(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) owner = client_new(NULL);
	g_autoptr(Client) forger = client_new(NULL);
	g_autoptr(Client) bobs = client_new(NULL);
	g_autofree gchar *altered = NULL;
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	seed(fx, "bob", BOB_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, owner, "alice", ALICE_PASSWORD);
	login_ok(fx, bobs, "bob", BOB_PASSWORD);
	reply = post_as(fx, bobs, "/logout", NULL);
	reply_clear(&reply);

	for (i = 0; i < 6; i++) {
		reply = login(fx, forger, "alice", "wrong-password", NULL);
		reply_clear(&reply);
	}

	/* The owner's cookie with its last character changed. */
	altered = g_strdup(client_cookie(owner, "axis_device"));
	altered[strlen(altered) - 1] = altered[strlen(altered) - 1] == 'A' ? 'B' : 'A';
	client_set_cookie(forger, "axis_device", altered);
	reply = login(fx, forger, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);

	/* Bob's remembered browser is a stranger to alice's account. */
	reply = login(fx, bobs, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);
}

/* The device key is kept in the data directory: a browser stays
 * remembered across a restart. */
static void
test_remembered_device_survives_restart(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) owner = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDataDir) dir = NULL;
	g_autofree gchar *scratch = NULL;
	g_autofree gchar *key_path = NULL;
	GStatBuf st;
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	scratch = g_dir_make_tmp("axis-device-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(scratch, 0700), ==, 0);
	dir = axis_data_dir_open(scratch, &error);
	g_assert_no_error(error);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	g_assert_true(axis_web_auth_load_device_key(fx->auth, axis_data_dir_get_fd(dir), &error));
	g_assert_no_error(error);
	key_path = g_build_filename(scratch, "device-key", NULL);
	g_assert_cmpint(g_stat(key_path, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, 32);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	login_ok(fx, owner, "alice", ALICE_PASSWORD);
	reply = post_as(fx, owner, "/logout", NULL);
	reply_clear(&reply);
	/* The record of devices is kept beside the key. */
	{
		g_autofree gchar *list_path = g_build_filename(scratch, "devices", NULL);

		g_assert_cmpint(g_stat(list_path, &st), ==, 0);
		g_assert_cmpint(st.st_mode & 0777, ==, 0600);
		g_assert_cmpint(st.st_size, >, 0);
	}

	restart(fx);
	g_assert_true(axis_web_auth_load_device_key(fx->auth, axis_data_dir_get_fd(dir), &error));
	g_assert_no_error(error);
	for (i = 0; i < 6; i++) {
		reply = login(fx, stranger, "alice", "wrong-password", NULL);
		reply_clear(&reply);
	}
	reply = login(fx, owner, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);

	/* A damaged key is made anew (the devices are forgotten), never fatal. */
	g_assert_true(g_file_set_contents(key_path, "short", -1, &error));
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "the device key*");
	restart(fx);
	g_assert_true(axis_web_auth_load_device_key(fx->auth, axis_data_dir_get_fd(dir), &error));
	g_assert_cmpint(g_stat(key_path, &st), ==, 0);
	g_assert_cmpint(st.st_size, ==, 32);

	g_unlink(key_path);
	{
		g_autofree gchar *list_path = g_build_filename(scratch, "devices", NULL);

		g_unlink(list_path);
	}
	g_rmdir(scratch);
}

/* Logs @client in as alice and out again: a remembered browser. */
static void
remember(
	Fixture *fx,
	Client  *client
){
	Reply reply;

	login_ok(fx, client, "alice", ALICE_PASSWORD);
	g_assert_nonnull(client_cookie(client, "axis_device"));
	reply = post_as(fx, client, "/logout", NULL);
	reply_clear(&reply);
}

/* A stranger fails until alice's username is delayed. */
static void
delay_alice(Fixture *fx)
{
	g_autoptr(Client) stranger = client_new(NULL);
	Reply reply;
	guint i;

	for (i = 0; i < 6; i++) {
		reply = login(fx, stranger, "alice", "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}
}

/*
 * Found by the review of fix-web-hunt-findings: a device cookie outlived a
 * password change, so someone who once had the password kept browsers with
 * guesses of their own.  The account's epoch is signed into it: a password
 * change (or a disable) ends every remembered device.
 */
static void
test_remembered_device_ends_with_password(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) owner = client_new(NULL);
	const gchar *fields[] = {
		"current_password", ALICE_PASSWORD, "new_password", "a-new-password-1",
		"new_password_confirm", "a-new-password-1", NULL
	};
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, owner, "alice", ALICE_PASSWORD);
	reply = post_as(fx, owner, "/account/password", fields);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	reply = post_as(fx, owner, "/logout", NULL);
	reply_clear(&reply);

	/* The browser still has its device cookie, of the epoch before. */
	g_assert_nonnull(client_cookie(owner, "axis_device"));
	delay_alice(fx);
	reply = login(fx, owner, "alice", "a-new-password-1", NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);
}

/* An account keeps its newest AXIS_DEVICE_MAX_PER_USER remembered
 * browsers; the oldest is a stranger again. */
static void
test_remembered_devices_capped(Fixture *fx, gconstpointer data)
{
	Client *browsers[AXIS_DEVICE_MAX_PER_USER + 1];
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	for (i = 0; i < G_N_ELEMENTS(browsers); i++) {
		browsers[i] = client_new(NULL);
		remember(fx, browsers[i]);
	}
	g_assert_cmpuint(axis_web_auth_count_devices(fx->auth, user_id(fx, "alice")), ==, AXIS_DEVICE_MAX_PER_USER);

	delay_alice(fx);
	reply = login(fx, browsers[0], "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);
	reply = login(fx, browsers[AXIS_DEVICE_MAX_PER_USER], "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);

	for (i = 0; i < G_N_ELEMENTS(browsers); i++)
		client_free(browsers[i]);
}

/*
 * Found by the review: each remembered browser had a count of its own and
 * nothing else, so browsers multiplied guesses.  Failures on all of an
 * account's devices together count toward one ceiling of the account's,
 * which no stranger's failure reaches.
 */
static void
test_remembered_devices_share_a_ceiling(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) a = client_new(NULL);
	g_autoptr(Client) b = client_new(NULL);
	g_autoptr(Client) c = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	Client *const order[] = { a, a, a, a, b, b, b, b, c, c, c };
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	remember(fx, a);
	remember(fx, b);
	remember(fx, c);

	/* Eleven failures over three browsers, none more than four on one. */
	for (i = 0; i < G_N_ELEMENTS(order); i++) {
		reply = login(fx, order[i], "alice", "wrong-password", NULL);
		g_assert_cmpuint(reply.status, ==, 401);
		reply_clear(&reply);
	}
	reply = login(fx, c, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 429);
	reply_clear(&reply);

	/* The strangers' count is another: untouched. */
	login_ok(fx, stranger, "alice", ALICE_PASSWORD);
}

/*
 * The other half of W-7: behind a proxy that is not trusted, every client
 * is the proxy's address.  When every forwarded request has come from one
 * private address and no proxy is trusted, the log says so once, and so do
 * the administrators' pages.
 */
static void
test_untrusted_proxy_hinted(Fixture *fx, gconstpointer data)
{
	static const gchar *const forwarded[] = { "X-Forwarded-For", "203.0.113.9", NULL };
	g_autoptr(Client) admin = client_new(NULL);
	g_autoptr(Client) through = client_new(forwarded);
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "carol", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	start(fx);
	login_ok(fx, admin, "carol", ALICE_PASSWORD);
	reply = get(fx, admin, "/admin/proxies");
	assert_lacks(reply.body, "no reverse proxy is trusted");
	reply_clear(&reply);

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "Every request so far came through one private address*");
	for (i = 0; i < 12; i++) {
		reply = get(fx, through, "/login");
		reply_clear(&reply);
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "Every request so far came through one private "
		"address, 127.0.0.1*"), ==, 1);

	reply = get(fx, admin, "/admin/proxies");
	assert_contains(reply.body, "no reverse proxy is trusted");
	assert_contains(reply.body, "AXIS_TRUSTED_PROXIES");
	reply_clear(&reply);
}

/* With a proxy trusted, no hint: its clients are told apart. */
static void
test_trusted_proxy_not_hinted(Fixture *fx, gconstpointer data)
{
	static const gchar *const forwarded[] = { "X-Forwarded-For", "203.0.113.9", NULL };
	g_autoptr(Client) through = client_new(forwarded);
	Reply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	fx->trusted = "127.0.0.1";
	start(fx);
	for (i = 0; i < 12; i++) {
		reply = get(fx, through, "/login");
		reply_clear(&reply);
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "Every request so far came through one private address*"), ==, 0);
	g_assert_null(axis_web_auth_forwarding_hint(fx->auth));
}

/* ── Cookies ────────────────────────────────────────────────────── */

/* Spec: Cookie attributes */
static void
test_cookie_attributes(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *cookie = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	cookie = set_cookie_header(&reply, "axis_session");
	g_assert_nonnull(cookie);
	assert_contains(cookie, "; HttpOnly");
	assert_contains(cookie, "; SameSite=Lax");
	assert_contains(cookie, "; Path=/");
	assert_contains(cookie, "; Max-Age=7776000");
	/* Plain HTTP from a client that is not a trusted proxy. */
	assert_lacks(cookie, "Secure");
	assert_lacks(cookie, "Domain");
	g_assert_null(set_cookie_header(&reply, "__Host-axis_session"));
	/* The token itself: 32 random bytes. */
	g_assert_cmpuint(strlen(client_cookie(browser, "axis_session")), ==, AXIS_TOKEN_LEN);
	reply_clear(&reply);
}

static const gchar *const via_proxy_https[] = {
	"X-Forwarded-For", "203.0.113.9", "X-Forwarded-Proto", "https", NULL
};

/* Spec: Secure over HTTPS (as a trusted proxy reports it) */
static void
test_cookie_secure_behind_trusted_proxy(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(via_proxy_https);
	g_autofree gchar *cookie = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	fx->trusted = "127.0.0.1";
	start(fx);

	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	cookie = set_cookie_header(&reply, "__Host-axis_session");
	g_assert_nonnull(cookie);
	assert_contains(cookie, "; Secure");
	assert_contains(cookie, "; HttpOnly");
	assert_contains(cookie, "; SameSite=Lax");
	assert_contains(cookie, "; Path=/");
	reply_clear(&reply);

	/* The secure cookie is what the session is read from over HTTPS. */
	g_assert_true(logged_in(fx, browser));

	/* The same browser's cookie over plain HTTP is not looked at. */
	{
		g_autoptr(Client) plain = client_new(NULL);

		client_set_cookie(plain, "__Host-axis_session", client_cookie(browser, "__Host-axis_session"));
		g_assert_false(logged_in(fx, plain));
	}
}

static void
test_cookie_secure_not_from_untrusted(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(via_proxy_https);
	g_autofree gchar *cookie = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	cookie = set_cookie_header(&reply, "axis_session");
	g_assert_nonnull(cookie);
	assert_lacks(cookie, "Secure");
	reply_clear(&reply);
}

static void
test_cookie_secure_always(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *cookie = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	fx->secure = AXIS_SECURE_COOKIES_ALWAYS;
	start(fx);

	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	cookie = set_cookie_header(&reply, "__Host-axis_session");
	g_assert_nonnull(cookie);
	assert_contains(cookie, "; Secure");
	reply_clear(&reply);
}

/* ── Sessions ───────────────────────────────────────────────────── */

/* Spec: Session survives restart */
static void
test_session_survives_restart(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	restart(fx);

	reply = get(fx, browser, "/account");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "alice");
	reply_clear(&reply);
}

/* Spec: Logout */
static void
test_session_logout(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *old = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
	old = g_strdup(client_cookie(browser, "axis_session"));

	reply = post_as(fx, browser, "/logout", NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(header(&reply, "Location"), ==, "/login");
	g_assert_null(client_cookie(browser, "axis_session"));
	reply_clear(&reply);

	/* The old cookie, kept by someone, is no longer a session ... */
	client_set_cookie(browser, "axis_session", old);
	g_assert_false(logged_in(fx, browser));

	/* ... nor after a restart. */
	axis_web_auth_wait_writes(fx->auth);
	assert_query(fx, "SELECT count(*) FROM web_session", "0");
	restart(fx);
	g_assert_false(logged_in(fx, browser));
}

/* Spec: Idle expiry */
static void
test_session_idle_expiry(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autoptr(Client) later = client_new(NULL);

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	/* 29 days of not being used is fine ... */
	axis_web_auth_set_time_offset(fx->auth, 29 * DAY);
	g_assert_true(logged_in(fx, browser));
	/* ... and using it moved the 30 days on. */
	axis_web_auth_set_time_offset(fx->auth, 58 * DAY);
	g_assert_true(logged_in(fx, browser));
	/* Over 30 days without use, it is gone. */
	axis_web_auth_set_time_offset(fx->auth, 89 * DAY);
	g_assert_false(logged_in(fx, browser));

	/* The same judged from the database at start. */
	axis_web_auth_set_time_offset(fx->auth, 0);
	login_ok(fx, later, "alice", ALICE_PASSWORD);
	axis_web_auth_wait_writes(fx->auth);
	axis_test_db_exec(fx->db, "UPDATE web_session SET last_seen_at = now() - interval '31 days'");
	restart(fx);
	g_assert_false(logged_in(fx, later));
}

static void
test_session_absolute_expiry(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	guint day;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	/* Used every 20 days, it still ends after 90 days in all. */
	for (day = 20; day <= 80; day += 20) {
		axis_web_auth_set_time_offset(fx->auth, day * DAY);
		g_assert_true(logged_in(fx, browser));
	}
	axis_web_auth_set_time_offset(fx->auth, 91 * DAY);
	g_assert_false(logged_in(fx, browser));
}

/* Spec: Stored session is not a credential */
static void
test_session_stored_not_credential(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *row = NULL;
	g_autofree gchar *csrf = NULL;
	static const gchar *const columns[] = {
		"token_hash", "public_id", "user_id::text", "user_epoch::text", "client_address",
		"user_agent", "created_at::text", "last_seen_at::text", NULL
	};
	guint i;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
	csrf = csrf_of(fx, browser);
	axis_web_auth_wait_writes(fx->auth);

	row = query(fx, "SELECT row_to_json(s)::text FROM web_session AS s");
	g_assert_nonnull(row);
	g_assert_null(strstr(row, client_cookie(browser, "axis_session")));
	g_assert_null(strstr(row, csrf));

	/* Nothing read from the table opens a session. */
	for (i = 0; columns[i] != NULL; i++) {
		g_autofree gchar *sql = g_strdup_printf("SELECT %s FROM web_session", columns[i]);
		g_autofree gchar *value = query(fx, sql);
		g_autoptr(Client) thief = client_new(NULL);

		client_set_cookie(thief, "axis_session", value != NULL ? value : "");
		g_assert_false(logged_in(fx, thief));
		client_set_cookie(thief, "__Host-axis_session", value != NULL ? value : "");
		g_assert_false(logged_in(fx, thief));
	}

	/* The real cookie still works. */
	g_assert_true(logged_in(fx, browser));
}

/* The revoke form on @html that is not for the current session. */
static gchar *
revoke_action(const gchar *html)
{
	const gchar *marker = "action=\"/account/sessions/";
	const gchar *start = strstr(html, marker);

	if (start == NULL)
		return NULL;
	start += strlen("action=\"");
	return g_strndup(start, strchr(start, '"') - start);
}

/* Spec: User revokes another session */
static void
test_session_revoke_other(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) laptop = client_new(NULL);
	g_autoptr(Client) phone = client_new(NULL);
	g_autofree gchar *action = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, laptop, "alice", ALICE_PASSWORD);
	login_ok(fx, phone, "alice", ALICE_PASSWORD);

	reply = get(fx, laptop, "/account");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "This session");
	action = revoke_action(reply.body);
	g_assert_nonnull(action);
	reply_clear(&reply);

	reply = post_as(fx, laptop, action, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/account?done=revoked");
	reply_clear(&reply);

	g_assert_false(logged_in(fx, phone));
	g_assert_true(logged_in(fx, laptop));
	reply = get(fx, laptop, "/account");
	g_assert_null(revoke_action(reply.body));
	reply_clear(&reply);

	/* Revoking it again, or someone else's, finds nothing. */
	reply = post_as(fx, laptop, action, NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);

	axis_web_auth_wait_writes(fx->auth);
	assert_query(fx, "SELECT count(*) FROM web_session", "1");
}

static void
test_session_revoke_not_others_users(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) alice = client_new(NULL);
	g_autoptr(Client) alice_phone = client_new(NULL);
	g_autoptr(Client) bob = client_new(NULL);
	g_autofree gchar *action = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	seed(fx, "bob", BOB_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, alice, "alice", ALICE_PASSWORD);
	login_ok(fx, alice_phone, "alice", ALICE_PASSWORD);
	login_ok(fx, bob, "bob", BOB_PASSWORD);

	reply = get(fx, alice, "/account");
	action = revoke_action(reply.body);
	reply_clear(&reply);

	/* Bob cannot end one of Alice's sessions. */
	reply = post_as(fx, bob, action, NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);
	g_assert_true(logged_in(fx, alice_phone));
}

/* ── Password change ────────────────────────────────────────────── */

/* Spec: Change with correct current password */
static void
test_password_change(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) laptop = client_new(NULL);
	g_autoptr(Client) phone = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	const gchar *fields[] = { "current_password", ALICE_PASSWORD,
		"new_password", "a-whole-new-password", "new_password_confirm", "a-whole-new-password", NULL };
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, laptop, "alice", ALICE_PASSWORD);
	login_ok(fx, phone, "alice", ALICE_PASSWORD);

	reply = post_as(fx, laptop, "/account/password", fields);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/account?done=password");
	reply_clear(&reply);

	/* This session goes on; the other has ended. */
	g_assert_true(logged_in(fx, laptop));
	g_assert_false(logged_in(fx, phone));

	reply = login(fx, stranger, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	login_ok(fx, phone, "alice", "a-whole-new-password");

	/* And so after a restart. */
	axis_web_auth_wait_writes(fx->auth);
	restart(fx);
	g_assert_true(logged_in(fx, laptop));
	g_assert_true(logged_in(fx, phone));
}

/* Spec: Wrong current password */
static void
test_password_change_wrong_current(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) laptop = client_new(NULL);
	g_autoptr(Client) phone = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	g_autoptr(Client) again = client_new(NULL);
	const gchar *fields[] = { "current_password", "not-my-password",
		"new_password", "a-whole-new-password", "new_password_confirm", "a-whole-new-password", NULL };
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, laptop, "alice", ALICE_PASSWORD);
	login_ok(fx, phone, "alice", ALICE_PASSWORD);
	before = query(fx, "SELECT password_hash || auth_epoch FROM app_user");

	reply = post_as(fx, laptop, "/account/password", fields);
	g_assert_cmpuint(reply.status, ==, 403);
	assert_contains(reply.body, "current password is not correct");
	reply_clear(&reply);

	after = query(fx, "SELECT password_hash || auth_epoch FROM app_user");
	g_assert_cmpstr(before, ==, after);
	g_assert_true(logged_in(fx, phone));
	reply = login(fx, stranger, "alice", "a-whole-new-password", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	login_ok(fx, again, "alice", ALICE_PASSWORD);
}

/* ── CSRF ───────────────────────────────────────────────────────── */

/* Spec: Missing token, Token from another session, Cross-site request */
static void
test_csrf_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) laptop = client_new(NULL);
	g_autoptr(Client) phone = client_new(NULL);
	g_autofree gchar *mine = NULL;
	g_autofree gchar *theirs = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, laptop, "alice", ALICE_PASSWORD);
	login_ok(fx, phone, "alice", ALICE_PASSWORD);
	mine = csrf_of(fx, laptop);
	theirs = csrf_of(fx, phone);
	before = query(fx, "SELECT password_hash FROM app_user");

	{
		const gchar *missing[] = { "current_password", ALICE_PASSWORD, "new_password",
			"a-whole-new-password", "new_password_confirm", "a-whole-new-password", NULL };
		const gchar *other[] = { "_csrf", theirs, "current_password", ALICE_PASSWORD, "new_password",
			"a-whole-new-password", "new_password_confirm", "a-whole-new-password", NULL };
		const gchar *right[] = { "_csrf", mine, "current_password", ALICE_PASSWORD, "new_password",
			"a-whole-new-password", "new_password_confirm", "a-whole-new-password", NULL };
		const gchar *const cross[] = { "Origin", "https://evil.example", NULL };
		const gchar *const cross_site[] = { "Sec-Fetch-Site", "cross-site", NULL };

		reply = send(fx, laptop, "POST", "/account/password", missing, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
		reply = send(fx, laptop, "POST", "/account/password", other, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
		reply = send(fx, laptop, "POST", "/account/password", right, cross);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
		reply = send(fx, laptop, "POST", "/account/password", right, cross_site);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}

	/* Nothing changed: same hash, both sessions alive. */
	after = query(fx, "SELECT password_hash FROM app_user");
	g_assert_cmpstr(before, ==, after);
	g_assert_true(logged_in(fx, phone));

	/* Logging out is state-changing too. */
	reply = send(fx, laptop, "POST", "/logout", NULL, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	g_assert_true(logged_in(fx, laptop));

	/* The login form without its pre-session token. */
	{
		g_autoptr(Client) stranger = client_new(NULL);
		const gchar *form[] = { "username", "alice", "password", ALICE_PASSWORD, NULL };

		reply = send(fx, stranger, "POST", "/login", form, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		g_assert_null(client_cookie(stranger, "axis_session"));
		reply_clear(&reply);
	}

	/* ... or with another browser's. */
	{
		g_autoptr(Client) stranger = client_new(NULL);
		g_autoptr(Client) victim = client_new(NULL);
		g_autofree gchar *victims = form_token(fx, victim, "/login");
		g_autofree gchar *own = form_token(fx, stranger, "/login");
		const gchar *form[] = { "_csrf", victims, "username", "alice", "password", ALICE_PASSWORD, NULL };

		g_assert_cmpstr(victims, !=, own);
		reply = send(fx, stranger, "POST", "/login", form, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}
}

/* Spec: htmx requests carry the token */
static void
test_csrf_htmx(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *origin = NULL;
	g_autofree gchar *token = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
	origin = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->port);

	/* The page the shell rendered gives htmx the token ... */
	token = csrf_of(fx, browser);

	/* ... which htmx sends as a header, with no form field. */
	{
		const gchar *const htmx[] = { "HX-Request", "true", "X-CSRF-Token", token,
			"Origin", origin, "Sec-Fetch-Site", "same-origin", NULL };

		reply = send(fx, browser, "POST", "/movies/poke", NULL, htmx);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Poked.");
		reply_clear(&reply);
		g_assert_cmpuint(fx->pokes, ==, 1);

		reply = send(fx, browser, "POST", "/logout", NULL, htmx);
		g_assert_cmpuint(reply.status, ==, 200);
		g_assert_cmpstr(header(&reply, "HX-Redirect"), ==, "/login");
		reply_clear(&reply);
	}
	g_assert_false(logged_in(fx, browser));
}

/* ── Roles ──────────────────────────────────────────────────────── */

/* Spec: Non-admin refused */
static void
test_roles_non_admin(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *path = NULL;
	const gchar *create[] = { "username", "mallory", "password", BOB_PASSWORD, "role", "admin", NULL };
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	seed(fx, "bob", BOB_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "bob", BOB_PASSWORD);

	reply = get(fx, browser, "/admin/users");
	g_assert_cmpuint(reply.status, ==, 403);
	assert_lacks(reply.body, "alice");
	reply_clear(&reply);

	reply = post_as(fx, browser, "/admin/users", create);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);

	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/disable", user_id(fx, "alice"));
	reply = post_as(fx, browser, path, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);

	/* Nor can bob make himself an administrator. */
	{
		const gchar *fields[] = { "role", "admin", NULL };

		g_free(path);
		path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/role", user_id(fx, "bob"));
		reply = post_as(fx, browser, path, fields);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}

	assert_query(fx, "SELECT string_agg(username || ':' || role || ':' || disabled, ',' ORDER BY username) "
		"FROM app_user", "alice:admin:false,bob:user:false");
}

/* Spec: Last administrator is protected */
static void
test_roles_last_admin(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autofree gchar *base = NULL;
	g_autofree gchar *disable = NULL;
	g_autofree gchar *remove = NULL;
	g_autofree gchar *role = NULL;
	const gchar *demote[] = { "role", "user", NULL };
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	/* A disabled administrator does not count. */
	seed(fx, "carol", BOB_PASSWORD, AXIS_USER_ROLE_ADMIN, TRUE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	base = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT, user_id(fx, "alice"));
	disable = g_strconcat(base, "/disable", NULL);
	remove = g_strconcat(base, "/delete", NULL);
	role = g_strconcat(base, "/role", NULL);

	reply = post_as(fx, browser, disable, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "only enabled administrator");
	reply_clear(&reply);
	reply = post_as(fx, browser, remove, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	reply_clear(&reply);
	reply = post_as(fx, browser, role, demote);
	g_assert_cmpuint(reply.status, ==, 409);
	reply_clear(&reply);

	assert_query(fx, "SELECT role || ':' || disabled FROM app_user WHERE username = 'alice'", "admin:false");
	g_assert_true(logged_in(fx, browser));

	/* With a second enabled administrator, the first may go. */
	{
		g_autofree gchar *enable = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/enable",
			user_id(fx, "carol"));

		reply = post_as(fx, browser, enable, NULL);
		g_assert_cmpuint(reply.status, ==, 303);
		reply_clear(&reply);
	}
	reply = post_as(fx, browser, role, demote);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	assert_query(fx, "SELECT role FROM app_user WHERE username = 'alice'", "user");

	/* And now carol is the last one. */
	{
		g_autoptr(Client) carol = client_new(NULL);
		g_autofree gchar *carol_disable = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/disable",
			user_id(fx, "carol"));

		login_ok(fx, carol, "carol", BOB_PASSWORD);
		reply = post_as(fx, carol, carol_disable, NULL);
		g_assert_cmpuint(reply.status, ==, 409);
		reply_clear(&reply);
	}
}

typedef struct
{
	AxisDbPool *pool;
	gint64      target;
	gint       *done;
	gint       *refused;
	GMutex     *start_lock;
} DisableArgs;

static gpointer
disable_thread(gpointer data)
{
	DisableArgs *args = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisUser) user = NULL;

	g_mutex_lock(args->start_lock);
	g_mutex_unlock(args->start_lock);
	user = axis_user_store_set_disabled(args->pool, args->target, TRUE, &error);
	if (user != NULL)
		g_atomic_int_inc(args->done);
	else if (g_error_matches(error, AXIS_USER_ERROR, AXIS_USER_ERROR_LAST_ADMIN))
		g_atomic_int_inc(args->refused);
	else
		g_error("unexpected: %s", error->message);

	return NULL;
}

/* Spec: Last administrator is protected (two disabling each other at once) */
static void
test_roles_last_admin_race(Fixture *fx, gconstpointer data)
{
	guint round;

	SKIP_WITHOUT_DB(fx);

	for (round = 0; round < 10; round++) {
		gint64 first;
		gint64 second;
		gint done = 0;
		gint refused = 0;
		GMutex start_lock;
		DisableArgs a;
		DisableArgs b;
		GThread *ta;
		GThread *tb;

		axis_test_db_exec(fx->db, "DELETE FROM app_user");
		first = seed(fx, "first", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
		second = seed(fx, "second", BOB_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);

		g_mutex_init(&start_lock);
		g_mutex_lock(&start_lock);
		a.pool = fx->pool;
		a.target = first;
		a.done = &done;
		a.refused = &refused;
		a.start_lock = &start_lock;
		b = a;
		b.target = second;
		ta = g_thread_new("disable-a", disable_thread, &a);
		tb = g_thread_new("disable-b", disable_thread, &b);
		g_mutex_unlock(&start_lock);
		g_thread_join(ta);
		g_thread_join(tb);
		g_mutex_clear(&start_lock);

		g_assert_cmpint(done, ==, 1);
		g_assert_cmpint(refused, ==, 1);
		assert_query(fx, "SELECT count(*) FROM app_user WHERE role = 'admin' AND NOT disabled", "1");
	}
}

/* Spec: Disabling a user ends their sessions */
static void
test_roles_disable_ends_sessions(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) admin = client_new(NULL);
	g_autoptr(Client) bob = client_new(NULL);
	g_autoptr(Client) bob_phone = client_new(NULL);
	g_autofree gchar *path = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	seed(fx, "bob", BOB_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, admin, "alice", ALICE_PASSWORD);
	login_ok(fx, bob, "bob", BOB_PASSWORD);
	login_ok(fx, bob_phone, "bob", BOB_PASSWORD);

	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/disable", user_id(fx, "bob"));
	reply = post_as(fx, admin, path, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/admin/users?done=disabled");
	reply_clear(&reply);

	/* At once, without a restart. */
	g_assert_false(logged_in(fx, bob));
	g_assert_false(logged_in(fx, bob_phone));
	reply = login(fx, bob, "bob", BOB_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	assert_contains(reply.body, LOGIN_FAILED);
	reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM web_session AS s JOIN app_user AS u ON u.id = s.user_id "
		"WHERE u.username = 'bob'", "0");

	/* Enabled again, the old sessions stay dead (after a restart too). */
	g_free(path);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/enable", user_id(fx, "bob"));
	reply = post_as(fx, admin, path, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	g_assert_false(logged_in(fx, bob_phone));
	restart(fx);
	g_assert_false(logged_in(fx, bob_phone));
	login_ok(fx, bob, "bob", BOB_PASSWORD);
}

static void
test_admin_manage_accounts(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) admin = client_new(NULL);
	g_autoptr(Client) erin = client_new(NULL);
	const gchar *create[] = { "username", "Erin", "password", BOB_PASSWORD, "role", "user", NULL };
	const gchar *reset[] = { "password", "erins-new-password", NULL };
	const gchar *promote[] = { "role", "admin", NULL };
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	start(fx);
	login_ok(fx, admin, "alice", ALICE_PASSWORD);

	reply = post_as(fx, admin, "/admin/users", create);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/admin/users?done=created");
	reply_clear(&reply);
	reply = post_as(fx, admin, "/admin/users", create);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "already exists");
	reply_clear(&reply);

	reply = get(fx, admin, "/admin/users");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">erin<");
	assert_contains(reply.body, "hx-confirm");
	reply_clear(&reply);

	login_ok(fx, erin, "erin", BOB_PASSWORD);
	base = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT, user_id(fx, "erin"));

	/* A new password set by an administrator ends the account's sessions. */
	path = g_strconcat(base, "/password", NULL);
	reply = post_as(fx, admin, path, reset);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	g_assert_false(logged_in(fx, erin));
	login_ok(fx, erin, "erin", "erins-new-password");

	g_free(path);
	path = g_strconcat(base, "/role", NULL);
	reply = post_as(fx, admin, path, promote);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	/* A change of the role ends every session of the account
	 * (add-request-portal): the new role applies from the next login. */
	g_assert_false(logged_in(fx, erin));
	login_ok(fx, erin, "erin", "erins-new-password");
	reply = get(fx, erin, "/admin/users");
	g_assert_cmpuint(reply.status, ==, 200);
	reply_clear(&reply);

	g_free(path);
	path = g_strconcat(base, "/delete", NULL);
	reply = post_as(fx, admin, path, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	g_assert_false(logged_in(fx, erin));
	assert_query(fx, "SELECT count(*) FROM app_user WHERE username = 'erin'", "0");

	/* An id that is not an account. */
	reply = post_as(fx, admin, "/admin/users/999999/disable", NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);
	reply = post_as(fx, admin, "/admin/users/01/disable", NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);
}

/* ── Trusted proxies ────────────────────────────────────────────── */

static const gchar *const via_proxy[] = { "X-Forwarded-For", "203.0.113.9", NULL };

/* Spec: Header from an untrusted peer is ignored */
static void
test_proxy_untrusted_ignored(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(via_proxy);
	g_autofree gchar *line = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);

	reply = login(fx, browser, "alice", "wrong-password", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "audit: login failed*");
	assert_contains(line, "client=127.0.0.1");
	assert_lacks(line, "203.0.113.9");

	login_ok(fx, browser, "alice", ALICE_PASSWORD);
	reply = get(fx, browser, "/account");
	assert_contains(reply.body, "127.0.0.1");
	assert_lacks(reply.body, "203.0.113.9");
	reply_clear(&reply);
}

/* Spec: Header from a trusted proxy is used */
static void
test_proxy_trusted_used(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(via_proxy_https);
	g_autofree gchar *line = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	fx->trusted = "127.0.0.0/8";
	start(fx);

	reply = login(fx, browser, "alice", "wrong-password", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "audit: login failed*");
	assert_contains(line, "client=203.0.113.9");

	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	/* Treated as HTTPS. */
	{
		g_autofree gchar *cookie = set_cookie_header(&reply, "__Host-axis_session");

		g_assert_nonnull(cookie);
		assert_contains(cookie, "; Secure");
	}
	reply_clear(&reply);

	reply = get(fx, browser, "/account");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "203.0.113.9");
	reply_clear(&reply);

	/* Throttling counts the forwarded address, not the proxy's. */
	{
		const gchar *const elsewhere[] = { "X-Forwarded-For", "198.51.100.1",
			"X-Forwarded-Proto", "https", NULL };
		g_autoptr(Client) other = client_new(elsewhere);
		guint i;

		for (i = 0; i < 21; i++) {
			g_autofree gchar *name = g_strdup_printf("nobody%u", i);

			reply = login(fx, other, name, "wrong-password", NULL);
			g_assert_cmpuint(reply.status, ==, 401);
			reply_clear(&reply);
		}
		reply = login(fx, other, "alice", ALICE_PASSWORD, NULL);
		g_assert_cmpuint(reply.status, ==, 429);
		reply_clear(&reply);

		/* The first client's address, behind the same proxy, is not
		 * held back. */
		{
			g_autoptr(Client) again = client_new(via_proxy_https);

			reply = login(fx, again, "alice", ALICE_PASSWORD, NULL);
			g_assert_cmpuint(reply.status, ==, 303);
			reply_clear(&reply);
		}
	}
}

static const gchar *const via_proxy_host[] = {
	"X-Forwarded-For", "203.0.113.9", "X-Forwarded-Proto", "https", "X-Forwarded-Host", "Axis.Example", NULL
};

/* A POST to /movies/poke with the session's CSRF token, as htmx sends it,
 * claiming to come from @origin; the status. */
static guint
poke_from(
	Fixture     *fx,
	Client      *browser,
	const gchar *origin
){
	g_autofree gchar *token = csrf_of(fx, browser);
	const gchar *const htmx[] = { "HX-Request", "true", "X-CSRF-Token", token, "Origin", origin, NULL };
	Reply reply = send(fx, browser, "POST", "/movies/poke", NULL, htmx);
	guint status = reply.status;

	reply_clear(&reply);
	return status;
}

/*
 * Spec (deployment): Through the proxy -- behind a listed proxy, the
 * cross-site check compares the browser's Origin with the public scheme and
 * host the proxy forwards (X-Forwarded-Proto, X-Forwarded-Host), not with
 * the address the proxy connected to.
 */
static void
test_proxy_forwarded_host_trusted(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(via_proxy_host);
	g_autofree gchar *direct = NULL;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	fx->trusted = "127.0.0.1";
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
	direct = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->port);

	g_assert_cmpuint(poke_from(fx, browser, "https://axis.example"), ==, 200);
	g_assert_cmpuint(poke_from(fx, browser, "https://AXIS.example:443"), ==, 200);
	/* Not the inner address, not plain http, not another host. */
	g_assert_cmpuint(poke_from(fx, browser, direct), ==, 403);
	g_assert_cmpuint(poke_from(fx, browser, "http://axis.example"), ==, 403);
	g_assert_cmpuint(poke_from(fx, browser, "https://evil.example"), ==, 403);
	g_assert_cmpuint(fx->pokes, ==, 2);
}

/* Spec (deployment): Not from a proxy -- the same headers from a peer that
 * is not listed are ignored: the request's own Host and scheme stand. */
static void
test_proxy_forwarded_host_untrusted(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(via_proxy_host);
	g_autofree gchar *direct = NULL;
	g_autofree gchar *line = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	fx->trusted = "10.0.0.0/8, ::ffff:192.0.2.1";
	start(fx);

	reply = login(fx, browser, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	/* Not Secure: the proxy's https was not believed. */
	g_assert_null(set_cookie_header(&reply, "__Host-axis_session"));
	reply_clear(&reply);
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "audit: login:*");
	assert_contains(line, "client=127.0.0.1");
	assert_lacks(line, "203.0.113.9");
	direct = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->port);

	g_assert_cmpuint(poke_from(fx, browser, "https://axis.example"), ==, 403);
	g_assert_cmpuint(poke_from(fx, browser, direct), ==, 200);
	g_assert_cmpuint(fx->pokes, ==, 1);
}

/* Spec (daemon-core): Effective list is logged */
static void
test_proxy_list_logged(Fixture *fx, gconstpointer data)
{
	SKIP_WITHOUT_DB(fx);
	fx->trusted = "127.0.0.0/8, ::1";
	start(fx);

	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "trusted proxies: 127.0.0.0/8, ::1"), ==, 1);
}

static gint64 name_clock_now;

static gint64
name_clock(gpointer data)
{
	return name_clock_now;
}

/* Spec (daemon-core): A proxy named by host name is believed again at its
 * new address after a request from it */
static void
test_proxy_name_follows_address(Fixture *fx, gconstpointer data)
{
	static const gchar *const elsewhere[] = { "10.9.9.9", NULL };
	static const gchar *const here[] = { "127.0.0.1", NULL };
	g_autoptr(Client) browser = client_new(via_proxy);
	g_autoptr(Client) admin = client_new(NULL);
	g_autoptr(GResolver) system = NULL;
	g_autoptr(GResolver) dns = NULL;
	g_autofree gchar *line = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "trusted proxy name*");
	system = g_resolver_get_default();
	dns = axis_test_resolver_new();
	g_resolver_set_default(dns);
	axis_test_resolver_set(dns, "edge-proxy", elsewhere);

	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	fx->trusted = "edge-proxy";
	start(fx);
	axis_test_resolver_drain();
	/* Time of the test's own, past the rate limit of the first lookups. */
	name_clock_now = g_get_monotonic_time() + G_GINT64_CONSTANT(3600) * G_USEC_PER_SEC;
	axis_proxy_list_set_clock(axis_web_auth_get_trusted(fx->auth), name_clock, NULL);

	/* The name stands for another address: the request's headers are ignored. */
	reply = login(fx, browser, "alice", "wrong-password", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "audit: login failed*");
	assert_contains(line, "client=127.0.0.1");

	/* The proxy is now at this address.  That request asked for a new lookup
	 * and did not wait for it; the next is believed. */
	axis_test_resolver_set(dns, "edge-proxy", here);
	name_clock_now += 2 * AXIS_PROXY_RESOLVE_MIN_INTERVAL_US;
	reply = login(fx, browser, "alice", "wrong-password", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	axis_test_resolver_drain();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "trusted proxy name*");
	reply = login(fx, browser, "alice", "wrong-password", NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	reply_clear(&reply);
	g_free(line);
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "audit: login failed*");
	assert_contains(line, "client=203.0.113.9");

	/* The Reverse proxies page says what the name stands for. */
	login_ok(fx, admin, "alice", ALICE_PASSWORD);
	reply = get(fx, admin, "/admin/proxies");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "edge-proxy");
	assert_contains(reply.body, "Host name");
	assert_contains(reply.body, "Resolved");
	assert_contains(reply.body, "127.0.0.1");
	reply_clear(&reply);

	stop(fx);
	g_resolver_set_default(system);
}

/* Spec (web-auth, slice P4, after its second review): A login that arrives
 * with a forwarding header while a trusted proxy's name has no address.
 * With a portal configured, such a login may be the proxy's, for a client
 * nobody can check: it is refused as a wrong password is, without a hash,
 * until the name has its address.  A login with no forwarding header is
 * taken meanwhile, and so is everything while no portal is configured. */
static void
test_portal_peers_unresolved_name(Fixture *fx, gconstpointer data)
{
	static const gchar *const here[] = { "127.0.0.1", NULL };
	g_autoptr(Client) early = client_new(via_proxy);
	g_autoptr(Client) direct = client_new(NULL);
	g_autoptr(Client) unset = client_new(via_proxy);
	g_autoptr(Client) browser = client_new(via_proxy);
	g_autoptr(GResolver) system = NULL;
	g_autoptr(GResolver) dns = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *line = NULL;
	Reply reply;
	guint hashes;

	SKIP_WITHOUT_DB(fx);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "trusted proxy name*");
	system = g_resolver_get_default();
	dns = axis_test_resolver_new();
	g_resolver_set_default(dns);
	axis_test_resolver_set(dns, "edge-proxy", here);
	/* The lookup is not answered yet: the name stands for no address. */
	axis_test_resolver_hold(dns, TRUE);

	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	fx->trusted = "edge-proxy";
	start(fx);
	g_assert_true(axis_proxy_list_has_unresolved_name(axis_web_auth_get_trusted(fx->auth)));
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "198.51.100.20", &error));
	g_assert_no_error(error);

	/* The right password, forwarded for an address that is nobody's. */
	hashes = axis_password_verify_count();
	reply = login(fx, early, "alice", ALICE_PASSWORD, NULL);
	g_assert_cmpuint(reply.status, ==, 401);
	assert_contains(reply.body, "The username or password is incorrect.");
	g_assert_null(client_cookie(early, "axis_session"));
	reply_clear(&reply);
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "audit: login refused: it arrived with X-Forwarded-For "
		"from 127.0.0.1, which is not a trusted proxy now, while a host name in server.trusted_proxies*"), ==, 1);
	g_assert_cmpuint(axis_web_auth_portal_logins_barred(fx->auth, NULL), ==, 1);

	/* Directly, with no forwarding header: taken. */
	login_ok(fx, direct, "alice", ALICE_PASSWORD);
	/* With no portal configured the rule is not there at all. */
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "", &error));
	g_assert_no_error(error);
	login_ok(fx, unset, "alice", ALICE_PASSWORD);
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "198.51.100.20", &error));
	g_assert_no_error(error);

	/* The name has its address: the proxy is believed, and its login taken. */
	axis_test_resolver_hold(dns, FALSE);
	axis_test_resolver_release(dns);
	axis_test_resolver_drain();
	g_assert_false(axis_proxy_list_has_unresolved_name(axis_web_auth_get_trusted(fx->auth)));
	login_ok(fx, browser, "alice", ALICE_PASSWORD);
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "audit: login: user=alice client=203.0.113.9*");
	g_assert_nonnull(line);
	g_assert_cmpuint(axis_web_auth_portal_logins_barred(fx->auth, NULL), ==, 1);

	stop(fx);
	g_resolver_set_default(system);
}

/* ── Themes ─────────────────────────────────────────────────────── */

/* The theme id written on the root element of @html. */
static gchar *
html_theme(const gchar *html)
{
	const gchar *start = strstr(html, "<html");
	const gchar *end;
	const gchar *attr;
	const gchar *quote;

	g_assert_nonnull(start);
	end = strchr(start, '>');
	g_assert_nonnull(end);
	attr = strstr(start, " data-theme=\"");
	g_assert_true(attr != NULL && attr < end);
	attr += strlen(" data-theme=\"");
	quote = strchr(attr, '"');
	g_assert_nonnull(quote);

	return g_strndup(attr, quote - attr);
}

/* The theme of the page at @path as @client sees it. */
static gchar *
theme_of(Fixture *fx, Client *client, const gchar *path)
{
	Reply page = get(fx, client, path);
	gchar *id;

	g_assert_cmpuint(page.status, ==, 200);
	id = html_theme(page.body);
	reply_clear(&page);

	return id;
}

static void
assert_theme(Fixture *fx, Client *client, const gchar *path, const gchar *expected)
{
	g_autofree gchar *id = theme_of(fx, client, path);

	g_assert_cmpstr(id, ==, expected);
}

/* Spec: Choice persists / Other users unaffected */
static void
test_theme_persists_and_is_personal(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) laptop = client_new(NULL);
	g_autoptr(Client) phone = client_new(NULL);
	g_autoptr(Client) bob = client_new(NULL);
	g_autoptr(Client) stranger = client_new(NULL);
	const gchar *dusk[] = { "theme", "dusk", NULL };
	const gchar *paper[] = { "theme", "paper", NULL };
	const gchar *none[] = { "theme", "default", NULL };
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	seed(fx, "bob", BOB_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, laptop, "alice", ALICE_PASSWORD);
	login_ok(fx, bob, "bob", BOB_PASSWORD);

	/* A new account uses the default, which is dark. */
	assert_theme(fx, laptop, "/", "midnight");
	assert_query(fx, "SELECT coalesce(theme, 'none') FROM app_user WHERE username = 'alice'", "none");

	reply = post_as(fx, laptop, "/account/theme", dusk);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/account?done=theme");
	reply_clear(&reply);
	assert_query(fx, "SELECT theme FROM app_user WHERE username = 'alice'", "dusk");

	/* The next page already has it, with the confirmation. */
	reply = get(fx, laptop, "/account?done=theme");
	assert_contains(reply.body, "Your theme has been saved.");
	reply_clear(&reply);
	assert_theme(fx, laptop, "/", "dusk");

	/* Another user's pages are unchanged (and so is the login page). */
	assert_theme(fx, bob, "/", "midnight");
	assert_query(fx, "SELECT coalesce(theme, 'none') FROM app_user WHERE username = 'bob'", "none");
	assert_theme(fx, stranger, "/login", "midnight");

	/* From another browser, after a new login. */
	login_ok(fx, phone, "alice", ALICE_PASSWORD);
	assert_theme(fx, phone, "/", "dusk");

	/* After the daemon restarts, from the database. */
	axis_web_auth_wait_writes(fx->auth);
	restart(fx);
	assert_theme(fx, laptop, "/", "dusk");
	assert_theme(fx, bob, "/", "midnight");

	/* A change is picked up by every browser of the account at once. */
	reply = post_as(fx, phone, "/account/theme", paper);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	assert_theme(fx, laptop, "/", "paper");
	assert_query(fx, "SELECT theme FROM app_user WHERE username = 'alice'", "paper");

	/* "Use the default" stores nothing. */
	reply = post_as(fx, laptop, "/account/theme", none);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);
	assert_query(fx, "SELECT coalesce(theme, 'none') FROM app_user WHERE username = 'alice'", "none");
	assert_theme(fx, laptop, "/", "midnight");
}

/* Spec: Unknown theme */
static void
test_theme_unknown_falls_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	/* A theme this version removed, as a later version might have. */
	axis_test_db_exec(fx->db, "UPDATE app_user SET theme = 'retired-theme' WHERE username = 'alice'");
	axis_web_auth_wait_writes(fx->auth);
	restart(fx);
	assert_theme(fx, browser, "/", "midnight");

	/* The picker shows it as "use the default". */
	reply = get(fx, browser, "/account");
	assert_contains(reply.body, "value=\"default\"");
	reply_clear(&reply);

	/* The database refuses a malformed id. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AxisUser) user = NULL;

		g_assert_true(axis_user_store_find(fx->pool, "alice", &user, NULL, &error));
		g_assert_no_error(error);
		g_assert_false(axis_user_store_set_theme(fx->pool, user->id, "Not A Theme!", &error));
		g_assert_nonnull(error);
	}
}

/* Spec: Forged request refused */
static void
test_theme_forged_request_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	g_autoptr(Client) attacker = client_new(NULL);
	const gchar *form[] = { "theme", "paper", NULL };
	const gchar *cross_site[] = { "Origin", "http://evil.example", NULL };
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, browser, "alice", ALICE_PASSWORD);

	/* No token at all. */
	reply = send(fx, browser, "POST", "/account/theme", form, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);

	/* A token that is not the session's. */
	{
		const gchar *wrong[] = { "_csrf", "not-the-token", "theme", "paper", NULL };

		reply = send(fx, browser, "POST", "/account/theme", wrong, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}

	/* The right token, from another site. */
	{
		g_autofree gchar *token = csrf_of(fx, browser);
		const gchar *right[] = { "_csrf", token, "theme", "paper", NULL };

		reply = send(fx, browser, "POST", "/account/theme", right, cross_site);
		g_assert_cmpuint(reply.status, ==, 403);
		reply_clear(&reply);
	}

	/* Not logged in. */
	reply = send(fx, attacker, "POST", "/account/theme", form, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	reply_clear(&reply);

	/* A theme that does not exist is refused, not stored. */
	{
		const gchar *bad[] = { "theme", "neon", NULL };

		reply = post_as(fx, browser, "/account/theme", bad);
		g_assert_cmpuint(reply.status, ==, 400);
		reply_clear(&reply);
	}

	assert_query(fx, "SELECT coalesce(theme, 'none') FROM app_user WHERE username = 'alice'", "none");
	assert_theme(fx, browser, "/", "midnight");
}

/* Spec: Dark by default (setup and login pages carry the mark) */
static void
test_setup_and_login_pages_have_the_mark(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) browser = client_new(NULL);
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	start(fx);

	reply = get(fx, browser, "/setup");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "class=\"lockup\"");
	assert_contains(reply.body, "id=\"axis-hub-lockup\"");
	assert_contains(reply.body, "id=\"axis-hub-header\"");
	assert_lacks(reply.body, " style=");
	{
		g_autofree gchar *id = html_theme(reply.body);

		g_assert_cmpstr(id, ==, "midnight");
	}
	reply_clear(&reply);

	stop(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	reply = get(fx, browser, "/login");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "class=\"lockup\"");
	assert_lacks(reply.body, " style=");
	reply_clear(&reply);
}

/* A form that stays on its page names the place in "at" and ends "#place";
 * that page shows the notice inside the element. */
static void
assert_place(
	Fixture     *fx,
	Client      *client,
	const gchar *location,
	const gchar *place
){
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *path = NULL;
	const gchar *element;
	const gchar *notice;
	Reply page;

	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strrchr(location, '#') - location);
	page = get(fx, client, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, id);
	notice = strstr(page.body, "notice-here");
	g_assert_nonnull(element);
	g_assert_nonnull(notice);
	g_assert_true(element < notice);
	reply_clear(&page);
}

/* The Account and Users forms come back to their card or the account's row. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autoptr(Client) admin = client_new(NULL);
	g_autoptr(Client) phone = client_new(NULL);
	g_autoptr(Client) bob = client_new(NULL);
	const gchar *create[] = { "username", "Erin", "password", BOB_PASSWORD, "role", "user", NULL };
	const gchar *reset[] = { "password", "erins-new-password", NULL };
	const gchar *promote[] = { "role", "admin", NULL };
	const gchar *theme[] = { "theme", "dusk", NULL };
	const gchar *fields[] = { "current_password", ALICE_PASSWORD,
		"new_password", "a-whole-new-password", "new_password_confirm", "a-whole-new-password", NULL };
	g_autofree gchar *action = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *row = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *origin = NULL;
	Reply reply;

	SKIP_WITHOUT_DB(fx);
	seed(fx, "alice", ALICE_PASSWORD, AXIS_USER_ROLE_ADMIN, FALSE);
	seed(fx, "bob", BOB_PASSWORD, AXIS_USER_ROLE_USER, FALSE);
	start(fx);
	login_ok(fx, admin, "alice", ALICE_PASSWORD);
	login_ok(fx, phone, "alice", ALICE_PASSWORD);
	login_ok(fx, bob, "bob", BOB_PASSWORD);

	/* Theme and ending a session: their cards. */
	reply = post_as(fx, admin, "/account/theme", theme);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), "theme");
	reply_clear(&reply);
	reply = get(fx, admin, "/account");
	action = revoke_action(reply.body);
	reply_clear(&reply);
	g_assert_nonnull(action);
	reply = post_as(fx, admin, action, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), "sessions");
	reply_clear(&reply);
	reply = post_as(fx, admin, "/account/password", fields);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), "change-password");
	reply_clear(&reply);

	/* Creating an account: its card. */
	reply = post_as(fx, admin, "/admin/users", create);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), "new-account");
	reply_clear(&reply);

	/* Disable, enable, role, a password: the account's row. */
	base = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT, user_id(fx, "bob"));
	row = g_strdup_printf("user-%" G_GINT64_FORMAT, user_id(fx, "bob"));
	path = g_strconcat(base, "/disable", NULL);
	reply = post_as(fx, admin, path, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), row);
	reply_clear(&reply);
	g_free(path);
	path = g_strconcat(base, "/enable", NULL);
	reply = post_as(fx, admin, path, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), row);
	reply_clear(&reply);
	g_free(path);
	path = g_strconcat(base, "/role", NULL);
	reply = post_as(fx, admin, path, promote);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), row);
	reply_clear(&reply);
	g_free(path);
	path = g_strconcat(base, "/password", NULL);
	reply = post_as(fx, admin, path, reset);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_place(fx, admin, header(&reply, "Location"), row);
	reply_clear(&reply);

	/* Delete is an htmx post: the place helper answers HX-Redirect, to the
	 * accounts' card (the row is gone). */
	g_free(path);
	path = g_strconcat(base, "/delete", NULL);
	origin = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->port);
	token = csrf_of(fx, admin);
	{
		const gchar *const htmx[] = { "HX-Request", "true", "X-CSRF-Token", token,
			"Origin", origin, "Sec-Fetch-Site", "same-origin", NULL };

		reply = send(fx, admin, "POST", path, NULL, htmx);
	}
	g_assert_cmpuint(reply.status, ==, 200);
	assert_place(fx, admin, header(&reply, "HX-Redirect"), "accounts");
	reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_assert_true(axis_password_init());
	axis_password_use_test_cost();

#define ADD(name, fn) \
	g_test_add(name, Fixture, NULL, fixture_setup, fn, fixture_teardown)

	ADD("/web-auth/setup/with-token", test_setup_with_token);
	ADD("/web-auth/setup/without-token", test_setup_without_token);
	ADD("/web-auth/setup/from-the-portals-address", test_setup_from_portal_address);
	ADD("/web-auth/setup/closed", test_setup_closed);
	ADD("/web-auth/setup/race", test_setup_race);
	ADD("/web-auth/setup/race-in-database", test_setup_race_in_database);

	ADD("/web-auth/password/stored-form", test_password_stored_form);
	ADD("/web-auth/password/short-refused", test_password_short_refused);
	ADD("/web-auth/password/absent-from-logs", test_password_absent_from_logs);
	ADD("/web-auth/password/change", test_password_change);
	ADD("/web-auth/password/change-wrong-current", test_password_change_wrong_current);

	ADD("/web-auth/login/success-returns", test_login_success_returns);
	ADD("/web-auth/login/open-redirect", test_login_open_redirect);
	ADD("/web-auth/login/indistinguishable", test_login_indistinguishable);
	ADD("/web-auth/login/username-case", test_login_username_case);

	ADD("/web-auth/throttle/repeated-failures", test_throttle_repeated_failures);
	ADD("/web-auth/throttle/success-clears", test_throttle_success_clears);
	ADD("/web-auth/throttle/burst", test_throttle_burst);
	ADD("/web-auth/throttle/burst-password-change", test_throttle_burst_password_change);
	ADD("/web-auth/throttle/remembered-device-not-locked-out", test_remembered_device_not_locked_out);
	ADD("/web-auth/throttle/remembered-device-has-its-own-count", test_remembered_device_has_its_own_count);
	ADD("/web-auth/throttle/remembered-device-signed-and-own", test_remembered_device_must_be_signed_and_own);
	ADD("/web-auth/throttle/remembered-device-survives-restart", test_remembered_device_survives_restart);
	ADD("/web-auth/throttle/remembered-device-ends-with-password", test_remembered_device_ends_with_password);
	ADD("/web-auth/throttle/remembered-devices-capped", test_remembered_devices_capped);
	ADD("/web-auth/throttle/remembered-devices-share-a-ceiling", test_remembered_devices_share_a_ceiling);
	ADD("/web-auth/throttle/password-change-not-locked-by-stranger", test_password_change_not_locked_by_stranger);
	ADD("/web-auth/proxy/untrusted-proxy-hinted", test_untrusted_proxy_hinted);
	ADD("/web-auth/proxy/trusted-proxy-not-hinted", test_trusted_proxy_not_hinted);

	ADD("/web-auth/cookie/attributes", test_cookie_attributes);
	ADD("/web-auth/cookie/secure-behind-trusted-proxy", test_cookie_secure_behind_trusted_proxy);
	ADD("/web-auth/cookie/not-secure-from-untrusted", test_cookie_secure_not_from_untrusted);
	ADD("/web-auth/cookie/secure-always", test_cookie_secure_always);

	ADD("/web-auth/session/survives-restart", test_session_survives_restart);
	ADD("/web-auth/session/logout", test_session_logout);
	ADD("/web-auth/session/idle-expiry", test_session_idle_expiry);
	ADD("/web-auth/session/absolute-expiry", test_session_absolute_expiry);
	ADD("/web-auth/session/stored-not-credential", test_session_stored_not_credential);
	ADD("/web-auth/session/revoke-other", test_session_revoke_other);
	ADD("/web-auth/session/revoke-not-others-users", test_session_revoke_not_others_users);

	ADD("/web-auth/csrf/refused", test_csrf_refused);
	ADD("/web-auth/csrf/htmx-carries-token", test_csrf_htmx);

	ADD("/web-auth/roles/non-admin-refused", test_roles_non_admin);
	ADD("/web-auth/roles/last-admin-protected", test_roles_last_admin);
	ADD("/web-auth/roles/last-admin-race", test_roles_last_admin_race);
	ADD("/web-auth/roles/disable-ends-sessions", test_roles_disable_ends_sessions);
	ADD("/web-auth/admin/manage-accounts", test_admin_manage_accounts);

	ADD("/web/account/back-to-place", test_back_to_place);

	ADD("/web-auth/theme/persists-and-is-personal", test_theme_persists_and_is_personal);
	ADD("/web-auth/theme/unknown-falls-back", test_theme_unknown_falls_back);
	ADD("/web-auth/theme/forged-request-refused", test_theme_forged_request_refused);
	ADD("/web-auth/theme/setup-and-login-have-the-mark", test_setup_and_login_pages_have_the_mark);

	ADD("/web-auth/proxy/untrusted-ignored", test_proxy_untrusted_ignored);
	ADD("/web-auth/proxy/trusted-used", test_proxy_trusted_used);
	ADD("/web-auth/proxy/forwarded-host-trusted", test_proxy_forwarded_host_trusted);
	ADD("/web-auth/proxy/forwarded-host-untrusted", test_proxy_forwarded_host_untrusted);
	ADD("/web-auth/proxy/list-logged", test_proxy_list_logged);
	ADD("/web-auth/proxy/name-follows-address", test_proxy_name_follows_address);
	ADD("/web-auth/proxy/forwarded-login-while-name-unresolved", test_portal_peers_unresolved_name);

	return g_test_run();
}
