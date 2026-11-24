/*
 * axis-movies.h - the movies module: the films Axis manages, their states,
 * searching for them, grabbing a release once, following the download, and
 * their history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A film is a metadata title (meta_title) that an administrator added with
 * a quality profile and a destination root (0023_movies.sql).  What it joins:
 *
 *   metadata (titles, other names, release date, runtime)
 *   -> indexers (a search by identifier, falling back to title and year)
 *   -> the release parser and matcher (is this release that film?)
 *   -> the film's blocklist
 *   -> quality profiles (accepted or rejected, with every reason; ranked)
 *   -> downloads (sent once; followed to completed, failed, stalled, gone)
 *
 * The promises it keeps:
 *
 *  - A film's state is derived, never stored or set: one query reads the
 *    facts (axis_movies_store_facts(), in axis-movies-store.c) and one
 *    function turns them into the state and the reason in words
 *    (axis_movie_derive()).  Every page, job and check uses those two.
 *  - A release is grabbed only when the matcher says it is this film (with
 *    its other names, against every title of that name the provider knows;
 *    ambiguous is never a match), the profile accepts it, and it is not on
 *    the film's blocklist.  Every release a search considered is kept with
 *    its verdict and reasons, and the film's page shows them.
 *  - A film is grabbed once: the check that nothing is in flight and the
 *    claim of the grab are one transaction with the film's row locked, and
 *    the database allows one open grab per film besides.  A send whose
 *    outcome is uncertain counts as in flight until the downloads layer has
 *    settled it.
 *  - A download that fails, stalls beyond movies.give_up_after or is gone is
 *    put on the film's blocklist, removed from its client with its data, and
 *    one search for the next best is queued.
 *  - A completed download is "to import": the follow-up asks the import
 *    pipeline (src/import/) for it, and that is what writes into the
 *    library; this module only answers for its films there
 *    (axis-movies-import.c) and never touches a file itself.
 *  - Automatic searches are paced: least recently searched first, at most
 *    movies.searches_per_run each run, and per film a floor that doubles
 *    from movies.search_floor to movies.search_floor_max while searches find
 *    nothing and is reset when the film's availability changes.  Nothing is
 *    searched before the film's minimum availability or while a grab is in
 *    flight.
 *  - Every action is an administrator's, recorded in the film's history with
 *    the user (movie_event).
 *  - The list shows the whole library: the managed films, and every movie
 *    identification has linked to a file on disk, read at query time and
 *    never written (axis-movies-library.c).  Such a movie is not monitored,
 *    is in no job, and nothing is searched, grabbed, upgraded, renamed or
 *    moved for it until an administrator has Axis manage it.
 *
 * The search-and-grab job, the follow-up of grabs, the blocklist and the
 * pacing are the acquisition layer's (src/acquire/axis-acquire.h), shared
 * with the series; this module answers for its films there
 * (axis-movies-search.c) and keeps what a film is, its state, its tables
 * and its pages.
 *
 * Threads: everything that touches the database or another layer's network
 * blocks: call it from a worker thread.  Create, register and free the
 * module on the main thread.
 */

#ifndef AXIS_MOVIES_H
#define AXIS_MOVIES_H

#include <glib.h>

#include "acquire/axis-acquire.h"
#include "core/axis-module.h"
#include "core/axis-settings.h"
#include "db/axis-db-conn.h"
#include "db/axis-db-pool.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "jobs/axis-job-live.h"
#include "meta/axis-meta.h"
#include "quality/axis-quality.h"
#include "quality/axis-quality-store.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_MOVIES_MODULE_ID            "movies"

/* The search-and-grab job (downloads queue, one per film at a time: its
 * de-duplication key is AXIS_MOVIES_SEARCH_KEY_PREFIX and the film's id),
 * the follow-up of grabs and the scheduler (maintenance queue). */
#define AXIS_MOVIES_SEARCH_KIND          "movies.search"
#define AXIS_MOVIES_SEARCH_KEY_PREFIX    "movies.search:"
#define AXIS_MOVIES_FOLLOW_KIND          "movies.follow"
#define AXIS_MOVIES_FOLLOW_SCHEDULE      "movies.follow"
#define AXIS_MOVIES_FOLLOW_INTERVAL_S    60
#define AXIS_MOVIES_SCHEDULE_KIND        "movies.schedule"
#define AXIS_MOVIES_SCHEDULE_SCHEDULE    "movies.schedule"
#define AXIS_MOVIES_SCHEDULE_INTERVAL_S  900

#define AXIS_MOVIES_SETTING_AVAILABILITY  "movies.default_min_availability"
#define AXIS_MOVIES_SETTING_RELEASED_AFTER "movies.released_after"
#define AXIS_MOVIES_SETTING_PER_RUN       "movies.searches_per_run"
#define AXIS_MOVIES_SETTING_FLOOR         "movies.search_floor"
#define AXIS_MOVIES_SETTING_FLOOR_MAX     "movies.search_floor_max"
#define AXIS_MOVIES_SETTING_GIVE_UP       "movies.give_up_after"
#define AXIS_MOVIES_SETTING_PROTOCOL      "movies.protocol"
#define AXIS_MOVIES_SETTING_RETENTION     "movies.usenet_retention"
#define AXIS_MOVIES_SETTING_LANGUAGES     "movies.languages"

/* How many of a search's acceptable releases are tried before it stops. */
#define AXIS_MOVIES_MAX_TRIES            AXIS_ACQUIRE_MAX_TRIES

/* A claim not handed to a client after this long, with no job of the film
 * running, is settled by the follow-up. */
#define AXIS_MOVIES_CLAIM_STALE_S        AXIS_ACQUIRE_CLAIM_STALE_S

/* How long a page waits for the metadata provider, and the search for the
 * film's full record. */
#define AXIS_MOVIES_META_WAIT_MS         4000

#define AXIS_MOVIES_ERROR (axis_movies_error_quark())

/**
 * AxisMoviesError:
 * @AXIS_MOVIES_ERROR_NOT_FOUND: no such film (or candidate, or entry)
 * @AXIS_MOVIES_ERROR_EXISTS: the title is managed already
 * @AXIS_MOVIES_ERROR_INVALID: the request cannot be made (not a film, no such
 *   profile or root, a value out of range); the message names the field
 * @AXIS_MOVIES_ERROR_REFUSED: a release a fixed safety rule rejects, which
 *   no one may grab
 * @AXIS_MOVIES_ERROR_BLOCKLISTED: the release is on the film's blocklist
 * @AXIS_MOVIES_ERROR_EXPIRED: the search that found the release has expired
 * @AXIS_MOVIES_ERROR_IN_FLIGHT: a grab of the film is in flight
 * @AXIS_MOVIES_ERROR_NOT_AVAILABLE: the film is before its minimum
 *   availability
 * @AXIS_MOVIES_ERROR_UNUSABLE: nothing can be sent now (no client for the
 *   protocol, the destination offline, the profile unusable); the message
 *   says which
 * @AXIS_MOVIES_ERROR_FAILED: the indexer or the client failed; the message
 *   is theirs
 */
typedef enum
{
	AXIS_MOVIES_ERROR_NOT_FOUND,
	AXIS_MOVIES_ERROR_EXISTS,
	AXIS_MOVIES_ERROR_INVALID,
	AXIS_MOVIES_ERROR_REFUSED,
	AXIS_MOVIES_ERROR_BLOCKLISTED,
	AXIS_MOVIES_ERROR_EXPIRED,
	AXIS_MOVIES_ERROR_IN_FLIGHT,
	AXIS_MOVIES_ERROR_NOT_AVAILABLE,
	AXIS_MOVIES_ERROR_UNUSABLE,
	AXIS_MOVIES_ERROR_FAILED
} AxisMoviesError;

GQuark
axis_movies_error_quark(void);

/* ── States ─────────────────────────────────────────────────────── */

