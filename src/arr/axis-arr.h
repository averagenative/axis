/*
 * axis-arr.h - bringing a household's Radarr and Sonarr over: the
 * connections, the reading and the preview
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * openspec/specs/arr-migration/spec.md, slices R1 to R3 (Radarr) and S1 (Sonarr:
 * its connection, its reading and its preview).  The connections, the
 * reading and the preview write nothing to the library: no movie or series
 * is added and no file is linked.  One confirmed run then brings what the
 * preview showed (the last section of this header, axis-arr-apply.c): a
 * Radarr's movies through the movies module, a Sonarr's series through the TV
 * module (slice S2), by the connection's app and nothing else.
 *
 * One machinery serves both apps: a connection, its key, its test, the job
 * arr.read, the plan and its items, the preview with its two mapping tables,
 * the limits, forgetting.  What an app has of its own is the title it lists
 * (a movie by its TMDb identifier, a series by its TheTVDB identifier with
 * its seasons) and, for Sonarr, the two lists it gives for each series (its
 * episodes and its episode files), which the reading asks for one series at
 * a time within the limits below.  A connection is of one app for good: a
 * key is read only together with its connection's address and app, the first
 * request to an address is system/status, and nothing else is asked until
 * that answer names the app the connection was made for.
 *
 * A series from Sonarr is matched by its TheTVDB identifier and would follow
 * TheTVDB's aired order (%AXIS_ARR_SERIES_SOURCE), the numbering Sonarr's
 * seasons, episodes and monitoring are in.  While Axis has no TheTVDB key no
 * series is brought: a reading of a Sonarr is refused saying so, and a plan
 * read before the key went skips every series for that reason
 * (axis_arr_series_ready()).
 *
 *  - A connection (arr_source) is an app, a name, an address and an API key.
 *    The key is a secret: stored in a table of its own, read in one place
 *    (for a request), handed to nothing but the HTTP session made for that
 *    connection's own origin, never returned by any function here, never
 *    logged and never in an error.  Every error this layer makes is fixed
 *    words: nothing an arr app answered is ever part of one.
 *  - Axis only reads from the arr app: every request is a GET through a
 *    session of purpose %AXIS_HTTP_PURPOSE_READ (core/axis-http.h), which
 *    cannot send anything else.  A redirect to another origin is not
 *    followed.
 *  - Nothing is asked of an address until its system/status answer has
 *    named the app the connection is for: the test asks only that, and a
 *    reading asks it first.
 *  - A reading is the job arr.read: it asks the app for its quality
 *    profiles, root folders, download queue and movies, stores them as a
 *    plan (arr_plan, arr_plan_item), and matches each movie to a stored
 *    title by its TMDb identifier, from the store first and then from the
 *    metadata provider within its limits.
 *  - The preview is computed from the plan and Axis's own tables under the
 *    mappings and options stored on the connection.  Opening it writes
 *    nothing; changing it writes the mappings and options to the connection
 *    and nothing else.
 *
 * Threads: everything here blocks on the database or the network: call it
 * from a worker thread.  Create and stop the layer on the main thread.
 */

#ifndef AXIS_ARR_H
#define AXIS_ARR_H

#include <glib.h>

#include "arr/axis-arr-parse.h"
#include "db/axis-db-pool.h"
#include "jobs/axis-job-context.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"

G_BEGIN_DECLS

#define AXIS_ARR_MODULE_ID         "bring-over"
#define AXIS_ARR_READ_KIND         "arr.read"
#define AXIS_ARR_CLEANUP_KIND      "arr.cleanup"
#define AXIS_ARR_CLEANUP_SCHEDULE  "arr.cleanup"
#define AXIS_ARR_CLEANUP_INTERVAL_S 3600
#define AXIS_ARR_READ_KEY_PREFIX   "arr.read:"
#define AXIS_ARR_APPLY_KIND        "arr.apply"
#define AXIS_ARR_APPLY_KEY_PREFIX  "arr.apply:"

/* A plan is used for this long after its reading began. */
#define AXIS_ARR_PLAN_KEEP_S       (24 * 3600)

/* The largest answer read for the list of every movie, and its deadline;
 * every other request's (design.md, section 11). */
#define AXIS_ARR_LIST_MAX_BYTES    (128u * 1024u * 1024u)
#define AXIS_ARR_LIST_TIMEOUT_MS   120000
#define AXIS_ARR_ANSWER_MAX_BYTES  (16u * 1024u * 1024u)
#define AXIS_ARR_REQUEST_TIMEOUT_MS 15000

/*
 * Sonarr gives a series' episodes and its episode files as two lists for
 * each series.  One reading asks for them within these, and fails as a whole
 * (keeping nothing) when one is passed:
 *  - one list's size and its deadline;
 *  - how many series' lists are asked for (two requests each);
 *  - the bytes of all those lists together;
 *  - one deadline for all of them;
 *  - how many episodes and episode files a plan holds.
 * The requests are paced at AXIS_ARR_REQUESTS_PER_SECOND (axis-arr-assumed.h),
 * one at a time: 500 series are about four minutes.  Each list is read,
 * reduced to a few numbers an episode, stored and freed before the next is
 * asked for, so the memory taken is one list's, whatever the library's size.
 */
#define AXIS_ARR_SERIES_MAX_BYTES     (32u * 1024u * 1024u)
#define AXIS_ARR_SERIES_TIMEOUT_MS    60000
#define AXIS_ARR_SERIES_READ_MAX      5000
#define AXIS_ARR_SERIES_TOTAL_BYTES   (G_GUINT64_CONSTANT(1024) * 1024 * 1024)
/* The one deadline of the whole reading, matching included. */
#define AXIS_ARR_SERIES_DEADLINE_MS   (60 * 60 * 1000)
/* A list that does not answer is asked for once more after this pause, then
 * its series is incomplete; this many series in a row so end and the app is
 * taken to be down. */
