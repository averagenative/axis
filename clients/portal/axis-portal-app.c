/*
 * axis-portal-app.c - axis-portal's pages: what a browser may ask of it and
 * what it then asks of the daemon
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "axis-portal-app.h"

#include <libsoup/soup.h>
#include <string.h>

#include "axis-portal-client.h"
#include "axis-portal-form.h"
#include "axis-portal-log.h"
#include "axis-portal-view.h"
#include "axis-portal-wire.h"
#include "api/axis-portal-proto.h"
#include "core/axis-proxy.h"
#include "core/axis-proxy-resolver.h"
#include "web/axis-login-throttle.h"
#include "web/axis-secret.h"
#include "web/axis-web-auth-input.h"
#include "web/axis-web-headers.h"
#include "web/axis-web-limits.h"

#define KEEP_CACHE_KEY "axis-portal-keep-cache-policy"

#define MINUTE_US (G_GINT64_CONSTANT(60) * G_USEC_PER_SEC)

/* What the CSRF tokens are derived for (axis_token_derive()). */
#define SESSION_CSRF_PURPOSE "axis portal session csrf v1"
#define FORM_CSRF_PURPOSE    "axis portal login form v1:"
/* What a session cookie's seal is derived for, under the key derived from
 * the token (AXIS_PORTAL_SEAL_KEY_PURPOSE). */
#define SEAL_PURPOSE         "axis portal session cookie seal v1:"

/* The longest `next` that is kept. */
#define NEXT_MAX 512

/* The kinds of line the log has a budget for (axis-portal-log.h): each
 * refusal, each failed login, each failure of the daemon's answer. */
typedef enum
{
	NOTE_LOGIN_ADDRESS,
	NOTE_LOGIN_FAILED,
	NOTE_LOGIN_DAEMON_LIMIT,
	NOTE_PROOF_ADDRESS,
	NOTE_SEARCHES,
	NOTE_ASKS,
	NOTE_PENDING,
	NOTE_DAEMON_LIMIT,
	NOTE_TOKEN,
	NOTE_UNREADABLE,
	NOTE_BUDGET,
	NOTE_PROXY,
	NOTE_CROSS_SITE,
	N_NOTES
} Note;

/* What the line that counts the ones left out calls each kind. */
static const gchar *const note_kinds[N_NOTES] = {
	"login: refused an address with too many failed logins",
	"login: failed",
	"login: refused by the daemon's limit",
	"refused an address with too many session cookies the daemon did not know",
	"search: refused a session with too many searches",
	"request: refused a session with too many requests",
	"too many pages wait for the daemon",
	"the daemon refused a request by one of its limits",
	"the daemon refused the portal's token",
	"the daemon's answer is not one the portal reads",
	"refused: the calls for logins and unconfirmed sessions are spent",
	"refused a visit: the trusted proxy did not say who the client is",
	"refused a post as from another site"
};

/* ── A limit of so many in any minute, for each key ─────────────── */

#define WINDOW_SLOTS 10

G_STATIC_ASSERT(AXIS_PORTAL_SEARCHES_A_MINUTE_HERE <= WINDOW_SLOTS);
G_STATIC_ASSERT(AXIS_PORTAL_ASKS_A_MINUTE_HERE <= WINDOW_SLOTS);

typedef struct
{
	gint64 at[WINDOW_SLOTS];	/* when each of the last ones was, a ring */
	guint  n;
	guint  next;			/* where the next goes: the oldest, when full */
} Hits;

typedef struct
{
	GHashTable *keys;	/* gchar* -> Hits* */
	guint       limit;
} Window;

static void
window_init(
	Window *window,
	guint   limit
){
	window->keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	window->limit = limit;
}

/* Takes one for @key: 0, or how long until one may be taken (and nothing
 * is).  The table never grows past its bound: a key with nothing in the
 * last minute goes when room is needed, and a new key waits while there is
 * none. */
static gint64
window_take(
	Window      *window,
	const gchar *key,
	gint64       now
){
	Hits *hits = g_hash_table_lookup(window->keys, key);

	if (hits == NULL) {
		if (g_hash_table_size(window->keys) >= AXIS_PORTAL_WINDOW_MAX_KEYS) {
			GHashTableIter iter;
			gpointer value;

			g_hash_table_iter_init(&iter, window->keys);
			while (g_hash_table_iter_next(&iter, NULL, &value)) {
				const Hits *old = value;

				if (old->n == 0 || now - old->at[(old->next + window->limit - 1) % window->limit] >= MINUTE_US)
					g_hash_table_iter_remove(&iter);
			}
		}
		if (g_hash_table_size(window->keys) >= AXIS_PORTAL_WINDOW_MAX_KEYS)
			return G_USEC_PER_SEC;
		hits = g_new0(Hits, 1);
		g_hash_table_insert(window->keys, g_strdup(key), hits);
	}
	if (hits->n >= window->limit) {
		gint64 oldest = hits->at[hits->next];

		if (now - oldest < MINUTE_US)
			return MAX(oldest + MINUTE_US - now, 1);
	} else {
		hits->n++;
	}
	hits->at[hits->next] = now;
	hits->next = (hits->next + 1) % window->limit;
	return 0;
}

/* ── The application ────────────────────────────────────────────── */

typedef struct
{
	AxisPortalWho *who;
	gint64         until_us;
} WhoKept;

static void
who_kept_free(gpointer data)
{
	WhoKept *kept = data;

	axis_portal_who_free(kept->who);
	g_free(kept);
}

typedef struct
{
	gint64  id;
	GBytes *bytes;
	gchar  *type;
	gint64  stored_us;
} Poster;

static void
poster_free(gpointer data)
{
	Poster *poster = data;

	g_bytes_unref(poster->bytes);
	g_free(poster->type);
	g_free(poster);
}

struct _AxisPortalApp
{
	AxisPortalClient  *client;
	AxisProxyList     *trusted;
	AxisProxyResolver *resolver;	/* nullable: only with names in the list */
	gboolean           insecure;	/* tests: plain http from a browser is served */
	guint              max_connections;
	guint              max_per_address;
	gchar             *form_key;	/* the key of the pre-login forms: secret, never leaves */
	gchar             *seal_key;	/* the key that seals the session cookies: secret */
	AxisLoginThrottle *logins;	/* failed logins, by client address */
	AxisLoginThrottle *proofs;	/* cookies the daemon did not know, by client address */
	Window             searches;	/* by session */
	Window             asks;
	gint64             unconfirmed_tat;	/* the calls for unconfirmed sessions and logins (GCRA) */
	GHashTable        *who;		/* session key -> WhoKept* */
	GHashTable        *confirmed;	/* session key -> gint64* until when */
	GHashTable        *posters;	/* &Poster.id -> Poster* (owned by the queue) */
	GQueue             poster_order;	/* Poster*, oldest first */
	gsize              poster_bytes;
	guint              pending;	/* pages waiting for the daemon */
	gboolean           stopping;
	gint64             time_offset_us;
	guint              noted[N_NOTES];	/* how many of each kind there were */
	AxisPortalLogBudget lines[N_NOTES];	/* how many were written */
	guint              lines_timer;	/* says the ones left out when a minute ends */
};

static gint64
now_us(AxisPortalApp *self)
{
	return g_get_monotonic_time() + self->time_offset_us;
}

/* Says, for each kind whose minute has ended by @now (every kind, with
 * %G_MAXINT64), how many of its lines were left out. */
static void
notes_flush(
	AxisPortalApp *self,
	gint64         now
){
	guint i;

	for (i = 0; i < N_NOTES; i++) {
		guint due = axis_portal_log_budget_due(&self->lines[i], now);

		if (due > 0)
			g_message("log: %u more lines of the kind \"%s\" were not written in the last minute", due,
				note_kinds[i]);
	}
}

static void notes_arm(AxisPortalApp *self);

static gboolean
on_lines_timer(gpointer user_data)
{
	AxisPortalApp *self = user_data;

	self->lines_timer = 0;
	notes_flush(self, now_us(self));
	notes_arm(self);
	return G_SOURCE_REMOVE;
}

/* When lines are left out, the count is said when their minute ends, even
 * if no other line comes. */
static void
notes_arm(AxisPortalApp *self)
{
	gint64 ends = G_MAXINT64;
	gint64 now = now_us(self);
	guint i;

	if (self->lines_timer != 0 || self->stopping)
		return;
	for (i = 0; i < N_NOTES; i++)
		ends = MIN(ends, axis_portal_log_budget_ends(&self->lines[i]));
	if (ends == G_MAXINT64)
		return;
	self->lines_timer = g_timeout_add((guint)CLAMP((ends - now) / 1000 + 1, 1, 61000), on_lines_timer, self);
}

/* Logs a refusal by a limit, a failed login, or a failure of the daemon's
 * answer, as the specification asks: each one, up to
 * AXIS_PORTAL_LOG_LINES_A_MINUTE of a kind in a minute, and then one line
 * that says how many more there were.  A line holds an address at most,
 * never what was sent. */
static void
note(
	AxisPortalApp *self,
	Note           which,
	const gchar   *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
note(
	AxisPortalApp *self,
	Note           which,
	const gchar   *format,
	...
){
	g_autofree gchar *text = NULL;
	gint64 now = now_us(self);
	va_list args;

	notes_flush(self, now);
	self->noted[which]++;
	if (!axis_portal_log_budget_take(&self->lines[which], now)) {
		notes_arm(self);
		return;
	}
	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);
	g_message("%s", text);
}

/* ── What is known of one request ───────────────────────────────── */

