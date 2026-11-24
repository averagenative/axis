/*
 * axis-web-auth.c - who may use the web interface: the middleware, the
 * route check, CSRF, cookies and the worker threads
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-auth-private.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"
#include "web/axis-web-auth-portal.h"
#include "web/axis-web-path.h"

#include <sodium.h>
#include <stdarg.h>
#include <string.h>

#include "api/axis-portal-proto.h"
#include "api/axis-worker-proto.h"

#define STATE_KEY "axis-web-auth-state"

/* Last-use times are written this often, expired sessions swept this
 * often. */
#define FLUSH_INTERVAL_S (60)
#define SWEEP_INTERVAL_S (15 * 60)

/* The throttles' numbers are in axis-web-auth-private.h: the portal API's
 * logins use the same. */
#define USER_FREE_FAILURES     AXIS_AUTH_USER_FREE_FAILURES
#define ADDRESS_FREE_FAILURES  AXIS_AUTH_ADDRESS_FREE_FAILURES
#define THROTTLE_BASE_US       AXIS_AUTH_THROTTLE_BASE_US
#define THROTTLE_MAX_US        AXIS_AUTH_THROTTLE_MAX_US
#define THROTTLE_FORGET_US     AXIS_AUTH_THROTTLE_FORGET_US
#define THROTTLE_MAX_KEYS      AXIS_AUTH_THROTTLE_MAX_KEYS

/* Wrong worker tokens: an address gets this many before it waits.  A token
 * is 256 random bits, so the throttle is not what protects it; it keeps a
 * misconfigured or hostile client from filling the log and the CPU. */
#define WORKER_FREE_FAILURES   10

/* ── Request state ──────────────────────────────────────────────── */

static void
state_free(gpointer data)
{
	AxisAuthState *state = data;

	if (state == NULL)
		return;

	g_free(state->client_address);
	g_free(state->peer_address);
	g_free(state->forwarded_host);
	g_free(state->username);
	g_free(state->theme);
	g_free(state->add_defaults);
	axis_secret_free(state->token);
	g_free(state->token_hash);
	g_free(state->public_id);
	axis_secret_free(state->csrf);
	g_free(state->worker_name);
	g_free(state);
}

const AxisAuthState *
axis_web_auth_state(HtmxRequest *request)
{
	if (request == NULL)
		return NULL;

	return g_object_get_data(G_OBJECT(request), STATE_KEY);
}

AxisWebAuth *
axis_web_auth_from_request(HtmxRequest *request)
{
	AxisWeb *web = axis_web_from_request(request);

	return web != NULL ? axis_web_get_auth(web) : NULL;
}

/* ── Paths ──────────────────────────────────────────────────────── */

/*
 * The exact paths answered without a session, besides everything under
 * /static/.  /livez, /readyz and /healthz are the probes (they say only
 * whether the daemon is alive or ready); /metrics answers only a request
 * carrying the metrics token, and 404 while there is none.  The last three
 * are the icons browsers and phones ask for at the site root without being
 * told to (see axis-web-assets.c); nothing else joins them.
 */
static const gchar *const public_paths[] = {
	"/healthz", "/livez", "/readyz", "/metrics", "/login", "/setup",
	"/favicon.ico", "/apple-touch-icon.png", "/apple-touch-icon-precomposed.png",
	NULL
};

gboolean
axis_web_auth_is_public_path(const gchar *path)
{
	guint i;

	if (path == NULL)
		return FALSE;

	for (i = 0; public_paths[i] != NULL; i++) {
		if (strcmp(path, public_paths[i]) == 0)
			return TRUE;
	}

	return g_str_has_prefix(path, AXIS_WEB_STATIC_PREFIX);
}

gboolean
axis_web_auth_is_public_pattern(const gchar *pattern)
{
	guint i;

	if (pattern == NULL)
		return FALSE;

	for (i = 0; public_paths[i] != NULL; i++) {
		if (strcmp(pattern, public_paths[i]) == 0)
			return TRUE;
	}

	return strcmp(pattern, AXIS_WEB_STATIC_PREFIX "*") == 0;
}

/* "/api/v1/worker" itself, or anything below it. */
static gboolean
under_worker_prefix(const gchar *path)
{
	static const gchar bare[] = "/api/v1/worker";

	G_STATIC_ASSERT(sizeof(bare) == sizeof(AXIS_WORKER_API_PREFIX) - 1);

	return path != NULL && strncmp(path, bare, sizeof(bare) - 1) == 0 &&
		(path[sizeof(bare) - 1] == '\0' || path[sizeof(bare) - 1] == '/');
}

gboolean
axis_web_auth_is_worker_path(const gchar *path)
{
	return under_worker_prefix(path);
}

gboolean
axis_web_auth_is_worker_pattern(const gchar *pattern)
{
	return under_worker_prefix(pattern);
}

/* "/api/v1/portal" itself, or anything below it. */
gboolean
axis_web_auth_is_portal_path(const gchar *path)
{
	static const gchar bare[] = "/api/v1/portal";

	G_STATIC_ASSERT(sizeof(bare) == sizeof(AXIS_PORTAL_API_PREFIX) - 1);

	return path != NULL && strncmp(path, bare, sizeof(bare) - 1) == 0 &&
		(path[sizeof(bare) - 1] == '\0' || path[sizeof(bare) - 1] == '/');
}

HtmxResponse *
axis_web_auth_portal_error(
	guint        status,
	const gchar *word,
	const gchar *message
){
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *body = axis_portal_error_to_json(word, message);

	htmx_response_set_status(response, status);
	htmx_response_set_content(response, body);
	htmx_response_set_content_type(response, "application/json");
	return response;
}

HtmxResponse *
axis_web_auth_worker_error(
	guint        status,
	const gchar *word,
	const gchar *message
){
	HtmxResponse *response = htmx_response_new();
	g_autofree gchar *body = axis_worker_error_to_json(word, message);

	htmx_response_set_status(response, status);
	htmx_response_set_content(response, body);
	htmx_response_set_content_type(response, "application/json");
	return response;
}

/* ── Time and the audit log ─────────────────────────────────────── */

gint64
axis_web_auth_now(AxisWebAuth *self)
{
	return g_get_real_time() + self->time_offset_us;
}

