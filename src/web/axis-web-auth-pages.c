/*
 * axis-web-auth-pages.c - the login, logout and first-run setup pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Login answers the same way, after the same work, whether the account
 * does not exist, is disabled or was given the wrong password: the
 * password is always verified against a hash (a dummy one when there is
 * no account), the status is always 401 and the message always the same.
 * Only the audit log line tells them apart, and it never names a
 * username that is not an account (it might be a password typed into
 * the wrong box).
 */

#include "web/axis-web-auth-private.h"
#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"

#include <sodium.h>
#include <string.h>

#define LOGIN_FAILED  "The username or password is incorrect."
#define USERNAME_RULES AXIS_USERNAME_RULES

/* ── Shared ─────────────────────────────────────────────────────── */

/*
 * @candidate if it may be used as the page to return to after logging
 * in: a safe local path that is not one of the auth pages themselves.
 */
static gchar *
safe_next(const gchar *candidate)
{
	static const gchar *const loops[] = { "/login", "/logout", "/setup", NULL };
	gsize path_len;
	guint i;

	if (!axis_auth_next_is_safe(candidate))
		return NULL;

	path_len = strcspn(candidate, "?#");
	for (i = 0; loops[i] != NULL; i++) {
		if (path_len == strlen(loops[i]) && strncmp(candidate, loops[i], path_len) == 0)
			return NULL;
	}

	return g_strdup(candidate);
}

/* The account name for an audit line: only one that exists. */
static const gchar *
known_name(
	AxisWebAuth *auth,
	const gchar *username
){
	return axis_session_store_find_user(auth->sessions, username) != NULL ? username : "-";
}

static gchar *
retry_text(gint64 wait_us)
{
	gint64 seconds = (wait_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC;

	return g_strdup_printf("%" G_GINT64_FORMAT, MAX(seconds, 1));
}

static gboolean
setup_open(AxisWebAuth *auth)
{
	return auth->setup_token != NULL && axis_session_store_count_users(auth->sessions) == 0;
}

/* Whether @submitted, without surrounding spaces (it is copied from a
 * log), is @expected; compared in constant time. */
static gboolean
setup_token_matches(
	const gchar *submitted,
	const gchar *expected
){
	gchar *copy;
	gsize len;
	gboolean match;

	if (submitted == NULL || expected == NULL)
		return FALSE;

	len = strlen(submitted);
	copy = g_strdup(submitted);
	match = axis_secret_equal(g_strstrip(copy), expected);
	sodium_memzero(copy, len);
	g_free(copy);

	return match;
}

static HtmxResponse *
not_found(HtmxRequest *request)
{
	return axis_web_error_respond(request, 404, "Not found", "There is no page at this address. The link may be old, or what it showed was removed.");
}

/* ── Login ──────────────────────────────────────────────────────── */

static HtmxResponse *
login_page(
	HtmxRequest *request,
	guint        status,
	const gchar *next,
	const gchar *username,
	const gchar *message
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	g_autoptr(HtmxForm) form = htmx_form_new();
	gchar *presession = axis_web_auth_presession_value(request);
	gchar *form_token = axis_token_derive(presession, AXIS_PRESESSION_CSRF_PURPOSE);
	HtmxResponse *response;

	htmx_element_add_class(HTMX_ELEMENT(content), "auth-page");
	htmx_node_set_text_content(HTMX_NODE(title), "Log in");
	axis_web_auth_add(content, axis_web_lockup_new());
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	if (message != NULL)
		axis_web_auth_add(content, axis_web_auth_notice(TRUE, message));

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/login");
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_hidden(AXIS_CSRF_FIELD, form_token));
	if (next != NULL)
		axis_web_auth_add(form, axis_web_auth_hidden("next", next));
	axis_web_auth_add(form, axis_web_auth_field("username", "Username", "username",
		HTMX_INPUT_TEXT, "username", username));
	axis_web_auth_add(form, axis_web_auth_field("password", "Password", "password",
		HTMX_INPUT_PASSWORD, "current-password", NULL));
	axis_web_auth_add(form, axis_web_auth_button("Log in", "button-primary"));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(form));

	response = axis_web_page_respond(request, "Log in", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	axis_web_auth_set_presession_cookie(request, response, presession);

	axis_secret_free(form_token);
	axis_secret_free(presession);
	return response;
}

static HtmxResponse *
handle_login_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	g_autofree gchar *next = safe_next(htmx_request_get_query_param(request, "next"));

	if (setup_open(auth))
		return axis_web_auth_redirect(request, "/setup");
	if (state->user_id != 0)
		return axis_web_auth_redirect(request, next != NULL ? next : "/");

	return login_page(request, 200, next, NULL, NULL);
}

