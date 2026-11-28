/*
 * axis-requests.h - requests: a person who is not an administrator asks for
 * a movie or for seasons of a series, and an administrator decides
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P1: requests inside the engine.)  A request is a
 * person, a title, a kind (movie or series) and, for a series, a list of
 * seasons (0083_requests.sql).  The promises this layer keeps:
 *
 *  - A request carries nothing a person typed: a title's id, a kind, and
 *    seasons chosen as the whole series, the first, the latest or picked
 *    numbers.  The choice becomes a list of season numbers when the request is
 *    made, from the seasons the title's record has then, and the list never
 *    widens by itself.  There are no requests for episodes.
 *  - Who asked is taken from the account's own row, never from the caller: its
 *    name, whether it is an administrator, its switches and its limits.
 *  - A request waits for an administrator unless the person's switch for its
 *    kind is on; while it waits nothing is added, searched for or fetched.
 *  - A quota for each person and kind over a calendar window (a day, a week
 *    from Monday, a month, in the server's local time), counted from the
 *    request rows when it is needed: there is no counter to drift.  The count
 *    and the insert are one transaction under a lock of the person, so two
 *    requests posted at the same moment cannot both pass a limit that has room
 *    for one.
 *  - One open request (waiting or approved) for a movie and for a season of a
 *    series: a second person follows the first's and is counted nothing.
 *    Every change to a title's requests is made under a lock of the title, and
 *    the database's own unique indexes hold the rule besides.
 *  - A request is decided once: the decision checks, with the row locked, that
 *    it still waits.
 *  - What became of a request after its decision (downloading, in the library,
 *    partly, failed) is never stored: it is read from the records the movies
 *    and TV modules keep, in a bounded number of statements for a page.
 *  - Approval is one path, the administrator's and the switch's alike
 *    (axis_requests_approve() and the same placing): the title is added, or
 *    found, with the administrator's folder and profile, the request's seasons
 *    are monitored and no other is changed, and its search is queued.  A title
 *    that cannot be added leaves the request approved with the engine's reason,
 *    for an administrator to try again.
 *
 * No connection is held while another module is called: a decision is
 * recorded and committed first, and the title is placed after.
 *
 * Threads: everything here blocks on the database (and placing may ask the
 * details provider): call it from a worker thread.  Create, register and free
 * on the main thread.
 */

#ifndef AXIS_REQUESTS_H
#define AXIS_REQUESTS_H

#include <glib.h>

#include "core/axis-module.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "quality/axis-quality-store.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_REQUESTS_MODULE_ID "requests"

#define AXIS_REQUESTS_SETTING_WINDOW         "requests.window"
#define AXIS_REQUESTS_SETTING_MOVIES         "requests.movies_per_window"
#define AXIS_REQUESTS_SETTING_SEASONS        "requests.seasons_per_window"
#define AXIS_REQUESTS_SETTING_AUTO_SEASONS   "requests.auto_max_seasons"
#define AXIS_REQUESTS_SETTING_SHOW_WHO       "requests.show_who_asked"
#define AXIS_REQUESTS_SETTING_MOVIES_ROOT    "requests.movies_root"
#define AXIS_REQUESTS_SETTING_MOVIES_PROFILE "requests.movies_profile"
#define AXIS_REQUESTS_SETTING_SERIES_ROOT    "requests.series_root"
#define AXIS_REQUESTS_SETTING_SERIES_PROFILE "requests.series_profile"
#define AXIS_REQUESTS_SETTING_PORTAL_DAY     "requests.portal_per_day"

/* The default of requests.portal_per_day: what all requests made through the
 * portal API may count in one calendar day. */
#define AXIS_REQUESTS_PORTAL_A_DAY 30

/* The most seasons one request may list, and the longest decline reason (in
 * characters). */
#define AXIS_REQUESTS_MAX_SEASONS 100
#define AXIS_REQUESTS_MAX_REASON  300

/* The largest limit a person may be given. */
#define AXIS_REQUESTS_MAX_LIMIT   1000

/* The most requests a person who is not an administrator may make in any 24
 * hours, whatever became of them (withdrawn ones too): a ceiling beside the
 * quota, which a withdrawal gives back.  Not a setting. */
#define AXIS_REQUESTS_MAX_A_DAY   100

#define AXIS_REQUESTS_ERROR (axis_requests_error_quark())