typedef struct
{
	AxisPortalRoute  route;
	gint64           id;		/* a title */
	gchar           *request_id;	/* a request */
	gboolean         post;
	gboolean         htmx;
	gchar           *client;	/* the client's address, as decided once */
	gchar           *client_key;	/* as counted (an IPv6 address by its /64) */
	gboolean         https;
	gboolean         secure;	/* the cookies are Secure and prefixed */
	gchar           *back;		/* the path and query of a page, to come back to (nullable) */
	gchar           *proof;		/* the proof of a session cookie this portal sealed: secret */
	gchar           *session_key;	/* its hash: what the portal counts and keeps by */
	gboolean         confirmed;	/* the daemon answered for it within AXIS_PORTAL_CONFIRMED_SECONDS */
	gboolean         cookie_refused;	/* a session cookie was there, and is none */
	gboolean         cross_site;	/* the browser says another site sent it (Sec-Fetch-Site) */
	gchar           *csrf;		/* the session's CSRF token */
	gchar           *form_cookie;	/* the pre-login form's cookie value, well-formed */
	gchar           *device;	/* the device cookie's value, of the right form: secret */
	gchar           *user_agent;	/* printable, cut */
	AxisPortalForm  *query;
	AxisPortalForm  *form;
} Visit;

static void
visit_clear(Visit *visit)
{
	g_free(visit->request_id);
	g_free(visit->client);
	g_free(visit->client_key);
	g_free(visit->back);
	axis_secret_free(visit->proof);
	g_free(visit->session_key);
	axis_secret_free(visit->csrf);
	axis_secret_free(visit->form_cookie);
	axis_secret_free(visit->device);
	g_free(visit->user_agent);
	axis_portal_form_free(visit->query);
	axis_portal_form_free(visit->form);
	memset(visit, 0, sizeof(*visit));
}

static const gchar *
header_of(
	HtmxRequest *request,
	const gchar *name
){
	SoupServerMessage *message = htmx_request_get_message(request);

	return message != NULL ? soup_message_headers_get_one(soup_server_message_get_request_headers(message), name) :
		NULL;
}

static const gchar *
header_list_of(
	HtmxRequest *request,
	const gchar *name
){
	SoupServerMessage *message = htmx_request_get_message(request);

	return message != NULL ? soup_message_headers_get_list(soup_server_message_get_request_headers(message), name) :
		NULL;
}

/* The value of the only cookie called @name; %NULL when there is none, or
 * more than one (another site on the same domain may have set one of them:
 * neither is believed). */
static gchar *
cookie_of(
	HtmxRequest *request,
	const gchar *name
){
	SoupServerMessage *message = htmx_request_get_message(request);
	SoupMessageHeadersIter iter;
	const gchar *header_name;
	const gchar *value;
	GString *joined = NULL;
	gchar *found;

	if (message == NULL)
		return NULL;
	soup_message_headers_iter_init(&iter, soup_server_message_get_request_headers(message));
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
	axis_secret_free(g_string_free(joined, FALSE));
	return found;
}

/* ── Answers ────────────────────────────────────────────────────── */

static void
set_cookie(
	HtmxResponse *response,
	const gchar  *name,
	const gchar  *value,
	gint64        max_age_s,
	gboolean      secure,
	const gchar  *same_site
){
	gchar *header = g_strdup_printf("%s=%s; Path=/; Max-Age=%" G_GINT64_FORMAT "; HttpOnly; SameSite=%s%s", name,
		value, max_age_s, same_site, secure ? "; Secure" : "");

	htmx_response_append_header(response, "Set-Cookie", header);
	axis_secret_free(header);
}

static const gchar *
session_cookie_name(gboolean secure)
{
	return secure ? AXIS_PORTAL_SESSION_COOKIE : AXIS_PORTAL_SESSION_COOKIE_PLAIN;
}

static const gchar *
form_cookie_name(gboolean secure)
{
	return secure ? AXIS_PORTAL_FORM_COOKIE : AXIS_PORTAL_FORM_COOKIE_PLAIN;
}

static const gchar *
device_cookie_name(gboolean secure)
{
	return secure ? AXIS_PORTAL_DEVICE_COOKIE : AXIS_PORTAL_DEVICE_COOKIE_PLAIN;
}

/* A page.  @html: (transfer full). */
static HtmxResponse *
html_response(
	guint  status,
	gchar *html
){
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, status);
	htmx_response_set_content(response, html);
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	g_free(html);
	return response;
}

/* A page that says one thing, for nobody in particular. */
static HtmxResponse *
say(
	guint        status,
	const gchar *heading,
	const gchar *text
){
	return html_response(status, axis_portal_view_message(NULL, heading, text));
}

static HtmxResponse *
not_found(void)
{
	return say(404, "Not found", "There is no page at this address.");
}

/* The one answer to every failure of the daemon: it says nothing of what
 * went wrong, which is in the log.  @frame: (nullable) the person's, when
 * known. */
static HtmxResponse *
unavailable_in(const AxisPortalFrame *frame)
{
	HtmxResponse *response = html_response(503, axis_portal_view_message(frame, "Requests are not available now",
		"Requests are not available at the moment. Try again in a few minutes."));

	htmx_response_add_header(response, "Retry-After", "30");
	return response;
}

static HtmxResponse *
unavailable(void)
{
	return unavailable_in(NULL);
}

/* "in 40 seconds", "in 3 minutes", "in 2 hours". */
static gchar *
wait_words(gint64 wait_s)
{
	if (wait_s < 120)
		return g_strdup_printf("in %" G_GINT64_FORMAT " seconds", MAX(wait_s, 1));
	if (wait_s < 2 * 3600)
		return g_strdup_printf("in %" G_GINT64_FORMAT " minutes", (wait_s + 59) / 60);
	if (wait_s < 2 * 24 * 3600)
		return g_strdup_printf("in %" G_GINT64_FORMAT " hours", (wait_s + 3599) / 3600);
	return g_strdup_printf("in %" G_GINT64_FORMAT " days", (wait_s + 86399) / 86400);
}

/* A refusal by a limit: 429, with the time to wait.  @frame: (nullable)
 * the person's, when known. */
static HtmxResponse *
too_many_in(
	const AxisPortalFrame *frame,
	const gchar           *what,
	gint64                 wait_s
){
	g_autofree gchar *when = wait_words(MAX(wait_s, 1));
	g_autofree gchar *text = g_strdup_printf("%s Try again %s.", what, when);
	g_autofree gchar *seconds = g_strdup_printf("%" G_GINT64_FORMAT, MAX(wait_s, 1));
	HtmxResponse *response = html_response(429, axis_portal_view_message(frame, "Too many requests", text));

	htmx_response_add_header(response, "Retry-After", seconds);
	return response;
}

static HtmxResponse *
too_many(
	const gchar *what,
	gint64       wait_s
){
	return too_many_in(NULL, what, wait_s);
}

static gint64
seconds_of(gint64 wait_us)
{
	return MAX((wait_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1);
}

/* @candidate if it may be gone to after a login: a path of the portal's own
 * pages, and nothing a browser could read as another site. */
static gchar *
safe_next(const gchar *candidate)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *request_id = NULL;
	AxisPortalRoute route;
	gint64 id = 0;

	if (candidate == NULL || strlen(candidate) > NEXT_MAX || strchr(candidate, '#') != NULL ||
	    !axis_auth_next_is_safe(candidate))
		return NULL;
	path = g_strndup(candidate, strcspn(candidate, "?"));
	route = axis_portal_route_parse(path, &id, &request_id);
	if (route != AXIS_PORTAL_ROUTE_HOME && route != AXIS_PORTAL_ROUTE_SEARCH && route != AXIS_PORTAL_ROUTE_TITLE &&
	    route != AXIS_PORTAL_ROUTE_REQUESTS)
		return NULL;
	return g_strdup(candidate);
}

/* A redirect to one of the portal's own paths: @location is a literal of
 * this file or came through safe_next(). */
static HtmxResponse *
see_other(const gchar *location)
{
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, 303);
	htmx_response_set_content(response, "");
	htmx_response_set_content_type(response, "text/html; charset=utf-8");
	htmx_response_add_header(response, "Location", location);
	return response;
}

/* Sends a request without a session to /login (an htmx request is answered
 * 401).  @ended: the cookie it carried is no session any more, and goes. */
static HtmxResponse *
to_login(
	const Visit *visit,
	gboolean     ended
){
	g_autofree gchar *next = safe_next(visit->back);
	g_autofree gchar *location = NULL;
	HtmxResponse *response;

	if (next != NULL && strcmp(next, "/") != 0) {
		g_autofree gchar *escaped = g_uri_escape_string(next, NULL, FALSE);

		location = g_strconcat("/login?next=", escaped, NULL);
	} else {
		location = g_strdup("/login");
	}
	if (visit->htmx) {
		response = say(401, "Log in", "You are not logged in, or your session has ended.");
		htmx_response_add_header(response, "HX-Redirect", location);
	} else {
		response = see_other(location);
	}
	if (ended)
		set_cookie(response, session_cookie_name(visit->secure), "", 0, visit->secure, "Lax");
	return response;
}

/* The security headers of the engine's interface, and no caching unless the
 * answer has a policy of its own (the stylesheet, a poster). */
static void
finish_headers(HtmxResponse *response)
{
	axis_web_add_security_headers(response);
	if (g_object_get_data(G_OBJECT(response), KEEP_CACHE_KEY) == NULL)
		htmx_response_add_header(response, "Cache-Control", "no-store");
}

static void
keep_cache_policy(
	HtmxResponse *response,
	const gchar  *policy
){
	htmx_response_add_header(response, "Cache-Control", policy);
	g_object_set_data(G_OBJECT(response), KEEP_CACHE_KEY, GINT_TO_POINTER(1));
}

/* ── What is kept: `whoami` for 30 seconds, posters ─────────────── */