gint64
axis_web_auth_now_monotonic(AxisWebAuth *self)
{
	return g_get_monotonic_time() + self->time_offset_us;
}

void
axis_web_auth_audit(
	const gchar *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list args;

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	g_message("audit: %s", text);
}

/* ── Cookies ────────────────────────────────────────────────────── */

static const gchar *
session_cookie_name(gboolean secure)
{
	return secure ? AXIS_SESSION_COOKIE_SECURE : AXIS_SESSION_COOKIE;
}

static const gchar *
presession_cookie_name(gboolean secure)
{
	return secure ? AXIS_PRESESSION_COOKIE_SECURE : AXIS_PRESESSION_COOKIE;
}

void
axis_web_auth_set_cookie(
	HtmxResponse *response,
	const gchar  *name,
	const gchar  *value,
	gint64        max_age_s,
	gboolean      secure,
	const gchar  *same_site
){
	gchar *header;

	header = g_strdup_printf("%s=%s; Path=/; Max-Age=%" G_GINT64_FORMAT "; HttpOnly; SameSite=%s%s",
		name, value, max_age_s, same_site, secure ? "; Secure" : "");
	htmx_response_append_header(response, "Set-Cookie", header);
	axis_secret_free(header);
}

static SoupMessageHeaders *
request_headers(HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);

	return message != NULL ? soup_server_message_get_request_headers(message) : NULL;
}

static const gchar *
request_header(
	HtmxRequest *request,
	const gchar *name
){
	SoupMessageHeaders *headers = request_headers(request);

	return headers != NULL ? soup_message_headers_get_one(headers, name) : NULL;
}

gchar *
axis_web_auth_get_cookie(
	HtmxRequest *request,
	const gchar *name
){
	SoupMessageHeaders *headers = request_headers(request);
	SoupMessageHeadersIter iter;
	const gchar *header_name;
	const gchar *value;
	GString *joined = NULL;
	gchar *found;

	if (headers == NULL)
		return NULL;

	soup_message_headers_iter_init(&iter, headers);
	while (soup_message_headers_iter_next(&iter, &header_name, &value)) {
		if (g_ascii_strcasecmp(header_name, "Cookie") != 0)
			continue;
		if (joined == NULL) {
			joined = g_string_new(value);
		} else {
			g_string_append(joined, "; ");
			g_string_append(joined, value);
		}
	}
	if (joined == NULL)
		return NULL;

	found = axis_cookie_find(joined->str, name);
	g_string_free(joined, TRUE);

	return found;
}

gchar *
axis_web_auth_presession_value(HtmxRequest *request)
{
	const AxisAuthState *state = axis_web_auth_state(request);
	gchar *value;

	value = axis_web_auth_get_cookie(request,
		presession_cookie_name(state != NULL && state->secure));
	if (!axis_auth_token_is_wellformed(value, AXIS_TOKEN_LEN)) {
		axis_secret_free(value);
		value = axis_random_token();
	}

	return value;
}

void
axis_web_auth_set_presession_cookie(
	HtmxRequest  *request,
	HtmxResponse *response,
	const gchar  *value
){
	const AxisAuthState *state = axis_web_auth_state(request);
	gboolean secure = state != NULL && state->secure;

	axis_web_auth_set_cookie(response, presession_cookie_name(secure), value,
		AXIS_PRESESSION_MAX_AGE, secure, "Strict");
}

void
axis_web_auth_clear_session_cookie(
	HtmxRequest  *request,
	HtmxResponse *response
){
	const AxisAuthState *state = axis_web_auth_state(request);
	gboolean secure = state != NULL && state->secure;

	axis_web_auth_set_cookie(response, session_cookie_name(secure), "", 0, secure, "Lax");
}

void
axis_web_auth_begin_session(
	AxisWebAuth  *self,
	HtmxRequest  *request,
	HtmxResponse *response,
	gint64        user_id,
	gint          epoch
){
	const AxisAuthState *state = axis_web_auth_state(request);
	gboolean secure = state != NULL && state->secure;
	gchar *token = NULL;

	/* Never carry a session over a login: whatever the browser held is
	 * ended and the new session has a token nobody has seen. */
	if (state != NULL && state->token_hash != NULL)
		axis_session_store_revoke(self->sessions, state->token_hash);

	axis_session_store_create(self->sessions, user_id, epoch,
		state != NULL ? state->client_address : NULL,
		request_header(request, "User-Agent"),
		axis_web_auth_now(self), &token);

	axis_web_auth_set_cookie(response, session_cookie_name(secure), token,
		AXIS_SESSION_COOKIE_MAX_AGE, secure, "Lax");
	axis_web_auth_set_cookie(response, presession_cookie_name(secure), "", 0, secure, "Strict");
	axis_secret_free(token);
}

/* ── Responses ──────────────────────────────────────────────────── */

HtmxResponse *
axis_web_auth_redirect(
	HtmxRequest *request,
	const gchar *location
){
	HtmxResponse *response = htmx_response_new();

	if (htmx_request_is_htmx(request)) {
		htmx_response_redirect(response, location);
	} else {
		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", location);
	}

	return response;
}

/* The path (and query) to come back to after logging in, if any. */
static gchar *
return_path(HtmxRequest *request)
{
	g_autofree gchar *candidate = NULL;

	if (htmx_request_is_htmx(request)) {
		/* The endpoint htmx called is not a page: the page it was on is. */
		const gchar *current = request_header(request, "HX-Current-URL");
		g_autoptr(GUri) uri = NULL;

		if (current == NULL)
			return NULL;
		uri = g_uri_parse(current, G_URI_FLAGS_ENCODED, NULL);
		if (uri == NULL)
			return NULL;
		candidate = g_uri_get_query(uri) != NULL ?
			g_strconcat(g_uri_get_path(uri), "?", g_uri_get_query(uri), NULL) :
			g_strdup(g_uri_get_path(uri));
	} else {
		HtmxMethod method = htmx_request_get_method(request);
		const gchar *query = htmx_request_get_query(request);

		if (method != HTMX_METHOD_GET && method != HTMX_METHOD_HEAD)
			return NULL;
		candidate = query != NULL ?
			g_strconcat(htmx_request_get_path(request), "?", query, NULL) :
			g_strdup(htmx_request_get_path(request));
	}

	if (!axis_auth_next_is_safe(candidate) || strcmp(candidate, "/") == 0)
		return NULL;

	return g_steal_pointer(&candidate);
}

