/*
 * test-portal-api.c - the portal API: its listener, its gate, the requester
 * role, the session proof, the eleven operations and the daemon's limits
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P2; the `portal` and `web-auth` specifications.)
 * Two servers run on the test's own main loop, each on a port the system gave:
 * the daemon's main listener (the shell, the login, the Users page) and the
 * portal listener (the API and nothing else).  The database, the scratch
 * directory and the fixture provider are the test's own, and the accounts are
 * real ones, with passwords:
 *
 *   root           an administrator
 *   uma            an ordinary user
 *   robin, sam,
 *   tess, vic      requesters
 *   dana           a requester, disabled
 *
 * Every test here tries to cross a barrier and expects to be refused: the
 * scope of the token is held by the daemon, not by whoever calls it.
 */

#define _GNU_SOURCE

#include <glib.h>
#include <htmx-glib.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "api/axis-portal-api.h"
#include "api/axis-portal-proto.h"
#include "api/axis-portal-tokens.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-plex.h"
#include "axis-test-requests.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "discover/axis-discover.h"
#include "web/axis-password.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-auth-portal.h"
#include "web/axis-web-auth-private.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-private.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

#define ADMIN_PASSWORD "a made-up password of the tests"
#define USER_PASSWORD  "another made-up password"
#define ROBIN_PASSWORD "lantern-orchard-pass"
#define SAM_PASSWORD   "saltmarsh-ledger-pass"
#define OTHER_PASSWORD "paper-kiln-quarry-pass"

/* Names no answer of the portal API may hold. */
#define FOLDER_MARK  "zq-folder-mark"
#define PROFILE_MARK "zq-profile-mark"

#define API AXIS_PORTAL_API_PREFIX

typedef struct
{
	AxisTestRequests   *r;
	AxisWebSettings    *web_settings;
	AxisWebMeta        *meta_ui;
	AxisWebIndexers    *indexers_ui;
	AxisWebDownloads   *downloads_ui;
	AxisWebQuality     *quality_ui;
	AxisModuleRegistry *registry;
	AxisWebHub         *hub;
	AxisWebJobs        *jobs;
	AxisDiscover       *discover;
	HtmxServer         *server;
	HtmxServer         *portal_server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisPortalTokens   *tokens;
	AxisPortalApi      *api;
	SoupSession        *session;	/* from 127.0.0.1: the portal's address in these tests */
	SoupSession        *people;	/* from 127.0.0.2: a person's desk, and anyone who is not the portal */
	guint16             port;
	guint16             portal_port;
	gchar              *token;	/* a valid portal token */
	gint64              token_id;
	gchar              *outside;	/* a session token written to the database before the daemon read it */
	gint64              uma;
	gint64              dana;
	gint64              tess;
	gint64              vic;
} Fixture;

/* The fixture with a session row of the engine's interface written from
 * outside for the requester robin, before the daemon loads its sessions. */
#define OUTSIDE_SESSION 1
/* The fixture whose daemon believes two reverse proxies about the client:
 * 127.0.0.1 and 127.0.0.2. */
#define BEHIND_PROXIES  2

/* The portal's address in these tests (server.portal_peers), and the address
 * the people of these tests log in to Axis from. */
#define PORTAL_ADDRESS "127.0.0.1"
#define PEOPLE_ADDRESS "127.0.0.2"

#define Q(...) axis_test_requests_q(fx->r, __VA_ARGS__)
#define N(...) axis_test_requests_n(fx->r, __VA_ARGS__)

static HtmxResponse *
handle_new_route(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_content(response, "zz-opened");
	return response;
}

static void
set_password(
	Fixture     *fx,
	const gchar *username,
	const gchar *password
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = axis_password_hash(password, &error);

	g_assert_no_error(error);
	g_assert_null(strchr(hash, '\''));
	N("UPDATE app_user SET password_hash = '%s' WHERE username = '%s' RETURNING id", hash, username);
}

static gint64
add_account(
	Fixture     *fx,
	const gchar *username,
	const gchar *role,
	const gchar *password,
	gboolean     disabled
){
	gint64 id = N("INSERT INTO app_user (username, password_hash, role, disabled) VALUES ('%s',"
		" '$argon2id$not-a-password', '%s', %s) RETURNING id", username, role, disabled ? "true" : "false");

	set_password(fx, username, password);
	return id;
}

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->r->fa->api, path, 200, "application/json", body, NULL);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestRequests *r;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	/* Hundreds of logins: libsodium's smallest limits. */
	axis_password_use_test_cost();
	fx->r = r = axis_test_requests_new();
	if (r == NULL)
		return;
	fx->web_settings = axis_web_settings_new(r->settings, r->t->pool);
	fx->meta_ui = axis_web_meta_new(r->meta);
	fx->indexers_ui = axis_web_indexers_new(r->indexers);
	fx->downloads_ui = axis_web_downloads_new(r->downloads, r->t->storage, NULL);
	fx->quality_ui = axis_web_quality_new(r->quality);
	fx->tokens = axis_portal_tokens_new();
	axis_requests_set_portal(r->requests, fx->tokens, 7801);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), r->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), r->tv, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_requests_get_module(), r->requests, &error));
	fx->hub = axis_web_hub_new(NULL);
	fx->jobs = axis_web_jobs_new(r->t->pool, fx->hub);
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_jobs_get_module(), fx->jobs, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(r->settings, fx->registry, &error));
	g_assert_no_error(error);
	axis_test_requests_ready(r);
	g_assert_true(axis_module_state_load(fx->registry, r->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);

	/* The accounts, before the daemon loads them. */
	g_assert_cmpint(r->admin, ==, 1);
	set_password(fx, "root", ADMIN_PASSWORD);
	N("UPDATE app_user SET role = 'requester' WHERE username IN ('robin', 'sam') RETURNING id");
	set_password(fx, "robin", ROBIN_PASSWORD);
	set_password(fx, "sam", SAM_PASSWORD);
	fx->uma = add_account(fx, "uma", "user", USER_PASSWORD, FALSE);
	fx->dana = add_account(fx, "dana", "requester", OTHER_PASSWORD, TRUE);
	fx->tess = add_account(fx, "tess", "requester", OTHER_PASSWORD, FALSE);
	fx->vic = add_account(fx, "vic", "requester", OTHER_PASSWORD, FALSE);
	if (GPOINTER_TO_UINT(data) == OUTSIDE_SESSION) {
		g_autofree gchar *hash = NULL;
		g_autofree gchar *public_id = axis_random_public_id();

		fx->outside = axis_random_token();
		hash = axis_token_hash(fx->outside);
		N("INSERT INTO web_session (token_hash, public_id, user_id, user_epoch, created_at, last_seen_at,"
			" client_address, user_agent) VALUES ('%s', '%s', %" G_GINT64_FORMAT ", (SELECT auth_epoch FROM app_user"
			" WHERE id = %" G_GINT64_FORMAT "), now(), now(), '192.0.2.9', 'test') RETURNING user_id", hash, public_id,
			r->robin, r->robin);
	}

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	/* No proxy is believed (but in the fixture that says so), and the
	 * portal's address is 127.0.0.1: the portal API answers that address
	 * alone, and since slice P4 nobody logs in at the engine's own interface
	 * from it.  The people of these tests log in directly from 127.0.0.2
	 * (fx->people, engine_login()), as they did before there was the rule. */
	fx->auth = axis_web_auth_new(r->t->pool, GPOINTER_TO_UINT(data) == BEHIND_PROXIES ? PORTAL_ADDRESS ", "
		PEOPLE_ADDRESS : "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, PORTAL_ADDRESS, &error));
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	/* A route a module adds and does nothing about authentication for. */
	axis_web_get(fx->web, "/zz-new-route", handle_new_route, NULL);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->port = axis_test_web_port(fx->server);

	fx->discover = axis_discover_new(r->t->pool, r->settings, r->meta);
	fx->api = axis_portal_api_new(r->t->pool, fx->auth, fx->tokens, r->requests, r->meta, fx->discover);
	fx->portal_server = htmx_server_new();
	axis_portal_api_install(fx->api, fx->portal_server, 0);
	g_assert_true(htmx_server_listen_on(fx->portal_server, "127.0.0.1", 0, &error));
	g_assert_true(htmx_server_start(fx->portal_server, &error));
	g_assert_no_error(error);
	fx->portal_port = axis_test_web_port(fx->portal_server);
	g_assert_cmpuint(fx->portal_port, !=, fx->port);
	g_assert_true(axis_portal_tokens_create(fx->tokens, r->t->pool, "root", &fx->token, &fx->token_id, &error));
	g_assert_no_error(error);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);
	{
		g_autoptr(GSocketAddress) desk = g_inet_socket_address_new_from_string(PEOPLE_ADDRESS, 0);

		g_assert_nonnull(desk);
		fx->people = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, "local-address", desk, NULL);
	}

	/* A search for films finds four of the made-up ones, and one its
	 * provider marks as adult. */
	set_route(fx, "/search/film", "{\"results\":["
		"{\"id\":\"" AXIS_TEST_REQUESTS_ORCHARD "\",\"kind\":\"film\",\"title\":\"The Lantern Orchard\",\"year\":2019,"
		"\"overview\":\"A made-up film about <b>lanterns</b> & an orchard.\"},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_LEDGER "\",\"kind\":\"film\",\"title\":\"Saltmarsh Ledger\",\"year\":2015},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_KILN "\",\"kind\":\"film\",\"title\":\"Paper Kiln\",\"year\":2011},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_ANNEX "\",\"kind\":\"film\",\"title\":\"Velvet Annex\",\"year\":2014,"
		"\"adult\":true},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_BELLS "\",\"kind\":\"film\",\"title\":\"Quarry Bells\",\"year\":2008}]}");
	set_route(fx, "/search/series", "{\"results\":["
		"{\"id\":\"" AXIS_TEST_REQUESTS_HARBOUR "\",\"kind\":\"series\",\"title\":\"Harbour of Glass\",\"year\":2020}]}");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->r == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	g_object_unref(fx->people);
	htmx_server_stop(fx->server);
	htmx_server_stop(fx->portal_server);
	axis_portal_api_stop(fx->api);
	axis_web_auth_stop(fx->auth);
	axis_web_stop(fx->web);
	axis_web_jobs_stop(fx->jobs);
	axis_requests_stop(fx->r->requests);
	axis_movies_stop(fx->r->movies);
	axis_tv_stop(fx->r->tv);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_object_unref(fx->portal_server);
	axis_portal_api_free(fx->api);
	axis_discover_unref(fx->discover);
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_jobs_free(fx->jobs);
	axis_web_auth_free(fx->auth);
	axis_portal_tokens_free(fx->tokens);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_quality_free(fx->quality_ui);
	axis_web_downloads_free(fx->downloads_ui);
	axis_web_indexers_free(fx->indexers_ui);
	axis_web_meta_free(fx->meta_ui);
	axis_web_settings_free(fx->web_settings);
	axis_secret_free(fx->token);
	axis_secret_free(fx->outside);
	axis_test_requests_free(fx->r);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->r != NULL)

/* ── Requests ───────────────────────────────────────────────────── */

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} Pending;

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Pending *pending = user_data;

	pending->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &pending->error);
	g_main_loop_quit(pending->loop);
}

/* A request as given, over @session (which says where from): @token as a
 * bearer token (nullable), @body with @content_type (application/json when
 * %NULL), @extra header pairs. */
static AxisTestReply
send_over(
	SoupSession        *session,
	guint16             port,
	const gchar        *method,
	const gchar        *path,
	const gchar        *token,
	const gchar        *content_type,
	const gchar        *body,
	const gchar *const *extra
){
	g_autofree gchar *bearer = token != NULL ? g_strconcat("Bearer ", token, NULL) : NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)port, path);
	SoupMessageHeaders *headers;
	AxisTestReply reply;
	Pending pending;
	gconstpointer data;
	gsize size = 0;
	guint i;

	memset(&reply, 0, sizeof(reply));
	reply.message = soup_message_new(method, url);
	g_assert_nonnull(reply.message);
	soup_message_add_flags(reply.message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(reply.message);
	if (bearer != NULL)
		soup_message_headers_replace(headers, "Authorization", bearer);
	for (i = 0; extra != NULL && extra[i] != NULL; i += 2)
		soup_message_headers_append(headers, extra[i], extra[i + 1]);
	if (body != NULL) {
		g_autoptr(GBytes) payload = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(reply.message,
			content_type != NULL ? content_type : "application/json", payload);
	}
	memset(&pending, 0, sizeof(pending));
	pending.loop = g_main_loop_new(NULL, FALSE);
	soup_session_send_and_read_async(session, reply.message, G_PRIORITY_DEFAULT, NULL, on_sent, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);
	g_assert_no_error(pending.error);
	data = g_bytes_get_data(pending.bytes, &size);
	reply.status = soup_message_get_status(reply.message);
	reply.body = g_malloc(size + 1);
	if (size > 0)
		memcpy(reply.body, data, size);
	reply.body[size] = '\0';
	reply.length = size;
	g_bytes_unref(pending.bytes);
	return reply;
}

/* The same from the portal's address. */
static AxisTestReply
send_to(
	Fixture            *fx,
	guint16             port,
	const gchar        *method,
	const gchar        *path,
	const gchar        *token,
	const gchar        *content_type,
	const gchar        *body,
	const gchar *const *extra
){
	return send_over(fx->session, port, method, path, token, content_type, body, extra);
}

/* An operation, with the fixture's token. */
static AxisTestReply
call(
	Fixture     *fx,
	const gchar *operation,
	const gchar *body
){
	g_autofree gchar *path = g_strconcat(API, operation, NULL);

	return send_to(fx, fx->portal_port, "POST", path, fx->token, NULL, body, NULL);
}

/* Every answer of the portal listener: JSON, no redirect, no cookie. */
static void
assert_api_reply(AxisTestReply *reply)
{
	const gchar *type = axis_test_reply_header(reply, "Content-Type");

	if (type == NULL || !g_str_has_prefix(type, "application/json"))
		g_error("expected JSON, got %s with status %u:\n%s", type != NULL ? type : "(none)", reply->status, reply->body);
	g_assert_null(axis_test_reply_header(reply, "Set-Cookie"));
	g_assert_null(axis_test_reply_header(reply, "Location"));
}

#define assert_status(reply, expected) G_STMT_START { \
	if ((reply)->status != (expected)) \
		g_error("%s:%d: expected %u, got %u:\n%s", __FILE__, __LINE__, (guint)(expected), (reply)->status, \
			(reply)->body); \
} G_STMT_END

/* Calls an operation and expects @status; the body (transfer full). */
static gchar *
expect(
	Fixture     *fx,
	const gchar *operation,
	const gchar *body,
	guint        status
){
	AxisTestReply reply = call(fx, operation, body);
	gchar *text;

	if (reply.status != status)
		g_error("%s %s: expected %u, got %u:\n%s", operation, body != NULL ? body : "", status, reply.status,
			reply.body);
	if (status != 200 || strcmp(operation, "art") != 0)
		assert_api_reply(&reply);
	text = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	return text;
}

static JsonObject *
object_of(const gchar *json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonNode *root;

	if (!json_parser_load_from_data(parser, json, -1, &error))
		g_error("not JSON (%s):\n%s", error->message, json);
	root = json_parser_get_root(parser);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
	return json_object_ref(json_node_get_object(root));
}

/* The member names of an object, sorted, joined by commas. */
static gchar *
members_of(JsonObject *object)
{
	GList *names = g_list_sort(json_object_get_members(object), (GCompareFunc)g_strcmp0);
	GString *joined = g_string_new(NULL);
	GList *l;

	for (l = names; l != NULL; l = l->next)
		g_string_append_printf(joined, "%s%s", joined->len > 0 ? "," : "", (const gchar *)l->data);
	g_list_free(names);
	return g_string_free(joined, FALSE);
}

static gchar *
string_of(
	const gchar *json,
	const gchar *member
){
	g_autoptr(JsonObject) object = object_of(json);

	return json_object_has_member(object, member) ? g_strdup(json_object_get_string_member(object, member)) : NULL;
}

/* Logs in through the API; the proof.  @device_in: (nullable) the device
 * value to present; @device_out: (out) (optional) the one answered. */
static gchar *
login_with(
	Fixture      *fx,
	const gchar  *username,
	const gchar  *password,
	const gchar  *device_in,
	gchar       **device_out
){
	g_autofree gchar *device = device_in != NULL ? g_strdup_printf(",\"device\":\"%s\"", device_in) : g_strdup("");
	g_autofree gchar *body = g_strdup_printf("{\"username\":\"%s\",\"password\":\"%s\",\"client_address\":"
		"\"198.51.100.20\",\"user_agent\":\"a test browser\"%s}", username, password, device);
	g_autofree gchar *answer = expect(fx, "login", body, 200);
	gchar *proof = string_of(answer, "proof");

	g_assert_nonnull(proof);
	g_assert_cmpuint(strlen(proof), ==, AXIS_PORTAL_PROOF_LEN);
	if (device_out != NULL)
		*device_out = string_of(answer, "device");
	return proof;
}

static gchar *
login(
	Fixture     *fx,
	const gchar *username,
	const gchar *password
){
	return login_with(fx, username, password, NULL, NULL);
}

static gchar *
login_body(
	const gchar *username,
	const gchar *password,
	const gchar *address
){
	return g_strdup_printf("{\"username\":\"%s\",\"password\":\"%s\",\"client_address\":\"%s\","
		"\"user_agent\":\"a test browser\"}", username, password, address);
}

static gint64
film(
	Fixture     *fx,
	const gchar *id
){
	return axis_test_requests_title(fx->r, AXIS_META_KIND_FILM, id);
}

static gint64
series(
	Fixture     *fx,
	const gchar *id
){
	return axis_test_requests_title(fx->r, AXIS_META_KIND_SERIES, id);
}

/* Asks for a movie through the API; the answer (transfer full). */
static gchar *
ask_movie(
	Fixture     *fx,
	const gchar *proof,
	gint64       title,
	guint        status
){
	g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"movie\"}", proof,
		title);

	return expect(fx, "request", body, status);
}

static gchar *
proof_body(const gchar *proof)
{
	return g_strdup_printf("{\"proof\":\"%s\"}", proof);
}

/* The engine's interface: a form posted from a page an administrator has
 * open (its CSRF token from there). */
