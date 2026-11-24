/*
 * axis-portal-wire.h - what axis-portal says to the daemon and how it reads
 * what the daemon answers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P3.)  The portal API is eleven operations, each
 * a POST of one JSON object (api/axis-portal-proto.h).  This file is the
 * portal's whole knowledge of their messages:
 *
 *  - a body is written by the functions below and by nothing else, from
 *    values the portal has checked: the members are the operation's own, so
 *    nothing a browser sent reaches the daemon under a name of its choosing;
 *  - an answer is read by the readers below and by nothing else.  The daemon
 *    is trusted no further than a page needs: an answer is one JSON object of
 *    at most AXIS_PORTAL_WIRE_MAX_ANSWER bytes with the members its
 *    operation defines and no others, each once and of its type; a word (a
 *    state, a kind, a cause, an outcome) is one of the fixed ones; a number
 *    is in its range; an account's name has the form of a username; and a
 *    text a provider or an administrator wrote (a title's name, an overview,
 *    a decline's reason) is cut to its length and has no control character
 *    left in it.  An answer that breaks a rule is refused whole, and the page
 *    says that requests are not available.
 *
 * GLib only, no I/O: read by the fuzz target (tests/fuzz/fuzz-portal-answer.c).
 */

#ifndef AXIS_PORTAL_WIRE_H
#define AXIS_PORTAL_WIRE_H

#include <glib.h>

G_BEGIN_DECLS

/* The largest answer read (a poster has its own limit, in the client). */
#define AXIS_PORTAL_WIRE_MAX_ANSWER      (1024 * 1024)

#define AXIS_PORTAL_WIRE_NAME_MAX_CHARS      300
#define AXIS_PORTAL_WIRE_OVERVIEW_MAX_CHARS  600
#define AXIS_PORTAL_WIRE_REASON_MAX_CHARS    300
#define AXIS_PORTAL_WIRE_MAX_TITLES          200
#define AXIS_PORTAL_WIRE_MAX_REQUESTS        200
#define AXIS_PORTAL_WIRE_MAX_TITLE_SEASONS   1000
#define AXIS_PORTAL_WIRE_MAX_LISTS           16
#define AXIS_PORTAL_WIRE_MAX_WAIT_S          (G_GINT64_CONSTANT(32) * 24 * 3600)
#define AXIS_PORTAL_WIRE_MAX_EXPIRES_S       (G_GINT64_CONSTANT(90) * 24 * 3600)

/* A device value as the daemon gives it: these many characters at most, of
 * [A-Za-z0-9_.-].  The portal reads nothing else of it. */
#define AXIS_PORTAL_WIRE_DEVICE_MAX          160

typedef enum
{
	AXIS_PORTAL_KIND_IS_MOVIE,
	AXIS_PORTAL_KIND_IS_SERIES
} AxisPortalKind;

/* A title's (or a season's) state. */
typedef enum
{
	AXIS_PORTAL_TITLE_OPEN,
	AXIS_PORTAL_TITLE_AVAILABLE,
	AXIS_PORTAL_TITLE_PARTLY,
	AXIS_PORTAL_TITLE_DOWNLOADING,
	AXIS_PORTAL_TITLE_APPROVED,
	AXIS_PORTAL_TITLE_REQUESTED,
	AXIS_PORTAL_TITLE_DECLINED,
	AXIS_PORTAL_TITLE_CLOSED
} AxisPortalTitleState;

/* A request's state. */
typedef enum
{
	AXIS_PORTAL_REQUEST_WAITING,
	AXIS_PORTAL_REQUEST_APPROVED,
	AXIS_PORTAL_REQUEST_DOWNLOADING,
	AXIS_PORTAL_REQUEST_PARTLY,
	AXIS_PORTAL_REQUEST_AVAILABLE,
	AXIS_PORTAL_REQUEST_DECLINED,
	AXIS_PORTAL_REQUEST_WITHDRAWN,
	AXIS_PORTAL_REQUEST_FAILED
} AxisPortalRequestState;

typedef enum
{
	AXIS_PORTAL_CAUSE_NONE,
	AXIS_PORTAL_CAUSE_NOT_ADDED,
	AXIS_PORTAL_CAUSE_DOWNLOAD_FAILED,
	AXIS_PORTAL_CAUSE_NEEDS_ADMIN,
	AXIS_PORTAL_CAUSE_REMOVED
} AxisPortalCause;

typedef enum
{
	AXIS_PORTAL_OUTCOME_MADE,
	AXIS_PORTAL_OUTCOME_FOLLOWED,
	AXIS_PORTAL_OUTCOME_AVAILABLE,
	AXIS_PORTAL_OUTCOME_UNCHANGED
} AxisPortalOutcome;