/*
 * Whether @request is for an event stream.  A browser follows a redirect
 * from an EventSource to the login page and then fails on its content
 * type with no way to tell why; a plain 401 says what happened.
 */
static gboolean
wants_event_stream(HtmxRequest *request)
{
	SoupMessageHeaders *headers = request_headers(request);
	const gchar *accept = headers != NULL ? soup_message_headers_get_one(headers, "Accept") : NULL;

	return g_strcmp0(htmx_request_get_path(request), "/events") == 0 ||
	       (accept != NULL && strstr(accept, "text/event-stream") != NULL);
}

/* Refuses a request without a session. */
static HtmxResponse *
unauthenticated(
	AxisWebAuth *self,
	HtmxRequest *request
){
	g_autofree gchar *back = NULL;
	g_autofree gchar *location = NULL;
	HtmxResponse *response;

	if (wants_event_stream(request))
		return axis_web_error_respond(request, 401, "Login required",
			"You are not logged in, or your session has ended.");

	if (axis_session_store_count_users(self->sessions) == 0 && self->setup_token != NULL) {
		location = g_strdup("/setup");
	} else {
		back = return_path(request);
		if (back != NULL) {
			g_autofree gchar *escaped = g_uri_escape_string(back, NULL, FALSE);

			location = g_strconcat("/login?next=", escaped, NULL);
		} else {
			location = g_strdup("/login");
		}
	}

	if (htmx_request_is_htmx(request)) {
		response = axis_web_error_respond(request, 401, "Login required",
			"You are not logged in, or your session has ended.");
		htmx_response_redirect(response, location);
		return response;
	}

	return axis_web_auth_redirect(request, location);
}

static HtmxResponse *
csrf_refused(HtmxRequest *request)
{
	return axis_web_error_respond(request, 403, "Request refused",
		"This request could not be verified. Reload the page and try again.");
}

static HtmxResponse *
busy(HtmxRequest *request)
{
	HtmxResponse *response = axis_web_error_respond(request, 503, "Axis is busy",
		"Axis is busy. Try again in a few seconds.");

	htmx_response_add_header(response, "Retry-After", "5");
	return response;
}

/* ── CSRF ───────────────────────────────────────────────────────── */

static gboolean
is_state_changing(HtmxMethod method)
{
	return method != HTMX_METHOD_GET && method != HTMX_METHOD_HEAD;
}

/*
 * Whether the browser says the request comes from this site.  Browsers
 * that send neither header are judged by the token alone.
 */
static gboolean
same_origin(
	HtmxRequest         *request,
	const AxisAuthState *state
){
	const gchar *site = request_header(request, "Sec-Fetch-Site");
	const gchar *origin = request_header(request, "Origin");
	/* The public host: what a trusted proxy says it was asked for, else
	 * the request's own Host header (build_state() decided which). */
	const gchar *host = state->forwarded_host != NULL ? state->forwarded_host :
		request_header(request, "Host");

	if (site != NULL && strcmp(site, "same-origin") != 0)
		return FALSE;
	if (origin != NULL && !axis_auth_origin_matches(origin, host, state->https))
		return FALSE;

	return TRUE;
}

/* The token a multipart form (a file's upload) presents in its field.  The
 * body is the request's, within AXIS_WEB_MAX_REQUEST_BODY. */
static gboolean
multipart_presents(
	HtmxRequest *request,
	const gchar *expected
){
	const gchar *content_type = htmx_request_get_content_type(request);
	GBytes *body = htmx_request_get_body_bytes(request);
	GHashTable *fields = NULL;
	GPtrArray *files;
	gboolean ok;

	if (content_type == NULL || body == NULL || g_ascii_strncasecmp(content_type, "multipart/form-data", 19) != 0)
		return FALSE;
	files = htmx_uploaded_file_parse_multipart(content_type, body, &fields, NULL);
	ok = fields != NULL && axis_secret_equal(g_hash_table_lookup(fields, AXIS_CSRF_FIELD), expected);
	g_clear_pointer(&fields, g_hash_table_unref);
	g_clear_pointer(&files, g_ptr_array_unref);
	return ok;
}

/* The token the request presents: the header, else the form field (of a
 * urlencoded form or a multipart one). */
static gboolean
presents(
	HtmxRequest *request,
	const gchar *expected
){
	const gchar *header = request_header(request, AXIS_CSRF_HEADER);
	const gchar *field = htmx_request_get_form_value(request, AXIS_CSRF_FIELD);

	if (expected == NULL)
		return FALSE;

	return axis_secret_equal(header, expected) || axis_secret_equal(field, expected) ||
		multipart_presents(request, expected);
}

static gboolean
session_csrf_ok(
	HtmxRequest         *request,
	const AxisAuthState *state
){
	return state->user_id != 0 && same_origin(request, state) && presents(request, state->csrf);
}

static gboolean
presession_csrf_ok(
	HtmxRequest         *request,
	const AxisAuthState *state
){
	gchar *cookie;
	gchar *expected;
	gboolean ok;

	if (!same_origin(request, state))
		return FALSE;

	cookie = axis_web_auth_get_cookie(request, presession_cookie_name(state->secure));
	if (!axis_auth_token_is_wellformed(cookie, AXIS_TOKEN_LEN)) {
		axis_secret_free(cookie);
		return FALSE;
	}

	expected = axis_token_derive(cookie, AXIS_PRESESSION_CSRF_PURPOSE);
	ok = presents(request, expected);
	axis_secret_free(expected);
	axis_secret_free(cookie);

	return ok;
}

