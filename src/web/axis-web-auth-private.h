/*
 * axis-web-auth-private.h - shared by the files of the web auth code
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_WEB_AUTH_PRIVATE_H
#define AXIS_WEB_AUTH_PRIVATE_H

#include <htmx-glib.h>

#include "api/axis-worker-tokens.h"
#include "core/axis-proxy.h"
#include "core/axis-proxy-resolver.h"
#include "web/axis-login-throttle.h"
#include "web/axis-session-store.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-private.h"

G_BEGIN_DECLS

/* Cookie names.  Over HTTPS the __Host- prefix makes browsers refuse a
 * cookie of that name set by any other host (a sibling subdomain) or
 * without Secure and Path=/. */
#define AXIS_SESSION_COOKIE            "axis_session"
#define AXIS_SESSION_COOKIE_SECURE     "__Host-axis_session"
#define AXIS_PRESESSION_COOKIE         "axis_presession"
#define AXIS_PRESESSION_COOKIE_SECURE  "__Host-axis_presession"

/* The session cookie lives as long as a session may; the pre-session one
 * long enough to fill in a form. */
#define AXIS_SESSION_COOKIE_MAX_AGE    (90 * 24 * 3600)
#define AXIS_PRESESSION_MAX_AGE        (2 * 3600)

/* What the CSRF tokens are derived for (axis_token_derive()). */
#define AXIS_CSRF_PURPOSE              "axis session csrf v1"
#define AXIS_PRESESSION_CSRF_PURPOSE   "axis presession csrf v1"

/* The CSRF token's form field and header. */
#define AXIS_CSRF_FIELD                "_csrf"
#define AXIS_CSRF_HEADER               "X-CSRF-Token"

/*
 * The remembered device: a cookie set at every successful login, signed
 * with the daemon's device key, that names the account it logged in to.
 * A login attempt that presents one for the account it names is throttled
 * on a count of its own (by_device), not on the username's and address's
 * that strangers' failures fill.  Strict, so no other site's request ever
 * carries it.
 */
#define AXIS_DEVICE_COOKIE             "axis_device"
#define AXIS_DEVICE_COOKIE_SECURE      "__Host-axis_device"
#define AXIS_DEVICE_MAX_AGE            (90 * 24 * 3600)
#define AXIS_DEVICE_KEY_BYTES          32

/* An account keeps at most this many remembered devices (the oldest goes);
 * failures on all of them together count toward one ceiling of the
 * account's (by_owner), with this many free. */
#define AXIS_DEVICE_MAX_PER_USER       20
#define AXIS_DEVICE_OWNER_FREE_FAILURES 10

/* Throttles: free failures, first delay, longest delay, forget after,
 * keys tracked.  A client address is allowed more failures than one
 * username, since several people may share one address. */
#define AXIS_AUTH_USER_FREE_FAILURES     5
#define AXIS_AUTH_ADDRESS_FREE_FAILURES  20
#define AXIS_AUTH_THROTTLE_BASE_US       (G_GINT64_CONSTANT(2) * G_USEC_PER_SEC)
#define AXIS_AUTH_THROTTLE_MAX_US        (G_GINT64_CONSTANT(15) * 60 * G_USEC_PER_SEC)
#define AXIS_AUTH_THROTTLE_FORGET_US     (G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC)
#define AXIS_AUTH_THROTTLE_MAX_KEYS      10000

/* The forwarding hint: this many requests with X-Forwarded-For, all from
 * one private address while no proxy is trusted, before it is given. */
#define AXIS_FORWARDING_HINT_REQUESTS  10

/**
 * AxisAuthAccountGoneFunc:
 * @user_id: the account that was disabled or deleted (committed already)
 * @user_data: (closure): as given to axis_web_auth_set_account_gone()
 *
 * Runs on a worker thread, after the Users page disabled or deleted an
 * account: what another part of Axis keeps for the account follows it (its
 * waiting requests are withdrawn).  It may block on the database.
 */
typedef void (*AxisAuthAccountGoneFunc)(
	gint64   user_id,
	gpointer user_data
);