#define AXIS_ARR_SERIES_RETRY_MS      2000
#define AXIS_ARR_SERIES_GIVE_UP       20
#define AXIS_ARR_PLAN_MAX_EPISODES    1000000
#define AXIS_ARR_PLAN_MAX_FILES       500000

/* The source of numbering a series brought from Sonarr follows
 * (series.episode_source): TheTVDB's aired order. */
#define AXIS_ARR_SERIES_SOURCE     "tvdb"
/* ... and the metadata provider whose numbering that is. */
#define AXIS_ARR_SERIES_PROVIDER   "tvdb"

/* Paged lists: records a page, and the most pages read. */
#define AXIS_ARR_PAGE_SIZE         250
#define AXIS_ARR_MAX_PAGES         400

/* How long one title's lookup waits for the metadata provider, and after
 * how many in a row without an answer the reading stops asking. */
#define AXIS_ARR_LOOKUP_WAIT_MS    10000
#define AXIS_ARR_LOOKUP_GIVE_UP    5

/* The most titles one reading asks the provider for: a library of many
 * thousands the store does not have yet is read again to go on (what the
 * first reading looked up is in the store by then).  And the one deadline
 * of reading the download queue, page after page. */
#define AXIS_ARR_LOOKUP_MAX        2000
#define AXIS_ARR_QUEUE_DEADLINE_MS (5 * 60 * 1000)

/* Rows of every list on the preview. */
#define AXIS_ARR_PAGE_ROWS         100

/* Folder rows and profile choices kept in a connection's mappings. */
#define AXIS_ARR_MAX_FOLDER_ROWS   64

#define AXIS_ARR_KEY_MAX_CHARS     1024

#define AXIS_ARR_ERROR (axis_arr_error_quark())

typedef enum
{
	AXIS_ARR_ERROR_NOT_FOUND,	/* no such connection */
	AXIS_ARR_ERROR_INVALID,		/* what was typed is refused; the message names the field */
	AXIS_ARR_ERROR_BUSY,		/* a reading of the connection is under way */
	AXIS_ARR_ERROR_REFUSED,		/* the app refused the key */
	AXIS_ARR_ERROR_NOT_APP,		/* the address answers, but not as the app */
	AXIS_ARR_ERROR_NO_ANSWER,
	AXIS_ARR_ERROR_CERTIFICATE,
	AXIS_ARR_ERROR_TOO_LARGE,
	AXIS_ARR_ERROR_UNREADABLE,	/* the app's answer could not be read */
	AXIS_ARR_ERROR_CANCELLED,
	AXIS_ARR_ERROR_FAILED,
	AXIS_ARR_ERROR_STALE,		/* the preview changed since its confirmation was shown */
	AXIS_ARR_ERROR_NEEDS_TICK,	/* Radarr did not answer: the person's statement is needed */
	AXIS_ARR_ERROR_NO_TVDB_KEY	/* a Sonarr cannot be read: Axis has no TheTVDB key */
} AxisArrError;

GQuark
axis_arr_error_quark(void);

typedef struct _AxisArr AxisArr;

AxisArr *
axis_arr_new(
	AxisDbPool *pool,
	AxisMeta   *meta
);

AxisArr *
axis_arr_ref(AxisArr *self);

void
axis_arr_unref(AxisArr *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArr, axis_arr_unref)

/* Requests in flight are cancelled, and no more are made. */
void
axis_arr_stop(AxisArr *self);

AxisDbPool *
axis_arr_get_pool(AxisArr *self);

/**
 * AxisArrTuning:
 *
 * For tests: a 0 (or %NULL) leaves the default.  @provider is the metadata
 * provider asked for a TMDb identifier (the default is TMDb itself); it must
 * be a static string.
 */
typedef struct
{
	guint        request_timeout_ms;
	guint        list_timeout_ms;
	gsize        list_max_bytes;
	guint        page_size;
	guint        lookup_wait_ms;
	guint        lookup_give_up;
	guint        lookup_max;
	guint        queue_deadline_ms;
	const gchar *provider;
	/* Sonarr's lists for each series: requests a second, one list's size and
	 * deadline, series asked about, bytes in all, the one deadline. */
	guint        requests_per_second;
	gsize        series_max_bytes;
	guint        series_timeout_ms;
	guint        series_read_max;
	guint64      series_total_bytes;
	guint        series_deadline_ms;	/* of the whole reading */
	guint        series_retry_ms;
} AxisArrTuning;

void
axis_arr_set_tuning(
	AxisArr             *self,
	const AxisArrTuning *tuning
);

/**
 * axis_arr_series_ready:
 *
 * Whether series can be brought from a Sonarr now: Axis has a TheTVDB key
 * (metadata.tvdb_key; the provider is offered).  Without one the reading of a
 * Sonarr is not offered and is refused, and the preview skips every series
 * as "No TheTVDB key in Axis".  Any thread.
 */
gboolean
axis_arr_series_ready(AxisArr *self);

/* ── Connections ────────────────────────────────────────────────── */

typedef enum
{
	AXIS_ARR_TEST_NONE,		/* never tested */
	AXIS_ARR_TEST_OK,
	AXIS_ARR_TEST_REFUSED,
	AXIS_ARR_TEST_NOT_APP,
	AXIS_ARR_TEST_NO_ANSWER,
	AXIS_ARR_TEST_CERTIFICATE
} AxisArrTest;

/**
 * AxisArrSource:
 * @secret_set: a key is stored (the key itself is in no struct)
 * @version: (nullable): the app's version, when the last test or reading
 *   found the app
 * @mappings: the preview's tables and options, a JSON object as text
 */
typedef struct
{
	gint64       id;
	AxisArrApp   app;
	gchar       *name;
	gchar       *origin;
	gchar       *path_prefix;
	gboolean     accept_untrusted;
	gboolean     secret_set;
	gchar       *made_by;
	AxisArrTest  last_test;
	gchar       *version;
	gint64       tested_us;
	gint64       created_us;
	gchar       *mappings;
} AxisArrSource;

