/*
 * axis-portal-api.h - the portal API: the eleven operations a request portal
 * may ask of the daemon, on a listener of their own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P2.)  A request portal (clients/portal, a later
 * slice) is the one piece of Axis meant to face the internet, so this API is
 * written for a caller that may be in the wrong hands.  What holds here holds
 * in the daemon, whatever the caller does:
 *
 *  - The API is answered by a server of its own (axis_portal_api_install()),
 *    whose router holds the eleven operations and nothing else: no page, no
 *    static file, no login form, no worker API, no probe, no metrics.  The
 *    daemon's main listener answers every path under AXIS_PORTAL_API_PREFIX
 *    as one that does not exist (web/axis-web-auth.c).
 *  - The gate, for every request, in this order: the connection's own
 *    address is one of the portal's (server.portal_peers,
 *    web/axis-web-auth-portal.h): while that list is empty everyone is
 *    answered 503, and an address outside it 403, before its token is looked
 *    at and with nothing counted; a path that is not under
 *    the prefix does not exist (404); the daemon serves (503 while it does
 *    not hold its database); no query string (400, whatever it carries: a
 *    token is taken from the Authorization header alone); a portal token:
 *    without a valid one, the client's address is not throttled for wrong
 *    tokens (429) and the request is refused (401, a wrong token counted
 *    against the address), while a valid token is never held to its
 *    address's count; the token's budget of operations (429); nothing a
 *    browser sends (Origin, Sec-Fetch-Site, Sec-Fetch-Mode or Cookie: 403);
 *    POST (405); a JSON body (415) of at most AXIS_PORTAL_MAX_REQUEST_BYTES
 *    (413); an operation that exists (404).  Every refusal is JSON; none
 *    redirects, none sets a cookie.  The client's address is the
 *    connection's own: forwarding headers are not read here.
 *  - A portal token alone acts as no one.  Every operation but hello and
 *    login needs the session proof of the person it is for
 *    (web/axis-web-auth-portal.h), checked before anything else of the body:
 *    a proof that is missing, malformed, unknown or ended is answered 401
 *    with one body.  The person is taken from the proof and from nothing
 *    else: no operation has a member that names a person, and a body with a
 *    member its operation does not define is refused whole (422).
 *  - Every operation checks its object itself: a request is the caller's or
 *    does not exist (404, the same answer as for an identifier never issued);
 *    a title its provider marks as adult, or whose mark is not known yet, or
 *    of a kind that cannot be asked for now, does not exist either.
 *  - An answer carries the members api/axis-portal-proto.h writes and no
 *    others: nothing of a file, a path, a release, an indexer, a download
 *    client, a quality profile, a library folder, a job, a setting, the
 *    daemon's version, a role, or an account other than as "asked_by" while
 *    requests.show_who_asked is on, and then only a requester's name (never
 *    that of an account that can log in to the engine's interface).
 *  - The daemon's own limits, counted here from its own records and clocks:
 *    %AXIS_PORTAL_TOKEN_OPS_A_MINUTE operations of any kind a minute for a
 *    token, and %AXIS_PORTAL_PERSON_OPS_A_MINUTE for a person (every
 *    operation that carries a valid proof, counted before its body is
 *    judged); the login's (web/axis-web-auth-portal.h);
 *    %AXIS_PORTAL_SEARCHES_A_MINUTE searches a minute for a person and
 *    %AXIS_PORTAL_SEARCHES_IN_ALL in all that ask the provider, and the same
 *    search answered from memory for %AXIS_PORTAL_SEARCH_CACHE_S; each
 *    person's quota, the portal's daily ceiling and one person's share of it
 *    (requests/axis-requests.h).  A refusal by a limit is 429 with the time
 *    to wait, and does nothing.  The budgets are kept in memory, at most
 *    %AXIS_PORTAL_BUDGET_MAX_KEYS keys each: a key whose budget is whole
 *    again is forgotten when room is needed, and a new key is refused while
 *    there is none.
 *
 * Each operation that reads the database runs on a thread pool of the API's
 * own, %AXIS_PORTAL_API_THREADS threads, so that it never holds a thread the
 * engine's own pages use; at most %AXIS_PORTAL_API_MAX_PENDING are running
 * or waiting for a thread at once (503 beyond).  Main loop thread, except
 * where said.
 */

