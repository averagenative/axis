/*
 * axis-web-auth.h - who may use the web interface
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An AxisWebAuth holds the accounts and sessions (axis-session-store.h),
 * the login throttles, the first-run setup token and the threads that
 * hash passwords and talk to the database.  Handing it to axis_web_new()
 * installs, in one middleware and in the shell's route table:
 *
 *  - default deny: a request without a valid session is refused unless
 *    its path is /healthz, /login, /setup, one of the three well-known
 *    icon paths (/favicon.ico, /apple-touch-icon.png,
 *    /apple-touch-icon-precomposed.png) or under /static/ (a page
 *    request is sent to /login?next=..., an htmx request gets 401 with
 *    HX-Redirect), and a route only answers without a session if it was
 *    also registered as public (AXIS_WEB_ROUTE_PUBLIC, allowed for those
 *    paths only);
 *  - CSRF: every request other than GET and HEAD must carry the session's
 *    CSRF token (the X-CSRF-Token header, which htmx sends from the
 *    hx-headers attribute on every page's body, or the _csrf field of a
 *    form), and must not be cross-site by its Origin or Sec-Fetch-Site
 *    header; the login and setup forms carry a token bound to a
 *    short-lived pre-session cookie instead;
 *  - roles: a route registered with AXIS_WEB_ROUTE_ADMIN answers 403 to
 *    anyone but an administrator;
 *  - the pages /login, /logout, /setup, /account and /admin/users;
 *  - a third class of route, the worker API (every path under
 *    AXIS_WORKER_API_PREFIX, api/axis-worker-proto.h), for other machines.
 *    There no cookie is read, so a session grants nothing; a request needs
 *    "Authorization: Bearer <token>" with a worker's token
 *    (api/axis-worker-tokens.h), and is answered in JSON, never with a
 *    redirect or a cookie: 401 without a valid token (each wrong one counts
 *    against the client's address, and an address with too many is
 *    answered 429 for a while without its token being looked at; other
 *    addresses are not affected), then 403 for anything a browser sends
 *    (Origin, Sec-Fetch-*, Cookie), 405 for any method but POST, 415 for
 *    a body that is not application/json, 404 for a path with no route.
 *    A worker's token is never looked at on any other route.
 *
 * Main loop thread only, apart from its own worker threads.
 */

#ifndef AXIS_WEB_AUTH_H
#define AXIS_WEB_AUTH_H

#include <glib.h>

#include "core/axis-config.h"
#include "core/axis-proxy.h"
#include "db/axis-db-pool.h"
#include "web/axis-user-store.h"

G_BEGIN_DECLS

typedef struct _AxisWebAuth AxisWebAuth;

/* Concurrent password hashes; each holds up to 256 MiB. */
#define AXIS_WEB_AUTH_WORKERS 2

/* Requests waiting for a worker beyond this are answered 503. */
#define AXIS_WEB_AUTH_MAX_PENDING 32

/**
 * axis_web_auth_new:
 * @pool: (nullable): the database; %NULL only in tests (see below)
 * @trusted_proxies: server.trusted_proxies, as the configuration holds it
 * @secure_cookies: server.secure_cookies
 * @error: (nullable): return location for an error
 *
 * Loads the accounts and sessions (blocking: call it before the main loop
 * runs) and logs the trusted proxies in effect.  While no account exists
 * it makes a one-time setup token and logs it, at warning level so that
 * it is seen, with the instruction to open /setup.
 *
 * Without a pool nothing is loaded or stored, no setup token is made, and
 * every page that needs the database answers 503: only for tests, which
 * open sessions with axis_web_auth_open_test_session().
 *
 * Returns: (transfer full) (nullable): the auth, %NULL on error
 */
AxisWebAuth *
axis_web_auth_new(
	AxisDbPool         *pool,
	const gchar        *trusted_proxies,
	AxisSecureCookies   secure_cookies,
	GError            **error
);

/**
 * axis_web_auth_new_with_admin:
 * @admin_username: (nullable): setup.admin_username
 * @admin_password: (nullable): setup.admin_password; given with
 *   @admin_username
 *
 * As axis_web_auth_new().  When no account exists and @admin_username is
 * given, it creates that administrator instead of making a setup token
 * (blocking: the password is hashed here): the username and password must
 * pass the same rules as the setup page, else this fails naming the setting
 * and the rule, never the password.  When an account exists, the two are
 * not used and that is logged.
 *
 * Returns: (transfer full) (nullable): the auth, %NULL on error
 */
AxisWebAuth *
axis_web_auth_new_with_admin(
	AxisDbPool         *pool,
	const gchar        *trusted_proxies,
	AxisSecureCookies   secure_cookies,
	const gchar        *admin_username,
	const gchar        *admin_password,
	GError            **error
);

/**
 * axis_web_auth_stop:
 *
 * Call after the server has stopped taking requests and before the pool
 * is freed.  Refuses new work, waits for the workers (work not started
 * yet is skipped), answers what they finished, then writes the session
 * changes still queued.  Blocks; runs the default main context while it
 * waits.
 */
void
axis_web_auth_stop(AxisWebAuth *self);

/* Stops @self if needed, and frees it. */
void
axis_web_auth_free(AxisWebAuth *self);

/* The workers' tokens the worker API checks (borrowed: it must outlive
 * @self's requests).  Until set, every request to the worker API is
 * refused with 401. */
typedef struct _AxisWorkerTokens AxisWorkerTokens;

void
axis_web_auth_set_worker_tokens(
	AxisWebAuth      *self,
	AxisWorkerTokens *tokens
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisWebAuth, axis_web_auth_free)

/**
 * axis_web_auth_load_device_key:
 * @dir_fd: the data directory's descriptor (core/axis-data-dir.h)
 * @error: (nullable): return location for an error
 *
 * Reads the key that signs remembered devices from the file "device-key"
 * beneath @dir_fd, or makes one there (mode 0600) when it is absent or
 * damaged, so that a device stays remembered across restarts.  Blocking:
 * call it at start.  Until it is called, and when it fails (the auth then
 * keeps a key of its own, made at random when it was created), devices
 * are remembered until the daemon stops.
 *
 * Returns: %TRUE when the key is the one in the file
 */
gboolean
axis_web_auth_load_device_key(
	AxisWebAuth  *self,
	gint          dir_fd,
	GError      **error
);

/* The trusted proxies in effect, with the state of their names (borrowed:
 * alive as long as @self). */
AxisProxyList *
axis_web_auth_get_trusted(AxisWebAuth *self);

/* ── For tests ────────────────────────────────────────────────── */

/* Moves the clock the auth uses (sessions and throttles) by @offset_us. */
void
axis_web_auth_set_time_offset(
	AxisWebAuth *self,
	gint64       offset_us
);

/**
 * axis_web_auth_open_test_session:
 *
 * Only for an auth without a pool: adds an account called @username with
 * @role (if there is none) and opens a session for it.
 *
 * Returns: (transfer full): the "name=value" pair for a Cookie header
 */
gchar *
axis_web_auth_open_test_session(
	AxisWebAuth  *self,
	const gchar  *username,
	AxisUserRole  role
);

/*
 * Only for an auth without a pool: sets the theme of the account called
 * @username (which must exist: see axis_web_auth_open_test_session()) to
 * @theme, any text, or %NULL for none.
 */
void
axis_web_auth_set_test_theme(
	AxisWebAuth *self,
	const gchar *username,
	const gchar *theme
);

/* Blocks until the session writer has written everything queued so far. */
void
axis_web_auth_wait_writes(AxisWebAuth *self);

G_END_DECLS

#endif /* AXIS_WEB_AUTH_H */
