/*
 * axis-discover.h - Discover: the metadata provider's lists of titles (popular,
 * trending, in cinemas or on the air, coming soon, top rated) and its
 * recommendations for what you have, kept as snapshots a job refreshes, and
 * the titles marked "not interested"
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Discover's pages read what is stored here and never ask the provider (one
 * title's own recommendations, for its page, are the exception: see "More
 * like this" below).  The job
 * (AXIS_DISCOVER_REFRESH_KIND, hourly, on the metadata queue) asks, through
 * the metadata layer (axis_meta_fetch_list()), for each list whose snapshot
 * is missing or older than discover.refresh_after: discover.pages pages of
 * it, replacing the snapshot only when every page arrived and keeping it,
 * with the problem, when one did not.  Recommendations are asked for the
 * titles some module refers to (axis_meta_referenced_sql(): what you have
 * and want), discover.recommend_per_run of them a run, each kept a week.
 *
 * "What you have" is that same set, everywhere: what "Hide what I have"
 * leaves out and what recommendations never include.
 *
 * Threads: everything but the constructor blocks on the database or the
 * network: call it from a worker thread.  Tables: 0048_discover.sql.
 */

#ifndef AXIS_DISCOVER_H
#define AXIS_DISCOVER_H

#include <gio/gio.h>
#include <glib.h>

#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "meta/axis-meta.h"

G_BEGIN_DECLS

#define AXIS_DISCOVER_MODULE_ID           "discover"
#define AXIS_DISCOVER_REFRESH_KIND        "discover.refresh"
#define AXIS_DISCOVER_REFRESH_SCHEDULE    "discover.refresh"
#define AXIS_DISCOVER_REFRESH_INTERVAL_S  3600

#define AXIS_DISCOVER_SETTING_PAGES         "discover.pages"
#define AXIS_DISCOVER_SETTING_REFRESH_AFTER "discover.refresh_after"
#define AXIS_DISCOVER_SETTING_RECOMMEND     "discover.recommend_per_run"

/* The defaults of the settings above. */
#define AXIS_DISCOVER_DEFAULT_PAGES          3
#define AXIS_DISCOVER_DEFAULT_REFRESH_AFTER  (12 * 3600)
#define AXIS_DISCOVER_DEFAULT_RECOMMEND      20

/* The most pages of a list kept (the setting's maximum). */
#define AXIS_DISCOVER_MAX_PAGES              5

/* A title's recommendations are kept a week, and so are the genre names. */
#define AXIS_DISCOVER_RECOMMEND_KEEP_S       (7 * 24 * 3600)
#define AXIS_DISCOVER_GENRES_KEEP_S          (7 * 24 * 3600)

/* How many of your titles a recommended title's card names. */
#define AXIS_DISCOVER_BECAUSE_NAMES          2

#define AXIS_DISCOVER_ERROR (axis_discover_error_quark())

typedef enum
{
	AXIS_DISCOVER_ERROR_NOT_FOUND,
	AXIS_DISCOVER_ERROR_INVALID
} AxisDiscoverError;

GQuark
axis_discover_error_quark(void);

typedef struct _AxisDiscover AxisDiscover;

/**
 * axis_discover_new:
 * @pool: the database, which must outlive the result
 * @settings: (nullable): the settings store; %NULL (or one without the
 *   module's settings) means the defaults
 * @meta: the metadata layer it asks through
 *
 * Returns: (transfer full): the layer, reference counted
 */
AxisDiscover *
axis_discover_new(
	AxisDbPool   *pool,
	AxisSettings *settings,
	AxisMeta     *meta
);

AxisDiscover *
axis_discover_ref(AxisDiscover *self);

void
axis_discover_unref(AxisDiscover *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisDiscover, axis_discover_unref)

/* The module's settings: pages, refresh_after, recommend_per_run.  Static. */
const AxisSettingInfo *
axis_discover_get_setting_table(void);

AxisMeta *
axis_discover_get_meta(AxisDiscover *self);

/* The settings in effect (their defaults without a settings store). */
guint
axis_discover_pages(AxisDiscover *self);

gint64
axis_discover_refresh_after(AxisDiscover *self);

/* 0: Recommended for you is off. */
guint
axis_discover_recommend_per_run(AxisDiscover *self);

/**
 * axis_discover_provider:
 * @ready: (out) (optional): whether it can be asked now (it needs no key, or
 *   its key is set)
 *
 * The provider Discover uses for @kind: the first that offers lists for it
 * and can be asked; when none can, the first that offers lists (whose stored
 * snapshots are still shown).
 *
 * Returns: (transfer none) (nullable): the provider, or %NULL when none
 *   offers lists for @kind
 */
const AxisMetaProviderInfo *
axis_discover_provider(
	AxisDiscover *self,
	AxisMetaKind  kind,
	gboolean     *ready
);

