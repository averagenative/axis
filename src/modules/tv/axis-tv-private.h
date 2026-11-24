/*
 * axis-tv-private.h - shared by the files of the TV module
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-tv.c          lifetime, settings, the module and its jobs, adding,
 *                      changing, removing, monitoring, queueing searches
 *   axis-tv-state.c    states, their derivation, matching a release (pure)
 *   axis-tv-store.c    the facts queries and the module's other SQL
 *   axis-tv-sync.c     seasons and episodes following the metadata
 *   axis-tv-search.c   the series' side of the acquisition layer
 *   axis-tv-follow.c   the follow-up and the scheduler
 *   axis-tv-import.c   the series' side of the import pipeline
 *   axis-tv-web*.c     the pages
 */

#ifndef AXIS_TV_PRIVATE_H
#define AXIS_TV_PRIVATE_H

#include "modules/tv/axis-tv.h"

#include "db/axis-db-conn.h"
#include "jobs/axis-job-context.h"
#include "web/axis-web-attention.h"

G_BEGIN_DECLS

/* Tasks waiting for a thread beyond this are answered 503. */
#define AXIS_TV_MAX_PENDING 16

struct _AxisTv
{
	gint               refs;	/* atomic */
	AxisDbPool        *pool;
	AxisSettings      *settings;	/* (nullable) owned */
	AxisMeta          *meta;	/* owned */
	AxisIndexers      *indexers;	/* owned */
	AxisDownloads     *downloads;	/* owned */
	AxisStorage       *storage;	/* owned */
	AxisQualityStore  *quality;	/* owned */
	AxisImport        *import;	/* (nullable) borrowed */
	AxisIdentify      *identify;	/* (nullable) borrowed: outlives the module */
	AxisAcquire       *acquire;	/* owned */
	gint64             clock_us;	/* tests: the time (0: real) */
	/* The scheduler's page (axis_tv_set_schedule_page()), atomic. */
	gint               schedule_page;

	guint              pending;	/* (main loop) */
	gboolean           stopping;	/* (main loop) */

	/* What the page "Needs attention" was last told of the series (the
	 * source of axis-tv-web.c, which alone touches it). */
	AxisAttentionTitles attention;
	/* The count under way: the series are gone through a batch at a time,
	 * over as many readings as it takes (the same source's). */
	struct
	{
		gboolean running;
		gint64   after_id;	/* the highest series id gone through */
		guint64  count;
		gint64   newest;
		guint    reasons[AXIS_ACQUIRE_ATTENTION_FAILED + 1];
		guint64  held;		/* series with a release only a person can grab */
		guint64  held_episodes;
		gint64   held_newest;
	} attention_pass;
	/* The last finished count's series that wait for "Grab this one" (slice
	 * N2): their own line, apart from @attention's count. */
	guint64            attention_held;
	guint64            attention_held_episodes;
	gint64             attention_held_newest;
};

#define axis_tv_int_text axis_acquire_int_text
#define axis_tv_text_cut axis_acquire_text_cut

/* ── Settings (axis-tv.c) ───────────────────────────────────────── */

gint64
axis_tv_setting_duration(
	AxisTv      *self,
	const gchar *key,
	gint64       fallback
);

gint64
axis_tv_setting_integer(
	AxisTv      *self,
	const gchar *key,
	gint64       fallback
);

gchar *
axis_tv_setting_string(
	AxisTv      *self,
	const gchar *key,
	const gchar *fallback
);

/* "2026-11-14" for @now_us, UTC. */
void
axis_tv_today(
	gint64 now_us,
	gchar  today[11]
);

/* The start of @date (UTC) in microseconds, 0 when it is not a date. */
gint64
axis_tv_date_us(const gchar *date);

/* ── The store (axis-tv-store.c) ────────────────────────────────── */

typedef enum
{
	AXIS_TV_SERIES_ALL,	/* no parameter */
	AXIS_TV_SERIES_ONE,	/* $1: a series' id */
	/* The two below are for lists and cards: without what only a series' own
	 * page and its searches read (its identifiers at the other providers, the
	 * releases its searches ruled out, its episodes without a provider's id,
	 * its stray downloads, the claims about its identifiers), which are NULL
	 * and 0, so that the statement looks nothing up a series. */
	AXIS_TV_SERIES_TITLES,	/* $1: bigint[] of meta_title ids */
	AXIS_TV_SERIES_LIST	/* $1: bigint[] of series ids, or NULL for every series; by title */
} AxisTvSeriesFilter;