HtmxResponse *
axis_web_auth_authorize(
	HtmxRequest *request,
	guint        flags
){
	const AxisAuthState *state = axis_web_auth_state(request);
	AxisWebAuth *self = axis_web_auth_from_request(request);

	if (state == NULL || self == NULL) {
		g_autoptr(GError) error = NULL;

		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"the authentication middleware did not run for this request");
		return axis_web_internal_error(request, error);
	}

	/* The worker API: a worker's token, and nothing else, is the
	 * credential, on its routes only. */
	if (flags & AXIS_WEB_ROUTE_WORKER) {
		if (state->worker_id == 0 || !axis_web_auth_is_worker_path(htmx_request_get_path(request)))
			return axis_web_auth_worker_error(401, "unauthorized", "a worker's token is required");
		return NULL;
	}
	if (axis_web_auth_is_worker_path(htmx_request_get_path(request)))
		return axis_web_auth_worker_error(401, "unauthorized", "a worker's token is required");
	/* The portal API is answered on its own listener and nowhere else. */
	if (axis_web_auth_is_portal_path(htmx_request_get_path(request)))
		return axis_web_auth_portal_error(404, "not-found", "there is no such address");

	/* Whatever the route's own class: a requester is no one here (the third
	 * of the role's barriers, beside the login and the session lookup). */
	if (!(flags & AXIS_WEB_ROUTE_PUBLIC) && (state->user_id == 0 || state->role == AXIS_USER_ROLE_REQUESTER))
		return unauthenticated(self, request);

	if ((flags & AXIS_WEB_ROUTE_ADMIN) &&
	    (state->user_id == 0 || state->role != AXIS_USER_ROLE_ADMIN))
		return axis_web_error_respond(request, 403, "Administrators only",
			"This page is only for administrators.");

	if (is_state_changing(htmx_request_get_method(request))) {
		gboolean ok = (flags & AXIS_WEB_ROUTE_PUBLIC) ?
			presession_csrf_ok(request, state) : session_csrf_ok(request, state);

		if (!ok)
			return csrf_refused(request);
	}

	return NULL;
}

/* ── Middleware ─────────────────────────────────────────────────── */

/*
 * The request's client address, scheme and public host, and with
 * @read_session its session (the worker API never reads a cookie).
 *
 * This is the one place they are decided: the audit lines, the login and
 * worker throttles, the sessions' recorded address, the Secure flag and
 * __Host- prefix of the cookies and the cross-site check all read what it
 * stores, and nothing else reads the peer or the forwarded headers.  The
 * peer's own address is stored too, for the login's rule on the portal's
 * addresses.
 */
static AxisAuthState *
build_state(
	AxisWebAuth *self,
	HtmxRequest *request,
	gboolean     read_session
){
	AxisAuthState *state = g_new0(AxisAuthState, 1);
	SoupServerMessage *message = htmx_request_get_message(request);
	SoupMessageHeaders *headers = request_headers(request);
	const gchar *peer = NULL;
	gboolean peer_https = FALSE;
	const gchar *forwarded_for = NULL;
	const gchar *forwarded_proto = NULL;
	const gchar *forwarded_host = NULL;
	const AxisSession *session;
	const AxisUser *user = NULL;
	gchar *token;

	if (message != NULL) {
		GUri *uri = soup_server_message_get_uri(message);

		peer = soup_server_message_get_remote_host(message);
		peer_https = uri != NULL && g_strcmp0(g_uri_get_scheme(uri), "https") == 0;
	}
	if (headers != NULL) {
		forwarded_for = soup_message_headers_get_list(headers, "X-Forwarded-For");
		forwarded_proto = soup_message_headers_get_list(headers, "X-Forwarded-Proto");
		forwarded_host = soup_message_headers_get_list(headers, "X-Forwarded-Host");
	}

	axis_proxy_resolve_full(self->trusted, peer, peer_https, forwarded_for, forwarded_proto,
		forwarded_host, &state->client_address, &state->https, &state->forwarded_host);
	/* Kept beside it for the one rule that asks both (the portal's addresses
	 * at the login, axis-web-auth-portal.h): what a peer forwards cannot
	 * take its own address away. */
	state->peer_address = g_strdup(peer);

	/* Forwarding headers nobody is trusted to send: perhaps a proxy that
	 * should be (axis-web-auth-device.c). */
	if (forwarded_for != NULL && axis_proxy_list_get_length(self->trusted) == 0)
		axis_web_auth_note_forwarded(self, state->client_address);

	/* A request from an address that is not trusted may be from a proxy that
	 * has a new address: have the names looked up again (soon, never waited
	 * for; the answer is for the requests after this one). */
	if (self->resolver != NULL) {
		AxisIpAddress from;

		if (axis_ip_address_parse(peer, &from) && !axis_proxy_list_contains(self->trusted, &from))
			axis_proxy_resolver_poke(self->resolver);
	}
	state->secure = state->https || self->secure_cookies == AXIS_SECURE_COOKIES_ALWAYS;
	if (!read_session)
		return state;

	token = axis_web_auth_get_cookie(request, session_cookie_name(state->secure));
	if (token == NULL)
		return state;

	session = axis_session_store_lookup(self->sessions, token, axis_web_auth_now(self), &user);
	/* A requester has no part in the engine's interface.  The session store
	 * finds no session of this kind for one; this is the same rule said
	 * again where the request's identity is decided, so that it holds
	 * whatever the store is given. */
	if (session == NULL || user == NULL || session->kind != AXIS_SESSION_WEB ||
	    user->role == AXIS_USER_ROLE_REQUESTER) {
		axis_secret_free(token);
		return state;
	}

	state->user_id = user->id;
	state->username = g_strdup(user->username);
	state->role = user->role;
	state->theme = g_strdup(user->theme);
	state->add_defaults = g_strdup(user->add_defaults);
	state->token = token;
	state->token_hash = g_strdup(session->token_hash);
	state->public_id = g_strdup(session->public_id);
	state->csrf = axis_token_derive(token, AXIS_CSRF_PURPOSE);

	return state;
}

/* Makes @state that of a request without a session: its address and scheme
 * stay, everything of an account goes. */
static void
forget_session(AxisAuthState *state)
{
	state->user_id = 0;
	state->role = AXIS_USER_ROLE_USER;
	g_clear_pointer(&state->username, g_free);
	g_clear_pointer(&state->theme, g_free);
	g_clear_pointer(&state->add_defaults, g_free);
	g_clear_pointer(&state->token, axis_secret_free);
	g_clear_pointer(&state->token_hash, g_free);
	g_clear_pointer(&state->public_id, g_free);
	g_clear_pointer(&state->csrf, axis_secret_free);
}

