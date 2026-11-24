/*
 * axis-web-auth-portal.c - what the accounts and sessions do for the portal
 * API: a requester's login, and the session proof
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "web/axis-web-auth-portal.h"

#include <string.h>

#include "web/axis-password.h"
#include "web/axis-web-auth-input.h"
#include "web/axis-web-auth-private.h"

/* Beyond the hourly ceiling a login is admitted this often. */
#define ADMIT_EVERY_US (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)
#define HOUR_US        (G_GINT64_CONSTANT(3600) * G_USEC_PER_SEC)

typedef struct
{
	AxisWebAuth         *auth;
	gchar               *username;	/* normalised; NULL if it cannot be one */
	gchar               *password;
	gchar               *address;	/* as reported, for the session and the audit */
	gchar               *address_key;	/* as counted */
	gchar               *user_agent;
	gchar               *device;	/* the remembered device's key, or NULL */
	gchar               *owner;	/* with it, the account's ceiling's key */
	AxisPortalLoginFunc  done;
	gpointer             user_data;
	/* The worker's findings. */
	AxisUser            *user;
	gboolean             match;
	GError              *error;
} Login;

static void
login_free(Login *login)
{
	g_free(login->username);
	axis_secret_free(login->password);
	g_free(login->address);
	g_free(login->address_key);
	g_free(login->user_agent);
	g_free(login->device);
	g_free(login->owner);
	axis_user_free(login->user);
	g_clear_error(&login->error);
	g_free(login);
}

void
axis_web_auth_portal_init(AxisWebAuth *self)
{
	self->portal_by_user = axis_login_throttle_new(AXIS_AUTH_USER_FREE_FAILURES, AXIS_AUTH_THROTTLE_BASE_US,
		AXIS_AUTH_THROTTLE_MAX_US, AXIS_AUTH_THROTTLE_FORGET_US, AXIS_AUTH_THROTTLE_MAX_KEYS);
	self->portal_by_address = axis_login_throttle_new(AXIS_AUTH_ADDRESS_FREE_FAILURES, AXIS_AUTH_THROTTLE_BASE_US,
		AXIS_AUTH_THROTTLE_MAX_US, AXIS_AUTH_THROTTLE_FORGET_US, AXIS_AUTH_THROTTLE_MAX_KEYS);
	self->portal_by_device = axis_login_throttle_new(AXIS_AUTH_USER_FREE_FAILURES, AXIS_AUTH_THROTTLE_BASE_US,
		AXIS_AUTH_THROTTLE_MAX_US, AXIS_AUTH_THROTTLE_FORGET_US, AXIS_AUTH_THROTTLE_MAX_KEYS);
	self->portal_by_owner = axis_login_throttle_new(AXIS_DEVICE_OWNER_FREE_FAILURES, AXIS_AUTH_THROTTLE_BASE_US,
		AXIS_AUTH_THROTTLE_MAX_US, AXIS_AUTH_THROTTLE_FORGET_US, AXIS_AUTH_THROTTLE_MAX_KEYS);
	g_queue_init(&self->portal_failures);
	g_queue_init(&self->portal_waiting);
	/* Nobody's, until the configuration says whose. */
	self->portal_peers = axis_proxy_list_parse("", NULL);
}

void
axis_web_auth_portal_clear(AxisWebAuth *self)
{
	axis_login_throttle_free(self->portal_by_user);
	axis_login_throttle_free(self->portal_by_address);
	axis_login_throttle_free(self->portal_by_device);
	axis_login_throttle_free(self->portal_by_owner);
	g_queue_clear_full(&self->portal_failures, g_free);
	g_queue_clear_full(&self->portal_waiting, (GDestroyNotify)login_free);
	g_clear_pointer(&self->portal_peers, axis_proxy_list_free);
}

/* ── The portal's addresses ─────────────────────────────────────── */