static const AxisPortalWho *
who_lookup(
	AxisPortalApp *self,
	const gchar   *session_key
){
	const WhoKept *kept = g_hash_table_lookup(self->who, session_key);

	if (kept == NULL)
		return NULL;
	if (kept->until_us <= now_us(self)) {
		g_hash_table_remove(self->who, session_key);
		return NULL;
	}
	return kept->who;
}

static void
who_drop(
	AxisPortalApp *self,
	const gchar   *session_key
){
	if (session_key != NULL)
		g_hash_table_remove(self->who, session_key);
}

static void
who_keep(
	AxisPortalApp       *self,
	const gchar         *session_key,
	const AxisPortalWho *who
){
	WhoKept *kept;
	gint64 now = now_us(self);

	if (g_hash_table_size(self->who) >= AXIS_PORTAL_WHO_MAX_KEPT) {
		GHashTableIter iter;
		gpointer value;

		g_hash_table_iter_init(&iter, self->who);
		while (g_hash_table_iter_next(&iter, NULL, &value)) {
			if (((const WhoKept *)value)->until_us <= now)
				g_hash_table_iter_remove(&iter);
		}
		/* Still full: nothing here is needed, only saved asking. */
		if (g_hash_table_size(self->who) >= AXIS_PORTAL_WHO_MAX_KEPT)
			g_hash_table_remove_all(self->who);
	}
	kept = g_new0(WhoKept, 1);
	kept->who = axis_portal_who_copy(who);
	kept->until_us = now + (gint64)AXIS_PORTAL_WHO_SECONDS * G_USEC_PER_SEC;
	g_hash_table_replace(self->who, g_strdup(session_key), kept);
}

/* ── Sealed cookies, confirmed sessions, the shared calls ───────── */

/* The seal of @proof under @key: a keyed BLAKE2b, AXIS_TOKEN_LEN
 * characters (secret). */
static gchar *
seal_of(
	const gchar *key,
	const gchar *proof
){
	gchar *purpose = g_strconcat(SEAL_PURPOSE, proof, NULL);
	gchar *seal = axis_token_derive(key, purpose);

	axis_secret_free(purpose);
	return seal;
}

static gchar *
sealed_cookie(
	const gchar *key,
	const gchar *proof
){
	gchar *seal = seal_of(key, proof);
	gchar *value = g_strconcat(proof, ".", seal, NULL);

	axis_secret_free(seal);
	return value;
}

gchar *
axis_portal_session_cookie(
	const gchar *token,
	const gchar *proof
){
	gchar *key;
	gchar *value;

	g_return_val_if_fail(token != NULL && proof != NULL, NULL);

	if (!axis_secret_init())
		return NULL;
	key = axis_token_derive(token, AXIS_PORTAL_SEAL_KEY_PURPOSE);
	value = sealed_cookie(key, proof);
	axis_secret_free(key);
	return value;
}

/* The proof in a session cookie's @value when this portal sealed it, else
 * %NULL: a cookie nobody sealed costs the daemon nothing. */
static gchar *
unsealed_proof(
	AxisPortalApp *self,
	const gchar   *value
){
	gchar *proof = NULL;
	gchar *seal = NULL;
	gchar *expected;
	gboolean ok;

	if (!axis_portal_wire_split_session_cookie(value, &proof, &seal))
		return NULL;
	expected = seal_of(self->seal_key, proof);
	ok = axis_secret_equal(seal, expected);
	axis_secret_free(expected);
	axis_secret_free(seal);
	if (!ok) {
		axis_secret_free(proof);
		return NULL;
	}
	return proof;
}

static gboolean
confirmed_has(
	AxisPortalApp *self,
	const gchar   *session_key
){
	const gint64 *until = g_hash_table_lookup(self->confirmed, session_key);

	if (until == NULL)
		return FALSE;
	if (*until <= now_us(self)) {
		g_hash_table_remove(self->confirmed, session_key);
		return FALSE;
	}
	return TRUE;
}

/* The daemon answered for @session_key.  The table is bounded: when it is
 * full, what has run out goes, then the one that runs out first (it is
 * then only asked of the daemon again). */
static void
confirmed_add(
	AxisPortalApp *self,
	const gchar   *session_key
){
	gint64 now = now_us(self);
	gint64 *until;

	if (session_key == NULL)
		return;
	if (!g_hash_table_contains(self->confirmed, session_key) &&
	    g_hash_table_size(self->confirmed) >= AXIS_PORTAL_CONFIRMED_MAX_KEPT) {
		GHashTableIter iter;
		gpointer key;
		gpointer value;
		gpointer soonest = NULL;
		gint64 soonest_us = G_MAXINT64;

		g_hash_table_iter_init(&iter, self->confirmed);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			if (*(const gint64 *)value <= now) {
				g_hash_table_iter_remove(&iter);
			} else if (*(const gint64 *)value < soonest_us) {
				soonest_us = *(const gint64 *)value;
				soonest = key;
			}
		}
		if (g_hash_table_size(self->confirmed) >= AXIS_PORTAL_CONFIRMED_MAX_KEPT && soonest != NULL)
			g_hash_table_remove(self->confirmed, soonest);
	}
	until = g_new(gint64, 1);
	*until = now + (gint64)AXIS_PORTAL_CONFIRMED_SECONDS * G_USEC_PER_SEC;
	g_hash_table_replace(self->confirmed, g_strdup(session_key), until);
}

static void
confirmed_drop(
	AxisPortalApp *self,
	const gchar   *session_key
){
	if (session_key != NULL)
		g_hash_table_remove(self->confirmed, session_key);
}

/* Takes one of the calls that logins and unconfirmed sessions share, from
 * every address together: 0, or how long until one may be taken (and
 * nothing is).  The generic cell rate algorithm: AXIS_PORTAL_UNCONFIRMED_A_MINUTE
 * in any minute, as many of them at once as that. */
static gint64
unconfirmed_take(AxisPortalApp *self)
{
	const gint64 interval = MINUTE_US / AXIS_PORTAL_UNCONFIRMED_A_MINUTE;
	const gint64 tolerance = MINUTE_US - interval;
	gint64 now = now_us(self);
	gint64 tat = MAX(self->unconfirmed_tat, now);

	if (tat - now > tolerance)
		return tat - now - tolerance;
	self->unconfirmed_tat = tat + interval;
	return 0;
}

static void
poster_remove(
	AxisPortalApp *self,
	Poster        *poster
){
	self->poster_bytes -= g_bytes_get_size(poster->bytes);
	g_hash_table_remove(self->posters, &poster->id);
	g_queue_remove(&self->poster_order, poster);
	poster_free(poster);
}

static const Poster *
poster_lookup(
	AxisPortalApp *self,
	gint64         id
){
	Poster *poster = g_hash_table_lookup(self->posters, &id);

	if (poster == NULL)
		return NULL;
	if (now_us(self) - poster->stored_us >= (gint64)AXIS_PORTAL_POSTER_SECONDS * G_USEC_PER_SEC) {
		poster_remove(self, poster);
		return NULL;
	}
	return poster;
}

static void
poster_keep(
	AxisPortalApp *self,
	gint64         id,
	GBytes        *bytes,
	const gchar   *type
){
	gsize size = g_bytes_get_size(bytes);
	Poster *poster = g_hash_table_lookup(self->posters, &id);

	if (poster != NULL)
		poster_remove(self, poster);
	if (size == 0 || size > AXIS_PORTAL_CLIENT_MAX_ART_BYTES)
		return;
	/* The oldest go until the new one fits under the fixed size. */
	while (self->poster_bytes + size > AXIS_PORTAL_POSTER_CACHE_BYTES && !g_queue_is_empty(&self->poster_order))
		poster_remove(self, g_queue_peek_head(&self->poster_order));
	poster = g_new0(Poster, 1);
	poster->id = id;
	poster->bytes = g_bytes_ref(bytes);
	poster->type = g_strdup(type);
	poster->stored_us = now_us(self);
	g_hash_table_insert(self->posters, &poster->id, poster);
	g_queue_push_tail(&self->poster_order, poster);
	self->poster_bytes += size;
}

/* Whether @bytes begin as an image of @type does: what is served as a
 * picture is one. */
static gboolean
image_is(
	GBytes      *bytes,
	const gchar *type
){
	gsize size = 0;
	const guchar *data = g_bytes_get_data(bytes, &size);

	if (data == NULL || type == NULL)
		return FALSE;
	if (strcmp(type, "image/jpeg") == 0)
		return size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff;
	if (strcmp(type, "image/png") == 0)
		return size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0;
	if (strcmp(type, "image/webp") == 0)
		return size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0;
	return FALSE;
}

static HtmxResponse *
poster_response(
	GBytes      *bytes,
	const gchar *type
){
	HtmxResponse *response = htmx_response_new();

	htmx_response_set_status(response, 200);
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, type);
	/* The browser may keep it, for this person only. */
	keep_cache_policy(response, "private, max-age=600");
	return response;
}

/* ── A page that waits for the daemon ───────────────────────────── */

typedef struct _Page Page;

typedef void (*PageStep)(Page *page, guint status, GBytes *body);

struct _Page
{
	AxisPortalApp    *app;
	HtmxResponse     *deferred;
	Visit             visit;
	PageStep          step;
	gchar            *content_type;	/* of the last answer */
	AxisPortalWho    *who;
	/* What the route's steps keep. */
	AxisPortalHello  *hello;
	AxisPortalKind    kind;
	gchar            *list;
	gchar            *query;
	gint              search_kind;	/* -1: both */
	guint             number;	/* the page asked for */
	AxisPortalAsk     ask;		/* a request for a series: which seasons */
	gint              picked[AXIS_PORTAL_MAX_SEASONS];
	guint             n_picked;
	AxisPortalAsked  *asked;
	const gchar      *notice;
	gchar            *next;		/* login: where to go afterwards */
};