gboolean
axis_tv_store_series(
	AxisDbConn          *conn,
	AxisTvSeriesFilter   filter,
	const gchar         *param,
	GPtrArray          **series,
	GError             **error
);

/* One series on @conn; %NULL (and %TRUE) when there is none. */
gboolean
axis_tv_store_series_one(
	AxisDbConn     *conn,
	gint64          series_id,
	AxisTvSeries  **series,
	GError        **error
);

/* THE episode facts query: the episodes (AxisTvEpisode) of the series
 * @series_ids ("{1,2}"), by series, season and number; gone ones only when
 * @with_gone. */
gboolean
axis_tv_store_episodes(
	AxisDbConn   *conn,
	const gchar  *series_ids,
	gboolean      with_gone,
	GPtrArray   **episodes,
	GError      **error
);

/* The series with an episode that can be due for a search at @rules' time,
 * by their ids (gint64), in their order: more of them than have something
 * due, never fewer (the statement says how it tells).  The scheduler reads
 * the episodes of these and of no other series. */
gboolean
axis_tv_store_schedule_series(
	AxisDbConn         *conn,
	const AxisTvRules  *rules,
	GArray            **ids,
	GError            **error
);

typedef enum
{
	AXIS_TV_EPISODES_OF_SERIES,	/* "{1,2}": series ids */
	AXIS_TV_EPISODES_OF_IDS,	/* "{1,2}": episode ids */
	AXIS_TV_EPISODES_OF_DAYS	/* "{2026-11-07,2026-11-28}": air dates, both included */
} AxisTvEpisodesOf;

/* As axis_tv_store_episodes(), of other episodes than whole series': the
 * same statement and the same facts. */
gboolean
axis_tv_store_episodes_of(
	AxisDbConn         *conn,
	AxisTvEpisodesOf    which,
	const gchar        *param,
	gboolean            with_gone,
	GPtrArray         **episodes,
	GError            **error
);

/* How many episodes are in each state, and need attention for each reason. */
typedef struct
{
	guint counts[AXIS_TV_N_STATES];
	guint kinds[AXIS_ACQUIRE_ATTENTION_FAILED + 1];
} AxisTvTotals;

typedef enum
{
	AXIS_TV_MEMBERS_OF_NONE,
	AXIS_TV_MEMBERS_OF_WANTED,	/* wanted, searched for, needing attention */
	AXIS_TV_MEMBERS_OF_UPGRADES	/* wanted for an upgrade */
} AxisTvMembersOf;

/* An episode by its place, with the state its facts give it.  @held: it
 * needs attention because its last search found a release only a person can
 * grab. */
typedef struct
{
	gint64       series_id;
	gint64       id;
	gint         season;
	gint         number;
	AxisTvState  state;
	gboolean     held;
} AxisTvMember;

/**
 * axis_tv_store_overview:
 * @series_ids: (nullable): "{1,2}", or %NULL for every series
 * @summaries: (out): AxisTvSummary, by title: every count of
 *   axis_tv_summary_new(), the state and the sentence, without the episodes
 *   that need attention (@attention is empty and @reason %NULL for a series
 *   that has some: axis_tv_store_overview_attention() reads them)
 * @totals: (out) (nullable): the episodes of all of them, by state
 * @members: (out) (nullable): AxisTvMember, by series, season and number:
 *   every episode that may be in the states @members_of names, each with its
 *   state (the caller keeps the ones it wants)
 *
 * Every series with its episodes counted, in two statements whatever their
 * number and without reading an episode: the series (AXIS_TV_SERIES_LIST)
 * and their episodes by class (a row for every distinct set of the facts a
 * state is derived from, within a series).  The state of each class is what
 * axis_tv_episode_derive_attention() says of it: the one definition.
 */
gboolean
axis_tv_store_overview(
	AxisTv           *self,
	AxisDbConn       *conn,
	const gchar      *series_ids,
	AxisTvMembersOf   members_of,
	GPtrArray       **summaries,
	AxisTvTotals     *totals,
	GArray          **members,
	GError          **error
);

/* Reads the episodes of the series among @summaries[@first .. @first + @n)
 * that need attention and were only counted, and puts their whole summary
 * (axis_tv_summary_new()) in their place: one statement for all of them,
 * none when there is none. */