typedef enum
{
	LOGIN_OK,
	LOGIN_UNKNOWN,
	LOGIN_DISABLED,
	LOGIN_WRONG
} LoginOutcome;

/*
 * Which counts of failures an attempt is held to.  A remembered device
 * (axis-web-auth-device.c) has its own, and all the account's devices
 * together one more (the owner's: no stranger's failure reaches it, and
 * devices cannot multiply guesses); any other attempt is held to the
 * username's and the client address's (an IPv6 address by its /64), which
 * every stranger shares.
 */
typedef struct
{
	AxisWebAuth *auth;
	gchar       *username;	/* normalised; NULL if it cannot be one */
	gchar       *address;	/* as counted */
	gchar       *device;	/* the remembered device's key, or NULL */
	gchar       *owner;	/* with it, the account's ceiling's key */
	gboolean     begun;	/* counted as in flight */
} Attempt;

static void
attempt_init(
	Attempt     *attempt,
	AxisWebAuth *auth,
	HtmxRequest *request,
	const gchar *username
){
	const AxisAuthState *state = axis_web_auth_state(request);
	const AxisUser *account = axis_session_store_find_user(auth->sessions, username);

	memset(attempt, 0, sizeof(*attempt));
	attempt->auth = auth;
	attempt->username = g_strdup(username);
	attempt->address = axis_proxy_address_key(state->client_address);
	if (account != NULL)
		attempt->device = axis_web_auth_device_of(auth, request, account);
	if (attempt->device != NULL)
		attempt->owner = g_strdup_printf("%" G_GINT64_FORMAT, account->id);
}

/* 0 if the attempt may be checked now, else how long until it may. */
static gint64
attempt_wait(
	Attempt *attempt,
	gint64   now
){
	AxisWebAuth *auth = attempt->auth;

	if (attempt->device != NULL)
		return MAX(axis_login_throttle_check(auth->by_device, attempt->device, now),
		           axis_login_throttle_check(auth->by_owner, attempt->owner, now));

	return MAX(axis_login_throttle_check(auth->by_user, attempt->username, now),
	           axis_login_throttle_check(auth->by_address, attempt->address, now));
}

/* Counts the attempt as in flight until attempt_end(). */
static void
attempt_begin(
	Attempt *attempt,
	gint64   now
){
	AxisWebAuth *auth = attempt->auth;

	if (attempt->device != NULL) {
		axis_login_throttle_begin(auth->by_device, attempt->device, now);
		axis_login_throttle_begin(auth->by_owner, attempt->owner, now);
	} else {
		axis_login_throttle_begin(auth->by_user, attempt->username, now);
		axis_login_throttle_begin(auth->by_address, attempt->address, now);
	}
	attempt->begun = TRUE;
}

static void
attempt_fail(
	Attempt *attempt,
	gint64   now
){
	AxisWebAuth *auth = attempt->auth;

	if (attempt->device != NULL) {
		axis_login_throttle_fail(auth->by_device, attempt->device, now);
		axis_login_throttle_fail(auth->by_owner, attempt->owner, now);
	} else {
		axis_login_throttle_fail(auth->by_user, attempt->username, now);
		axis_login_throttle_fail(auth->by_address, attempt->address, now);
	}
}

/* A success clears the counts it was held to, and only those: the
 * owner's login from a remembered device does not undo a stranger's
 * failures. */
static void
attempt_succeed(Attempt *attempt)
{
	AxisWebAuth *auth = attempt->auth;

	if (attempt->device != NULL) {
		axis_login_throttle_clear(auth->by_device, attempt->device);
		axis_login_throttle_clear(auth->by_owner, attempt->owner);
	} else {
		axis_login_throttle_clear(auth->by_user, attempt->username);
	}
}

static void
attempt_clear(Attempt *attempt)
{
	AxisWebAuth *auth = attempt->auth;

	if (attempt->begun) {
		if (attempt->device != NULL) {
			axis_login_throttle_end(auth->by_device, attempt->device);
			axis_login_throttle_end(auth->by_owner, attempt->owner);
		} else {
			axis_login_throttle_end(auth->by_user, attempt->username);
			axis_login_throttle_end(auth->by_address, attempt->address);
		}
		attempt->begun = FALSE;
	}
	g_clear_pointer(&attempt->username, g_free);
	g_clear_pointer(&attempt->address, g_free);
	g_clear_pointer(&attempt->device, g_free);
	g_clear_pointer(&attempt->owner, g_free);
}