/* How a series' seasons are asked for. */
typedef enum
{
	AXIS_PORTAL_ASK_ALL,
	AXIS_PORTAL_ASK_FIRST,
	AXIS_PORTAL_ASK_LATEST,
	AXIS_PORTAL_ASK_PICKED
} AxisPortalAsk;

/* Why the daemon refused: the fixed word of its refusal.  Its sentence is
 * never read: a page says the portal's own words. */
typedef enum
{
	AXIS_PORTAL_REFUSED_OTHER,
	AXIS_PORTAL_REFUSED_PROOF,		/* 401: the session has ended */
	AXIS_PORTAL_REFUSED_LOGIN,		/* 401: the username or password */
	AXIS_PORTAL_REFUSED_TOKEN,		/* 401: the portal's own token */
	AXIS_PORTAL_REFUSED_THROTTLED,		/* 429: failed logins */
	AXIS_PORTAL_REFUSED_QUOTA,		/* 429: the person's quota */
	AXIS_PORTAL_REFUSED_TOO_MANY,		/* 429: the person's requests in a day */
	AXIS_PORTAL_REFUSED_PORTAL_LIMIT,	/* 429: the portal's day */
	AXIS_PORTAL_REFUSED_PORTAL_SHARE,	/* 429: the person's share of it */
	AXIS_PORTAL_REFUSED_SEARCH_LIMIT,	/* 429 */
	AXIS_PORTAL_REFUSED_PERSON_LIMIT,	/* 429 */
	AXIS_PORTAL_REFUSED_TOKEN_LIMIT,	/* 429 */
	AXIS_PORTAL_REFUSED_NOT_FOUND,		/* 404 */
	AXIS_PORTAL_REFUSED_CLOSED,		/* 409: closed to requests */
	AXIS_PORTAL_REFUSED_KIND_OFF,		/* 409 */
	AXIS_PORTAL_REFUSED_DECIDED,		/* 409: no longer waiting */
	AXIS_PORTAL_REFUSED_INVALID,		/* 422 */
	AXIS_PORTAL_REFUSED_BUSY		/* 503 */
} AxisPortalRefusedWhy;

typedef struct
{
	AxisPortalRefusedWhy why;
	gint64               wait_s;	/* 0: none given */
} AxisPortalRefusal;

typedef struct
{
	guint  limit;
	guint  left;
	gint64 window_ends;	/* seconds since 1970 */
} AxisPortalQuota;

typedef struct
{
	gchar           *name;
	AxisPortalQuota  movies;
	AxisPortalQuota  seasons;
} AxisPortalWho;

typedef struct
{
	gboolean   movies;
	gboolean   series;
	gchar    **lists;	/* %NULL-ended; each of [a-z0-9_-] */
	gboolean   show_names;
} AxisPortalHello;

typedef struct
{
	gchar  *proof;		/* secret */
	gchar  *name;
	gint64  expires_in;	/* seconds */
	gchar  *device;		/* opaque; secret */
} AxisPortalLogin;

typedef struct
{
	gint                 number;
	gint                 episodes;
	gint                 aired;
	AxisPortalTitleState state;
} AxisPortalSeasonInfo;

typedef struct
{
	gint64                id;
	AxisPortalKind        kind;
	gchar                *name;
	gint                  year;
	gchar                *overview;
	gboolean              has_poster;
	AxisPortalTitleState  state;
	gchar                *asked_by;	/* nullable */
	gboolean              has_seasons;
	GArray               *seasons;		/* AxisPortalSeasonInfo; nullable */
} AxisPortalTitleInfo;

typedef struct
{
	GPtrArray *titles;	/* AxisPortalTitleInfo* */
	guint      page;
	gboolean   more;
	gboolean   stale;
} AxisPortalTitles;

typedef struct
{
	gchar                  *id;
	gint64                  title;
	AxisPortalKind          kind;
	gchar                  *name;
	gint                    year;
	GArray                 *seasons;	/* gint */
	AxisPortalRequestState  state;
	gboolean                counts;
	guint                   have;
	guint                   of;
	AxisPortalCause         cause;
	gchar                  *reason;		/* nullable */
	gboolean                made;		/* else followed */
	gint64                  asked_at;
	gint64                  changed_at;
} AxisPortalRequestInfo;

typedef struct
{
	GPtrArray *requests;	/* AxisPortalRequestInfo* */
	guint      page;
	gboolean   more;
} AxisPortalRequests;