/**
 * AxisMovieState:
 * @AXIS_MOVIE_NOT_AVAILABLE: before its minimum availability, by the stored
 *   release date
 * @AXIS_MOVIE_WANTED: available, no file and nothing in flight
 * @AXIS_MOVIE_SEARCHING: a search for it is queued or running
 * @AXIS_MOVIE_DOWNLOADING: a grab is in flight
 * @AXIS_MOVIE_TO_IMPORT: downloaded, waiting for the import pipeline
 * @AXIS_MOVIE_IN_LIBRARY: a file of the library is linked to it
 * @AXIS_MOVIE_UPGRADE_WANTED: in the library below its profile's cutoff,
 *   monitored
 * @AXIS_MOVIE_ATTENTION: every acceptable release failed, the destination
 *   is offline, no indexer or no client is usable, or the last search found
 *   releases and rejected them all
 */
typedef enum
{
	AXIS_MOVIE_NOT_AVAILABLE,
	AXIS_MOVIE_WANTED,
	AXIS_MOVIE_SEARCHING,
	AXIS_MOVIE_DOWNLOADING,
	AXIS_MOVIE_TO_IMPORT,
	AXIS_MOVIE_IN_LIBRARY,
	AXIS_MOVIE_UPGRADE_WANTED,
	AXIS_MOVIE_ATTENTION
} AxisMovieState;

#define AXIS_MOVIE_N_STATES 8

/* "not_available", "wanted", "searching", "downloading", "to_import",
 * "in_library", "upgrade_wanted", "attention": stable (addresses, classes). */
const gchar *
axis_movie_state_name(AxisMovieState state);

gboolean
axis_movie_state_from_name(
	const gchar    *name,
	AxisMovieState *state
);

/* "Not available", "Wanted", "Searching", "Downloading", "To import", "In
 * library", "Upgrade wanted", "Needs attention". */
const gchar *
axis_movie_state_label(AxisMovieState state);

/* The short form a poster's mark carries ("Attention", "Upgrade"). */
const gchar *
axis_movie_state_short(AxisMovieState state);

/**
 * AxisMovieAvailability:
 * @AXIS_MOVIE_ANNOUNCED: as soon as the film is known
 * @AXIS_MOVIE_IN_CINEMAS: from its release date
 * @AXIS_MOVIE_RELEASED: movies.released_after after its release date (the
 *   date is its first release anywhere, a cinema's as a rule; a digital or
 *   disc date is not known separately)
 */
typedef enum
{
	AXIS_MOVIE_ANNOUNCED,
	AXIS_MOVIE_IN_CINEMAS,
	AXIS_MOVIE_RELEASED
} AxisMovieAvailability;

/* "announced", "in_cinemas", "released" (stored, posted). */
const gchar *
axis_movie_availability_name(AxisMovieAvailability availability);

gboolean
axis_movie_availability_from_name(
	const gchar           *name,
	AxisMovieAvailability *availability
);

/* "When announced", "In cinemas", "Released". */
const gchar *
axis_movie_availability_label(AxisMovieAvailability availability);

/**
 * AxisMovieFacts:
 *
 * What is known of one film, as axis_movies_store_facts() reads it: the
 * film's own row, its title, its profile, its destination, the job that
 * searches it, its open grab with that grab's download, other downloads of
 * the title still at a client, its linked files and its last finished
 * search.  Times are microseconds since 1970, 0 for none.  Texts are as
 * stored (they came from providers, indexers and clients: show them as
 * text).
 */
typedef struct
{
	gint64                 id;
	gint64                 title_id;
	gchar                 *title;
	gchar                 *original_title;
	gint                   year;
	gchar                 *release_date;	/* "YYYY-MM-DD" or %NULL */
	AxisMetaStatus         status;
	gint                   runtime;
	gchar                 *poster_ref;
	gchar                 *provider;

	gint64                 profile_id;
	gchar                 *profile_name;
	gchar                 *profile_slug;
	gboolean               profile_usable;
	AxisQuality            profile_cutoff;
	gboolean               profile_upgrades;
	gboolean               profile_upgrade_unknown;

	gint64                 root_id;		/* 0: none */
	gchar                 *root_name;
	gint64                 volume_id;
	gchar                 *volume_name;
	AxisVolumeState        volume_state;

	gboolean               monitored;
	AxisMovieAvailability  availability;
	gchar                 *added_by;
	gint64                 added_us;
	gint64                 last_search_us;
	gint64                 next_search_us;
	gint64                 floor_s;

	/* The search job: queued or running (by its de-duplication key). */
	gboolean               search_queued;
	gboolean               search_running;

	/* The open grab, if any, and its download. */
	gint64                 grab_id;
	gchar                 *grab_release;
	gchar                 *grab_quality;
	gboolean               grab_manual;
	gchar                 *grab_by;
	gint64                 grab_us;
	gint64                 grab_completed_us;
	gint64                 download_id;	/* 0: not handed to a client yet */
	gboolean               download_known;
	AxisDownloadState      download_state;
	gint64                 download_size;	/* -1: not known */
	gint64                 download_done;
	gint64                 download_rate;
	gint64                 download_eta;
	gboolean               download_stalled;
	gint64                 download_progress_us;	/* when it last grew, or was sent */
	gchar                 *download_problem;
	gchar                 *download_message;
	gchar                 *client_name;
	gchar                 *download_volume;
	gchar                 *download_rel_path;
	gchar                 *download_client_path;

	/* Downloads of this title (module movies) still at a client that are
	 * not the open grab's: one Axis gave up on and could not remove yet. */
	guint                  other_live;

	/* Linked files that are present, and the best of them by quality: its
	 * ladder quality, or (unknown) the resolution of an unknown source.  A
	 * file on a disk that is offline is still present: a disk that is away
	 * never means a file is gone (library_file.state is only changed by a
	 * scan of a disk that was verified online). */
	guint                  n_files;
	AxisQuality            file_quality;
	AxisReleaseResolution  file_resolution;
	gint64                 size_bytes;	/* of the present files, 0 for none */
	/* How many copies of it those files are: one identified folder or file
	 * each (an imported file identification has not looked at yet counts as
	 * one).  More than one is a duplicate, shown and never hidden. */
	guint                  n_copies;
	/* (nullable) Who keeps a present file of it by hand (movie_file.kept: a
	 * forced grab's file, or one a person keeps): then no automatic search
	 * and no Search now replaces it.  "a person" when no name was kept. */
	gchar                 *kept_by;
	/* With @kept_by: every present file of it that is kept is kept because it
	 * was brought over from Radarr (kept_how "brought"), and no person chose
	 * it.  The pages then say that, not "kept by hand by NAME". */
	gboolean               kept_brought;
	/* (nullable) The release of a grab by force that was let go because its
	 * import could take no file, while that download is still held for a
	 * person (its import is blocked): the movie is not searched for
	 * automatically meanwhile. */
	gchar                 *held_forced;
	/* (nullable) Why no automatic search runs for it (movie.search_hold,
	 * axis-movies-brought.c): "brought", "file_unseen", "arr_downloading". */
	gchar                 *search_hold;

	/* A movie on disk that Axis does not manage (axis-movies-library.c):
	 * identification links files that are present to its title, and there
	 * is no film row.  Then id is 0, there is no profile, it is not
	 * monitored, root_id and root_name are the root its files are in,
	 * added_us is when identification found it, and n_away of its files
	 * are on a disk that is not online (away_volume names one).  Its state
	 * is always "in library", and nothing is ever searched for it. */
	gboolean               library_only;
	guint                  n_away;
	gchar                 *away_volume;

	/* The latest import of the open grab's download, or else of the film
	 * (0: none): its state ("waiting", "running", "blocked", "imported",
	 * "dismissed") and why. */
	gint64                 import_id;
	gchar                 *import_state;
	gchar                 *import_reason;
	gchar                 *import_text;

	/* The last finished search. */
	gint64                 search_id;
	gint64                 search_us;
	gchar                 *search_outcome;
	gchar                 *search_text;
	guint                  search_found;
	guint                  search_accepted;
} AxisMovieFacts;