static AxisPortalFrame
frame_of(const Page *page)
{
	AxisPortalFrame frame;

	frame.who = page->who;
	frame.csrf = page->visit.csrf;
	return frame;
}

/* Ends the page with @response (transfer full). */
static void
page_finish(
	Page         *page,
	HtmxResponse *response
){
	AxisPortalApp *self = page->app;

	htmx_response_complete(page->deferred, response);
	g_object_unref(response);
	g_object_unref(page->deferred);
	visit_clear(&page->visit);
	g_free(page->content_type);
	axis_portal_who_free(page->who);
	axis_portal_hello_free(page->hello);
	g_free(page->list);
	g_free(page->query);
	axis_portal_asked_free(page->asked);
	g_free(page->next);
	g_free(page);
	self->pending--;
}

/* A message, with the navigation when the person is known. */
static void
page_say(
	Page        *page,
	guint        status,
	const gchar *heading,
	const gchar *text
){
	AxisPortalFrame frame = frame_of(page);

	page_finish(page, html_response(status, axis_portal_view_message(&frame, heading, text)));
}

static void
on_answer(
	guint        status,
	GBytes      *body,
	const gchar *content_type,
	gpointer     user_data
){
	Page *page = user_data;

	g_free(page->content_type);
	page->content_type = g_strdup(content_type);
	page->step(page, status, body);
}

/* Asks the daemon; @step is given its answer.  @json: (transfer full). */
static void
page_call(
	Page         *page,
	AxisPortalOp  op,
	gchar        *json,
	PageStep      step
){
	page->step = step;
	axis_portal_client_call(page->app->client, op, json, on_answer, page);
}

/* The session's cookie is none the daemon knows: what was kept of it goes,
 * its address is counted, and the person is sent to /login. */
static void
page_session_ended(Page *page)
{
	AxisPortalApp *self = page->app;

	who_drop(self, page->visit.session_key);
	confirmed_drop(self, page->visit.session_key);
	axis_login_throttle_fail(self->proofs, page->visit.client_key, now_us(self));
	page_finish(page, to_login(&page->visit, TRUE));
}

/* What a refusal by one of the daemon's limits is said as. */
static const gchar *
limit_words(AxisPortalRefusedWhy why)
{
	switch (why) {
	case AXIS_PORTAL_REFUSED_QUOTA:
		return "You have reached your limit for now.";
	case AXIS_PORTAL_REFUSED_TOO_MANY:
		return "You have made too many requests in a day.";
	case AXIS_PORTAL_REFUSED_PORTAL_LIMIT:
		return "The limit of requests for today is reached for everyone.";
	case AXIS_PORTAL_REFUSED_PORTAL_SHARE:
		return "You have reached your share of today's requests.";
	case AXIS_PORTAL_REFUSED_SEARCH_LIMIT:
		return "There were too many searches.";
	case AXIS_PORTAL_REFUSED_THROTTLED:
		return "There were too many failed logins.";
	default:
		return "There were too many requests.";
	}
}

/*
 * Whether the daemon answered what the step asked for (200).  Anything else
 * ends the page here, in the portal's own words: the daemon's sentence is
 * never read, and a failure says nothing of itself.
 */
static gboolean
page_answered(
	Page   *page,
	guint   status,
	GBytes *body
){
	AxisPortalApp *self = page->app;
	AxisPortalFrame frame = frame_of(page);
	AxisPortalRefusal refusal;
	const gchar *data = NULL;
	gsize length = 0;
	HtmxResponse *response;

	if (status == 200)
		return TRUE;
	if (body != NULL)
		data = g_bytes_get_data(body, &length);
	if (status == 0 || !axis_portal_wire_read_refusal(data, length, &refusal)) {
		if (status != 0)
			note(self, NOTE_UNREADABLE, "the daemon answered %u with what the portal cannot read", status);
		page_finish(page, unavailable_in(&frame));
		return FALSE;
	}
	if (status == 401 && refusal.why == AXIS_PORTAL_REFUSED_PROOF) {
		page_session_ended(page);
		return FALSE;
	}
	switch (status) {
	case 401:
		note(self, NOTE_TOKEN, "the daemon refused the portal's token: it is wrong, or was revoked on the page "
			"\"Request portal\"");
		page_finish(page, unavailable_in(&frame));
		break;
	case 404:
		page_say(page, 404, "Not found", "There is no such title or request.");
		break;
	case 409:
		page_say(page, 409, "That cannot be done", refusal.why == AXIS_PORTAL_REFUSED_CLOSED ?
			"This title is closed to requests." : refusal.why == AXIS_PORTAL_REFUSED_DECIDED ?
			"The request does not wait any more, so it cannot be withdrawn." :
			"That cannot be requested at the moment.");
		break;
	case 422:
		page_say(page, 400, "That cannot be done", "The request could not be understood. Go back and try again.");
		break;
	case 429:
		note(self, NOTE_DAEMON_LIMIT, "the daemon refused a request by one of its limits (%u seconds to wait)",
			(guint)MIN(refusal.wait_s, G_MAXUINT));
		response = too_many_in(&frame, limit_words(refusal.why), refusal.wait_s);
		page_finish(page, response);
		break;
	default:
		page_finish(page, unavailable_in(&frame));
		break;
	}
	return FALSE;
}

/* The bytes of an answer, for a reader. */
static const gchar *
bytes_of(
	GBytes *body,
	gsize  *length
){
	*length = 0;
	return body != NULL ? g_bytes_get_data(body, length) : NULL;
}

/* An answer that is not what its operation answers: logged, never shown. */
static void
page_unreadable(
	Page        *page,
	const gchar *operation
){
	AxisPortalFrame frame = frame_of(page);

	note(page->app, NOTE_UNREADABLE, "the daemon's answer to %s is not one the portal reads; the page was not shown",
		operation);
	page_finish(page, unavailable_in(&frame));
}

/* ── The routes' steps ──────────────────────────────────────────── */

static void page_run(Page *page);

static void
step_requests(
	Page   *page,
	guint   status,
	GBytes *body
){
	g_autoptr(AxisPortalRequests) requests = NULL;
	AxisPortalFrame frame = frame_of(page);
	const gchar *data;
	gsize length;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	/* The page asked for, and no other. */
	requests = axis_portal_wire_read_requests(data, length, page->number);
	if (requests == NULL) {
		page_unreadable(page, "requests");
		return;
	}
	page_finish(page, html_response(200, axis_portal_view_requests(&frame, requests, page->notice)));
}

static void
step_title(
	Page   *page,
	guint   status,
	GBytes *body
){
	g_autoptr(AxisPortalTitleInfo) title = NULL;
	AxisPortalFrame frame = frame_of(page);
	const gchar *data;
	gsize length;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	title = axis_portal_wire_read_title(data, length);
	/* The title asked for, and no other. */
	if (title == NULL || title->id != page->visit.id) {
		page_unreadable(page, "title");
		return;
	}
	page_finish(page, html_response(200, axis_portal_view_title(&frame, title, page->asked)));
}

static void
step_titles(
	Page   *page,
	guint   status,
	GBytes *body
){
	g_autoptr(AxisPortalTitles) titles = NULL;
	AxisPortalFrame frame = frame_of(page);
	gboolean search = page->visit.route == AXIS_PORTAL_ROUTE_SEARCH;
	const gchar *data;
	gsize length;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	/* The page asked for, and no other. */
	titles = axis_portal_wire_read_titles(data, length, search, page->number);
	if (titles == NULL) {
		page_unreadable(page, search ? "search" : "browse");
		return;
	}
	if (search)
		page_finish(page, html_response(200, axis_portal_view_search(&frame, page->query, page->search_kind, titles)));
	else
		page_finish(page, html_response(200, axis_portal_view_home(&frame, page->hello, page->kind, page->list,
			titles)));
}

static void
step_hello(
	Page   *page,
	guint   status,
	GBytes *body
){
	const AxisPortalForm *query = page->visit.query;
	AxisPortalFrame frame = frame_of(page);
	AxisPortalKind wanted = AXIS_PORTAL_KIND_IS_MOVIE;
	const gchar *list = axis_portal_form_get(query, "list");
	const gchar *data;
	gsize length;
	guint i;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	page->hello = axis_portal_wire_read_hello(data, length);
	if (page->hello == NULL) {
		page_unreadable(page, "hello");
		return;
	}
	/* The kind and the list asked for, when the daemon offers them; else
	 * the first it offers.  A name the daemon did not give is never sent. */
	if (axis_portal_form_kind(axis_portal_form_get(query, "kind"), &wanted) &&
	    (wanted == AXIS_PORTAL_KIND_IS_SERIES ? page->hello->series : page->hello->movies))
		page->kind = wanted;
	else
		page->kind = page->hello->movies ? AXIS_PORTAL_KIND_IS_MOVIE : AXIS_PORTAL_KIND_IS_SERIES;
	for (i = 0; list != NULL && page->hello->lists[i] != NULL; i++) {
		if (strcmp(page->hello->lists[i], list) == 0)
			page->list = g_strdup(list);
	}
	if (page->list == NULL && page->hello->lists[0] != NULL)
		page->list = g_strdup(page->hello->lists[0]);
	if ((!page->hello->movies && !page->hello->series) || page->list == NULL) {
		page_finish(page, html_response(200, axis_portal_view_home(&frame, page->hello, page->kind, NULL, NULL)));
		return;
	}
	page_call(page, AXIS_PORTAL_BROWSE, axis_portal_wire_browse(page->visit.proof, page->list, page->kind,
		page->number), step_titles);
}

