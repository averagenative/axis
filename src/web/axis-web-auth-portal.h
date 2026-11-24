/*
 * axis-web-auth-portal.h - what the accounts and sessions do for the portal
 * API: a requester's login, and the session proof
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P2.)  The portal API (api/axis-portal-api.h)
 * holds no account and no session of its own.  It asks here, and what is
 * decided here is decided by the daemon from its own records:
 *
 *  - A login is for an enabled account whose role is requester, and for no
 *    other.  An unknown name, a name that cannot be one, a disabled account,
 *    a wrong password and an account of another role end the same way
 *    (%AXIS_PORTAL_LOGIN_REFUSED), after the same work: one password hash in
 *    each case, against a dummy hash where there is no account.  Nothing is
 *    written to an account by a login (no new hash under new limits either).
 *  - It is throttled as the engine's own login is, on counts of its own (what
 *    happens here delays no login at the engine's interface): the username's
 *    (five free failures, then a doubling delay); the address the portal
 *    reports, which is never checked and only ever adds a refusal; and a
 *    ceiling of %AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR failed logins an hour in
 *    all, beyond which one login a minute is admitted.  A login that presents
 *    a device value the daemon issued to the account at an earlier login is
 *    held to that device's count and to the account's devices' together
 *    instead, and not to the ceiling: no stranger's failures reach it.
 *  - At most one password hash at a time is computed for the portal API, and
 *    at most %AXIS_PORTAL_LOGIN_MAX_WAITING logins wait for it; beyond that a
 *    login is answered %AXIS_PORTAL_LOGIN_BUSY and no hash is begun.  The
 *    engine's own logins keep the other hashing place.
 *  - A session proof is the token of a session of the kind
 *    %AXIS_SESSION_PORTAL (web/axis-session-store.h): random, stored as its
 *    SHA-256 only, valid while its account is an enabled requester under the
 *    epoch it was made in, 30 days after its last use and 90 days after it
 *    was made.  It is no session of the engine's interface, and no session of
 *    the engine's interface is a proof.
 *
 * Main loop thread only.
 */

#ifndef AXIS_WEB_AUTH_PORTAL_H
#define AXIS_WEB_AUTH_PORTAL_H

#include <glib.h>

#include "web/axis-web-auth.h"

G_BEGIN_DECLS

#define AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR 30
#define AXIS_PORTAL_LOGIN_MAX_WAITING      8

typedef enum
{
	AXIS_PORTAL_LOGIN_OK,
	AXIS_PORTAL_LOGIN_REFUSED,	/* the one answer of every failed login */
	AXIS_PORTAL_LOGIN_THROTTLED,	/* not checked: wait */
	AXIS_PORTAL_LOGIN_BUSY		/* not checked: too many wait, or the daemon cannot now */
} AxisPortalLoginStatus;

/**
 * AxisPortalLoginResult:
 * @proof: (nullable): the session proof (only with %AXIS_PORTAL_LOGIN_OK)
 * @name: (nullable): the account's name
 * @expires_in: seconds until the proof ends if it is not used
 * @device: (nullable): the device value for the browser to keep
 * @wait_s: with %AXIS_PORTAL_LOGIN_THROTTLED: how long to wait
 *
 * Borrowed by the callback: copy what is kept.
 */
typedef struct
{
	AxisPortalLoginStatus  status;
	const gchar           *proof;
	const gchar           *name;
	gint64                 expires_in;
	const gchar           *device;
	gint64                 wait_s;
} AxisPortalLoginResult;

typedef void (*AxisPortalLoginFunc)(
	const AxisPortalLoginResult *result,
	gpointer                     user_data
);

/**
 * axis_web_auth_portal_login:
 * @username: what was typed as the username (any text)
 * @password: what was typed as the password; copied, and the copy wiped
 * @device: (nullable): the device value the browser kept
 * @client_address: the client's address as the portal reports it
 * @user_agent: (nullable): the browser's, as the portal reports it
 * @at_once: (out caller-allocates): the answer, when %FALSE is returned
 * @done: called once, later, on the main loop, when %TRUE is returned
 *
 * Returns: %TRUE when the login is being checked (@done will be called);
 *   %FALSE when it was answered at once (throttled or busy) in @at_once,
 *   which holds nothing to free
 */
gboolean
axis_web_auth_portal_login(
	AxisWebAuth            *self,
	const gchar            *username,
	const gchar            *password,
	const gchar            *device,
	const gchar            *client_address,
	const gchar            *user_agent,
	AxisPortalLoginResult  *at_once,
	AxisPortalLoginFunc     done,
	gpointer                user_data
);

/**
 * axis_web_auth_portal_person:
 * @proof: (nullable): what a request presents as its session proof
 * @user_id: (out): the account
 * @name: (out) (transfer full) (optional): its name
 *
 * The person a proof is for, from the proof alone.  Marks the session used.
 *
 * Returns: %TRUE when @proof is the proof of a portal session that is valid
 *   now: its account exists, is enabled, is a requester and is under the
 *   epoch the session was made in, and the session has not ended
 */