/**
 * AxisRequestsError:
 * @AXIS_REQUESTS_ERROR_NOT_FOUND: no such request, title or account; also a
 *   request the person neither made nor follows (the same answer, so that
 *   nothing says whether it exists)
 * @AXIS_REQUESTS_ERROR_INVALID: the request cannot be made as posted (not a
 *   title of that kind, a season the title's record does not have, a reason
 *   too long); the message says which, and holds nothing that was posted
 * @AXIS_REQUESTS_ERROR_KIND_OFF: the kind's module is switched off
 * @AXIS_REQUESTS_ERROR_CLOSED: the title is closed to requests
 * @AXIS_REQUESTS_ERROR_QUOTA: the person's limit for the kind does not have
 *   room; the message says how many are left and when the window ends
 * @AXIS_REQUESTS_ERROR_DECIDED: the request no longer waits (it was approved,
 *   declined or withdrawn already)
 * @AXIS_REQUESTS_ERROR_PLACEMENT: an approval needs a library folder and a
 *   quality profile, and neither the form nor the defaults gave both
 * @AXIS_REQUESTS_ERROR_TOO_MANY: the person made %AXIS_REQUESTS_MAX_A_DAY
 *   requests in the last 24 hours, whatever became of them
 * @AXIS_REQUESTS_ERROR_PORTAL_FULL: the request came through the portal API,
 *   and what all such requests count today (a movie one, a season one each,
 *   whatever became of them) has reached requests.portal_per_day
 * @AXIS_REQUESTS_ERROR_PORTAL_SHARE: the request came through the portal API,
 *   and what the person's own such requests count today (withdrawn ones
 *   included) has reached AXIS_REQUESTS_PORTAL_SHARE_OF() of
 *   requests.portal_per_day: one person cannot use up the portal's day
 */
typedef enum
{
	AXIS_REQUESTS_ERROR_NOT_FOUND,
	AXIS_REQUESTS_ERROR_INVALID,
	AXIS_REQUESTS_ERROR_KIND_OFF,
	AXIS_REQUESTS_ERROR_CLOSED,
	AXIS_REQUESTS_ERROR_QUOTA,
	AXIS_REQUESTS_ERROR_DECIDED,
	AXIS_REQUESTS_ERROR_PLACEMENT,
	AXIS_REQUESTS_ERROR_TOO_MANY,
	AXIS_REQUESTS_ERROR_PORTAL_FULL,
	AXIS_REQUESTS_ERROR_PORTAL_SHARE
} AxisRequestsError;

/* One person's share of requests.portal_per_day (@ceiling): a third of it, at
 * least one. */
#define AXIS_REQUESTS_PORTAL_SHARE_OF(ceiling) MAX((ceiling) / 3, 1)

GQuark
axis_requests_error_quark(void);

/* ── What a request is ──────────────────────────────────────────── */

typedef enum
{
	AXIS_REQUEST_MOVIE,
	AXIS_REQUEST_SERIES
} AxisRequestKind;

/* "movie", "series" (stored, posted). */
const gchar *
axis_request_kind_name(AxisRequestKind kind);

gboolean
axis_request_kind_from_name(
	const gchar     *name,
	AxisRequestKind *kind
);

/**
 * AxisRequestDecision:
 *
 * What people decided about a request: the only thing stored of its state.
 * @AXIS_REQUEST_REPLACED: declined, and asked for again since (the newer
 * request stands in its place); or, with no decline reason, joined to another
 * request for the same thing when two records of its title merged.
 * @AXIS_REQUEST_REMOVED: approved, its title added, and the title removed
 * from Axis since (set by the database): closed, so the title can be asked
 * for again, and still counted against the quota.
 */
typedef enum
{
	AXIS_REQUEST_WAITING,
	AXIS_REQUEST_APPROVED,
	AXIS_REQUEST_DECLINED,
	AXIS_REQUEST_WITHDRAWN,
	AXIS_REQUEST_REPLACED,
	AXIS_REQUEST_REMOVED
} AxisRequestDecision;

/* "waiting", "approved", "declined", "withdrawn", "replaced", "removed"
 * (stored). */
const gchar *
axis_request_decision_name(AxisRequestDecision decision);

/**
 * AxisRequestSeasons:
 *
 * How a person chooses a series' seasons.  The specials (season 0) are in a
 * request only when picked by number.
 */
typedef enum
{
	AXIS_REQUEST_SEASONS_ALL,
	AXIS_REQUEST_SEASONS_FIRST,
	AXIS_REQUEST_SEASONS_LATEST,
	AXIS_REQUEST_SEASONS_PICKED
} AxisRequestSeasons;