gboolean
axis_web_auth_set_portal_peers(
	AxisWebAuth  *self,
	const gchar  *peers,
	GError      **error
){
	AxisProxyList *list;

	g_return_val_if_fail(self != NULL, FALSE);

	list = axis_proxy_list_parse(peers != NULL ? peers : "", error);
	if (list == NULL)
		return FALSE;
	if (axis_proxy_list_has_names(list)) {
		g_set_error_literal(error, AXIS_PROXY_ERROR, AXIS_PROXY_ERROR_INVALID,
			"the portal's addresses are IP addresses and CIDR ranges only, never a host name");
		axis_proxy_list_free(list);
		return FALSE;
	}
	axis_proxy_list_free(self->portal_peers);
	self->portal_peers = list;
	return TRUE;
}

gboolean
axis_web_auth_portal_peers_none(AxisWebAuth *self)
{
	g_return_val_if_fail(self != NULL, TRUE);

	return axis_proxy_list_get_length(self->portal_peers) == 0;
}

gchar *
axis_web_auth_portal_peers_text(AxisWebAuth *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return axis_proxy_list_to_string(self->portal_peers);
}

gboolean
axis_web_auth_portal_peer_listed(
	AxisWebAuth *self,
	const gchar *address
){
	AxisIpAddress parsed;

	g_return_val_if_fail(self != NULL, FALSE);

	/* (The parser folds an IPv4-mapped IPv6 address to its IPv4 address,
	 * and the list was read by the same parser.) */
	return address != NULL && axis_ip_address_parse(address, &parsed) &&
		axis_proxy_list_contains(self->portal_peers, &parsed);
}

gboolean
axis_web_auth_portal_peers_overlap(
	AxisWebAuth  *self,
	gchar       **peer,
	gchar       **proxy
){
	g_return_val_if_fail(self != NULL, FALSE);

	return axis_proxy_lists_overlap(self->portal_peers, self->trusted, peer, proxy);
}

/* Why a login is not taken. */
typedef enum
{
	BAR_NONE,
	BAR_PEER,		/* the connection's own address is listed */
	BAR_CLIENT,		/* the client the trusted proxies name is listed */
	BAR_FORWARDED,		/* an address in X-Forwarded-For is listed */
	BAR_LINK_LOCAL,		/* one of them is an IPv6 link-local address */
	BAR_UNRESOLVED		/* forwarded, while a trusted proxy's name stands for no address */
} Bar;

/* Whether @text is an address of the kind @link_local asks for: a listed
 * one, or an IPv6 link-local one.  A zone is dropped ("fe80::1%eth0"), which
 * is how a proxy writes a client that reached it over such an address. */
static gboolean
address_is(
	AxisWebAuth *self,
	const gchar *text,
	gboolean     link_local
){
	AxisIpAddress parsed;

	if (!axis_ip_address_parse_zoned(text, &parsed))
		return FALSE;
	return link_local ? axis_ip_address_is_link_local6(&parsed) :
		axis_proxy_list_contains(self->portal_peers, &parsed);
}

/* An address as the log shows it: read and written again, so that nothing
 * of a header's own text (a zone is any text) reaches the log. */
static gchar *
address_text(const gchar *text)
{
	AxisIpAddress parsed;

	if (!axis_ip_address_parse_zoned(text, &parsed))
		return g_strdup(AXIS_PROXY_UNKNOWN_CLIENT);
	return axis_ip_address_to_string(&parsed);
}

/*
 * The rule itself.  @which is set to the address the refusal is for.
 *
 * Every address of X-Forwarded-For is read, from whoever sent the header and
 * however far the trusted proxies' walk went: a sender can add entries in
 * front of the one its proxy writes and can take none away, so the portal's
 * address is in the header whatever the portal put before it, whether or not
 * the proxy that wrote it is trusted (yet), and whether the walk stopped
 * before it or stepped over it.  The only login a made-up entry can stop is
 * its sender's own.  An entry that is no address says nothing.
 */