gboolean
axis_web_auth_portal_person(
	AxisWebAuth  *self,
	const gchar  *proof,
	gint64       *user_id,
	gchar       **name
);

/* Ends the session @proof is of.  Returns: %TRUE if there was a valid one. */
gboolean
axis_web_auth_portal_logout(
	AxisWebAuth *self,
	const gchar *proof
);

/*
 * (add-request-portal, slice P4, after its review.)  The portal's addresses:
 * server.portal_peers (AXIS_PORTAL_PEERS), a list of addresses and ranges
 * that the administrator writes.  It means two things at once:
 *
 *  - the portal listener answers a connection whose own address (the socket's
 *    peer; that listener reads no forwarding header) is in the list, and no
 *    other: any other peer is refused before its token is looked at, and
 *    counts for nothing.  While the list is empty, it answers nobody;
 *  - no login at the engine's own interface (and no first setup) is taken
 *    when the connection's own address or the client's address, as the
 *    trusted proxies resolve it, is in the list.  Both are asked: a peer that
 *    is a listed proxy as well cannot forward its own address away.  The
 *    answer is that of a wrong password, no password is checked and no count
 *    is touched.  A session that exists is not affected.
 *
 * Nothing is learnt and nothing expires: what a portal does, or a restart of
 * the daemon, changes none of it.  A range is as wide as it is written (an
 * IPv6 address is not widened to its /64); an IPv4-mapped IPv6 address is
 * its IPv4 address.  So a password typed into a portal that is in the wrong
 * hands cannot be used from the portal's own place in the network.
 */

/**
 * axis_web_auth_set_portal_peers:
 * @peers: (nullable): server.portal_peers, as the configuration holds it
 *
 * Before anything is served; main loop.  A host name is refused.
 *
 * Returns: %FALSE with @error set when @peers is not such a list (the list
 *   then stays as it was)
 */
gboolean
axis_web_auth_set_portal_peers(
	AxisWebAuth  *self,
	const gchar  *peers,
	GError      **error
);

/* Whether the list is empty: nobody is the portal.  Any thread. */
gboolean
axis_web_auth_portal_peers_none(AxisWebAuth *self);

/* The list as written, "" when it is empty (transfer full).  Any thread. */
gchar *
axis_web_auth_portal_peers_text(AxisWebAuth *self);

/* Whether @address (text; nullable) is one of the portal's.  Any thread. */
gboolean
axis_web_auth_portal_peer_listed(
	AxisWebAuth *self,
	const gchar *address
);

/**
 * axis_web_auth_portal_peers_overlap:
 * @peer: (out) (transfer full) (optional): the entry of the portal's
 *   addresses
 * @proxy: (out) (transfer full) (optional): the entry of the trusted proxies
 *
 * Whether an address is both a portal's and a trusted proxy's.  Then the
 * proxy administrators come through is refused at the login as the portal
 * is, whoever it forwards: worth a warning.  Any thread.
 */
gboolean
axis_web_auth_portal_peers_overlap(
	AxisWebAuth  *self,
	gchar       **peer,
	gchar       **proxy
);

/**
 * axis_web_auth_portal_bars_login:
 * @peer: (nullable): the connection's own address
 * @client: (nullable): the client's address as the trusted proxies resolve it
 * @what: "login" or "setup", for the log
 *
 * Whether a login from there is refused because one of the two is a
 * portal's address.  When it is, the refusal is counted and the log says so,
 * naming the setting: at once the first time, then once a minute at most,
 * with how many more there were.  Main loop only.
 */
gboolean
axis_web_auth_portal_bars_login(
	AxisWebAuth *self,
	const gchar *peer,
	const gchar *client,
	const gchar *what
);

/* How many logins were refused for that since the daemon started, and when
 * the last one was (@last_us: (out) (optional), microseconds since 1970; 0
 * when never).  Main loop only. */
guint
axis_web_auth_portal_logins_barred(
	AxisWebAuth *self,
	gint64      *last_us
);

/**
 * axis_web_auth_portal_set_ceiling_func:
 * @func: (nullable): called on the main loop when the failed logins of the
 *   last hour are at %AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR or over it, once an
 *   hour at most
 *
 * Who is told that the hourly ceiling was reached (the notice and the page
 * "Request portal").
 */
void
axis_web_auth_portal_set_ceiling_func(
	AxisWebAuth *self,
	void       (*func)(gpointer user_data),
	gpointer     user_data
);

/* How many failed logins through the portal API the last hour holds, for the
 * page "Request portal".  Main loop only. */
guint
axis_web_auth_portal_failures(AxisWebAuth *self);

/* How many logins wait for the hashing place now, and whether one is being
 * checked: for tests. */
guint
axis_web_auth_portal_waiting(
	AxisWebAuth *self,
	gboolean    *hashing
);

G_END_DECLS

#endif /* AXIS_WEB_AUTH_PORTAL_H */