#ifndef AXIS_PORTAL_API_H
#define AXIS_PORTAL_API_H

#include <glib.h>
#include <htmx-glib.h>

#include "api/axis-portal-proto.h"
#include "api/axis-portal-tokens.h"
#include "db/axis-db-pool.h"
#include "discover/axis-discover.h"
#include "meta/axis-meta.h"
#include "requests/axis-requests.h"
#include "web/axis-web-auth.h"

G_BEGIN_DECLS

#define AXIS_PORTAL_API_MAX_PENDING    32
#define AXIS_PORTAL_API_THREADS        4
#define AXIS_PORTAL_TOKEN_OPS_A_MINUTE  600
#define AXIS_PORTAL_PERSON_OPS_A_MINUTE 120
#define AXIS_PORTAL_BUDGET_MAX_KEYS    4096
#define AXIS_PORTAL_SEARCHES_A_MINUTE  10
#define AXIS_PORTAL_SEARCHES_IN_ALL    30
#define AXIS_PORTAL_SEARCH_CACHE_S     600
#define AXIS_PORTAL_SEARCH_CACHE_MAX   256

/* Wrong tokens: an address gets this many before it waits. */
#define AXIS_PORTAL_TOKEN_FREE_FAILURES 10

/* How many titles and how many requests one page of an answer holds. */
#define AXIS_PORTAL_TITLES_A_PAGE      20
#define AXIS_PORTAL_REQUESTS_A_PAGE    50

/* The largest poster the art operation answers with. */
#define AXIS_PORTAL_ART_MAX_BYTES      (4 * 1024 * 1024)

typedef struct _AxisPortalApi AxisPortalApi;

/**
 * axis_portal_api_new:
 * @pool: the database; must outlive the API
 * @auth: the accounts and sessions (borrowed; must outlive the API's requests)
 * @tokens: the portal's tokens (borrowed; must outlive the API)
 * @requests: the requests layer (referenced)
 * @meta: the metadata layer (referenced)
 * @discover: (nullable): Discover's stored lists (referenced); without it
 *   there is nothing to browse
 */
AxisPortalApi *
axis_portal_api_new(
	AxisDbPool       *pool,
	AxisWebAuth      *auth,
	AxisPortalTokens *tokens,
	AxisRequests     *requests,
	AxisMeta         *meta,
	AxisDiscover     *discover
);

/**
 * axis_portal_api_install:
 * @server: a server that is the portal listener's alone, before it starts
 * @max_connections: the most connections open at once (0: the default of
 *   web/axis-web-limits.h)
 *
 * Installs the connection and body limits, the gate and the eleven routes on
 * @server.  Nothing else must be installed on it.
 */
void
axis_portal_api_install(
	AxisPortalApi *self,
	HtmxServer    *server,
	guint          max_connections
);

/**
 * AxisPortalApiServingFunc:
 *
 * Whether the daemon serves now (it holds its database).  While it does not,
 * every request to the API is answered 503.  Any thread.
 */
typedef gboolean (*AxisPortalApiServingFunc)(gpointer user_data);

void
axis_portal_api_set_serving(
	AxisPortalApi            *self,
	AxisPortalApiServingFunc  func,
	gpointer                  user_data
);

/* Refuses new requests (503) and waits for those being handled.  Runs the
 * default main context while it waits.  Call it before the auth is stopped. */
void
axis_portal_api_stop(AxisPortalApi *self);

void
axis_portal_api_free(AxisPortalApi *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalApi, axis_portal_api_free)

/* ── For tests ────────────────────────────────────────────────── */

/* Moves the clock of the API's own throttles and budgets by @offset_us. */
void
axis_portal_api_set_time_offset(
	AxisPortalApi *self,
	gint64         offset_us
);

/* Stops that clock at @at_us (monotonic, offset included); 0: it runs
 * again. */
void
axis_portal_api_freeze_clock(
	AxisPortalApi *self,
	gint64         at_us
);

/* The clock of the API's own throttles and budgets, now. */
gint64
axis_portal_api_now(AxisPortalApi *self);

/* The operations' names, %NULL-ended: what the router holds. */
const gchar *const *
axis_portal_api_operations(void);

G_END_DECLS

#endif /* AXIS_PORTAL_API_H */