void
axis_movie_facts_free(AxisMovieFacts *facts);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMovieFacts, axis_movie_facts_free)

/**
 * AxisMovieRules:
 * @today: "YYYY-MM-DD", UTC
 * @now_us: the time
 * @released_after_days: movies.released_after, in days
 * @give_up_after_s: movies.give_up_after
 *
 * The settings and the clock the derivation reads.
 */
typedef struct
{
	gchar  today[11];
	gint64 now_us;
	gint   released_after_days;
	gint64 give_up_after_s;
} AxisMovieRules;

/* The rules as of now with the defaults (a test changes them). */
void
axis_movie_rules_init(
	AxisMovieRules *rules,
	gint64          now_us
);

/**
 * axis_movie_available:
 * @from: (out) (optional) (transfer full): when it is not, the date it will
 *   be ("YYYY-MM-DD"), or %NULL when no release date is known
 *
 * Whether the film has reached its minimum availability: when announced,
 * always; in cinemas, from its release date; released, movies.released_after
 * after it.  A film with no release date is available (except when
 * announced is enough) only when its provider says it is released.
 */
gboolean
axis_movie_available(
	const AxisMovieFacts *facts,
	const AxisMovieRules *rules,
	gchar               **from
);

/**
 * axis_movie_in_flight:
 *
 * Whether a grab of the film is in flight: an open grab that is not
 * complete (claimed and not yet handed over, sent, being reconciled after an
 * uncertain send, or failed and not yet dealt with), or a download of its
 * title still at a client that Axis gave up on.  The grab-once check.
 */
gboolean
axis_movie_in_flight(const AxisMovieFacts *facts);

/**
 * axis_movie_library_away_note:
 *
 * For a movie on disk that Axis does not manage: the sentence saying that its
 * disk (or one of them) is offline, or %NULL when none is.
 */
gchar *
axis_movie_library_away_note(const AxisMovieFacts *facts);

/**
 * axis_movie_derive:
 * @reason: (out) (optional) (transfer full): why, a sentence or two
 *
 * The film's one state, from its facts, in this order: downloading (in
 * flight), to import, needs attention for a download still at a client,
 * searching, in library (or upgrade wanted), not available, needs attention
 * (no destination, destination offline, or the last search's outcome), and
 * wanted.  A movie on disk that Axis does not manage is in library, always.
 * Pure.
 */
AxisMovieState
axis_movie_derive(
	const AxisMovieFacts *facts,
	const AxisMovieRules *rules,
	gchar               **reason
);

/* The same, and for a film that needs attention why, as a kind (NONE for
 * the other states): what its page offers to put it right. */
AxisMovieState
axis_movie_derive_attention(
	const AxisMovieFacts  *facts,
	const AxisMovieRules  *rules,
	gchar                **reason,
	AxisAcquireAttention  *kind
);

/* Whether a last search's @outcome means the film needs attention. */
gboolean
axis_movie_outcome_needs_attention(const gchar *outcome);

/* axis_acquire_name_key(): the folded form a release is known by on a
 * blocklist without its hash. */
gchar *
axis_movie_name_key(
	gint64       source_id,
	gint64       indexer_id,
	const gchar *indexer,
	const gchar *name
);

/* ── The module ─────────────────────────────────────────────────── */

typedef struct _AxisMovies AxisMovies;

/**
 * axis_movies_new:
 * @pool: the database, which must outlive the module
 * @settings: (nullable): the settings store
 * @meta, @indexers, @downloads, @storage, @quality: the layers it joins
 *
 * Registers the films as references of the metadata layer (so that they are
 * kept fresh) and as users of quality profiles (so that a profile a film
 * uses is not deleted).  Main thread, before any of those layers is used.
 *
 * Returns: (transfer full): the module, reference counted
 */
AxisMovies *
axis_movies_new(
	AxisDbPool       *pool,
	AxisSettings     *settings,
	AxisMeta         *meta,
	AxisIndexers     *indexers,
	AxisDownloads    *downloads,
	AxisStorage      *storage,
	AxisQualityStore *quality
);

AxisMovies *
axis_movies_ref(AxisMovies *self);

void
axis_movies_unref(AxisMovies *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMovies, axis_movies_unref)

/* The household's players, for the profiles' players preference (%NULL:
 * none known). */
void
axis_movies_set_compat(
	AxisMovies *self,
	AxisCompat *compat
);

/**
 * axis_movies_set_import:
 * @import: (nullable): the import pipeline (borrowed: it outlives the
 *   module)
 *
 * Registers the films as an owner of imports (axis-movies-import.c), and
 * lets the follow-up ask for the import of every completed download.
 * Without it, completed films stay "to import".  Main thread, before use.
 */
void
axis_movies_set_import(
	AxisMovies *self,
	AxisImport *import
);

/**
 * axis_movies_set_identify:
 * @identify: (nullable): identification (borrowed: it outlives the module)
 *
 * Registers the films as the owner of identified films
 * (axis-movies-identify.c): a managed film's links to the files identified
 * as it follow its identities, a film added later gets them at once (and is
 * then in the library, so it is not searched for), and every identified film
 * of a root can be added at once.  Main thread, before use.
 */
void
axis_movies_set_identify(
	AxisMovies   *self,
	AxisIdentify *identify
);

/* The module's description, with its jobs, schedules, settings, navigation
 * and pages (axis-movies-web.c), for axis_module_registry_register() with
 * the module as user data. */
const AxisModuleInfo *
axis_movies_get_module(void);

/* The module's pages wait for their work in flight (main loop). */
void
axis_movies_stop(AxisMovies *self);

/* The rules now, from the settings. */
void
axis_movies_get_rules(
	AxisMovies     *self,
	AxisMovieRules *rules
);

/* ── Reading ────────────────────────────────────────────────────── */

/* Every film (AxisMovieFacts), by title.  Reads every film's facts: for
 * the scheduler and for tests, never for a page (axis_movies_list_page()). */
gboolean
axis_movies_list(
	AxisMovies  *self,
	GPtrArray  **films,
	GError     **error
);

/* ── A page of the list ─────────────────────────────────────────── */

/*
 * The list of movies is every film Axis manages and every movie on disk it
 * does not (AxisMovieFacts.library_only): one entry per title, whatever the
 * number of its files, copies and disks.
 */

/**
 * AxisMoviesSort:
 * @AXIS_MOVIES_SORT_ADDED: the latest added first
 * @AXIS_MOVIES_SORT_TITLE: by title, then year
 * @AXIS_MOVIES_SORT_YEAR: the newest first, then by title (no year last)
 * @AXIS_MOVIES_SORT_SIZE: the largest on disk first
 */
typedef enum
{
	AXIS_MOVIES_SORT_ADDED,
	AXIS_MOVIES_SORT_TITLE,
	AXIS_MOVIES_SORT_YEAR,
	AXIS_MOVIES_SORT_SIZE
} AxisMoviesSort;

/* "added", "title", "year", "size" (as a list's address carries them). */
const gchar *
axis_movies_sort_name(AxisMoviesSort sort);

gboolean
axis_movies_sort_from_name(
	const gchar    *name,
	AxisMoviesSort *sort
);

/* The most films one page holds. */
#define AXIS_MOVIES_PAGE_MAX 200

/**
 * AxisMoviesListQuery:
 * @states: bit (1 << state) for each state wanted; 0 for every state
 * @name: (nullable): what was typed in the name filter: films whose title,
 *   original title or another name contains it, case, accents and
 *   punctuation aside (meta_title.name_keys)
 * @sort: the order
 * @limit: the page's size, 1 to %AXIS_MOVIES_PAGE_MAX
 * @after: (nullable): a cursor (a page's next): the films after it
 * @before: (nullable): a cursor (a page's previous): the films before it
 * @managed_only: the films Axis manages alone, not the movies on disk it
 *   does not manage (the dashboard's "added lately")
 */
typedef struct
{
	guint           states;
	const gchar    *name;
	AxisMoviesSort  sort;
	guint           limit;
	const gchar    *after;
	const gchar    *before;
	gboolean        managed_only;
} AxisMoviesListQuery;

