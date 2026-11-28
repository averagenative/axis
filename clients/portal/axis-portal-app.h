/*
 * axis-portal-app.h - axis-portal's pages: what a browser may ask of it and
 * what it then asks of the daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  The program faces the internet and is
 * built as if it were attacked.  It holds no database connection, no media,
 * no provider's key and no session: a person's session is the proof in that
 * person's cookie, which the daemon judges every time.  What it does hold,
 * in memory only: its token and the key derived from it that seals the
 * session cookies, the counts of its throttles, the key of its pre-login
 * forms, what `whoami` said for at most 30 seconds, which sessions the
 * daemon confirmed in the last 15 minutes, and posters up to a fixed size.
 * It writes no file.
 *
 * One function answers every request (there is no router to add a route
 * to), in this order, and whatever it answers carries the security headers
 * of the engine's interface (web/axis-web-headers.h), with the portal's own
 * stricter content security policy (AXIS_PORTAL_CSP), and over HTTPS
 * Strict-Transport-Security:
 *
 *   1. the path is one of the portal's own, written exactly, or the answer
 *      is the portal's own 404.  Nothing is passed on, ever;
 *   2. the method is one the path takes (405);
 *   3. the stylesheet and the probe (the probe to a loopback address, with a
 *      loopback Host, that forwards for nobody: no forwarding header of any
 *      kind; 404 for anyone else) are answered here;
 *   4. a listed trusted proxy that does not say who the client is (no
 *      X-Forwarded-For with an address that is not a proxy's) is answered
 *      503, and a line of the log says why; the request came over HTTPS, as
 *      a listed trusted proxy reports it, or the answer is a page that says
 *      the portal needs it (unless the test-only override is set).
 *      Forwarding headers from anyone else are ignored;
 *   5. the query and, for a POST, the form are read by axis-portal-form.h or
 *      refused whole (400; a form that is not urlencoded 415); a POST that
 *      the browser reports as coming from another site is refused (403);
 *   6. /login: the form carries a token bound to the cookie set with it, to
 *      the time it was given and to a key only this process has, and given
 *      at most AXIS_PORTAL_FORM_MAX_AGE_S ago (403); a form that cannot be a
 *      username and a password is answered 400 and counted nowhere.  A
 *      login with a device cookie this portal sealed for the username typed
 *      (a remembered browser) spends the calls of remembered browsers, and
 *      each username's share of them (429), and nothing of an address's.
 *      Any other login: failed logins from one address are refused here
 *      (429), and it spends the calls that logins share, from every address
 *      together, and its address's share of them, whatever the daemon then
 *      answers (429);
 *   7. every other path needs the session cookie, once (two are none), and
 *      sealed by this portal: its value is the proof and a keyed hash of the
 *      proof under a key derived from the portal's token, checked in
 *      constant time.  A request without one is sent to /login, and the
 *      daemon is asked nothing.  A session the daemon has not confirmed in
 *      the last 15 minutes is held to its address's count of cookies the
 *      daemon did not know (429), and its first call to the daemon spends
 *      one of the calls that such sessions share, from every address
 *      together (429): a stranger, who cannot seal a cookie, spends none of
 *      them;
 *   8. a POST carries the CSRF token derived from that cookie (403);
 *   9. the route's own values are checked (a search with a query that the
 *      browser reports as from another site is 403), and only then is the
 *      daemon asked, through the eleven operations' own members
 *      (axis-portal-wire.h) and nothing else.  Every call for a session
 *      takes one of the session's own (429), and a session the daemon
 *      refused by the person's budget is held off for the time it said.  A
 *      search or a request post is counted for the session (429) once the
 *      daemon has said whose session it is, before the search or the
 *      request is asked for.
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

#include "axis-portal-client.h"
#include "axis-portal-config.h"

G_BEGIN_DECLS

/* The largest body of a request (the limits answer 413 beyond it). */
#define AXIS_PORTAL_MAX_BODY_BYTES        (64 * 1024)