void
axis_arr_source_free(AxisArrSource *source);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArrSource, axis_arr_source_free)

/**
 * AxisArrSourceSpec:
 * @app: "radarr" or "sonarr"; a connection keeps the app it was made for
 *   (saving it with another is %AXIS_ARR_ERROR_NOT_FOUND, and writes nothing)
 * @address: as typed (axis_arr_address_split())
 * @secret: (nullable): the API key; %NULL or "" keeps the stored one
 * @user: (nullable): who saves it
 */
typedef struct
{
	const gchar *app;
	const gchar *name;
	const gchar *address;
	gboolean     accept_untrusted;
	const gchar *secret;
	const gchar *user;
} AxisArrSourceSpec;

/* (element-type AxisArrSource), by name. */
gboolean
axis_arr_list_sources(
	AxisArr    *self,
	GPtrArray **sources,
	GError    **error
);

/* Fails with %AXIS_ARR_ERROR_NOT_FOUND. */
gboolean
axis_arr_get_source(
	AxisArr        *self,
	gint64          id,
	AxisArrSource **source,
	GError        **error
);

/**
 * axis_arr_save_source:
 * @id: the connection, 0 for a new one
 * @saved: (out) (optional): its id
 *
 * Fails with %AXIS_ARR_ERROR_INVALID, storing nothing, when a field is
 * refused; the message is a fixed sentence naming the field, never its
 * value.  A new connection needs a key.  A connection whose address changes
 * needs its key typed again: a stored key is only ever sent to the address
 * it was stored for.
 */
gboolean
axis_arr_save_source(
	AxisArr                  *self,
	gint64                    id,
	const AxisArrSourceSpec  *spec,
	gint64                   *saved,
	GError                  **error
);

/* Deletes the connection, its key and its plans. */
gboolean
axis_arr_delete_source(
	AxisArr  *self,
	gint64    id,
	GError  **error
);

/**
 * axis_arr_test_source:
 * @result: (out): what the app's system/status answer was
 *
 * One GET of system/status, recorded on the connection.  A connection
 * without a key is %AXIS_ARR_ERROR_INVALID; nothing is sent.
 *
 * Returns: %TRUE when the test was made (whatever it found)
 */
gboolean
axis_arr_test_source(
	AxisArr      *self,
	gint64        id,
	AxisArrTest  *result,
	GError      **error
);

/* "Radarr 5.2.6 answered.", "Radarr refused the API key.", "This address
 * answers, but not as Radarr.", ...; %NULL for %AXIS_ARR_TEST_NONE. */
gchar *
axis_arr_test_sentence(
	AxisArrApp   app,
	AxisArrTest  result,
	const gchar *version
);

/* ── The reading ────────────────────────────────────────────────── */

/**
 * AxisArrPlan:
 * @state: "reading", "complete" or "failed"
 * @problem: (nullable): why it failed
 * @expired: it began more than %AXIS_ARR_PLAN_KEEP_S ago
 * @job_live: its job is queued or running
 * @progress: the job's progress, 0 to 1 (-1 when it has none yet)
 * @progress_message: (nullable): where the job is
 */
typedef struct
{
	gint64    id;
	gint64    source_id;
	gchar    *state;
	gint64    started_us;
	gint64    finished_us;
	gchar    *started_by;
	gchar    *problem;
	guint     n_items;
	gboolean  expired;
	gboolean  job_live;
	gint64    job_id;
	gdouble   progress;
	gchar    *progress_message;
} AxisArrPlan;

void
axis_arr_plan_free(AxisArrPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArrPlan, axis_arr_plan_free)

/**
 * axis_arr_request_read:
 * @user: (nullable): who asks
 * @plan_id: (out) (optional): the plan the reading fills
 *
 * Queues the job arr.read for the connection, one at a time (its
 * de-duplication key): %AXIS_ARR_ERROR_BUSY when one is queued or running.
 * A Sonarr is not read while Axis has no TheTVDB key
 * (%AXIS_ARR_ERROR_NO_TVDB_KEY): nothing is asked of it and no plan is made.
 */
gboolean
axis_arr_request_read(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *user,
	gint64       *plan_id,
	GError      **error
);

/* The handler of arr.read (payload: "source" and "plan"). */
gboolean
axis_arr_run_read(
	AxisArr         *self,
	AxisJobContext  *context,
	GError         **error
);

/* The handler of arr.cleanup: plans older than a day go. */
gboolean
axis_arr_run_cleanup(
	AxisArr  *self,
	GError  **error
);

/**
 * axis_arr_plans:
 * @latest: (out) (transfer full) (nullable): the connection's newest plan
 * @usable: (out) (transfer full) (nullable) (optional): its newest complete
 *   plan that has not expired (perhaps the same)
 */
gboolean
axis_arr_plans(
	AxisArr      *self,
	gint64        source_id,
	AxisArrPlan **latest,
	AxisArrPlan **usable,
	GError      **error
);

/* ── The preview ────────────────────────────────────────────────── */

/* Why a title would not be brought, in the order they are decided. */
typedef enum
{
	AXIS_ARR_SKIP_NONE,		/* it would be brought */
	AXIS_ARR_SKIP_INCOMPLETE,
	AXIS_ARR_SKIP_TWICE,		/* the app lists the same movie more than once */
	AXIS_ARR_SKIP_NO_ID,
	AXIS_ARR_SKIP_UNKNOWN,
	AXIS_ARR_SKIP_NOT_LOOKED_UP,
	AXIS_ARR_SKIP_ALREADY,
	AXIS_ARR_SKIP_REMOVED,		/* this connection brought it, and it was removed in Axis since */
	AXIS_ARR_SKIP_UNMONITORED,
	AXIS_ARR_SKIP_PROFILE,
	AXIS_ARR_SKIP_FOLDER,
	AXIS_ARR_SKIP_OFFLINE,
	/* A series only (decided after "twice" and before the identifier). */
	AXIS_ARR_SKIP_NO_TVDB_KEY,	/* Axis has no TheTVDB key: no series is brought */
	AXIS_ARR_SKIP_NOT_LISTED,	/* TheTVDB's aired order does not list it */
	/* A kind of series Axis does not know (decided after the key).  A series
	 * Sonarr has as anime is no reason: it is brought as any series, and is of
	 * the kind anime in Axis (add-anime-support, A1). */
	AXIS_ARR_SKIP_UNKNOWN_KIND,
	AXIS_ARR_SKIP_N
} AxisArrSkip;