/**
 * AxisMoviesListPage:
 * @films: (element-type AxisMovieFacts): the page's entries, in order:
 *   managed films and movies on disk Axis does not manage alike
 * @states: their states, one per entry, as axis_movie_derive() has them
 * @counts: how many entries are in each state, managed or not, with the
 *   name filter (not the state filter, nor @managed_only): the list's tabs
 * @library: of counts[in library], the movies on disk Axis does not manage
 * @review: units identification holds for a person (in roots for movies),
 *   which are in no entry yet; @waiting: units it waits for the metadata
 *   provider to answer about (neither is filtered by name)
 * @matching: how many entries the whole query matches (name, states and
 *   @managed_only)
 * @next: (nullable): the cursor of the next page
 * @previous: (nullable): the cursor of the previous page (none on the first)
 * @facts_read: how many entries' facts were read: the page's, never more
 */
typedef struct
{
	GPtrArray *films;
	GArray    *states;
	guint      counts[AXIS_MOVIE_N_STATES];
	guint      library;
	guint      review;
	guint      waiting;
	guint      matching;
	gchar     *next;
	gchar     *previous;
	guint      facts_read;
} AxisMoviesListPage;

void
axis_movies_list_page_clear(AxisMoviesListPage *page);

/**
 * axis_movies_list_page:
 * @page: (out caller-allocates): the page; clear it
 *
 * One page of the movies.  Reads, whatever their number: the counts by
 * state with the movies on disk Axis does not manage and the units waiting
 * for review (one statement, the state of a managed film worked out in the
 * database the way axis_movie_derive() works it out, from the same facts),
 * the ids of the page's entries (one statement: the managed films keyset
 * paged on an index where the order has one, the state worked out only when
 * a state is asked for; the movies on disk in one pass over the linked
 * units, each side cut to the page before the two are merged), and the
 * facts of those entries alone (one statement for the managed films, one
 * for the others).  A cursor that does not parse, or is of another order, is
 * the first page.
 *
 * Returns: %FALSE for a database error
 */
gboolean
axis_movies_list_page(
	AxisMovies                *self,
	const AxisMoviesListQuery *query,
	AxisMoviesListPage        *page,
	GError                   **error
);

/* How many kinds of reason there are (the size of the arrays below). */
#define AXIS_MOVIES_ATTENTION_KINDS (AXIS_ACQUIRE_ATTENTION_FAILED + 1)

/**
 * axis_movies_count_attention:
 * @timeout_ms: the server cancels the count after this long
 * @counts: (out caller-allocates) (array fixed-size=AXIS_MOVIES_ATTENTION_KINDS):
 *   how many managed movies need attention for each kind of reason
 *   (AxisAcquireAttention; one whose reason is none of them under NONE)
 * @newest: (out caller-allocates) (array fixed-size=AXIS_MOVIES_ATTENTION_KINDS):
 *   the highest movie id among them, for each kind
 *
 * The movies in the state "Needs attention", counted by why, in one statement
 * that loads no film: the state is worked out in the database as the list's
 * own filter works it out (the Wanted page's "Needs attention" shows exactly
 * these), and the reason as axis_movie_derive_attention() gives it.  The sum
 * over the kinds is the list's count for the state.  Fails with the
 * database's error when the statement is cancelled.
 */
gboolean
axis_movies_count_attention(
	AxisMovies  *self,
	guint        timeout_ms,
	guint64     *counts,
	gint64      *newest,
	GError     **error
);

/**
 * axis_movies_list_states:
 * @states: (out) (transfer full): film id (gint64 *) -> AxisMovieState
 *   (GINT_TO_POINTER)
 *
 * Every film's state as the list works it out in the database: for tests,
 * which hold it against axis_movie_derive().
 */
gboolean
axis_movies_list_states(
	AxisMovies  *self,
	GHashTable **states,
	GError     **error
);

/**
 * axis_movies_list_explain:
 * @plan: (out) (transfer full): the plan, as PostgreSQL's EXPLAIN (ANALYZE,
 *   FORMAT JSON) gives it
 *
 * How the database read the ids of the page @query asks for: for tests,
 * which bound the rows it read.
 */
gboolean
axis_movies_list_explain(
	AxisMovies                *self,
	const AxisMoviesListQuery *query,
	gchar                    **plan,
	GError                   **error
);

/* Fails with %AXIS_MOVIES_ERROR_NOT_FOUND. */
gboolean
axis_movies_get(
	AxisMovies      *self,
	gint64           movie_id,
	AxisMovieFacts **facts,
	GError         **error
);

/* The films of these meta_title ids (gint64) that are managed. */
gboolean
axis_movies_get_by_titles(
	AxisMovies  *self,
	GArray      *title_ids,
	GPtrArray  **films,
	GError     **error
);

/* ── Movies on disk that Axis does not manage (axis-movies-library.c) ── */

/*
 * A movie on disk is read at query time from identification's links
 * (library_identity, library_identity_file) and the file index: nothing is
 * written for it, it is never searched for, grabbed, upgraded, renamed or
 * moved, and its files are touched by nothing.  It stops being one when
 * Axis manages it (axis_movies_add() links its files), and becomes one
 * again when that film is removed while its files are on disk.
 */

/* The movies on disk of these meta_title ids (gint64) that Axis does not
 * manage (AxisMovieFacts with library_only); titles that are not are left
 * out. */
gboolean
axis_movies_get_library_by_titles(
	AxisMovies  *self,
	GArray      *title_ids,
	GPtrArray  **films,
	GError     **error
);


/* A reason a release was rejected ("safety", "profile", "upgrade",
 * "blocklist"), one release a search considered, one blocklist entry and
 * one stored search: the acquisition layer's rows. */
typedef AxisAcquireReason     AxisMovieReason;
typedef AxisAcquireCandidate  AxisMovieCandidate;
typedef AxisAcquireBlock      AxisMovieBlock;
typedef AxisAcquireSearchInfo AxisMovieSearchInfo;

void
axis_movie_candidate_free(AxisMovieCandidate *candidate);

void
axis_movie_block_free(AxisMovieBlock *block);

typedef struct
{
	gint64  id;
	gint64  movie_id;	/* 0: the film was removed */
	gint64  title_id;
	gchar  *title_label;
	gchar  *kind;
	gchar  *text;
	gchar  *by_user;
	gint64  created_us;
} AxisMovieEvent;

void
axis_movie_event_free(AxisMovieEvent *event);

void
axis_movie_search_info_free(AxisMovieSearchInfo *search);

/**
 * AxisMovieFile:
 *
 * A file of the library linked to a film, with how it came there.
 * @rel_path is in the stored form, relative to its root.
 */
typedef struct
{
	gint64  file_id;
	gchar  *root_name;
	gchar  *volume_name;
	gchar  *rel_path;
	gint64  size;
	gchar  *state;
	gchar  *quality;
	gchar  *quality_text;
	gboolean pinned;
	gint64  import_id;
	gchar  *mode;
	gint64  linked_us;
	gchar  *resolution;	/* an unknown source's, or NULL */
	gint64  identity_id;	/* made by identification: the unit, else 0 */
	gchar  *identity_how;	/* "auto", "confirmed", "chosen", "imported" */
	gboolean best;		/* the identified copy upgrades are judged by */
	gboolean away;		/* its disk is not online (the file still counts) */
	gchar  *hand_how;	/* (nullable) a person made the link: "forced", "assigned", "linked"; or it was
				 * brought over from another app: "brought" */
	gchar  *hand_by;	/* (nullable) who */
	gboolean kept;		/* kept by hand: not upgraded by itself */
	gboolean grab_open;	/* with import_id: the grab of its download is still open */
	gboolean kept_brought;	/* with @kept: kept because it was brought over, by no person's choice */
} AxisMovieFile;

void
axis_movie_file_free(AxisMovieFile *file);