/* The token of "Bearer <token>": the scheme in any case, one space, and the
 * rest exactly; %NULL for anything else (two headers are joined by a comma,
 * which no token has). */
static const gchar *
bearer_token(const gchar *header)
{
	if (header == NULL || g_ascii_strncasecmp(header, "Bearer ", 7) != 0)
		return NULL;
	return header + 7;
}

gboolean
axis_web_auth_is_worker_token(
	AxisWebAuth *self,
	const gchar *authorization
){
	const gchar *token = bearer_token(authorization);
	gint64 id = 0;

	return self != NULL && token != NULL && self->worker_tokens != NULL &&
	       axis_worker_tokens_lookup(self->worker_tokens, token, &id, NULL);
}

/* Whether the body is declared JSON: "application/json", with nothing but
 * a UTF-8 charset after it. */
static gboolean
json_content_type(const gchar *type)
{
	g_autofree gchar *lower = NULL;
	g_autofree gchar *compact = NULL;
	GString *out;
	const gchar *p;

	if (type == NULL)
		return FALSE;
	lower = g_ascii_strdown(type, -1);
	out = g_string_new(NULL);
	for (p = lower; *p != '\0'; p++) {
		if (*p != ' ' && *p != '\t')
			g_string_append_c(out, *p);
	}
	compact = g_string_free(out, FALSE);
	return strcmp(compact, "application/json") == 0 || strcmp(compact, "application/json;charset=utf-8") == 0;
}

/*
 * The worker API's gate.  In this order: the client's address is not
 * throttled (429, before its token is looked at); a worker's token (401,
 * a wrong one counted against the address); nothing a browser sends (403);
 * POST (405); a JSON body (415); a route (404).  Every refusal is JSON,
 * none redirects, none sets a cookie.
 */