struct _AxisWebAuth
{
	AxisDbPool        *pool;		/* nullable (tests) */
	AxisProxyList     *trusted;
	AxisProxyResolver *resolver;	/* keeps the names in @trusted current; NULL if it has none */
	AxisSecureCookies  secure_cookies;
	AxisSessionStore  *sessions;
	AxisLoginThrottle *by_user;
	AxisLoginThrottle *by_address;
	AxisLoginThrottle *by_device;	/* a remembered device's own count */
	AxisLoginThrottle *by_owner;	/* all of an account's devices together */
	AxisLoginThrottle *by_session;	/* the current password asked of a session */
	guint8             device_key[AXIS_DEVICE_KEY_BYTES];	/* signs the device cookies */
	GHashTable        *devices;	/* gint64 account -> GPtrArray of its devices, oldest first */
	gint               device_dir_fd;	/* where they are kept, or -1 */
	gchar             *forwarding_peer;	/* the one address forwarded requests came from */
	guint              forwarding_seen;	/* how many, while it was the one */
	gboolean           forwarding_mixed;	/* they came from more than one */
	gboolean           forwarding_logged;
	gchar             *setup_token;	/* while no account exists */
	GThreadPool       *workers;
	GThread           *warmup;		/* makes the dummy hash at start */
	GMainContext      *context;		/* where work is answered */
	guint              pending;		/* dispatched, not yet answered */
	gint               stopping;	/* atomic */
	gboolean           stopped;
	gint64             time_offset_us;
	guint              flush_id;
	guint              sweep_id;
	AxisWeb           *web;		/* set by axis_web_auth_install() */
	AxisWorkerTokens  *worker_tokens;	/* borrowed; NULL: the worker API refuses all */
	AxisLoginThrottle *by_worker_address;	/* wrong worker tokens, per client address */
	AxisAuthAccountGoneFunc account_gone;	/* told of an account disabled or deleted; nullable */
	gpointer           account_gone_data;
	/* The portal API's logins (axis-web-auth-portal.c): counts of their own. */
	AxisLoginThrottle *portal_by_user;
	AxisLoginThrottle *portal_by_address;	/* the address the portal reports: only ever a refusal more */
	AxisLoginThrottle *portal_by_device;
	AxisLoginThrottle *portal_by_owner;
	GQueue             portal_failures;	/* gint64*: when each failed login of the last hour was (monotonic) */
	gint64             portal_admitted_us;	/* when a login was last admitted over the hourly ceiling */
	GQueue             portal_waiting;	/* logins waiting for the one hashing place */
	gboolean           portal_hashing;	/* one is being checked */
	void             (*portal_ceiling)(gpointer user_data);	/* told when the hour's failures reach the ceiling; nullable */
	gpointer           portal_ceiling_data;
	gint64             portal_ceiling_told_us;	/* when it was last told (monotonic); 0: never */
	/* The addresses a valid portal token was presented from, as counted
	 * (an IPv6 address by its /64) -> gint64* when last (monotonic): no login
	 * at the engine's own interface from one of them. */
	GHashTable        *portal_addresses;
};

/* Made and freed with the auth; and, as it stops, every login that still
 * waits is answered busy (axis-web-auth-portal.c). */
void
axis_web_auth_portal_init(AxisWebAuth *self);

void
axis_web_auth_portal_stop(AxisWebAuth *self);

void
axis_web_auth_portal_clear(AxisWebAuth *self);

/* What follows an account that goes: see AxisAuthAccountGoneFunc.  Main
 * loop, before the Users page is used; %NULL removes it. */
void
axis_web_auth_set_account_gone(
	AxisWebAuth             *self,
	AxisAuthAccountGoneFunc  func,
	gpointer                 user_data
);

/*
 * What the middleware found out about a request, attached to it for the
 * route checks, the layout and the handlers.  user_id 0 means no valid
 * session.  The strings are copies: the session itself may be revoked
 * while a deferred handler is still working.
 */
typedef struct
{
	gchar        *client_address;
	gchar        *forwarded_host;	/* the public host a trusted proxy reports, or NULL */
	gboolean      https;		/* the client's request was HTTPS */
	gboolean      secure;		/* cookies get Secure */
	gint64        user_id;
	gchar        *username;
	AxisUserRole  role;
	gchar        *theme;		/* the account's chosen theme id, or NULL */
	gchar        *add_defaults;	/* the account's choices for Add from a list (JSON), or NULL */
	gchar        *token;		/* the cookie's token; wiped when freed */
	gchar        *token_hash;
	gchar        *public_id;
	gchar        *csrf;		/* the session's CSRF token */
	/* On the worker API only: the worker whose token the request carried
	 * (0 for none; then user_id is 0 too, whatever the cookies say). */
	gint64        worker_id;
	gchar        *worker_name;
} AxisAuthState;

/* The state the middleware attached to @request, or %NULL. */
const AxisAuthState *
axis_web_auth_state(HtmxRequest *request);

/* The auth serving @request, or %NULL. */
AxisWebAuth *
axis_web_auth_from_request(HtmxRequest *request);

/* Installs the middleware (inside the shell's) and the auth pages. */
void
axis_web_auth_install(
	AxisWebAuth *self,
	AxisWeb     *web,
	HtmxServer  *server
);