/* A request was made: the title's page again, with what the daemon said. */
static void
step_asked(
	Page   *page,
	guint   status,
	GBytes *body
){
	AxisPortalApp *self = page->app;
	static const gchar *const outcomes[] = { "made", "followed", "already in the library", "unchanged" };
	const gchar *data;
	gsize length;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	page->asked = axis_portal_wire_read_asked(data, length);
	if (page->asked == NULL) {
		page_unreadable(page, "request");
		return;
	}
	g_message("request: %s asked for title %" G_GINT64_FORMAT ": %s", page->who->name, page->visit.id,
		outcomes[MIN((guint)page->asked->outcome, G_N_ELEMENTS(outcomes) - 1)]);
	/* What is left is what this answer says. */
	page->who->movies = page->asked->movies;
	page->who->seasons = page->asked->seasons;
	who_keep(self, page->visit.session_key, page->who);
	confirmed_add(self, page->visit.session_key);
	page_call(page, AXIS_PORTAL_TITLE, axis_portal_wire_title(page->visit.proof, page->visit.id), step_title);
}

static void step_who(Page *page, guint status, GBytes *body);

/* A request was withdrawn: My requests again, with what is left now. */
static void
step_withdrawn(
	Page   *page,
	guint   status,
	GBytes *body
){
	const gchar *data;
	gsize length;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	if (!axis_portal_wire_read_empty(data, length)) {
		page_unreadable(page, "withdraw");
		return;
	}
	g_message("request: %s withdrew a request", page->who->name);
	who_drop(page->app, page->visit.session_key);
	g_clear_pointer(&page->who, axis_portal_who_free);
	page->visit.route = AXIS_PORTAL_ROUTE_REQUESTS;
	page->number = 1;
	page->notice = "The request is withdrawn.";
	page_call(page, AXIS_PORTAL_WHOAMI, axis_portal_wire_proof(page->visit.proof), step_who);
}

static void
step_art(
	Page   *page,
	guint   status,
	GBytes *body
){
	if (status == 404) {
		page_finish(page, not_found());
		return;
	}
	if (!page_answered(page, status, body))
		return;
	if (body == NULL || !image_is(body, page->content_type)) {
		note(page->app, NOTE_UNREADABLE, "the daemon's answer to art is not a picture of its type; it was not served");
		page_finish(page, not_found());
		return;
	}
	poster_keep(page->app, page->visit.id, body, page->content_type);
	page_finish(page, poster_response(body, page->content_type));
}

static void
step_logged_out(
	Page   *page,
	guint   status,
	GBytes *body
){
	if (!page_answered(page, status, body))
		return;
	who_drop(page->app, page->visit.session_key);
	confirmed_drop(page->app, page->visit.session_key);
	g_message("logout: a session was ended from %s", page->visit.client);
	page_finish(page, to_login(&page->visit, TRUE));
}

/* `whoami` answered: the person is known, and the route runs. */
static void
step_who(
	Page   *page,
	guint   status,
	GBytes *body
){
	const gchar *data;
	gsize length;

	if (!page_answered(page, status, body))
		return;
	data = bytes_of(body, &length);
	page->who = axis_portal_wire_read_whoami(data, length);
	if (page->who == NULL) {
		page_unreadable(page, "whoami");
		return;
	}
	who_keep(page->app, page->visit.session_key, page->who);
	confirmed_add(page->app, page->visit.session_key);
	page->visit.confirmed = TRUE;
	page_run(page);
}

/* A search or a request post of a session the daemon has just said is one:
 * counted for that session here, so that a cookie nobody vouched for takes
 * no room in the counts.  %FALSE: refused (the page is finished). */
static gboolean
page_counted(
	Page     *page,
	gboolean  search
){
	AxisPortalApp *self = page->app;
	AxisPortalFrame frame = frame_of(page);
	gint64 wait = window_take(search ? &self->searches : &self->asks, page->visit.session_key, now_us(self));

	if (wait == 0)
		return TRUE;
	if (search)
		note(self, NOTE_SEARCHES, "search: refused a session from %s, which searched %d times in a minute",
			page->visit.client, AXIS_PORTAL_SEARCHES_A_MINUTE_HERE);
	else
		note(self, NOTE_ASKS, "request: refused a session from %s, which asked %d times in a minute",
			page->visit.client, AXIS_PORTAL_ASKS_A_MINUTE_HERE);
	page_finish(page, too_many_in(&frame, search ? "There were too many searches." : "There were too many requests.",
		seconds_of(wait)));
	return FALSE;
}

/* The person is known (the daemon said so within the last 30 seconds): the
 * route's own call, which the daemon judges by the proof again. */
static void
page_run(Page *page)
{
	const Visit *visit = &page->visit;
	AxisPortalFrame frame = frame_of(page);
	const Poster *poster;

	switch (visit->route) {
	case AXIS_PORTAL_ROUTE_HOME:
		page_call(page, AXIS_PORTAL_HELLO, g_strdup("{}"), step_hello);
		break;
	case AXIS_PORTAL_ROUTE_SEARCH:
		if (page->query == NULL) {
			page_finish(page, html_response(200, axis_portal_view_search(&frame, NULL, page->search_kind, NULL)));
			break;
		}
		if (!page_counted(page, TRUE))
			break;
		page_call(page, AXIS_PORTAL_SEARCH, axis_portal_wire_search(visit->proof, page->query, page->search_kind,
			page->number), step_titles);
		break;
	case AXIS_PORTAL_ROUTE_TITLE:
		page_call(page, AXIS_PORTAL_TITLE, axis_portal_wire_title(visit->proof, visit->id), step_title);
		break;
	case AXIS_PORTAL_ROUTE_REQUESTS:
		page_call(page, AXIS_PORTAL_REQUESTS, axis_portal_wire_requests(visit->proof, page->number), step_requests);
		break;
	case AXIS_PORTAL_ROUTE_ASK:
		if (!page_counted(page, FALSE))
			break;
		page_call(page, AXIS_PORTAL_REQUEST, axis_portal_wire_request(visit->proof, visit->id, page->kind, page->ask,
			page->picked, page->n_picked), step_asked);
		break;
	case AXIS_PORTAL_ROUTE_WITHDRAW:
		page_call(page, AXIS_PORTAL_WITHDRAW, axis_portal_wire_withdraw(visit->proof, visit->request_id),
			step_withdrawn);
		break;
	case AXIS_PORTAL_ROUTE_ART:
		/* A poster the portal holds, for a person the daemon knew a moment
		 * ago; else the daemon is asked for it. */
		poster = poster_lookup(page->app, visit->id);
		if (poster != NULL)
			page_finish(page, poster_response(poster->bytes, poster->type));
		else
			page_call(page, AXIS_PORTAL_ART, axis_portal_wire_title(visit->proof, visit->id), step_art);
		break;
	default:
		page_finish(page, not_found());
		break;
	}
}

/* ── Login ──────────────────────────────────────────────────────── */

/* The token of the pre-login form that goes with @cookie: bound to the
 * cookie and to a key that only this process has. */
static gchar *
form_token(
	AxisPortalApp *self,
	const gchar   *cookie
){
	g_autofree gchar *purpose = g_strconcat(FORM_CSRF_PURPOSE, cookie, NULL);

	return axis_token_derive(self->form_key, purpose);
}

/* The login page, with its form and the cookie the form is bound to. */
static HtmxResponse *
login_page(
	AxisPortalApp *self,
	const Visit   *visit,
	guint          status,
	const gchar   *next,
	const gchar   *message
){
	gchar *cookie = visit->form_cookie != NULL ? g_strdup(visit->form_cookie) : axis_random_token();
	gchar *token = form_token(self, cookie);
	HtmxResponse *response = html_response(status, axis_portal_view_login(token, next, message));

	set_cookie(response, form_cookie_name(visit->secure), cookie, AXIS_PORTAL_FORM_MAX_AGE_S, visit->secure, "Strict");
	axis_secret_free(token);
	axis_secret_free(cookie);
	return response;
}

static void
ignore_answer(
	guint        status,
	GBytes      *body,
	const gchar *content_type,
	gpointer     user_data
){
}

