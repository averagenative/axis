/*
 * axis-identify.h - identification: the films and series already in the
 * library matched to metadata titles, with evidence, a confidence and a
 * review by a person
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Roots of movies and roots for TV are identified by the same units, the
 * same evidence and the same rules (axis-identify-rules.h): a unit of a
 * root of movies is a film folder or file, one of a root for TV a series'
 * folder at the top of the root (its season folders and episodes below it)
 * or a video at the top.  A series' unit is looked up as a series; each of
 * its videos is read for the episodes its name holds
 * (axis_identify_episodes_read(), library_identity_episode), and which
 * episodes of the series' data those are is read against the metadata when
 * shown (the view library_episode_file, 0045_identify_series.sql).  The
 * seasons a linked series needs are fetched once by the runs, within the
 * provider's limits; the metadata layer keeps them fresh.
 *
 * What it promises:
 *
 *  - It only reads.  The index (library_file), the stored probe results, and
 *    sidecar .nfo files, which are opened through a volume guard taken to
 *    read, beneath it one component at a time (no symbolic link followed, a
 *    regular file only), and of which at most AXIS_IDENTIFY_NFO_MAX bytes are
 *    read and scanned as bytes (axis-identify-ids.h).  Nothing is renamed,
 *    moved, created, deleted or changed on any volume, and no .nfo is written.
 *  - Only a sure match is linked by itself (axis-identify-rules.h says
 *    exactly when a match is sure).  Likely and unknown units wait for an
 *    administrator on the Identify page with their candidates and reasons.
 *    Once a person has decided about a unit, nothing links it by itself
 *    again.
 *  - Paced.  A run looks at a bounded number of scopes of one root
 *    (identify.units_per_run), the newest first, and stops early when the
 *    metadata provider does not answer (that is "try again later", never
 *    "unknown").  Every lookup goes through the metadata layer, its store
 *    first and its rate limit.  A scope whose videos' names, sizes and probe
 *    state are as recorded is never looked at again.
 *  - Identity is apart from management.  A unit says what it is whether or
 *    not its film is managed; an owner (the movies module) makes its own
 *    links from a linked identity, now and when the film is added later, and
 *    drops them when the link is undone.  An import's link of a file wins
 *    over identification's.  The owner lists a linked title that it does not
 *    manage as a movie on disk, read from these tables at query time.
 *  - Every person's action is an administrator's, recorded with the user
 *    (library_identity_action).
 *
 * Threads: everything here blocks on the database, the metadata provider or
 * a volume: call it from a worker thread.  Create and free on the main
 * thread.
 */

#ifndef AXIS_IDENTIFY_H
#define AXIS_IDENTIFY_H

#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-conn.h"
#include "db/axis-db-pool.h"
#include "identify/axis-identify-rules.h"
#include "jobs/axis-job-context.h"
#include "meta/axis-meta.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_IDENTIFY_MODULE_ID            "identify"

/* One root's run (scan queue, one at a time per root: de-duplicated by
 * AXIS_IDENTIFY_KEY_PREFIX and the root's id), and the scheduler that queues
 * one for every films root that has something to look at (maintenance). */
#define AXIS_IDENTIFY_KIND                 "identify.root"
#define AXIS_IDENTIFY_KEY_PREFIX           "identify:"
#define AXIS_IDENTIFY_SCHEDULE_KIND        "identify.schedule"
#define AXIS_IDENTIFY_SCHEDULE             "identify.schedule"
#define AXIS_IDENTIFY_SCHEDULE_INTERVAL_S  600

/* The schedule (of the same kind, payload {"continue": true}) that queues
 * the next run of a root whose last run left work, every minute: a run looks
 * at a bounded batch and the next follows at once, not ten minutes later. */
#define AXIS_IDENTIFY_CONTINUE             "identify.continue"
#define AXIS_IDENTIFY_CONTINUE_INTERVAL_S  60

#define AXIS_IDENTIFY_SETTING_PER_RUN      "identify.units_per_run"
#define AXIS_IDENTIFY_SETTING_PERCENT      "identify.runtime_percent"
#define AXIS_IDENTIFY_SETTING_MINUTES      "identify.runtime_minutes"