/* "already", "folder", ...: the reason in an address; and back. */
const gchar *
axis_arr_skip_key(AxisArrSkip skip);

gboolean
axis_arr_skip_from_key(
	const gchar *key,
	AxisArrSkip *skip
);

/* "Already in Axis", "Folder not mapped", ...  (@app names the app). */
gchar *
axis_arr_skip_label(
	AxisArrSkip skip,
	AxisArrApp  app
);

/* What stands between a file of the app's and its link. */
typedef enum
{
	AXIS_ARR_FILE_NONE,		/* the title has no file */
	AXIS_ARR_FILE_FOUND,		/* in the index at its path, of its size: it would be linked */
	AXIS_ARR_FILE_NOT_INDEXED,
	AXIS_ARR_FILE_OTHER_SIZE,
	AXIS_ARR_FILE_LINKED_ELSEWHERE,
	AXIS_ARR_FILE_UNMAPPABLE,
	AXIS_ARR_FILE_NO_PATH,
	AXIS_ARR_FILE_N
} AxisArrFileState;

gchar *
axis_arr_file_label(
	AxisArrFileState state,
	AxisArrApp       app
);

/* An id and a name: one of Axis's quality profiles. */
typedef struct
{
	gint64  id;
	gchar  *name;
} AxisArrChoice;

/* A library folder of the app's media type. */
typedef struct
{
	gint64    id;
	gchar    *name;
	gchar    *volume_name;
	gboolean  online;
	gint64    scanned_us;	/* its last complete scan, 0 for never */
} AxisArrRootChoice;

/*
 * Where a row's choice comes from.  A choice a person saved is theirs and is
 * never replaced ("none" and "not mapped" are choices too); a row with no
 * saved choice shows what Axis works out from what it already knows, and that
 * counts for the preview and for a run exactly as a saved choice does.
 */
typedef enum
{
	AXIS_ARR_FROM_SAVED = 0,	/* a person's saved choice */
	AXIS_ARR_FROM_NAME,		/* a profile: Axis has one of the same name */
	AXIS_ARR_FROM_PERSON,		/* a profile: the default of the person who read the app */
	AXIS_ARR_FROM_DEFAULT,		/* a profile: Axis's default profile */
	AXIS_ARR_FROM_FILES,		/* a folder: its files are in Axis's index there */
	AXIS_ARR_FROM_NOTHING,		/* no saved choice and nothing to suggest (see the row's why) */
} AxisArrFrom;

/* Why a folder row with no saved choice got no suggestion. */
typedef enum
{
	AXIS_ARR_UNSURE_NONE = 0,
	AXIS_ARR_UNSURE_NO_FILES,	/* the app has no file with a path and a size under it */
	AXIS_ARR_UNSURE_NOT_FOUND,	/* too few of its files are in the index under one place */
	AXIS_ARR_UNSURE_TWO,		/* its files are in the index under more than one place */
} AxisArrUnsure;

/* How a folder suggestion is decided: of at most this many of the app's files
 * for each of its root folders... */
#define AXIS_ARR_SUGGEST_SAMPLE   200
/* ... (and this many in all), */
#define AXIS_ARR_SUGGEST_MAX      10000
/* ... at least this share, in percent, and at least this many (or all of them,
 * when there are fewer) must be in Axis's index at the same path below one
 * place, with the size the app recorded; and one place only may reach that. */
#define AXIS_ARR_SUGGEST_PERCENT  80
#define AXIS_ARR_SUGGEST_MIN      3

/* A row of the quality profile table. */
typedef struct
{
	gint64       arr_id;
	gchar       *name;
	guint        n_titles;
	gint64       chosen;	/* one of Axis's, 0 for none */
	AxisArrFrom  from;
} AxisArrProfileRow;

/**
 * AxisArrFolderRow:
 * @prefix: the app's path, in its one form
 * @is_root: one of the app's root folders (else a row a person added, or a
 *   folder movies were found under)
 * @accessible: the app said it can reach it
 * @n_titles: titles of the plan under it
 * @root_id: the library folder chosen, 0 for none
 * @below: the path below the library folder, "" for its top
 * @n_files: the app's files under it
 * @n_found: of those, how many are in Axis's index at their mapped path with
 *   the size the app recorded
 * @from: %AXIS_ARR_FROM_SAVED, %AXIS_ARR_FROM_FILES (suggested: @root_id and
 *   @below are where its files were found) or %AXIS_ARR_FROM_NOTHING
 * @unsure: why nothing was suggested, for %AXIS_ARR_FROM_NOTHING
 * @n_sampled, @n_sample_found: the files looked at for a suggestion, and how
 *   many of them the best place holds
 */
typedef struct
{
	gchar         *prefix;
	gboolean       is_root;
	gboolean       accessible;
	guint          n_titles;
	gint64         root_id;
	gchar         *below;
	guint          n_files;
	guint          n_found;
	AxisArrFrom    from;
	AxisArrUnsure  unsure;
	guint          n_sampled;
	guint          n_sample_found;
} AxisArrFolderRow;

/* A title that would not be brought. */
typedef struct
{
	gchar       *title;	/* nullable: nothing of its record could be read */
	gint         year;
	guint        seq;	/* its place in the app's list, from 1 */
	AxisArrSkip  skip;
	gchar       *detail;	/* the disk's name, for %AXIS_ARR_SKIP_OFFLINE; "anime" for an anime series
	                        * of an older plan, for %AXIS_ARR_SKIP_NOT_LOOKED_UP */
	gint64       title_id;	/* the stored title, for a link; 0 for none */
} AxisArrSkipRow;