/*
 * The route check, run just before a handler: %NULL to go ahead, else
 * the response refusing the request.  @flags are the route's
 * AxisWebRouteFlags.
 */
HtmxResponse *
axis_web_auth_authorize(
	HtmxRequest *request,
	guint        flags
);

/* Whether @path may be requested without a session. */
gboolean
axis_web_auth_is_public_path(const gchar *path);

/* Whether @path, and a route @pattern, are of the worker API (the third
 * class of route, beside public ones and those that need a session). */
gboolean
axis_web_auth_is_worker_path(const gchar *path);

gboolean
axis_web_auth_is_worker_pattern(const gchar *pattern);

/* Whether @authorization (an Authorization header, as a list; nullable) is
 * "Bearer <token>" with a token of a worker that is not revoked. */
gboolean
axis_web_auth_is_worker_token(
	AxisWebAuth *self,
	const gchar *authorization
);

/* A JSON refusal of the worker API (axis_worker_error_to_json()). */
HtmxResponse *
axis_web_auth_worker_error(
	guint        status,
	const gchar *word,
	const gchar *message
);

/* Whether @path (or a route's pattern) is of the portal API
 * (AXIS_PORTAL_API_PREFIX, api/axis-portal-proto.h): answered on the portal
 * listener only.  On this one every such path is one that does not exist,
 * and no route may be registered there. */
gboolean
axis_web_auth_is_portal_path(const gchar *path);

/* A JSON refusal of the portal API (axis_portal_error_to_json()). */
HtmxResponse *
axis_web_auth_portal_error(
	guint        status,
	const gchar *word,
	const gchar *message
);

/* Whether a route @pattern may be registered as public. */
gboolean
axis_web_auth_is_public_pattern(const gchar *pattern);

/* The current time for sessions (wall clock) and for the throttles
 * (monotonic), both moved by the test offset. */
gint64
axis_web_auth_now(AxisWebAuth *self);

gint64
axis_web_auth_now_monotonic(AxisWebAuth *self);

/* Writes "audit: <message>" at info level.  Never pass a secret. */
void
axis_web_auth_audit(
	const gchar *format,
	...
) G_GNUC_PRINTF(1, 2);

/* ── Work off the main loop ───────────────────────────────────── */

/* Runs on a worker thread: may block, must not touch main-loop state. */
typedef void (*AxisAuthWorkFunc)(AxisWebAuth *auth, gpointer data);

/* Runs on the main loop afterwards and builds the answer. */
typedef HtmxResponse *(*AxisAuthDoneFunc)(AxisWebAuth *auth, HtmxRequest *request, gpointer data);

/*
 * Runs @work on a worker, then @done on the main loop, and answers
 * @request with what @done returns.  Returns the deferred response for
 * the handler to return, or at once a 503 when too much work is waiting
 * or the auth has no database or is stopping (then @done is not called).
 * @free_data is called on the main loop in every case.
 */
HtmxResponse *
axis_web_auth_dispatch(
	AxisWebAuth      *self,
	HtmxRequest      *request,
	AxisAuthWorkFunc  work,
	AxisAuthDoneFunc  done,
	gpointer          data,
	GDestroyNotify    free_data
);

/* Runs on the main loop after work that has no request: @skipped when the
 * auth was stopping and the work did not run. */
typedef void (*AxisAuthAfterFunc)(AxisWebAuth *auth, gpointer data, gboolean skipped);

/*
 * Runs @work on a worker, then @after on the main loop.  Returns: %FALSE,
 * having done nothing, when too much work is waiting or the auth has no
 * database or is stopping.
 */
gboolean
axis_web_auth_run(
	AxisWebAuth       *self,
	AxisAuthWorkFunc   work,
	AxisAuthAfterFunc  after,
	gpointer           data
);

/* ── Responses and cookies ────────────────────────────────────── */

/* A 303 to @location, or for htmx a 200 with HX-Redirect. */
HtmxResponse *
axis_web_auth_redirect(
	HtmxRequest *request,
	const gchar *location
);

/* Appends a Set-Cookie header. */
void
axis_web_auth_set_cookie(
	HtmxResponse *response,
	const gchar  *name,
	const gchar  *value,
	gint64        max_age_s,
	gboolean      secure,
	const gchar  *same_site
);

/* The value of cookie @name (in the request's every Cookie header), as
 * axis_cookie_find() decides it. */
gchar *
axis_web_auth_get_cookie(
	HtmxRequest *request,
	const gchar *name
);

/*
 * The pre-session token for a login or setup form: the request's
 * pre-session cookie, or a new one.  The form's CSRF field is
 * axis_token_derive() of it with AXIS_PRESESSION_CSRF_PURPOSE; set it on
 * the response with axis_web_auth_set_presession_cookie() (which also
 * renews its lifetime).  Free with axis_secret_free().
 */