static void
step_login(
	Page   *page,
	guint   status,
	GBytes *body
){
	AxisPortalApp *self = page->app;
	Visit *visit = &page->visit;
	g_autoptr(AxisPortalLogin) login = NULL;
	AxisPortalRefusal refusal;
	HtmxResponse *response;
	const gchar *data;
	gsize length;
	gint64 now = now_us(self);

	axis_login_throttle_end(self->logins, visit->client_key);
	data = bytes_of(body, &length);
	if (status == 401 && axis_portal_wire_read_refusal(data, length, &refusal) &&
	    refusal.why == AXIS_PORTAL_REFUSED_LOGIN) {
		/* One answer, whatever it was that failed; the name that was typed
		 * is not logged (it is sometimes a password). */
		axis_login_throttle_fail(self->logins, visit->client_key, now);
		note(self, NOTE_LOGIN_FAILED, "login: failed from %s", visit->client);
		page_finish(page, login_page(self, visit, 401, page->next, "The username or password is incorrect."));
		return;
	}
	/* The daemon's own limit (for the username, say, which a stranger may
	 * have spent) is not a failure of this address. */
	if (status == 429 && axis_portal_wire_read_refusal(data, length, &refusal)) {
		note(self, NOTE_LOGIN_DAEMON_LIMIT, "login: refused by the daemon's limit, from %s", visit->client);
		response = too_many(limit_words(AXIS_PORTAL_REFUSED_THROTTLED), refusal.wait_s);
		page_finish(page, response);
		return;
	}
	if (!page_answered(page, status, body))
		return;
	login = axis_portal_wire_read_login(data, length);
	if (login == NULL) {
		page_unreadable(page, "login");
		return;
	}
	g_message("login: %s from %s", login->name, visit->client);
	/* A login that worked forgets the address's failed ones, so that the
	 * mistakes of a home do not add up to a delay for everyone there. */
	axis_login_throttle_clear(self->logins, visit->client_key);
	response = see_other(page->next != NULL ? page->next : "/");
	{
		gchar *value = sealed_cookie(self->seal_key, login->proof);
		gchar *key = axis_token_hash(login->proof);

		set_cookie(response, session_cookie_name(visit->secure), value,
			MIN(login->expires_in, AXIS_PORTAL_SESSION_MAX_AGE_S), visit->secure, "Lax");
		/* The daemon has just vouched for it. */
		confirmed_add(self, key);
		axis_secret_free(value);
		g_free(key);
	}
	if (login->device != NULL)
		set_cookie(response, device_cookie_name(visit->secure), login->device, AXIS_PORTAL_DEVICE_MAX_AGE_S,
			visit->secure, "Strict");
	set_cookie(response, form_cookie_name(visit->secure), "", 0, visit->secure, "Strict");
	/* A session the browser carried before is not carried over: it is ended
	 * at the daemon too (nobody waits for that answer). */
	if (visit->proof != NULL && !axis_secret_equal(visit->proof, login->proof)) {
		who_drop(self, visit->session_key);
		confirmed_drop(self, visit->session_key);
		/* (When every call is taken, the old proof ends on its own.) */
		if (axis_portal_client_get_pending(self->client) < AXIS_PORTAL_CLIENT_MAX_CALLS)
			axis_portal_client_call(self->client, AXIS_PORTAL_LOGOUT, axis_portal_wire_proof(visit->proof),
				ignore_answer, NULL);
	}
	page_finish(page, response);
}

/* ── One request ────────────────────────────────────────────────── */

/* Moves @visit into a page that waits for the daemon. */
static Page *
page_new(
	AxisPortalApp *self,
	Visit         *visit
){
	Page *page = g_new0(Page, 1);

	page->app = self;
	page->deferred = htmx_response_new_deferred();
	page->visit = *visit;
	memset(visit, 0, sizeof(*visit));
	page->search_kind = -1;
	page->number = 1;
	self->pending++;
	return page;
}

/* Whether the browser says the request comes from this site.  A browser
 * that sends neither header is judged by the token alone. */
static gboolean
same_origin(
	HtmxRequest *request,
	const gchar *forwarded_host,
	gboolean     https
){
	const gchar *site = header_of(request, "Sec-Fetch-Site");
	const gchar *origin = header_of(request, "Origin");
	const gchar *host = forwarded_host != NULL ? forwarded_host : header_of(request, "Host");

	if (site != NULL && strcmp(site, "same-origin") != 0)
		return FALSE;
	if (origin != NULL && !axis_auth_origin_matches(origin, host, https))
		return FALSE;
	return TRUE;
}

/* Whether a posted body is declared an urlencoded form. */
static gboolean
is_form(const gchar *content_type)
{
	static const gchar type[] = "application/x-www-form-urlencoded";

	if (content_type == NULL || g_ascii_strncasecmp(content_type, type, sizeof(type) - 1) != 0)
		return FALSE;
	return content_type[sizeof(type) - 1] == '\0' || content_type[sizeof(type) - 1] == ';' ||
		content_type[sizeof(type) - 1] == ' ';
}

static HtmxResponse *
method_refused(const gchar *allow)
{
	HtmxResponse *response = say(405, "Not allowed", "This address does not take that kind of request.");

	htmx_response_add_header(response, "Allow", allow);
	return response;
}

static HtmxResponse *
bad_request(void)
{
	return say(400, "That cannot be done", "The request could not be understood. Go back and try again.");
}

static HtmxResponse *
forbidden(void)
{
	return say(403, "Request refused", "This request could not be verified. Load the page again and try again.");
}

static HtmxResponse *
handle_login(
	AxisPortalApp *self,
	Visit         *visit
){
	static const gchar *const fields[] = { AXIS_PORTAL_CSRF_FIELD, "username", "password", "next", NULL };
	g_autofree gchar *next = NULL;
	g_autofree gchar *agent = NULL;
	const gchar *username;
	const gchar *password;
	const gchar *address;
	AxisIpAddress parsed;
	HtmxResponse *deferred;
	gchar *expected;
	gchar *json;
	Page *page;
	gint64 now = now_us(self);
	gint64 wait;
	gboolean ok;

	if (!visit->post) {
		next = safe_next(axis_portal_form_get(visit->query, "next"));
		return login_page(self, visit, 200, next, NULL);
	}
	/* The form's token: bound to the cookie that was set with the form. */
	if (visit->form_cookie == NULL)
		return forbidden();
	expected = form_token(self, visit->form_cookie);
	ok = axis_secret_equal(axis_portal_form_get(visit->form, AXIS_PORTAL_CSRF_FIELD), expected);
	axis_secret_free(expected);
	if (!ok)
		return forbidden();
	if (!axis_portal_form_only(visit->form, fields))
		return bad_request();

	/* Failed logins from this address: refused here, and the daemon is not
	 * asked. */
	wait = axis_login_throttle_check(self->logins, visit->client_key, now);
	if (wait > 0) {
		note(self, NOTE_LOGIN_ADDRESS, "login: refused %s, which has too many failed logins", visit->client);
		return too_many(limit_words(AXIS_PORTAL_REFUSED_THROTTLED), seconds_of(wait));
	}
	next = safe_next(axis_portal_form_get(visit->form, "next"));
	username = axis_portal_form_get(visit->form, "username");
	password = axis_portal_form_get(visit->form, "password");
	/* The address is reported as decided; when it is not known, as no
	 * address.  The daemon only ever adds a refusal for it. */
	address = axis_ip_address_parse(visit->client, &parsed) ? visit->client : "0.0.0.0";
	agent = g_strdup(visit->user_agent != NULL && visit->user_agent[0] != '\0' ? visit->user_agent : "-");
	json = axis_portal_wire_login(username, password, address, agent, visit->device);
	if (json == NULL) {
		/* What was typed cannot be a username and a password at all: the
		 * same answer as for a wrong one. */
		axis_login_throttle_fail(self->logins, visit->client_key, now);
		note(self, NOTE_LOGIN_FAILED, "login: failed from %s", visit->client);
		return login_page(self, visit, 401, next, "The username or password is incorrect.");
	}
	if (self->pending >= AXIS_PORTAL_MAX_PENDING ||
	    axis_portal_client_get_pending(self->client) >= AXIS_PORTAL_CLIENT_MAX_CALLS) {
		axis_portal_wire_free(json);
		note(self, NOTE_PENDING, "too many pages wait for the daemon; one was refused");
		return unavailable();
	}
	/* Logins from every address together have a share of the daemon's
	 * calls, so that a flood of them leaves the rest to the sessions the
	 * daemon has confirmed. */
	wait = unconfirmed_take(self);
	if (wait > 0) {
		axis_portal_wire_free(json);
		note(self, NOTE_BUDGET, "login: refused from %s: the calls for logins and for sessions the daemon has not "
			"confirmed are spent for this minute", visit->client);
		return too_many("There are too many logins at the moment.", seconds_of(wait));
	}
	axis_login_throttle_begin(self->logins, visit->client_key, now);
	page = page_new(self, visit);
	page->next = g_steal_pointer(&next);
	deferred = g_object_ref(page->deferred);
	page_call(page, AXIS_PORTAL_LOGIN, json, step_login);
	return deferred;
}

/* A page for a person: the route's values are checked and its limit
 * counted here, before the daemon is asked anything. */