gboolean
axis_tv_store_overview_attention(
	AxisTv      *self,
	AxisDbConn  *conn,
	GPtrArray   *summaries,
	guint        first,
	guint        n,
	GError     **error
);

/* Every series with its episodes counted (AxisTvSummary), from every
 * episode's facts: for a few series (a page's, one title's), never for all
 * of them on a page view. */
gboolean
axis_tv_store_summaries(
	AxisTv              *self,
	AxisDbConn          *conn,
	AxisTvSeriesFilter   filter,
	const gchar         *param,
	GPtrArray          **summaries,
	GError             **error
);

AxisTvSummary *
axis_tv_summary_new(
	AxisTvSeries      *series,
	const GPtrArray   *episodes,
	const AxisTvRules *rules
);

/* Appends to the series' history.  @series_id 0 with @title_label for a
 * series already gone; @episode_id 0 for none. */
gboolean
axis_tv_store_event(
	AxisDbConn   *conn,
	gint64        series_id,
	gint64        title_id,
	const gchar  *title_label,
	gint64        episode_id,
	const gchar  *kind,
	const gchar  *text,
	const gchar  *user,
	gint64        search_id,
	gint64        grab_id,
	GError      **error
);

gboolean
axis_tv_store_eventf(
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	gint64               episode_id,
	const gchar         *kind,
	const gchar         *user,
	GError             **error,
	const gchar         *format,
	...
) G_GNUC_PRINTF(7, 8);

/* Locks the series' row; *@found %FALSE when there is none. */
gboolean
axis_tv_store_lock(
	AxisDbConn  *conn,
	gint64       series_id,
	gboolean    *found,
	GError     **error
);

/* The de-duplication key of a search job. */
gchar *
axis_tv_search_key(
	gint64       series_id,
	AxisTvScope  scope,
	gint64       item
);

/* The key of a manual search's job ("tv.search:<series>:list:..."): never
 * one of the automatic searches', so neither waits for nor shows as the
 * other. */
gchar *
axis_tv_manual_key(
	gint64       series_id,
	AxisTvScope  scope,
	gint64       item
);

/* Queues a search job on @conn (de-duplicated by what it is for). */
gboolean
axis_tv_store_enqueue_search(
	AxisDbConn   *conn,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *cause,
	const gchar  *user,
	gboolean     *created,
	GError      **error
);

/* ── Adding, and what was brought over (axis-tv.c, axis-tv-brought.c) ── */

/* The body of axis_tv_add(), in the caller's transaction: what a person's
 * Add does, and what bringing a series over does through it
 * (@spec->brought_from). */
gboolean
axis_tv_add_in_transaction(
	AxisTv            *self,
	AxisDbConn        *conn,
	const AxisTvSpec  *spec,
	gint64            *series_id,
	GError           **error
);

/**
 * axis_tv_store_end_hold:
 * @series: the series, locked by the caller
 * @scope, @item: what a person searches for: the series, a season (its
 *   number) or an episode (its id)
 * @what: "Search now"
 *
 * What a person's Search now ends of the holds of a series brought over,
 * before it queues (a manual search, which only lists, never comes here: its
 * job passes the hold by its cause and ends nothing).  For an episode: that
 * episode's own hold, and nothing of the series'.  For a season: nothing; the
 * search runs for the season's episodes that are not held apart.  For the
 * whole series: the series' own hold, and never an episode's (a second copy
 * of a file Sonarr has would be fetched).  One line in the history says what
 * ended.  Nothing when nothing is held.  For the whole series
 * @series->search_hold is cleared, so that what the caller plans next sees no
 * hold.
 */
gboolean
axis_tv_store_end_hold(
	AxisDbConn    *conn,
	AxisTvSeries  *series,
	AxisTvScope    scope,
	gint64         item,
	const gchar   *user,
	const gchar   *what,
	GError       **error
);

/* ── Sync (axis-tv-sync.c) ──────────────────────────────────────── */

/* Whether the choice @monitor monitors the season @number (0: the specials,
 * only for "every episode" with @specials), of a series whose @first and
 * @latest seasons are those: the one rule, for a series being added and for
 * the monitoring of a whole series changed later
 * (axis_tv_set_series_monitoring()). */
