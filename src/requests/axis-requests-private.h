/*
 * axis-requests-private.h - shared by the files of the requests layer
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_REQUESTS_PRIVATE_H
#define AXIS_REQUESTS_PRIVATE_H

#include "requests/axis-requests.h"

#include "db/axis-db-conn.h"

G_BEGIN_DECLS

/* Defined in web/axis-web.h. */
typedef struct _AxisWeb AxisWeb;

/* At most this many requests' pages work at a time; beyond it a request is
 * answered 503. */
#define AXIS_REQUESTS_MAX_PENDING 32

struct _AxisRequests
{
	gatomicrefcount     ref;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisMeta           *meta;
	AxisMovies         *movies;
	AxisTv             *tv;
	AxisStorage        *storage;
	AxisQualityStore   *quality;
	AxisModuleRegistry *registry;
	gint64              clock_us;	/* tests: 0 for the real time */
	GTimeZone          *zone;
	/* The page "Request portal" (web/axis-web-portal.c): the portal's
	 * tokens (borrowed; NULL: this daemon has no portal API) and the port of
	 * its listener (0: off). */
	AxisPortalTokens   *portal_tokens;
	guint               portal_port;
	/* The pages (main loop). */
	guint               pending;
	gboolean            stopping;
};

/* The time (the test clock when one is set). */
gint64
axis_requests_now(AxisRequests *self);

/* "YYYY-MM-DD", UTC, of axis_requests_now(): what has aired. */
void
axis_requests_today(
	AxisRequests *self,
	gchar         today[11]
);

gint64
axis_requests_setting_int(
	AxisRequests *self,
	const gchar  *key,
	gint64        fallback
);

AxisRequestWindow
axis_requests_window(AxisRequests *self);

/* A decimal number as text (transfer full). */
gchar *
axis_requests_int_text(gint64 value);

/* "{1,2,3}" (transfer full). */
gchar *
axis_requests_array_text(const GArray *ids);

/* "1,2,3" into gint (or gint64 with @wide) values, appended to @out. */
void
axis_requests_parse_list(
	const gchar *text,
	gboolean     wide,
	GArray      *out
);

/* SQL for a timestamptz from microseconds since 1970 in parameter @n, and
 * for microseconds from a timestamptz expression. */
#define AXIS_REQUESTS_TS(n)  "('epoch'::timestamptz + $" #n "::bigint * interval '1 microsecond')"
#define AXIS_REQUESTS_US(x)  "coalesce((extract(epoch FROM " x ") * 1000000)::bigint, 0)"

/* The locks every change takes, in this order: the title's, then a
 * person's.  Transaction advisory locks. */
gboolean
axis_requests_lock_title(
	AxisDbConn  *conn,
	gint64       title_id,
	GError     **error
);

gboolean
axis_requests_lock_person(
	AxisDbConn  *conn,
	gint64       user_id,
	GError     **error
);

/*
 * The lock on placing a title (axis_requests_place()): a session advisory
 * lock, of a class of its own, held on a connection of the pool that does
 * nothing else until axis_requests_unlock_placing() lets go of it and gives
 * it back.  Two placings of one title wait for each other; nothing else does.
 * It is not the title's lock: placing calls the movies and TV modules, which
 * may store the title's record, and a merge of the title's records takes the
 * title's lock (0083_requests.sql) on a connection the placing waits for.
 * Returns: (transfer full) (nullable): the connection, %NULL with @error.
 */
AxisDbConn *
axis_requests_lock_placing(
	AxisRequests  *self,
	gint64         title_id,
	GError       **error
);

void
axis_requests_unlock_placing(
	AxisRequests *self,
	AxisDbConn   *conn,
	gint64        title_id
);

/* ── What the engine's records say (axis-requests-read.c) ───────── */

typedef struct
{
	gint64   movie_id;	/* 0: not managed */
	gboolean on_disk;	/* a file is in the library (managed or not) */
	gboolean in_flight;
	gboolean blocked;	/* downloaded, and its import needs a person */
	gboolean failed;	/* its last grab failed and nothing is in flight */
} AxisRequestsMovieFact;

typedef struct
{
	gint     season;
	gboolean monitored;
	guint    aired;
	guint    have;
	gboolean in_flight;
	gboolean blocked;
	gboolean failed;
} AxisRequestsSeasonFact;

typedef struct
{
	gint64  series_id;
	GArray *seasons;	/* AxisRequestsSeasonFact, by number */
} AxisRequestsSeriesFact;

typedef struct
{
	GHashTable *movies;	/* title id -> AxisRequestsMovieFact */
	GHashTable *series;	/* title id -> AxisRequestsSeriesFact */
} AxisRequestsFacts;

void
axis_requests_facts_clear(AxisRequestsFacts *facts);

/*
 * What the movies and TV modules' records say about these titles
 * (@films, @series: gint64 title ids, either may be empty): a bounded number
 * of statements, on connections of their own (hold none while calling).
 */
gboolean
axis_requests_facts_load(
	AxisRequests       *self,
	const GArray       *films,
	const GArray       *series,
	AxisRequestsFacts  *facts,
	GError            **error
);

const AxisRequestsSeasonFact *
axis_requests_season_fact(
	const AxisRequestsSeriesFact *series,
	gint                          season
);

/* Whether every aired episode of a season is in the library. */
gboolean
axis_requests_season_available(const AxisRequestsSeasonFact *fact);

/* ── Placing an approved request's title (axis-requests-decide.c) ── */

/*
 * Adds (or finds) the title of the approved request @request_id with the
 * folder and profile recorded on it, monitors its seasons and queues its
 * search; records the title, or the engine's reason when it could not be
 * added (*@added %FALSE, and %TRUE returned).  Fails only for a database
 * error.  Hold no connection.
 */
gboolean
axis_requests_place(
	AxisRequests  *self,
	gint64         request_id,
	gboolean      *added,
	GError       **error
);

/* Withdraws the waiting request @request_id of @title_id, or hands it to its
 * earliest follower, inside the caller's transaction with the title locked
 * and the request's row locked. */
gboolean
axis_requests_withdraw_locked(
	AxisRequests  *self,
	AxisDbConn    *conn,
	gint64         request_id,
	AxisRequestKind kind,
	gboolean      *handed_over,
	GError       **error
);

/* The quota of the account @user_id read on @conn (inside a transaction, the
 * person locked, when it guards an insert).  @found: (out): the account
 * exists and is enabled. */
gboolean
axis_requests_quota_on(
	AxisRequests      *self,
	AxisDbConn        *conn,
	gint64             user_id,
	AxisRequestQuota  *quota,
	gboolean          *found,
	GError           **error
);

/* The defaults read on the pool, with whether they still exist. */
gboolean
axis_requests_defaults_read(
	AxisRequests         *self,
	AxisDbConn           *conn,
	AxisRequestDefaults  *defaults,
	GError              **error
);

/* The accounts that are not administrators and whose limit for movies
 * (@no_movies) or for seasons (@no_series) is 0, by their own limit or the
 * installation's: gint64 ids.  One statement over the accounts that are. */
gboolean
axis_requests_people_without(
	AxisRequests  *self,
	GArray       **no_movies,
	GArray       **no_series,
	GError       **error
);

/* The page "Request portal" (web/axis-web-portal.c), with the pages below. */
void
axis_web_portal_register(
	AxisRequests *self,
	AxisWeb      *web
);

/* The pages (web/axis-web-requests.c). */
void
axis_web_requests_register(
	AxisRequests *self,
	AxisWeb      *web
);

G_END_DECLS

#endif /* AXIS_REQUESTS_PRIVATE_H */
