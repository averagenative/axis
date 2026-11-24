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

/* (add-request-portal, slice P4.)  An address a valid portal token was
 * presented from is remembered this long after the last time, and at most
 * this many of them (the one unused longest is forgotten for a new one). */
#define AXIS_PORTAL_ADDRESS_REMEMBERED_US (G_GINT64_CONSTANT(24) * 3600 * G_USEC_PER_SEC)
#define AXIS_PORTAL_ADDRESSES_MAX         256

/**
 * axis_web_auth_portal_note_address:
 * @address: (nullable): the address of the connection a valid portal token
 *   arrived on (the portal listener reads no forwarding header)
 *
 * Remembers that @address speaks as a portal.  For
 * %AXIS_PORTAL_ADDRESS_REMEMBERED_US after the last time, a login at the
 * engine's own interface from that address is refused with the answer of a
 * wrong password, and its password is not checked: a password typed into a
 * portal that is in the wrong hands cannot be used from the portal's own
 * place in the network.  A session that exists is not affected.  Kept in
 * memory: a restart of the daemon forgets the addresses until the portal's
 * next request.  Main loop only.
 */
void
axis_web_auth_portal_note_address(
	AxisWebAuth *self,
	const gchar *address
);

/* Whether @address (a client's, as the engine's login sees it) is such an
 * address now.  Main loop only. */
gboolean
axis_web_auth_portal_address_known(
	AxisWebAuth *self,
	const gchar *address
);

/* How many addresses are remembered now.  Main loop only. */
guint
axis_web_auth_portal_addresses(AxisWebAuth *self);

/**
 * axis_web_auth_portal_set_ceiling_func:
 * @func: (nullable): called on the main loop each time the failed logins of
 *   the last hour reach %AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR from below
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