gboolean
axis_tv_monitor_takes_season(
	AxisTvMonitor monitor,
	gboolean      specials,
	gint          number,
	gint          first,
	gint          latest
);

/* In the caller's transaction, the series' row locked: its seasons and
 * episodes as the metadata store has them now (see axis_tv_sync()); new
 * seasons and episodes take @new_monitored when the series is being added
 * (@adding), else their season's or the series' policy.  *@changes: rows
 * that appeared, moved or went. */
gboolean
axis_tv_sync_in_transaction(
	AxisTv              *self,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const AxisTvSpec    *adding,
	guint               *changes,
	GError             **error
);

/* As axis_tv_sync_in_transaction(), writing no "episodes" event when @quiet
 * (a change of numbering records its own). */
gboolean
axis_tv_sync_rows(
	AxisTv              *self,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const AxisTvSpec    *adding,
	gboolean             quiet,
	guint               *changes,
	GError             **error
);

/* ── Numbering (axis-tv-numbering.c) ────────────────────────────── */

/* Tells the metadata layer which sources the managed series follow now
 * (axis_meta_set_followed()): one statement; a failure is logged. */
void
axis_tv_note_followed(AxisTv *self);

/* The one episode a file's name at @path gives (the reading imports and
 * identification use); -1 for none. */
void
axis_tv_name_episode(
	const gchar *path,
	gint        *season,
	gint        *number
);

/* ── Anime: what the matcher is given (axis-tv-absolute.c) ──────── */

/**
 * AxisTvAnime:
 *
 * What axis_tv_match_release_anime() needs of one series, owned: the kind,
 * the state of its absolute numbers, each known episode's number, the day,
 * and the series' names.  For a series that is not marked as anime only
 * @read.anime (%FALSE) is set: nothing is read or computed for it.
 * @read.name is the caller's to set for each name it matches.
 */
typedef struct
{
	AxisTvAnimeRead  read;
	gchar            today[11];
	gint            *absolute;
	GPtrArray       *names;
} AxisTvAnime;

/* How the matcher's two sentences about a name numbered absolutely that is
 * not matched begin (axis-tv-state.c), by which the series' own counts of
 * such releases find them among its searches' candidates (axis-tv-store.c). */
#define AXIS_TV_SAYS_ABSOLUTE_ONLY   "it states only an absolute episode number"
#define AXIS_TV_SAYS_SEASON_ABSOLUTE "it states a season and an absolute episode number"

/* How the note of a file an import placed through an absolute number begins
 * (the import's page), and with it the line the episode's history gains:
 * "Read by its absolute number: 137 is S06E12 by TheTVDB (TheTVDB's
 * numbering as of 2026-11-21)." */
#define AXIS_TV_ABSOLUTE_NOTE "Read by its absolute number"

/* How the record of a grab made by absolute numbers begins
 * (series_grab.close_text) when its download was imported and had no file for
 * some of the episodes it was grabbed for; those follow, "S02E01, S02E02".
 * A search of the series reads it, and does not grab that release again while
 * one of those episodes still has no file (the A3 review). */
#define AXIS_TV_GRAB_LEFT_HOLES "Imported, with no file for:"

/* @episodes: (element-type AxisTvEpisode): the series' episodes that are not
 * gone, in the order of the known episodes the matcher will be given.  For
 * an anime series, on @conn (nullable), the series' other names are read
 * too.  %FALSE only for a database error. */
gboolean
axis_tv_anime_init(
	AxisTvAnime         *anime,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	const GPtrArray     *episodes,
	const gchar         *today,
	GError             **error
);

void
axis_tv_anime_clear(AxisTvAnime *anime);

/* The one episode a file's name at @path gives of an anime series, through
 * its absolute number when reading it as any series' gives none (@known as
 * @anime was made for; -1 for none; *@absolute: the number it was read by, 0
 * when it states a season and an episode).  For a series not marked as anime
 * this is axis_tv_name_episode(). */
void
axis_tv_name_episode_anime(
	const gchar        *path,
	AxisTvAnime        *anime,
	const AxisTvKnown  *known,
	guint               n,
	gint               *season,
	gint               *number,
	gint               *absolute
);

/* ── Files placed by their names (axis-tv-place.c) ──────────────── */

/* The import states in which an import of a series is open: the numbering
 * switch waits for them, and placing by names and changing links by hand
 * are refused.  One that ended partly imported is open too: "retry" takes
 * up its other files under its grab, until it is retried to the end or
 * dismissed (add-force-grab-and-assign, C1). */