/* How long one lookup waits for the provider in a run, and a run's budget. */
#define AXIS_IDENTIFY_WAIT_MS              8000
#define AXIS_IDENTIFY_RUN_BUDGET_MS        45000

/* How long a run waits after the provider did not answer.  A unit whose
 * full record was not answered waits this long, then twice as long at each
 * try, up to AXIS_IDENTIFY_WAIT_MAX_S. */
#define AXIS_IDENTIFY_RETRY_S              900
#define AXIS_IDENTIFY_WAIT_MAX_S           86400

/* At most this many videos of one folder are looked at, and .nfo files read
 * per unit; of a series' folder (every season), this many. */
#define AXIS_IDENTIFY_MAX_VIDEOS           64
#define AXIS_IDENTIFY_MAX_NFO              2
#define AXIS_IDENTIFY_MAX_EPISODE_FILES    5000

/* A run of a root for TV fetches at most this many seasons its linked
 * series need (the rest on the next run), and waits this long for each. */
#define AXIS_IDENTIFY_SEASONS_PER_RUN      200

#define AXIS_IDENTIFY_ERROR (axis_identify_error_quark())

/**
 * AxisIdentifyError:
 * @AXIS_IDENTIFY_ERROR_NOT_FOUND: no such unit, root or title
 * @AXIS_IDENTIFY_ERROR_INVALID: the request cannot be made (the message says
 *   why)
 * @AXIS_IDENTIFY_ERROR_UNAVAILABLE: no owner manages films, or no provider
 *   serves them
 */
typedef enum
{
	AXIS_IDENTIFY_ERROR_NOT_FOUND,
	AXIS_IDENTIFY_ERROR_INVALID,
	AXIS_IDENTIFY_ERROR_UNAVAILABLE
} AxisIdentifyError;

GQuark
axis_identify_error_quark(void);

/* ── The owners of films and of series ──────────────────────────── */

/**
 * AxisIdentifyOwnerInfo:
 * @module: the owner's module ("movies", "tv")
 * @link: in the caller's transaction: the identity @identity_id is
 *   @title_id now (its files are in library_identity_file); the owner makes
 *   its links from it when it manages the title, replacing those it made
 *   from this identity before.  @user: (nullable) a person, or %NULL for a
 *   sure match.
 * @unlink: in the caller's transaction: the identity's links go
 * @manage: every title linked in @root_id that the owner does not manage
 *   yet is managed now, with @profile_id (0: the default) and @monitored
 *   (a film: for upgrades; a series: its future episodes, else nothing),
 *   searching nothing; *@added and *@skipped (managed already) say how many
 * @managed: whether the owner manages @title_id
 * @count_managed: how many of the titles linked in @root_id it manages
 * @kind: the titles it owns: %AXIS_META_KIND_FILM (a root of movies) or
 *   %AXIS_META_KIND_SERIES (a root for TV)
 */
typedef struct
{
	const gchar *module;
	gboolean   (*link)(gpointer data, AxisDbConn *conn, gint64 identity_id, gint64 title_id, const gchar *user,
	                   GError **error);
	gboolean   (*unlink)(gpointer data, AxisDbConn *conn, gint64 identity_id, const gchar *user, GError **error);
	gboolean   (*manage)(gpointer data, gint64 root_id, gint64 profile_id, gboolean monitored, const gchar *user,
	                     guint *added, guint *skipped, GError **error);
	gboolean   (*managed)(gpointer data, AxisDbConn *conn, gint64 title_id, gboolean *managed, GError **error);
	gboolean   (*count_managed)(gpointer data, AxisDbConn *conn, gint64 root_id, guint *managed, GError **error);
	AxisMetaKind kind;
} AxisIdentifyOwnerInfo;

/* ── The layer ──────────────────────────────────────────────────── */

typedef struct _AxisIdentify AxisIdentify;

/**
 * axis_identify_new:
 * @pool: the database, which must outlive the layer
 * @settings: (nullable): the settings store (%NULL: the defaults)
 * @storage, @meta: the layers it reads through
 *
 * Registers the identified titles as references of the metadata layer, so
 * that they are kept fresh.  Main thread, before the metadata layer is used.
 *
 * Returns: (transfer full): the layer, reference counted
 */