static void
worker_gate(
	AxisWebAuth        *self,
	HtmxContext        *context,
	HtmxRequest        *request,
	AxisAuthState      *state,
	HtmxMiddlewareNext  next,
	gpointer            next_data
){
	gint64 now = axis_web_auth_now_monotonic(self);
	g_autofree gchar *address = axis_proxy_address_key(state->client_address);
	gint64 wait;
	const gchar *token;
	HtmxResponse *response;
	gchar *name = NULL;
	gint64 id = 0;

	/* An IPv6 address counts by its /64. */
	wait = axis_login_throttle_check(self->by_worker_address, address, now);
	if (wait > 0) {
		g_autofree gchar *seconds = g_strdup_printf("%" G_GINT64_FORMAT, (wait + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC);

		response = axis_web_auth_worker_error(429, "throttled", "too many wrong tokens from this address; wait");
		htmx_response_add_header(response, "Retry-After", seconds);
		htmx_context_set_response(context, response);
		return;
	}

	/* As a list: two Authorization headers join with a comma, which makes
	 * no well-formed token. */
	token = bearer_token(request_headers(request) != NULL ?
		soup_message_headers_get_list(request_headers(request), "Authorization") : NULL);
	if (token == NULL) {
		response = axis_web_auth_worker_error(401, "unauthorized", "a worker's token is required");
		htmx_response_add_header(response, "WWW-Authenticate", "Bearer");
		htmx_context_set_response(context, response);
		return;
	}
	if (self->worker_tokens == NULL || !axis_worker_tokens_lookup(self->worker_tokens, token, &id, &name)) {
		axis_login_throttle_fail(self->by_worker_address, address, now);
		/* The token is never written anywhere; the address is. */
		g_message("worker API: a request from %s carried a token that is not a worker's (or is revoked)",
			state->client_address != NULL ? state->client_address : "?");
		response = axis_web_auth_worker_error(401, "unauthorized", "the token is not a worker's, or it was revoked");
		htmx_response_add_header(response, "WWW-Authenticate", "Bearer");
		htmx_context_set_response(context, response);
		return;
	}
	state->worker_id = id;
	state->worker_name = name;

	/* A browser can never be made to call this: what it would send is
	 * refused even with a valid token. */
	if (request_header(request, "Origin") != NULL || request_header(request, "Sec-Fetch-Site") != NULL ||
	    request_header(request, "Sec-Fetch-Mode") != NULL || request_header(request, "Cookie") != NULL) {
		htmx_context_set_response(context, axis_web_auth_worker_error(403, "forbidden",
			"the worker API takes no request a browser sends"));
		return;
	}
	if (htmx_request_get_method(request) != HTMX_METHOD_POST) {
		response = axis_web_auth_worker_error(405, "method", "the worker API takes POST only");
		htmx_response_add_header(response, "Allow", "POST");
		htmx_context_set_response(context, response);
		return;
	}
	if (!json_content_type(htmx_request_get_content_type(request))) {
		htmx_context_set_response(context, axis_web_auth_worker_error(415, "content-type",
			"the worker API takes application/json only"));
		return;
	}

	next(context, next_data);
	if (htmx_context_get_response(context) == NULL)
		htmx_context_set_response(context, axis_web_auth_worker_error(404, "not-found",
			"there is no such request in the worker API"));
}

static void
auth_middleware(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	AxisWebAuth *self = user_data;
	HtmxRequest *request = htmx_context_get_request(context);
	const gchar *path = htmx_request_get_path(request);
	AxisAuthState *state;

	/* The portal API (add-request-portal) is not on this listener: every
	 * path under it is one that does not exist, before any credential is
	 * looked at (no cookie, no Authorization header is read). */
	if (axis_web_auth_is_portal_path(path)) {
		htmx_context_set_response(context, axis_web_auth_portal_error(404, "not-found", "there is no such address"));
		return;
	}

	/* The third class of route: a worker's token, never a session. */
	if (axis_web_auth_is_worker_path(path)) {
		state = build_state(self, request, FALSE);
		g_object_set_data_full(G_OBJECT(request), STATE_KEY, state, state_free);
		worker_gate(self, context, request, state, next, next_data);
		return;
	}

	state = build_state(self, request, TRUE);
	/* The third of the requester role's barriers (the login and the session
	 * lookup are the others): whatever identified the request as a
	 * requester's, it goes on as a request without a session, on every path,
	 * public ones too, so that no page or handler ever sees one. */
	if (state->user_id != 0 && state->role == AXIS_USER_ROLE_REQUESTER)
		forget_session(state);
	g_object_set_data_full(G_OBJECT(request), STATE_KEY, state, state_free);

	/* Default deny: only the public paths go further without a session. */
	if (state->user_id == 0 && !axis_web_auth_is_public_path(path)) {
		htmx_context_set_response(context, unauthenticated(self, request));
		return;
	}

	/*
	 * Routes registered through the shell are checked again, CSRF
	 * included, just before their handler.  Anything the shell does not
	 * know of (a route added to the router directly) is held to the CSRF
	 * rule here; a known path with the wrong method needs no check, as
	 * no handler runs for it (the shell answers 405).
	 */
	if (state->user_id != 0 && is_state_changing(htmx_request_get_method(request)) &&
	    !axis_web_knows_path(self->web, path) && !session_csrf_ok(request, state)) {
		htmx_context_set_response(context, csrf_refused(request));
		return;
	}

	next(context, next_data);
}

/* ── Workers ────────────────────────────────────────────────────── */

typedef struct
{
	AxisWebAuth      *auth;
	HtmxRequest      *request;
	HtmxResponse     *deferred;
	AxisAuthWorkFunc  work;
	AxisAuthDoneFunc  done;
	AxisAuthAfterFunc after;	/* work without a request (axis_web_auth_run()) */
	gpointer          data;
	GDestroyNotify    free_data;
	gboolean          skipped;
} Work;

static void
work_free(Work *work)
{
	if (work->free_data != NULL)
		work->free_data(work->data);
	g_clear_object(&work->request);
	g_clear_object(&work->deferred);
	g_free(work);
}

/* On the main loop: answer the request. */
static gboolean
work_complete(gpointer data)
{
	Work *work = data;
	HtmxResponse *response = NULL;

	if (work->after != NULL) {
		work->auth->pending--;
		work->after(work->auth, work->data, work->skipped);
		work_free(work);
		return G_SOURCE_REMOVE;
	}

	if (!work->skipped)
		response = work->done(work->auth, work->request, work->data);
	if (response == NULL)
		response = busy(work->request);

	htmx_response_complete(work->deferred, response);
	g_object_unref(response);

	work->auth->pending--;
	work_free(work);

	return G_SOURCE_REMOVE;
}

/* On a worker thread. */
static void
work_run(
	gpointer data,
	gpointer user_data
){
	Work *work = data;
	GSource *source;

	if (g_atomic_int_get(&work->auth->stopping))
		work->skipped = TRUE;
	else
		work->work(work->auth, work->data);

	source = g_idle_source_new();
	g_source_set_priority(source, G_PRIORITY_DEFAULT);
	g_source_set_callback(source, work_complete, work, NULL);
	g_source_attach(source, work->auth->context);
	g_source_unref(source);
}

HtmxResponse *
axis_web_auth_dispatch(
	AxisWebAuth      *self,
	HtmxRequest      *request,
	AxisAuthWorkFunc  work,
	AxisAuthDoneFunc  done,
	gpointer          data,
	GDestroyNotify    free_data
){
	Work *item;

	if (self->workers == NULL || self->pool == NULL || g_atomic_int_get(&self->stopping) ||
	    self->pending >= AXIS_WEB_AUTH_MAX_PENDING) {
		if (free_data != NULL)
			free_data(data);
		return busy(request);
	}

	item = g_new0(Work, 1);
	item->auth = self;
	item->request = g_object_ref(request);
	item->deferred = htmx_response_new_deferred();
	item->work = work;
	item->done = done;
	item->data = data;
	item->free_data = free_data;

	self->pending++;
	g_thread_pool_push(self->workers, item, NULL);

	return g_object_ref(item->deferred);
}

gboolean
axis_web_auth_run(
	AxisWebAuth       *self,
	AxisAuthWorkFunc   work,
	AxisAuthAfterFunc  after,
	gpointer           data
){
	Work *item;

	if (self->workers == NULL || self->pool == NULL || g_atomic_int_get(&self->stopping) ||
	    self->pending >= AXIS_WEB_AUTH_MAX_PENDING)
		return FALSE;

	item = g_new0(Work, 1);
	item->auth = self;
	item->work = work;
	item->after = after;
	item->data = data;
	self->pending++;
	g_thread_pool_push(self->workers, item, NULL);
	return TRUE;
}

/* ── Page parts ─────────────────────────────────────────────────── */

void
axis_web_auth_add(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

HtmxNode *
axis_web_auth_hidden(
	const gchar *name,
	const gchar *value
){
	return HTMX_NODE(htmx_input_new_hidden(name, value != NULL ? value : ""));
}

HtmxNode *
axis_web_auth_field(
	const gchar   *id,
	const gchar   *label,
	const gchar   *name,
	HtmxInputType  type,
	const gchar   *autocomplete,
	const gchar   *value
){
	HtmxDiv *field = htmx_div_new();
	HtmxLabel *text = htmx_label_new_for(id);
	HtmxInput *input = htmx_input_new(type);

	htmx_element_add_class(HTMX_ELEMENT(field), "field");
	htmx_node_set_text_content(HTMX_NODE(text), label);
	htmx_element_set_id(HTMX_ELEMENT(input), id);
	htmx_input_set_name(input, name);
	htmx_input_set_required(input, TRUE);
	if (autocomplete != NULL)
		htmx_element_set_attribute(HTMX_ELEMENT(input), "autocomplete", autocomplete);
	if (value != NULL)
		htmx_input_set_value(input, value);

	axis_web_auth_add(field, text);
	axis_web_auth_add(field, input);

	return HTMX_NODE(field);
}

HtmxNode *
axis_web_auth_notice(
	gboolean     error,
	const gchar *text
){
	HtmxP *notice = htmx_p_new();

	htmx_element_add_class(HTMX_ELEMENT(notice), "notice");
	htmx_element_add_class(HTMX_ELEMENT(notice), error ? "notice-error" : "notice-ok");
	htmx_element_set_attribute(HTMX_ELEMENT(notice), "role", error ? "alert" : "status");
	htmx_node_set_text_content(HTMX_NODE(notice), text);

	return HTMX_NODE(notice);
}

HtmxNode *
axis_web_auth_button(
	const gchar *label,
	const gchar *css_class
){
	HtmxButton *button = htmx_button_new_submit();

	htmx_node_set_text_content(HTMX_NODE(button), label);
	if (css_class != NULL)
		htmx_element_add_class(HTMX_ELEMENT(button), css_class);

	return HTMX_NODE(button);
}

HtmxNode *
axis_web_auth_post_form(
	HtmxRequest *request,
	const gchar *action
){
	HtmxForm *form = htmx_form_new();
	HtmxNode *csrf = axis_web_csrf_field(request);

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", action);
	if (csrf != NULL)
		axis_web_auth_add(form, csrf);

	return HTMX_NODE(form);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static gboolean
on_flush(gpointer data)
{
	AxisWebAuth *self = data;

	axis_session_store_flush(self->sessions);
	return G_SOURCE_CONTINUE;
}

static gboolean
on_sweep(gpointer data)
{
	AxisWebAuth *self = data;

	axis_session_store_sweep(self->sessions, axis_web_auth_now(self));
	return G_SOURCE_CONTINUE;
}

static gpointer
warmup_main(gpointer data)
{
	axis_password_prepare();
	return NULL;
}

/*
 * The first administrator, from the configuration, while no account
 * exists: the same username and password rules as the setup page, the
 * same one-statement insert that only succeeds on an empty table.  The
 * password goes to the hash and nowhere else; no message repeats it.
 */
static gboolean
create_configured_admin(
	AxisWebAuth  *self,
	const gchar  *typed,
	const gchar  *password,
	GError      **error
){
	g_autofree gchar *username = axis_auth_normalise_username(typed);
	g_autoptr(GError) local = NULL;
	AxisUser *created;
	const gchar *problem;
	gchar *hash;

	if (username == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"the first administrator's username (AXIS_ADMIN_USERNAME, or admin_username in [setup]) "
			"cannot be used: %s", AXIS_USERNAME_RULES);
		return FALSE;
	}
	problem = axis_auth_check_password(password);
	if (problem != NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"the first administrator's password (AXIS_ADMIN_PASSWORD, AXIS_ADMIN_PASSWORD_FILE, or "
			"admin_password in [setup]) cannot be used: %s", problem);
		return FALSE;
	}

	hash = axis_password_hash(password, error);
	if (hash == NULL)
		return FALSE;
	created = axis_user_store_create_first_admin(self->pool, username, hash, &local);
	axis_secret_free(hash);
	if (created == NULL) {
		if (g_error_matches(local, AXIS_USER_ERROR, AXIS_USER_ERROR_SETUP_CLOSED))
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
				"an account was created in the database while the daemon started, so the first "
				"administrator from the configuration was not: start the daemon again");
		else
			g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}

	axis_session_store_put_user(self->sessions, created);
	axis_web_auth_audit("setup: created administrator user=%s from the configuration", created->username);
	axis_user_free(created);
	return TRUE;
}