/* "all", "first", "latest", "picked" (posted). */
gboolean
axis_request_seasons_from_name(
	const gchar        *name,
	AxisRequestSeasons *seasons
);

/**
 * AxisRequestState:
 *
 * The one state a title, a season or a request reads as, the least first.
 * Everything from @AXIS_REQUEST_STATE_DOWNLOADING on is worked out from the
 * engine's own records when it is read.
 */
typedef enum
{
	AXIS_REQUEST_STATE_CAN,		/* can be requested */
	AXIS_REQUEST_STATE_CLOSED,	/* closed to requests */
	AXIS_REQUEST_STATE_WITHDRAWN,	/* (a request only) */
	AXIS_REQUEST_STATE_REMOVED,	/* (a request only) its title was removed from Axis */
	AXIS_REQUEST_STATE_DECLINED,
	AXIS_REQUEST_STATE_REQUESTED,	/* waits for an administrator */
	AXIS_REQUEST_STATE_APPROVED,
	AXIS_REQUEST_STATE_FAILED,	/* (a request only) with a cause */
	AXIS_REQUEST_STATE_DOWNLOADING,
	AXIS_REQUEST_STATE_PARTLY,
	AXIS_REQUEST_STATE_AVAILABLE
} AxisRequestState;

/* "can", "closed", "withdrawn", "removed", "declined", "requested",
 * "approved", "failed", "downloading", "partly", "available": stable
 * (classes). */
const gchar *
axis_request_state_name(AxisRequestState state);

/* "Can be requested", "Closed to requests", "Withdrawn", "Removed from Axis",
 * "Declined", "Requested", "Approved", "Failed", "Downloading", "Partly
 * available", "Available". */
const gchar *
axis_request_state_label(AxisRequestState state);

/**
 * AxisRequestCause:
 *
 * Why a request reads failed: a fixed list, in plain words, that names no
 * release, file, indexer, download client or path.
 */
typedef enum
{
	AXIS_REQUEST_CAUSE_NONE,
	AXIS_REQUEST_CAUSE_NOT_ADDED,
	AXIS_REQUEST_CAUSE_DOWNLOAD_FAILED,
	AXIS_REQUEST_CAUSE_NEEDS_ADMIN
} AxisRequestCause;

/* "The title could not be added.", "A download failed. Axis is looking
 * again.", "A download finished and needs the administrator."; "" for none.
 * (A title removed from Axis after it was added is not a failure: the request
 * is closed, AXIS_REQUEST_REMOVED.) */
const gchar *
axis_request_cause_words(AxisRequestCause cause);

/* ── The calendar window and the quota (pure) ───────────────────── */

typedef enum
{
	AXIS_REQUEST_WINDOW_DAY,
	AXIS_REQUEST_WINDOW_WEEK,
	AXIS_REQUEST_WINDOW_MONTH
} AxisRequestWindow;

/* "day", "week", "month" (the setting's values). */
gboolean
axis_request_window_from_name(
	const gchar       *name,
	AxisRequestWindow *window
);

/**
 * axis_request_window_bounds:
 * @zone: the time zone the calendar is read in
 * @now_us: a moment (microseconds since 1970)
 * @start_us: (out): the start of the window that holds it: that day's
 *   midnight, the midnight that began its Monday, or the first of its month
 * @end_us: (out): the start of the next window
 *
 * The calendar window in @zone.  A day on which the clocks change is 23 or 25
 * hours long, and the window is that day, whatever its length.  Pure.
 */
void
axis_request_window_bounds(
	GTimeZone         *zone,
	gint64             now_us,
	AxisRequestWindow  window,
	gint64            *start_us,
	gint64            *end_us
);

/**
 * AxisRequestAllowance:
 * @unlimited: an administrator: nothing is counted against a limit
 * @limit: how many in a window (0: the person cannot ask for the kind)
 * @used: how many are counted in this window
 * @left: what is left (never below 0)
 */
typedef struct
{
	gboolean unlimited;
	guint    limit;
	guint    used;
	guint    left;
} AxisRequestAllowance;

typedef struct
{
	AxisRequestWindow    window;
	gint64               start_us;
	gint64               end_us;
	AxisRequestAllowance movies;
	AxisRequestAllowance seasons;
} AxisRequestQuota;