static Bar
login_bar(
	AxisWebAuth  *self,
	const gchar  *peer,
	const gchar  *client,
	const gchar  *forwarded_for,
	gchar       **which
){
	g_auto(GStrv) hops = NULL;
	AxisIpAddress from;
	gboolean link_local;
	guint i;

	*which = NULL;
	/* Nobody is the portal: nothing of this applies. */
	if (axis_proxy_list_get_length(self->portal_peers) == 0)
		return BAR_NONE;

	if (forwarded_for != NULL) {
		hops = g_strsplit(forwarded_for, ",", -1);
		for (i = 0; hops[i] != NULL; i++)
			g_strstrip(hops[i]);
	}

	/* First the listed addresses, then the link-local ones: an IPv6
	 * link-local address is one an interface gives itself, on the portal's
	 * networks as on any other, and no list of the portal's addresses can
	 * name it.  It is never the browser of an administrator who comes through
	 * a proxy from elsewhere; with a portal configured it is taken for the
	 * portal's. */
	for (link_local = FALSE; ; link_local = TRUE) {
		if (address_is(self, peer, link_local)) {
			*which = address_text(peer);
			return link_local ? BAR_LINK_LOCAL : BAR_PEER;
		}
		if (address_is(self, client, link_local)) {
			*which = address_text(client);
			return link_local ? BAR_LINK_LOCAL : BAR_CLIENT;
		}
		for (i = 0; hops != NULL && hops[i] != NULL; i++) {
			if (address_is(self, hops[i], link_local)) {
				*which = address_text(hops[i]);
				return link_local ? BAR_LINK_LOCAL : BAR_FORWARDED;
			}
		}
		if (link_local)
			break;
	}

	/* A trusted proxy that is named (trusted_proxies: traefik) is not
	 * believed until its name has an address, and no longer once the name has
	 * failed past its grace.  A login that arrives with a forwarding header
	 * from an address that is not trusted may then be that proxy's, carrying
	 * a client nobody can check: it is not taken.  (The listed addresses in
	 * the header were refused above already; this is for what the header does
	 * not say.) */
	if (forwarded_for != NULL && axis_proxy_list_has_unresolved_name(self->trusted) &&
	    !(axis_ip_address_parse(peer, &from) && axis_proxy_list_contains(self->trusted, &from))) {
		*which = g_strdup(peer != NULL ? peer : AXIS_PROXY_UNKNOWN_CLIENT);
		return BAR_UNRESOLVED;
	}

	return BAR_NONE;
}

gboolean
axis_web_auth_portal_bars_login(
	AxisWebAuth *self,
	const gchar *peer,
	const gchar *client,
	const gchar *forwarded_for,
	const gchar *what
){
	g_autofree gchar *which = NULL;
	Bar bar = login_bar(self, peer, client, forwarded_for, &which);
	gint64 now = axis_web_auth_now_monotonic(self);
	g_autofree gchar *more = NULL;

	if (bar == BAR_NONE)
		return FALSE;

	self->portal_refused++;
	self->portal_refused_at = axis_web_auth_now(self);
	/* One line, then one a minute at most: whoever holds the portal can try
	 * as often as it likes, and the log is not its to fill.  The line names
	 * the rule and the setting, so that an administrator who is refused from
	 * their own desk finds out why. */
	if (self->portal_refused_told_us != 0 && now - self->portal_refused_told_us < ADMIT_EVERY_US) {
		self->portal_refused_untold++;
		return TRUE;
	}
	more = self->portal_refused_untold > 0 ? g_strdup_printf(" (and %u more such refusals since this was last said)",
		self->portal_refused_untold) : g_strdup("");
	switch (bar) {
	case BAR_PEER:
		axis_web_auth_audit("%s refused: the connection came from %s, one of the request portal's addresses "
			"(server.portal_peers, AXIS_PORTAL_PEERS), and nobody logs in to Axis itself from those, whatever is "
			"forwarded. The browser was told what a wrong password is told. If this was your own login, reach Axis "
			"from an address that is not in that list, or take the address out of it and restart Axis%s", what, which,
			more);
		break;
	case BAR_CLIENT:
		axis_web_auth_audit("%s refused: the client %s, as a trusted proxy names it, is one of the request portal's "
			"addresses (server.portal_peers, AXIS_PORTAL_PEERS), and nobody logs in to Axis itself from those. The "
			"browser was told what a wrong password is told. If this was your own login, reach Axis from an address "
			"that is not in that list, or take the address out of it and restart Axis%s", what, which, more);
		break;
	case BAR_FORWARDED:
		axis_web_auth_audit("%s refused: X-Forwarded-For names %s, one of the request portal's addresses "
			"(server.portal_peers, AXIS_PORTAL_PEERS), and nobody logs in to Axis itself by way of those, whoever "
			"sent the header and whatever else is in it. The browser was told what a wrong password is told. If this "
			"was your own login, reach Axis by a way that does not pass an address in that list, or take the address "
			"out of it and restart Axis%s", what, which, more);
		break;
	case BAR_LINK_LOCAL:
		axis_web_auth_audit("%s refused: it came from or by way of %s, an IPv6 link-local address (fe80::/10). Such "
			"an address says which link and never which machine, the request portal's container has one unless "
			"IPv6 is switched off in it, and no list can name it: while server.portal_peers (AXIS_PORTAL_PEERS) is "
			"set, nobody logs in to Axis itself from one. The browser was told what a wrong password is told. If "
			"this was your own login, reach Axis by an address that is not link-local%s", what, which, more);
		break;
	case BAR_UNRESOLVED:
	default:
		axis_web_auth_audit("%s refused: it arrived with X-Forwarded-For from %s, which is not a trusted proxy now, "
			"while a host name in server.trusted_proxies (AXIS_TRUSTED_PROXIES) stands for no address (not looked up "
			"yet, or the lookup fails). That may be the proxy, and whom it forwards for cannot be checked against "
			"the request portal's addresses (server.portal_peers, AXIS_PORTAL_PEERS). The browser was told what a "
			"wrong password is told. Try again in a few seconds; if it lasts, see the log's lines about the trusted "
			"proxy's name, or reach Axis directly%s", what, which, more);
		break;
	}
	self->portal_refused_told_us = now;
	self->portal_refused_untold = 0;
	return TRUE;
}