AxisIdentify *
axis_identify_new(
	AxisDbPool   *pool,
	AxisSettings *settings,
	AxisStorage  *storage,
	AxisMeta     *meta
);

AxisIdentify *
axis_identify_ref(AxisIdentify *self);

void
axis_identify_unref(AxisIdentify *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIdentify, axis_identify_unref)

/* The settings, ended by an entry whose key is %NULL. */
const AxisSettingInfo *
axis_identify_get_setting_table(void);

/* Main thread, before use; @info and @data must outlive the layer.  One
 * owner of each kind (@info->kind). */
void
axis_identify_add_owner(
	AxisIdentify                *self,
	const AxisIdentifyOwnerInfo *info,
	gpointer                     data
);

/* The films' owner's module ("movies"), or %NULL when there is none.  While
 * that module is on, its pages show every linked title with its files,
 * managed or not (the movies module lists the movies on disk it does not
 * manage). */
const gchar *
axis_identify_get_owner_module(AxisIdentify *self);

/* The owner of @kind's module ("movies", "tv"), or %NULL. */
const gchar *
axis_identify_get_owner_module_for(
	AxisIdentify *self,
	AxisMetaKind  kind
);

/* What a root of @media_type ("movies", "tv") is identified as: a film, a
 * series, or 0 for a root of any other type. */
AxisMetaKind
axis_identify_root_kind(const gchar *media_type);

AxisDbPool *
axis_identify_get_pool(AxisIdentify *self);

AxisMeta *
axis_identify_get_meta(AxisIdentify *self);

/**
 * AxisIdentifyTuning:
 * @wait_ms: one lookup's wait (0: AXIS_IDENTIFY_WAIT_MS)
 * @budget_ms: one run's time (0: AXIS_IDENTIFY_RUN_BUDGET_MS)
 * @latest_year: the latest year names are believed up to (0: next year)
 *
 * For tests.
 */
typedef struct
{
	guint wait_ms;
	guint budget_ms;
	gint  latest_year;
} AxisIdentifyTuning;

void
axis_identify_set_tuning(
	AxisIdentify             *self,
	const AxisIdentifyTuning *tuning
);

/* ── Runs ───────────────────────────────────────────────────────── */

/**
 * AxisIdentifyReport:
 * @skipped: (nullable): why nothing was looked at (static text)
 * @scopes: scopes the root has
 * @examined: scopes looked at by this run
 * @pending: new or changed scopes left after it
 * @removed: scopes gone from the root, forgotten with their units
 * @units: units looked at
 * @linked: of those, linked as sure
 * @review: of those, left for a person
 * @waiting: of those, waiting for the provider
 * @provider_down: the run stopped because the provider did not answer
 * @seasons: a root for TV: seasons of its linked series fetched by this run
 * @seasons_pending: such seasons left for the next run
 */
typedef struct
{
	const gchar *skipped;
	guint        scopes;
	guint        examined;
	guint        pending;
	guint        removed;
	guint        units;
	guint        linked;
	guint        review;
	guint        waiting;
	gboolean     provider_down;
	guint        seasons;
	guint        seasons_pending;
} AxisIdentifyReport;

/**
 * axis_identify_run_root:
 * @context: (nullable): the job, for cancellation and progress
 * @report: (out) (optional)
 *
 * One run over a root of movies or for TV (see the promises above); for TV,
 * then the seasons its linked series need whose episodes were never
 * fetched, at most %AXIS_IDENTIFY_SEASONS_PER_RUN.  Fails only for a
 * database error or cancellation; a volume away, a provider down or a root
 * not scanned yet are the report's.
 */
gboolean
axis_identify_run_root(
	AxisIdentify        *self,
	gint64               root_id,
	AxisJobContext      *context,
	AxisIdentifyReport  *report,
	GError             **error
);