static AxisTestReply
admin_post(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *token_from,
	const gchar *path,
	const gchar *form
){
	AxisTestReply opened = axis_test_web_request(fx->session, fx->port, "GET", token_from, cookie, NULL, NULL);
	g_autofree gchar *token = axis_test_web_csrf_token(opened.body);
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;

	assert_status(&opened, 200);
	axis_test_reply_clear(&opened);
	g_assert_nonnull(token);
	escaped = g_uri_escape_string(token, NULL, TRUE);
	body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

/* The cookies a reply sets, as a Cookie header's value; a cookie that is
 * being cleared is left out. */
static gchar *
cookies_after(AxisTestReply *reply)
{
	GString *cookies = g_string_new(NULL);
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;

	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(reply->message));
	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		g_autofree gchar *pair = NULL;

		if (g_ascii_strcasecmp(name, "Set-Cookie") != 0)
			continue;
		pair = g_strndup(value, strcspn(value, ";"));
		if (g_str_has_suffix(pair, "=") || strstr(value, "Max-Age=0") != NULL)
			continue;
		g_string_append_printf(cookies, "%s%s", cookies->len > 0 ? "; " : "", pair);
	}
	return g_string_free(cookies, FALSE);
}

/* A login at the engine's own login page over @session (fx->people: from a
 * person's desk, 127.0.0.2; fx->session: from 127.0.0.1, where the portal of
 * these tests is), with the header pairs @from (nullable): the reply (its
 * status says whether it worked: 303, or 401). */
static AxisTestReply
engine_login_from(
	Fixture            *fx,
	SoupSession        *session,
	const gchar        *username,
	const gchar        *password,
	const gchar *const *from
){
	g_autoptr(GRegex) field = g_regex_new("name=\"_csrf\"[^>]*value=\"([^\"]+)\"|value=\"([^\"]+)\"[^>]*name=\"_csrf\"", 0,
		0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *secret = g_uri_escape_string(password, NULL, TRUE);
	g_autofree gchar *form = NULL;
	AxisTestReply page = axis_test_web_request(session, fx->port, "GET", "/login", NULL, from, NULL);

	assert_status(&page, 200);
	before = cookies_after(&page);
	g_assert_true(g_regex_match(field, page.body, 0, &match));
	token = g_match_info_fetch(match, 1);
	if (token == NULL || token[0] == '\0') {
		g_free(token);
		token = g_match_info_fetch(match, 2);
	}
	axis_test_reply_clear(&page);
	escaped = g_uri_escape_string(token, NULL, TRUE);
	form = g_strdup_printf("_csrf=%s&username=%s&password=%s", escaped, username, secret);
	return axis_test_web_request(session, fx->port, "POST", "/login", before, from, form);
}

/* A person's login: directly, with no proxy between, from an address that
 * is not the portal's. */
static AxisTestReply
engine_login(
	Fixture     *fx,
	const gchar *username,
	const gchar *password
){
	return engine_login_from(fx, fx->people, username, password, NULL);
}

static gchar *
engine_session(
	Fixture     *fx,
	const gchar *username,
	const gchar *password
){
	AxisTestReply reply = engine_login(fx, username, password);
	gchar *cookies;

	assert_status(&reply, 303);
	cookies = cookies_after(&reply);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(cookies, !=, "");
	return cookies;
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
assert_holds(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

/* ── The listener ───────────────────────────────────────────────── */

/* Scenarios: Only the API on the portal listener; Not on the main listener;
 * Off by default (the main listener's half: with or without a portal
 * listener it answers no path of the portal API). */
static void
test_listener(Fixture *fx, gconstpointer data)
{
	static const gchar *const elsewhere[] = {
		"/", "/login", "/static/axis.css", "/admin/users", "/api/v1/worker/claim", "/healthz", "/livez", "/readyz",
		"/metrics", "/requests", "/api/v1/portal", "/api/v1/portalx/hello", "/API/V1/PORTAL/hello", NULL
	};
	static const gchar *const methods[] = { "GET", "POST", "HEAD", NULL };
	g_autofree gchar *admin = engine_session(fx, "root", ADMIN_PASSWORD);
	gint64 sessions;
	AxisTestReply reply;
	guint i;
	guint m;

	/* The administrator's own session is written behind its login: counted
	 * only once that write has landed. */
	axis_web_auth_wait_writes(fx->auth);
	sessions = N("SELECT count(*) FROM web_session");

	/* Nothing of the daemon's own on the portal listener: with the token,
	 * with an administrator's session, with neither. */
	for (i = 0; elsewhere[i] != NULL; i++) {
		for (m = 0; methods[m] != NULL; m++) {
			const gchar *const cookie[] = { "Cookie", admin, NULL };

			reply = send_to(fx, fx->portal_port, methods[m], elsewhere[i], fx->token, NULL,
				strcmp(methods[m], "POST") == 0 ? "{}" : NULL, NULL);
			assert_status(&reply, 404);
			if (strcmp(methods[m], "HEAD") != 0) {
				assert_api_reply(&reply);
				assert_holds(reply.body, "\"error\":\"not-found\"");
			}
			axis_test_reply_clear(&reply);
			reply = send_to(fx, fx->portal_port, methods[m], elsewhere[i], NULL, NULL, NULL, cookie);
			assert_status(&reply, 404);
			g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
			g_assert_null(axis_test_reply_header(&reply, "Location"));
			axis_test_reply_clear(&reply);
		}
	}

	/* The main listener answers every path of the portal API as one that
	 * does not exist: with a valid token, with an administrator's session,
	 * with both. */
	for (i = 0; axis_portal_api_operations()[i] != NULL; i++) {
		g_autofree gchar *path = g_strconcat(API, axis_portal_api_operations()[i], NULL);
		const gchar *const cookie[] = { "Cookie", admin, NULL };

		reply = send_to(fx, fx->port, "POST", path, fx->token, NULL, "{}", NULL);
		assert_status(&reply, 404);
		axis_test_reply_clear(&reply);
		reply = send_to(fx, fx->port, "POST", path, fx->token, NULL, "{}", cookie);
		assert_status(&reply, 404);
		axis_test_reply_clear(&reply);
		reply = send_to(fx, fx->port, "GET", path, NULL, NULL, NULL, cookie);
		assert_status(&reply, 404);
		axis_test_reply_clear(&reply);
	}
	{
		g_autofree gchar *body = login_body("robin", ROBIN_PASSWORD, "198.51.100.20");

		reply = send_to(fx, fx->port, "POST", API "login", fx->token, NULL, body, NULL);
		assert_status(&reply, 404);
		assert_lacks(reply.body, "proof");
		axis_test_reply_clear(&reply);
	}
	reply = send_to(fx, fx->port, "POST", "/api/v1/portal", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 404);
	axis_test_reply_clear(&reply);
	/* Nothing was done: no session was made by any of it. */
	axis_web_auth_wait_writes(fx->auth);
	g_assert_cmpint(N("SELECT count(*) FROM web_session"), ==, sessions);
}

/* ── The token and the gate ─────────────────────────────────────── */

/* Scenarios: A valid token; Default deny holds; A worker's token; A portal
 * token elsewhere; A session on the portal API; A token in the address; Not
 * an operation; A browser; Methods and bodies; Replacing a token; A revoked
 * token. */
static void
test_gate(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *admin = engine_session(fx, "root", ADMIN_PASSWORD);
	g_autofree gchar *answer = NULL;
	g_autofree gchar *with_query = g_strdup_printf(API "hello?token=%s", fx->token);
	g_autofree gchar *second = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;
	gboolean changed = FALSE;
	gint64 second_id = 0;
	guint i;

	answer = expect(fx, "hello", "{}", 200);
	{
		g_autoptr(JsonObject) hello = object_of(answer);
		g_autofree gchar *members = members_of(hello);

		g_assert_cmpstr(members, ==, "kinds,lists,protocol,show_names");
		g_assert_cmpint(json_object_get_int_member(hello, "protocol"), ==, AXIS_PORTAL_PROTOCOL);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(hello, "kinds")), ==, 2);
	}

	/* No credential: 401, in JSON, and no redirect. */
	reply = send_to(fx, fx->portal_port, "POST", API "hello", NULL, NULL, "{}", NULL);
	assert_status(&reply, 401);
	assert_api_reply(&reply);
	g_assert_cmpstr(axis_test_reply_header(&reply, "WWW-Authenticate"), ==, "Bearer");
	axis_test_reply_clear(&reply);
	/* An administrator's session and no token. */
	{
		const gchar *const cookie[] = { "Cookie", admin, NULL };

		reply = send_to(fx, fx->portal_port, "POST", API "hello", NULL, NULL, "{}", cookie);
		assert_status(&reply, 401);
		assert_api_reply(&reply);
		axis_test_reply_clear(&reply);
		reply = send_to(fx, fx->portal_port, "POST", API "requests", NULL, NULL, "{}", cookie);
		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
	}
	/* A worker's token is no portal token (and counts as a wrong one). */
	{
		g_autofree gchar *worker = axis_random_token();
		g_autofree gchar *hash = axis_token_hash(worker);

		N("INSERT INTO worker (name, token_hash, created_by) VALUES ('garage', '%s', 'root') RETURNING id", hash);
		reply = send_to(fx, fx->portal_port, "POST", API "hello", worker, NULL, "{}", NULL);
		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*carried a token that is not a portal's*"), ==, 1);
	}
	/* A portal token opens nothing else: not the worker API, not a page. */
	reply = send_to(fx, fx->port, "POST", "/api/v1/worker/claim", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->port, "GET", "/admin/users", fx->token, NULL, NULL, NULL);
	assert_status(&reply, 303);
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
	axis_test_reply_clear(&reply);

	/* Nothing is taken from an address, with or without the header. */
	reply = send_to(fx, fx->portal_port, "POST", with_query, NULL, NULL, "{}", NULL);
	assert_status(&reply, 400);
	assert_api_reply(&reply);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", with_query, fx->token, NULL, "{}", NULL);
	assert_status(&reply, 400);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello?a=b", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 400);
	axis_test_reply_clear(&reply);

	/* Not an operation. */
	{
		static const gchar *const not_operations[] = {
			API "approve", API "users", API "requests/all", API "hello/", API "Hello", API "", API "login/robin", NULL
		};

		for (i = 0; not_operations[i] != NULL; i++) {
			reply = send_to(fx, fx->portal_port, "POST", not_operations[i], fx->token, NULL, "{}", NULL);
			assert_status(&reply, 404);
			assert_api_reply(&reply);
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	}

	/* What a browser sends is refused even with a valid token. */
	{
		static const gchar *const each[][3] = {
			{ "Origin", "http://127.0.0.1", NULL }, { "Sec-Fetch-Site", "same-origin", NULL },
			{ "Sec-Fetch-Mode", "cors", NULL }, { "Cookie", "a=b", NULL }
		};

		for (i = 0; i < G_N_ELEMENTS(each); i++) {
			reply = send_to(fx, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", each[i]);
			assert_status(&reply, 403);
			assert_api_reply(&reply);
			axis_test_reply_clear(&reply);
		}
	}

	/* Methods and bodies. */
	reply = send_to(fx, fx->portal_port, "GET", API "hello", fx->token, NULL, NULL, NULL);
	assert_status(&reply, 405);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Allow"), ==, "POST");
	assert_api_reply(&reply);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "PUT", API "hello", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 405);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", fx->token, "application/x-www-form-urlencoded", "a=b",
		NULL);
	assert_status(&reply, 415);
	assert_api_reply(&reply);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", fx->token, "text/plain", "{}", NULL);
	assert_status(&reply, 415);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *filler = g_strnfill(17 * 1024, 'a');
		g_autofree gchar *big = g_strdup_printf("{\"query\":\"%s\"}", filler);

		reply = send_to(fx, fx->portal_port, "POST", API "search", fx->token, NULL, big, NULL);
		assert_status(&reply, 413);
		assert_api_reply(&reply);
		axis_test_reply_clear(&reply);
	}
	/* Not one JSON object. */
	{
		static const gchar *const bodies[] = { "[]", "\"hello\"", "{", "{\"a\":1}{\"b\":2}", "nul", NULL };

		for (i = 0; bodies[i] != NULL; i++) {
			g_autofree gchar *refused = expect(fx, "hello", bodies[i], 422);

			assert_lacks(refused, "nul\"");
		}
	}

	/* Replacing a token: a second one works beside the first, and the first
	 * is refused from the request after its revocation. */
	g_assert_true(axis_portal_tokens_create(fx->tokens, fx->r->t->pool, "root", &second, &second_id, &error));
	g_assert_no_error(error);
	g_free(expect(fx, "hello", "{}", 200));
	reply = send_to(fx, fx->portal_port, "POST", API "hello", second, NULL, "{}", NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	g_assert_true(axis_portal_tokens_revoke_stored(fx->tokens, fx->r->t->pool, fx->token_id, "root", &changed, &error));
	g_assert_no_error(error);
	g_assert_true(changed);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", second, NULL, "{}", NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	axis_secret_free(g_steal_pointer(&second));
	/* No log line holds a token. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*Bearer*"), ==, 0);

	/* A token's use is written down once a minute, whatever the address:
	 * two portals with one token write no row a request. */
	{
		gint64 later = g_get_monotonic_time() + G_GINT64_CONSTANT(600) * G_USEC_PER_SEC;

		g_assert_true(axis_portal_tokens_note_use(fx->tokens, second_id, "192.0.2.10", later));
		g_assert_false(axis_portal_tokens_note_use(fx->tokens, second_id, "192.0.2.11", later + G_USEC_PER_SEC));
		g_assert_false(axis_portal_tokens_note_use(fx->tokens, second_id, "192.0.2.10", later + 2 * G_USEC_PER_SEC));
		g_assert_true(axis_portal_tokens_note_use(fx->tokens, second_id, "192.0.2.11",
			later + G_GINT64_CONSTANT(61) * G_USEC_PER_SEC));
	}
}

/* Scenario: Shown once, stored as a hash; and the page's own revoking. */
static void
test_token_page(Fixture *fx, gconstpointer data)
{
	g_autoptr(GRegex) shown = g_regex_new("<code>([A-Za-z0-9_-]{43})</code>", 0, 0, NULL);
	g_autofree gchar *admin = engine_session(fx, "root", ADMIN_PASSWORD);
	g_autofree gchar *uma = engine_session(fx, "uma", USER_PASSWORD);
	g_autoptr(GMatchInfo) match = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *revoke = NULL;
	g_autofree gchar *pattern = NULL;
	AxisTestReply reply;
	gint64 id;
	guint i;

	/* Administrators only. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/portal", uma, NULL, NULL);
	assert_status(&reply, 403);
	axis_test_reply_clear(&reply);

	reply = admin_post(fx, admin, "/admin/portal", "/admin/portal/tokens", "");
	assert_status(&reply, 200);
	g_assert_true(g_regex_match(shown, reply.body, 0, &match));
	token = g_match_info_fetch(match, 1);
	assert_holds(reply.body, "only time this token is shown");
	axis_test_reply_clear(&reply);

	/* It works, the database holds only its hash, and the page never shows
	 * it again. */
	reply = send_to(fx, fx->portal_port, "POST", API "hello", token, NULL, "{}", NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	hash = axis_token_hash(token);
	id = N("SELECT id FROM portal_token WHERE token_hash = '%s'", hash);
	g_assert_cmpint(id, >, 0);
	g_assert_cmpint(N("SELECT count(*) FROM portal_token WHERE to_jsonb(portal_token)::text LIKE '%%%s%%'", token), ==,
		0);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/portal", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_lacks(reply.body, token);
	assert_holds(reply.body, "Active");
	assert_holds(reply.body, "port 7801");
	axis_test_reply_clear(&reply);
	pattern = g_strdup_printf("*%s*", token);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, pattern), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, pattern), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG, pattern), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: portal token * created by root*"), ==, 1);

	/* Four at most: the fixture's and this one are two. */
	for (i = 0; i < 2; i++) {
		reply = admin_post(fx, admin, "/admin/portal", "/admin/portal/tokens", "");
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
	}
	reply = admin_post(fx, admin, "/admin/portal", "/admin/portal/tokens", "");
	assert_status(&reply, 303);
	assert_holds(axis_test_reply_header(&reply, "Location"), "done=full");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM portal_token WHERE revoked_at IS NULL"), ==, 4);

	/* Revoked on the page: refused from the next request. */
	revoke = g_strdup_printf("/admin/portal/tokens/%" G_GINT64_FORMAT "/revoke", id);
	reply = admin_post(fx, admin, "/admin/portal", revoke, "");
	assert_status(&reply, 303);
	assert_holds(axis_test_reply_header(&reply, "Location"), "done=revoked");
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", token, NULL, "{}", NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM portal_token WHERE id = %" G_GINT64_FORMAT " AND revoked_by = 'root'", id),
		==, 1);
	/* A person who is not an administrator revokes nothing. */
	revoke = (g_free(revoke), g_strdup_printf("/admin/portal/tokens/%" G_GINT64_FORMAT "/revoke", fx->token_id));
	reply = axis_test_web_request(fx->session, fx->port, "POST", revoke, uma, NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_free(expect(fx, "hello", "{}", 200));
}

/* Scenarios: Wrong tokens are throttled; Throttled; and a valid token is
 * never held to its address's count. */
static void
test_token_throttle(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *wrong = axis_random_token();
	AxisTestReply reply;
	guint i;

	for (i = 0; i < AXIS_PORTAL_TOKEN_FREE_FAILURES + 1; i++) {
		reply = send_to(fx, fx->portal_port, "POST", API "hello", wrong, NULL, "{}", NULL);
		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
	}
	/* From now on the address waits, with a wrong token and with none. */
	reply = send_to(fx, fx->portal_port, "POST", API "hello", wrong, NULL, "{}", NULL);
	assert_status(&reply, 429);
	assert_api_reply(&reply);
	assert_holds(reply.body, "\"error\":\"throttled\"");
	g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", NULL, NULL, "{}", NULL);
	assert_status(&reply, 429);
	axis_test_reply_clear(&reply);
	/* The portal's valid token, from the same address (a proxy or a gateway
	 * shared with whoever sent the wrong ones), is looked up first and
	 * answered; and it clears nothing of the address's count. */
	g_free(expect(fx, "hello", "{}", 200));
	reply = send_to(fx, fx->portal_port, "POST", API "hello", wrong, NULL, "{}", NULL);
	assert_status(&reply, 429);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*carried a token that is not a portal's*"), ==,
		AXIS_PORTAL_TOKEN_FREE_FAILURES + 1);
	/* The delay passes. */
	axis_portal_api_set_time_offset(fx->api, G_GINT64_CONSTANT(3600) * G_USEC_PER_SEC);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", wrong, NULL, "{}", NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
}

