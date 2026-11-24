/*
 * axis-portal-app.h - axis-portal's pages: what a browser may ask of it and
 * what it then asks of the daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  The program faces the internet and is
 * built as if it were attacked.  It holds no database connection, no media,
 * no provider's key and no session: a person's session is the proof in that
 * person's cookie, which the daemon judges every time.  What it does hold,
 * in memory only: its token, the counts of its throttles, the key of its
 * pre-login forms, what `whoami` said for at most 30 seconds, and posters up
 * to a fixed size.  It writes no file.
 *
 * One function answers every request (there is no router to add a route
 * to), in this order, and whatever it answers carries the security headers
 * of the engine's interface (web/axis-web-headers.h):
 *
 *   1. the path is one of the portal's own, written exactly, or the answer
 *      is the portal's own 404.  Nothing is passed on, ever;
 *   2. the method is one the path takes (405);
 *   3. the stylesheet and the probe (the probe to a loopback address that
 *      forwards for nobody; 404 for anyone else) are answered here;
 *   4. the request came over HTTPS, as a listed trusted proxy reports it,
 *      or the answer is a page that says the portal needs it (unless the
 *      test-only override is set).  Forwarding headers from anyone else are
 *      ignored;
 *   5. the query and, for a POST, the form are read by axis-portal-form.h or
 *      refused whole (400; a form that is not urlencoded 415); a POST that
 *      the browser reports as coming from another site is refused (403);
 *   6. /login: the form carries a token bound to the cookie set with it and
 *      to a key only this process has (403); failed logins from one address
 *      are refused here, before the daemon is asked (429);
 *   7. every other path needs the session cookie, once (two are none), and
 *      sealed by this portal: its value is the proof and a keyed hash of the
 *      proof under a key derived from the portal's token, checked in
 *      constant time.  A request without one is sent to /login, and the
 *      daemon is asked nothing.  A session the daemon has not confirmed in
 *      the last 15 minutes is held to its address's count of cookies the
 *      daemon did not know (429), and its first call to the daemon spends
 *      one of the calls that such sessions and logins share, from every
 *      address together (429);
 *   8. a POST carries the CSRF token derived from that cookie (403);
 *   9. the route's own values are checked, its limit for the session is
 *      counted (429), and only then is the daemon asked, through the eleven
 *      operations' own members (axis-portal-wire.h) and nothing else.
 *
 * What the daemon answers is read by the strict readers of
 * axis-portal-wire.h and written by axis-portal-view.h; a failure of the
 * daemon, or an answer that cannot be read, is one plain page (503) that
 * says nothing of what went wrong.  The daemon's own sentences are never
 * shown.
 *
 * Main loop only.
 */

#ifndef AXIS_PORTAL_APP_H
#define AXIS_PORTAL_APP_H

#include <glib.h>
#include <htmx-glib.h>

#include "axis-portal-config.h"

G_BEGIN_DECLS

/* The largest body of a request (the limits answer 413 beyond it). */
#define AXIS_PORTAL_MAX_BODY_BYTES        (64 * 1024)

/* What `whoami` said is kept this long, to draw a page's header. */
#define AXIS_PORTAL_WHO_SECONDS           30
#define AXIS_PORTAL_WHO_MAX_KEPT          4096

/* Posters kept in memory: this much in all, each this long. */
#define AXIS_PORTAL_POSTER_CACHE_BYTES    (32 * 1024 * 1024)
#define AXIS_PORTAL_POSTER_SECONDS        600

/* A session's own limits, each in any minute. */
#define AXIS_PORTAL_SEARCHES_A_MINUTE_HERE 10
#define AXIS_PORTAL_ASKS_A_MINUTE_HERE     10
#define AXIS_PORTAL_WINDOW_MAX_KEYS        4096

/* Failed logins from one client address (an IPv6 address by its /64): the
 * free failures and the delays of the engine's own address throttle. */
#define AXIS_PORTAL_LOGIN_FREE_FAILURES   20
/* Session cookies the daemon did not know, from one client address: the
 * same.  A stranger cannot make the portal ask the daemon without end. */
#define AXIS_PORTAL_PROOF_FREE_FAILURES   20
#define AXIS_PORTAL_THROTTLE_BASE_US      (G_GINT64_CONSTANT(2) * G_USEC_PER_SEC)
#define AXIS_PORTAL_THROTTLE_MAX_US       (G_GINT64_CONSTANT(15) * 60 * G_USEC_PER_SEC)
#define AXIS_PORTAL_THROTTLE_FORGET_US    (G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC)
#define AXIS_PORTAL_THROTTLE_MAX_KEYS     10000
/* A full table drops no address that has failures: the addresses it has no
 * room for share one count (axis_login_throttle_share_when_full()), which
 * is forgotten after this long without a failure. */