/* "2 of 5 left this week" ("today", "this month"); "No limit" for an
 * administrator; "None: you cannot ask for movies" for a limit of 0 (@kind
 * chooses the allowance and the word).  Transfer full. */
gchar *
axis_request_quota_words(
	const AxisRequestQuota *quota,
	AxisRequestKind         kind
);

/* When the window ends, as a refusal says it: "tomorrow", "Monday", "the 1st
 * of next month".  Static. */
const gchar *
axis_request_window_until(AxisRequestWindow window);

/* ── The layer ──────────────────────────────────────────────────── */

typedef struct _AxisRequests AxisRequests;

/**
 * axis_requests_new:
 * @pool: the database, which must outlive the layer
 * @settings: (nullable): the settings store (without it the defaults apply
 *   and no placement default is set)
 * @meta: the metadata layer
 * @movies: (nullable): the movies module; without it movies cannot be asked
 *   for
 * @tv: (nullable): the TV module; without it series cannot be asked for
 * @storage, @quality: (nullable): for the choices an approval offers
 *
 * Registers the requested and the closed titles as references of the metadata
 * layer.  Main thread, before any of those layers is used.
 *
 * Returns: (transfer full): the layer, reference counted
 */
AxisRequests *
axis_requests_new(
	AxisDbPool       *pool,
	AxisSettings     *settings,
	AxisMeta         *meta,
	AxisMovies       *movies,
	AxisTv           *tv,
	AxisStorage      *storage,
	AxisQualityStore *quality
);

AxisRequests *
axis_requests_ref(AxisRequests *self);

void
axis_requests_unref(AxisRequests *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisRequests, axis_requests_unref)

/* The modules' switches: a kind whose module is switched off cannot be asked
 * for.  %NULL (the default): a kind can be asked for when its module was
 * given.  Borrowed: the registry outlives the layer's use. */
void
axis_requests_set_registry(
	AxisRequests       *self,
	AxisModuleRegistry *registry
);

/* Whether the Requests module itself is on (always, without a registry):
 * while it is off, nothing can be asked for through the portal API either. */
gboolean
axis_requests_is_on(AxisRequests *self);

/* Defined in api/axis-portal-tokens.h. */
typedef struct _AxisPortalTokens AxisPortalTokens;

/**
 * axis_requests_set_portal:
 * @tokens: (nullable): the request portal's tokens (borrowed: they outlive
 *   the layer's pages); %NULL: this daemon has no portal API
 * @port: the port of the portal listener, 0 while it is off
 *
 * What the page "Request portal" (System) shows and changes.  Main thread,
 * before the pages are registered.
 */
void
axis_requests_set_portal(
	AxisRequests     *self,
	AxisPortalTokens *tokens,
	guint             port
);

/**
 * AxisRequestsPortalLimit:
 * @AXIS_REQUESTS_PORTAL_LIMIT_DAY: requests.portal_per_day, what all requests
 *   made through the portal API count in one day
 * @AXIS_REQUESTS_PORTAL_LIMIT_LOGINS: the failed logins of an hour through
 *   the portal API (web/axis-web-auth-portal.h)
 *
 * (add-request-portal, slice P4.)  The portal API's two ceilings over all
 * people.  That one was reached is worth an administrator's look: it is what
 * a portal in the wrong hands, or a stranger who guesses passwords, runs
 * into.  A person's own quota or share is not among them.
 */
typedef enum
{
	AXIS_REQUESTS_PORTAL_LIMIT_DAY,
	AXIS_REQUESTS_PORTAL_LIMIT_LOGINS
} AxisRequestsPortalLimit;

#define AXIS_REQUESTS_PORTAL_LIMITS     2
/* This many times are remembered for each (more than a day can hold: the
 * day's ceiling is reached once a day, the hour's at most once an hour). */
#define AXIS_REQUESTS_PORTAL_LIMIT_KEPT 32

/**
 * axis_requests_portal_limit_reached:
 * @not_again_since_us: 0, or a time (microseconds since 1970): when the
 *   limit was already recorded as reached at or after it, nothing is recorded
 *
 * Records that a ceiling of the portal API was reached now.  The day's
 * ceiling is reached by the first request it refuses on a calendar day (the
 * ask passes the day's start); the hour's by the failed login that fills the
 * hour.  Kept in memory, with no account's name: a restart forgets it.  Any
 * thread.
 */
void
axis_requests_portal_limit_reached(
	AxisRequests            *self,
	AxisRequestsPortalLimit  which,
	gint64                   not_again_since_us
);