/* ── The scope ──────────────────────────────────────────────────── */

/* A route's pattern as a path: a parameter is "1", a wildcard "x". */
static gchar *
path_of(const gchar *pattern)
{
	GString *path = g_string_new(NULL);
	const gchar *p = pattern;

	while (*p != '\0') {
		if (*p == ':') {
			g_string_append_c(path, '1');
			while (*p != '\0' && *p != '/')
				p++;
		} else if (*p == '*') {
			g_string_append_c(path, 'x');
			p++;
		} else {
			g_string_append_c(path, *p++);
		}
	}
	return g_string_free(path, FALSE);
}

/* Scenarios: Every other route of the daemon (here: every route of the test's
 * daemon, the shell's, the accounts' and the modules of the requests'
 * fixture, not every module of axisd); A new route is closed to the token; A
 * portal token elsewhere. */
static void
test_every_route(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) routes = axis_web_list_routes(fx->web);
	g_autofree gchar *bearer = g_strconcat("Bearer ", fx->token, NULL);
	const gchar *const with_token[] = { "Authorization", bearer, NULL };
	gint64 requests = N("SELECT count(*) FROM request");
	gint64 users = N("SELECT count(*) FROM app_user");
	gboolean saw_new = FALSE;
	guint opened = 0;
	guint i;

	g_assert_cmpuint(routes->len, >, 40);
	for (i = 0; i < routes->len; i++) {
		g_auto(GStrv) parts = g_strsplit(g_ptr_array_index(routes, i), " ", 3);
		gboolean public_route = g_strv_length(parts) == 3;
		const gchar *method = parts[0];
		g_autofree gchar *path = path_of(parts[public_route ? 2 : 1]);
		gboolean has_body = strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0;
		AxisTestReply plain;
		AxisTestReply reply;

		/* The token changes nothing: the answer is the one a request with no
		 * credential at all gets. */
		plain = axis_test_web_request(fx->session, fx->port, method, path, NULL, NULL, has_body ? "a=b" : NULL);
		reply = axis_test_web_request(fx->session, fx->port, method, path, NULL, with_token, has_body ? "a=b" : NULL);
		if (reply.status != plain.status ||
		    g_strcmp0(axis_test_reply_header(&reply, "Location"), axis_test_reply_header(&plain, "Location")) != 0)
			g_error("%s %s answers a portal token with %u and no credential with %u", method, path, reply.status,
				plain.status);
		/* And unless the route needs no session, it does not open. */
		if (!public_route && reply.status >= 200 && reply.status < 300)
			g_error("%s %s opened to a portal token (%u)", method, path, reply.status);
		if (reply.status >= 200 && reply.status < 300)
			opened++;
		assert_lacks(reply.body, "zz-opened");
		if (strcmp(path, "/zz-new-route") == 0) {
			saw_new = TRUE;
			assert_status(&reply, 303);
		}
		axis_test_reply_clear(&plain);
		axis_test_reply_clear(&reply);

		/* On the portal listener none of them exists. */
		reply = send_to(fx, fx->portal_port, "POST", path, fx->token, NULL, "{}", NULL);
		assert_status(&reply, 404);
		assert_api_reply(&reply);
		axis_test_reply_clear(&reply);
		reply = send_to(fx, fx->portal_port, method, path, fx->token, NULL, NULL, NULL);
		assert_status(&reply, 404);
		axis_test_reply_clear(&reply);
	}
	g_assert_true(saw_new);
	/* Only what needs no session answered at all (the login page, the
	 * static assets, the icons). */
	g_assert_cmpuint(opened, <, 12);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, requests);
	g_assert_cmpint(N("SELECT count(*) FROM app_user"), ==, users);
	/* What does something with the token is the eleven operations on the
	 * portal listener, and nothing else there is one. */
	g_assert_cmpuint(g_strv_length((gchar **)axis_portal_api_operations()), ==, AXIS_PORTAL_N_OPS);
	g_assert_cmpuint(AXIS_PORTAL_N_OPS, ==, 11);
}

/* Scenarios: A member that is not defined; No address is taken; Naming a
 * person does nothing. */
static void
test_members(Fixture *fx, gconstpointer data)
{
	/* The names the engine's forms use, and the ones an attacker would try. */
	static const gchar *const foreign[] = {
		"profile", "folder", "requester", "user_id", "approve", "root", "root_id", "profile_id", "quality", "user",
		"username", "requested_by", "role", "admin", "url", "address", "path", "hash", "file", "note", "comment",
		"episode", "episodes", "season", "monitor", "_csrf", "token", "limit", "offset", "provider", "decision", NULL
	};
	g_autofree gchar *proof = login(fx, "robin", ROBIN_PASSWORD);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	guint before = axis_test_plex_n_requests(fx->r->fa->api) + axis_test_plex_n_requests(fx->r->fa->images);
	const gchar *const *operations = axis_portal_api_operations();
	guint o;
	guint i;

	for (o = 0; operations[o] != NULL; o++) {
		for (i = 0; foreign[i] != NULL; i++) {
			g_autofree gchar *base = NULL;
			g_autofree gchar *body = NULL;

			/* A body that is the operation's own, plus the one member. */
			if (strcmp(operations[o], "hello") == 0) {
				base = g_strdup("");
			} else if (strcmp(operations[o], "login") == 0) {
				if (strcmp(foreign[i], "username") == 0)
					continue;
				base = g_strdup_printf("\"username\":\"robin\",\"password\":\"%s\",\"client_address\":\"198.51.100.20\","
					"\"user_agent\":\"t\",", ROBIN_PASSWORD);
			} else if (strcmp(operations[o], "search") == 0) {
				base = g_strdup_printf("\"proof\":\"%s\",\"query\":\"orchard\",", proof);
			} else if (strcmp(operations[o], "browse") == 0) {
				base = g_strdup_printf("\"proof\":\"%s\",\"list\":\"popular\",\"kind\":\"movie\",", proof);
			} else if (strcmp(operations[o], "title") == 0 || strcmp(operations[o], "art") == 0) {
				base = g_strdup_printf("\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",", proof, orchard);
			} else if (strcmp(operations[o], "request") == 0) {
				base = g_strdup_printf("\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"movie\",", proof, orchard);
			} else if (strcmp(operations[o], "withdraw") == 0) {
				base = g_strdup_printf("\"proof\":\"%s\",\"request\":\"AAAAAAAAAAAAAAAAAAAAAA\",", proof);
			} else {
				base = g_strdup_printf("\"proof\":\"%s\",", proof);
			}
			body = g_strdup_printf("{%s\"%s\":\"sam\"}", base, foreign[i]);
			g_free(expect(fx, operations[o], body, 422));
			g_free(body);
			body = g_strdup_printf("{%s\"%s\":3}", base, foreign[i]);
			g_free(expect(fx, operations[o], body, 422));
		}
		/* A minute for each operation: what is tried here is more than a
		 * person's and a token's budgets have in one. */
		axis_portal_api_set_time_offset(fx->api, (gint64)(o + 1) * 61 * G_USEC_PER_SEC);
	}
	/* Nothing was done by any of them, and nothing was fetched. */
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM web_session WHERE kind = 'portal'"), <=, 1);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->r->fa->api) + axis_test_plex_n_requests(fx->r->fa->images), ==,
		before);

	/* Members of the wrong type, out of range, or where they do not belong. */
	{
		g_autofree gchar *a = g_strdup_printf("{\"proof\":\"%s\",\"id\":\"%" G_GINT64_FORMAT "\"}", proof, orchard);
		g_autofree gchar *b = g_strdup_printf("{\"proof\":\"%s\",\"id\":0}", proof);
		g_autofree gchar *c = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"movie\","
			"\"seasons\":\"all\"}", proof, orchard);
		g_autofree gchar *d = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\"}", proof,
			harbour);
		g_autofree gchar *e = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
			"\"seasons\":[1,1]}", proof, harbour);
		g_autofree gchar *f = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
			"\"seasons\":[\"1\"]}", proof, harbour);
		g_autofree gchar *g = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"episode\"}",
			proof, harbour);
		g_autofree gchar *h = g_strdup_printf("{\"proof\":\"%s\",\"proof\":\"%s\"}", proof, proof);
		g_autofree gchar *k = g_strdup_printf("{\"proof\":\"%s\",\"page\":0}", proof);
		g_autofree gchar *l = g_strdup_printf("{\"proof\":\"%s\",\"request\":\"../../requests\"}", proof);
		g_autofree gchar *m = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
			"\"seasons\":\"1x02\"}", proof, harbour);

		g_free(expect(fx, "title", a, 422));
		g_free(expect(fx, "art", b, 422));
		g_free(expect(fx, "request", c, 422));
		g_free(expect(fx, "request", d, 422));
		g_free(expect(fx, "request", e, 422));
		g_free(expect(fx, "request", f, 422));
		g_free(expect(fx, "request", g, 422));
		g_free(expect(fx, "requests", h, 422));
		g_free(expect(fx, "requests", k, 422));
		g_free(expect(fx, "withdraw", l, 422));
		g_free(expect(fx, "request", m, 422));
		g_free(expect(fx, "hello", "{\"proof\":\"x\"}", 422));
	}
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
}

/* ── Login ──────────────────────────────────────────────────────── */

/* Scenarios: A requester logs in; Uniform failures; An administrator's
 * password opens nothing here; Not a session of the engine; Stored as a
 * hash; Kinds do not cross. */
static void
test_login(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *username;
		const gchar *password;
	} failures[] = {
		{ "nobody-of-that-name", ROBIN_PASSWORD },	/* unknown */
		{ "dana", OTHER_PASSWORD },			/* disabled, the right password */
		{ "robin", "not-the-right-password" },		/* wrong */
		{ "root", ADMIN_PASSWORD },			/* an administrator, the right password */
		{ "uma", USER_PASSWORD },			/* an ordinary user, the right password */
		{ "not a username!", ROBIN_PASSWORD }		/* cannot be a name */
	};
	g_autofree gchar *body = login_body("robin", ROBIN_PASSWORD, "198.51.100.20");
	g_autofree gchar *answer = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *proof = NULL;
	g_autofree gchar *admin = NULL;
	g_autofree gchar *pattern = NULL;
	gint64 sessions;
	guint hashes;
	guint i;

	hashes = axis_password_verify_count();
	answer = expect(fx, "login", body, 200);
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes + 1);
	{
		g_autoptr(JsonObject) object = object_of(answer);
		g_autofree gchar *members = members_of(object);

		g_assert_cmpstr(members, ==, "device,expires_in,name,proof");
		g_assert_cmpstr(json_object_get_string_member(object, "name"), ==, "robin");
		g_assert_cmpint(json_object_get_int_member(object, "expires_in"), ==, 30 * 24 * 3600);
		proof = g_strdup(json_object_get_string_member(object, "proof"));
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: portal login: user=robin reported_client="
		"198.51.100.20*"), ==, 1);

	/* Every failure: one status, one body, and one call of the password
	 * check (counted; the time each takes is not measured, and the stored
	 * hashes here have the dummy's limits). */
	axis_web_auth_wait_writes(fx->auth);
	sessions = N("SELECT count(*) FROM web_session");
	for (i = 0; i < G_N_ELEMENTS(failures); i++) {
		g_autofree gchar *address = g_strdup_printf("198.51.100.%u", 30 + i);
		g_autofree gchar *attempt = login_body(failures[i].username, failures[i].password, address);
		g_autofree gchar *refused = NULL;

		hashes = axis_password_verify_count();
		refused = expect(fx, "login", attempt, 401);
		g_assert_cmpuint(axis_password_verify_count(), ==, hashes + 1);
		if (first == NULL)
			first = g_strdup(refused);
		g_assert_cmpstr(refused, ==, first);
		assert_lacks(refused, "proof");
	}
	/* No session of any kind was made for any of them, here or for the
	 * engine's interface. */
	axis_web_auth_wait_writes(fx->auth);
	g_assert_cmpint(N("SELECT count(*) FROM web_session"), ==, sessions);
	g_assert_cmpint(N("SELECT count(*) FROM web_session s JOIN app_user u ON u.id = s.user_id"
		" WHERE u.username IN ('root', 'uma', 'dana')"), ==, 0);
	/* A name that is not an account is never written down. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*nobody-of-that-name*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*not a username*"), ==, 0);
	/* Nor a password, nor a proof. */
	pattern = g_strdup_printf("*%s*", proof);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, pattern), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*" ROBIN_PASSWORD "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*" ADMIN_PASSWORD "*"), ==, 0);

	/* Stored as a hash, with its kind. */
	{
		g_autofree gchar *hash = axis_token_hash(proof);

		g_assert_cmpint(N("SELECT count(*) FROM web_session WHERE to_jsonb(web_session)::text LIKE '%%%s%%'", proof), ==,
			0);
		g_assert_cmpint(N("SELECT count(*) FROM web_session WHERE token_hash = '%s' AND kind = 'portal' AND user_id = %"
			G_GINT64_FORMAT " AND client_address = '198.51.100.20'", hash, fx->r->robin), ==, 1);
	}

	/* The kinds do not cross.  A proof is no session cookie of the engine's
	 * interface... */
	{
		g_autofree gchar *cookie = g_strconcat("axis_session=", proof, NULL);
		AxisTestReply reply = axis_test_web_request(fx->session, fx->port, "GET", "/", cookie, NULL, NULL);
		g_autofree gchar *who = NULL;
		g_autofree gchar *again = proof_body(proof);

		assert_status(&reply, 303);
		g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
		axis_test_reply_clear(&reply);
		/* ... and trying it there did not end it where it belongs. */
		who = expect(fx, "whoami", again, 200);
		assert_holds(who, "\"name\":\"robin\"");
	}
	/* ... and an engine session's cookie value is no proof. */
	admin = engine_session(fx, "root", ADMIN_PASSWORD);
	{
		const gchar *value = strstr(admin, "axis_session=");
		g_autofree gchar *token = NULL;
		g_autofree gchar *as_proof = NULL;
		AxisTestReply reply;

		g_assert_nonnull(value);
		token = g_strndup(value + strlen("axis_session="), strcspn(value + strlen("axis_session="), ";"));
		g_assert_cmpuint(strlen(token), ==, AXIS_PORTAL_PROOF_LEN);
		as_proof = proof_body(token);
		g_free(expect(fx, "whoami", as_proof, 401));
		g_free(expect(fx, "requests", as_proof, 401));
		g_free(expect(fx, "logout", as_proof, 401));
		/* The administrator's session is as it was. */
		reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/users", admin, NULL, NULL);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
	}
}

/* ── The proof ──────────────────────────────────────────────────── */

/* Scenarios: The token without a proof; A made-up proof; Naming a person
 * does nothing; Only the caller's requests; Another person's request;
 * Logout. */
static void
test_proof(Fixture *fx, gconstpointer data)
{
	static const gchar *const need_proof[] = {
		"requests", "request", "search", "browse", "title", "art", "whoami", "withdraw", "logout", NULL
	};
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *sam = login(fx, "sam", SAM_PASSWORD);
	g_autofree gchar *made_up = axis_random_token();
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	g_autofree gchar *no_proof = NULL;
	g_autofree gchar *sams = NULL;
	g_autofree gchar *robins = NULL;
	g_autofree gchar *sam_request = NULL;
	g_autofree gchar *robin_request = NULL;
	g_autofree gchar *unknown = NULL;
	guint i;

	/* Without a proof, with a made-up one, with a malformed one: 401 and one
	 * body, whatever else the body holds, and nothing is read or done. */
	for (i = 0; need_proof[i] != NULL; i++) {
		g_autofree gchar *full = g_strdup_printf("{\"id\":%" G_GINT64_FORMAT ",\"kind\":\"movie\",\"query\":\"orchard\","
			"\"list\":\"popular\",\"request\":\"AAAAAAAAAAAAAAAAAAAAAA\"}", orchard);
		g_autofree gchar *with_made_up = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT
			",\"kind\":\"movie\"}", made_up, orchard);
		g_autofree gchar *a = expect(fx, need_proof[i], "{}", 401);
		g_autofree gchar *b = expect(fx, need_proof[i], full, 401);
		g_autofree gchar *c = expect(fx, need_proof[i], with_made_up, 401);
		g_autofree gchar *d = expect(fx, need_proof[i], "{\"proof\":\"short\"}", 401);
		g_autofree gchar *e = expect(fx, need_proof[i], "{\"proof\":\"robin\",\"user\":\"robin\"}", 401);

		if (no_proof == NULL)
			no_proof = g_strdup(a);
		g_assert_cmpstr(a, ==, no_proof);
		g_assert_cmpstr(b, ==, no_proof);
		g_assert_cmpstr(c, ==, no_proof);
		g_assert_cmpstr(d, ==, no_proof);
		g_assert_cmpstr(e, ==, no_proof);
	}
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, 0);

	/* Each asks for a movie. */
	sams = ask_movie(fx, sam, orchard, 200);
	robins = ask_movie(fx, robin, ledger, 200);
	{
		g_autoptr(JsonObject) a = object_of(sams);
		g_autoptr(JsonObject) b = object_of(robins);

		g_assert_cmpstr(json_object_get_string_member(a, "outcome"), ==, "made");
		sam_request = g_strdup(json_object_get_string_member(json_object_get_object_member(a, "request"), "id"));
		robin_request = g_strdup(json_object_get_string_member(json_object_get_object_member(b, "request"), "id"));
	}
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'portal' AND requested_by = %" G_GINT64_FORMAT,
		fx->r->sam), ==, 1);

	/* A member that names sam is refused; without it the answer is robin's
	 * requests and no other. */
	{
		g_autofree gchar *naming = g_strdup_printf("{\"proof\":\"%s\",\"requester\":\"sam\"}", robin);
		g_autofree gchar *by_id = g_strdup_printf("{\"proof\":\"%s\",\"user_id\":%" G_GINT64_FORMAT "}", robin,
			fx->r->sam);
		g_autofree gchar *plain = proof_body(robin);
		g_autofree gchar *list = NULL;
		g_autoptr(JsonObject) object = NULL;
		JsonArray *requests;

		g_free(expect(fx, "requests", naming, 422));
		g_free(expect(fx, "requests", by_id, 422));
		list = expect(fx, "requests", plain, 200);
		object = object_of(list);
		requests = json_object_get_array_member(object, "requests");
		g_assert_cmpuint(json_array_get_length(requests), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(requests, 0), "id"), ==,
			robin_request);
		assert_lacks(list, sam_request);
		assert_lacks(list, "Lantern Orchard");
	}

	/* Another person's request is one that does not exist: the very answer
	 * of an identifier that was never issued, and it is unchanged. */
	{
		g_autofree gchar *anothers = g_strdup_printf("{\"proof\":\"%s\",\"request\":\"%s\"}", robin, sam_request);
		g_autofree gchar *never = g_strdup_printf("{\"proof\":\"%s\",\"request\":\"%s\"}", robin,
			"AAAAAAAAAAAAAAAAAAAAAA");
		g_autofree gchar *a = expect(fx, "withdraw", anothers, 404);

		unknown = expect(fx, "withdraw", never, 404);
		g_assert_cmpstr(a, ==, unknown);
		g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'waiting'", sam_request),
			==, 1);
	}
	/* A follower does not withdraw it either: only who made it. */
	{
		g_autofree gchar *follow = ask_movie(fx, robin, orchard, 200);
		g_autofree gchar *anothers = g_strdup_printf("{\"proof\":\"%s\",\"request\":\"%s\"}", robin, sam_request);
		g_autofree gchar *a = NULL;

		assert_holds(follow, "\"outcome\":\"followed\"");
		a = expect(fx, "withdraw", anothers, 404);
		g_assert_cmpstr(a, ==, unknown);
		g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'waiting'", sam_request),
			==, 1);
	}
	/* Their own they withdraw, once. */
	{
		g_autofree gchar *own = g_strdup_printf("{\"proof\":\"%s\",\"request\":\"%s\"}", robin, robin_request);

		g_free(expect(fx, "withdraw", own, 200));
		g_free(expect(fx, "withdraw", own, 409));
		g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'withdrawn'",
			robin_request), ==, 1);
	}

	/* Logout: the proof is none from the next request, and only that one. */
	{
		g_autofree gchar *body = proof_body(robin);
		g_autofree gchar *other = proof_body(sam);
		g_autofree gchar *after = NULL;

		g_free(expect(fx, "logout", body, 200));
		after = expect(fx, "whoami", body, 401);
		g_assert_cmpstr(after, ==, no_proof);
		g_free(expect(fx, "logout", body, 401));
		g_free(expect(fx, "whoami", other, 200));
	}
}