/* The portal's content security policy: the engine interface's is wider
 * than the portal needs.  Its pages load nothing but its own stylesheet and
 * pictures, have no script at all, and post only to itself. */
#define AXIS_PORTAL_CSP "default-src 'none'; style-src 'self'; img-src 'self'; form-action 'self'; " \
	"frame-ancestors 'none'; base-uri 'none'"

/* Sent with every answer to a visit that came over HTTPS: a browser that has
 * seen it does not open the portal over plain http again for a year. */
#define AXIS_PORTAL_HSTS "max-age=31536000"

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

/*
 * The calls to the daemon that are not a confirmed session's, each in any
 * minute (the generic cell rate algorithm: as many of them at once as that).
 * After the security review (F1), three budgets of their own, so that what
 * a stranger can spend is apart from what a remembered browser and a
 * returning session need:
 *
 *  - logins without a remembered browser's cookie, from every address
 *    together, and of them from one client address (an IPv6 address by its
 *    /64): every call the daemon answers counts, whatever it answers (429
 *    and 503 too), so one address cannot spend the others' for them;
 *  - logins with a device cookie this portal sealed for the username typed:
 *    a stranger cannot make one.  Not held to an address's count of failed
 *    logins or to its share; each username has a share of its own;
 *  - sessions the daemon has not confirmed lately: their cookies are sealed,
 *    so a stranger cannot spend these either.
 *
 * Together 240, under the 600 operations a minute the daemon allows the
 * portal's token: one person (AXIS_PORTAL_PERSON_OPS_A_MINUTE, 240, at the
 * daemon) and all three spent leave 120 for everyone else's confirmed
 * sessions, and strangers alone, who can spend only the first, leave 480.
 */
#define AXIS_PORTAL_LOGINS_A_MINUTE                 120
#define AXIS_PORTAL_LOGINS_A_MINUTE_FROM_ADDRESS    10
#define AXIS_PORTAL_REMEMBERED_A_MINUTE             60
#define AXIS_PORTAL_REMEMBERED_A_MINUTE_FOR_NAME    5
#define AXIS_PORTAL_UNCONFIRMED_A_MINUTE            60

/* Each session's calls to the daemon, in any minute (after the security
 * review, F2 and F5).  A poster the portal holds, for a person the daemon
 * knew within the last 30 seconds, is no call.  A home page whose 20
 * posters the portal does not hold is hello, browse and 20 art, 22 calls,
 * and whoami is asked at most every 30 seconds: ten such pages a minute are
 * 222.  The daemon's budget for a person is as large; a session it refused
 * by that budget is held off here for as long as it said. */
#define AXIS_PORTAL_SESSION_CALLS_A_MINUTE          240

/* The most keys of each table of shares above (an address, a username, a
 * session).  A key whose share is whole again goes when room is needed; a
 * new key waits while there is none. */
#define AXIS_PORTAL_SHARES_MAX_KEYS                 4096

/* Pages waiting for the daemon at once; one more is answered 503.  As many
 * as the calls that may be in flight: a page waits for one at a time. */
#define AXIS_PORTAL_MAX_PENDING           AXIS_PORTAL_CLIENT_MAX_CALLS

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

/* The pre-login form's cookie lives long enough to fill the form in, and
 * its token is refused once it is older (it carries its time inside its
 * keyed hash: after the security review, F3). */
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

/**
 * axis_portal_device_cookie:
 * @token: the portal's token
 * @name: the username the daemon logged in
 * @device: the device value the daemon gave with it
 *
 * Returns: (transfer full): the device cookie's value, as the portal with
 *   @token sets it: "<device>.<seal>", the seal a keyed hash of @device and
 *   of @name as a login's username is read (a secret).  A login whose
 *   username is another name does not count it.
 */
gchar *
axis_portal_device_cookie(
	const gchar *token,
	const gchar *name,
	const gchar *device
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

/* For how many sessions searches and request posts are counted. */
guint
axis_portal_app_get_counted_sessions(AxisPortalApp *self);

G_END_DECLS

#endif /* AXIS_PORTAL_APP_H */