/* How often @which was reached in the last 24 hours, and when last
 * (@last_us: (out) (optional); 0 when never).  Any thread. */
guint
axis_requests_portal_limit_count(
	AxisRequests            *self,
	AxisRequestsPortalLimit  which,
	gint64                  *last_us
);

/* What the requests made through the portal API count on this calendar day
 * (@counted) against requests.portal_per_day (@ceiling), as the ask counts
 * them.  One statement; not on the main loop. */
gboolean
axis_requests_portal_today(
	AxisRequests  *self,
	gint64        *counted,
	gint64        *ceiling,
	GError       **error
);

/* Whether @kind can be asked for now (its module given and switched on). */
gboolean
axis_requests_kind_on(
	AxisRequests    *self,
	AxisRequestKind  kind
);

/* The module's description (settings, navigation, pages), for
 * axis_module_registry_register() with the layer as user data. */
const AxisModuleInfo *
axis_requests_get_module(void);

/* The pages wait for their work in flight (main loop). */
void
axis_requests_stop(AxisRequests *self);

/* For tests: the time everything is read and written at (0: the real time),
 * and the zone the calendar window is read in (%NULL: the server's local). */
void
axis_requests_set_clock(
	AxisRequests *self,
	gint64        now_us
);

void
axis_requests_set_zone(
	AxisRequests *self,
	GTimeZone    *zone
);

/* Whether a person who is not an administrator is shown who asked. */
gboolean
axis_requests_show_who(AxisRequests *self);

/* ── Asking ─────────────────────────────────────────────────────── */

/**
 * AxisRequestAsk:
 * @user_id: the account that asks (everything else about the person is read
 *   from its row)
 * @title_id: the stored title
 * @kind: what the person says it is; a title of another kind is refused
 * @seasons, @picked, @n_picked: a series' seasons (ignored for a movie)
 * @portal: the request came through the portal API (a later slice; recorded)
 */
typedef struct
{
	gint64              user_id;
	gint64              title_id;
	AxisRequestKind     kind;
	AxisRequestSeasons  seasons;
	const gint         *picked;
	guint               n_picked;
	gboolean            portal;
} AxisRequestAsk;

/**
 * AxisRequestOutcome:
 * @AXIS_REQUEST_MADE: a request was made (for the seasons that remained)
 * @AXIS_REQUEST_FOLLOWED: none was made: the person now follows the open
 *   requests that hold what they asked for
 * @AXIS_REQUEST_ALREADY_AVAILABLE: none was made: it is in the library
 * @AXIS_REQUEST_UNCHANGED: none was made and nothing changed: the person had
 *   asked for all of it already
 */
typedef enum
{
	AXIS_REQUEST_MADE,
	AXIS_REQUEST_FOLLOWED,
	AXIS_REQUEST_ALREADY_AVAILABLE,
	AXIS_REQUEST_UNCHANGED
} AxisRequestOutcome;

/**
 * AxisRequestMade:
 * @outcome: what came of it
 * @request_id, @public_id: the request made (0 and %NULL for none)
 * @decision: its decision now (waiting, or approved without waiting)
 * @approved_how: (nullable): "auto", "existing" when it was approved at once
 * @wait_reason: (nullable): why it waits although the person's switch is on:
 *   "no_defaults", "defaults_gone", "too_many_seasons"
 * @seasons: (element-type gint): the seasons it lists
 * @left_available: (element-type gint): seasons left out because they are in
 *   the library
 * @left_requested: (element-type gint): seasons left out because an open
 *   request lists them
 * @n_followed: how many requests the person began to follow
 * @placed: an approval at once added (or found) the title
 * @quota: the person's quota afterwards
 */
typedef struct
{
	AxisRequestOutcome   outcome;
	gint64               request_id;
	gchar               *public_id;
	AxisRequestDecision  decision;
	gchar               *approved_how;
	gchar               *wait_reason;
	GArray              *seasons;
	GArray              *left_available;
	GArray              *left_requested;
	guint                n_followed;
	gboolean             placed;
	AxisRequestQuota     quota;
} AxisRequestMade;

void
axis_request_made_clear(AxisRequestMade *made);