/* Scenarios: Disabled at once; A password or role change ends proofs; Ended
 * by an administrator; A role change ends sessions; The last administrator;
 * Created by an administrator; Password rules for a requester. */
static void
test_account_changes(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *admin = engine_session(fx, "root", ADMIN_PASSWORD);
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *robin2 = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *sam = login(fx, "sam", SAM_PASSWORD);
	g_autofree gchar *tess = login(fx, "tess", OTHER_PASSWORD);
	g_autofree gchar *vic = login(fx, "vic", OTHER_PASSWORD);
	g_autofree gchar *robin_body = proof_body(robin);
	g_autofree gchar *robin2_body = proof_body(robin2);
	g_autofree gchar *sam_body = proof_body(sam);
	g_autofree gchar *tess_body = proof_body(tess);
	g_autofree gchar *vic_body = proof_body(vic);
	g_autofree gchar *path = NULL;
	AxisTestReply reply;

	/* The Users page lists a requester's portal sessions, and ends them. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/users", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "End 2 portal sessions");
	assert_holds(reply.body, "Requester");
	axis_test_reply_clear(&reply);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/portal-sessions/end", fx->r->robin);
	reply = admin_post(fx, admin, "/admin/users", path, "");
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_free(expect(fx, "whoami", robin_body, 401));
	g_free(expect(fx, "whoami", robin2_body, 401));
	g_free(expect(fx, "whoami", sam_body, 200));
	axis_web_auth_wait_writes(fx->auth);
	g_assert_cmpint(N("SELECT count(*) FROM web_session WHERE user_id = %" G_GINT64_FORMAT, fx->r->robin), ==, 0);

	/* Disabled: refused in the very next request, and no login either. */
	g_free(path);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/disable", fx->r->sam);
	reply = admin_post(fx, admin, "/admin/users", path, "");
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_free(expect(fx, "whoami", sam_body, 401));
	{
		gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);

		g_free(ask_movie(fx, sam, orchard, 401));
		g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	}
	{
		g_autofree gchar *body = login_body("sam", SAM_PASSWORD, "198.51.100.21");

		g_free(expect(fx, "login", body, 401));
	}

	/* A password change ends every proof issued before. */
	g_free(path);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/password", fx->tess);
	reply = admin_post(fx, admin, "/admin/users", path, "password=a-new-made-up-password");
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_free(expect(fx, "whoami", tess_body, 401));
	{
		g_autofree gchar *fresh = login(fx, "tess", "a-new-made-up-password");
		g_autofree gchar *body = proof_body(fresh);

		g_free(expect(fx, "whoami", body, 200));
	}
	/* The rules of a requester's password hold there too. */
	reply = admin_post(fx, admin, "/admin/users", path, "password=elevenchars");
	assert_status(&reply, 400);
	assert_holds(reply.body, "at least 12 characters");
	axis_test_reply_clear(&reply);
	reply = admin_post(fx, admin, "/admin/users", path, "password=xx-Tess-is-in-it");
	assert_status(&reply, 400);
	assert_holds(reply.body, "must not contain the username");
	axis_test_reply_clear(&reply);

	/* A role change ends them too: vic becomes an ordinary user.  The
	 * password vic typed into the portal opens nothing else: the change
	 * needs a new one, held to the rule of the new role, and not the old. */
	g_free(path);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/role", fx->vic);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/users", admin, NULL, NULL);
	assert_status(&reply, 200);
	{
		g_autofree gchar *field = g_strdup_printf("id=\"role-user-password-%" G_GINT64_FORMAT "\"", fx->vic);

		assert_holds(reply.body, field);
	}
	axis_test_reply_clear(&reply);
	{
		static const gchar *const refused[] = {
			"role=user", "role=user&password=", "role=user&password=short", "role=admin",
			"role=user&password=" OTHER_PASSWORD, NULL
		};
		guint i;

		for (i = 0; refused[i] != NULL; i++) {
			reply = admin_post(fx, admin, "/admin/users", path, refused[i]);
			assert_status(&reply, 400);
			axis_test_reply_clear(&reply);
		}
		g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE id = %" G_GINT64_FORMAT " AND role = 'requester'",
			fx->vic), ==, 1);
		g_free(expect(fx, "whoami", vic_body, 200));
	}
	reply = admin_post(fx, admin, "/admin/users", path, "role=user&password=a-new-password-of-vics");
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_free(expect(fx, "whoami", vic_body, 401));
	{
		g_autofree gchar *body = login_body("vic", OTHER_PASSWORD, "198.51.100.22");
		g_autofree gchar *as_user = NULL;

		/* No longer a requester: no login here.  At the engine's own login
		 * the password the portal saw fails, and the new one opens. */
		g_free(expect(fx, "login", body, 401));
		reply = engine_login(fx, "vic", OTHER_PASSWORD);
		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
		as_user = engine_session(fx, "vic", "a-new-password-of-vics");
		g_assert_nonnull(as_user);
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*role changed: user=vic role=user (with a new "
			"password)*"), ==, 1);
	}

	/* And the other way: uma, logged in at the engine, is made a requester.
	 * Her session ends at once, and the engine's login refuses her. */
	{
		g_autofree gchar *uma = engine_session(fx, "uma", USER_PASSWORD);
		g_autofree gchar *wrong_body = NULL;

		reply = axis_test_web_request(fx->session, fx->port, "GET", "/requests", uma, NULL, NULL);
		assert_status(&reply, 200);
		axis_test_reply_clear(&reply);
		g_free(path);
		path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/role", fx->uma);
		reply = admin_post(fx, admin, "/admin/users", path, "role=requester");
		assert_status(&reply, 303);
		axis_test_reply_clear(&reply);
		reply = axis_test_web_request(fx->session, fx->port, "GET", "/requests", uma, NULL, NULL);
		assert_status(&reply, 303);
		g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
		axis_test_reply_clear(&reply);
		/* Scenario: A requester at the engine's login.  The right password:
		 * the status and the message of a wrong one, and no session. */
		reply = engine_login(fx, "uma", "not-her-password-at-all");
		assert_status(&reply, 401);
		wrong_body = g_strdup(strstr(reply.body, "The username or password is incorrect."));
		g_assert_nonnull(wrong_body);
		axis_test_reply_clear(&reply);
		reply = engine_login(fx, "uma", USER_PASSWORD);
		assert_status(&reply, 401);
		assert_holds(reply.body, "The username or password is incorrect.");
		assert_lacks(reply.body, "equester");
		{
			g_autofree gchar *set = cookies_after(&reply);

			assert_lacks(set, "axis_session");
		}
		axis_test_reply_clear(&reply);
		axis_web_auth_wait_writes(fx->auth);
		g_assert_cmpint(N("SELECT count(*) FROM web_session WHERE user_id = %" G_GINT64_FORMAT, fx->uma), ==, 0);
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*login failed: user=uma*a requester*"), ==, 1);
	}

	/* The last administrator cannot be made a requester. */
	g_free(path);
	path = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/role", fx->r->admin);
	reply = admin_post(fx, admin, "/admin/users", path, "role=requester");
	assert_status(&reply, 409);
	assert_holds(reply.body, "only enabled administrator");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'root' AND role = 'admin'"), ==, 1);

	/* Created by an administrator: a requester, with both switches off; and
	 * the password's rules. */
	reply = admin_post(fx, admin, "/admin/users", "/admin/users", "username=quill&password=elevenchars&role=requester");
	assert_status(&reply, 400);
	assert_holds(reply.body, "at least 12 characters");
	axis_test_reply_clear(&reply);
	reply = admin_post(fx, admin, "/admin/users", "/admin/users",
		"username=quill&password=my-QUILL-password&role=requester");
	assert_status(&reply, 400);
	assert_holds(reply.body, "must not contain the username");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'quill'"), ==, 0);
	/* (Eleven characters are enough for an account of the engine's own.) */
	reply = admin_post(fx, admin, "/admin/users", "/admin/users", "username=wren&password=elevenchars&role=user");
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	reply = admin_post(fx, admin, "/admin/users", "/admin/users",
		"username=quill&password=a-made-up-pass-of-quality&role=requester");
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'quill' AND role = 'requester'"
		" AND NOT request_auto_movies AND NOT request_auto_series AND NOT disabled"), ==, 1);
	{
		g_autofree gchar *proof = login(fx, "quill", "a-made-up-pass-of-quality");
		g_autofree gchar *body = proof_body(proof);

		g_free(expect(fx, "whoami", body, 200));
	}
}

/* Scenario: No session for a requester (every route of the main listener):
 * a session row of the engine's interface written from outside for an
 * account whose role is requester is no session on any route. */
static void
test_no_session_for_requester(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) routes = axis_web_list_routes(fx->web);
	g_autofree gchar *cookie = g_strconcat("axis_session=", fx->outside, NULL);
	g_autofree gchar *hash = axis_token_hash(fx->outside);
	g_autofree gchar *as_proof = proof_body(fx->outside);
	AxisTestReply reply;
	guint i;

	/* The daemon read the row at its start (it is of an enabled account,
	 * under its epoch) and holds the session in memory; the purge of rows
	 * that can no longer be used has deleted it from the database since. */
	g_assert_cmpuint(axis_web_auth_count_held_sessions(fx->auth, fx->r->robin), ==, 1);
	axis_web_auth_wait_writes(fx->auth);
	g_assert_cmpint(N("SELECT count(*) FROM web_session WHERE token_hash = '%s'", hash), ==, 0);
	for (i = 0; i < routes->len; i++) {
		g_auto(GStrv) parts = g_strsplit(g_ptr_array_index(routes, i), " ", 3);
		gboolean public_route = g_strv_length(parts) == 3;
		g_autofree gchar *path = path_of(parts[public_route ? 2 : 1]);
		gboolean has_body = strcmp(parts[0], "GET") != 0 && strcmp(parts[0], "HEAD") != 0;
		AxisTestReply plain;

		plain = axis_test_web_request(fx->session, fx->port, parts[0], path, NULL, NULL, has_body ? "a=b" : NULL);
		reply = axis_test_web_request(fx->session, fx->port, parts[0], path, cookie, NULL, has_body ? "a=b" : NULL);
		if (reply.status != plain.status ||
		    g_strcmp0(axis_test_reply_header(&reply, "Location"), axis_test_reply_header(&plain, "Location")) != 0)
			g_error("%s %s answers a requester's session with %u and no session with %u", parts[0], path, reply.status,
				plain.status);
		if (!public_route && reply.status >= 200 && reply.status < 300)
			g_error("%s %s opened to a requester's session (%u)", parts[0], path, reply.status);
		assert_lacks(reply.body, "robin");
		axis_test_reply_clear(&plain);
		axis_test_reply_clear(&reply);
	}
	/* Presented once, it was ended. */
	g_assert_cmpuint(axis_web_auth_count_held_sessions(fx->auth, fx->r->robin), ==, 0);
	/* Nor is it a proof: it is of the other kind. */
	g_free(expect(fx, "whoami", as_proof, 401));
}

/* ── Answers ────────────────────────────────────────────────────── */

#define TITLE_MEMBERS   "has_poster,id,kind,name,overview,state,year"
#define REQUEST_MEMBERS "asked_at,changed_at,id,kind,mine,name,seasons,state,title,year"

/* Scenarios: A search; Asked once; A search result's members; Nothing of
 * the library's inner workings; Who asked follows the setting; Too long;
 * Browse asks nobody; A poster by title; Not held; and the adult filter. */