guint
axis_web_auth_portal_logins_barred(
	AxisWebAuth *self,
	gint64      *last_us
){
	g_return_val_if_fail(self != NULL, 0);

	if (last_us != NULL)
		*last_us = self->portal_refused_at;
	return self->portal_refused;
}

void
axis_web_auth_portal_set_ceiling_func(
	AxisWebAuth *self,
	void       (*func)(gpointer user_data),
	gpointer     user_data
){
	self->portal_ceiling = func;
	self->portal_ceiling_data = user_data;
}

/* ── The counts ─────────────────────────────────────────────────── */

/* Forgets the failures older than an hour. */
static void
failures_trim(
	AxisWebAuth *self,
	gint64       now
){
	gint64 *oldest;

	while ((oldest = g_queue_peek_head(&self->portal_failures)) != NULL && now - *oldest >= HOUR_US)
		g_free(g_queue_pop_head(&self->portal_failures));
}

/*
 * The ceiling over all names: 0 when a login may be checked now, else how
 * long until one may.  With the hour's failures at the ceiling, one login is
 * admitted each ADMIT_EVERY_US, until enough of them are older than an hour.
 */
static gint64
ceiling_wait(
	AxisWebAuth *self,
	gint64       now
){
	failures_trim(self, now);
	if (g_queue_get_length(&self->portal_failures) < AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR)
		return 0;
	if (self->portal_admitted_us != 0 && now - self->portal_admitted_us < ADMIT_EVERY_US)
		return ADMIT_EVERY_US - (now - self->portal_admitted_us);
	return 0;
}

static void
failures_add(
	AxisWebAuth *self,
	gint64       now
){
	gint64 *when = g_new(gint64, 1);

	failures_trim(self, now);
	*when = now;
	g_queue_push_tail(&self->portal_failures, when);
	/* The ceiling is reached by the failure that fills the hour, and is
	 * told once an hour for as long as the hour stays full: the ones admitted
	 * beyond it, one a minute, keep it at the ceiling or over it, and a flow
	 * of guesses that goes on for a day is told of in each of its hours, not
	 * in the first alone. */
	if (g_queue_get_length(&self->portal_failures) >= AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR &&
	    (self->portal_ceiling_told_us == 0 || now - self->portal_ceiling_told_us >= HOUR_US)) {
		self->portal_ceiling_told_us = now;
		if (self->portal_ceiling != NULL)
			self->portal_ceiling(self->portal_ceiling_data);
	}
	/* Never more than the ceiling and what could be in flight beyond it. */
	while (g_queue_get_length(&self->portal_failures) > AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR * 4)
		g_free(g_queue_pop_head(&self->portal_failures));
}