/* The scheduler's work: a run queued for a root of movies or for TV with a
 * complete scan only when something could have changed since its last run
 * began: it never ran, a scan of it completed since, a file of it was
 * probed since, the wait after a provider that did not answer is over, or
 * units (or a TV root's seasons) were left to look at (an import's link
 * leaves one).  *@queued: how many were queued. */
gboolean
axis_identify_run_schedule(
	AxisIdentify  *self,
	guint         *queued,
	GError       **error
);

/* The work of the minute's schedule: a run queued for each root of movies
 * or for TV whose last run left units (or a TV root's seasons) to look at
 * and is not waiting for the provider; one look at a row per root, so a
 * quiet library costs nothing.  *@queued: how many were queued. */
gboolean
axis_identify_run_continue(
	AxisIdentify  *self,
	guint         *queued,
	GError       **error
);

/* Queues a run of @root_id now ("look again"), or finds the one queued. */
gboolean
axis_identify_request(
	AxisIdentify  *self,
	gint64         root_id,
	gboolean      *created,
	GError       **error
);

/* ── Reading ────────────────────────────────────────────────────── */

/**
 * AxisIdentifyRootInfo:
 *
 * A root of movies or for TV as the Identify page shows it (@kind: film or
 * series).  Counts are of units: @sure linked by themselves (or by an
 * import), @confirmed linked by a person, @likely and @unknown waiting for
 * one, @not_film marked by one (not a movie, or not a series), @waiting for
 * the provider, @doubted linked and listed for a person because a later
 * look did not confirm the link.  @titles: the distinct titles linked;
 * @managed: of those,
 * the ones the owner manages.  @seasons_pending: a TV root's seasons still
 * to fetch.
 */
typedef struct
{
	gint64          root_id;
	AxisMetaKind    kind;
	guint           seasons_pending;
	gchar          *name;
	gchar          *volume_name;
	AxisVolumeState volume_state;
	gboolean        scanned;
	guint           scopes;
	guint           pending;
	guint           sure;
	guint           confirmed;
	guint           likely;
	guint           unknown;
	guint           not_film;
	guint           waiting;
	guint           doubted;
	guint           titles;
	guint           managed;
	gint64          last_run_us;
	gchar          *last_run_text;
	gint64          retry_after_us;
	gboolean        running;
} AxisIdentifyRootInfo;

void
axis_identify_root_info_free(AxisIdentifyRootInfo *info);

/* Every root of movies and for TV, the movies' first, each kind by name. */
gboolean
axis_identify_list_roots(
	AxisIdentify  *self,
	GPtrArray    **roots,
	GError       **error
);

/* Fails with %AXIS_IDENTIFY_ERROR_NOT_FOUND for a root that is not one of
 * movies or for TV. */
gboolean
axis_identify_get_root(
	AxisIdentify          *self,
	gint64                 root_id,
	AxisIdentifyRootInfo **info,
	GError               **error
);

/* A video of a unit. */
typedef struct
{
	gint64  file_id;
	gchar  *rel_path;
	gint64  size;
	gint64  duration_ms;	/* 0: not probed */
	gboolean best;
	gchar  *quality;	/* ladder name, or NULL */
	gchar  *resolution;	/* with no quality, or NULL */
	gchar  *quality_text;
	/* A series' file: why its name holds no episode (NULL: it does), and
	 * what it holds or that reason in words. */
	gchar  *episode_code;
	gchar  *episode_text;
} AxisIdentifyFileInfo;

/* A title shown as a candidate: the stored record (brief) and why.
 * @seasons: a series' numbered seasons the stored record lists (specials
 * not counted), 0 when it lists none. */
typedef struct
{
	AxisMetaTitle *title;
	gchar         *why;
	gchar         *poster;	/* the stored poster's content hash, or NULL */
	guint          seasons;
} AxisIdentifyCandidateInfo;

/* A person's action on a unit. */
typedef struct
{
	gchar  *action;
	gchar  *by_user;
	gchar  *text;
	gint64  created_us;
} AxisIdentifyActionInfo;

/**
 * AxisIdentifyUnitInfo:
 *
 * A unit as the pages show it.  @rel_path is stored form, relative to the
 * root.  @title is the linked title (or, under review, the best candidate's
 * label is in @candidates).  @doubt: (nullable) a link not made by a person
 * that a later look did not confirm: it stands, and this says why it is
 * listed for a person.
 */