#define AXIS_TV_IMPORT_OPEN "('waiting', 'running', 'blocked', 'partial')"

/* *@busy: a sentence naming an open import of the series by its real state
 * and where to find it, or %NULL when none is open. */
gboolean
axis_tv_import_open(
	AxisDbConn   *conn,
	gint64        series_id,
	gchar       **busy,
	GError      **error
);

/* In the caller's transaction: the series' files whose names, read under
 * the numbering it follows, say another episode than the one they are on
 * (AxisTvMisplaced). */
gboolean
axis_tv_misplaced_in(
	AxisTv        *self,
	AxisDbConn    *conn,
	gint64         series_id,
	gint64         only,
	GPtrArray    **list,
	gchar        **left,
	gchar        **digest,
	GError       **error
);

/* In the caller's transaction, the series locked: @file_id (0: every one
 * that "Place all" would, never one named under an earlier numbering)
 * placed on the episode its name says; *@placed: how many.  @digest (0 only,
 * nullable): the list the person saw; when what is placeable is another,
 * nothing is placed and *@changed is set. */
gboolean
axis_tv_place_in(
	AxisTv              *self,
	AxisDbConn          *conn,
	const AxisTvSeries  *series,
	gint64               file_id,
	const gchar         *digest,
	const gchar         *user,
	guint               *placed,
	gboolean            *changed,
	GError             **error
);

/*
 * The placement engine, one for the plan a page draws and for the change
 * itself.  Episodes hold a count of files; each selected file in turn is
 * placed on the episode its name says when that episode holds no file
 * *now* (after the files placed before it), is in the numbering, and neither
 * it nor the one the file leaves is in an open grab.  Nothing is read from
 * the database here.
 */
typedef struct
{
	gint64        id;		/* 0: not stored yet */
	gint          season;
	gint          number;
	gboolean      monitored;
	gboolean      aired;
	gboolean      grab;
	guint         files;		/* files on it now */
	guint         before;		/* and when the run began */
	const gchar  *file;		/* (nullable, borrowed) the path of one of them */
} AxisTvPlaceEp;

typedef struct
{
	gint64        file_id;
	gchar        *path;		/* owned */
	guint         from;		/* index of its episode */
	gint          name_season;
	gint          name_number;
	gboolean      earlier;	/* an Axis import linked it, or its name was read under another numbering */
	gboolean      by_import;
	gboolean      selected;	/* in: this run places it */
	gint          to;		/* out: index of the episode, -1: none */
	gboolean      placed;
	gchar        *refused;	/* out, owned: why not */
	gboolean      leaves_wanted;	/* out: the episode it leaves is empty then, monitored and aired */
	/* What the run of all made of it, kept by the listing beside its own
	 * judgement alone (axis-tv-place.c, model_judge()). */
	gboolean      all_placed;
	gchar        *all_refused;	/* owned */
	gboolean      all_leaves_wanted;
} AxisTvPlaceFile;

/* Runs the selected files in order over @eps (changed: pass a copy) with
 * @busy (nullable: a sentence that refuses them all). */
void
axis_tv_place_run(
	GArray       *eps,
	GArray       *files,
	const gchar  *busy
);

/* The episodes @eps (after a run) hold no file of that held one at its
 * start, and which of them are wanted, a sentence; %NULL when none. */
gchar *
axis_tv_place_left(const GArray *eps);

/* A digest of what @files (after a run) place. */
gchar *
axis_tv_place_digest(
	const GArray *eps,
	const GArray *files
);

void
axis_tv_place_file_clear(AxisTvPlaceFile *file);

/* The seasons of @source (%NULL: the record provider's own) of the series'
 * title whose episodes were never fetched, fetched now (waiting at most
 * @wait_ms for each); a source that comes whole (TheTVDB's orders), fetched
 * whole when it is not fresh. */
void
axis_tv_fetch_seasons(
	AxisTv      *self,
	gint64       title_id,
	const gchar *source,
	guint        wait_ms
);

/* ── Searching (axis-tv-search.c) ───────────────────────────────── */

const AxisAcquireOwnerInfo *
axis_tv_acquire_info(void);

/* ── Planning searches (axis-tv-follow.c) ───────────────────────── */