/* A file that would not be linked, of a title that would be brought. */
typedef struct
{
	gchar            *title;
	gint              year;
	AxisArrFileState  state;
	gchar            *arr_path;	/* as the app gives it; nullable */
	gchar            *mapped;	/* "LIBRARY FOLDER: path below it"; nullable */
	gchar            *other;	/* the title it is linked to; nullable */
} AxisArrFileRow;

/**
 * AxisArrBringRow:
 *
 * A series that would be brought, as the preview lists it: how Sonarr
 * monitors it, each of its seasons, how many of its episodes Sonarr monitors
 * and has a file for, where it would go, and how many of its files Axis's
 * index holds at their mapped paths.
 *
 * @seasons_on, @seasons_off: (nullable): the seasons Sonarr monitors and
 *   those it does not, as text ("1, 2, 4"; the specials are "specials")
 * @n_odd: entries of its two lists that could not be read
 */
typedef struct
{
	gchar    *title;
	gint      year;
	guint     seq;
	gboolean  monitored;
	gchar    *seasons_on;
	gchar    *seasons_off;
	guint     n_episodes;
	guint     n_episodes_monitored;
	guint     n_episodes_with_file;
	guint     n_files;
	guint     n_found;
	guint     n_odd;
	gchar    *profile;	/* Axis's quality profile it would get */
	gchar    *root;		/* the library folder it would get */
	/* Sonarr has it as anime (add-anime-support, A1): it would be migrated as
	 * any series and marked as anime in Axis.  What comparing Sonarr's
	 * absolute number of each episode with TheTVDB's gives, as a sentence and
	 * as the episodes that differ (nullable: none differ).  A report only: no
	 * count, decision or digest of the preview depends on it. */
	gboolean  anime;
	guint     absolute_differ;
	gchar    *absolute;
	gchar    *absolute_list;
} AxisArrBringRow;

/* Which page of each list a preview shows. */
typedef struct
{
	AxisArrSkip skip_filter;	/* %AXIS_ARR_SKIP_NONE for every reason */
	guint       skip_page;
	guint       file_page;
	guint       bring_page;		/* the series that would be brought (Sonarr) */
} AxisArrPages;

/**
 * AxisArrPreview:
 * @plan: (nullable): the plan it is drawn from; %NULL: there is none to
 *   preview (read first), and nothing else is set but @source and @latest
 * @latest: (nullable): the connection's newest plan, whatever its state
 * @counts: titles by reason (%AXIS_ARR_SKIP_NONE: they would be brought)
 * @files: of the titles that would be brought, their files by state
 * @skip_filter: the reason the list of skipped titles is cut to, or
 *   %AXIS_ARR_SKIP_NONE for all
 * @skipped: (element-type AxisArrSkipRow): one page
 * @n_skipped_listed: how many the filter leaves in all
 * @unusable: (element-type AxisArrFileRow): one page
 * @n_unusable: how many files would not be linked in all
 */
typedef struct
{
	AxisArrSource *source;
	AxisArrPlan   *plan;
	AxisArrPlan   *latest;
	guint          n_titles;
	guint          n_monitored;
	guint          n_with_file;
	guint          n_in_queue;
	guint          counts[AXIS_ARR_SKIP_N];
	guint          files[AXIS_ARR_FILE_N];
	gboolean       unmonitored;
	GPtrArray     *quality_profiles;	/* AxisArrChoice */
	GPtrArray     *roots;			/* AxisArrRootChoice */
	GPtrArray     *profile_rows;		/* AxisArrProfileRow */
	GPtrArray     *folder_rows;		/* AxisArrFolderRow */
	AxisArrSkip    skip_filter;
	guint          skip_page;
	GPtrArray     *skipped;
	guint          n_skipped_listed;
	guint          file_page;
	GPtrArray     *unusable;
	guint          n_unusable;
	/* Files of movies this connection brought before that Axis did not see
	 * then and holds now: a run links them. */
	guint          n_relink;
	/* A Sonarr's: such files of series that no longer follow TheTVDB's
	 * numbering in Axis (a person changed it since): never linked, and said. */
	guint          n_relink_renumbered;
	/* (nullable: no plan) What a confirmation of a run is of: the plan, the
	 * choices and options, and every count above. */
	gchar         *digest;
	/* (nullable) The digest of the same run with the tick "Start searching
	 * for these as soon as they are brought over": the tick box's value. */
	gchar         *digest_start;
	/* (nullable) The plan and the choices alone: what a run holds to while
	 * it works (its counts change as it brings). */
	gchar         *choices;
	/* A Sonarr's: whether series can be brought now (axis_arr_series_ready());
	 * what its lists held in all, and of the series that would be brought;
	 * and one page of those series. */
	gboolean       series_ready;
	guint          n_series_read;		/* series whose two lists were read */
	guint          n_episodes;
	guint          n_episodes_monitored;
	guint          n_episodes_with_file;
	guint          n_odd;			/* entries of those lists that could not be read */
	guint          bring_episodes;
	guint          bring_episodes_monitored;
	guint          bring_files;		/* their episode files, found or not */
	/* Of the titles that would be brought, those the app monitors (the others
	 * come unmonitored); a Sonarr's: the seasons and the episodes it monitors
	 * of those monitored series, which is the monitoring that comes with
	 * them. */
	guint          bring_monitored;
	guint          bring_seasons_on;
	guint          bring_episodes_on;
	/* Rows of the two tables that show a suggestion and no saved choice. */
	guint          n_suggested;
	guint          bring_page;
	GPtrArray     *bringing;		/* (nullable) AxisArrBringRow */
	/* Of the series that would be brought, those Sonarr has as anime. */
	guint          bring_anime;
} AxisArrPreview;