static void
test_answers(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *sam = login(fx, "sam", SAM_PASSWORD);
	g_autofree gchar *search = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"lantern orchard\",\"kind\":\"movie\"}",
		robin);
	g_autofree gchar *search_sam = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"Lantern  ORCHARD\","
		"\"kind\":\"movie\"}", sam);
	g_autofree gchar *found = NULL;
	g_autofree gchar *everything = NULL;
	gint64 orchard;
	gint64 ledger;
	gint64 harbour;
	gint64 annex;
	gint64 marked_root;
	guint i;

	/* A library with names no answer may hold. */
	marked_root = axis_test_library_add_root(fx->r->t, fx->r->volume_id, fx->r->disk, FOLDER_MARK, AXIS_MEDIA_MOVIES);
	N("UPDATE quality_profile SET name = '" PROFILE_MARK "' WHERE id = %" G_GINT64_FORMAT " RETURNING id",
		fx->r->profile_id);

	/* The provider is asked once, and each result carries its state. */
	found = expect(fx, "search", search, 200);
	g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/series"), ==, 0);
	orchard = N("SELECT id FROM meta_title WHERE provider_id = '" AXIS_TEST_REQUESTS_ORCHARD "'");
	ledger = N("SELECT id FROM meta_title WHERE provider_id = '" AXIS_TEST_REQUESTS_LEDGER "'");
	annex = N("SELECT id FROM meta_title WHERE provider_id = '" AXIS_TEST_REQUESTS_ANNEX "'");
	{
		g_autoptr(JsonObject) object = object_of(found);
		g_autofree gchar *members = members_of(object);
		JsonArray *titles = json_object_get_array_member(object, "titles");

		g_assert_cmpstr(members, ==, "more,page,stale,titles");
		g_assert_false(json_object_get_boolean_member(object, "stale"));
		g_assert_cmpint(json_object_get_int_member(object, "page"), ==, 1);
		/* Four: the one marked as adult is not among them. */
		g_assert_cmpuint(json_array_get_length(titles), ==, 4);
		for (i = 0; i < json_array_get_length(titles); i++) {
			JsonObject *title = json_array_get_object_element(titles, i);
			g_autofree gchar *each = members_of(title);

			g_assert_cmpstr(each, ==, TITLE_MEMBERS);
			g_assert_cmpstr(json_object_get_string_member(title, "state"), ==, "open");
			g_assert_cmpstr(json_object_get_string_member(title, "kind"), ==, "movie");
			g_assert_cmpint(json_object_get_int_member(title, "id"), !=, annex);
			g_assert_false(json_object_get_boolean_member(title, "has_poster"));
		}
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(titles, 0), "name"), ==,
			"The Lantern Orchard");
	}
	assert_lacks(found, "Velvet Annex");
	/* Provider text is never markup, however it is read. */
	assert_lacks(found, "<b>");
	assert_holds(found, "\\u003cb\\u003elanterns");
	g_assert_cmpint(N("SELECT adult::integer FROM meta_title WHERE id = %" G_GINT64_FORMAT, annex), ==, 1);

	/* The same search, five times, by two people: asked once. */
	for (i = 0; i < 2; i++) {
		g_free(expect(fx, "search", search, 200));
		g_free(expect(fx, "search", search_sam, 200));
	}
	g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, 1);

	/* Too long, a control character, nothing to look for: refused, and the
	 * provider is not asked. */
	{
		g_autofree gchar *long_query = g_strnfill(101, 'q');
		g_autofree gchar *exact = g_strnfill(100, 'q');
		g_autofree gchar *a = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"%s\",\"kind\":\"movie\"}", robin, long_query);
		g_autofree gchar *b = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"orchard\\u0007\",\"kind\":\"movie\"}",
			robin);
		g_autofree gchar *c = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"or\\nchard\",\"kind\":\"movie\"}", robin);
		g_autofree gchar *d = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"   \",\"kind\":\"movie\"}", robin);
		g_autofree gchar *e = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"%s\",\"kind\":\"movie\"}", robin, exact);
		/* A page the engine's search does not ask for: refused by the reader,
		 * before the search budgets count it, and for what it is. */
		g_autofree gchar *third = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"orchard\",\"kind\":\"movie\","
			"\"page\":3}", robin);
		g_autofree gchar *second_page = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"orchard\",\"kind\":\"movie\","
			"\"page\":2}", robin);
		g_autofree gchar *refused = NULL;
		guint asked = axis_test_plex_count(fx->r->fa->api, "/search/film");

		g_free(expect(fx, "search", a, 422));
		g_free(expect(fx, "search", b, 422));
		g_free(expect(fx, "search", c, 422));
		g_free(expect(fx, "search", d, 422));
		refused = expect(fx, "search", third, 422);
		assert_holds(refused, "two pages at most");
		g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, asked);
		g_free(expect(fx, "search", second_page, 200));
		asked = axis_test_plex_count(fx->r->fa->api, "/search/film");
		g_free(expect(fx, "search", e, 200));
		g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, asked + 1);
	}

	/* Browse answers from what is stored and asks nobody.  (The fixture's
	 * provider offers no list, so the answer holds no title: the adult
	 * filter and the members of a title are not seen through browse here.) */
	{
		g_autofree gchar *browse = g_strdup_printf("{\"proof\":\"%s\",\"list\":\"popular\",\"kind\":\"movie\"}", robin);
		g_autofree gchar *recommended = g_strdup_printf("{\"proof\":\"%s\",\"list\":\"recommended\","
			"\"kind\":\"movie\"}", robin);
		g_autofree gchar *nonsense = g_strdup_printf("{\"proof\":\"%s\",\"list\":\"everything\",\"kind\":\"movie\"}",
			robin);
		guint asked = axis_test_plex_n_requests(fx->r->fa->api);
		g_autofree gchar *listed = expect(fx, "browse", browse, 200);
		g_autoptr(JsonObject) object = object_of(listed);
		g_autofree gchar *members = members_of(object);

		g_assert_cmpstr(members, ==, "more,page,titles");
		g_free(expect(fx, "browse", recommended, 422));
		g_free(expect(fx, "browse", nonsense, 422));
		g_assert_cmpuint(axis_test_plex_n_requests(fx->r->fa->api), ==, asked);
	}

	/* A title, and a series with its seasons. */
	harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	{
		g_autofree gchar *one = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, harbour);
		g_autofree gchar *shown = expect(fx, "title", one, 200);
		g_autoptr(JsonObject) object = object_of(shown);
		g_autofree gchar *members = members_of(object);
		JsonObject *title = json_object_get_object_member(object, "title");
		g_autofree gchar *of_title = members_of(title);
		JsonArray *seasons = json_object_get_array_member(title, "seasons");

		g_assert_cmpstr(members, ==, "title");
		g_assert_cmpstr(of_title, ==, "has_poster,id,kind,name,overview,seasons,state,year");
		g_assert_cmpuint(json_array_get_length(seasons), ==, 4);
		for (i = 0; i < json_array_get_length(seasons); i++) {
			g_autofree gchar *each = members_of(json_array_get_object_element(seasons, i));

			g_assert_cmpstr(each, ==, "aired,episodes,number,state");
		}
		g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(seasons, 1), "episodes"), ==, 2);
	}
	/* A title its provider marks as adult is one that does not exist: the
	 * answer of a number never given, for the title and for its poster. */
	{
		g_autofree gchar *marked = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, annex);
		g_autofree gchar *never = g_strdup_printf("{\"proof\":\"%s\",\"id\":987654}", robin);
		g_autofree gchar *a = expect(fx, "title", marked, 404);
		g_autofree gchar *b = expect(fx, "title", never, 404);
		g_autofree gchar *c = expect(fx, "art", marked, 404);
		g_autofree gchar *d = expect(fx, "art", never, 404);
		g_autofree gchar *e = ask_movie(fx, robin, annex, 404);
		g_autofree gchar *f = ask_movie(fx, robin, 987654, 404);
		/* Asked for as the other kind, the number says no more: not "the
		 * title is not a series". */
		g_autofree gchar *as_series = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
			"\"seasons\":\"all\"}", robin, annex);
		g_autofree gchar *never_series = g_strdup_printf("{\"proof\":\"%s\",\"id\":987654,\"kind\":\"series\","
			"\"seasons\":[1]}", robin);
		g_autofree gchar *g = expect(fx, "request", as_series, 404);
		g_autofree gchar *h = expect(fx, "request", never_series, 404);
		g_autofree gchar *k = NULL;

		g_assert_cmpstr(a, ==, b);
		g_assert_cmpstr(c, ==, d);
		g_assert_cmpstr(e, ==, f);
		g_assert_cmpstr(g, ==, f);
		g_assert_cmpstr(h, ==, f);
		/* Nor whether it is closed to requests: not "closed". */
		N("INSERT INTO request_closed_title (title_id, kind, closed_by) VALUES (%" G_GINT64_FORMAT ", 'movie', 'root')"
			" RETURNING title_id", annex);
		k = ask_movie(fx, robin, annex, 404);
		g_assert_cmpstr(k, ==, f);
		g_free(expect(fx, "request", as_series, 404));
		N("DELETE FROM request_closed_title WHERE title_id = %" G_GINT64_FORMAT " RETURNING title_id", annex);
		g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	}

	/* A poster: not held, then held; nothing is fetched for a requester. */
	{
		g_autofree gchar *art = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, orchard);
		g_autofree gchar *one = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, orchard);
		g_autofree gchar *hash = NULL;
		guint fetched = axis_test_plex_n_requests(fx->r->fa->images);
		AxisTestReply reply;

		N("UPDATE meta_title SET poster_ref = '/p101.png' WHERE id = %" G_GINT64_FORMAT " RETURNING id", orchard);
		g_free(expect(fx, "art", art, 404));
		{
			g_autofree gchar *shown = expect(fx, "title", one, 200);

			assert_holds(shown, "\"has_poster\":false");
		}
		axis_test_pump_for(200);
		g_assert_cmpuint(axis_test_plex_n_requests(fx->r->fa->images), ==, fetched);
		/* The engine fetches it (an administrator looked at the title). */
		g_assert_true(axis_meta_artwork_fetch(fx->r->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &hash,
			&error));
		g_assert_no_error(error);
		reply = call(fx, "art", art);
		assert_status(&reply, 200);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Content-Type"), ==, "image/png");
		g_assert_cmpuint(reply.length, >, 8);
		g_assert_cmpint(memcmp(reply.body, "\x89PNG", 4), ==, 0);
		g_assert_null(axis_test_reply_header(&reply, "Set-Cookie"));
		axis_test_reply_clear(&reply);
		{
			g_autofree gchar *shown = expect(fx, "title", one, 200);

			assert_holds(shown, "\"has_poster\":true");
			/* No address of the image, and no name of its file. */
			assert_lacks(shown, hash);
			assert_lacks(shown, "p101");
		}
	}

	/* A request, with files and a grab behind it, in a marked folder. */
	{
		g_autofree gchar *made = ask_movie(fx, sam, orchard, 200);
		g_autoptr(JsonObject) object = object_of(made);
		g_autofree gchar *members = members_of(object);
		g_autofree gchar *of_request = members_of(json_object_get_object_member(object, "request"));
		g_autofree gchar *id = g_strdup(json_object_get_string_member(json_object_get_object_member(object, "request"),
			"id"));
		gboolean added = FALSE;
		gint64 movie;

		g_assert_cmpstr(members, ==, "followed,left_out,movies,outcome,request,seasons");
		g_assert_cmpstr(of_request, ==, REQUEST_MEMBERS);
		g_assert_true(axis_requests_approve(fx->r->requests, id, "root", marked_root, fx->r->profile_id, &added, &error));
		g_assert_no_error(error);
		g_assert_true(added);
		movie = N("SELECT movie_id FROM request WHERE public_id = '%s'", id);
		axis_test_requests_movie_grab(fx->r, movie, NULL);
		axis_test_requests_movie_file(fx->r, movie);
	}
	{
		static const gint seasons[] = { 1, 2 };
		g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
			"\"seasons\":[%d,%d]}", robin, harbour, seasons[0], seasons[1]);
		g_autofree gchar *made = expect(fx, "request", body, 200);
		g_autoptr(JsonObject) object = object_of(made);
		g_autofree gchar *of_request = members_of(json_object_get_object_member(object, "request"));

		g_assert_cmpstr(of_request, ==, "asked_at,changed_at,have,id,kind,mine,name,of,seasons,state,title,year");
	}
	{
		g_autofree gchar *follow = ask_movie(fx, robin, orchard, 200);

		assert_holds(follow, "\"outcome\":\"available\"");
	}

	/* Every answer, read as text: none of the library's inner workings. */
	{
		g_autofree gchar *who = proof_body(sam);
		g_autofree gchar *one = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", sam, orchard);
		g_autofree gchar *again = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"lantern orchard\",\"kind\":\"movie\"}",
			sam);
		g_autofree gchar *browse = g_strdup_printf("{\"proof\":\"%s\",\"list\":\"popular\",\"kind\":\"movie\"}", sam);
		g_autofree gchar *a = expect(fx, "requests", who, 200);
		g_autofree gchar *b = expect(fx, "title", one, 200);
		g_autofree gchar *c = expect(fx, "search", again, 200);
		g_autofree gchar *d = expect(fx, "browse", browse, 200);
		g_autofree gchar *e = expect(fx, "whoami", who, 200);
		g_autofree gchar *f = expect(fx, "hello", "{}", 200);
		static const gchar *const never[] = {
			FOLDER_MARK, PROFILE_MARK, "Made Up", "/tmp", "bluray", "fixture", "root_id", "profile", "version",
			"admin", "requester", "password", "epoch", NULL
		};
		g_autoptr(JsonObject) mine = object_of(a);
		g_autoptr(JsonObject) me = object_of(e);
		g_autofree gchar *of_me = members_of(me);
		JsonObject *first = json_array_get_object_element(json_object_get_array_member(mine, "requests"), 0);

		everything = g_strconcat(a, b, c, d, e, f, NULL);
		for (i = 0; never[i] != NULL; i++)
			assert_lacks(everything, never[i]);
		assert_lacks(everything, fx->r->disk);
		g_assert_cmpstr(json_object_get_string_member(first, "state"), ==, "available");
		g_assert_cmpstr(json_object_get_string_member(first, "mine"), ==, "made");
		assert_holds(b, "\"state\":\"available\"");
		assert_holds(c, "\"state\":\"available\"");
		g_assert_cmpstr(of_me, ==, "movies,name,seasons");
		{
			g_autofree gchar *allowance = members_of(json_object_get_object_member(me, "movies"));

			g_assert_cmpstr(allowance, ==, "left,limit,window_ends");
			g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(me, "movies"), "limit"), ==, 5);
			g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(me, "movies"), "left"), ==, 4);
		}
	}

	/* Who asked follows the setting: on, a title says who; off, no answer
	 * of any operation carries asked_by, or a name but the caller's own. */
	{
		g_autofree gchar *one = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", sam, harbour);
		g_autofree gchar *mine = proof_body(sam);
		g_autofree gchar *look = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"harbour\",\"kind\":\"series\"}", sam);
		g_autofree gchar *on = expect(fx, "title", one, 200);
		g_autofree gchar *off = NULL;
		g_autofree gchar *off_search = NULL;
		g_autofree gchar *off_requests = NULL;
		g_autofree gchar *off_hello = NULL;
		g_autofree gchar *off_ask = NULL;

		assert_holds(on, "\"asked_by\":\"robin\"");
		/* An ordinary user's request, and an administrator's: the title says
		 * that it is requested, and never by whom.  Those names log in to
		 * the engine's interface. */
		{
			gint64 kiln = N("SELECT id FROM meta_title WHERE provider_id = '" AXIS_TEST_REQUESTS_KILN "'");
			const gint64 makers[] = { fx->uma, fx->r->admin };
			const gint64 titles[] = { ledger, kiln };
			guint m;

			for (m = 0; m < G_N_ELEMENTS(makers); m++) {
				g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", sam,
					titles[m]);
				g_autofree gchar *shown = NULL;
				AxisRequestMade made;
				AxisRequestAsk ask;

				memset(&ask, 0, sizeof(ask));
				ask.user_id = makers[m];
				ask.title_id = titles[m];
				ask.kind = AXIS_REQUEST_MOVIE;
				g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
				g_assert_no_error(error);
				g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
				axis_request_made_clear(&made);
				shown = expect(fx, "title", body, 200);
				assert_lacks(shown, "asked_by");
				assert_lacks(shown, "uma");
				assert_lacks(shown, "root");
				g_assert_true(strstr(shown, "\"state\":\"requested\"") != NULL ||
					strstr(shown, "\"state\":\"approved\"") != NULL);
			}
		}
		g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_SHOW_WHO, "false", &error));
		g_assert_no_error(error);
		off = expect(fx, "title", one, 200);
		off_search = expect(fx, "search", look, 200);
		off_requests = expect(fx, "requests", mine, 200);
		off_hello = expect(fx, "hello", "{}", 200);
		/* sam follows robin's request for the first season. */
		{
			g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
				"\"seasons\":\"first\"}", sam, harbour);

			off_ask = expect(fx, "request", body, 200);
			assert_holds(off_ask, "\"outcome\":\"followed\"");
			g_free(off_requests);
			off_requests = expect(fx, "requests", mine, 200);
			assert_holds(off_requests, "\"mine\":\"followed\"");
		}
		g_free(everything);
		everything = g_strconcat(off, off_search, off_requests, off_hello, off_ask, NULL);
		assert_lacks(everything, "asked_by");
		assert_lacks(everything, "robin");
		assert_holds(off_hello, "\"show_names\":false");
		assert_holds(off, "\"state\":\"requested\"");
	}
	(void)ledger;
}

/* Scenario: The provider is unreachable. */
static void
test_search_stale(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *first = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"paper kiln\",\"kind\":\"movie\"}", robin);
	g_autofree gchar *other = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"never asked before\",\"kind\":\"movie\"}",
		robin);
	g_autofree gchar *fresh = expect(fx, "search", first, 200);
	g_autofree gchar *saved = NULL;
	g_autofree gchar *none = NULL;
	g_autoptr(GBytes) empty = g_bytes_new_static("", 0);

	assert_holds(fresh, "\"stale\":false");
	/* The provider stops answering; what was kept in memory has ended, and
	 * the stored answer is old. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_plex_route_full(fx->r->fa->api, "/search/film", 503, "text/plain", empty, NULL);
	axis_portal_api_set_time_offset(fx->api, (gint64)(AXIS_PORTAL_SEARCH_CACHE_S + 60) * G_USEC_PER_SEC);
	N("UPDATE meta_search SET fetched_at = now() - interval '40 days' RETURNING 1");
	saved = expect(fx, "search", first, 200);
	assert_holds(saved, "\"stale\":true");
	assert_holds(saved, "Paper Kiln");
	none = expect(fx, "search", other, 200);
	assert_holds(none, "\"stale\":true");
	assert_holds(none, "\"titles\":[]");
}

/* ── Limits ─────────────────────────────────────────────────────── */

/* Scenarios: Guessing one password; Reported addresses clear nothing; The
 * owner still gets in; An altered device value. */
static void
test_login_throttle(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *device = NULL;
	g_autofree gchar *sams_device = NULL;
	g_autofree gchar *proof = login_with(fx, "robin", ROBIN_PASSWORD, NULL, &device);
	g_autofree gchar *sam = login_with(fx, "sam", SAM_PASSWORD, NULL, &sams_device);
	g_autofree gchar *right = login_body("robin", ROBIN_PASSWORD, "203.0.113.5");
	g_autofree gchar *elsewhere = login_body("robin", ROBIN_PASSWORD, "203.0.113.200");
	g_autofree gchar *refused = NULL;
	guint hashes;
	guint i;

	g_assert_nonnull(device);
	g_assert_nonnull(sam);
	/* A stranger fails for robin until the name is delayed. */
	for (i = 0; i < 6; i++) {
		g_autofree gchar *wrong = login_body("robin", "a-wrong-password-of-a-stranger", "203.0.113.5");
		AxisTestReply reply = call(fx, "login", wrong);

		if (reply.status == 429) {
			axis_test_reply_clear(&reply);
			break;
		}
		assert_status(&reply, 401);
		axis_test_reply_clear(&reply);
	}
	/* Refused with a time to wait, even with the right password, before any
	 * password work. */
	hashes = axis_password_verify_count();
	{
		AxisTestReply reply = call(fx, "login", right);

		assert_status(&reply, 429);
		assert_api_reply(&reply);
		g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
		refused = g_strdup(reply.body);
		assert_holds(refused, "\"wait\":");
		axis_test_reply_clear(&reply);
	}
	/* Another reported address clears nothing. */
	{
		g_autofree gchar *again = expect(fx, "login", elsewhere, 429);

		assert_holds(again, "\"error\":\"throttled\"");
	}
	/* An altered device value, and another account's, change nothing. */
	{
		g_autofree gchar *altered = g_strdup(device);
		g_autofree gchar *a = NULL;
		g_autofree gchar *b = NULL;
		g_autofree gchar *c = NULL;

		altered[strlen(altered) - 1] = altered[strlen(altered) - 1] == 'A' ? 'B' : 'A';
		a = g_strdup_printf("{\"username\":\"robin\",\"password\":\"%s\",\"client_address\":\"203.0.113.5\","
			"\"user_agent\":\"t\",\"device\":\"%s\"}", ROBIN_PASSWORD, altered);
		b = g_strdup_printf("{\"username\":\"robin\",\"password\":\"%s\",\"client_address\":\"203.0.113.5\","
			"\"user_agent\":\"t\",\"device\":\"%s\"}", ROBIN_PASSWORD, sams_device);
		c = g_strdup_printf("{\"username\":\"robin\",\"password\":\"%s\",\"client_address\":\"203.0.113.5\","
			"\"user_agent\":\"t\",\"device\":\"1.0.1.AAAAAAAAAAAAAAAAAAAAAA.AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}",
			ROBIN_PASSWORD);
		g_free(expect(fx, "login", a, 429));
		g_free(expect(fx, "login", b, 429));
		g_free(expect(fx, "login", c, 429));
	}
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	/* The owner, from the browser that logged in before, gets in at once. */
	{
		g_autofree gchar *next = NULL;
		g_autofree gchar *again = login_with(fx, "robin", ROBIN_PASSWORD, device, &next);
		g_autofree gchar *body = proof_body(again);

		g_free(expect(fx, "whoami", body, 200));
		g_assert_nonnull(next);
		g_assert_cmpstr(next, !=, device);
	}
	/* And the stranger still waits. */
	g_free(expect(fx, "login", right, 429));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*portal login refused, too many failures: user=robin*"),
		>=, 2);
}