guint
axis_web_auth_portal_failures(AxisWebAuth *self)
{
	failures_trim(self, axis_web_auth_now_monotonic(self));
	return g_queue_get_length(&self->portal_failures);
}

static gint64
login_wait(
	Login  *login,
	gint64  now
){
	AxisWebAuth *self = login->auth;

	/* A browser the account logged in from before: its own count and the
	 * account's devices' together, which no stranger's failure fills. */
	if (login->device != NULL)
		return MAX(axis_login_throttle_check(self->portal_by_device, login->device, now),
		           axis_login_throttle_check(self->portal_by_owner, login->owner, now));

	/* Anyone else: the name's count, the ceiling over all names, and the
	 * reported address's, which can only add a refusal. */
	return MAX(MAX(axis_login_throttle_check(self->portal_by_user, login->username, now), ceiling_wait(self, now)),
	           axis_login_throttle_check(self->portal_by_address, login->address_key, now));
}

static void
login_begin(
	Login  *login,
	gint64  now
){
	AxisWebAuth *self = login->auth;

	if (login->device != NULL) {
		axis_login_throttle_begin(self->portal_by_device, login->device, now);
		axis_login_throttle_begin(self->portal_by_owner, login->owner, now);
		return;
	}
	axis_login_throttle_begin(self->portal_by_user, login->username, now);
	axis_login_throttle_begin(self->portal_by_address, login->address_key, now);
	/* Over the ceiling, this is the one admitted for now. */
	if (g_queue_get_length(&self->portal_failures) >= AXIS_PORTAL_LOGIN_FAILURES_AN_HOUR)
		self->portal_admitted_us = now;
}

static void
login_end(Login *login)
{
	AxisWebAuth *self = login->auth;

	if (login->device != NULL) {
		axis_login_throttle_end(self->portal_by_device, login->device);
		axis_login_throttle_end(self->portal_by_owner, login->owner);
		return;
	}
	axis_login_throttle_end(self->portal_by_user, login->username);
	axis_login_throttle_end(self->portal_by_address, login->address_key);
}

static void
login_failed(
	Login  *login,
	gint64  now
){
	AxisWebAuth *self = login->auth;

	if (login->device != NULL) {
		axis_login_throttle_fail(self->portal_by_device, login->device, now);
		axis_login_throttle_fail(self->portal_by_owner, login->owner, now);
		return;
	}
	axis_login_throttle_fail(self->portal_by_user, login->username, now);
	axis_login_throttle_fail(self->portal_by_address, login->address_key, now);
	failures_add(self, now);
}

/* A success clears the counts it was held to, and only those: never the
 * reported address's, nor the hour's failures. */
static void
login_succeeded(Login *login)
{
	AxisWebAuth *self = login->auth;

	if (login->device != NULL) {
		axis_login_throttle_clear(self->portal_by_device, login->device);
		axis_login_throttle_clear(self->portal_by_owner, login->owner);
	} else {
		axis_login_throttle_clear(self->portal_by_user, login->username);
	}
}

/* ── The check ──────────────────────────────────────────────────── */

/* On a worker: the account, and one password hash whatever is found. */
static void
login_work(
	AxisWebAuth *auth,
	gpointer     data
){
	Login *login = data;
	gchar *hash = NULL;

	if (login->username != NULL &&
	    !axis_user_store_find(auth->pool, login->username, &login->user, &hash, &login->error)) {
		/* The database failed: still one hash, so that the time says
		 * nothing of why. */
		(void)axis_password_verify(NULL, login->password);
	} else {
		login->match = axis_password_verify(login->user != NULL ? hash : NULL, login->password);
	}
	axis_secret_free(login->password);
	login->password = NULL;
	axis_secret_free(hash);
}