/* What the scheduler would queue for every wanted aired episode of the
 * series now, at most @limit searches: a season when several of it are
 * wanted (tv.pack_threshold), else each episode; the whole series first
 * when every aired episode of several seasons is wanted.  @leave_out:
 * (nullable) episode ids (gint64) not searched for now (a person's search of
 * them ended a moment ago). */
gboolean
axis_tv_plan_series(
	AxisTv       *self,
	AxisDbConn   *conn,
	gint64        series_id,
	const gchar  *cause,
	const gchar  *user,
	guint         limit,
	GHashTable   *leave_out,
	guint        *queued,
	GError      **error
);

/* After a season (or series) search that took nothing: each wanted episode
 * of the season (each season of the series). */
gboolean
axis_tv_queue_narrower(
	AxisTv       *self,
	AxisDbConn   *conn,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *cause,
	const gchar  *user,
	guint        *queued,
	GError      **error
);

/* ── Series on disk (axis-tv-library.c) ─────────────────────────── */

/*
 * The units identification linked in a root for TV that hold videos present
 * in the index: the "on disk" of a series on disk.  An offline disk freezes
 * its rows as present, so a series whose disk is away stays listed; a file a
 * scan found gone is left out.  li is the unit, llr its root, llif a file's
 * row in it, llf the file: the tables, then the conditions.
 */
#define AXIS_TV_ON_DISK_TABLES \
	" library_identity li JOIN library_root llr ON llr.id = li.root_id AND llr.media_type = 'tv'" \
	" JOIN library_identity_file llif ON llif.identity_id = li.id" \
	" JOIN library_file llf ON llf.id = llif.file_id AND llf.state = 'present'"

#define AXIS_TV_ON_DISK_WHERE \
	" WHERE li.state = 'linked' AND li.title_id IS NOT NULL" \
	" AND NOT EXISTS (SELECT 1 FROM series ls WHERE ls.title_id = li.title_id)"

/**
 * axis_tv_store_on_disk:
 * @titles: (nullable): a bigint[] of meta_title ids, or %NULL for every
 *   series on disk
 * @name_key: (nullable): only those one of whose names holds this key
 * @series: (out) (element-type AxisTvOnDisk): by title, not counted
 *
 * One statement, one pass over the linked units' files.
 */
gboolean
axis_tv_store_on_disk(
	AxisDbConn   *conn,
	const gchar  *titles,
	const gchar  *name_key,
	GPtrArray   **series,
	GError      **error
);

/* The counts of @series (AxisTvOnDisk, a page's) against their data as of
 * @today: one statement. */
gboolean
axis_tv_store_on_disk_counts(
	AxisDbConn   *conn,
	GPtrArray    *series,
	const gchar  *today,
	GError      **error
);

/* How many units of the roots for TV identification holds for a person, and
 * how many wait for the provider. */
gboolean
axis_tv_store_held(
	AxisDbConn  *conn,
	guint       *review,
	guint       *waiting,
	GError     **error
);

/* Whether @title_id is a series on disk Axis does not manage, as @conn sees
 * it. */
gboolean
axis_tv_store_is_on_disk(
	AxisDbConn  *conn,
	gint64       title_id,
	gboolean    *on_disk,
	GError     **error
);

/* ── Identification (axis-tv-identify.c) ────────────────────────── */

/* In the caller's transaction: the links of the series @series_id to every
 * episode file identified as @title_id and placed on one of its episodes (an
 * import's links stand); *@linked: how many files. */
gboolean
axis_tv_link_identified(
	AxisDbConn   *conn,
	gint64        series_id,
	gint64        title_id,
	guint        *linked,
	GError      **error
);

/* In the caller's transaction: the links of the series' identified files
 * that no link holds yet, by their names under the numbering it follows;
 * no existing link moves (what a change of numbering does). */
gboolean
axis_tv_link_unlinked(
	AxisDbConn   *conn,
	gint64        series_id,
	guint        *linked,
	GError      **error
);

/* ── Imports (axis-tv-import.c) ─────────────────────────────────── */

gboolean
axis_tv_request_imports(
	AxisTv                   *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
);

/* ── The pages (axis-tv-web.c) ──────────────────────────────────── */

void
axis_tv_web_register(
	AxisTv  *self,
	AxisWeb *web
);

G_END_DECLS

#endif /* AXIS_TV_PRIVATE_H */