typedef struct
{
	gchar        *username;	/* normalised; NULL if it cannot be one */
	gchar        *typed;	/* as submitted, to fill the form again */
	gchar        *password;
	gchar        *next;
	Attempt       attempt;	/* in flight until this is freed */
	AxisUser     *user;
	LoginOutcome  outcome;
	GError       *error;
} LoginWork;

/* On the main loop, in every case (axis_web_auth_dispatch()). */
static void
login_work_free(gpointer data)
{
	LoginWork *work = data;

	attempt_clear(&work->attempt);
	g_free(work->username);
	g_free(work->typed);
	axis_secret_free(work->password);
	g_free(work->next);
	axis_user_free(work->user);
	g_clear_error(&work->error);
	g_free(work);
}

/* On a worker: look the account up and verify, always hashing once. */
static void
login_work(
	AxisWebAuth *auth,
	gpointer     data
){
	LoginWork *work = data;
	gchar *hash = NULL;
	gboolean match;

	if (work->username != NULL &&
	    !axis_user_store_find(auth->pool, work->username, &work->user, &hash, &work->error))
		goto out;

	if (work->user == NULL) {
		(void)axis_password_verify(NULL, work->password);
		work->outcome = LOGIN_UNKNOWN;
		goto out;
	}

	match = axis_password_verify(hash, work->password);
	if (work->user->disabled) {
		work->outcome = LOGIN_DISABLED;
	} else if (!match) {
		work->outcome = LOGIN_WRONG;
	} else {
		work->outcome = LOGIN_OK;

		/* The limits have changed since this hash was made: store a new
		 * one, unless the password changed in the meantime. */
		if (axis_password_needs_rehash(hash)) {
			g_autoptr(GError) error = NULL;
			gchar *fresh = axis_password_hash(work->password, &error);

			if (fresh == NULL ||
			    !axis_user_store_replace_hash(auth->pool, work->user->id, hash, fresh, &error))
				g_warning("login: could not store a new password hash for '%s': %s",
					work->user->username, error->message);
			g_free(fresh);
		}
	}

out:
	axis_secret_free(work->password);
	work->password = NULL;
	g_free(hash);
}

static HtmxResponse *
login_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	LoginWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	gint64 now = axis_web_auth_now_monotonic(auth);
	HtmxResponse *response;

	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	if (work->outcome == LOGIN_OK &&
	    axis_session_store_get_user(auth->sessions, work->user->id) == NULL) {
		g_autoptr(GError) error = NULL;

		/* Not taken from the database here: the account may have been
		 * deleted by this daemon a moment ago, and must stay gone. */
		g_set_error(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			"login: the account '%s' was not created by this daemon since it started; "
			"restart it to log in with that account", work->user->username);
		return axis_web_internal_error(request, error);
	}

	if (work->outcome == LOGIN_OK) {
		attempt_succeed(&work->attempt);
		response = axis_web_auth_redirect(request, work->next != NULL ? work->next : "/");
		axis_web_auth_begin_session(auth, request, response, work->user->id, work->user->epoch);
		/* This browser is remembered from now on (a new one each time),
		 * under the account's epoch as the session is. */
		axis_web_auth_remember_device(auth, request, response,
			axis_session_store_get_user(auth->sessions, work->user->id));
		axis_web_auth_audit("login: user=%s client=%s%s", work->user->username, state->client_address,
			work->attempt.device != NULL ? " (remembered device)" : "");
		return response;
	}

	attempt_fail(&work->attempt, now);

	if (work->outcome == LOGIN_UNKNOWN)
		axis_web_auth_audit("login failed: no such account client=%s", state->client_address);
	else
		axis_web_auth_audit("login failed: user=%s client=%s%s", work->user->username,
			state->client_address,
			work->outcome == LOGIN_DISABLED ? " (account disabled)" : " (wrong password)");

	return login_page(request, 401, work->next, work->typed, LOGIN_FAILED);
}