typedef struct
{
	gint64      id;
	gint64      root_id;
	AxisMetaKind kind;	/* what its root holds: film or series */
	gchar      *root_name;
	gchar      *unit_kind;
	gchar      *rel_path;
	gchar      *state;
	gchar      *confidence;
	gint64      title_id;
	gchar      *title_label;
	gchar      *linked_how;
	gchar      *decided_by;
	gint64      decided_us;
	gboolean    held;
	GPtrArray  *reasons;	/* AxisIdentifyReason */
	GPtrArray  *candidates;	/* AxisIdentifyCandidateInfo, best first (filled by axis_identify_get_unit) */
	gchar      *read_text;
	gchar      *read_title;
	gint        read_year;
	gint64      duration_ms;
	gchar      *problem;
	gint64      examined_us;
	GPtrArray  *files;	/* AxisIdentifyFileInfo */
	GPtrArray  *actions;	/* AxisIdentifyActionInfo (filled by axis_identify_get_unit) */
	gboolean    managed;
	gchar      *doubt;
} AxisIdentifyUnitInfo;

void
axis_identify_unit_info_free(AxisIdentifyUnitInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisIdentifyUnitInfo, axis_identify_unit_info_free)

typedef enum
{
	AXIS_IDENTIFY_LIST_LIKELY,	/* review, likely */
	AXIS_IDENTIFY_LIST_UNKNOWN,	/* review, unknown */
	AXIS_IDENTIFY_LIST_LINKED,	/* linked, the latest first */
	AXIS_IDENTIFY_LIST_NOT_FILM,
	AXIS_IDENTIFY_LIST_WAITING,
	AXIS_IDENTIFY_LIST_DOUBT	/* linked, a later look not confirming it */
} AxisIdentifyList;

/**
 * axis_identify_list_units:
 * @with_candidates: fill each unit's candidates (with their posters)
 * @total: (out) (optional): how many there are in all
 *
 * @limit units of @root_id of the list, from @offset, by path (the linked:
 * the latest first), each with its files.
 */
gboolean
axis_identify_list_units(
	AxisIdentify      *self,
	gint64             root_id,
	AxisIdentifyList   list,
	guint              offset,
	guint              limit,
	gboolean           with_candidates,
	GPtrArray        **units,
	guint             *total,
	GError           **error
);

/**
 * axis_identify_list_neighbours:
 * @next_id: (out): the unit after @unit_id in the list's order, or 0
 * @previous_id: (out): the unit before it, or 0
 * @position: (out): how many of the list come before it
 *
 * Where @unit_id stands in @list of its root, in the order
 * axis_identify_list_units() reads it, whether or not it is in the list
 * itself: for a page that takes a person back to the card after the one
 * they acted on.  An unknown unit has no neighbours.
 */
gboolean
axis_identify_list_neighbours(
	AxisIdentify      *self,
	gint64             unit_id,
	AxisIdentifyList   list,
	gint64            *next_id,
	gint64            *previous_id,
	guint             *position,
	GError           **error
);

/* One unit with its candidates, files and actions; fails with
 * %AXIS_IDENTIFY_ERROR_NOT_FOUND. */
gboolean
axis_identify_get_unit(
	AxisIdentify          *self,
	gint64                 unit_id,
	AxisIdentifyUnitInfo **info,
	GError               **error
);

/* The units linked to @title_id, with their files; *@managed: whether the
 * owner manages the title. */
gboolean
axis_identify_units_of_title(
	AxisIdentify  *self,
	gint64         title_id,
	GPtrArray    **units,
	gboolean      *managed,
	GError       **error
);

/**
 * AxisIdentifyFolder:
 * @dir_id: the library folder (its library_dir row)
 * @unit_id: the unit it is
 * @state: "linked", "review", "not_film" or "waiting"
 * @title: (nullable): when linked, the title: a brief record (id, kind,
 *   provider, provider_id, title, year, poster_ref)
 * @poster: (nullable): that title's kept small poster's hash (one not kept
 *   yet is asked for in the background)
 */