/**
 * AxisMovieDetail:
 *
 * What a film's page shows besides its facts: its files in the library,
 * its last search with every candidate, its blocklist and its history (the
 * latest first).
 */
typedef struct
{
	AxisMovieFacts      *facts;
	AxisMovieSearchInfo *search;		/* (nullable) */
	GPtrArray           *candidates;	/* AxisMovieCandidate, by position */
	GPtrArray           *blocklist;	/* AxisMovieBlock, the latest first */
	GPtrArray           *events;		/* AxisMovieEvent, the latest first */
	GPtrArray           *files;		/* AxisMovieFile */
	gboolean             manual;		/* a manual search of it was made */
} AxisMovieDetail;

void
axis_movie_detail_free(AxisMovieDetail *detail);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMovieDetail, axis_movie_detail_free)

/* A film's page: facts, the latest search, blocklist, @n_events events. */
gboolean
axis_movies_get_detail(
	AxisMovies       *self,
	gint64            movie_id,
	guint             n_events,
	AxisMovieDetail **detail,
	GError          **error
);

/* The page of a movie on disk that Axis does not manage
 * (axis-movies-library.c): its facts, its files (identification's, by
 * unit) and the history its title kept from a film removed before
 * (@n_events, the latest first); no search, no blocklist.  Fails with
 * %AXIS_MOVIES_ERROR_NOT_FOUND when @title_id is managed or has no file on
 * disk. */
gboolean
axis_movies_get_library_detail(
	AxisMovies       *self,
	gint64            title_id,
	guint             n_events,
	AxisMovieDetail **detail,
	GError          **error
);

/* The history across films, the latest first: @limit events after
 * @before_id (0: from the latest). */
gboolean
axis_movies_list_events(
	AxisMovies  *self,
	gint64       before_id,
	guint        limit,
	GPtrArray  **events,
	GError     **error
);

/* ── Acting (administrators) ────────────────────────────────────── */

/**
 * AxisMovieSpec:
 * @title_id: the meta_title (a film)
 * @profile_id: a quality profile (0: the default profile)
 * @root_id: a library root of type movies
 * @monitored: searched for automatically
 * @availability: its minimum availability
 * @search_now: queue a search at once (if it is available)
 * @user: who
 * @brought_from: (nullable): the movie is brought over from another app
 *   ("Radarr (Living room)"), not added by a person's Add: its one history
 *   line says so, nothing is searched for whatever @search_now says, and its
 *   library folder need not be writable now (nothing is fetched for it until
 *   a person starts its searches).  Only axis_movies_bring() sets it.
 */
typedef struct
{
	gint64                 title_id;
	gint64                 profile_id;
	gint64                 root_id;
	gboolean               monitored;
	AxisMovieAvailability  availability;
	gboolean               search_now;
	const gchar           *user;
	const gchar           *brought_from;
} AxisMovieSpec;

/**
 * axis_movies_add:
 * @movie_id: (out) (optional)
 *
 * Adds a film.  Fails with %AXIS_MOVIES_ERROR_EXISTS for a title managed
 * already, %AXIS_MOVIES_ERROR_INVALID for a title that is not a stored
 * film, a profile that does not exist or is unusable, or a root that is not
 * of type movies.
 */
gboolean
axis_movies_add(
	AxisMovies          *self,
	const AxisMovieSpec *spec,
	gint64              *movie_id,
	GError             **error
);

/* Changes the profile, root, monitoring and minimum availability (the
 * spec's title and search_now are ignored); a change of availability
 * resets the search floor. */
gboolean
axis_movies_edit(
	AxisMovies          *self,
	gint64               movie_id,
	const AxisMovieSpec *spec,
	GError             **error
);

/**
 * AxisMovieEdited:
 * @changed: anything changed
 * @profile_changed: the quality profile changed
 * @searched: a search was queued with the new profile
 * @monitored: the film is monitored (after the change)
 */
typedef struct
{
	gboolean changed;
	gboolean profile_changed;
	gboolean searched;
	gboolean monitored;
} AxisMovieEdited;

/* axis_movies_edit(), and with a new profile a monitored film that is
 * wanted under it (missing, or below its cutoff by the usual rules) and may
 * be searched for now is searched for again at once (cause "profile"), in
 * the same transaction.  A change of destination alone searches for
 * nothing. */
gboolean
axis_movies_edit_full(
	AxisMovies          *self,
	gint64               movie_id,
	const AxisMovieSpec *spec,
	AxisMovieEdited     *edited,
	GError             **error
);

/**
 * axis_movies_try_profiles:
 * @candidates: (element-type AxisMovieCandidate): releases of the film's
 *   searches
 * @profile_ids: (element-type gint64)
 * @tries: (out) (element-type AxisAcquireProfileTry)
 *
 * What each profile would make of each release now
 * (axis_acquire_try_profiles()).  Writes nothing.
 */
gboolean
axis_movies_try_profiles(
	AxisMovies       *self,
	gint64            movie_id,
	const GPtrArray  *candidates,
	const GArray     *profile_ids,
	GPtrArray       **tries,
	GError          **error
);

/* Forgets a film: its searches, grabs and blocklist (its history is kept).
 * No file is touched, and a download in progress stays with its client.  A
 * film whose files identification links stays in the list as a movie on
 * disk that Axis does not manage. */
gboolean
axis_movies_remove(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	GError      **error
);

/**
 * axis_movies_request_search:
 * @cause: "manual", "added", "scheduled" or "retry"
 * @user: (nullable)
 * @created: (out) (optional): %FALSE when a search was queued or running
 *   already (that one stands)
 *
 * Queues the film's search job.  For "manual" it refuses a film that is not
 * available (%AXIS_MOVIES_ERROR_NOT_AVAILABLE) or has a grab in flight
 * (%AXIS_MOVIES_ERROR_IN_FLIGHT), recording the request in its history.
 */
gboolean
axis_movies_request_search(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *cause,
	const gchar  *user,
	gboolean     *created,
	GError      **error
);

/**
 * AxisMoviesAsked:
 * @AXIS_MOVIES_ASKED_QUEUED: a search was queued
 * @AXIS_MOVIES_ASKED_IN_HAND: nothing new: one of that kind was queued or
 *   running already
 * @AXIS_MOVIES_ASKED_RECENT: nothing new: one of that kind ended less than
 *   %AXIS_ACQUIRE_SEARCH_AGAIN_S ago
 *
 * What a person's click on a film's search came to.
 */
typedef enum
{
	AXIS_MOVIES_ASKED_QUEUED,
	AXIS_MOVIES_ASKED_IN_HAND,
	AXIS_MOVIES_ASKED_RECENT
} AxisMoviesAsked;

/**
 * axis_movies_ask_search:
 * @list: a manual search (it lists and grabs nothing) rather than Search
 *   now; the two are separate kinds, neither standing for the other
 * @asked: (out): what came of it
 *
 * A person's search of the film, made only when nothing does its work: one
 * of its kind queued or running (%AXIS_MOVIES_ASKED_IN_HAND) or ended a
 * moment ago (%AXIS_MOVIES_ASKED_RECENT) answers the click.  Otherwise as
 * axis_movies_request_search() ("manual") or
 * axis_movies_request_manual_search().
 */
gboolean
axis_movies_ask_search(
	AxisMovies       *self,
	gint64            movie_id,
	gboolean          list,
	const gchar      *user,
	AxisMoviesAsked  *asked,
	GError          **error
);

/**
 * AxisMoviesWork:
 * @search: (nullable): its search (Search now, or an automatic one) queued
 *   or running
 * @list: (nullable): its manual search queued or running
 * @ended_us, @outcome, @outcome_text, @n_found: its last search that ended
 *   within the time asked for (0 and %NULL for none)
 * @list_ended_us, @list_n_found: its last manual search's, likewise
 * @now_us: the database's clock when they were read
 *
 * What is in hand for a film, and what ended a while ago.
 */