void
axis_arr_preview_free(AxisArrPreview *preview);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArrPreview, axis_arr_preview_free)

/**
 * axis_arr_preview:
 * @skip_filter: the reason to list, %AXIS_ARR_SKIP_NONE for every one
 * @skip_page: which page of the skipped titles, from 0
 * @file_page: which page of the files that would not be linked, from 0
 *
 * Reads only: no row of any table is written.
 */
gboolean
axis_arr_preview(
	AxisArr         *self,
	gint64           source_id,
	AxisArrSkip      skip_filter,
	guint            skip_page,
	guint            file_page,
	AxisArrPreview **preview,
	GError         **error
);

/* As axis_arr_preview(), with every page named (@pages nullable: the first
 * of each). */
gboolean
axis_arr_preview_pages(
	AxisArr             *self,
	gint64               source_id,
	const AxisArrPages  *pages,
	AxisArrPreview     **preview,
	GError             **error
);

/* One row of the folder table as a form gives it. */
typedef struct
{
	const gchar *prefix;	/* as typed */
	gint64       root_id;	/* 0 for none: "not mapped", a choice that is kept */
	const gchar *below;	/* as typed; nullable */
} AxisArrFolderInput;

/* One choice of the profile table as a form gives it. */
typedef struct
{
	gint64 arr_id;
	gint64 chosen;		/* 0 for none */
} AxisArrProfileInput;

/**
 * axis_arr_save_mappings:
 * @profiles: (array length=n_profiles): the profile table's choices
 * @folders: (array length=n_folders): the folder table's rows; one whose
 *   prefix is empty is left out
 * @unmonitored: also bring the movies the app does not monitor
 *
 * Writes the mappings and options to the connection, and nothing else.
 * Fails with %AXIS_ARR_ERROR_INVALID, writing nothing, when a row's path is
 * refused (a relative path, a "." or ".." segment, a character a stored
 * path cannot have) or there are too many rows.
 */
gboolean
axis_arr_save_mappings(
	AxisArr                    *self,
	gint64                      source_id,
	const AxisArrProfileInput  *profiles,
	guint                       n_profiles,
	const AxisArrFolderInput   *folders,
	guint                       n_folders,
	gboolean                    unmonitored,
	GError                    **error
);

/* ── The run (slice R3, axis-arr-apply.c) ───────────────────────── */

/*
 * One confirmed run brings what the preview showed: each movie through the
 * movies module's own Add, its file linked where Axis's index holds it at the
 * mapped path, its searches held.  The run asks Radarr nothing and opens no
 * file or folder on any disk: it reads the plan and Axis's own tables, and
 * writes rows.
 *
 * The decisions of 2026-11-21 on the proposal's questions are each in
 * one place:
 *   1 the connection is kept          nothing deletes it but "Forget"
 *   2 a brought file is kept          AXIS_MOVIE_BROUGHT_KEPT (axis-movies.h),
 *                                     AXIS_TV_BROUGHT_KEPT (axis-tv.h); let go
 *                                     by axis_arr_release_kept()
 *   3 searches are held until started axis_movies_bring() sets the hold;
 *                                     axis_arr_start_searching() ends it, and
 *                                     with the tick on the run's confirmation
 *                                     the run does at its end (start_at_end())
 *   5 no blocklist is brought         nothing reads the app's blocklist
 *   6 no history is brought           one line, written by axis_movies_bring()
 *   7 a file Axis cannot see          AXIS_MOVIE_HOLD_FILE_UNSEEN, there too
 *   8 Radarr's queue, when started    axis_arr_start_review()
 */

/**
 * axis_arr_set_movies:
 * @movies: (nullable): the movies module (a reference is kept)
 *
 * Without it nothing can be brought: a run fails saying so.  Main thread,
 * before use.
 */
void
axis_arr_set_movies(
	AxisArr    *self,
	AxisMovies *movies
);

/**
 * axis_arr_set_tv:
 * @tv: (nullable): the TV module (a reference is kept)
 *
 * What a run of a Sonarr adds series through (slice S2): without it none can
 * be brought, and a run of a Sonarr is refused saying so.  A connection's app
 * decides the module, and nothing else does: a Radarr's run never reaches
 * this one, nor a Sonarr's the movies module.  Main thread, before use.
 */
void
axis_arr_set_tv(
	AxisArr *self,
	AxisTv  *tv
);

/**
 * axis_arr_request_apply:
 * @digest: the digest of the preview the confirmation was shown with
 *
 * Queues the job arr.apply for the connection, one at a time.  Fails with
 * %AXIS_ARR_ERROR_STALE, queueing nothing, when the preview that would be
 * drawn now has another digest (the reading, a choice, an option or what the
 * library holds changed since the page was made), %AXIS_ARR_ERROR_INVALID
 * when there is nothing to bring, %AXIS_ARR_ERROR_BUSY when a run of the
 * connection is queued or running, %AXIS_ARR_ERROR_FAILED when the module of
 * the connection's app is not there, and %AXIS_ARR_ERROR_NO_TVDB_KEY for a
 * Sonarr while Axis has no TheTVDB key.
 *
 * @start: (nullable): the value the tick "Start searching for these as soon
 * as they are brought over" was posted with; %NULL or empty: not ticked.  The
 * tick is part of the run's digest: its value must be the digest of the run
 * with the tick (#AxisArrPreview.digest_start, axis_arr_run_digest()), else
 * %AXIS_ARR_ERROR_STALE.  It is stored with the job ("start"), and the run
 * then does at its end what "Start searching" does for the connection.
 */
gboolean
axis_arr_request_apply(
	AxisArr      *self,
	gint64        source_id,
	const gchar  *digest,
	const gchar  *start,
	const gchar  *user,
	GError      **error
);

/* The digest of a run of the preview whose digest is @digest: @digest itself
 * for a run without the tick, another (of @digest and the tick) with it. */
gchar *
axis_arr_run_digest(
	const gchar *digest,
	gboolean     start
);