gchar *
axis_web_auth_presession_value(HtmxRequest *request);

void
axis_web_auth_set_presession_cookie(
	HtmxRequest  *request,
	HtmxResponse *response,
	const gchar  *value
);

/*
 * Opens a session for @user_id (under @epoch) and sets its cookie on
 * @response; ends the request's own session if it had one (the token is
 * always new at login) and clears the pre-session cookie.
 */
void
axis_web_auth_begin_session(
	AxisWebAuth  *self,
	HtmxRequest  *request,
	HtmxResponse *response,
	gint64        user_id,
	gint          epoch
);

/* Clears the session cookie on @response. */
void
axis_web_auth_clear_session_cookie(
	HtmxRequest  *request,
	HtmxResponse *response
);

/* ── Remembered devices (web/axis-web-auth-device.c) ──────────── */

/*
 * The throttle key of the remembered device @request presents for
 * @account ("<user id>/<nonce>"), or %NULL when it presents none, or one
 * that is not signed with this daemon's key, is for another account or an
 * earlier epoch of this one (a password change, a disable), is older than
 * AXIS_DEVICE_MAX_AGE, or is not among the account's recorded devices.
 */
gchar *
axis_web_auth_device_of(
	AxisWebAuth    *self,
	HtmxRequest    *request,
	const AxisUser *account
);

/* As axis_web_auth_device_of(), for a device value however it arrived (the
 * portal API carries it in its login). */
gchar *
axis_web_auth_device_of_value(
	AxisWebAuth    *self,
	const gchar    *value,
	const AxisUser *account
);

/* Makes a new device value for @account and records it (transfer full). */
gchar *
axis_web_auth_device_issue(
	AxisWebAuth    *self,
	const AxisUser *account
);

/* Sets a new device cookie for @account on @response (a successful login)
 * and records it, the account's oldest dropped beyond
 * AXIS_DEVICE_MAX_PER_USER. */
void
axis_web_auth_remember_device(
	AxisWebAuth    *self,
	HtmxRequest    *request,
	HtmxResponse   *response,
	const AxisUser *account
);

/* How many devices are recorded for @user_id; for tests. */
guint
axis_web_auth_count_devices(
	AxisWebAuth *self,
	gint64       user_id
);

/* Makes a fresh random device key and an empty record (until they are
 * loaded); and frees them. */
void
axis_web_auth_device_init(AxisWebAuth *self);

void
axis_web_auth_device_clear(AxisWebAuth *self);

/*
 * Called by the middleware for each request that carries X-Forwarded-For
 * while no proxy is trusted, with its peer's address: counts it, and logs
 * the forwarding hint once when AXIS_FORWARDING_HINT_REQUESTS such requests
 * have all come from that one private address.
 */
void
axis_web_auth_note_forwarded(
	AxisWebAuth *self,
	const gchar *peer
);

/*
 * The forwarding hint: one sentence for the log and the administrators'
 * pages when every request with forwarding headers has come from one
 * private address and no proxy is trusted, else %NULL.
 */
gchar *
axis_web_auth_forwarding_hint(AxisWebAuth *self);

/* ── Page parts ───────────────────────────────────────────────── */

/* A hidden input. */
HtmxNode *
axis_web_auth_hidden(
	const gchar *name,
	const gchar *value
);

/* A labelled input in a div.field. */
HtmxNode *
axis_web_auth_field(
	const gchar   *id,
	const gchar   *label,
	const gchar   *name,
	HtmxInputType  type,
	const gchar   *autocomplete,
	const gchar   *value
);

/* A message box: an error (role=alert) or a confirmation. */
HtmxNode *
axis_web_auth_notice(
	gboolean     error,
	const gchar *text
);

/* A submit button. */
HtmxNode *
axis_web_auth_button(
	const gchar *label,
	const gchar *css_class
);

/* A POST form to @action with the session's CSRF field. */
HtmxNode *
axis_web_auth_post_form(
	HtmxRequest *request,
	const gchar *action
);

/* Adds @child to @parent and drops the caller's reference. */
void
axis_web_auth_add(
	gpointer parent,
	gpointer child
);

/* The pages, registered by axis_web_auth_install(). */
void
axis_web_auth_pages_register(
	AxisWebAuth *self,
	AxisWeb     *web
);

void
axis_web_account_register(
	AxisWebAuth *self,
	AxisWeb     *web
);

/* The Reverse proxies page (web/axis-web-auth-proxies.c). */
void
axis_web_auth_proxies_register(
	AxisWebAuth *self,
	AxisWeb     *web
);

G_END_DECLS

#endif /* AXIS_WEB_AUTH_PRIVATE_H */