/* Scenario: Guessing across names. */
static void
test_login_ceiling(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *right = login_body("robin", ROBIN_PASSWORD, "198.51.100.77");
	g_autofree gchar *sams = login_body("sam", SAM_PASSWORD, "198.51.100.78");
	guint hashes;
	guint i;

	/* Thirty failures, each for another name and from another address. */
	for (i = 0; i < AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR; i++) {
		g_autofree gchar *name = g_strdup_printf("stranger%u", i);
		g_autofree gchar *address = g_strdup_printf("203.0.113.%u", i + 1);
		g_autofree gchar *body = login_body(name, "some-password-or-other", address);

		g_free(expect(fx, "login", body, 401));
	}
	/* The next one is admitted (one a minute), and then the door is shut,
	 * even for the right password of another name from another address. */
	g_free(expect(fx, "login", right, 200));
	hashes = axis_password_verify_count();
	{
		g_autofree gchar *refused = expect(fx, "login", sams, 429);

		assert_holds(refused, "\"wait\":");
	}
	g_free(expect(fx, "login", right, 429));
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	/* A minute later one more is admitted, and only one. */
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(61) * G_USEC_PER_SEC);
	g_free(expect(fx, "login", sams, 200));
	g_free(expect(fx, "login", right, 429));
	/* When the hour has passed, logins are admitted as before. */
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(3700) * G_USEC_PER_SEC);
	g_free(expect(fx, "login", right, 200));
	g_free(expect(fx, "login", sams, 200));
}

/* Scenarios: The portal's queue is bounded; An administrator logs in during
 * a flood. */
typedef struct
{
	guint ok;
	guint refused;
	guint busy;
} Flood;

static void
on_flood_login(
	const AxisPortalLoginResult *result,
	gpointer                     user_data
){
	Flood *flood = user_data;

	if (result->status == AXIS_PORTAL_LOGIN_OK)
		flood->ok++;
	else if (result->status == AXIS_PORTAL_LOGIN_BUSY)
		flood->busy++;
	else
		flood->refused++;
}

static void
test_hashing(Fixture *fx, gconstpointer data)
{
	guint hashes = axis_password_verify_count();
	guint at_once_busy = 0;
	guint queued = 0;
	gboolean hashing = FALSE;
	AxisTestReply reply;
	Flood flood;
	guint i;

	memset(&flood, 0, sizeof(flood));
	/* Twelve logins arrive before any is checked: one takes the portal's
	 * hashing place, eight wait, and the rest are told at once that the
	 * daemon is busy, with no hash begun for them. */
	for (i = 0; i < 12; i++) {
		g_autofree gchar *name = g_strdup_printf("flood%u", i);
		g_autofree gchar *address = g_strdup_printf("203.0.113.%u", i + 1);
		AxisPortalLoginResult at_once;

		if (axis_web_auth_portal_login(fx->auth, name, "a-password-of-the-flood", NULL, address, "t", &at_once,
		                               on_flood_login, &flood)) {
			queued++;
		} else {
			g_assert_cmpint(at_once.status, ==, AXIS_PORTAL_LOGIN_BUSY);
			at_once_busy++;
		}
	}
	g_assert_cmpuint(queued, ==, 1 + AXIS_PORTAL_LOGIN_MAX_WAITING);
	g_assert_cmpuint(at_once_busy, ==, 3);
	g_assert_cmpuint(axis_web_auth_portal_waiting(fx->auth, &hashing), ==, AXIS_PORTAL_LOGIN_MAX_WAITING);
	g_assert_true(hashing);
	/* Through the API the same answer: 503. */
	{
		g_autofree gchar *body = login_body("robin", ROBIN_PASSWORD, "198.51.100.20");

		reply = call(fx, "login", body);
		/* (The first of the flood may have finished while this one was on
		 * its way: then it waits its turn, and is answered.) */
		if (reply.status == 503) {
			assert_api_reply(&reply);
			g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
		} else {
			assert_status(&reply, 200);
		}
		axis_test_reply_clear(&reply);
	}
	/* An administrator logs in at the engine's own interface meanwhile: the
	 * portal's logins hold one of the two hashing places, never both. */
	reply = engine_login(fx, "root", ADMIN_PASSWORD);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
	/* The flood drains: nine checked, each one hash; three never hashed. */
	while (flood.ok + flood.refused + flood.busy < queued)
		g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(flood.refused, ==, queued);
	g_assert_cmpuint(flood.busy, ==, 0);
	g_assert_cmpuint(axis_web_auth_portal_waiting(fx->auth, &hashing), ==, 0);
	/* (Nine of the flood, the administrator's, and the one through the API
	 * when it was let in.) */
	g_assert_cmpuint(axis_password_verify_count() - hashes, >=, queued + 1);
	g_assert_cmpuint(axis_password_verify_count() - hashes, <=, queued + 2);
}

/* Scenario: The search budget. */
static void
test_search_budget(Fixture *fx, gconstpointer data)
{
	const gchar *const people[][2] = {
		{ "robin", ROBIN_PASSWORD }, { "sam", SAM_PASSWORD }, { "tess", OTHER_PASSWORD }, { "vic", OTHER_PASSWORD }
	};
	gchar *proofs[4];
	guint asked = 0;
	guint done = 0;
	guint p;
	guint i;

	for (p = 0; p < 4; p++)
		proofs[p] = login(fx, people[p][0], people[p][1]);

	/* Each person: ten a minute, and the eleventh is told to wait. */
	for (i = 0; i < AXIS_PORTAL_SEARCHES_A_MINUTE; i++) {
		g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"made-up search %u\",\"kind\":\"movie\"}",
			proofs[0], done++);

		g_free(expect(fx, "search", body, 200));
	}
	{
		g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"one more\",\"kind\":\"movie\"}",
			proofs[0]);
		AxisTestReply reply = call(fx, "search", body);

		assert_status(&reply, 429);
		assert_api_reply(&reply);
		g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
		assert_holds(reply.body, "\"wait\":");
		axis_test_reply_clear(&reply);
	}
	/* In all: thirty that ask the provider.  The thirty-first different
	 * search, by a person with searches of their own left, is refused. */
	for (p = 1; p < 3; p++) {
		for (i = 0; i < AXIS_PORTAL_SEARCHES_A_MINUTE; i++) {
			g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"made-up search %u\","
				"\"kind\":\"movie\"}", proofs[p], done++);

			g_free(expect(fx, "search", body, 200));
		}
	}
	g_assert_cmpuint(done, ==, AXIS_PORTAL_SEARCHES_IN_ALL);
	asked = axis_test_plex_count(fx->r->fa->api, "/search/film");
	g_assert_cmpuint(asked, ==, AXIS_PORTAL_SEARCHES_IN_ALL);
	{
		g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"the thirty-first\",\"kind\":\"movie\"}",
			proofs[3]);
		g_autofree gchar *known = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"made-up search 3\",\"kind\":\"movie\"}",
			proofs[3]);

		g_free(expect(fx, "search", body, 429));
		/* What the memory holds is still answered: it asks nobody. */
		g_free(expect(fx, "search", known, 200));
	}
	g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, AXIS_PORTAL_SEARCHES_IN_ALL);
	/* A minute later there is room again. */
	axis_portal_api_set_time_offset(fx->api, G_GINT64_CONSTANT(61) * G_USEC_PER_SEC);
	{
		g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"the thirty-first\",\"kind\":\"movie\"}",
			proofs[3]);

		g_free(expect(fx, "search", body, 200));
	}
	for (p = 0; p < 4; p++)
		g_free(proofs[p]);
}

/* Scenarios: The portal's daily ceiling; Quota does not depend on the
 * client. */
static void
test_request_limits(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *sam = login(fx, "sam", SAM_PASSWORD);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	guint i;

	/* A person at their limit is refused again and again, whatever the
	 * caller does, and no request exists. */
	N("UPDATE app_user SET request_movies_limit = 1 WHERE username = 'robin' RETURNING id");
	g_free(ask_movie(fx, robin, orchard, 200));
	for (i = 0; i < 5; i++) {
		AxisTestReply reply;
		g_autofree gchar *body = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"movie\"}",
			robin, ledger);

		reply = call(fx, "request", body);
		assert_status(&reply, 429);
		assert_api_reply(&reply);
		assert_holds(reply.body, "\"error\":\"quota\"");
		assert_holds(reply.body, "\"wait\":");
		g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 0);

	/* The portal's day: with two counted, another person with quota left is
	 * refused, and no request exists. */
	g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_PORTAL_DAY, "2", &error));
	g_assert_no_error(error);
	g_free(ask_movie(fx, sam, kiln, 200));
	{
		g_autofree gchar *refused = ask_movie(fx, sam, bells, 429);
		g_autofree gchar *who = proof_body(sam);
		g_autofree gchar *me = expect(fx, "whoami", who, 200);

		assert_holds(refused, "\"error\":\"portal-limit\"");
		assert_holds(refused, "the portal's limit for today is reached");
		assert_holds(refused, "\"wait\":");
		assert_holds(me, "\"left\":4");
	}
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE title_id = %" G_GINT64_FORMAT, bells), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'portal'"), ==, 2);
}

/* The public id of the request an answer of `request` made. */
static gchar *
made_id(const gchar *answer)
{
	g_autoptr(JsonObject) object = object_of(answer);

	g_assert_cmpstr(json_object_get_string_member(object, "outcome"), ==, "made");
	return g_strdup(json_object_get_string_member(json_object_get_object_member(object, "request"), "id"));
}

/*
 * Scenario: one person's share of the portal's day (review of P2, S4).  A
 * requester who asks and withdraws again and again uses up their own share,
 * a third of requests.portal_per_day, withdrawn requests included, and then
 * is refused with a refusal of its own; the others can still ask.
 */
static void
test_portal_share(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *sam = login(fx, "sam", SAM_PASSWORD);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 share = AXIS_REQUESTS_PORTAL_SHARE_OF(AXIS_REQUESTS_PORTAL_A_DAY);
	g_autofree gchar *refused = NULL;
	g_autofree gchar *me = NULL;
	gint64 i;

	g_assert_cmpint(share, ==, 10);
	for (i = 0; i < share; i++) {
		g_autofree gchar *made = ask_movie(fx, robin, orchard, 200);
		g_autofree gchar *id = made_id(made);
		g_autofree gchar *withdraw = g_strdup_printf("{\"proof\":\"%s\",\"request\":\"%s\"}", robin, id);

		g_free(expect(fx, "withdraw", withdraw, 200));
	}
	/* robin's quota has every one of them back, and holds no request. */
	{
		g_autofree gchar *who = proof_body(robin);

		me = expect(fx, "whoami", who, 200);
		assert_holds(me, "\"left\":5");
	}
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE decision IN ('waiting', 'approved')"), ==, 0);
	refused = ask_movie(fx, robin, orchard, 429);
	assert_holds(refused, "\"error\":\"portal-share\"");
	assert_holds(refused, "\"wait\":");
	assert_holds(refused, "one person's share of the portal's day is 10");
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'portal'"), ==, share);
	/* The portal's day is not used up: sam asks for both. */
	{
		g_autofree gchar *a = ask_movie(fx, sam, orchard, 200);
		g_autofree gchar *b = ask_movie(fx, sam, ledger, 200);

		assert_holds(a, "\"outcome\":\"made\"");
		assert_holds(b, "\"outcome\":\"made\"");
	}
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE via = 'portal' AND requested_by = %" G_GINT64_FORMAT,
		fx->r->sam), ==, 2);
	/* And robin follows what sam asked for: that makes no row, and is not
	 * refused. */
	{
		g_autofree gchar *follow = ask_movie(fx, robin, ledger, 200);

		assert_holds(follow, "\"outcome\":\"followed\"");
	}
}

/*
 * Scenario: a title whose adult mark is not known (review of P2, S2): stored
 * before migration 0084, or from no provider's answer.  For a requester it is
 * a number never given, for every operation and whatever the kind it is asked
 * for as; a search answered from memory leaves it out; and once a provider's
 * answer about it says it is not adult, it is shown and can be asked for.
 */
static void
test_mark_not_known(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	g_autofree gchar *search = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"lantern orchard\",\"kind\":\"movie\"}",
		robin);
	g_autofree gchar *again = g_strdup_printf("{\"proof\":\"%s\",\"query\":\"the lantern orchard\",\"kind\":\"movie\"}",
		robin);
	g_autofree gchar *never = g_strdup_printf("{\"proof\":\"%s\",\"id\":987654}", robin);
	g_autofree gchar *found = expect(fx, "search", search, 200);
	gint64 orchard = N("SELECT id FROM meta_title WHERE provider_id = '" AXIS_TEST_REQUESTS_ORCHARD "'");
	g_autofree gchar *one = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, orchard);
	g_autofree gchar *as_series = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT ",\"kind\":\"series\","
		"\"seasons\":\"all\"}", robin, orchard);
	g_autofree gchar *not_found = expect(fx, "title", never, 404);
	g_autofree gchar *no_art = expect(fx, "art", never, 404);
	g_autofree gchar *never_asked = ask_movie(fx, robin, 987654, 404);
	g_autofree gchar *hash = NULL;
	gint64 asked;

	assert_holds(found, "The Lantern Orchard");
	g_assert_cmpint(N("SELECT count(*) FROM meta_title WHERE id = %" G_GINT64_FORMAT " AND adult = false", orchard), ==,
		1);
	/* Its poster is held, so that art would answer it. */
	N("UPDATE meta_title SET poster_ref = '/p101.png' WHERE id = %" G_GINT64_FORMAT " RETURNING id", orchard);
	g_assert_true(axis_meta_artwork_fetch(fx->r->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &hash,
		&error));
	g_assert_no_error(error);
	g_free(expect(fx, "art", one, 200));

	/* As a title stored before the migration has it. */
	N("UPDATE meta_title SET adult = NULL WHERE id = %" G_GINT64_FORMAT " RETURNING id", orchard);
	{
		g_autofree gchar *a = expect(fx, "title", one, 404);
		g_autofree gchar *b = expect(fx, "art", one, 404);
		g_autofree gchar *c = ask_movie(fx, robin, orchard, 404);
		g_autofree gchar *d = expect(fx, "request", as_series, 404);
		g_autofree gchar *from_memory = NULL;

		g_assert_cmpstr(a, ==, not_found);
		g_assert_cmpstr(b, ==, no_art);
		g_assert_cmpstr(c, ==, never_asked);
		g_assert_cmpstr(d, ==, never_asked);
		/* The same search, from memory: the provider is not asked, and the
		 * title is not among the answers. */
		asked = axis_test_plex_count(fx->r->fa->api, "/search/film");
		from_memory = expect(fx, "search", search, 200);
		g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, asked);
		assert_lacks(from_memory, "The Lantern Orchard");
		assert_holds(from_memory, "Saltmarsh Ledger");
	}
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	/* The engine's own ask path refuses an ordinary user the same way, and
	 * lets an administrator ask. */
	{
		AxisRequestMade made;
		AxisRequestAsk ask;

		memset(&ask, 0, sizeof(ask));
		ask.user_id = fx->uma;
		ask.title_id = orchard;
		ask.kind = AXIS_REQUEST_MOVIE;
		g_assert_false(axis_requests_ask(fx->r->requests, &ask, &made, &error));
		g_assert_error(error, AXIS_REQUESTS_ERROR, AXIS_REQUESTS_ERROR_NOT_FOUND);
		g_clear_error(&error);
		axis_request_made_clear(&made);
	}

	/* Another search asks the provider, whose answer says the title is not
	 * adult: from now on it is shown, and can be asked for. */
	{
		g_autofree gchar *answered = expect(fx, "search", again, 200);
		g_autofree gchar *shown = NULL;
		g_autofree gchar *made = NULL;

		g_assert_cmpuint(axis_test_plex_count(fx->r->fa->api, "/search/film"), ==, asked + 1);
		assert_holds(answered, "The Lantern Orchard");
		g_assert_cmpint(N("SELECT count(*) FROM meta_title WHERE id = %" G_GINT64_FORMAT " AND adult = false", orchard),
			==, 1);
		shown = expect(fx, "title", one, 200);
		assert_holds(shown, "The Lantern Orchard");
		g_free(expect(fx, "art", one, 200));
		made = ask_movie(fx, robin, orchard, 200);
		assert_holds(made, "\"outcome\":\"made\"");
	}
}

/*
 * Scenario: a kind that cannot be asked for (review of P2, M6).  While the TV
 * module is off, a series is a number never given for title and art, as it
 * is not offered by search and browse.
 */
static void
test_kind_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *robin = login(fx, "robin", ROBIN_PASSWORD);
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *of_series = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, harbour);
	g_autofree gchar *of_film = g_strdup_printf("{\"proof\":\"%s\",\"id\":%" G_GINT64_FORMAT "}", robin, orchard);
	g_autofree gchar *never = g_strdup_printf("{\"proof\":\"%s\",\"id\":987654}", robin);
	g_autofree gchar *not_found = expect(fx, "title", never, 404);
	g_autofree gchar *no_art = expect(fx, "art", never, 404);

	g_free(expect(fx, "title", of_series, 200));
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_TV_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	g_assert_false(axis_requests_kind_on(fx->r->requests, AXIS_REQUEST_SERIES));
	{
		g_autofree gchar *a = expect(fx, "title", of_series, 404);
		g_autofree gchar *b = expect(fx, "art", of_series, 404);
		g_autofree gchar *hello = expect(fx, "hello", "{}", 200);

		g_assert_cmpstr(a, ==, not_found);
		g_assert_cmpstr(b, ==, no_art);
		assert_lacks(hello, "\"series\"");
	}
	/* A movie is answered as before. */
	g_free(expect(fx, "title", of_film, 200));
}

/*
 * Scenario: every operation's budgets (review of P2, S3).  A person has
 * AXIS_PORTAL_PERSON_OPS_A_MINUTE operations a minute, whatever they ask, and
 * a token AXIS_PORTAL_TOKEN_OPS_A_MINUTE; beyond either the answer is 429 with
 * the time to wait, the operation is not done, and the others are not held to
 * it.  The clock is stopped, so that the counts are exact.
 */
#define MINUTE_US (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