static HtmxResponse *
handle_login(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *typed = htmx_request_get_form_value(request, "username");
	const gchar *password = htmx_request_get_form_value(request, "password");
	g_autofree gchar *username = axis_auth_normalise_username(typed);
	g_autofree gchar *next = safe_next(htmx_request_get_form_value(request, "next"));
	gint64 now = axis_web_auth_now_monotonic(auth);
	gint64 wait;
	Attempt attempt;
	LoginWork *work;

	/* Refused before any password work, even when it would be right; the
	 * attempts already being checked count as the failures they may be. */
	attempt_init(&attempt, auth, request, username);
	wait = attempt_wait(&attempt, now);
	if (wait > 0) {
		g_autofree gchar *seconds = retry_text(wait);
		g_autofree gchar *message = g_strdup_printf(
			"Too many failed attempts. Try again in %s seconds.", seconds);
		HtmxResponse *response;

		axis_web_auth_audit("login refused, too many failures: user=%s client=%s%s",
			known_name(auth, username), state->client_address,
			attempt.device != NULL ? " (remembered device)" : "");
		attempt_clear(&attempt);
		response = login_page(request, 429, next, typed, message);
		htmx_response_add_header(response, "Retry-After", seconds);
		return response;
	}

	if (password == NULL || strlen(password) > AXIS_PASSWORD_MAX_BYTES) {
		attempt_fail(&attempt, now);
		attempt_clear(&attempt);
		axis_web_auth_audit("login failed: unusable password client=%s", state->client_address);
		return login_page(request, 401, next, typed, LOGIN_FAILED);
	}

	work = g_new0(LoginWork, 1);
	work->username = g_steal_pointer(&username);
	work->typed = g_strdup(typed);
	work->password = g_strdup(password);
	work->next = g_steal_pointer(&next);
	/* In flight from here until the work is freed, whatever happens. */
	work->attempt = attempt;
	attempt_begin(&work->attempt, now);

	return axis_web_auth_dispatch(auth, request, login_work, login_done, work, login_work_free);
}

/* ── Logout ─────────────────────────────────────────────────────── */

static HtmxResponse *
handle_logout(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	HtmxResponse *response;

	axis_session_store_revoke(auth->sessions, state->token_hash);
	axis_web_auth_audit("logout: user=%s client=%s", state->username, state->client_address);

	response = axis_web_auth_redirect(request, "/login");
	axis_web_auth_clear_session_cookie(request, response);
	return response;
}

/* ── Setup ──────────────────────────────────────────────────────── */

static HtmxResponse *
setup_page(
	HtmxRequest *request,
	guint        status,
	const gchar *username,
	const gchar *message
){
	g_autoptr(HtmxDiv) content = htmx_div_new();
	g_autoptr(HtmxHeading) title = htmx_h1_new();
	g_autoptr(HtmxP) intro = htmx_p_new();
	g_autoptr(HtmxForm) form = htmx_form_new();
	gchar *presession = axis_web_auth_presession_value(request);
	gchar *form_token = axis_token_derive(presession, AXIS_PRESESSION_CSRF_PURPOSE);
	HtmxResponse *response;

	htmx_element_add_class(HTMX_ELEMENT(content), "auth-page");
	htmx_node_set_text_content(HTMX_NODE(title), "Set up Axis");
	htmx_node_set_text_content(HTMX_NODE(intro),
		"No account exists yet. Create the first administrator. The one-time setup token "
		"is in the Axis log (docker compose logs axis); restarting Axis prints a new one.");
	axis_web_auth_add(content, axis_web_lockup_new());
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(title));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(intro));
	if (message != NULL)
		axis_web_auth_add(content, axis_web_auth_notice(TRUE, message));

	htmx_element_set_attribute(HTMX_ELEMENT(form), "action", "/setup");
	htmx_element_add_class(HTMX_ELEMENT(form), "form");
	axis_web_auth_add(form, axis_web_auth_hidden(AXIS_CSRF_FIELD, form_token));
	axis_web_auth_add(form, axis_web_auth_field("setup_token", "Setup token", "setup_token",
		HTMX_INPUT_TEXT, "off", NULL));
	axis_web_auth_add(form, axis_web_auth_field("username", "Username", "username",
		HTMX_INPUT_TEXT, "username", username));
	axis_web_auth_add(form, axis_web_auth_field("password", "Password (at least 10 characters)",
		"password", HTMX_INPUT_PASSWORD, "new-password", NULL));
	axis_web_auth_add(form, axis_web_auth_field("password_confirm", "Password again",
		"password_confirm", HTMX_INPUT_PASSWORD, "new-password", NULL));
	axis_web_auth_add(form, axis_web_auth_button("Create administrator", "button-primary"));
	htmx_node_add_child(HTMX_NODE(content), HTMX_NODE(form));

	response = axis_web_page_respond(request, "Set up Axis", HTMX_NODE(content));
	htmx_response_set_status(response, status);
	axis_web_auth_set_presession_cookie(request, response, presession);

	axis_secret_free(form_token);
	axis_secret_free(presession);
	return response;
}

static HtmxResponse *
handle_setup_page(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;

	if (!setup_open(auth))
		return not_found(request);

	return setup_page(request, 200, NULL, NULL);
}