#define AXIS_PORTAL_THROTTLE_SHARED_FORGET_US AXIS_PORTAL_THROTTLE_MAX_US

/* Sessions the daemon confirmed (it answered a call with the proof): kept
 * this long, at most this many.  A cookie of one is not held to its
 * address's count of cookies the daemon did not know, and does not spend
 * the calls below. */
#define AXIS_PORTAL_CONFIRMED_SECONDS     (15 * 60)
#define AXIS_PORTAL_CONFIRMED_MAX_KEPT    4096

/* Calls to the daemon for sessions it has not confirmed, and logins, from
 * every address together, in any minute: well under the 600 operations a
 * minute the daemon allows the portal's token, so that the sessions it has
 * confirmed keep the rest whoever sends what. */
#define AXIS_PORTAL_UNCONFIRMED_A_MINUTE  120

/* Pages waiting for the daemon at once; one more is answered 503. */
#define AXIS_PORTAL_MAX_PENDING           256

/* The cookies.  The __Host- prefix makes a browser refuse one of that name
 * set by any other host or without Secure and Path=/; the names without it
 * are for the test-only plain-http override. */
#define AXIS_PORTAL_SESSION_COOKIE        "__Host-axis_portal_session"
#define AXIS_PORTAL_FORM_COOKIE           "__Host-axis_portal_form"
#define AXIS_PORTAL_DEVICE_COOKIE         "__Host-axis_portal_device"
#define AXIS_PORTAL_SESSION_COOKIE_PLAIN  "axis_portal_session"
#define AXIS_PORTAL_FORM_COOKIE_PLAIN     "axis_portal_form"
#define AXIS_PORTAL_DEVICE_COOKIE_PLAIN   "axis_portal_device"

/* What the key that seals the session cookies is derived from the token
 * for (axis_token_derive()): a fixed label, so that the token itself is
 * never a key.  Another token is another key, and every cookie sealed with
 * the old one is none. */
#define AXIS_PORTAL_SEAL_KEY_PURPOSE      "axis portal cookie v1"

/* The pre-login form's cookie lives long enough to fill the form in. */
#define AXIS_PORTAL_FORM_MAX_AGE_S        1800
#define AXIS_PORTAL_SESSION_MAX_AGE_S     (90 * 24 * 3600)
#define AXIS_PORTAL_DEVICE_MAX_AGE_S      (90 * 24 * 3600)

typedef struct _AxisPortalApp AxisPortalApp;

/**
 * axis_portal_app_new:
 * @config: the configuration, with its token
 * @error: (nullable): why not
 *
 * Returns: (transfer full) (nullable): the application; it serves nothing
 *   until axis_portal_app_install()
 */
AxisPortalApp *
axis_portal_app_new(
	const AxisPortalConfig  *config,
	GError                 **error
);

/* Installs the request limits and the one function that answers every
 * request on @server, which must not listen yet. */
void
axis_portal_app_install(
	AxisPortalApp *self,
	HtmxServer    *server
);

/* Answers nothing new from now on (503) and waits for the pages in flight. */
void
axis_portal_app_stop(AxisPortalApp *self);

void
axis_portal_app_free(AxisPortalApp *self);

/**
 * axis_portal_session_cookie:
 * @token: the portal's token
 * @proof: a session proof the daemon gave
 *
 * Returns: (transfer full): the session cookie's value for @proof, as the
 *   portal with @token sets it: "<proof>.<seal>" (a secret)
 */
gchar *
axis_portal_session_cookie(
	const gchar *token,
	const gchar *proof
);

/* ── For tests ────────────────────────────────────────────────── */

/* Moves the clock of the throttles, the limits and what is kept. */
void
axis_portal_app_set_time_offset(
	AxisPortalApp *self,
	gint64         offset_us
);

/* How many bytes of posters are kept, and for how many people `whoami` is. */
gsize
axis_portal_app_get_poster_bytes(AxisPortalApp *self);

guint
axis_portal_app_get_who_kept(AxisPortalApp *self);

/* For how many sessions the daemon's confirmation is kept. */
guint
axis_portal_app_get_confirmed_kept(AxisPortalApp *self);

G_END_DECLS

#endif /* AXIS_PORTAL_APP_H */