/* The handler of arr.apply (payload: "source", "plan", "choices", "user",
 * "start").  With "start" true and every title taken, it reads the app's
 * download queue (GET) and starts the searches as "Start searching" does;
 * when the queue cannot be read it starts none and says so in the
 * connection's run note. */
gboolean
axis_arr_run_apply(
	AxisArr         *self,
	AxisJobContext  *context,
	GError         **error
);

/*
 * Which held titles "Start searching" would have searched for, as one
 * bounded statement can tell it.  The connection's card and the notice of
 * "Needs attention" count by these, so the two state the same numbers.
 *
 *  - A movie waits when it is held as brought ('brought': the hold "Start
 *    searching" ends), is monitored, and has no file present in the library.
 *    A movie with a file is not counted: a file that was migrated is kept and
 *    no automatic search replaces it, so starting changes nothing for it.
 *  - A series waits when it is held as brought and one of its monitored
 *    episodes, not gone and not held apart, has no file present.  An episode
 *    that has not aired counts: the hold is what keeps Axis from fetching it
 *    when it airs.
 *
 * Not told here, because it is derived in C from settings and dates: whether
 * a movie is available yet, and whether its library folder and profile can
 * be used.  Such a title is counted, and is searched for once it can be.
 * @m and @s are the aliases of a row of movie and of series (NULL-extended
 * rows are false).
 */
#define AXIS_ARR_MOVIE_HAS_FILE_SQL(m) \
	"EXISTS (SELECT 1 FROM movie_file wmf JOIN library_file wlf ON wlf.id = wmf.file_id" \
	" WHERE wmf.movie_id = " m ".id AND wlf.state = 'present')"
#define AXIS_ARR_MOVIE_WAITS_SQL(m) \
	"(" m ".search_hold = 'brought' AND " m ".monitored AND NOT " AXIS_ARR_MOVIE_HAS_FILE_SQL(m) ")"
#define AXIS_ARR_SERIES_WAITS_SQL(s) \
	"(" s ".search_hold = 'brought' AND EXISTS (SELECT 1 FROM series_episode wse WHERE wse.series_id = " s ".id" \
	" AND wse.gone_at IS NULL AND wse.monitored AND wse.search_hold IS NULL AND NOT EXISTS (SELECT 1" \
	" FROM series_file_episode wsfe JOIN library_file wsl ON wsl.id = wsfe.file_id AND wsl.state = 'present'" \
	" WHERE wsfe.episode_id = wse.id)))"

/**
 * AxisArrRun:
 * @job_live: a run is queued or running; @progress (-1: none yet) and
 *   @progress_message say where it is
 * @last_state: (nullable): how the last run that ended did ("succeeded",
 *   "failed", "cancelled"); @last_error: (nullable): why it failed
 * @n_brought: movies this connection brought; @n_gone: of those, removed in
 *   Axis since
 * @n_linked: files it linked
 * @n_held: brought movies whose searches are not started; @n_unseen: held
 *   because Radarr has a file Axis does not see; @n_downloading: held because
 *   Radarr was downloading them when searching was started
 * @started_us: when searching was last started (0: never)
 * @n_kept: files of what it brought that are still kept because they were
 *   brought over (what "Let Axis upgrade these files" would let go)
 * @run_note: (nullable): what the last run said about starting the searches
 *   at its end (the tick on its confirmation)
 * @n_waiting: of the titles held as brought, those "Start searching" would
 *   have searched for (AXIS_ARR_MOVIE_WAITS_SQL, AXIS_ARR_SERIES_WAITS_SQL):
 *   the number the notice of "Needs attention" states
 * @n_with_file: of the titles held as brought, the movies that have a file,
 *   or the series that have no monitored episode without one: starting
 *   searching changes nothing for them now
 */
typedef struct
{
	guint     n_waiting;
	guint     n_with_file;
	guint     n_kept;
	/* Files of what it brought that the app had and the run did not see (not
	 * indexed, another size) and that Axis has since indexed and identification
	 * linked: present, not kept, not linked by a person or an import.  A later
	 * run keeps them. */
	guint     n_unkept;
	gchar    *run_note;
	gboolean  job_live;
	gdouble   progress;
	gchar    *progress_message;
	gchar    *last_state;
	gchar    *last_error;
	guint     n_brought;
	guint     n_gone;
	guint     n_linked;
	guint     n_held;
	guint     n_unseen;
	guint     n_downloading;
	gint64    started_us;
	/* A Sonarr's: episodes of the series it brought that are held apart (Sonarr
	 * has a file for each that Axis does not see); they outlast "Start
	 * searching".  @n_unseen is a Radarr's (movies) and 0 here. */
	guint     n_unseen_episodes;
} AxisArrRun;

void
axis_arr_run_free(AxisArrRun *run);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArrRun, axis_arr_run_free)

gboolean
axis_arr_run_state(
	AxisArr     *self,
	gint64       source_id,
	AxisArrRun **run,
	GError     **error
);

/* A title the connection brought, for the report. */
typedef struct
{
	gint64  title_id;
	gint64  movie_id;	/* 0: removed in Axis since */
	gchar  *label;
	gchar  *file;		/* arr_brought.file */
	gchar  *file_detail;	/* nullable */
	gchar  *hold;		/* nullable: the movie's (or the series') search_hold */
	gint64  brought_us;
	/* A series: what became of its episode files and episodes
	 * (arr_brought.counts, a JSON object as text; nullable), and how many of
	 * its episodes are held apart now.  @movie_id is then the series' id. */
	gchar  *counts;
	guint   held_episodes;
} AxisArrBroughtRow;

/* What became of the app's file, in words ("Linked", "Not in Axis's index at
 * that path", ...); @file is arr_brought.file. */
gchar *
axis_arr_brought_file_label(
	const gchar *file,
	const gchar *detail,
	AxisArrApp   app
);