typedef struct
{
	AxisJobLive *search;
	AxisJobLive *list;
	gint64       ended_us;
	gchar       *outcome;
	gchar       *outcome_text;
	guint        n_found;
	gint64       list_ended_us;
	guint        list_n_found;
	gint64       now_us;
} AxisMoviesWork;

void
axis_movies_work_free(AxisMoviesWork *work);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMoviesWork, axis_movies_work_free)

gboolean
axis_movies_get_work(
	AxisMovies       *self,
	gint64            movie_id,
	guint             ended_within_s,
	AxisMoviesWork  **work,
	GError          **error
);

/* What a search did (searched, skipped, search_id, outcome, grab_id,
 * tries): the acquisition layer's report. */
typedef AxisAcquireSearchReport AxisMovieSearchReport;

/**
 * axis_movies_run_search:
 * @cancellable: (nullable)
 * @report: (out) (optional)
 *
 * The job's work, for one film: search, read, match, blocklist, decide,
 * rank, then claim, fetch and send the best acceptable release whose
 * protocol has a usable client (see the promises above).  Idempotent: a
 * film with a grab in flight is not searched.  Fails only for a database
 * error or cancellation; everything else is the search's outcome.
 */
gboolean
axis_movies_run_search(
	AxisMovies             *self,
	gint64                  movie_id,
	const gchar            *cause,
	const gchar            *user,
	GCancellable           *cancellable,
	AxisMovieSearchReport  *report,
	GError                **error
);

/**
 * axis_movies_grab:
 * @candidate_id: a release one of the film's searches considered
 * @grab_id: (out) (optional)
 *
 * An administrator's grab of a listed release, accepted or rejected by the
 * profile.  Refused with %AXIS_MOVIES_ERROR_REFUSED for a release a fixed
 * safety rule rejects (not this film, ambiguous, a sample, an archive...),
 * %AXIS_MOVIES_ERROR_BLOCKLISTED, %AXIS_MOVIES_ERROR_EXPIRED,
 * %AXIS_MOVIES_ERROR_IN_FLIGHT, %AXIS_MOVIES_ERROR_UNUSABLE; then
 * %AXIS_MOVIES_ERROR_FAILED with the indexer's or the client's words.
 * Recorded as a manual grab with @user.
 */
gboolean
axis_movies_grab(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        candidate_id,
	const gchar  *user,
	gint64       *grab_id,
	GError      **error
);

/**
 * axis_movies_request_manual_search:
 * @created: (out) (optional): %FALSE when one was queued or running already
 *
 * Queues a manual search of the film (cause "list": it lists and grabs
 * nothing), with a job key of its own, whether or not it is wanted now; the
 * history says who asked.
 */
gboolean
axis_movies_request_manual_search(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	gboolean     *created,
	GError      **error
);

/* The film's last manual search (*@search %NULL for none) with its
 * releases (AxisMovieCandidate), and whether one is queued or running. */
gboolean
axis_movies_get_manual(
	AxisMovies            *self,
	gint64                 movie_id,
	AxisMovieSearchInfo  **search,
	GPtrArray            **candidates,
	gboolean              *running,
	GError               **error
);

/* What a grab by hand of a stored release would do (NOT_FOUND: no such
 * movie, or not one of its searches'). */
gboolean
axis_movies_hand_preview(
	AxisMovies               *self,
	gint64                    movie_id,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
);

/* Grab this one (axis_acquire_grab_by_hand()); @grabbed, @result: (out)
 * (optional). */
gboolean
axis_movies_grab_by_hand(
	AxisMovies                  *self,
	gint64                       movie_id,
	gint64                       candidate_id,
	const AxisAcquireHandTicks  *ticks,
	const gchar                 *user,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result,
	GError                     **error
);

/* What a grab by force of a stored release would do
 * (axis_acquire_force_preview(); NOT_FOUND as above). */
gboolean
axis_movies_force_preview(
	AxisMovies               *self,
	gint64                    movie_id,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
);

/* Grab it by force (axis_acquire_grab_by_force()): the grab is bound to
 * this movie, so its import does not judge the name again; @grabbed,
 * @result: (out) (optional).  With *@result CHANGED, the error says what
 * its confirmation showed has changed. */
gboolean
axis_movies_grab_by_force(
	AxisMovies                   *self,
	gint64                        movie_id,
	gint64                        candidate_id,
	const AxisAcquireForceTicks  *ticks,
	const gchar                  *user,
	AxisAcquireHandGrabbed       *grabbed,
	AxisAcquireGrabResult        *result,
	GError                      **error
);

/**
 * axis_movies_set_kept:
 * @file_id: a library file linked to the movie
 * @kept: kept by hand (no automatic search and no Search now replaces it),
 *   or let go (upgraded by the ordinary rules again)
 * @user: who
 *
 * Sets or clears "kept by hand" on one file's link, and says so in the
 * movie's history.  Fails with NOT_FOUND when the file is not the movie's.
 */
gboolean
axis_movies_set_kept(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        file_id,
	gboolean      kept,
	const gchar  *user,
	GError      **error
);

/**
 * axis_movies_give_up:
 *
 * An administrator gives up on the film's open grab (downloading, or
 * downloaded and waiting for import): it is dealt with as a failure (on the
 * blocklist, removed from its client with its data, the next best sought).
 */
gboolean
axis_movies_give_up(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	GError      **error
);

/* Block this release (add-block-release): a release one of the film's
 * searches found is put on its blocklist by @user
 * (axis_acquire_block_candidate()).  Nothing else changes. */
gboolean
axis_movies_block_candidate(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        candidate_id,
	const gchar  *user,
	GError      **error
);

/* The grab Block and search again is about, by its id, the download it sent
 * or an import of that download (the first that is not 0), with its film;
 * %AXIS_MOVIES_ERROR_NOT_FOUND when there is none.  Read only. */
gboolean
axis_movies_block_find(
	AxisMovies             *self,
	gint64                  grab_id,
	gint64                  download_id,
	gint64                  import_id,
	AxisAcquireBlockGrab  **grab,
	AxisMovieFacts        **facts,
	GError                **error
);

/* Block and search again (axis_acquire_block_grab()): an open grab is given
 * up on as axis_movies_give_up() does, with the release blocked by @user
 * (*@cancelled); a closed one is only put on the blocklist, and no file or
 * link is touched. */
gboolean
axis_movies_block_grab(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
);

/* Removes one blocklist entry (@entry_id), or all of the film's (0). */
gboolean
axis_movies_clear_blocklist(
	AxisMovies   *self,
	gint64        movie_id,
	gint64        entry_id,
	const gchar  *user,
	guint        *removed,
	GError      **error
);

/* ── Links by hand (axis-movies-links.c) ───────────────────────── */

/* The most files with no link one page of the links page lists. */
#define AXIS_MOVIES_LINK_PAGE 100

/**
 * AxisMovieLinkFile:
 *
 * A video file a movie's links page lists: one linked to the movie, one in
 * its folder with no link (never linked, or unlinked by hand), or one asked
 * for that lies in a library folder for movies (it may be another movie's).
 */
typedef struct
{
	gint64    file_id;
	gchar    *path;			/* stored form */
	gboolean  linked;		/* to this movie */
	gchar    *how;			/* how its link was made, or why it has none, a sentence */
	gboolean  hand;			/* a person made the link, or keeps it */
	gboolean  kept;
	gboolean  unlinked_by_hand;
	gchar    *other;		/* (nullable) the other movie it is linked to */
} AxisMovieLinkFile;

void
axis_movie_link_file_free(AxisMovieLinkFile *file);

/* What a person chose for one file: @unlink removes its link to the movie;
 * else it is linked to the movie. */
typedef struct
{
	gint64    file_id;
	gboolean  unlink;
} AxisMovieLinkAsk;

/* A file of the change: the movie it is linked to before and after. */
typedef struct
{
	gint64    file_id;
	gchar    *path;
	gchar    *before;	/* "Hearth (1995)", "No movie" */
	gchar    *after;
	gchar    *note;		/* (nullable) it leaves another movie; or it was ticked */
	gboolean  other;	/* unlinked by an "unlink it instead" tick */
} AxisMovieLinkChange;