static void
test_operation_budgets(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *robin = NULL;
	g_autofree gchar *sam = NULL;
	g_autofree gchar *robin_body = NULL;
	g_autofree gchar *sam_body = NULL;
	g_autofree gchar *second = NULL;
	gint64 second_id = 0;
	gint64 now = axis_portal_api_now(fx->api);
	/* What the token's budget gets back while robin's gets one. */
	gint64 refilled = (MINUTE_US / AXIS_PORTAL_PERSON_OPS_A_MINUTE) / (MINUTE_US / AXIS_PORTAL_TOKEN_OPS_A_MINUTE);
	AxisTestReply reply;
	gint64 used = 0;
	gint64 i;

	axis_portal_api_freeze_clock(fx->api, now);
	/* (The two logins are the token's first two operations.) */
	robin = login(fx, "robin", ROBIN_PASSWORD);
	sam = login(fx, "sam", SAM_PASSWORD);
	robin_body = proof_body(robin);
	sam_body = proof_body(sam);
	used = 2;
	/* A person's: every operation with their proof takes one, a body that
	 * is not the operation's too. */
	for (i = 0; i < AXIS_PORTAL_PERSON_OPS_A_MINUTE; i++) {
		if (i % 2 == 0) {
			g_free(expect(fx, "whoami", robin_body, 200));
		} else {
			g_autofree gchar *wrong = g_strdup_printf("{\"proof\":\"%s\",\"user\":\"sam\"}", robin);

			g_free(expect(fx, "requests", wrong, 422));
		}
		used++;
	}
	reply = call(fx, "whoami", robin_body);
	assert_status(&reply, 429);
	assert_api_reply(&reply);
	assert_holds(reply.body, "\"error\":\"person-limit\"");
	assert_holds(reply.body, "\"wait\":");
	g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
	axis_test_reply_clear(&reply);
	used++;
	{
		g_autofree gchar *list = expect(fx, "requests", robin_body, 429);

		assert_holds(list, "person-limit");
		used++;
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*robin used the budget of*"), ==, 1);
	/* sam is not held to robin's, and robin's comes back a little at a
	 * time: one each minute / the budget. */
	g_free(expect(fx, "whoami", sam_body, 200));
	used++;
	axis_portal_api_freeze_clock(fx->api, now + MINUTE_US / AXIS_PORTAL_PERSON_OPS_A_MINUTE);
	g_free(expect(fx, "whoami", robin_body, 200));
	g_free(expect(fx, "whoami", robin_body, 429));
	used += 2;

	/* A token's: what is left of its minute (and of what came back in the
	 * moment above), then 429 for anything it asks, hello and login too;
	 * another token is not held to it. */
	g_assert_cmpint(refilled, ==, 5);
	for (i = used; i < AXIS_PORTAL_TOKEN_OPS_A_MINUTE + refilled; i++)
		g_free(expect(fx, "hello", "{}", 200));
	{
		g_autofree gchar *body = login_body("sam", SAM_PASSWORD, "198.51.100.20");
		g_autofree gchar *a = expect(fx, "hello", "{}", 429);
		g_autofree gchar *b = expect(fx, "login", body, 429);
		g_autofree gchar *c = expect(fx, "whoami", sam_body, 429);

		assert_holds(a, "\"error\":\"token-limit\"");
		assert_holds(b, "\"error\":\"token-limit\"");
		assert_holds(c, "\"error\":\"token-limit\"");
	}
	g_assert_true(axis_portal_tokens_create(fx->tokens, fx->r->t->pool, "root", &second, &second_id, &error));
	g_assert_no_error(error);
	reply = send_to(fx, fx->portal_port, "POST", API "whoami", second, NULL, sam_body, NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	axis_secret_free(g_steal_pointer(&second));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*portal token used its budget*"), ==, 1);

	/* Two minutes later both are whole again. */
	axis_portal_api_freeze_clock(fx->api, now + 2 * MINUTE_US);
	g_free(expect(fx, "hello", "{}", 200));
	g_free(expect(fx, "whoami", robin_body, 200));
	axis_portal_api_freeze_clock(fx->api, 0);
}

/* The engine's own password work, held until the test lets it go. */
typedef struct
{
	GMutex   lock;
	GCond    cond;
	gboolean open;
	guint    after;
} Blocker;

static void
blocked_work(
	AxisWebAuth *auth,
	gpointer     data
){
	Blocker *blocker = data;

	g_mutex_lock(&blocker->lock);
	while (!blocker->open)
		g_cond_wait(&blocker->cond, &blocker->lock);
	g_mutex_unlock(&blocker->lock);
}

static void
blocked_after(
	AxisWebAuth *auth,
	gpointer     data,
	gboolean     skipped
){
	Blocker *blocker = data;

	blocker->after++;
}

/*
 * A login through the portal while the engine's own queue of password work is
 * full (review of P2, "could not be judged"): the login is answered busy
 * before the handler returns its deferred response, and htmx-glib writes that
 * answer then.  The request is answered at once, 503 with a time to wait, and
 * does not hang; no hash is begun, and nothing is left waiting.
 */
static void
test_engine_queue_full(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *body = login_body("robin", ROBIN_PASSWORD, "198.51.100.20");
	gboolean hashing = TRUE;
	AxisTestReply reply;
	Blocker blocker;
	guint queued = 0;
	guint hashes;
	gint64 began;

	memset(&blocker, 0, sizeof(blocker));
	g_mutex_init(&blocker.lock);
	g_cond_init(&blocker.cond);
	axis_web_auth_wait_writes(fx->auth);
	while (g_main_context_iteration(NULL, FALSE))
		;
	while (axis_web_auth_run(fx->auth, blocked_work, blocked_after, &blocker))
		queued++;
	/* Nothing else was waiting: the whole queue is the test's. */
	g_assert_cmpuint(queued, ==, AXIS_WEB_AUTH_MAX_PENDING);

	hashes = axis_password_verify_count();
	began = g_get_monotonic_time();
	reply = call(fx, "login", body);
	assert_status(&reply, 503);
	assert_api_reply(&reply);
	assert_holds(reply.body, "\"error\":\"busy\"");
	g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
	axis_test_reply_clear(&reply);
	g_assert_cmpint(g_get_monotonic_time() - began, <, 10 * G_USEC_PER_SEC);
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	g_assert_cmpuint(axis_web_auth_portal_waiting(fx->auth, &hashing), ==, 0);
	g_assert_false(hashing);
	/* The other operations are answered meanwhile. */
	g_free(expect(fx, "hello", "{}", 200));

	/* The engine's work goes on; then a login is answered as before. */
	g_mutex_lock(&blocker.lock);
	blocker.open = TRUE;
	g_cond_broadcast(&blocker.cond);
	g_mutex_unlock(&blocker.lock);
	while (blocker.after < queued)
		g_main_context_iteration(NULL, TRUE);
	g_free(login(fx, "robin", ROBIN_PASSWORD));
	g_mutex_clear(&blocker.lock);
	g_cond_clear(&blocker.cond);
}

/*
 * Scenarios (web-auth, slice P4, after its review): A login from one of the
 * portal's addresses; Another address; Nothing is learnt and nothing
 * expires.  The portal's address is 127.0.0.1 because the configuration
 * says so, not because anything called from it: with no request made to the
 * portal API at all, the administrator's right password is refused at the
 * engine's login from that address as a wrong one is, without a hash and
 * without a session, and still two days later; the same administrator logs
 * in from another address, directly and with no proxy between, and a session
 * that existed goes on.  The log says why once, then once a minute at most,
 * and the page "Request portal" shows when it last happened.
 */
static void
test_portal_peers_login(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *cookies = NULL;
	AxisTestReply reply;
	guint hashes;
	guint i;

	/* A person's desk is not the portal's address: accepted, with the plain
	 * cookie of a direct login over http. */
	reply = engine_login(fx, "root", ADMIN_PASSWORD);
	assert_status(&reply, 303);
	before = cookies_after(&reply);
	axis_test_reply_clear(&reply);
	g_assert_nonnull(strstr(before, "axis_session="));
	g_assert_null(strstr(before, "__Host-"));
	g_assert_cmpuint(axis_web_auth_portal_logins_barred(fx->auth, NULL), ==, 0);
	reply = axis_test_web_request(fx->people, fx->port, "GET", "/admin/portal", before, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "AXIS_PORTAL_PEERS in the environment): 127.0.0.1. The portal API answers these addresses "
		"and no other.");
	assert_holds(reply.body, "No login to Axis was refused for coming from one of these addresses since Axis started.");
	assert_lacks(reply.body, "The portal&#39;s addresses are not set");
	assert_lacks(reply.body, "share an address");
	axis_test_reply_clear(&reply);

	/* The administrator's right password, from the portal's address: the
	 * answer of a wrong one, no hash, no session.  The portal has asked the
	 * daemon nothing yet.  More often than a username's free failures: no
	 * count is touched by it. */
	hashes = axis_password_verify_count();
	for (i = 0; i < AXIS_AUTH_USER_FREE_FAILURES + 3; i++) {
		reply = engine_login_from(fx, fx->session, "root", ADMIN_PASSWORD, NULL);
		assert_status(&reply, 401);
		assert_holds(reply.body, "The username or password is incorrect.");
		cookies = cookies_after(&reply);
		g_assert_null(strstr(cookies, "axis_session="));
		g_clear_pointer(&cookies, g_free);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	g_assert_cmpuint(axis_web_auth_portal_logins_barred(fx->auth, NULL), ==, AXIS_AUTH_USER_FREE_FAILURES + 3);
	/* One line that names the rule and the setting, not one for each. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: login refused: the connection came from "
		"127.0.0.1, one of the request portal's addresses (server.portal_peers, AXIS_PORTAL_PEERS)*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: login refused*more such refusals*"), ==, 0);
	/* A wrong password from somewhere else reads the same. */
	reply = engine_login(fx, "uma", "not-umas-password-at-all");
	assert_status(&reply, 401);
	assert_holds(reply.body, "The username or password is incorrect.");
	axis_test_reply_clear(&reply);

	/* The session that existed is not affected, wherever it is used from. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/users", before, NULL, NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	/* The page says when, so that an administrator who did it to themselves
	 * finds the reason. */
	reply = axis_test_web_request(fx->people, fx->port, "GET", "/admin/portal", before, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "A login to Axis from one of the portal&#39;s addresses was last refused ");
	assert_holds(reply.body, "(8 since Axis started)");
	axis_test_reply_clear(&reply);

	/* Another address: the same administrator logs in, at once. */
	reply = engine_login(fx, "root", ADMIN_PASSWORD);
	assert_status(&reply, 303);
	cookies = cookies_after(&reply);
	g_assert_nonnull(strstr(cookies, "axis_session="));
	g_clear_pointer(&cookies, g_free);
	axis_test_reply_clear(&reply);

	/* Nothing expires: two days on, with the portal silent throughout, the
	 * address is the portal's still.  The log's next line counts the ones it
	 * left out. */
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(48) * 3600 * G_USEC_PER_SEC);
	reply = engine_login_from(fx, fx->session, "root", ADMIN_PASSWORD, NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes + 2);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: login refused: the connection came from "
		"127.0.0.1*(and 7 more such refusals since this was last said)"), ==, 1);
	/* And a portal that calls changes nothing either way. */
	g_free(expect(fx, "hello", "{}", 200));
	reply = engine_login(fx, "root", ADMIN_PASSWORD);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
}

/*
 * What the list holds is what was written, no wider: an address, a range to
 * its last address and not one further, an IPv4-mapped IPv6 address as its
 * IPv4 address, an IPv6 address alone and not its /64.  A host name is
 * refused and leaves the list as it was.
 */
static void
test_portal_peers_forms(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *address;
		gboolean     listed;
	} cases[] = {
		{ "127.0.0.1", TRUE }, { "::ffff:127.0.0.1", TRUE }, { "[::ffff:127.0.0.1]", TRUE }, { "127.0.0.2", FALSE },
		{ "::1", FALSE }, { "192.0.2.15", FALSE }, { "192.0.2.16", TRUE }, { "192.0.2.31", TRUE },
		{ "::ffff:192.0.2.31", TRUE }, { "192.0.2.32", FALSE }, { "172.30.2.3", TRUE }, { "172.30.2.4", FALSE },
		{ "2001:db8:1:2::7", TRUE }, { "2001:db8:1:2::8", FALSE }, { "2001:db8:1:2:aaaa::7", FALSE },
		{ "2001:db8:9::1", TRUE }, { "2001:db8:9:0:ffff:ffff:ffff:ffff", TRUE }, { "2001:db8:9:1::1", FALSE },
		{ "unknown", FALSE }, { "", FALSE }, { NULL, FALSE }
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	guint i;

	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "127.0.0.1, 192.0.2.20/28, ::ffff:172.30.2.3, "
		"2001:db8:1:2::7, 2001:db8:9::/64", &error));
	g_assert_no_error(error);
	text = axis_web_auth_portal_peers_text(fx->auth);
	g_assert_cmpstr(text, ==, "127.0.0.1, 192.0.2.16/28, 172.30.2.3, 2001:db8:1:2::7, 2001:db8:9::/64");
	g_assert_false(axis_web_auth_portal_peers_none(fx->auth));
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_test_message("%s", cases[i].address != NULL ? cases[i].address : "(null)");
		g_assert_true(axis_web_auth_portal_peer_listed(fx->auth, cases[i].address) == cases[i].listed);
		/* The login asks the same of the connection and of the client. */
		g_assert_true(axis_web_auth_portal_bars_login(fx->auth, cases[i].address, "198.51.100.1", "login") ==
			cases[i].listed);
		g_assert_true(axis_web_auth_portal_bars_login(fx->auth, "198.51.100.1", cases[i].address, "login") ==
			cases[i].listed);
	}
	g_assert_false(axis_web_auth_portal_bars_login(fx->auth, NULL, NULL, "login"));

	g_assert_false(axis_web_auth_set_portal_peers(fx->auth, "127.0.0.1, axis-portal", &error));
	g_assert_error(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(axis_web_auth_set_portal_peers(fx->auth, "0.0.0.0/0", &error));
	g_clear_error(&error);
	g_clear_pointer(&text, g_free);
	text = axis_web_auth_portal_peers_text(fx->auth);
	g_assert_cmpstr(text, ==, "127.0.0.1, 192.0.2.16/28, 172.30.2.3, 2001:db8:1:2::7, 2001:db8:9::/64");

	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "", &error));
	g_assert_true(axis_web_auth_portal_peers_none(fx->auth));
	g_assert_false(axis_web_auth_portal_peer_listed(fx->auth, "127.0.0.1"));
}

/*
 * Scenarios (web-auth, slice P4, after its review): The client a trusted
 * proxy names is the portal's; A forged X-Forwarded-For from the portal's
 * own address; The portal's addresses and the trusted proxies overlap.  The
 * daemon believes two proxies, 127.0.0.1 and 127.0.0.2.
 */
static void
test_portal_peers_forwarded(Fixture *fx, gconstpointer data)
{
	static const gchar *const from_portal[] = { "X-Forwarded-For", "198.51.100.20", "X-Forwarded-Proto", "https", NULL };
	static const gchar *const from_mapped[] = { "X-Forwarded-For", "::ffff:198.51.100.20", NULL };
	static const gchar *const from_desk[] = { "X-Forwarded-For", "198.51.100.21", "X-Forwarded-Proto", "https", NULL };
	static const gchar *const forged[] = { "X-Forwarded-For", "198.51.100.7", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *admin = NULL;
	g_autofree gchar *cookies = NULL;
	g_autofree gchar *peer = NULL;
	g_autofree gchar *proxy = NULL;
	AxisTestReply reply;
	guint hashes;

	/* The portal is somewhere else, and reaches the engine's login through
	 * the reverse proxy administrators use (the way Traefik would carry it):
	 * the connection is the proxy's, the client is the portal's, refused. */
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "198.51.100.20", &error));
	g_assert_no_error(error);
	g_assert_false(axis_web_auth_portal_peers_overlap(fx->auth, NULL, NULL));
	hashes = axis_password_verify_count();
	reply = engine_login_from(fx, fx->people, "root", ADMIN_PASSWORD, from_portal);
	assert_status(&reply, 401);
	assert_holds(reply.body, "The username or password is incorrect.");
	cookies = cookies_after(&reply);
	g_assert_null(strstr(cookies, "axis_session="));
	g_clear_pointer(&cookies, g_free);
	axis_test_reply_clear(&reply);
	reply = engine_login_from(fx, fx->session, "root", ADMIN_PASSWORD, from_mapped);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: login refused: the client 198.51.100.20, as a "
		"trusted proxy names it, is one of the request portal's addresses*"), ==, 1);
	/* Through the same proxy from the address beside it: accepted. */
	reply = engine_login_from(fx, fx->people, "root", ADMIN_PASSWORD, from_desk);
	assert_status(&reply, 303);
	admin = cookies_after(&reply);
	g_assert_nonnull(strstr(admin, "__Host-axis_session="));
	axis_test_reply_clear(&reply);
	/* Nothing overlaps: no notice. */
	axis_test_web_read_attention(fx->web);
	reply = axis_test_web_request(fx->people, fx->port, "GET", "/attention", admin, from_desk, NULL);
	assert_status(&reply, 200);
	assert_lacks(reply.body, "share an address");
	axis_test_reply_clear(&reply);

	/* The portal's own address is also a listed proxy's (the review's S1: a
	 * pod network trusted whole).  What it forwards cannot take its own
	 * address away: the connection is asked too. */
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, PORTAL_ADDRESS, &error));
	g_assert_no_error(error);
	/* (Two minutes on, so that the log has a line for the next refusal.) */
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(120) * G_USEC_PER_SEC);
	hashes = axis_password_verify_count();
	reply = engine_login_from(fx, fx->session, "root", ADMIN_PASSWORD, forged);
	assert_status(&reply, 401);
	assert_holds(reply.body, "The username or password is incorrect.");
	cookies = cookies_after(&reply);
	g_assert_null(strstr(cookies, "axis_session="));
	g_clear_pointer(&cookies, g_free);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: login refused: the connection came from "
		"127.0.0.1*whatever is forwarded*"), ==, 1);
	/* The forged address was not let in by another door either: it holds no
	 * failure, and the same password from the other proxy, for a client that
	 * is nobody's, opens a session. */
	reply = engine_login_from(fx, fx->people, "root", ADMIN_PASSWORD, forged);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);

	/* That overlap is an administrator's mistake, and is said: by the
	 * function the daemon asks at start, under "Worth a look", and on the
	 * page. */
	g_assert_true(axis_web_auth_portal_peers_overlap(fx->auth, &peer, &proxy));
	g_assert_cmpstr(peer, ==, "127.0.0.1");
	g_assert_cmpstr(proxy, ==, "127.0.0.1");
	axis_test_web_read_attention(fx->web);
	reply = axis_test_web_request(fx->people, fx->port, "GET", "/attention", admin, from_desk, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "Worth a look");
	assert_holds(reply.body, "The request portal and a trusted reverse proxy share an address: 127.0.0.1 is among the "
		"portal&#39;s addresses (portal_peers) and 127.0.0.1 among the trusted proxies.");
	assert_holds(reply.body, "Give the portal an address of its own");
	assert_holds(reply.body, "href=\"/admin/portal\"");
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->people, fx->port, "GET", "/admin/portal", admin, from_desk, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "they share an address, so every login to Axis that arrives through that proxy is refused");
	axis_test_reply_clear(&reply);
}