/* The same for a series, from arr_brought.counts: "12 of 14 files linked. 1
 * not in Axis's index at its path. 1 on an episode Axis's numbering does not
 * have. 3 episodes Sonarr has that Axis's numbering does not."  Each reason a
 * file was left alone is named with its count. */
gchar *
axis_arr_brought_counts_label(
	const gchar *counts,
	AxisArrApp   app
);

/* One page (%AXIS_ARR_PAGE_ROWS) of what the connection brought, by title;
 * @only_unlinked: only those whose file was not linked. */
gboolean
axis_arr_list_brought(
	AxisArr    *self,
	gint64      source_id,
	gboolean    only_unlinked,
	guint       page,
	GPtrArray **rows,
	guint      *total,
	GError    **error
);

/* What reading Radarr's download queue came to when searching is started. */
typedef enum
{
	AXIS_ARR_QUEUE_READ,		/* read: the movies in it are left out */
	AXIS_ARR_QUEUE_UNCLEAR,		/* Radarr answered, but which movies it is downloading could not be told:
					 * said in words, and nothing is held back for it */
	AXIS_ARR_QUEUE_NO_ANSWER	/* Radarr did not answer (or no key is stored): starting needs the tick */
} AxisArrQueueCheck;

/**
 * AxisArrStart:
 * @n_held: brought movies that would be started
 * @n_missing: of those, monitored without a file (searched for first)
 * @n_with_file: of those, with a file (searched only below their cutoff)
 * @n_unseen: movies that stay held: Radarr has a file Axis does not see
 * @per_run, @interval_s: the scheduler's pace
 * @queue: how the check of Radarr's queue went
 * @n_queue: downloads Radarr lists (for %AXIS_ARR_QUEUE_UNCLEAR)
 * @in_queue: (element-type utf8): the movies left out, by label (the first
 *   50); @n_in_queue: how many in all
 * @n_monitored: how many of the movies to start Radarr still monitored at
 *   the last reading; -1 when no reading is kept
 * @started: after axis_arr_start_searching(): how many were started
 */
typedef struct
{
	AxisArrSource     *source;
	guint              n_held;
	guint              n_missing;
	guint              n_with_file;
	guint              n_unseen;
	guint              per_run;
	guint              interval_s;
	AxisArrQueueCheck  queue;
	guint              n_queue;
	GPtrArray         *in_queue;
	guint              n_in_queue;
	gint               n_monitored;
	guint              started;
	/* A Sonarr's (the counts above are then of series: @n_missing those with
	 * a monitored episode that has no file, @n_with_file the others): the
	 * monitored episodes without a file of the series to start, and the
	 * episodes that stay held apart, whose files Axis does not see. */
	guint              n_missing_episodes;
	guint              n_unseen_episodes;
	/* For a run's own start (only its titles): the titles of this connection
	 * that earlier runs brought and that are still held. */
	guint              n_other_held;
} AxisArrStart;

void
axis_arr_start_free(AxisArrStart *start);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArrStart, axis_arr_start_free)

/**
 * axis_arr_start_review:
 *
 * What "Start searching for the movies brought over" would do now.  Reads
 * Radarr's download queue (GET only) and Axis's own tables; writes nothing.
 */
gboolean
axis_arr_start_review(
	AxisArr       *self,
	gint64         source_id,
	AxisArrStart **start,
	GError       **error
);

/**
 * axis_arr_start_searching:
 * @stopped: the person stated that Radarr is stopped or no longer monitors
 *   these movies (needed when Radarr does not answer)
 * @start: (out): the review as it was when the movies were started, with its
 *   @started; or, with %AXIS_ARR_ERROR_NEEDS_TICK, the review to show again
 *
 * Reads Radarr's queue again, leaves out (and holds as "Radarr is downloading
 * it") the movies in it, and ends the hold of the others.  Nothing is
 * searched at once: the scheduler takes them at its ordinary pace.  A movie
 * whose file Axis does not see stays held.
 */
gboolean
axis_arr_start_searching(
	AxisArr       *self,
	gint64         source_id,
	gboolean       stopped,
	const gchar   *user,
	AxisArrStart **start,
	GError       **error
);

/* ── Letting the brought files go ───────────────────────────────── */

/**
 * AxisArrRelease:
 * @n_files: the files of what the connection brought that are kept because
 *   they were brought over
 * @n_titles: the movies (or series) they are of
 * @digest: of the connection and those two counts: what the confirmation's
 *   post carries
 * @released: after axis_arr_release_kept(): how many files were let go
 */
typedef struct
{
	AxisArrSource *source;
	guint          n_files;
	guint          n_titles;
	gchar         *digest;
	guint          released;
} AxisArrRelease;

void
axis_arr_release_free(AxisArrRelease *release);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisArrRelease, axis_arr_release_free)

/* What "Let Axis upgrade these files" would do now.  Reads Axis's own tables;
 * writes nothing and asks the app nothing. */
gboolean
axis_arr_release_review(
	AxisArr         *self,
	gint64           source_id,
	AxisArrRelease **release,
	GError         **error
);

/**
 * axis_arr_release_kept:
 * @digest: the digest of the review the confirmation was shown with
 * @release: (out): the review, with its @released; or, with
 *   %AXIS_ARR_ERROR_STALE, the review to show again
 *
 * Lets go of the files the connection's titles have that are kept because
 * they were brought over (hand_how "brought", kept, kept_how "brought"): never
 * a link a person kept, let go or linked by hand since.  From then on a file
 * below its profile's cutoff is replaced at the ordinary pace, where its title
 * is searched for at all (a hold is not touched).  Fails with
 * %AXIS_ARR_ERROR_STALE, changing nothing, when the review differs now, and
 * %AXIS_ARR_ERROR_INVALID when there is nothing to let go.
 */
gboolean
axis_arr_release_kept(
	AxisArr         *self,
	gint64           source_id,
	const gchar     *digest,
	const gchar     *user,
	AxisArrRelease **release,
	GError         **error
);

G_END_DECLS

#endif /* AXIS_ARR_H */