/* A movie the change touches: its files before and after. */
typedef struct
{
	gint64    movie_id;
	gchar    *label;
	gchar    *before;	/* its files' paths, "; " between; "No file" */
	gchar    *after;
	gchar    *what;		/* (nullable) left without a file and whether wanted; or two files now */
} AxisMovieLinkTouched;

/* A file of the movie outside the change, when the change links a file to
 * the movie: the tick "unlink it instead". */
typedef struct
{
	gint64    file_id;
	gchar    *path;
	gboolean  ticked;
} AxisMovieLinkOffer;

/**
 * AxisMovieLinks:
 * @files: (element-type AxisMovieLinkFile): by path
 * @doubles: (nullable): the sentence for a movie with more than one file
 *   linked now
 * @busy: (nullable): why no link can be changed now (an open import)
 * @changes, @touched, @offers: the review of a plan (empty for a reading)
 * @refused: (nullable): why the plan cannot be applied now
 * @digest: (nullable): of everything the review shows
 */
typedef struct
{
	AxisMovieFacts *movie;
	GPtrArray      *files;
	gchar          *doubles;
	gchar          *busy;
	GPtrArray      *changes;	/* AxisMovieLinkChange */
	GPtrArray      *touched;	/* AxisMovieLinkTouched */
	GPtrArray      *offers;		/* AxisMovieLinkOffer */
	gchar          *refused;
	gchar          *digest;
	guint           page;		/* the page of files with no link listed (from 0) */
	guint           free_total;	/* every file with no link in the movie's reach */
} AxisMovieLinks;

void
axis_movie_links_free(AxisMovieLinks *links);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMovieLinks, axis_movie_links_free)

/**
 * axis_movies_links_read:
 * @also: (nullable) (element-type gint64): files to list wherever they lie in
 *   a library folder for movies
 * @page: which page (of AXIS_MOVIES_LINK_PAGE) of the files with no link
 *
 * The links page of a movie.  Reads only.
 */
gboolean
axis_movies_links_read(
	AxisMovies      *self,
	gint64           movie_id,
	const GArray    *also,
	guint            page,
	AxisMovieLinks **links,
	GError         **error
);

/**
 * axis_movies_links_review:
 * @asks: (element-type AxisMovieLinkAsk): the plan
 * @others: (nullable) (element-type gint64): the movie's files ticked to be
 *   unlinked instead of staying beside the file linked
 *
 * What the plan would do, judged as one.  Writes nothing.  Fails with
 * INVALID for a plan that cannot be (a file that is no video in a library
 * folder for movies, a file named twice, nothing chosen).
 */
gboolean
axis_movies_links_review(
	AxisMovies       *self,
	gint64            movie_id,
	const GPtrArray  *asks,
	const GArray     *others,
	AxisMovieLinks  **links,
	GError          **error
);

/**
 * axis_movies_links_apply:
 * @digest: the review's
 * @links: (out) (optional): the review as it is now (when it could be read)
 * @changed: (out) (optional): the review is not what @digest was made of:
 *   nothing was done (and %TRUE is returned)
 *
 * The plan, applied in one transaction with the movie locked.  Only links
 * change: no file on disk is touched.  Fails with IN_FLIGHT while an import
 * of the movie (or of a movie a file leaves) is open or a grab of it is in
 * flight; *@links then says why in its refused.
 */
gboolean
axis_movies_links_apply(
	AxisMovies       *self,
	gint64            movie_id,
	const GPtrArray  *asks,
	const GArray     *others,
	const gchar      *digest,
	const gchar      *user,
	AxisMovieLinks  **links,
	gboolean         *changed,
	GError          **error
);

/* What "Let Axis manage this file again" does for one file. */
typedef enum
{
	/* Identification holds the file as this movie too: the link stays and is
	 * identification's again. */
	AXIS_MOVIES_MANAGE_IDENTIFIED,
	/* An import's link: it stays; only the marks of a person go. */
	AXIS_MOVIES_MANAGE_RELEASE,
	/* Identification does not hold it as this movie: nothing to hand back. */
	AXIS_MOVIES_MANAGE_NOTHING,
	/* A file unlinked by hand: the mark goes. */
	AXIS_MOVIES_MANAGE_UNLINKED
} AxisMoviesManageKind;

typedef struct
{
	AxisMovieFacts       *movie;
	gint64                file_id;
	gchar                *path;
	AxisMoviesManageKind  kind;
	gchar                *outcome;	/* what happens, a sentence or two */
	gchar                *refused;	/* (nullable) why it cannot be done */
	gchar                *digest;
} AxisMovieLinkManage;

void
axis_movie_link_manage_free(AxisMovieLinkManage *manage);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMovieLinkManage, axis_movie_link_manage_free)

/* What handing @file_id (linked, kept or unlinked by hand, in the movie's
 * reach) back to Axis would do.  Writes nothing.  NOT_FOUND otherwise. */
gboolean
axis_movies_links_manage_review(
	AxisMovies           *self,
	gint64                movie_id,
	gint64                file_id,
	AxisMovieLinkManage **manage,
	GError              **error
);

/* Does it, in one transaction, when @digest is still the review's (else
 * *@changed, and nothing is done).  IN_FLIGHT when it cannot be done; *@manage
 * then says why. */
gboolean
axis_movies_links_manage(
	AxisMovies           *self,
	gint64                movie_id,
	gint64                file_id,
	const gchar          *digest,
	const gchar          *user,
	AxisMovieLinkManage **manage,
	gboolean             *changed,
	GError              **error
);

/* ── Brought over from another app (axis-movies-brought.c) ──────── */

/*
 * import-from-arr, slice R3.  A movie brought over from Radarr is added by
 * the code a person's Add uses, its file on disk is linked where Axis's own
 * index holds it, and its searches are held until a person starts them.
 * Nothing here opens, moves, renames or deletes a file: only rows change.
 *
 * The decisions on the proposal's questions that concern a movie are each in one
 * place here: a brought file is kept (AXIS_MOVIE_BROUGHT_KEPT), a brought
 * movie is held (axis_movies_bring() sets the hold, and
 * axis_movie_hold_stops() says what a hold stops).
 */

/* movie.search_hold. */
#define AXIS_MOVIE_HOLD_BROUGHT      "brought"
#define AXIS_MOVIE_HOLD_FILE_UNSEEN  "file_unseen"
#define AXIS_MOVIE_HOLD_DOWNLOADING  "arr_downloading"

/* Whether a file linked by bringing over is kept (never upgraded or replaced
 * by Axis itself).  Decided on 2026-11-21 (the proposal's question 2):
 * yes, such files are not upgraded automatically.  Such a link has kept
 * true and kept_how 'brought' (0073_arr_brought_kept.sql), with hand_how
 * 'brought' as before; a person lets one file go on the movie's page, or all
 * of a connection's on its page (axis_movies_release_brought()). */
#define AXIS_MOVIE_BROUGHT_KEPT      TRUE
/* movie_file.kept_how of such a link: kept because it was brought over, by no
 * person's choice. */
#define AXIS_MOVIE_KEPT_HOW_BROUGHT  "brought"

/* Whether @hold (a movie's search_hold, nullable) stops its automatic
 * searches: the scheduled search, a search for an upgrade, the search after a
 * change of profile and the follow-up after a failed grab. */
gboolean
axis_movie_hold_stops(const gchar *hold);

/* What a held movie's page and state say (%NULL for no hold): "Brought over
 * from Radarr: Axis does not search for it until you start searching.", ... */
const gchar *
axis_movie_hold_sentence(const gchar *hold);

/* The word of its mark in a list: "Held". */
const gchar *
axis_movie_hold_short(const gchar *hold);