static gchar *
token_use(Fixture *fx)
{
	return Q("SELECT coalesce(last_used_at::text, 'never') || ' ' || coalesce(last_address, '-') FROM portal_token"
		" WHERE id = %" G_GINT64_FORMAT, fx->token_id);
}

/*
 * Scenarios (portal, slice P4, after its review): Only the portal's
 * addresses; A valid token from another address; The portal's addresses are
 * not set.  The portal listener answers 127.0.0.1 alone.  From 127.0.0.2
 * every request gets one answer, whether its token is right, wrong or
 * absent, before the token is looked at: nothing is counted for the address
 * and the token is not marked used.  The listener reads no forwarding header,
 * from an address it refuses or from the portal's.  With the list empty it
 * answers 503 to everyone, and the page says why.
 */
static void
test_portal_peers_gate(Fixture *fx, gconstpointer data)
{
	static const gchar *const claims_portal[] = { "X-Forwarded-For", PORTAL_ADDRESS, NULL };
	static const gchar *const claims_other[] = { "X-Forwarded-For", "198.51.100.9", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *admin = engine_session(fx, "root", ADMIN_PASSWORD);
	g_autofree gchar *unused = token_use(fx);
	g_autofree gchar *used = NULL;
	g_autofree gchar *refused = NULL;
	AxisTestReply reply;
	guint i;

	g_assert_cmpstr(unused, ==, "never -");
	/* Outside the list, with the right token: refused, and the body is the
	 * one every other request from there gets. */
	reply = send_over(fx->people, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 403);
	assert_api_reply(&reply);
	assert_holds(reply.body, "\"error\":\"not-a-portal\"");
	g_assert_null(axis_test_reply_header(&reply, "WWW-Authenticate"));
	refused = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	/* A wrong token, far more often than an address may send one: the same
	 * answer each time, never a wait.  Nothing was counted. */
	for (i = 0; i < AXIS_PORTAL_TOKEN_FREE_FAILURES + 10; i++) {
		reply = send_over(fx->people, fx->portal_port, "POST", API "hello", "zz-not-a-token", NULL, "{}", NULL);
		assert_status(&reply, 403);
		g_assert_cmpstr(reply.body, ==, refused);
		g_assert_null(axis_test_reply_header(&reply, "Retry-After"));
		axis_test_reply_clear(&reply);
	}
	/* No token, another path, another method, a query, what a browser
	 * sends, a login: the same. */
	reply = send_over(fx->people, fx->portal_port, "POST", API "hello", NULL, NULL, "{}", NULL);
	assert_status(&reply, 403);
	g_assert_cmpstr(reply.body, ==, refused);
	axis_test_reply_clear(&reply);
	reply = send_over(fx->people, fx->portal_port, "GET", "/healthz?x=1", fx->token, NULL, NULL, NULL);
	assert_status(&reply, 403);
	g_assert_cmpstr(reply.body, ==, refused);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *body = login_body("robin", ROBIN_PASSWORD, "203.0.113.9");
		guint hashes = axis_password_verify_count();

		reply = send_over(fx->people, fx->portal_port, "POST", API "login", fx->token, NULL, body, NULL);
		assert_status(&reply, 403);
		g_assert_cmpstr(reply.body, ==, refused);
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(axis_password_verify_count(), ==, hashes);
	}
	/* The listener reads no forwarding header: an address outside the list
	 * that says it is the portal is outside the list. */
	reply = send_over(fx->people, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", claims_portal);
	assert_status(&reply, 403);
	g_assert_cmpstr(reply.body, ==, refused);
	axis_test_reply_clear(&reply);
	/* The token was looked at by none of these: it is as unused as it was. */
	while (g_main_context_iteration(NULL, FALSE))
		;
	used = token_use(fx);
	g_assert_cmpstr(used, ==, unused);
	g_clear_pointer(&used, g_free);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*carried a token that is not a portal's*"), ==, 0);
	/* Written down once, with the address to list. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*portal API: a connection from 127.0.0.2 was refused "
		"before its token was looked at*AXIS_PORTAL_PEERS*"), ==, 1);

	/* From the portal's address the API answers; and there, too, what is
	 * forwarded is not read: the token was used from 127.0.0.1, whatever the
	 * request said of where it came from. */
	reply = send_to(fx, fx->portal_port, "POST", API "hello", "zz-not-a-token", NULL, "{}", NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", claims_other);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);
	for (i = 0; i < 200; i++) {
		g_clear_pointer(&used, g_free);
		used = token_use(fx);
		if (g_strcmp0(used, unused) != 0)
			break;
		g_usleep(10000);
		while (g_main_context_iteration(NULL, FALSE))
			;
	}
	g_assert_true(g_str_has_suffix(used, " 127.0.0.1"));
	assert_lacks(used, "198.51.100.9");

	/* That nothing was counted for the address that was refused: once it is
	 * listed, its first wrong token is its first (401, not the wait that
	 * twenty wrong tokens would have earned it), and its right one works. */
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, PORTAL_ADDRESS ", " PEOPLE_ADDRESS, &error));
	g_assert_no_error(error);
	reply = send_over(fx->people, fx->portal_port, "POST", API "hello", "zz-not-a-token", NULL, "{}", NULL);
	assert_status(&reply, 401);
	axis_test_reply_clear(&reply);
	reply = send_over(fx->people, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 200);
	axis_test_reply_clear(&reply);

	/* The list is empty: nobody is the portal, and everyone is told the
	 * same, the portal's own address and a right token included. */
	g_assert_true(axis_web_auth_set_portal_peers(fx->auth, "", &error));
	g_assert_no_error(error);
	reply = send_to(fx, fx->portal_port, "POST", API "hello", fx->token, NULL, "{}", NULL);
	assert_status(&reply, 503);
	assert_api_reply(&reply);
	assert_holds(reply.body, "\"error\":\"portal-peers\"");
	assert_holds(reply.body, "AXIS_PORTAL_PEERS");
	g_clear_pointer(&refused, g_free);
	refused = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	reply = send_over(fx->people, fx->portal_port, "POST", API "hello", "zz-not-a-token", NULL, "{}", NULL);
	assert_status(&reply, 503);
	g_assert_cmpstr(reply.body, ==, refused);
	axis_test_reply_clear(&reply);
	reply = send_to(fx, fx->portal_port, "GET", "/", NULL, NULL, NULL, NULL);
	assert_status(&reply, 503);
	g_assert_cmpstr(reply.body, ==, refused);
	axis_test_reply_clear(&reply);
	/* The page says so in plain words, and how to set them. */
	reply = axis_test_web_request(fx->people, fx->port, "GET", "/admin/portal", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "The portal&#39;s addresses are not set, so the portal API answers nobody: every request "
		"to it gets 503.");
	assert_holds(reply.body, "Set portal_peers in the [server] section of the configuration file, or AXIS_PORTAL_PEERS "
		"in the environment");
	axis_test_reply_clear(&reply);
	/* And with nobody named, nobody is refused at the login for it. */
	reply = engine_login_from(fx, fx->session, "root", ADMIN_PASSWORD, NULL);
	assert_status(&reply, 303);
	axis_test_reply_clear(&reply);
}

/*
 * The review's S3: a flow of failed logins that goes on is told of in each
 * of its hours.  Thirty failures fill the hour; from then on one login a
 * minute is admitted and fails, for three hours.  The hour's ceiling is
 * recorded as reached once at the start and once in each hour after: four
 * times, where "== the ceiling" recorded it once and let the notice lapse
 * while the guessing went on.
 */
static void
test_limit_sustained(Fixture *fx, gconstpointer data)
{
	guint minute;
	guint i;

	for (i = 0; i < AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR; i++) {
		g_autofree gchar *name = g_strdup_printf("stranger%u", i);
		g_autofree gchar *address = g_strdup_printf("203.0.113.%u", i + 1);
		g_autofree gchar *body = login_body(name, "some-password-or-other", address);

		g_free(expect(fx, "login", body, 401));
	}
	g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS, NULL), ==, 1);
	for (minute = 1; minute <= 180; minute++) {
		g_autofree gchar *name = g_strdup_printf("later%u", minute);
		g_autofree gchar *address = g_strdup_printf("198.51.100.%u", 1 + minute % 250);
		g_autofree gchar *body = login_body(name, "some-password-or-other", address);

		axis_web_auth_set_time_offset(fx->auth, (gint64)minute * 61 * G_USEC_PER_SEC);
		g_free(expect(fx, "login", body, 401));
		/* (The hour stays full throughout.) */
		g_assert_cmpuint(axis_web_auth_portal_failures(fx->auth), >=, AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR);
		if (minute == 30)
			g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS,
				NULL), ==, 1);
	}
	/* 180 minutes of 61 seconds: three hours and three minutes. */
	g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS, NULL), ==, 4);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*the portal reached its limit of failed logins*"), ==, 4);
}

/*
 * Scenario (attention, slice P4): The portal reached a limit.  The hour's
 * ceiling of failed logins is reached twice, and the day's ceiling of
 * requests once: one notice under "Worth a look" says which and how often,
 * links to the page "Request portal" and names no account; the page shows
 * where the day and the hour stand.  Nobody but an administrator is shown
 * either.
 */
static void
test_limit_notice(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *admin = engine_session(fx, "root", ADMIN_PASSWORD);
	g_autofree gchar *uma = engine_session(fx, "uma", USER_PASSWORD);
	g_autofree gchar *sam = NULL;
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	AxisTestReply reply;
	guint round;
	guint i;

	/* Nothing reached: no notice, and the page says so. */
	axis_test_web_read_attention(fx->web);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/attention", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_lacks(reply.body, "The request portal reached");
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/portal", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "Requests made through the portal today count 0 of 30");
	assert_holds(reply.body, "Failed logins through the portal in the last hour: 0 of 30.");
	assert_holds(reply.body, "Neither limit was reached in the last 24 hours");
	assert_holds(reply.body, "have not been reviewed for exposure to the internet");
	assert_holds(reply.body, "only from a network you trust");
	assert_lacks(reply.body, "is not part of this version");
	axis_test_reply_clear(&reply);

	/* Twenty-nine failures reach nothing. */
	for (i = 0; i < AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR - 1; i++) {
		g_autofree gchar *name = g_strdup_printf("stranger%u", i);
		g_autofree gchar *address = g_strdup_printf("203.0.113.%u", i + 1);
		g_autofree gchar *body = login_body(name, "some-password-or-other", address);

		g_free(expect(fx, "login", body, 401));
	}
	g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS, NULL), ==, 0);

	/* The thirtieth reaches the hour's ceiling; the ones admitted beyond it
	 * reach nothing anew.  More than an hour later, thirty more: twice. */
	for (round = 0; round < 2; round++) {
		guint from = round == 0 ? AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR - 1 : 0;

		axis_web_auth_set_time_offset(fx->auth, (gint64)round * 3700 * G_USEC_PER_SEC);
		for (i = from; i < AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR; i++) {
			g_autofree gchar *name = g_strdup_printf("stranger%u-%u", round, i);
			g_autofree gchar *address = g_strdup_printf("203.0.113.%u", 100 + i);
			g_autofree gchar *body = login_body(name, "some-password-or-other", address);

			g_free(expect(fx, "login", body, 401));
		}
		g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS, NULL),
			==, round + 1);
		{
			/* One more is admitted over the ceiling, and fails. */
			g_autofree gchar *body = login_body("stranger-more", "some-password-or-other", "203.0.113.250");

			g_free(expect(fx, "login", body, 401));
		}
		g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_LOGINS, NULL),
			==, round + 1);
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*the portal reached its limit of failed logins*"), ==, 2);

	axis_test_web_read_attention(fx->web);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/attention", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "Worth a look");
	assert_holds(reply.body, "The request portal reached its limit of failed logins in an hour twice in the last 24 "
		"hours.");
	assert_holds(reply.body, "href=\"/admin/portal\"");
	assert_holds(reply.body, "Open Request portal");
	/* It names no account: not one that failed, not one that exists. */
	assert_lacks(reply.body, "stranger");
	assert_lacks(reply.body, "robin");
	assert_lacks(reply.body, "203.0.113.");
	axis_test_reply_clear(&reply);

	/* The day's ceiling, reached by the first request it refuses; the ones
	 * refused after it on the same day reach nothing anew.  (A remembered
	 * device is not held to the hour's ceiling, but sam has none: the hour
	 * is waited out.) */
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(3) * 3700 * G_USEC_PER_SEC);
	sam = login(fx, "sam", SAM_PASSWORD);
	g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_PORTAL_DAY, "1", &error));
	g_assert_no_error(error);
	g_free(ask_movie(fx, sam, kiln, 200));
	g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_DAY, NULL), ==, 0);
	g_free(ask_movie(fx, sam, bells, 429));
	g_free(ask_movie(fx, sam, ledger, 429));
	g_assert_cmpuint(axis_requests_portal_limit_count(fx->r->requests, AXIS_REQUESTS_PORTAL_LIMIT_DAY, NULL), ==, 1);

	axis_test_web_read_attention(fx->web);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/attention", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "The request portal reached its limit of failed logins in an hour twice and its limit of "
		"requests in a day once in the last 24 hours.");
	assert_lacks(reply.body, "sam");
	axis_test_reply_clear(&reply);

	/* The page: where the day and the hour stand, and how often. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/portal", admin, NULL, NULL);
	assert_status(&reply, 200);
	assert_holds(reply.body, "Requests made through the portal today count 1 of 1");
	/* (An apostrophe is written as a character reference.) */
	assert_holds(reply.body, "share of the day is 1.");
	assert_holds(reply.body, "limit of requests 1 time, the hour&#39;s limit of failed logins 2 times.");
	assert_lacks(reply.body, "stranger");
	axis_test_reply_clear(&reply);

	/* Not an administrator: no notice, and no page. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/", uma, NULL, NULL);
	assert_status(&reply, 200);
	assert_lacks(reply.body, "The request portal reached");
	assert_lacks(reply.body, "/attention");
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "GET", "/admin/portal", uma, NULL, NULL);
	g_assert_cmpuint(reply.status, !=, 200);
	axis_test_reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web/auth/login-from-portal-address", Fixture, NULL, test_portal_peers_login);
	AXIS_TEST_ADD_DB_CASE("/web/auth/portal-addresses-as-written", Fixture, NULL, test_portal_peers_forms);
	AXIS_TEST_ADD_DB_CASE("/web/auth/portal-address-forwarded", Fixture, GUINT_TO_POINTER(BEHIND_PROXIES),
		test_portal_peers_forwarded);
	AXIS_TEST_ADD_DB_CASE("/portal/api/listener/only-the-portals-addresses", Fixture, NULL, test_portal_peers_gate);
	AXIS_TEST_ADD_DB_CASE("/web/attention/portal-limit-sustained", Fixture, NULL, test_limit_sustained);
	AXIS_TEST_ADD_DB_CASE("/web/attention/portal-reached-a-limit", Fixture, NULL, test_limit_notice);
	AXIS_TEST_ADD_DB_CASE("/portal/api/listener/only-the-api", Fixture, NULL, test_listener);
	AXIS_TEST_ADD_DB_CASE("/portal/api/token/gate", Fixture, NULL, test_gate);
	AXIS_TEST_ADD_DB_CASE("/web/portal/token-shown-once", Fixture, NULL, test_token_page);
	AXIS_TEST_ADD_DB_CASE("/portal/api/token/wrong-throttled", Fixture, NULL, test_token_throttle);
	AXIS_TEST_ADD_DB_CASE("/portal/api/scope/every-other-route", Fixture, NULL, test_every_route);
	AXIS_TEST_ADD_DB_CASE("/portal/api/scope/member-not-defined", Fixture, NULL, test_members);
	AXIS_TEST_ADD_DB_CASE("/portal/api/login/uniform-failures", Fixture, NULL, test_login);
	AXIS_TEST_ADD_DB_CASE("/portal/api/proof/anothers-request", Fixture, NULL, test_proof);
	AXIS_TEST_ADD_DB_CASE("/portal/api/proof/account-changes", Fixture, NULL, test_account_changes);
	AXIS_TEST_ADD_DB_CASE("/web/auth/no-session-for-requester", Fixture, GUINT_TO_POINTER(OUTSIDE_SESSION),
		test_no_session_for_requester);
	AXIS_TEST_ADD_DB_CASE("/portal/api/fields/answers", Fixture, NULL, test_answers);
	AXIS_TEST_ADD_DB_CASE("/portal/api/search/provider-unreachable", Fixture, NULL, test_search_stale);
	AXIS_TEST_ADD_DB_CASE("/portal/api/limits/one-password", Fixture, NULL, test_login_throttle);
	AXIS_TEST_ADD_DB_CASE("/portal/api/limits/across-names", Fixture, NULL, test_login_ceiling);
	AXIS_TEST_ADD_DB_CASE("/portal/api/hashing/queue-bounded", Fixture, NULL, test_hashing);
	AXIS_TEST_ADD_DB_CASE("/portal/api/limits/search-budget", Fixture, NULL, test_search_budget);
	AXIS_TEST_ADD_DB_CASE("/portal/api/limits/daily-ceiling", Fixture, NULL, test_request_limits);
	AXIS_TEST_ADD_DB_CASE("/portal/api/limits/one-persons-share", Fixture, NULL, test_portal_share);
	AXIS_TEST_ADD_DB_CASE("/portal/api/limits/operation-budgets", Fixture, NULL, test_operation_budgets);
	AXIS_TEST_ADD_DB_CASE("/portal/api/fields/mark-not-known", Fixture, NULL, test_mark_not_known);
	AXIS_TEST_ADD_DB_CASE("/portal/api/fields/kind-off", Fixture, NULL, test_kind_off);
	AXIS_TEST_ADD_DB_CASE("/portal/api/login/engine-queue-full", Fixture, NULL, test_engine_queue_full);

	return g_test_run();
}