static void login_start_next(AxisWebAuth *self);

static void
answer(
	Login                 *login,
	AxisPortalLoginStatus  status
){
	AxisPortalLoginResult result;

	memset(&result, 0, sizeof(result));
	result.status = status;
	login->done(&result, login->user_data);
}

/* On the main loop. */
static void
login_after(
	AxisWebAuth *self,
	gpointer     data,
	gboolean     skipped
){
	Login *login = data;
	gint64 now = axis_web_auth_now_monotonic(self);
	const AxisUser *cached = NULL;
	gboolean ok;

	self->portal_hashing = FALSE;
	login_end(login);

	if (skipped || login->error != NULL) {
		if (login->error != NULL)
			g_warning("portal login: the account could not be read: %s", login->error->message);
		answer(login, AXIS_PORTAL_LOGIN_BUSY);
		login_free(login);
		login_start_next(self);
		return;
	}

	/* Only an enabled requester, as the database has it and as this daemon
	 * knows it now (an account deleted or changed a moment ago stays so). */
	ok = login->user != NULL && login->match && !login->user->disabled &&
		login->user->role == AXIS_USER_ROLE_REQUESTER;
	if (ok) {
		cached = axis_session_store_get_user(self->sessions, login->user->id);
		ok = cached != NULL && !cached->disabled && cached->role == AXIS_USER_ROLE_REQUESTER;
	}

	if (ok) {
		AxisPortalLoginResult result;
		gchar *proof = NULL;
		g_autofree gchar *device = NULL;

		login_succeeded(login);
		axis_session_store_create_kind(self->sessions, AXIS_SESSION_PORTAL, login->user->id, login->user->epoch,
			login->address, login->user_agent, axis_web_auth_now(self), &proof);
		device = axis_web_auth_device_issue(self, cached);
		axis_web_auth_audit("portal login: user=%s reported_client=%s%s", cached->username, login->address,
			login->device != NULL ? " (remembered device)" : "");
		memset(&result, 0, sizeof(result));
		result.status = AXIS_PORTAL_LOGIN_OK;
		result.proof = proof;
		result.name = cached->username;
		result.expires_in = (gint64)axis_session_store_get_idle_seconds();
		result.device = device;
		login->done(&result, login->user_data);
		axis_secret_free(proof);
	} else {
		login_failed(login, now);
		/* Never a name that is not an account: it may be a password typed
		 * into the wrong box. */
		if (login->user == NULL)
			axis_web_auth_audit("portal login failed: no such account reported_client=%s", login->address);
		else
			axis_web_auth_audit("portal login failed: user=%s reported_client=%s%s", login->user->username,
				login->address, login->user->role != AXIS_USER_ROLE_REQUESTER ? " (not a requester)" :
				login->user->disabled ? " (account disabled)" : " (wrong password)");
		answer(login, AXIS_PORTAL_LOGIN_REFUSED);
	}
	login_free(login);
	login_start_next(self);
}

/* Gives the hashing place to the login that has waited longest. */
static void
login_start_next(AxisWebAuth *self)
{
	Login *login;

	while (!self->portal_hashing && (login = g_queue_pop_head(&self->portal_waiting)) != NULL) {
		if (axis_web_auth_run(self, login_work, login_after, login)) {
			self->portal_hashing = TRUE;
			return;
		}
		login_end(login);
		answer(login, AXIS_PORTAL_LOGIN_BUSY);
		login_free(login);
	}
}