/* What became of the file a brought movie has in the other app. */
typedef enum
{
	AXIS_MOVIE_BROUGHT_NO_FILE,	/* none was given */
	AXIS_MOVIE_BROUGHT_LINKED,	/* linked now, marked as brought over */
	AXIS_MOVIE_BROUGHT_ALREADY,	/* linked to this movie already: left as it is */
	AXIS_MOVIE_BROUGHT_ELSEWHERE,	/* linked to another movie: left there */
	AXIS_MOVIE_BROUGHT_UNLINKED,	/* a person unlinked it by hand: left so */
	AXIS_MOVIE_BROUGHT_GONE		/* the index does not hold it as present any more */
} AxisMovieBroughtFile;

/**
 * AxisMovieBring:
 * @spec: the movie, as for axis_movies_add(); its brought_from names the app
 *   and the connection, and its search_now is ignored
 * @file_id: the library file (library_file.id) the other app has for it, as
 *   Axis's index holds it at the mapped path; 0 for none
 * @quality: (nullable): the file's quality, a name of Axis's ladder
 * @release: (nullable): the release the file came from, as the app recorded
 * @has_file: the other app has a file for it, linked here or not
 */
typedef struct
{
	AxisMovieSpec  spec;
	gint64         file_id;
	const gchar   *quality;
	const gchar   *release;
	gboolean       has_file;
} AxisMovieBring;

/**
 * AxisMovieBrought:
 * @movie_id: the movie
 * @file: what became of the file
 * @other: (nullable): for %AXIS_MOVIE_BROUGHT_ELSEWHERE, the movie it is
 *   linked to ("Hearth (1995)")
 * @hold: (nullable) (static): the hold the movie has afterwards
 */
typedef struct
{
	gint64                movie_id;
	AxisMovieBroughtFile  file;
	gchar                *other;
	const gchar          *hold;
} AxisMovieBrought;

void
axis_movie_brought_clear(AxisMovieBrought *brought);

/**
 * axis_movies_bring:
 * @conn: a connection with a transaction open (the caller's: what it records
 *   of the movie commits with it)
 *
 * Adds the movie through the code a person's Add uses (so its title, profile
 * and library folder are checked as there, and the files identification holds
 * for the title are linked at once), then links @bring's file when it is
 * linked to nothing and no person unlinked it, and holds the movie's
 * searches: %AXIS_MOVIE_HOLD_FILE_UNSEEN when the other app has a file and
 * the movie has none in Axis, else %AXIS_MOVIE_HOLD_BROUGHT.  One history
 * line is written.  Fails with %AXIS_MOVIES_ERROR_EXISTS for a title managed
 * already (nothing is changed then) and %AXIS_MOVIES_ERROR_INVALID as
 * axis_movies_add().
 */
gboolean
axis_movies_bring(
	AxisMovies            *self,
	AxisDbConn            *conn,
	const AxisMovieBring  *bring,
	AxisMovieBrought      *brought,
	GError               **error
);

/**
 * axis_movies_bring_file:
 * @movie_id: a movie brought before
 * @origin: the app and the connection, for the history line
 *
 * A later run: links @file_id to the movie when the file is linked to
 * nothing and no person unlinked it by hand, and then lets go of a
 * %AXIS_MOVIE_HOLD_FILE_UNSEEN hold (to @hold_after, nullable).  The movie's
 * monitoring, profile, library folder and other links are not read or
 * changed.  In the caller's transaction, with the movie locked here.
 */
gboolean
axis_movies_bring_file(
	AxisMovies            *self,
	AxisDbConn            *conn,
	gint64                 movie_id,
	gint64                 file_id,
	const gchar           *quality,
	const gchar           *release,
	const gchar           *origin,
	const gchar           *user,
	const gchar           *hold_after,
	AxisMovieBrought      *brought,
	GError               **error
);

/**
 * axis_movies_start_searching:
 * @user: who
 * @was_held: (out) (optional): it was held
 *
 * "Let Axis search for it": ends the movie's hold, of whatever kind, and says
 * so in its history.  Nothing is searched at once: the scheduler takes it at
 * its ordinary pace.  Fails with %AXIS_MOVIES_ERROR_NOT_FOUND.
 */
gboolean
axis_movies_start_searching(
	AxisMovies   *self,
	gint64        movie_id,
	const gchar  *user,
	gboolean     *was_held,
	GError      **error
);

/**
 * axis_movies_start_brought:
 * @conn: a connection with a transaction open
 * @start: (element-type gint64): movies whose hold ends
 * @downloading: (element-type gint64): movies the other app is downloading:
 *   they are held as %AXIS_MOVIE_HOLD_DOWNLOADING
 * @origin: the app and the connection, for the history lines
 * @started: (out): how many holds ended
 *
 * "Start searching for the movies brought over".  Only a hold of the kinds
 * %AXIS_MOVIE_HOLD_BROUGHT and %AXIS_MOVIE_HOLD_DOWNLOADING is touched: a
 * movie whose file Axis cannot see stays held.
 */
gboolean
axis_movies_start_brought(
	AxisMovies    *self,
	AxisDbConn    *conn,
	const GArray  *start,
	const GArray  *downloading,
	const gchar   *origin,
	const gchar   *user,
	guint         *started,
	GError       **error
);

/**
 * axis_movies_release_brought:
 * @conn: a connection with a transaction open
 * @movies: (element-type gint64): the movies a connection brought
 * @origin: the app and the connection, for the history lines
 * @released: (out): how many files were let go
 *
 * "Let Axis upgrade these files": the files of @movies that are kept because
 * they were brought over (hand_how "brought", kept, kept_how "brought") are
 * no longer kept, and each movie's history says so.  A link a person kept or
 * let go on the movie's page, or linked by hand since, is never touched.  No
 * hold is touched, and nothing is searched at once.
 */
gboolean
axis_movies_release_brought(
	AxisMovies    *self,
	AxisDbConn    *conn,
	const GArray  *movies,
	const gchar   *origin,
	const gchar   *user,
	guint         *released,
	GError       **error
);

/* The scheduler's pace, in plain numbers: how many searches a run, and how
 * long between runs. */
void
axis_movies_search_pace(
	AxisMovies *self,
	guint      *per_run,
	guint      *interval_s
);

/* ── The follow-up and the scheduler ────────────────────────────── */

/* What the follow-up did (settled, failed, removed, removal_failed,
 * retries, completed, imports): the acquisition layer's report. */
typedef AxisAcquireFollowReport AxisMovieFollowReport;

/* The follow-up job's work (movies.follow, every minute). */
gboolean
axis_movies_run_follow(
	AxisMovies             *self,
	AxisMovieFollowReport  *report,
	GError                **error
);

/* The follow-up's prune of old searches, now and not at its hour
 * (axis_acquire_prune_searches()): one time's batches.  For the tests. */
gboolean
axis_movies_prune_searches(
	AxisMovies  *self,
	guint       *deleted,
	GError     **error
);

/**
 * AxisMovieScheduleReport:
 * @considered: monitored films looked at
 * @due: of those, films that may be searched now
 * @queued: searches queued (at most movies.searches_per_run)
 * @became_available: films whose availability changed (floor reset)
 */
typedef struct
{
	guint considered;
	guint due;
	guint queued;
	guint became_available;
} AxisMovieScheduleReport;

/* The scheduler's work (movies.schedule). */
gboolean
axis_movies_run_schedule(
	AxisMovies               *self,
	AxisMovieScheduleReport  *report,
	GError                  **error
);

/* ── For tests ──────────────────────────────────────────────────── */

/* Called (on the search's thread) just before the claim transaction, with
 * the film's id. */
typedef AxisAcquireClaimHook AxisMoviesClaimHook;

void
axis_movies_set_claim_hook(
	AxisMovies          *self,
	AxisMoviesClaimHook  hook,
	gpointer             user_data
);

/* axis_acquire_set_replace_hook(), for tests: called with the movie's id in a
 * replace of the download in flight, after the look at where the download is
 * and before its grab is ended. */
void
axis_movies_set_replace_hook(
	AxisMovies          *self,
	AxisMoviesClaimHook  hook,
	gpointer             user_data
);

G_END_DECLS

#endif /* AXIS_MOVIES_H */