typedef struct
{
	gint64         dir_id;
	gint64         unit_id;
	gchar         *state;
	AxisMetaTitle *title;
	gchar         *poster;
} AxisIdentifyFolder;

void
axis_identify_folder_free(AxisIdentifyFolder *folder);

/**
 * axis_identify_folders:
 * @dir_ids: (array length=n_dirs): library folders (library_dir ids), at
 *   most AXIS_IDENTIFY_FOLDERS_MAX
 * @folders: (out) (element-type AxisIdentifyFolder): those of them that
 *   are a unit (a film folder), in no order
 *
 * What a page of the Library's folders is: one statement, each folder
 * found by the units' unique index on their paths.  Blocks.
 */
#define AXIS_IDENTIFY_FOLDERS_MAX 500

gboolean
axis_identify_folders(
	AxisIdentify  *self,
	const gint64  *dir_ids,
	guint          n_dirs,
	GPtrArray    **folders,
	GError       **error
);

/* How many units of every root of movies and for TV wait for a person
 * (likely and unknown). */
gboolean
axis_identify_count_review(
	AxisIdentify  *self,
	guint         *count,
	GError       **error
);

/* The same, of the roots of movies (*@films) and for TV (*@series) apart,
 * in one statement. */
gboolean
axis_identify_count_review_by_kind(
	AxisIdentify  *self,
	guint         *films,
	guint         *series,
	GError       **error
);

/* ── Acting (administrators) ────────────────────────────────────── */

/**
 * AxisIdentifyAction:
 * @AXIS_IDENTIFY_CONFIRM: link to the best candidate
 * @AXIS_IDENTIFY_CHOOSE: link to the stored title @title_id of the root's
 *   kind (a film, a series; a candidate, or one found by a search)
 * @AXIS_IDENTIFY_NOT_FILM: it is not of the root's kind (not a movie, not a
 *   series)
 * @AXIS_IDENTIFY_UNDO: a link or a "not a movie" is undone: back to review
 *
 * A person's link of a series' unit asks for a run of its root, which
 * fetches the seasons the series needs.
 */
typedef enum
{
	AXIS_IDENTIFY_CONFIRM,
	AXIS_IDENTIFY_CHOOSE,
	AXIS_IDENTIFY_NOT_FILM,
	AXIS_IDENTIFY_UNDO
} AxisIdentifyAction;

/* "confirm", "choose", "not_film", "undo". */
const gchar *
axis_identify_action_name(AxisIdentifyAction action);

gboolean
axis_identify_action_from_name(
	const gchar        *name,
	AxisIdentifyAction *action
);

/**
 * axis_identify_act:
 * @title_id: for %AXIS_IDENTIFY_CHOOSE, the film
 * @user: who
 *
 * In one transaction with the unit's row locked: the unit's state, the
 * owner's links, and the action with the user.  Fails with
 * %AXIS_IDENTIFY_ERROR_NOT_FOUND (no such unit or film) or
 * %AXIS_IDENTIFY_ERROR_INVALID (nothing to confirm, a title that is not a
 * film, nothing to undo).
 */
gboolean
axis_identify_act(
	AxisIdentify        *self,
	gint64               unit_id,
	AxisIdentifyAction   action,
	gint64               title_id,
	const gchar         *user,
	GError             **error
);

/**
 * axis_identify_add_all:
 * @profile_id: a quality profile (0: the default)
 * @monitored: monitored for upgrades (else only shown)
 * @added: (out) (optional): films managed now
 * @skipped: (out) (optional): films that were managed already
 *
 * Manages every film linked in @root_id through the owner, searching for
 * none of them, and records it with @user.  Fails with
 * %AXIS_IDENTIFY_ERROR_UNAVAILABLE when no owner manages films.
 */
gboolean
axis_identify_add_all(
	AxisIdentify  *self,
	gint64         root_id,
	gint64         profile_id,
	gboolean       monitored,
	const gchar   *user,
	guint         *added,
	guint         *skipped,
	GError       **error
);

G_END_DECLS

#endif /* AXIS_IDENTIFY_H */