/**
 * axis_requests_ask:
 * @made: (out caller-allocates): what came of it; clear it
 *
 * A person asks for a title.  Refused whole, changing nothing, with
 * KIND_OFF, CLOSED, QUOTA, TOO_MANY, PORTAL_FULL, PORTAL_SHARE, INVALID or
 * NOT_FOUND (no such title, or no such enabled account; also a title its
 * provider marks as adult, or whose mark is not known yet, for anyone but an
 * administrator: the same answer, given before the title's kind or closed
 * state is looked at, so that the title's number says nothing).  A second
 * person is never refused for asking for
 * what is requested already: they follow it, unless their limit for the kind
 * is 0 (QUOTA: they follow nothing either).
 */
gboolean
axis_requests_ask(
	AxisRequests         *self,
	const AxisRequestAsk *ask,
	AxisRequestMade      *made,
	GError              **error
);

/* A person's quota now (NOT_FOUND: no such account). */
gboolean
axis_requests_get_quota(
	AxisRequests      *self,
	gint64             user_id,
	AxisRequestQuota  *quota,
	GError           **error
);

/**
 * axis_requests_withdraw:
 * @public_id: the request
 * @user_id: who asks; NOT_FOUND unless they made it
 * @handed_over: (out) (optional): its earliest follower took it over (it
 *   still waits, in their name, counted against them)
 *
 * The person who asked withdraws a request that waits (DECIDED when it no
 * longer does).  With followers, the earliest becomes the person who asked if
 * they have quota left; else it is withdrawn for all.
 */
gboolean
axis_requests_withdraw(
	AxisRequests  *self,
	const gchar   *public_id,
	gint64         user_id,
	gboolean      *handed_over,
	GError       **error
);

/* A follower stops following (NOT_FOUND unless they follow it). */
gboolean
axis_requests_unfollow(
	AxisRequests  *self,
	const gchar   *public_id,
	gint64         user_id,
	GError       **error
);

/**
 * axis_requests_account_gone:
 *
 * An account was disabled or deleted: the requests it made that still wait
 * are withdrawn, or handed to their earliest follower as for a withdrawal,
 * and it follows nothing any more.  Its approved requests and the titles they
 * added are not touched.  Call it after the account's change is committed: a
 * deleted account's waiting requests name no one by then, and every waiting
 * request that names no one is dealt with here.
 */
gboolean
axis_requests_account_gone(
	AxisRequests  *self,
	gint64         user_id,
	GError       **error
);

/* ── Deciding (administrators) ──────────────────────────────────── */

/**
 * axis_requests_approve:
 * @admin: who decides
 * @root_id, @profile_id: the library folder and the quality profile (0: the
 *   default for the kind); PLACEMENT, approving nothing, when either is
 *   neither given nor set as a default
 * @added: (out) (optional): the title was added (or found) and its search
 *   queued; %FALSE: the request is approved and its title could not be added
 *   (its add_error says why, for "Try again")
 *
 * Approves a request that waits (DECIDED when it no longer does; of two
 * decisions at the same moment one gets that), then places its title.
 */
gboolean
axis_requests_approve(
	AxisRequests  *self,
	const gchar   *public_id,
	const gchar   *admin,
	gint64         root_id,
	gint64         profile_id,
	gboolean      *added,
	GError       **error
);

/* Places the title of an approved request that could not be added again, with
 * another folder or profile when given (0: as the approval had them, else the
 * defaults).  DECIDED for a request that is not approved, and for one whose
 * title was added and is still there: an administrator who stopped
 * monitoring its seasons since did so on purpose. */
gboolean
axis_requests_try_again(
	AxisRequests  *self,
	const gchar   *public_id,
	const gchar   *admin,
	gint64         root_id,
	gint64         profile_id,
	gboolean      *added,
	GError       **error
);

/**
 * axis_requests_decline:
 * @reason: the administrator's, 1 to %AXIS_REQUESTS_MAX_REASON characters,
 *   one line
 * @close_title: also close the title to requests ("Do not ask again")
 */
gboolean
axis_requests_decline(
	AxisRequests  *self,
	const gchar   *public_id,
	const gchar   *admin,
	const gchar   *reason,
	gboolean       close_title,
	GError       **error
);

/* Closes a title to requests, or opens it again. */
gboolean
axis_requests_set_title_closed(
	AxisRequests  *self,
	gint64         title_id,
	gboolean       closed,
	const gchar   *admin,
	GError       **error
);

/**
 * AxisRequestPerson:
 * @auto_movies, @auto_series: the two switches
 * @movies_limit, @seasons_limit: the person's own limits, -1 for the
 *   installation's
 */