AxisWebAuth *
axis_web_auth_new(
	AxisDbPool         *pool,
	const gchar        *trusted_proxies,
	AxisSecureCookies   secure_cookies,
	GError            **error
){
	return axis_web_auth_new_with_admin(pool, trusted_proxies, secure_cookies, NULL, NULL, error);
}

AxisWebAuth *
axis_web_auth_new_with_admin(
	AxisDbPool         *pool,
	const gchar        *trusted_proxies,
	AxisSecureCookies   secure_cookies,
	const gchar        *admin_username,
	const gchar        *admin_password,
	GError            **error
){
	g_autoptr(AxisWebAuth) self = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *proxies = NULL;

	if (!axis_password_init()) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"libsodium could not be initialised");
		return NULL;
	}

	self = g_new0(AxisWebAuth, 1);
	self->pool = pool;
	self->secure_cookies = secure_cookies;
	self->context = g_main_context_ref_thread_default();
	self->by_user = axis_login_throttle_new(USER_FREE_FAILURES, THROTTLE_BASE_US,
		THROTTLE_MAX_US, THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	self->by_address = axis_login_throttle_new(ADDRESS_FREE_FAILURES, THROTTLE_BASE_US,
		THROTTLE_MAX_US, THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	/* A remembered device is held to what one username is, and all of an
	 * account's devices together to a ceiling of their own (which no
	 * stranger's failure reaches); a session asked for its account's
	 * current password, to what one username is. */
	self->by_device = axis_login_throttle_new(USER_FREE_FAILURES, THROTTLE_BASE_US,
		THROTTLE_MAX_US, THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	self->by_owner = axis_login_throttle_new(AXIS_DEVICE_OWNER_FREE_FAILURES, THROTTLE_BASE_US,
		THROTTLE_MAX_US, THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	self->by_session = axis_login_throttle_new(USER_FREE_FAILURES, THROTTLE_BASE_US,
		THROTTLE_MAX_US, THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	axis_web_auth_device_init(self);
	self->by_worker_address = axis_login_throttle_new(WORKER_FREE_FAILURES, THROTTLE_BASE_US,
		THROTTLE_MAX_US, THROTTLE_FORGET_US, THROTTLE_MAX_KEYS);
	axis_web_auth_portal_init(self);

	self->trusted = axis_proxy_list_parse(trusted_proxies != NULL ? trusted_proxies : "", &local);
	if (self->trusted == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"trusted proxies: %s", local->message);
		self->stopped = TRUE;
		return NULL;
	}

	self->sessions = axis_session_store_new(pool, axis_web_auth_now(self), error);
	if (self->sessions == NULL) {
		self->stopped = TRUE;
		return NULL;
	}

	if (pool != NULL) {
		self->warmup = g_thread_new("axis-auth-warmup", warmup_main, NULL);
		self->workers = g_thread_pool_new(work_run, NULL, AXIS_WEB_AUTH_WORKERS, FALSE, NULL);
		self->flush_id = g_timeout_add_seconds(FLUSH_INTERVAL_S, on_flush, self);
		self->sweep_id = g_timeout_add_seconds(SWEEP_INTERVAL_S, on_sweep, self);
	}

	proxies = axis_proxy_list_to_string(self->trusted);
	if (axis_proxy_list_has_names(self->trusted)) {
		self->resolver = axis_proxy_resolver_new(self->trusted);
		axis_proxy_resolver_start(self->resolver);
	}
	if (self->resolver != NULL)
		g_info("trusted proxies: %s (names are looked up now, then every %d seconds and when a request "
			"comes from an address that is not trusted)", proxies, AXIS_PROXY_RESOLVE_PERIOD_S);
	else if (proxies[0] != '\0')
		g_info("trusted proxies: %s", proxies);
	else
		g_info("trusted proxies: none (X-Forwarded-For, X-Forwarded-Proto and X-Forwarded-Host are ignored)");
	g_info("secure cookies: %s", secure_cookies == AXIS_SECURE_COOKIES_ALWAYS ?
		"always" : "when the request arrived over HTTPS");

	if (pool != NULL && axis_session_store_count_users(self->sessions) == 0) {
		if (admin_username != NULL) {
			if (!create_configured_admin(self, admin_username, admin_password, error))
				return NULL;
		} else {
			self->setup_token = axis_random_setup_token();
			g_warning("no account exists yet: open /setup in a browser and enter this one-time "
				"setup token to create the first administrator: %s", self->setup_token);
		}
	} else if (pool != NULL && admin_username != NULL) {
		g_message("accounts exist already, so the first administrator in the configuration "
			"(AXIS_ADMIN_USERNAME) was not created; remove it and its password from the configuration");
	}

	return g_steal_pointer(&self);
}

void
axis_web_auth_stop(AxisWebAuth *self)
{
	g_return_if_fail(self != NULL);

	if (self->stopped)
		return;

	g_atomic_int_set(&self->stopping, TRUE);
	/* Portal logins that wait for the hashing place are answered now. */
	axis_web_auth_portal_stop(self);
	axis_proxy_resolver_stop(self->resolver);
	if (self->workers != NULL) {
		g_thread_pool_free(self->workers, FALSE, TRUE);
		self->workers = NULL;
	}
	if (self->warmup != NULL) {
		g_thread_join(self->warmup);
		self->warmup = NULL;
	}
	while (self->pending > 0)
		g_main_context_iteration(self->context, TRUE);

	if (self->flush_id != 0) {
		g_source_remove(self->flush_id);
		self->flush_id = 0;
	}
	if (self->sweep_id != 0) {
		g_source_remove(self->sweep_id);
		self->sweep_id = 0;
	}

	axis_session_store_stop(self->sessions);
	self->stopped = TRUE;
}

void
axis_web_auth_free(AxisWebAuth *self)
{
	if (self == NULL)
		return;

	if (self->sessions != NULL)
		axis_web_auth_stop(self);

	axis_session_store_free(self->sessions);
	axis_login_throttle_free(self->by_user);
	axis_login_throttle_free(self->by_address);
	axis_login_throttle_free(self->by_device);
	axis_login_throttle_free(self->by_owner);
	axis_login_throttle_free(self->by_session);
	axis_login_throttle_free(self->by_worker_address);
	axis_web_auth_portal_clear(self);
	axis_web_auth_device_clear(self);
	sodium_memzero(self->device_key, sizeof(self->device_key));
	g_free(self->forwarding_peer);
	axis_proxy_resolver_free(self->resolver);
	axis_proxy_list_free(self->trusted);
	axis_secret_free(self->setup_token);
	if (self->context != NULL)
		g_main_context_unref(self->context);
	g_free(self);
}

AxisProxyList *
axis_web_auth_get_trusted(AxisWebAuth *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->trusted;
}

void
axis_web_auth_install(
	AxisWebAuth *self,
	AxisWeb     *web,
	HtmxServer  *server
){
	g_return_if_fail(self != NULL);

	self->web = web;
	htmx_server_use(server, auth_middleware, self, NULL);
	axis_web_auth_pages_register(self, web);
	axis_web_account_register(self, web);
	axis_web_auth_proxies_register(self, web);
}

void
axis_web_auth_set_worker_tokens(
	AxisWebAuth      *self,
	AxisWorkerTokens *tokens
){
	g_return_if_fail(self != NULL);

	self->worker_tokens = tokens;
}

/* ── For tests ──────────────────────────────────────────────────── */

void
axis_web_auth_set_time_offset(
	AxisWebAuth *self,
	gint64       offset_us
){
	g_return_if_fail(self != NULL);

	self->time_offset_us = offset_us;
}

gchar *
axis_web_auth_open_test_session(
	AxisWebAuth  *self,
	const gchar  *username,
	AxisUserRole  role
){
	const AxisUser *existing;
	gint64 user_id;
	gint epoch;
	gchar *token = NULL;
	gchar *pair;

	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(self->pool == NULL, NULL);

	existing = axis_session_store_find_user(self->sessions, username);
	if (existing == NULL) {
		AxisUser user;

		memset(&user, 0, sizeof(user));
		user.id = axis_session_store_count_users(self->sessions) + 1;
		user.username = (gchar *)username;
		user.role = role;
		axis_session_store_put_user(self->sessions, &user);
		existing = axis_session_store_find_user(self->sessions, username);
	}
	user_id = existing->id;
	epoch = existing->epoch;

	axis_session_store_create(self->sessions, user_id, epoch, "127.0.0.1", "test",
		axis_web_auth_now(self), &token);
	pair = g_strconcat(AXIS_SESSION_COOKIE "=", token, NULL);
	axis_secret_free(token);

	return pair;
}

void
axis_web_auth_set_test_theme(
	AxisWebAuth *self,
	const gchar *username,
	const gchar *theme
){
	const AxisUser *existing;
	g_autoptr(AxisUser) updated = NULL;

	g_return_if_fail(self != NULL);
	g_return_if_fail(self->pool == NULL);

	existing = axis_session_store_find_user(self->sessions, username);
	g_return_if_fail(existing != NULL);

	updated = axis_user_copy(existing);
	g_free(updated->theme);
	updated->theme = g_strdup(theme);
	axis_session_store_put_user(self->sessions, updated);
}

guint
axis_web_auth_count_held_sessions(
	AxisWebAuth *self,
	gint64       user_id
){
	g_return_val_if_fail(self != NULL, 0);

	return axis_session_store_count_held(self->sessions, user_id);
}

void
axis_web_auth_wait_writes(AxisWebAuth *self)
{
	g_return_if_fail(self != NULL);

	axis_session_store_wait_idle(self->sessions);
}