static HtmxResponse *
handle_session(
	AxisPortalApp *self,
	Visit         *visit
){
	static const gchar *const of_ask[] = { AXIS_PORTAL_CSRF_FIELD, "kind", "what", "season", NULL };
	static const gchar *const of_post[] = { AXIS_PORTAL_CSRF_FIELD, NULL };
	const AxisPortalWho *who;
	HtmxResponse *deferred;
	AxisPortalKind kind = AXIS_PORTAL_KIND_IS_MOVIE;
	AxisPortalAsk ask = AXIS_PORTAL_ASK_ALL;
	gint picked[AXIS_PORTAL_MAX_SEASONS];
	guint n_picked = 0;
	g_autofree gchar *query = NULL;
	gint search_kind = -1;
	gint64 number = 1;
	gint64 wait;
	const gchar *text;
	gchar *json = NULL;
	Page *page;

	/* The route's own values. */
	switch (visit->route) {
	case AXIS_PORTAL_ROUTE_HOME:
	case AXIS_PORTAL_ROUTE_REQUESTS:
		text = axis_portal_form_get(visit->query, "page");
		if (text != NULL && !axis_portal_form_number(text, 1, AXIS_PORTAL_MAX_PAGE, &number))
			return bad_request();
		break;
	case AXIS_PORTAL_ROUTE_SEARCH:
		text = axis_portal_form_get(visit->query, "page");
		if (text != NULL && !axis_portal_form_number(text, 1, AXIS_PORTAL_SEARCH_MAX_PAGE, &number))
			return bad_request();
		text = axis_portal_form_get(visit->query, "kind");
		if (text != NULL && strcmp(text, "any") != 0) {
			if (!axis_portal_form_kind(text, &kind))
				return bad_request();
			search_kind = kind == AXIS_PORTAL_KIND_IS_SERIES ? 1 : 0;
		}
		text = axis_portal_form_get(visit->query, "q");
		if (text != NULL) {
			query = g_strstrip(g_strdup(text));
			if (query[0] == '\0') {
				g_clear_pointer(&query, g_free);
			} else {
				/* The writer says whether it is a query the daemon takes:
				 * 100 characters, no control character. */
				json = axis_portal_wire_search(visit->proof, query, search_kind, (guint)number);
				if (json == NULL)
					return say(400, "That cannot be searched for", "A search is 1 to 100 characters, on one line.");
				axis_portal_wire_free(json);
				json = NULL;
			}
		}
		break;
	case AXIS_PORTAL_ROUTE_ASK:
		if (!axis_portal_form_only(visit->form, of_ask) ||
		    !axis_portal_form_kind(axis_portal_form_get(visit->form, "kind"), &kind))
			return bad_request();
		if (kind == AXIS_PORTAL_KIND_IS_SERIES) {
			if (!axis_portal_form_seasons(visit->form, &ask, picked, &n_picked))
				return say(400, "Nothing was asked for", "Choose the whole series, the first or the latest season, or "
					"tick the seasons you want.");
		} else if (axis_portal_form_count(visit->form, "what") > 0 || axis_portal_form_count(visit->form, "season") > 0) {
			return bad_request();
		}
		break;
	case AXIS_PORTAL_ROUTE_WITHDRAW:
	case AXIS_PORTAL_ROUTE_LOGOUT:
		if (!axis_portal_form_only(visit->form, of_post))
			return bad_request();
		break;
	default:
		break;
	}

	/* A search is a GET, and the session cookie is Lax: a link on another
	 * site would spend the person's searches.  (The session's own counts
	 * are taken in page_run(), once the daemon has said whose it is.) */
	if (visit->route == AXIS_PORTAL_ROUTE_SEARCH && query != NULL && visit->cross_site)
		return forbidden();

	who = who_lookup(self, visit->session_key);
	/* A poster the portal holds, for a person the daemon knew within the
	 * last 30 seconds: served from memory. */
	if (visit->route == AXIS_PORTAL_ROUTE_ART && who != NULL) {
		const Poster *poster = poster_lookup(self, visit->id);

		if (poster != NULL)
			return poster_response(poster->bytes, poster->type);
	}
	if (self->pending >= AXIS_PORTAL_MAX_PENDING ||
	    axis_portal_client_get_pending(self->client) >= AXIS_PORTAL_CLIENT_MAX_CALLS) {
		note(self, NOTE_PENDING, "too many pages wait for the daemon; one was refused");
		return unavailable();
	}
	/* A session the daemon has not confirmed lately spends one of the calls
	 * that such sessions and logins share; once it is confirmed, its pages
	 * spend none. */
	if (!visit->confirmed) {
		wait = unconfirmed_take(self);
		if (wait > 0) {
			note(self, NOTE_BUDGET, "refused a session from %s that the daemon has not confirmed lately: the calls "
				"for such sessions and for logins are spent for this minute", visit->client);
			return too_many("There are too many requests at the moment.", seconds_of(wait));
		}
	}

	page = page_new(self, visit);
	visit = &page->visit;
	page->kind = kind;
	page->query = g_steal_pointer(&query);
	page->search_kind = search_kind;
	page->number = (guint)number;
	page->ask = ask;
	page->n_picked = n_picked;
	if (n_picked > 0)
		memcpy(page->picked, picked, n_picked * sizeof(picked[0]));
	page->who = axis_portal_who_copy(who);
	deferred = g_object_ref(page->deferred);

	switch (visit->route) {
	case AXIS_PORTAL_ROUTE_LOGOUT:
		page_call(page, AXIS_PORTAL_LOGOUT, axis_portal_wire_proof(visit->proof), step_logged_out);
		break;
	case AXIS_PORTAL_ROUTE_ART:
		/* The daemon is asked for the poster, with the proof; or, when the
		 * portal holds it, who this is. */
		if (poster_lookup(self, visit->id) != NULL)
			page_call(page, AXIS_PORTAL_WHOAMI, axis_portal_wire_proof(visit->proof), step_who);
		else
			page_call(page, AXIS_PORTAL_ART, axis_portal_wire_title(visit->proof, visit->id), step_art);
		break;
	default:
		if (page->who != NULL)
			page_run(page);
		else
			page_call(page, AXIS_PORTAL_WHOAMI, axis_portal_wire_proof(visit->proof), step_who);
		break;
	}
	return deferred;
}

/* Answers one request: see the order in axis-portal-app.h. */
static HtmxResponse *
serve(
	AxisPortalApp *self,
	HtmxRequest   *request
){
	SoupServerMessage *message = htmx_request_get_message(request);
	HtmxMethod method = htmx_request_get_method(request);
	const gchar *peer = message != NULL ? soup_server_message_get_remote_host(message) : NULL;
	const gchar *raw_query = htmx_request_get_query(request);
	g_autofree gchar *forwarded_host = NULL;
	g_autofree gchar *cookie = NULL;
	HtmxResponse *response = NULL;
	AxisIpAddress from;
	gboolean readable;
	gchar *expected;
	Visit visit;
	gint64 wait;
	gboolean ok;

	memset(&visit, 0, sizeof(visit));
	if (self->stopping)
		return unavailable();

	/* 1. One of the portal's own paths, or its own 404. */
	visit.route = axis_portal_route_parse(htmx_request_get_path(request), &visit.id, &visit.request_id);
	if (visit.route == AXIS_PORTAL_ROUTE_NONE) {
		response = not_found();
		goto out;
	}
	/* 2. A method the path takes. */
	readable = method == HTMX_METHOD_GET || method == HTMX_METHOD_HEAD;
	visit.post = method == HTMX_METHOD_POST;
	switch (visit.route) {
	case AXIS_PORTAL_ROUTE_LOGIN:
		if (!readable && !visit.post)
			response = method_refused("GET, HEAD, POST");
		break;
	case AXIS_PORTAL_ROUTE_LOGOUT:
	case AXIS_PORTAL_ROUTE_ASK:
	case AXIS_PORTAL_ROUTE_WITHDRAW:
		if (!visit.post)
			response = method_refused("POST");
		break;
	default:
		if (!readable)
			response = method_refused("GET, HEAD");
		break;
	}
	if (response != NULL)
		goto out;

	/* Who asks, and how: decided here, once, from the peer and from what a
	 * listed proxy says.  Anyone else's forwarding headers are ignored. */
	axis_proxy_resolve_full(self->trusted, peer, FALSE, header_list_of(request, "X-Forwarded-For"),
		header_list_of(request, "X-Forwarded-Proto"), header_list_of(request, "X-Forwarded-Host"), &visit.client,
		&visit.https, &forwarded_host);
	visit.client_key = axis_proxy_address_key(visit.client);
	if (self->resolver != NULL && peer != NULL && axis_ip_address_parse(peer, &from) &&
	    !axis_proxy_list_contains(self->trusted, &from))
		axis_proxy_resolver_poke(self->resolver);

	/* 3. The stylesheet, and the probe. */
	if (visit.route == AXIS_PORTAL_ROUTE_STYLE) {
		response = htmx_response_new();
		htmx_response_set_content(response, axis_portal_view_style());
		htmx_response_set_content_type(response, "text/css; charset=utf-8");
		keep_cache_policy(response, "public, max-age=3600");
		goto out;
	}
	if (visit.route == AXIS_PORTAL_ROUTE_HEALTH) {
		/* Only to this machine itself, asking for nobody else: it says that
		 * the program runs, and nothing more. */
		if (peer != NULL && axis_ip_address_parse(peer, &from) && axis_ip_address_is_loopback(&from) &&
		    header_of(request, "X-Forwarded-For") == NULL && header_of(request, "Forwarded") == NULL) {
			response = htmx_response_new();
			htmx_response_set_content(response, "ok\n");
			htmx_response_set_content_type(response, "text/plain; charset=utf-8");
		} else {
			response = not_found();
		}
		goto out;
	}

	/* 4. Over HTTPS, or not at all. */
	if (!visit.https && !self->insecure) {
		response = say(403, "HTTPS is needed", "This portal answers over HTTPS only. Open it through its https "
			"address. If you are its administrator: the portal must be behind a reverse proxy that it trusts "
			"(AXIS_PORTAL_TRUSTED_PROXIES) and that reports the visit as https.");
		goto out;
	}
	visit.secure = visit.https || !self->insecure;
	visit.htmx = htmx_request_is_htmx(request);
	{
		const gchar *site = header_of(request, "Sec-Fetch-Site");

		/* "none" is a bookmark or a typed address. */
		visit.cross_site = site != NULL && strcmp(site, "same-origin") != 0 && strcmp(site, "none") != 0;
	}

	/* 5. The query and the form, read whole or refused whole. */
	if (visit.post && raw_query != NULL) {
		response = bad_request();
		goto out;
	}
	visit.query = axis_portal_form_parse(raw_query, raw_query != NULL ? strlen(raw_query) : 0,
		AXIS_PORTAL_QUERY_MAX_BYTES);
	if (visit.query == NULL) {
		response = bad_request();
		goto out;
	}
	if (visit.post) {
		GBytes *body = htmx_request_get_body_bytes(request);
		const gchar *data = NULL;
		gsize length = 0;

		if (!is_form(htmx_request_get_content_type(request))) {
			response = say(415, "That cannot be done", "This address takes a posted form only.");
			goto out;
		}
		if (body != NULL)
			data = g_bytes_get_data(body, &length);
		visit.form = axis_portal_form_parse(data, length, AXIS_PORTAL_MAX_BODY_BYTES);
		if (visit.form == NULL) {
			response = bad_request();
			goto out;
		}
		if (!same_origin(request, forwarded_host, visit.https)) {
			response = forbidden();
			goto out;
		}
	} else if (visit.route != AXIS_PORTAL_ROUTE_ART) {
		/* The page to come back to after a login. */
		visit.back = raw_query != NULL ? g_strconcat(htmx_request_get_path(request), "?", raw_query, NULL) :
			g_strdup(htmx_request_get_path(request));
	}

	/* The cookies: each once, in its form, or not at all.  The session's
	 * must be one this portal sealed (checked in constant time), so that a
	 * cookie someone made up is none, and costs the daemon nothing. */
	cookie = cookie_of(request, session_cookie_name(visit.secure));
	if (cookie != NULL) {
		visit.proof = unsealed_proof(self, cookie);
		visit.cookie_refused = visit.proof == NULL;
	}
	if (visit.proof != NULL) {
		visit.session_key = axis_token_hash(visit.proof);
		visit.csrf = axis_token_derive(visit.proof, SESSION_CSRF_PURPOSE);
		visit.confirmed = confirmed_has(self, visit.session_key);
	}
	axis_secret_free(g_steal_pointer(&cookie));

	/* 6. /login. */
	if (visit.route == AXIS_PORTAL_ROUTE_LOGIN) {
		cookie = cookie_of(request, form_cookie_name(visit.secure));
		if (axis_auth_token_is_wellformed(cookie, AXIS_TOKEN_LEN))
			visit.form_cookie = g_steal_pointer(&cookie);
		axis_secret_free(g_steal_pointer(&cookie));
		cookie = cookie_of(request, device_cookie_name(visit.secure));
		if (axis_portal_wire_is_device(cookie))
			visit.device = g_steal_pointer(&cookie);
		axis_secret_free(g_steal_pointer(&cookie));
		visit.user_agent = axis_auth_clean_user_agent(header_of(request, "User-Agent"));
		response = handle_login(self, &visit);
		goto out;
	}

	/* 7. Every other path needs the session cookie.  Without one, the
	 * daemon is asked nothing; one that this portal did not seal goes. */
	if (visit.proof == NULL) {
		response = to_login(&visit, visit.cookie_refused);
		goto out;
	}
	/* An address that keeps presenting cookies the daemon does not know is
	 * refused here for a while; a session the daemon confirmed lately is
	 * not, so that one device behind an address does not lock out the
	 * others. */
	if (!visit.confirmed) {
		wait = axis_login_throttle_check(self->proofs, visit.client_key, now_us(self));
		if (wait > 0) {
			note(self, NOTE_PROOF_ADDRESS, "refused %s, which presented too many session cookies the daemon did not "
				"know", visit.client);
			response = too_many("There were too many requests.", seconds_of(wait));
			goto out;
		}
	}
	/* 8. A POST carries the token derived from the session cookie. */
	if (visit.post) {
		expected = visit.csrf;
		ok = axis_secret_equal(axis_portal_form_get(visit.form, AXIS_PORTAL_CSRF_FIELD), expected);
		if (!ok) {
			response = forbidden();
			goto out;
		}
	}
	/* 9. The route. */
	response = handle_session(self, &visit);

out:
	visit_clear(&visit);
	return response;
}