typedef struct
{
	gint64    id;
	gchar    *username;
	gboolean  admin;
	gboolean  disabled;
	gboolean  auto_movies;
	gboolean  auto_series;
	gint      movies_limit;
	gint      seasons_limit;
} AxisRequestPerson;

void
axis_request_person_free(AxisRequestPerson *person);

/* Every account (AxisRequestPerson), by name. */
gboolean
axis_requests_list_people(
	AxisRequests  *self,
	GPtrArray    **people,
	GError       **error
);

/* Sets a person's switches and limits (@person's id and the four values are
 * read; a limit above %AXIS_REQUESTS_MAX_LIMIT is INVALID). */
gboolean
axis_requests_set_person(
	AxisRequests            *self,
	const AxisRequestPerson *person,
	GError                 **error
);

/* ── Reading ────────────────────────────────────────────────────── */

/**
 * AxisRequestInfo:
 *
 * A request as its pages show it.  @state, @cause, @have and @aired are
 * worked out when it is read.  @decline_reason, @add_error, the placement and
 * @would_look_for are for whoever may see them: the caller decides
 * (axis_requests_get() already answers NOT_FOUND to a stranger).
 */
typedef struct
{
	gint64               id;
	gchar               *public_id;
	gint64               title_id;
	AxisRequestKind      kind;
	gchar               *title;
	gint                 year;
	gint64               requested_by;	/* 0: the account is gone */
	gchar               *requested_name;
	gint64               created_us;
	gint64               decided_us;
	AxisRequestDecision  decision;
	gchar               *approved_how;	/* (nullable) "admin", "auto", "existing" */
	gchar               *wait_reason;	/* (nullable) */
	gchar               *decided_by;	/* (nullable) */
	gchar               *decline_reason;	/* (nullable) */
	GArray              *seasons;		/* gint, a series' */
	guint                n_followers;
	gboolean             follows;		/* the viewer follows it (lists of one person) */
	gint64               followed_us;	/* since when the viewer follows it; 0: they do not */
	guint                declined_before;	/* how often the title was declined before it */
	gint64               root_id;
	gint64               profile_id;
	gint64               movie_id;
	gint64               series_id;
	gint64               added_us;
	gchar               *add_error;		/* (nullable) the engine's own reason */

	AxisRequestState     state;
	AxisRequestCause     cause;
	guint                have;		/* a series: aired episodes of its seasons with a file */
	guint                aired;		/* a series: aired episodes of its seasons */
	/* A waiting request: how many aired episodes an approval would look for
	 * (a movie: 1), when the title's record says (@look_known). */
	guint                would_look_for;
	gboolean             look_known;
} AxisRequestInfo;

void
axis_request_info_free(AxisRequestInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisRequestInfo, axis_request_info_free)

/* What a request reads as, in words: "Partly available, 7 of 16 episodes",
 * "Failed: A download failed. Axis is looking again.", "In the library".
 * Transfer full. */
gchar *
axis_request_info_words(const AxisRequestInfo *info);

/* "Season 2", "Seasons 1, 2 and 3", "Specials"; %NULL for a movie.  Transfer
 * full. */
gchar *
axis_request_seasons_words(const GArray *seasons);

/* The most requests one page lists. */
#define AXIS_REQUESTS_PAGE_MAX 200

/**
 * axis_requests_list_all:
 * @limit: at most this many (1 to %AXIS_REQUESTS_PAGE_MAX)
 * @requests: (out) (element-type AxisRequestInfo): the waiting ones first, the
 *   oldest of them first; then the others, the latest decided first
 * @n_waiting: (out) (optional): how many wait in all
 *
 * Every request, for an administrator.  A bounded number of statements,
 * whatever the library's size.
 */
gboolean
axis_requests_list_all(
	AxisRequests  *self,
	guint          limit,
	GPtrArray    **requests,
	guint         *n_waiting,
	GError       **error
);

/* The requests @user_id made and the ones they follow, the newest first. */
gboolean
axis_requests_list_mine(
	AxisRequests  *self,
	gint64         user_id,
	guint          limit,
	GPtrArray    **requests,
	GError       **error
);

/* One request, for @viewer_id: NOT_FOUND unless they made it, follow it or
 * are an administrator (@viewer_admin), the same as for an identifier that
 * was never issued. */
gboolean
axis_requests_get(
	AxisRequests     *self,
	const gchar      *public_id,
	gint64            viewer_id,
	gboolean          viewer_admin,
	AxisRequestInfo **info,
	GError          **error
);