/**
 * axis_discover_lists:
 * @lists: (out caller-allocates) (array fixed-size=6): the lists
 *
 * The lists Discover shows for @kind, in the order of the page's tabs:
 * popular, trending, now, coming soon, top rated, recommended, each only when
 * @provider offers it.
 *
 * Returns: how many were written
 */
guint
axis_discover_lists(
	const AxisMetaProviderInfo *provider,
	AxisMetaKind                kind,
	AxisMetaList               *lists
);

/* ── The job ────────────────────────────────────────────────────── */

typedef struct
{
	guint lists;		/* snapshots of lists replaced */
	guint recommended;	/* snapshots of a title's recommendations replaced */
	guint dropped;		/* recommendations of titles no longer had, dropped */
	guint genres;		/* genre lists replaced */
	guint pages;		/* pages asked for */
	guint failed;		/* lists left as they were because a page failed */
	guint skipped;		/* kinds with no provider able to answer */
} AxisDiscoverReport;

/**
 * axis_discover_run_refresh:
 * @context: (nullable): the job, for progress and cancellation
 * @all: fetch every list again whatever its age (a person asked); a
 *   title's recommendations still wait their week
 * @report: (out) (optional): what was done
 *
 * One refresh run (see the top of this file).  A provider that is
 * unavailable, rate limited, without its key or stopping ends that kind's
 * part of the run; a list it failed keeps its snapshot with the problem.
 *
 * Returns: %FALSE only for a database error or cancellation
 */
gboolean
axis_discover_run_refresh(
	AxisDiscover        *self,
	AxisJobContext      *context,
	gboolean             all,
	AxisDiscoverReport  *report,
	GError             **error
);

/* The member of a refresh job's payload that asks for every list. */
#define AXIS_DISCOVER_PAYLOAD_ALL "all"

/* Queues a refresh now, or finds the one queued: for a page with nothing
 * stored (@all %FALSE), or "Fetch the lists now" (@all %TRUE: every list,
 * whatever its age). */
gboolean
axis_discover_request_refresh(
	AxisDiscover  *self,
	gboolean       all,
	gint64        *job_id,
	gboolean      *created,
	GError       **error
);

/* ── What the pages read ────────────────────────────────────────── */

/**
 * AxisDiscoverQuery:
 * @genre: a provider genre number, 0 for any
 * @year_from, @year_to: 0 for no bound; a title whose year is not known is
 *   left out when either is set
 * @min_rating: in tenths (70 is 7.0), 0 for any; an entry with no rating is
 *   left out when it is set
 * @hide_have: leave out what you have (recommendations always do)
 * @hide_unreleased: leave out a film the Movies module would call "Not
 *   available yet" (its availability setting and its released-after
 *   setting; with "announced" a film counts once its date is past) and a
 *   series that has not started airing (its first air date is ahead, or
 *   it has none and is not continuing or ended); recommendations too
 * @offset, @limit: the page of the matching entries
 */
typedef struct
{
	AxisMetaKind kind;
	AxisMetaList list;
	gint         genre;
	gint         year_from;
	gint         year_to;
	gint         min_rating;
	gboolean     hide_have;
	gboolean     hide_unreleased;
	guint        offset;
	guint        limit;
} AxisDiscoverQuery;

/**
 * AxisDiscoverItem:
 * @title: (transfer full): the stored brief record
 * @rating: tenths of a point, -1 for none
 * @because_n: recommendations: how many of your titles recommend it
 * @because: (element-type utf8): recommendations: the names of up to
 *   %AXIS_DISCOVER_BECAUSE_NAMES of them
 */
typedef struct
{
	AxisMetaTitle *title;
	gint           rating;
	gint           votes;
	guint          because_n;
	GPtrArray     *because;
} AxisDiscoverItem;

/**
 * AxisDiscoverPage:
 * @provider: (nullable): whose lists these are
 * @ready: @provider can be asked now (its key is set)
 * @items: (element-type AxisDiscoverItem): the page
 * @total: how many entries match
 * @size: how many the list holds, unfiltered (recommendations: how many
 *   titles are recommended, what you have and what is not wanted left out)
 * @stored: there is a snapshot of this list
 * @tried_ever: anything was ever stored or tried for this kind
 * @fetched_us: when the snapshot was fetched (recommendations: the latest)
 * @tried_us, @problem: the last attempt, and why it failed (%NULL: it did
 *   not)
 * @have: recommendations: how many titles of this kind you have
 * @seeds: recommendations: of those, how many have their recommendations kept
 */
typedef struct
{
	const AxisMetaProviderInfo *provider;
	gboolean                    ready;
	GPtrArray                  *items;
	guint                       total;
	guint                       size;
	gboolean                    stored;
	gboolean                    tried_ever;
	gint64                      fetched_us;
	gint64                      tried_us;
	gchar                      *problem;
	guint                       have;
	guint                       seeds;
} AxisDiscoverPage;

void
axis_discover_page_clear(AxisDiscoverPage *page);