static void
on_page_complete(
	HtmxResponse *deferred,
	HtmxResponse *response,
	gpointer      user_data
){
	finish_headers(response);
}

/* The one function that answers every request.  The router is never
 * reached: it holds no route. */
static void
portal_middleware(
	HtmxContext        *context,
	HtmxMiddlewareNext  next,
	gpointer            next_data,
	gpointer            user_data
){
	AxisPortalApp *self = user_data;
	HtmxResponse *response = serve(self, htmx_context_get_request(context));

	if (response == NULL)
		response = not_found();
	if (htmx_response_is_deferred(response))
		htmx_response_on_complete(response, on_page_complete, NULL, NULL);
	else
		finish_headers(response);
	htmx_context_set_response(context, response);
}

/* ── Lifetime ───────────────────────────────────────────────────── */

static AxisProxyList *
limits_trusted(gpointer user_data)
{
	AxisPortalApp *self = user_data;

	return self->trusted;
}

AxisPortalApp *
axis_portal_app_new(
	const AxisPortalConfig  *config,
	GError                 **error
){
	AxisPortalApp *self;
	g_autoptr(GError) local = NULL;

	g_return_val_if_fail(config != NULL, NULL);

	if (!axis_secret_init()) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "no source of random numbers can be used");
		return NULL;
	}
	self = g_new0(AxisPortalApp, 1);
	self->insecure = config->insecure_browser_http;
	self->max_connections = config->max_connections;
	self->max_per_address = config->max_per_address;
	self->trusted = axis_proxy_list_parse(config->trusted_proxies != NULL ? config->trusted_proxies : "", &local);
	if (self->trusted == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "trusted proxies: %s", local->message);
		g_free(self);
		return NULL;
	}
	self->client = axis_portal_client_new(config, error);
	if (self->client == NULL) {
		axis_proxy_list_free(self->trusted);
		g_free(self);
		return NULL;
	}
	if (axis_proxy_list_has_names(self->trusted)) {
		self->resolver = axis_proxy_resolver_new(self->trusted);
		axis_proxy_resolver_start(self->resolver);
	}
	self->form_key = axis_random_token();
	self->seal_key = axis_token_derive(config->token, AXIS_PORTAL_SEAL_KEY_PURPOSE);
	self->logins = axis_login_throttle_new(AXIS_PORTAL_LOGIN_FREE_FAILURES, AXIS_PORTAL_THROTTLE_BASE_US,
		AXIS_PORTAL_THROTTLE_MAX_US, AXIS_PORTAL_THROTTLE_FORGET_US, AXIS_PORTAL_THROTTLE_MAX_KEYS);
	self->proofs = axis_login_throttle_new(AXIS_PORTAL_PROOF_FREE_FAILURES, AXIS_PORTAL_THROTTLE_BASE_US,
		AXIS_PORTAL_THROTTLE_MAX_US, AXIS_PORTAL_THROTTLE_FORGET_US, AXIS_PORTAL_THROTTLE_MAX_KEYS);
	/* An address that is one of many cannot make the tables forget the
	 * others it used. */
	axis_login_throttle_share_when_full(self->logins, AXIS_PORTAL_THROTTLE_SHARED_FORGET_US);
	axis_login_throttle_share_when_full(self->proofs, AXIS_PORTAL_THROTTLE_SHARED_FORGET_US);
	window_init(&self->searches, AXIS_PORTAL_SEARCHES_A_MINUTE_HERE);
	window_init(&self->asks, AXIS_PORTAL_ASKS_A_MINUTE_HERE);
	self->who = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, who_kept_free);
	self->confirmed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->posters = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_queue_init(&self->poster_order);
	return self;
}

void
axis_portal_app_install(
	AxisPortalApp *self,
	HtmxServer    *server
){
	AxisWebLimitsOptions options;
	AxisWebLimits *limits;

	g_return_if_fail(self != NULL && HTMX_IS_SERVER(server));

	/* (libsoup undoes the escapes of a path before the portal sees it: its
	 * "raw-paths" can be set only when the server is made, which htmx-glib
	 * does.  So "/title/7%2Frequest" arrives as "/title/7/request".  It is
	 * then that route, with every check of that route: an escape is another
	 * spelling of a path the portal serves, never a way around one.  A proxy
	 * in front must not decide anything by the path; see the known limits.) */

	/* The engine interface's limits on connections, heads and bodies; a
	 * body of at most 64 KiB, refused beyond that before it is read. */
	memset(&options, 0, sizeof(options));
	options.max_connections = self->max_connections;
	options.max_per_address = self->max_per_address;
	options.max_body_bytes = AXIS_PORTAL_MAX_BODY_BYTES;
	limits = axis_web_limits_install(server, &options);
	axis_web_limits_set_sources(limits, limits_trusted, NULL, self);
	htmx_server_use(server, portal_middleware, self, NULL);
}

void
axis_portal_app_stop(AxisPortalApp *self)
{
	g_return_if_fail(self != NULL);

	self->stopping = TRUE;
	if (self->resolver != NULL)
		axis_proxy_resolver_stop(self->resolver);
	while (self->pending > 0)
		g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
	/* What was left out of the log is said now. */
	if (self->lines_timer != 0) {
		g_source_remove(self->lines_timer);
		self->lines_timer = 0;
	}
	notes_flush(self, G_MAXINT64);
}

void
axis_portal_app_free(AxisPortalApp *self)
{
	if (self == NULL)
		return;
	axis_portal_app_stop(self);
	axis_portal_client_free(self->client);
	if (self->resolver != NULL)
		axis_proxy_resolver_free(self->resolver);
	axis_proxy_list_free(self->trusted);
	axis_secret_free(self->form_key);
	axis_secret_free(self->seal_key);
	axis_login_throttle_free(self->logins);
	axis_login_throttle_free(self->proofs);
	g_hash_table_unref(self->searches.keys);
	g_hash_table_unref(self->asks.keys);
	g_hash_table_unref(self->who);
	g_hash_table_unref(self->confirmed);
	g_hash_table_unref(self->posters);
	g_queue_clear_full(&self->poster_order, poster_free);
	g_free(self);
}

void
axis_portal_app_set_time_offset(
	AxisPortalApp *self,
	gint64         offset_us
){
	g_return_if_fail(self != NULL);

	self->time_offset_us = offset_us;
}

gsize
axis_portal_app_get_poster_bytes(AxisPortalApp *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->poster_bytes;
}

guint
axis_portal_app_get_who_kept(AxisPortalApp *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_hash_table_size(self->who);
}

guint
axis_portal_app_get_confirmed_kept(AxisPortalApp *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_hash_table_size(self->confirmed);
}

guint
axis_portal_app_get_counted_sessions(AxisPortalApp *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return g_hash_table_size(self->searches.keys) + g_hash_table_size(self->asks.keys);
}