typedef struct
{
	gint              season;
	AxisRequestState  state;
	guint             have;
	guint             aired;
} AxisRequestSeasonState;

/**
 * AxisRequestTitleState:
 * @state: the title's one state
 * @seasons: (element-type AxisRequestSeasonState): a series: every season the
 *   title's record has, by number, each with its state
 * @request_public_id: (nullable): the request the state is of (the open one,
 *   else the last declined)
 * @asked_by: (nullable): who asked for it
 * @asked_by_id: the account that made that request (0: none, or gone)
 * @decline_reason: (nullable): of a declined one
 * @makers: (element-type gint64): who made a request of the title that still
 *   counts (an open one, or the last declined)
 * @followers: (element-type gint64): who follows one
 * @reason_people: (element-type gint64): who made or follows the declined
 *   request whose reason is kept: besides administrators, the only people who
 *   may be told it
 *
 * A title's state for everyone who can ask, with what only some may be told.
 * It is read without knowing the viewer: axis_request_title_view() says what
 * a viewer is shown.
 */
typedef struct
{
	gint64            title_id;
	AxisRequestKind   kind;
	AxisRequestState  state;
	GArray           *seasons;
	gchar            *request_public_id;
	gchar            *asked_by;
	gint64            asked_by_id;
	gchar            *decline_reason;
	GArray           *makers;
	GArray           *followers;
	GArray           *reason_people;
} AxisRequestTitleState;

void
axis_request_title_state_free(AxisRequestTitleState *state);

/**
 * AxisRequestTitleView:
 * @asked_by: (nullable): the name to show, or %NULL when the viewer is shown
 *   none (borrowed from the state)
 * @decline_reason: (nullable): the reason, when the viewer may see it
 * @mine: the viewer made a request of the title that still counts
 * @follows: the viewer follows one
 */
typedef struct
{
	const gchar *asked_by;
	const gchar *decline_reason;
	gboolean     mine;
	gboolean     follows;
} AxisRequestTitleView;

/**
 * axis_request_title_view:
 * @show_who: requests.show_who_asked
 *
 * What @viewer_id is shown of a title's state: the name of who asked only to
 * an administrator, to the person who made the very request it names, or
 * while @show_who is on (a person with a request for another season of the
 * same series is not that person); a decline's reason only to an
 * administrator, to who asked and to followers.  Pure.
 */
void
axis_request_title_view(
	const AxisRequestTitleState *state,
	gint64                       viewer_id,
	gboolean                     viewer_admin,
	gboolean                     show_who,
	AxisRequestTitleView        *view
);

/**
 * axis_requests_title_states:
 * @title_ids: (element-type gint64): stored titles (a page of them)
 * @states: (out) (transfer full): title id (gint64 *) -> AxisRequestTitleState,
 *   one for each title that is a movie or a series
 *
 * The state of each title, in a bounded number of statements whatever their
 * number and the library's size.
 */
gboolean
axis_requests_title_states(
	AxisRequests  *self,
	const GArray  *title_ids,
	GHashTable   **states,
	GError       **error
);

/**
 * AxisRequestDefaults:
 *
 * The administrator's defaults for placing a requested title: 0 for one that
 * is not set.  @movies_ok and @series_ok: both are set and still exist.
 */
typedef struct
{
	gint64   movies_root;
	gint64   movies_profile;
	gint64   series_root;
	gint64   series_profile;
	gboolean movies_ok;
	gboolean series_ok;
} AxisRequestDefaults;

gboolean
axis_requests_get_defaults(
	AxisRequests         *self,
	AxisRequestDefaults  *defaults,
	GError              **error
);

/* Stores the four defaults (0: none), each checked to exist. */
gboolean
axis_requests_set_defaults(
	AxisRequests               *self,
	const AxisRequestDefaults  *defaults,
	GError                    **error
);

/* A title closed to requests. */
typedef struct
{
	gint64  title_id;
	gchar  *title;
	gint    year;
	gchar  *closed_by;
	gint64  closed_us;
} AxisRequestClosed;

void
axis_request_closed_free(AxisRequestClosed *closed);

/* The closed titles (AxisRequestClosed), the latest closed first. */
gboolean
axis_requests_list_closed(
	AxisRequests  *self,
	GPtrArray    **closed,
	GError       **error
);

G_END_DECLS

#endif /* AXIS_REQUESTS_H */