/* Fails with %AXIS_DISCOVER_ERROR_INVALID for a list the provider does not
 * offer, a limit of 0 or above 100, a year or rating out of range. */
gboolean
axis_discover_read(
	AxisDiscover             *self,
	const AxisDiscoverQuery  *query,
	AxisDiscoverPage         *page,
	GError                  **error
);

/* The stored genre names of @kind (AxisMetaGenre), by name. */
gboolean
axis_discover_genres(
	AxisDiscover  *self,
	AxisMetaKind   kind,
	GPtrArray    **genres,
	GError       **error
);

/* ── More like this: one title's own recommendations ────────────── */

/* The most of a title's recommendations its page shows. */
#define AXIS_DISCOVER_LIKE_LIMIT    12

/* A failed attempt for one title is not repeated, by opening its page,
 * within this long. */
#define AXIS_DISCOVER_LIKE_RETRY_S  3600

/**
 * AxisDiscoverLike:
 * @provider: (nullable): the provider that can be asked about the title;
 *   %NULL when none can (the title has no id of the provider's: its record
 *   is another provider's and keeps none; or the provider offers no
 *   recommendations): nothing else is set then
 * @ready: @provider can be asked now (its key is set)
 * @fresh: nothing is to be fetched now: the snapshot was fetched within
 *   %AXIS_DISCOVER_RECOMMEND_KEEP_S, or an attempt failed within
 *   %AXIS_DISCOVER_LIKE_RETRY_S
 * @stored: there is a snapshot (which may hold nothing)
 * @fetched_us: when the snapshot was fetched
 * @tried_us, @problem: the last attempt, and why it failed (%NULL: it did
 *   not)
 * @size: how many titles the snapshot holds, those marked Not interested
 *   too
 * @items: (element-type AxisDiscoverItem): the recommended titles in the
 *   provider's order, those marked Not interested left out; what you have is
 *   included
 */
typedef struct
{
	const AxisMetaProviderInfo *provider;
	gboolean                    ready;
	gboolean                    fresh;
	gboolean                    stored;
	gint64                      fetched_us;
	gint64                      tried_us;
	gchar                      *problem;
	guint                       size;
	GPtrArray                  *items;
} AxisDiscoverLike;

void
axis_discover_like_clear(AxisDiscoverLike *like);

/**
 * axis_discover_read_like:
 * @seed: a stored title (its @id is the store's)
 * @limit: the most items, 1 to 100
 * @like: (out caller-allocates): what is stored of @seed's recommendations;
 *   clear it with axis_discover_like_clear()
 *
 * The store alone: no provider is asked.  It does not depend on
 * discover.recommend_per_run.
 */
gboolean
axis_discover_read_like(
	AxisDiscover         *self,
	const AxisMetaTitle  *seed,
	guint                 limit,
	AxisDiscoverLike     *like,
	GError              **error
);

/**
 * axis_discover_fetch_like:
 * @cancellable: (nullable)
 * @asked: (out) (optional): whether the provider was asked
 *
 * Asks the provider for @seed's recommendations here and now (one request,
 * as axis_meta_fetch_list()) and stores them as the refresh job does, unless
 * they are fresh (#AxisDiscoverLike.fresh) or the provider cannot be asked.
 * A failure of the provider is recorded with the snapshot and is no failure
 * of this call.
 *
 * Returns: %FALSE only for a failure of the store
 */
gboolean
axis_discover_fetch_like(
	AxisDiscover         *self,
	const AxisMetaTitle  *seed,
	GCancellable         *cancellable,
	gboolean             *asked,
	GError              **error
);

/* ── Not interested ─────────────────────────────────────────────── */

/* Marks @title_id (a stored title) not interested, by @user; marking it again
 * changes nothing.  Fails with %AXIS_DISCOVER_ERROR_NOT_FOUND when there is
 * no such title. */
gboolean
axis_discover_dismiss(
	AxisDiscover  *self,
	gint64         title_id,
	const gchar   *user,
	GError       **error
);

/* Shows @title_id again.  @removed: (out) (optional) whether it was marked. */
gboolean
axis_discover_undismiss(
	AxisDiscover  *self,
	gint64         title_id,
	gboolean      *removed,
	GError       **error
);

typedef struct
{
	AxisMetaTitle *title;	/* (transfer full) */
	gchar         *by;	/* (nullable) */
	gint64         when_us;
} AxisDiscoverDismissed;

void
axis_discover_dismissed_free(AxisDiscoverDismissed *item);

/* The titles marked not interested, the latest first: a page of them
 * (AxisDiscoverDismissed) and how many there are. */
gboolean
axis_discover_list_dismissed(
	AxisDiscover  *self,
	guint          offset,
	guint          limit,
	GPtrArray    **items,
	guint         *total,
	GError       **error
);

G_END_DECLS

#endif /* AXIS_DISCOVER_H */