typedef struct
{
	AxisPortalOutcome      outcome;
	AxisPortalRequestInfo *request;		/* nullable */
	GArray                *left_available;	/* gint: seasons left out */
	GArray                *left_requested;
	guint                  followed;
	AxisPortalQuota        movies;
	AxisPortalQuota        seasons;
} AxisPortalAsked;

void axis_portal_who_free(AxisPortalWho *who);
void axis_portal_hello_free(AxisPortalHello *hello);
void axis_portal_login_free(AxisPortalLogin *login);
void axis_portal_title_info_free(AxisPortalTitleInfo *title);
void axis_portal_titles_free(AxisPortalTitles *titles);
void axis_portal_request_info_free(AxisPortalRequestInfo *request);
void axis_portal_requests_free(AxisPortalRequests *requests);
void axis_portal_asked_free(AxisPortalAsked *asked);

AxisPortalWho *axis_portal_who_copy(const AxisPortalWho *who);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalWho, axis_portal_who_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalHello, axis_portal_hello_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalLogin, axis_portal_login_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalTitleInfo, axis_portal_title_info_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalTitles, axis_portal_titles_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalRequestInfo, axis_portal_request_info_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalRequests, axis_portal_requests_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisPortalAsked, axis_portal_asked_free)

/* ── Reading ────────────────────────────────────────────────────── */

/* Each returns %NULL for an answer that is not exactly what its operation
 * answers (see the header of this file).  @data need not end in NUL. */

AxisPortalHello *
axis_portal_wire_read_hello(const gchar *data, gsize length);

AxisPortalLogin *
axis_portal_wire_read_login(const gchar *data, gsize length);

AxisPortalWho *
axis_portal_wire_read_whoami(const gchar *data, gsize length);

/* The answer of search (@search: it has "stale") or of browse. */
AxisPortalTitles *
axis_portal_wire_read_titles(const gchar *data, gsize length, gboolean search);

AxisPortalTitleInfo *
axis_portal_wire_read_title(const gchar *data, gsize length);

AxisPortalAsked *
axis_portal_wire_read_asked(const gchar *data, gsize length);

AxisPortalRequests *
axis_portal_wire_read_requests(const gchar *data, gsize length);

/* The answer of logout and of withdraw: an object with no member. */
gboolean
axis_portal_wire_read_empty(const gchar *data, gsize length);

/* A refusal: "error", "message" and maybe "wait".  %FALSE when the answer is
 * not one (@refusal is then OTHER with no wait). */
gboolean
axis_portal_wire_read_refusal(const gchar *data, gsize length, AxisPortalRefusal *refusal);

/* ── Checks on values that go into a body ───────────────────────── */

/* Whether @text is a session proof in the form the daemon gives. */
gboolean
axis_portal_wire_is_proof(const gchar *text);

/* Whether @text is a request's identifier in that form. */
gboolean
axis_portal_wire_is_request_id(const gchar *text);

/* Whether @text may be a device value: 1 to AXIS_PORTAL_WIRE_DEVICE_MAX
 * characters of [A-Za-z0-9_.-]. */
gboolean
axis_portal_wire_is_device(const gchar *text);

/* Whether @text is the name of a list: 1 to 16 of [a-z0-9_-]. */
gboolean
axis_portal_wire_is_list(const gchar *text);

/* ── Writing ────────────────────────────────────────────────────── */

/* Each returns the body of its operation (transfer full; it may hold a
 * secret: free it with axis_portal_wire_free()), or %NULL when a value is
 * not one the operation takes: nothing is sent then. */

void
axis_portal_wire_free(gchar *body);

gchar *
axis_portal_wire_login(const gchar *username, const gchar *password, const gchar *client_address,
                       const gchar *user_agent, const gchar *device);

/* logout, whoami */
gchar *
axis_portal_wire_proof(const gchar *proof);

/* @kind: -1 for both kinds. */
gchar *
axis_portal_wire_search(const gchar *proof, const gchar *query, gint kind, guint page);

gchar *
axis_portal_wire_browse(const gchar *proof, const gchar *list, AxisPortalKind kind, guint page);

/* title, art */
gchar *
axis_portal_wire_title(const gchar *proof, gint64 id);

/* @ask, @picked and @n_picked are for a series only. */
gchar *
axis_portal_wire_request(const gchar *proof, gint64 id, AxisPortalKind kind, AxisPortalAsk ask, const gint *picked,
                         guint n_picked);

gchar *
axis_portal_wire_requests(const gchar *proof, guint page);

gchar *
axis_portal_wire_withdraw(const gchar *proof, const gchar *request_id);

G_END_DECLS

#endif /* AXIS_PORTAL_WIRE_H */