void
axis_web_auth_portal_stop(AxisWebAuth *self)
{
	Login *login;

	while ((login = g_queue_pop_head(&self->portal_waiting)) != NULL) {
		login_end(login);
		answer(login, AXIS_PORTAL_LOGIN_BUSY);
		login_free(login);
	}
}

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
){
	gint64 now = axis_web_auth_now_monotonic(self);
	const AxisUser *account;
	Login *login;
	gint64 wait;

	g_return_val_if_fail(self != NULL && password != NULL && done != NULL && at_once != NULL, FALSE);

	memset(at_once, 0, sizeof(*at_once));
	login = g_new0(Login, 1);
	login->auth = self;
	login->username = axis_auth_normalise_username(username);
	login->address = g_strdup(client_address != NULL ? client_address : "");
	login->address_key = axis_proxy_address_key(login->address);
	login->user_agent = g_strdup(user_agent);
	login->done = done;
	login->user_data = user_data;
	account = axis_session_store_find_user(self->sessions, login->username);
	/* A device value counts only for a requester's account it was issued to
	 * under the account's epoch now (axis-web-auth-device.c). */
	if (account != NULL && account->role == AXIS_USER_ROLE_REQUESTER)
		login->device = axis_web_auth_device_of_value(self, device, account);
	if (login->device != NULL)
		login->owner = g_strdup_printf("%" G_GINT64_FORMAT, account->id);

	/* Refused before any password work, even when it would be right. */
	wait = login_wait(login, now);
	if (wait > 0) {
		axis_web_auth_audit("portal login refused, too many failures: user=%s reported_client=%s%s",
			account != NULL ? account->username : "-", login->address,
			login->device != NULL ? " (remembered device)" : "");
		at_once->status = AXIS_PORTAL_LOGIN_THROTTLED;
		at_once->wait_s = MAX((wait + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1);
		login_free(login);
		return FALSE;
	}
	/* One hashing place for the portal API, and a bounded wait for it. */
	if (self->pool == NULL || g_atomic_int_get(&self->stopping) ||
	    g_queue_get_length(&self->portal_waiting) >= AXIS_PORTAL_LOGIN_MAX_WAITING) {
		at_once->status = AXIS_PORTAL_LOGIN_BUSY;
		login_free(login);
		return FALSE;
	}

	login->password = g_strdup(password);
	login_begin(login, now);
	g_queue_push_tail(&self->portal_waiting, login);
	login_start_next(self);
	return TRUE;
}

/* ── The proof ──────────────────────────────────────────────────── */

gboolean
axis_web_auth_portal_person(
	AxisWebAuth  *self,
	const gchar  *proof,
	gint64       *user_id,
	gchar       **name
){
	const AxisSession *session;
	const AxisUser *user = NULL;

	g_return_val_if_fail(self != NULL && user_id != NULL, FALSE);

	*user_id = 0;
	if (name != NULL)
		*name = NULL;
	if (proof == NULL)
		return FALSE;
	session = axis_session_store_lookup_kind(self->sessions, AXIS_SESSION_PORTAL, proof, axis_web_auth_now(self),
		&user);
	/* The store's own rule, said again where the person is decided. */
	if (session == NULL || user == NULL || session->kind != AXIS_SESSION_PORTAL || user->disabled ||
	    user->role != AXIS_USER_ROLE_REQUESTER)
		return FALSE;
	*user_id = user->id;
	if (name != NULL)
		*name = g_strdup(user->username);
	return TRUE;
}

gboolean
axis_web_auth_portal_logout(
	AxisWebAuth *self,
	const gchar *proof
){
	const AxisSession *session;
	const AxisUser *user = NULL;
	g_autofree gchar *hash = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (proof == NULL)
		return FALSE;
	session = axis_session_store_lookup_kind(self->sessions, AXIS_SESSION_PORTAL, proof, axis_web_auth_now(self),
		&user);
	/* Only a portal session of a requester is ended here, whatever the store
	 * is given: never a session of the engine's interface. */
	if (session == NULL || user == NULL || session->kind != AXIS_SESSION_PORTAL ||
	    user->role != AXIS_USER_ROLE_REQUESTER)
		return FALSE;
	hash = g_strdup(session->token_hash);
	axis_web_auth_audit("portal logout: user=%s", user->username);
	return axis_session_store_revoke(self->sessions, hash);
}

guint
axis_web_auth_portal_waiting(
	AxisWebAuth *self,
	gboolean    *hashing
){
	g_return_val_if_fail(self != NULL, 0);

	if (hashing != NULL)
		*hashing = self->portal_hashing;
	return g_queue_get_length(&self->portal_waiting);
}