typedef struct
{
	gchar    *username;
	gchar    *password;
	AxisUser *created;
	GError   *error;
} SetupWork;

static void
setup_work_free(gpointer data)
{
	SetupWork *work = data;

	g_free(work->username);
	axis_secret_free(work->password);
	axis_user_free(work->created);
	g_clear_error(&work->error);
	g_free(work);
}

static void
setup_work(
	AxisWebAuth *auth,
	gpointer     data
){
	SetupWork *work = data;
	gchar *hash = axis_password_hash(work->password, &work->error);

	axis_secret_free(work->password);
	work->password = NULL;
	if (hash == NULL)
		return;

	work->created = axis_user_store_create_first_admin(auth->pool, work->username, hash,
		&work->error);
	g_free(hash);
}

static HtmxResponse *
setup_done(
	AxisWebAuth *auth,
	HtmxRequest *request,
	gpointer     data
){
	SetupWork *work = data;
	const AxisAuthState *state = axis_web_auth_state(request);
	HtmxResponse *response;

	if (g_error_matches(work->error, AXIS_USER_ERROR, AXIS_USER_ERROR_SETUP_CLOSED)) {
		axis_web_auth_audit("setup refused: an account already exists client=%s",
			state->client_address);
		return not_found(request);
	}
	if (work->error != NULL)
		return axis_web_internal_error(request, work->error);

	axis_session_store_put_user(auth->sessions, work->created);
	axis_secret_free(auth->setup_token);
	auth->setup_token = NULL;

	response = axis_web_auth_redirect(request, "/");
	axis_web_auth_begin_session(auth, request, response, work->created->id, work->created->epoch);
	axis_web_auth_audit("setup: created administrator user=%s client=%s",
		work->created->username, state->client_address);
	return response;
}

static HtmxResponse *
handle_setup(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	AxisWebAuth *auth = user_data;
	const AxisAuthState *state = axis_web_auth_state(request);
	const gchar *typed = htmx_request_get_form_value(request, "username");
	const gchar *password = htmx_request_get_form_value(request, "password");
	const gchar *confirm = htmx_request_get_form_value(request, "password_confirm");
	const gchar *token = htmx_request_get_form_value(request, "setup_token");
	g_autofree gchar *username = NULL;
	gint64 now = axis_web_auth_now_monotonic(auth);
	gint64 wait;
	const gchar *problem;
	SetupWork *work;

	if (!setup_open(auth))
		return not_found(request);

	wait = axis_login_throttle_check(auth->by_address, state->client_address, now);
	if (wait > 0) {
		g_autofree gchar *seconds = retry_text(wait);
		g_autofree gchar *message = g_strdup_printf(
			"Too many failed attempts. Try again in %s seconds.", seconds);
		HtmxResponse *response = setup_page(request, 429, typed, message);

		htmx_response_add_header(response, "Retry-After", seconds);
		return response;
	}

	if (!setup_token_matches(token, auth->setup_token)) {
		axis_login_throttle_fail(auth->by_address, state->client_address, now);
		axis_web_auth_audit("setup refused: wrong setup token client=%s", state->client_address);
		return setup_page(request, 403, typed,
			"The setup token is not correct. Copy the newest one from the Axis log.");
	}

	username = axis_auth_normalise_username(typed);
	if (username == NULL)
		return setup_page(request, 400, typed, USERNAME_RULES);
	problem = axis_auth_check_password(password);
	if (problem != NULL)
		return setup_page(request, 400, typed, problem);
	if (g_strcmp0(password, confirm) != 0)
		return setup_page(request, 400, typed, "The two passwords do not match.");

	work = g_new0(SetupWork, 1);
	work->username = g_steal_pointer(&username);
	work->password = g_strdup(password);

	return axis_web_auth_dispatch(auth, request, setup_work, setup_done, work, setup_work_free);
}

void
axis_web_auth_pages_register(
	AxisWebAuth *self,
	AxisWeb     *web
){
	axis_web_route_full(web, HTMX_METHOD_GET, "/login", AXIS_WEB_ROUTE_PUBLIC, handle_login_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/login", AXIS_WEB_ROUTE_PUBLIC, handle_login, self);
	axis_web_route_full(web, HTMX_METHOD_GET, "/setup", AXIS_WEB_ROUTE_PUBLIC, handle_setup_page, self);
	axis_web_route_full(web, HTMX_METHOD_POST, "/setup", AXIS_WEB_ROUTE_PUBLIC, handle_setup, self);
	axis_web_post(web, "/logout", handle_logout, self);
}
