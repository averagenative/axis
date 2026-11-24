/*
 * axis-tv.h - the TV module: the series Axis manages, their seasons and
 * episodes and the state of each, searching for episodes when they air and
 * until found, grabbing a release once per episode, importing episodes file
 * by file, and their history
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A series is a metadata title (meta_title, kind series) an administrator
 * added with a quality profile, a destination root and a choice of what to
 * monitor (0028_series.sql).  Its seasons and episodes follow the metadata
 * store (axis-tv-sync.c): an episode is the metadata episode it is (the
 * provider's own id), so a renumbering moves its number and keeps its grabs,
 * files and history; an episode that appears takes its season's monitoring.
 *
 * The promises it keeps, beside the shared ones of src/acquire/:
 *
 *  - An episode's state is derived, never stored: one query reads the facts
 *    of a series' episodes (axis_tv_store_episodes()) and one function turns
 *    them into a state and its reason (axis_tv_episode_derive()).
 *  - A release is matched to the episodes it holds without guessing
 *    (axis_tv_match_release()): season and episode numbers that exist in the
 *    series' data are those episodes; a date is the episode aired that day;
 *    an absolute number alone is rejected; numbers the series' data does not
 *    have are rejected as "no such episode in the series' data" and counted
 *    on the series' page.  A release is grabbed only for this series (the
 *    matcher, with its other names, against every series of that name).
 *  - A pack is taken only when every monitored episode it holds is wanted or
 *    an upgrade and it holds no more than one season (several only for a
 *    search of the whole series); the profile decides it against the worst
 *    file of the episodes it would replace.
 *  - An episode is grabbed once: the claim locks the episodes a release is
 *    for in one transaction, and series_grab_episode allows an episode in one
 *    open grab only, so two releases that overlap never both go out.
 *  - An episode is wanted from its air date plus tv.air_delay; one with no
 *    air date is never searched for automatically.  The scheduler takes the
 *    episodes aired in the last week first, then the backlog, each paced
 *    with the floor that grows while nothing is found; a season with several
 *    wanted episodes is sought as a whole season first.
 *  - Every action is an administrator's, recorded in the series' history
 *    with the user (series_event).
 *
 * Threads: everything that touches the database or another layer's network
 * blocks: call it from a worker thread.  Create, register and free the
 * module on the main thread.
 */

#ifndef AXIS_TV_H
#define AXIS_TV_H

#include <glib.h>

#include "acquire/axis-acquire.h"
#include "core/axis-module.h"
#include "core/axis-settings.h"
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

#define AXIS_TV_MODULE_ID              "tv"

/* The search job (downloads queue; de-duplicated by what it is for:
 * AXIS_TV_SEARCH_KEY_PREFIX, the series, and the episode, the season or the
 * whole series), the follow-up and the scheduler (maintenance queue). */
#define AXIS_TV_SEARCH_KIND            "tv.search"
#define AXIS_TV_SEARCH_KEY_PREFIX      "tv.search:"
#define AXIS_TV_FOLLOW_KIND            "tv.follow"
#define AXIS_TV_FOLLOW_SCHEDULE        "tv.follow"
#define AXIS_TV_FOLLOW_INTERVAL_S      60
#define AXIS_TV_SCHEDULE_KIND          "tv.schedule"
#define AXIS_TV_SCHEDULE_SCHEDULE      "tv.schedule"
#define AXIS_TV_SCHEDULE_INTERVAL_S    900

#define AXIS_TV_SETTING_AIR_DELAY      "tv.air_delay"
#define AXIS_TV_SETTING_PACK_SHARE     "tv.pack_threshold"
#define AXIS_TV_SETTING_MONITOR        "tv.default_monitor"
#define AXIS_TV_SETTING_PER_RUN        "tv.searches_per_run"
#define AXIS_TV_SETTING_FLOOR          "tv.search_floor"
#define AXIS_TV_SETTING_FLOOR_MAX      "tv.search_floor_max"
#define AXIS_TV_SETTING_GIVE_UP        "tv.give_up_after"
#define AXIS_TV_SETTING_PROTOCOL       "tv.protocol"
#define AXIS_TV_SETTING_RETENTION      "tv.usenet_retention"
#define AXIS_TV_SETTING_LANGUAGES      "tv.languages"

/* An episode aired in the last this many days is "recent": searched before
 * the backlog. */
#define AXIS_TV_RECENT_DAYS            7

/* The scheduler queues at most this many searches of one series a run. */
#define AXIS_TV_PER_SERIES_RUN         3

/* The scheduler reads the episodes of this many series at a time: what a
 * run holds in memory is a page, however large the library. */
#define AXIS_TV_SCHEDULE_PAGE          50

/*
 * A series' episodes follow the metadata store once in this long (the store
 * itself follows the provider at the provider's pace, axis-meta-refresh.c:
 * a sync reads the store, and asks the provider only for a season that was
 * never fetched).  A run takes as many of the series that are behind as go
 * round all of them once in that time: their number times the run's interval
 * over this, so 5 a run for 100 series and 209 a run for 5,000.  Never fewer
 * than AXIS_TV_SYNC_MIN, never more than AXIS_TV_SYNC_MAX, and none begun
 * once a run has spent AXIS_TV_SYNC_BUDGET_S on them (a provider that is
 * slow to give a season).  After a start, or a long stop, every series is
 * behind: they are taken at the same pace, the longest behind first, not all
 * at once.
 */
#define AXIS_TV_SYNC_CYCLE_S           (6 * 3600)
#define AXIS_TV_SYNC_MIN               5
#define AXIS_TV_SYNC_MAX               500
#define AXIS_TV_SYNC_BUDGET_S          120

/* How many series a run of the scheduler syncs, of @n_series. */
guint
axis_tv_sync_batch(gint64 n_series);

/* How long a page or a search waits for the metadata provider. */
#define AXIS_TV_META_WAIT_MS           4000

#define AXIS_TV_ERROR (axis_tv_error_quark())

/**
 * AxisTvError:
 * @AXIS_TV_ERROR_NOT_FOUND: no such series, season, episode or candidate
 * @AXIS_TV_ERROR_EXISTS: the title is managed already
 * @AXIS_TV_ERROR_INVALID: the request cannot be made (the message says why)
 * @AXIS_TV_ERROR_REFUSED: a release a fixed rule rejects, which no one may
 *   grab
 * @AXIS_TV_ERROR_BLOCKLISTED: the release is on the series' blocklist
 * @AXIS_TV_ERROR_EXPIRED: the search that found it has expired
 * @AXIS_TV_ERROR_IN_FLIGHT: a grab of an episode it holds is in flight
 * @AXIS_TV_ERROR_UNUSABLE: nothing can be sent now (no client, the
 *   destination offline)
 * @AXIS_TV_ERROR_FAILED: the indexer or the client failed
 */
typedef enum
{
	AXIS_TV_ERROR_NOT_FOUND,
	AXIS_TV_ERROR_EXISTS,
	AXIS_TV_ERROR_INVALID,
	AXIS_TV_ERROR_REFUSED,
	AXIS_TV_ERROR_BLOCKLISTED,
	AXIS_TV_ERROR_EXPIRED,
	AXIS_TV_ERROR_IN_FLIGHT,
	AXIS_TV_ERROR_UNUSABLE,
	AXIS_TV_ERROR_FAILED
} AxisTvError;

GQuark
axis_tv_error_quark(void);

/* ── Monitoring ─────────────────────────────────────────────────── */

/**
 * AxisTvMonitor:
 * @AXIS_TV_MONITOR_ALL: every episode (specials only when chosen)
 * @AXIS_TV_MONITOR_FUTURE: the episodes not aired yet
 * @AXIS_TV_MONITOR_LATEST: the latest season
 * @AXIS_TV_MONITOR_FIRST: the first season
 * @AXIS_TV_MONITOR_NONE: nothing yet
 * @AXIS_TV_MONITOR_BROUGHT: as the app the series was brought over from had
 *   it: each season's and each episode's monitoring was copied, not chosen
 *   (axis_tv_bring()).  Never offered by a form, and refused by Add.
 *
 * What to monitor when a series is added.  A season the metadata announces
 * later is monitored when the series was added with all or future.
 */
typedef enum
{
	AXIS_TV_MONITOR_ALL,
	AXIS_TV_MONITOR_FUTURE,
	AXIS_TV_MONITOR_LATEST,
	AXIS_TV_MONITOR_FIRST,
	AXIS_TV_MONITOR_NONE,
	AXIS_TV_MONITOR_BROUGHT
} AxisTvMonitor;

/* "all", "future", "latest", "first", "none". */
const gchar *
axis_tv_monitor_name(AxisTvMonitor monitor);

gboolean
axis_tv_monitor_from_name(
	const gchar   *name,
	AxisTvMonitor *monitor
);

/* "Every episode", "Future episodes", ... */
const gchar *
axis_tv_monitor_label(AxisTvMonitor monitor);

/* ── States ─────────────────────────────────────────────────────── */

/**
 * AxisTvState:
 * @AXIS_TV_NOT_AIRED: its air date plus the delay has not passed, or it has
 *   no air date
 * @AXIS_TV_WANTED: aired, monitored, no file and nothing in flight
 * @AXIS_TV_SEARCHING: a search that holds it is queued or running
 * @AXIS_TV_DOWNLOADING: a grab of it is in flight
 * @AXIS_TV_TO_IMPORT: downloaded, waiting for the import
 * @AXIS_TV_IN_LIBRARY: a file of the library holds it
 * @AXIS_TV_UPGRADE_WANTED: in the library below the profile's cutoff,
 *   monitored
 * @AXIS_TV_ATTENTION: a person should look: the last search found nothing
 *   usable, the import of it failed, the destination is offline...
 * @AXIS_TV_UNMONITORED: not monitored (and not in the library)
 */
typedef enum
{
	AXIS_TV_NOT_AIRED,
	AXIS_TV_WANTED,
	AXIS_TV_SEARCHING,
	AXIS_TV_DOWNLOADING,
	AXIS_TV_TO_IMPORT,
	AXIS_TV_IN_LIBRARY,
	AXIS_TV_UPGRADE_WANTED,
	AXIS_TV_ATTENTION,
	AXIS_TV_UNMONITORED
} AxisTvState;

#define AXIS_TV_N_STATES 9

/* "not_aired", "wanted", "searching", "downloading", "to_import",
 * "in_library", "upgrade_wanted", "attention", "unmonitored" (stable). */
const gchar *
axis_tv_state_name(AxisTvState state);

gboolean
axis_tv_state_from_name(
	const gchar *name,
	AxisTvState *state
);

/* "Not aired", "Wanted", ..., "Needs attention", "Not monitored". */
const gchar *
axis_tv_state_label(AxisTvState state);

/* How bad a state is, for a series' own (its worst): attention first. */
guint
axis_tv_state_severity(AxisTvState state);

/**
 * AxisTvRules:
 * @today: "YYYY-MM-DD", UTC
 * @now_us: the time
 * @air_delay_s: tv.air_delay
 * @give_up_after_s: tv.give_up_after
 */
typedef struct
{
	gchar  today[11];
	gint64 now_us;
	gint64 air_delay_s;
	gint64 give_up_after_s;
} AxisTvRules;

void
axis_tv_rules_init(
	AxisTvRules *rules,
	gint64       now_us
);

/**
 * AxisTvSeries:
 *
 * What is known of one managed series (axis_tv_store_series()): its row, its
 * title, its profile, its destination.  Times are microseconds since 1970,
 * 0 for none.
 */
typedef struct
{
	gint64          id;
	gint64          title_id;
	gchar          *title;
	gchar          *original_title;
	gint            year;
	AxisMetaStatus  status;
	gint            runtime;
	gchar          *poster_ref;
	gchar          *provider;
	gchar          *tvdb_id;
	gchar          *tmdb_id;
	gchar          *imdb_id;

	gint64          profile_id;
	gchar          *profile_name;
	gchar          *profile_slug;	/* its page: /quality/<slug> */
	gboolean        profile_usable;
	AxisQuality     profile_cutoff;
	gboolean        profile_upgrades;
	/* A file of an unknown source whose resolution is known may be
	 * upgraded (the profile's upgrade_unknown), as for films. */
	gboolean        profile_upgrade_unknown;

	gint64          root_id;
	gchar          *root_name;
	gint64          volume_id;
	gchar          *volume_name;
	AxisVolumeState volume_state;

	AxisTvMonitor   monitor_choice;
	gboolean        monitor_new;
	/* Its seasons as their own rows say (the specials are not counted among
	 * them): how many there are and how many are monitored, and whether it
	 * has specials and they are monitored.  What says whether anything of it
	 * is monitored at all (axis_tv_series_monitored()). */
	guint           n_seasons;
	guint           n_seasons_monitored;
	gboolean        has_specials;
	gboolean        specials_monitored;
	gchar          *added_by;
	gint64          added_us;
	gint64          synced_us;
	/* Releases its kept searches found numbered for episodes its data does
	 * not have, and its episodes known by their place alone. */
	guint           n_mismatched;
	guint           n_by_place;
	/* Releases its kept searches found that were ruled out for stating only an
	 * absolute episode number, and for stating a season with a bare number
	 * ("S2 - 05"): what a series not marked as anime is told, and the record
	 * of the mixed form for one that is (add-anime-support, A2). */
	guint           n_absolute_only;
	guint           n_season_absolute;
	/* A download of it still at a client that Axis gave up on. */
	guint           other_live;
	/* The source of numbering it follows ("tmdb", "tvdb": TheTVDB's aired
	 * order): its episodes are that source's (add-metadata-tvdb). */
	gchar          *episode_source;
	/* (nullable) What providers' records claim about identifiers that
	 * another title holds (axis_tv_claim_texts() reads it). */
	gchar          *claims;
	/* Its title's record (provider_id) now, and the one its rows were last
	 * synced against (0059_series_record_id.sql). */
	gchar          *record_id;
	gchar          *synced_record_id;
	/* (nullable) Why no automatic search runs for it (series.search_hold,
	 * AXIS_TV_HOLD_*): it was brought over and searching was not started. */
	gchar          *search_hold;
	/* Its kind (series.kind, AXIS_TV_KIND_*): anime when an administrator
	 * marked it so, or a migration from a Sonarr that has it as anime did.
	 * Axis never marks a series by itself (add-anime-support, A1). */
	gboolean        anime;
	/* (nullable) What the migration that made it found when it compared
	 * Sonarr's absolute numbers with TheTVDB's (series.brought_absolute, JSON:
	 * axis_tv_absolute_compare_parse() reads it).  A report only. */
	gchar          *brought_absolute;
} AxisTvSeries;

/* series.kind. */
#define AXIS_TV_KIND_STANDARD "standard"
#define AXIS_TV_KIND_ANIME    "anime"

/*
 * ── Anime: absolute numbers (add-anime-support, slice A1) ──────────
 *
 * In slice A1 the kind of a series changed nothing but what its page shows.
 * Since slice A2 a name numbered absolutely is matched for an anime series
 * whose numbers are usable (axis_tv_match_release_anime(), below).  Since
 * slice A3 a search of an anime series asks in the anime category, by the
 * words of its name and an absolute number, and takes a batch as the episodes
 * it holds (anime_asks() and judge() in axis-tv-search.c).
 * The answers the proposal recommends to its four open questions are each
 * in one place:
 *   1 an anime series added by hand follows TheTVDB's aired order when Axis
 *     has a TheTVDB key and TheTVDB lists it      axis_tv_anime_source()
 *   2 Axis never marks a series anime by itself   AxisTvSpec.anime is only
 *     ever what a form or Sonarr's seriesType said; the Add form's hint is
 *     axis_tv_title_hints_anime()
 *   3 "S2 - 05" and "S01E137" are not read        axis_tv_match_release_anime()
 *     refuses both ("season_and_absolute", "no_such_episode"), and "S2 05"
 *     with them
 *   4 no absolute number can be edited            there is no such table
 *
 * An episode's absolute number is the one TheTVDB states for it in the aired
 * order the series follows (series_episode.absolute_number, copied from the
 * metadata store by every sync of the series, and by a change of numbering).
 * TMDb gives none, so a series under TMDb's numbering has none.  Nothing is
 * ever counted, inferred, or taken from a file's name, a release or Sonarr.
 */

/* The state of a series' absolute numbers, computed when read and stored
 * nowhere. */
typedef enum
{
	AXIS_TV_ABSOLUTE_NO_SOURCE,	/* the series does not follow TheTVDB's aired order */
	AXIS_TV_ABSOLUTE_NONE_GIVEN,	/* it does, and no episode of seasons 1 and up has a number */
	AXIS_TV_ABSOLUTE_USABLE,	/* each number on one episode, rising with season and episode */
	AXIS_TV_ABSOLUTE_NOT_USABLE	/* a number on two episodes, or numbers out of order */
} AxisTvAbsoluteState;

/**
 * AxisTvAbsolute:
 * @n_episodes: the series' live episodes of seasons 1 and up (a special is
 *   never counted, whatever number TheTVDB gives it)
 * @n_numbered: of those, the ones TheTVDB gives an absolute number
 * @duplicate: not usable: the two episodes at fault share a number (else the
 *   second's number is not above the first's)
 * @fault_season, @fault_number, @fault_absolute: not usable: the two episodes
 *   at fault, in the series' order
 * @plain_count: usable: every number equals the episode's place in a plain
 *   count of the series' episodes in aired order
 * @off_season, @off_number, @off_absolute, @off_count: usable and not a plain
 *   count: the first episode whose number differs from that count, its number
 *   and its place in the count
 */
typedef struct
{
	AxisTvAbsoluteState state;
	guint               n_episodes;
	guint               n_numbered;
	gboolean            duplicate;
	gint                fault_season[2];
	gint                fault_number[2];
	gint                fault_absolute[2];
	gboolean            plain_count;
	gint                off_season;
	gint                off_number;
	gint                off_absolute;
	guint               off_count;
} AxisTvAbsolute;

/*
 * The state of @series' absolute numbers from its @episodes (AxisTvEpisode,
 * in any order).  Usable only when the series follows TheTVDB's aired order,
 * every number is on exactly one live episode of seasons 1 and up, and the
 * numbers rise with season and episode.  Pure.
 */
void
axis_tv_absolute_state(
	const AxisTvSeries *series,
	const GPtrArray    *episodes,
	AxisTvAbsolute     *absolute
);

/* What the series' page says of that state, a sentence or two (transfer
 * full).  @tvdb_key: Axis has a TheTVDB key (what "no source" says to do). */
gchar *
axis_tv_absolute_sentence(
	const AxisTvSeries   *series,
	const AxisTvAbsolute *absolute,
	gboolean              tvdb_key
);

/* An episode whose absolute number differs between the other app and TheTVDB. */
typedef struct
{
	gint season;
	gint number;
	gint theirs;	/* the other app's */
	gint tvdb;	/* TheTVDB's, as Axis holds it */
} AxisTvAbsoluteDiffer;

/* The episodes that differ that are kept and listed, at most. */
#define AXIS_TV_ABSOLUTE_LISTED 50

/**
 * AxisTvAbsoluteCompare:
 * @agree: episodes both give the same number
 * @differ: episodes both give a number, and not the same
 * @one_side: episodes only one of the two gives a number
 * @differs: (element-type AxisTvAbsoluteDiffer): the first
 *   %AXIS_TV_ABSOLUTE_LISTED that differ, in the order they were added
 *
 * Sonarr's own absolute numbers beside TheTVDB's, for a series Sonarr has as
 * anime.  A report only: nothing is linked, moved, monitored or held
 * differently because they disagree.  It is the one independent check of
 * TheTVDB's numbers Axis can make.
 */
typedef struct
{
	guint   agree;
	guint   differ;
	guint   one_side;
	GArray *differs;
} AxisTvAbsoluteCompare;

void
axis_tv_absolute_compare_init(AxisTvAbsoluteCompare *compare);

void
axis_tv_absolute_compare_clear(AxisTvAbsoluteCompare *compare);

/* One episode of seasons 1 and up that both sides list: @theirs and @tvdb
 * are 0 for none.  An episode neither gives a number is counted nowhere. */
void
axis_tv_absolute_compare_add(
	AxisTvAbsoluteCompare *compare,
	gint                   season,
	gint                   number,
	gint                   theirs,
	gint                   tvdb
);

/* As stored in series.brought_absolute (transfer full), and read back
 * (%FALSE: @json is none of it; @compare is initialised either way). */
gchar *
axis_tv_absolute_compare_json(const AxisTvAbsoluteCompare *compare);

gboolean
axis_tv_absolute_compare_parse(
	const gchar           *json,
	AxisTvAbsoluteCompare *compare
);

/* "Absolute numbers: Sonarr and TheTVDB agree for 291 of 291 episodes." or
 * "... differ for 3 of 291 episodes, the first S03E02 (Sonarr 29, TheTVDB
 * 30). ..." (transfer full). */
gchar *
axis_tv_absolute_compare_sentence(const AxisTvAbsoluteCompare *compare);

/* The episodes that differ, "S03E02 (Sonarr 29, TheTVDB 30); ...", with how
 * many more there are than are listed; %NULL for none (transfer full). */
gchar *
axis_tv_absolute_compare_list(const AxisTvAbsoluteCompare *compare);

/* Whether a stored title's details say animation whose original language is
 * Japanese (@genres: (nullable) (element-type utf8)): what the Add form says
 * beside its "Anime" box.  It ticks nothing: Axis never marks a series anime
 * by itself. */
gboolean
axis_tv_title_hints_anime(
	const GPtrArray *genres,
	const gchar     *original_language
);

/*
 * ── Anime: what a search asks for (add-anime-support, slice A3) ────
 *
 * No indexer has a parameter for an absolute number: it can only be asked as
 * words, the series' name and the number.  All of this is pure.
 */

/* Whether @name is written in Latin letters: it has a letter, and every
 * letter it has is Latin (digits, spaces and punctuation are any name's). */
gboolean
axis_tv_name_is_latin(const gchar *name);

/**
 * axis_tv_anime_second_name:
 * @record: the series' stored details
 *
 * The one other name of an anime series that a search asks by, beside its
 * title: fansub releases are usually named by the romanised original title.
 * By a fixed rule: the original title when it is written in Latin letters and
 * is not the title again; else the other name the details mark as Japanese
 * (its language "ja", or its country "JP") that is written in Latin letters,
 * the first by its text when there are several.  A name whose title key is
 * the title's, or holds the title's (a part's name: "Tenkuu Rangers 2nd
 * Season", under which no absolute number is read), is passed over.
 *
 * Returns: (nullable) (transfer none): the name, borrowed from @record
 */
const gchar *
axis_tv_anime_second_name(const AxisMetaTitle *record);

/* How many digits a series' absolute numbers are written with in the words a
 * search asks: 2, or 3 when the highest of @absolute (@n of them, 0 for none)
 * is 100 or more: groups pad to the width of the series. */
guint
axis_tv_absolute_digits(
	const gint *absolute,
	guint       n
);

/* The words a search asks for an absolute number: "Tenkuu Rangers 137",
 * "Tenkuu Rangers 05", "Tenkuu Rangers 005" (@digits: 2 or 3).  %NULL when
 * @name is empty or @absolute is not 1 to 9999 (transfer full). */
gchar *
axis_tv_absolute_words(
	const gchar *name,
	gint         absolute,
	guint        digits
);

/* "TMDb", "TheTVDB's aired order", "TheTVDB's DVD order", ...: a source of
 * numbering as a page names it. */
const gchar *
axis_tv_source_label(const gchar *source);

/* Where a source's episodes are listed, as a sentence names it: "TMDb's
 * episode list", "TheTVDB's aired order". */
const gchar *
axis_tv_source_list_name(const gchar *source);

/* Whether @source is one of TheTVDB's orders ("tvdb", "tvdb-dvd", ...). */
gboolean
axis_tv_source_is_tvdb(const gchar *source);

/* The disagreements of @series' identifiers as sentences (one each), or an
 * empty array. */
GPtrArray *
axis_tv_claim_texts(const AxisTvSeries *series);

void
axis_tv_series_free(AxisTvSeries *series);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvSeries, axis_tv_series_free)

/**
 * AxisTvEpisode:
 *
 * What is known of one episode of a managed series, as
 * axis_tv_store_episodes() reads it.
 */
typedef struct
{
	gint64                 id;
	gint64                 series_id;
	gchar                 *provider_id;	/* (nullable): known by its place */
	/* The source of numbering its identity is of: the series' own, or (a row
	 * marked gone by a change of numbering) the one it followed before. */
	gchar                 *source;
	gint                   season;
	gint                   number;
	gint                   absolute;
	gchar                 *title;
	gchar                 *air_date;	/* "YYYY-MM-DD" or %NULL */
	gint                   runtime;
	gboolean               monitored;
	gboolean               season_monitored;
	gboolean               gone;
	gint64                 last_search_us;
	gint64                 next_search_us;
	gint64                 floor_s;

	/* A search job that holds it: queued or running. */
	gboolean               search_queued;
	gboolean               search_running;

	/* Its open grab, if any, with its download. */
	gint64                 grab_id;
	gchar                 *grab_release;
	gchar                 *grab_holds;
	gboolean               grab_completed;
	gint64                 download_id;
	gboolean               download_known;
	AxisDownloadState      download_state;
	gint64                 download_size;
	gint64                 download_done;
	gchar                 *client_name;
	/* A closed grab of it whose download is still at its client. */
	gboolean               removal_pending;

	/* The latest import of its open grab's download. */
	gint64                 import_id;
	gchar                 *import_state;
	gchar                 *import_text;

	/* Its files present in the library: how many, the worst and the best by
	 * quality, the name the best came in as. */
	guint                  n_files;
	AxisQuality            file_quality;
	AxisReleaseResolution  file_resolution;
	AxisQuality            worst_quality;
	AxisReleaseResolution  worst_resolution;
	gchar                 *worst_release;
	/* The worst is a file Axis converted (it counts as worst_quality, its
	 * source's, unknown when that was not recorded). */
	gboolean               worst_converted;
	gchar                 *file_path;	/* the best one's, stored form */
	gint64                 files_size;	/* of its files present */
	/* (nullable) Who keeps a present file of it by hand (series_file.kept: a
	 * forced grab's file, or one a person keeps; "a person" when no name was
	 * recorded): then it is not upgraded by itself. */
	gchar                 *kept_by;
	/* With @kept_by: every present file of it that is kept is kept because it
	 * was brought over from Sonarr (kept_how "brought"), and no person chose
	 * it.  The pages then say that, not "kept by hand by NAME". */
	gboolean               kept_brought;
	/* (nullable) How a person made the link of a present file of it
	 * ("forced", "assigned", "linked"). */
	gchar                 *hand_how;
	/* (nullable) Each present file of it that a person made the link of or
	 * keeps, for a keep button of its own: "<library file id>:<1 if kept>",
	 * comma between, by file id. */
	gchar                 *hand_files;

	/* The last finished search for it alone. */
	gint64                 search_id;
	gchar                 *search_outcome;
	gchar                 *search_text;
	/* (nullable) Why no automatic search runs for this episode alone
	 * (series_episode.search_hold, AXIS_TV_HOLD_FILE_UNSEEN). */
	gchar                 *search_hold;
} AxisTvEpisode;

void
axis_tv_episode_free(AxisTvEpisode *episode);

/* Whether @episode has aired by @rules (its air date's start, UTC, plus the
 * delay); *@from: when it does (0: no air date). */
gboolean
axis_tv_episode_aired(
	const AxisTvEpisode *episode,
	const AxisTvRules   *rules,
	gint64              *from
);

/* Whether an episode with a file is below its profile's cutoff. */
gboolean
axis_tv_episode_upgradable(
	const AxisTvEpisode *episode,
	const AxisTvSeries  *series
);

/* Whether a grab of the episode is in flight (open, or a download Axis gave
 * up on still at its client). */
gboolean
axis_tv_episode_in_flight(const AxisTvEpisode *episode);

/**
 * axis_tv_episode_derive:
 * @reason: (out) (optional) (transfer full): why, a sentence
 *
 * The episode's one state, in this order: downloading, to import (or needs
 * attention for an import that failed), needs attention for a download left
 * at a client, searching, in library (or upgrade wanted), not monitored, not
 * aired, needs attention (no destination, destination offline, an unusable
 * profile, the last search's outcome), wanted.  Pure.
 */
AxisTvState
axis_tv_episode_derive(
	const AxisTvEpisode *episode,
	const AxisTvSeries  *series,
	const AxisTvRules   *rules,
	gchar              **reason
);

/* The same, and for one that needs attention why, as a kind (NONE for the
 * other states): what the pages offer to put it right. */
AxisTvState
axis_tv_episode_derive_attention(
	const AxisTvEpisode   *episode,
	const AxisTvSeries    *series,
	const AxisTvRules     *rules,
	gchar                **reason,
	AxisAcquireAttention  *kind
);

/* Whether a last search's @outcome means the episode needs attention. */
gboolean
axis_tv_outcome_needs_attention(const gchar *outcome);

/* "S01E02". */
gchar *
axis_tv_episode_label(
	gint season,
	gint number
);

/* "S01E02", "S01E02-E03" (a run), "S01E02E05"; "" for none. */
gchar *
axis_tv_episode_label_of(
	gint        season,
	const gint *numbers,
	guint       n
);

/* "Fixture Street (2021)". */
gchar *
axis_tv_series_label(
	const gchar *title,
	gint         year
);

/* ── Matching a release to episodes (pure) ──────────────────────── */

/**
 * AxisTvKnown:
 *
 * One episode of the series' data as the matcher sees it.
 */
typedef struct
{
	gint64       id;
	gint         season;
	gint         number;
	const gchar *air_date;
} AxisTvKnown;

/**
 * AxisTvHolds:
 * @ok: the release holds episodes of the series' data
 * @code: when not ok: "absolute_only", "no_such_episode", "no_such_season",
 *   "no_such_date", "ambiguous_date", "no_episode", "cross_season"; and from
 *   axis_tv_match_release_anime() alone: "season_and_absolute",
 *   "absolute_unusable", "absolute_zero", "absolute_decimal",
 *   "absolute_unknown", "absolute_unaired", "absolute_ambiguous",
 *   "absolute_batch", "absolute_not_read", "absolute_part_name",
 *   "numberings_differ", "special"
 * @by_absolute: what it holds was found through absolute numbers
 * @absolute_text: (nullable): then, for a history line: "137 is S06E12 by
 *   TheTVDB", "1 to 28 are S01E01 to S02E04 by TheTVDB"
 * @batch: it states a range of absolute numbers.  A range that is exactly
 *   whole seasons is their pack (@pack).  Any other range is refused as
 *   "absolute_batch", unless the caller reads batches
 *   (%AxisTvAnimeRead.batches): then it holds exactly the episodes the numbers
 *   map to, of one season or several, with @pack %FALSE and a @label such as
 *   "S01E01-S02E04"
 * @text: (transfer full): why not, or what it holds in words
 * @indices: (element-type guint): into the known episodes: what it holds,
 *   by season and number
 * @pack: it holds whole seasons (a season pack, several, or Complete)
 * @season_first, @season_last: the seasons it holds
 * @label: "S01E02", "S01E02-E03", "S01", "S01-S03"
 * @found: (element-type guint) (nullable): with "no_such_episode", the
 *   episodes it names that the data has (never taken by the matcher: a
 *   person may grab it by hand for them)
 * @found_label: (nullable): those, "S01E05"
 * @missing: (nullable): with "no_such_episode", the numbers the data does
 *   not have, "S01E06"
 * @person_only: it holds these episodes by an absolute number, and by a
 *   reading too weak for a search to grab on by itself: the number was read
 *   only because it follows the series' name, or it stands under a name that
 *   is not the series' title or original title and is low enough to be a
 *   later season's own count.  A search lists it; a person may grab it
 * @person_why: (nullable): then, why, as sentences for the release's reason
 */
typedef struct
{
	gboolean     ok;
	const gchar *code;
	gchar       *text;
	GArray      *indices;
	gboolean     pack;
	gint         season_first;
	gint         season_last;
	gchar       *label;
	GArray      *found;
	gchar       *found_label;
	gchar       *missing;
	gboolean     by_absolute;
	gchar       *absolute_text;
	gboolean     batch;
	gboolean     person_only;
	gchar       *person_why;
} AxisTvHolds;

void
axis_tv_holds_clear(AxisTvHolds *holds);

/**
 * axis_tv_match_release:
 * @parsed: what the release parser read from a name
 * @known: (array length=n): the series' episodes (not gone)
 *
 * What the name holds, never guessed: season and episode numbers that
 * exist in @known are those episodes; a season with no episode named is the
 * whole season (it must exist); "Complete" with no season is every season
 * but the specials; a date is the episode aired that day (one, else not);
 * an absolute number alone is not matched; a number @known does not have is
 * "no such episode in the series' data".
 */
void
axis_tv_match_release(
	const AxisRelease  *parsed,
	const AxisTvKnown  *known,
	guint               n,
	AxisTvHolds        *holds
);

/*
 * ── Anime: a name numbered absolutely (add-anime-support, slice A2) ──
 *
 * For a series marked as anime whose absolute numbers are usable, and for no
 * other series, a name that states an absolute episode number is matched to
 * the one episode that has that number.  The mapping is a lookup in the
 * series' episodes as they are now (TheTVDB's number for each, under the
 * numbering the series follows): never arithmetic, never a count, never
 * clamped.  Every doubtful form is refused with a code and a sentence.
 *
 * The proposal's question 3 is answered here, in one place: "S2 - 05" and
 * "S01E137" are refused ("season_and_absolute", "no_such_episode"), never
 * interpreted.
 */

/**
 * AxisTvAnimeRead:
 * @anime: the series is marked as anime (series.kind); %FALSE: every name is
 *   matched exactly as axis_tv_match_release() matches it
 * @state: the state of its absolute numbers (axis_tv_absolute_state())
 * @absolute: (array) (nullable): TheTVDB's absolute number of each known
 *   episode, in the order of the known episodes the matcher is given and as
 *   many; 0 for none.  %NULL: no episode has one
 * @today: "YYYY-MM-DD", UTC: a number of an episode that airs after it, or
 *   has no air date, is refused
 * @name: (nullable): the name itself, for what reading it by itself leaves
 *   out: the reading after the series' names, a special's word, half an
 *   episode ("12.5")
 * @titles: (array length=n_titles) (nullable): the series' names (its title,
 *   its original title, its other names), for the reading after a title
 * @n_own: how many of @titles, from the first, are the series' title and its
 *   original title (0 is taken as 1).  An absolute number counts the series'
 *   episodes under these; under another name that is one of the series' names
 *   with more after it ("Tenkuu Rangers 2nd Season") it does not
 * @year: the series' year, 0 when not known: a name read after the series'
 *   name that states another year (more than one off) is not the series'
 * @batches: a range of absolute numbers that is not exactly whole seasons is
 *   what it holds (a batch: slice A3), and not refused as "absolute_batch".
 *   Only a search's judging of a release sets it, and the claim's own reading
 *   of the release it grabbed: a file of a download, a file on disk and a
 *   grab by force are read without it, so that no file is ever placed on a
 *   range of episodes by this reading
 */
typedef struct
{
	gboolean             anime;
	AxisTvAbsoluteState  state;
	const gint          *absolute;
	const gchar         *today;
	const gchar         *name;
	const gchar *const  *titles;
	guint                n_titles;
	guint                n_own;
	gint                 year;
	gboolean             batches;
} AxisTvAnimeRead;

/**
 * axis_tv_match_release_anime:
 * @read: (nullable): the series' kind and numbers; %NULL, or not anime: the
 *   answer is axis_tv_match_release()'s, unchanged
 *
 * What a name holds of an anime series.  A date, a season with episodes, a
 * whole season and "Complete" are matched as for any series; a name that
 * states a season and an episode is never read as absolute.  Beyond that:
 *
 *  - an absolute number alone ("- 137", "E137", or one read after the
 *    series' name: "Tenkuu Rangers 137") is the one episode of seasons 1 and
 *    up whose absolute number it is.  No such episode, an episode not aired,
 *    0, a number on two episodes, or numbers that are not usable: refused;
 *  - a bare number of three or four digits (no hyphen, no "E") that is also a
 *    season and an episode the series has ("101": S01E01) is refused as
 *    ambiguous;
 *  - a range must map whole.  Exactly one or more whole seasons is their pack;
 *    any other range is "absolute_batch", or, for a caller that reads batches
 *    (a search judging a release), the episodes it maps to;
 *  - a season and an episode beside an absolute number must be the same
 *    episode, or the name is refused ("numberings_differ");
 *  - a season with a bare number ("S2 - 05", "S2 05") is refused;
 *  - a special named by a word ("OVA 2") is refused: assign it by hand;
 *  - an absolute number under a name that is one of the series' names with
 *    more after it, and is not the series' title or original title ("Tenkuu
 *    Rangers 2nd Season - 05", "Tenkuu Rangers II 05"), is refused
 *    ("absolute_part_name"): it counts a part's episodes, not the series';
 *  - a second number after the one read by itself ("- 05 - 06", "- 05 [06]")
 *    is refused.
 */
void
axis_tv_match_release_anime(
	const AxisRelease     *parsed,
	const AxisTvKnown     *known,
	guint                  n,
	const AxisTvAnimeRead *read,
	AxisTvHolds           *holds
);

/* Whether a name that reading by itself gives no episode and another title
 * ("Tenkuu Rangers 137": the title read is "Tenkuu Rangers 137") is the
 * series' all the same: the series is anime with usable numbers, @parsed
 * states no season, episode, date or absolute number, and the reading after
 * the series' names (@read's name and titles) gives a number or a range.
 * *@why_not (optional, transfer full): when the name begins with the series'
 * name and what follows is not read, or the name states a year that is not
 * the series', why, as a clause.
 *
 * A name claimed this way is the weakest reading there is (nothing in the
 * name marks the number as an episode's, and another title's name can look
 * the same): a search lists it and never grabs it by itself; a person may. */
gboolean
axis_tv_anime_claims(
	const AxisRelease      *parsed,
	const AxisTvAnimeRead  *read,
	gint                   *version,
	gchar                 **why_not
);

/* Whether a name the series would claim (axis_tv_anime_claims(), @read's
 * name) can be another title's: one of @others, the names of the other titles
 * a search compared, is a name the name begins with too (the same name, or a
 * longer one that takes the number in), or is the series' name followed by
 * that very number ("Tenkuu Rangers 2" for "Tenkuu Rangers 02").
 * Returns: (nullable) (transfer full): why it is not claimed, as a clause. */
gchar *
axis_tv_anime_other_title(
	const AxisTvAnimeRead *read,
	const gchar *const    *others,
	guint                  n_others
);

/* The state of a series' absolute numbers as a refusal names it ("the series
 * does not follow TheTVDB's aired order", ...). */
const gchar *
axis_tv_absolute_state_words(AxisTvAbsoluteState state);

/* ── The module ─────────────────────────────────────────────────── */

typedef struct _AxisTv AxisTv;

/**
 * axis_tv_new:
 * @pool: the database, which must outlive the module
 * @settings: (nullable): the settings store
 *
 * Registers the series as references of the metadata layer and as users of
 * quality profiles.  Main thread, before any of those layers is used.
 */
AxisTv *
axis_tv_new(
	AxisDbPool       *pool,
	AxisSettings     *settings,
	AxisMeta         *meta,
	AxisIndexers     *indexers,
	AxisDownloads    *downloads,
	AxisStorage      *storage,
	AxisQualityStore *quality
);

AxisTv *
axis_tv_ref(AxisTv *self);

void
axis_tv_unref(AxisTv *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTv, axis_tv_unref)

/* The household's players, for the profiles' players preference. */
void
axis_tv_set_compat(
	AxisTv     *self,
	AxisCompat *compat
);

/* Registers the series as an owner of imports (file by file); without it,
 * completed episodes stay "to import".  Main thread, before use. */
void
axis_tv_set_import(
	AxisTv     *self,
	AxisImport *import
);

/**
 * axis_tv_folder_titles:
 * @user_data: the #AxisTv (not owned)
 * @root_id: the series root the folders are in
 * @dirs: (element-type AxisDirRecord): the Library's folders of a page
 * @titles: (element-type AxisWebFolderTitle): what each is, added here
 *
 * An #AxisWebFolderTitlesFunc (src/web/axis-web-browser.h) for the series
 * roots: a folder at the top of one is a series' folder, listed with the
 * series a file in it is linked to (by an import), or as not identified.
 * One statement for the page; each folder's files are walked by the index
 * of their paths until one is linked.  Blocks: a worker thread.
 */
gboolean
axis_tv_folder_titles(
	gpointer    user_data,
	gint64      root_id,
	GPtrArray  *dirs,
	GPtrArray  *titles,
	GError    **error
);

/* The module's description, for axis_module_registry_register() with the
 * module as user data. */
const AxisModuleInfo *
axis_tv_get_module(void);

/* The module's pages wait for their work in flight (main loop). */
void
axis_tv_stop(AxisTv *self);

/* The rules now, from the settings (and the clock a test set). */
void
axis_tv_get_rules(
	AxisTv      *self,
	AxisTvRules *rules
);

/* For tests: the time every rule is read at (0: the real time). */
void
axis_tv_set_clock(
	AxisTv *self,
	gint64  now_us
);

/* ── Reading ────────────────────────────────────────────────────── */

/**
 * AxisTvAttentionItem:
 *
 * An episode that needs attention, with why (the sentence its row says, and
 * its kind) and what that refers to: its last search (0: none) and the
 * import that needs a person (0: none).
 */
typedef struct
{
	gint64                episode_id;
	gint                  season;
	gint                  number;
	AxisAcquireAttention  kind;
	gchar                *reason;
	gint64                search_id;
	gint64                import_id;
} AxisTvAttentionItem;

void
axis_tv_attention_item_free(AxisTvAttentionItem *item);

/**
 * AxisTvAttentionGroup:
 * @kind, @reason: what its episodes have in common (borrowed)
 * @items: (element-type AxisTvAttentionItem): borrowed, by season and number
 * @order: where its first episode came
 */
typedef struct
{
	AxisAcquireAttention  kind;
	const gchar          *reason;
	GPtrArray            *items;
	guint                 order;
} AxisTvAttentionGroup;

/* The items by their reason, the most common first (AxisTvAttentionGroup,
 * borrowing from @items). */
GPtrArray *
axis_tv_attention_groups(const GPtrArray *items);

/* "S01E03 needs attention: <its reason>", "3 episodes need attention: ...",
 * "5 episodes need attention. The most common reason, for 3 of them: ...";
 * %NULL for none. */
gchar *
axis_tv_attention_sentence(const GPtrArray *items);

/**
 * AxisTvSummary:
 *
 * A series with its episodes' states counted: how many of its aired
 * monitored episodes are in the library, and its worst state.  When it
 * needs attention, @reason says why in words (the most common reason first)
 * and @attention lists each episode that needs it (AxisTvAttentionItem, by
 * season and number).
 *
 * The series' own state, when no episode of it is wanted, in flight, not
 * aired or in need of attention (every episode is in the library or not
 * monitored): %AXIS_TV_UNMONITORED only when nothing of it is monitored
 * (axis_tv_series_monitored() says no: no season, no episode, and new
 * seasons are not); else %AXIS_TV_IN_LIBRARY when an episode of it is in the
 * library, and %AXIS_TV_NOT_AIRED when none is.  Episodes that are not
 * monitored (the specials, a season left out) never make a monitored series
 * read as not monitored, whatever the order its episodes are counted in.
 */
typedef struct
{
	AxisTvSeries *series;
	guint         counts[AXIS_TV_N_STATES];
	guint         in_library;	/* aired, monitored, in the library */
	guint         aired_monitored;
	guint         episodes;
	guint         monitored;	/* monitored, aired or not, with a file or not */
	AxisTvState   state;
	gchar        *reason;
	gchar        *next_air_date;	/* the next monitored episode's, or %NULL */
	GPtrArray    *attention;
	/* Of a summary counted without reading its episodes
	 * (axis_tv_list_counted()): how many of its episodes need attention
	 * because their last search found a release only a person can grab, and
	 * its place among the summaries counted with it. */
	guint         held;
	guint         overview_index;
} AxisTvSummary;

void
axis_tv_summary_free(AxisTvSummary *summary);

/* Whether anything of the series is monitored: a season, the specials, one
 * of its @monitored_episodes, or the seasons that come later. */
gboolean
axis_tv_series_monitored(
	const AxisTvSeries *series,
	guint               monitored_episodes
);

/* Every series (AxisTvSummary), by title, from the facts of every episode
 * of the library.  No page uses it: it is what axis_tv_list_counted() is
 * compared with (tests/test-web-tv-scale.c). */
gboolean
axis_tv_list(
	AxisTv      *self,
	GPtrArray  **series,
	GError     **error
);

/* For tests: how many episodes' facts (AxisTvEpisode) every AxisTv of the
 * process has read from the database so far.  A page that lists the library
 * reads a page's worth, whatever the library's size. */
guint
axis_tv_episodes_read(void);

/* For tests, to explain their plans: the text of a statement of the store,
 * by name: "classes" (the episodes counted by class; $1 series ids or NULL,
 * $2 the last day that has aired, $3 'wanted', 'upgrade' or neither),
 * "episodes" (the episode facts of the series $1; $2 the gone ones too),
 * "series-list" ($1 series ids or NULL) and "series-all".  The caller frees
 * it; %NULL for another name. */
gchar *
axis_tv_store_statement(const gchar *name);

/* Every series (AxisTvSummary), by title, as the lists read them: the
 * episodes counted by the database (two statements, no episode read), and,
 * with @attention, the episodes of the series that need attention (one more
 * statement, when there are any; a list reads them for its page only).  The
 * same counts, states and sentences as axis_tv_list(); without @attention a
 * series that needs attention has its counts and its state, no sentence
 * (@reason is %NULL) and no episodes in @attention.  A series' facts that
 * only its own page shows (AXIS_TV_SERIES_LIST) are not read. */
gboolean
axis_tv_list_counted(
	AxisTv      *self,
	gboolean     attention,
	GPtrArray  **series,
	GError     **error
);

/* The series of these meta_title ids (gint64) that are managed
 * (AxisTvSummary). */
gboolean
axis_tv_get_by_titles(
	AxisTv      *self,
	GArray      *title_ids,
	GPtrArray  **series,
	GError     **error
);

/* One series' facts and its episodes (AxisTvEpisode, by season and number;
 * gone ones last). */
gboolean
axis_tv_get(
	AxisTv         *self,
	gint64          series_id,
	AxisTvSeries  **series,
	GPtrArray     **episodes,
	GError        **error
);

typedef struct
{
	gint64  id;
	gint64  series_id;	/* 0: the series was removed */
	gint64  title_id;
	gchar  *title_label;
	gint64  episode_id;
	gchar  *kind;
	gchar  *text;
	gchar  *by_user;
	gint64  created_us;
} AxisTvEvent;

void
axis_tv_event_free(AxisTvEvent *event);

/* The history (AxisTvEvent), the latest first: of one series (0: of all),
 * @limit events after @before_id (0: from the latest). */
gboolean
axis_tv_list_events(
	AxisTv      *self,
	gint64       series_id,
	gint64       before_id,
	guint        limit,
	GPtrArray  **events,
	GError     **error
);

/**
 * AxisTvReleases:
 *
 * A search's releases for an episode or a season: the search and its
 * candidates, each with the episodes it holds ("S01E02-E03").
 */
typedef struct
{
	AxisAcquireSearchInfo *search;		/* (nullable) none yet */
	gchar                 *scope;
	GPtrArray             *candidates;	/* AxisAcquireCandidate */
	GPtrArray             *holds;		/* gchar *, one per candidate (nullable entries) */
	GArray                *mismatched;	/* gboolean, one per candidate */
	GPtrArray             *blocklist;	/* AxisAcquireBlock */
} AxisTvReleases;

void
axis_tv_releases_free(AxisTvReleases *releases);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvReleases, axis_tv_releases_free)

/* The latest search for the episode @episode_id (season @season of the
 * series when it is 0 and @season >= 0) with its releases. */
gboolean
axis_tv_get_releases(
	AxisTv           *self,
	gint64            series_id,
	gint64            episode_id,
	gint              season,
	AxisTvReleases  **releases,
	GError          **error
);

/* ── Acting (administrators) ────────────────────────────────────── */

/**
 * AxisTvSpec:
 * @title_id: the meta_title (a series)
 * @profile_id: a quality profile (0: the default)
 * @root_id: a library root of type tv
 * @monitor: what to monitor
 * @specials: monitor the specials too (with all)
 * @search_now: search for the wanted episodes at once
 * @user: who
 * @source: (nullable): the source of numbering the series follows from the
 *   start ("tvdb": TheTVDB's aired order); %NULL: its record provider's own.
 *   One the series cannot follow now (TheTVDB not offered, the series without
 *   a TheTVDB identifier, or its order not in the metadata store) is INVALID:
 *   a series is never added under one numbering to be switched after.
 * @brought_from: (nullable): the series is brought over from another app
 *   ("Sonarr (NAME)"): its history line says so, nothing is searched for
 *   whatever @search_now says, and its library folder need not be writable
 *   now (axis_tv_bring() is the only caller that sets it)
 * @anime: the series is of the kind anime: an administrator ticked the box
 *   on the Add form, or the other app has it as anime.  Added by hand with no
 *   @source, it follows the numbering axis_tv_anime_source() gives
 */
typedef struct
{
	gint64         title_id;
	gint64         profile_id;
	gint64         root_id;
	AxisTvMonitor  monitor;
	gboolean       specials;
	gboolean       search_now;
	const gchar   *user;
	const gchar   *source;
	const gchar   *brought_from;
	gboolean       anime;
} AxisTvSpec;

/**
 * axis_tv_anime_source:
 *
 * The numbering a series added by hand as anime follows (the proposal's
 * question 1, built to its recommendation): TheTVDB's aired order ("tvdb"),
 * which carries absolute numbers, when Axis has a TheTVDB key and TheTVDB
 * lists the series (its order is asked for when the metadata store does not
 * hold it, so this may wait for the provider; call it outside a
 * transaction); else %NULL, the numbering any new series gets.  %NULL too
 * for a series whose episodes Identify holds on disk (one being managed where
 * its files are): it keeps the numbering its files were read under, and its
 * numbering is changed on the page that shows the mapping first.
 *
 * Returns: (nullable) (transfer none): a static string
 */
const gchar *
axis_tv_anime_source(
	AxisTv *self,
	gint64  title_id
);

/* Whether a series added as anime now can follow TheTVDB's aired order at
 * all (Axis has a TheTVDB key): what the Add form says beside the box. */
gboolean
axis_tv_tvdb_offered(AxisTv *self);

/**
 * axis_tv_set_kind:
 * @anime: the kind to set
 * @changed: (out) (optional): whether it was another kind before
 *
 * Marks a series as anime, or as standard again (an administrator, on the
 * series' page).  One write and one history line; no file, link, episode,
 * monitoring or numbering changes with it, and nothing is searched for.
 */
gboolean
axis_tv_set_kind(
	AxisTv       *self,
	gint64        series_id,
	gboolean      anime,
	const gchar  *user,
	gboolean     *changed,
	GError      **error
);

/* Adds a series, its seasons and episodes as the metadata has them (asking
 * the provider for the seasons not fetched yet), monitored as chosen. */
gboolean
axis_tv_add(
	AxisTv            *self,
	const AxisTvSpec  *spec,
	gint64            *series_id,
	GError           **error
);

/* Changes the profile and the root (the spec's other fields are ignored). */
gboolean
axis_tv_edit(
	AxisTv            *self,
	gint64             series_id,
	const AxisTvSpec  *spec,
	GError           **error
);

/**
 * AxisTvEdited:
 * @changed: anything changed
 * @profile_changed: the quality profile changed
 * @searches: the searches that queued (with a new profile)
 */
typedef struct
{
	gboolean changed;
	gboolean profile_changed;
	guint    searches;
} AxisTvEdited;

/**
 * axis_tv_edit_full:
 * @episode: (optional) an episode to search for too with a new profile (0:
 *   none), the one whose releases the change was made from
 * @season: likewise a season (-1: none)
 * @edited: (out) (optional): what came of it
 *
 * axis_tv_edit(), and with a new profile every wanted episode is searched
 * for again at once (cause "profile"), as the scheduler would group them
 * (whole seasons where enough of one is wanted), their waits set back.  An
 * episode in the library at or above the new cutoff is not; one below it is
 * an upgrade by the usual rules.  A change of destination alone searches
 * for nothing.  All in one transaction with the change and its history.
 */
gboolean
axis_tv_edit_full(
	AxisTv            *self,
	gint64             series_id,
	const AxisTvSpec  *spec,
	gint64             episode,
	gint               season,
	AxisTvEdited      *edited,
	GError           **error
);

/**
 * axis_tv_request_undated:
 * @queued: (out) (optional): searches queued
 *
 * Searches by hand for every monitored episode of @season that has no air
 * date and no file and is not in flight (the scheduler never takes them):
 * one episode search each, at most 100, and one line in the history.
 */
gboolean
axis_tv_request_undated(
	AxisTv       *self,
	gint64        series_id,
	gint          season,
	const gchar  *user,
	guint        *queued,
	GError      **error
);

/**
 * axis_tv_try_profiles:
 * @search_id: one of the series' searches
 * @candidates: (element-type AxisAcquireCandidate): releases of that search
 * @profile_ids: (element-type gint64)
 * @tries: (out) (element-type AxisAcquireProfileTry)
 *
 * What each profile would make of each release now, for what that search
 * was for (axis_acquire_try_profiles()).  Writes nothing.
 */
gboolean
axis_tv_try_profiles(
	AxisTv           *self,
	gint64            series_id,
	gint64            search_id,
	const GPtrArray  *candidates,
	const GArray     *profile_ids,
	GPtrArray       **tries,
	GError          **error
);

/* Forgets a series (its history is kept).  No file is touched. */
gboolean
axis_tv_remove(
	AxisTv       *self,
	gint64        series_id,
	const gchar  *user,
	GError      **error
);

/* A season monitored or not, with its episodes (the specials' too). */
gboolean
axis_tv_set_season_monitored(
	AxisTv       *self,
	gint64        series_id,
	gint          season,
	gboolean      monitored,
	const gchar  *user,
	GError      **error
);

gboolean
axis_tv_set_episode_monitored(
	AxisTv       *self,
	gint64        series_id,
	gint64        episode_id,
	gboolean      monitored,
	const gchar  *user,
	GError      **error
);

/**
 * AxisTvWholeMonitoring:
 * @seasons: change the seasons too, as @monitor says; without it only
 *   @monitor_new is set, and the seasons, the episodes and the series'
 *   choice ("What to monitor") stay as they are
 * @monitor: %AXIS_TV_MONITOR_ALL (every season; the specials only with
 *   @specials, else left as they are), %AXIS_TV_MONITOR_LATEST (the latest
 *   season and nothing else, as the Add form's choice: the other seasons and
 *   the specials are not monitored) or %AXIS_TV_MONITOR_NONE (no season, the
 *   specials neither)
 * @specials: with %AXIS_TV_MONITOR_ALL, the specials too
 * @monitor_new: the seasons that come later are monitored when announced
 * @user: (nullable) who changes it
 */
typedef struct
{
	gboolean       seasons;
	AxisTvMonitor  monitor;
	gboolean       specials;
	gboolean       monitor_new;
	const gchar   *user;
} AxisTvWholeMonitoring;

/**
 * AxisTvWholeMonitored:
 * @changed: anything changed (a history line was written)
 * @seasons: the seasons whose own tick changed
 * @episodes: the episodes whose monitoring changed
 * @new_changed: whether new seasons are monitored changed
 */
typedef struct
{
	gboolean changed;
	guint    seasons;
	guint    episodes;
	gboolean new_changed;
} AxisTvWholeMonitored;

/**
 * axis_tv_set_series_monitoring:
 * @done: (out) (optional): what changed
 *
 * The monitoring of a whole series in one change: whether its new seasons
 * are monitored, and, with @change->seasons, every season of it as the
 * choice says, each with its episodes (as axis_tv_set_season_monitored()
 * changes one).  The choice is decided a season by the rule the Add form's
 * choices follow (axis_tv_monitor_takes_season()).  The series' choice
 * becomes the one made, so a series brought over ("As in Sonarr") says what
 * a person chose from then on.
 *
 * Only what is wanted changes: no file, no link between a file and an
 * episode, no grab and no download is touched, and an episode that has a
 * file keeps it whether it is monitored or not.  Nothing is searched for at
 * once: an episode monitored now has its wait cleared, and the next
 * automatic search takes it at the scheduler's ordinary pace, as after a
 * change of one season.  A hold of a series brought over is not ended.  One
 * line in the series' history, when anything changed.
 */
gboolean
axis_tv_set_series_monitoring(
	AxisTv                       *self,
	gint64                        series_id,
	const AxisTvWholeMonitoring  *change,
	AxisTvWholeMonitored         *done,
	GError                      **error
);

/**
 * axis_tv_sync:
 * @fetch: ask the provider for seasons whose episodes were never fetched
 * @changes: (out) (optional): episodes that appeared, moved or went
 *
 * The series' seasons and episodes follow the metadata store (see the top).
 */
gboolean
axis_tv_sync(
	AxisTv   *self,
	gint64    series_id,
	gboolean  fetch,
	guint    *changes,
	GError  **error
);

/* ── Changing a series' numbering (axis-tv-numbering.c) ─────────── */

/*
 * A series follows one source of numbering (AxisTvSeries.episode_source):
 * its record provider's (TMDb) or TheTVDB's aired order.  Changing it maps
 * each of its episodes to at most one episode of the other source, in this
 * order, each step taking only a pairing that is the only one on both sides
 * (a tie is left unmapped):
 *
 *   1. held: the identity the row held under that source before (rows
 *      remember each, series_episode_identity), so that changing back is
 *      exact;
 *   2. air date and title: the same air date and the same title, compared
 *      as letters and digits only, case folded;
 *   3. air date only (flagged): the only episode of that date on each side,
 *      both running times known and within half of each other;
 *   4. title only (flagged): an episode with no air date whose title is
 *      unique on both sides;
 *   5. file name (flagged): every file of the episode names, by the reading
 *      imports and identification use, the same one episode of the other
 *      source.
 *
 * A mapped row keeps its id and so its files, grabs, history, search pacing
 * and monitoring; it takes the other source's identity, number, title, air
 * date and running time.  An unmapped row is kept, marked gone, with its
 * files linked, never searched for.  An episode of the other source with no
 * counterpart is added, monitored as its season is (or left unmonitored when
 * asked).  No file is moved, renamed or deleted, and every linked file
 * stays on the row it is on (what it holds did not change, only the
 * numbering); only a file no link held is placed by its name.  The stored
 * releases of the series are cleared, and searches, grabs and links carry
 * the numbering they were made under (0061_numbering_stamps.sql).
 */

/**
 * AxisTvMapped:
 * @AXIS_TV_MAPPED_NONE: no counterpart: kept, marked gone
 * @AXIS_TV_MAPPED_HELD: the episode it was under that source before
 * @AXIS_TV_MAPPED_DATE_TITLE: the same air date and title
 * @AXIS_TV_MAPPED_DATE: by air date only (flagged)
 * @AXIS_TV_MAPPED_TITLE: by title only (flagged)
 * @AXIS_TV_MAPPED_FILE_NAME: by its file's name (flagged)
 * @AXIS_TV_MAPPED_RETURNS: a row of the other source kept from before, which
 *   comes back with its files and history
 */
typedef enum
{
	AXIS_TV_MAPPED_NONE,
	AXIS_TV_MAPPED_HELD,
	AXIS_TV_MAPPED_DATE_TITLE,
	AXIS_TV_MAPPED_DATE,
	AXIS_TV_MAPPED_TITLE,
	AXIS_TV_MAPPED_FILE_NAME,
	AXIS_TV_MAPPED_RETURNS
} AxisTvMapped;

#define AXIS_TV_N_MAPPED 7

/* "As before", "Air date and title", "Air date only", ...: how a row was
 * matched, as the confirmation page names it. */
const gchar *
axis_tv_mapped_label(AxisTvMapped how);

/* Whether @how is a pairing a person should check (date only, title only,
 * file name). */
gboolean
axis_tv_mapped_flagged(AxisTvMapped how);

/**
 * AxisTvRenumbered:
 *
 * One episode row of the series and what a change of numbering makes of it.
 * Numbers are -1 where there are none (a row kept from before has no
 * numbering now; an unmapped one none after).
 */
typedef struct
{
	gint64        episode_id;
	gint          season;
	gint          number;
	gchar        *title;		/* (nullable) */
	gchar        *air_date;		/* (nullable) */
	AxisTvMapped  how;
	gint          to_season;
	gint          to_number;
	gchar        *to_title;		/* (nullable) */
	gchar        *to_air_date;	/* (nullable) */
	gboolean      monitored;
	gboolean      grab;		/* in an open grab */
	guint         n_files;		/* its files present now */
	guint         n_files_after;	/* and after (identified files follow their names) */
	gchar        *file_path;	/* (nullable) the first of them, stored form */
	gboolean      identified;	/* that file was linked by identification */
	gint          name_season;	/* the episode that file's name gives, -1: none */
	gint          name_number;
	gboolean      wanted_before;
	gboolean      wanted_after;
} AxisTvRenumbered;

void
axis_tv_renumbered_free(AxisTvRenumbered *row);

/**
 * AxisTvRenumberedFile:
 *
 * A file of the series whose name, after the change, says another episode
 * than the one it would be on: where placing it by its name would put it, or
 * why it would not.  @earlier: its name was read under another numbering than
 * the one the series would follow, or an import by Axis made its link; the
 * tick box never places it.
 */
typedef struct
{
	gchar    *path;		/* stored form */
	gint      name_season;
	gint      name_number;
	gint      to_season;	/* the episode it would be on */
	gint      to_number;
	gboolean  earlier;
	gboolean  placed;	/* the tick box would place it */
	gchar    *by_name;	/* what placing it would do, a sentence */
} AxisTvRenumberedFile;

void
axis_tv_renumbered_file_free(AxisTvRenumberedFile *file);

/* An episode of the other source with no counterpart: added by the change. */
typedef struct
{
	gint      season;
	gint      number;
	gchar    *title;	/* (nullable) */
	gchar    *air_date;	/* (nullable) */
	gboolean  monitored;	/* as its season is */
	gboolean  aired;
	gboolean  gets_file;	/* an identified file's name places it here */
	gboolean  new_season;	/* in a season the series did not have */
} AxisTvRenumberNew;

void
axis_tv_renumber_new_free(AxisTvRenumberNew *added);

/**
 * AxisTvRenumberPlan:
 * @from: the source the series follows now; @to: the one it would follow
 * @rows: (element-type AxisTvRenumbered): every live row, by its numbering
 *   now, then the rows of @to kept from before
 * @added: (element-type AxisTvRenumberNew): the episodes it would add
 * @waits: (element-type utf8): the grabs, imports and searches of the series
 *   in flight, each named: the change waits for them
 * @problem: (nullable): why the change cannot be made now (the other
 *   source's episodes are not known), a sentence
 * @digest: what the series and the other source's episodes are now: a
 *   confirmation made from another digest is refused
 */
typedef struct
{
	gint64      series_id;
	gchar      *from;
	gchar      *to;
	GPtrArray  *rows;
	GPtrArray  *added;
	GPtrArray  *waits;
	gchar      *problem;
	gchar      *notes;	/* (nullable) what the plan cannot match, said plainly */
	gchar      *digest;
	guint       n_by[AXIS_TV_N_MAPPED];
	guint       n_unmapped_files;	/* unmapped rows that hold files */
	guint       n_new_wanted;	/* added episodes that would be wanted */
	guint       n_new_monitored;	/* added episodes that would be monitored */
	guint       n_new_in_new_seasons;	/* added in seasons the series did not have */
	guint       n_old_monitored;	/* added in seasons it has: monitored as the season is */
	guint       n_old_wanted;	/* and of them, wanted */
	guint       n_disagree;		/* files whose name and episode after disagree */
	GPtrArray  *names;		/* AxisTvRenumberedFile */
	/* The links a person made (AxisTvRenumberedFile; never among @names, and
	 * never placed by the tick box): @name_season and @name_number are the
	 * episode each is on now (-1: one kept as lacking), @to_season and
	 * @to_number the one after (-1: kept as lacking), @by_name how and by whom
	 * the link was made.  Part of @digest. */
	GPtrArray  *hand;
	guint       n_names_placed;	/* of them, those the tick box places */
	gchar      *names_left;		/* (nullable) the episodes the tick box leaves without a file, a sentence */
	guint       wanted_before;
	guint       wanted_after;
	guint       monitored_before;
	guint       monitored_after;
} AxisTvRenumberPlan;

void
axis_tv_renumber_plan_free(AxisTvRenumberPlan *plan);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvRenumberPlan, axis_tv_renumber_plan_free)

/**
 * axis_tv_numbering_choices:
 *
 * The sources @series may follow, the one it follows first: its record
 * provider's own, and TheTVDB's aired order while TheTVDB is offered (its key
 * set) and the series has a TVDB id, or while it follows it.
 *
 * Returns: (transfer full) (element-type utf8): source names
 */
GPtrArray *
axis_tv_numbering_choices(
	AxisTv             *self,
	const AxisTvSeries *series
);

/**
 * axis_tv_plan_numbering:
 * @to: one of axis_tv_numbering_choices() but the one followed
 * @fetch: ask the other source's provider first when its episodes are not
 *   fresh (they are kept in the metadata store, nothing of the series)
 * @plan: (out) (transfer full): what the change would do
 *
 * Writes nothing of the series.  Fails with INVALID for a source the series
 * cannot follow, NOT_FOUND for no such series.
 */
gboolean
axis_tv_plan_numbering(
	AxisTv               *self,
	gint64                series_id,
	const gchar          *to,
	gboolean              fetch,
	AxisTvRenumberPlan  **plan,
	GError              **error
);

/**
 * axis_tv_change_numbering:
 * @digest: the plan's, as the confirmation page showed it
 * @leave_new_unmonitored: the episodes added are not monitored
 * @plan: (out) (transfer full) (optional): the plan as it stands (when the
 *   series changed, the page to show again)
 * @changed: (out): the series or the other source's episodes changed since
 *   @digest: nothing was done
 *
 * Fetches the other source's episodes first when they are not fresh, then,
 * in one transaction under the series' lock, keys the series' rows anew as
 * the plan says, follows the other source from then on, adds what it lacks
 * and records one event in the history with @user and the counts.  Refused
 * (IN_FLIGHT, the message naming them) while a grab, an import or a search
 * of the series is open; INVALID when the plan has a problem.  A provider
 * that fails leaves the series as it was.
 */
gboolean
axis_tv_change_numbering(
	AxisTv               *self,
	gint64                series_id,
	const gchar          *to,
	const gchar          *digest,
	gboolean              leave_new_unmonitored,
	const gchar          *user,
	AxisTvRenumberPlan  **plan,
	gboolean             *changed,
	GError              **error
);

/* ── Files whose names say another episode (axis-tv-place.c) ───── */

/**
 * AxisTvMisplaced:
 *
 * A file of the series on one episode whose own name, read under the
 * numbering the series follows, says another (it was linked under another
 * numbering, or from a release named in another).  Numbers -1 where none.
 */
typedef struct
{
	gint64    file_id;
	gchar    *path;		/* stored form */
	gint64    episode_id;	/* the episode it is on */
	gint      season;
	gint      number;
	gint      name_season;	/* the episode its name says */
	gint      name_number;
	gint64    to_episode_id;	/* the series' episode of that number, 0: none */
	gchar    *refused;	/* (nullable) why it cannot be placed now, alone, a sentence */
	gboolean  leaves_wanted;	/* the episode it leaves is then wanted (monitored, aired, no other file) */
	gboolean  earlier;	/* its link was made by an Axis import, or its name was read under another numbering */
	gboolean  by_import;	/* (of earlier) an import by Axis made the link */
	/* What "Place all" makes of it, the files going one after the other
	 * (never an @earlier one): the confirmation of Place all shows these, a
	 * file's own row @refused and @leaves_wanted. */
	gboolean  all_placed;
	gchar    *all_refused;	/* (nullable) */
	gboolean  all_leaves_wanted;
} AxisTvMisplaced;

void
axis_tv_misplaced_free(AxisTvMisplaced *misplaced);

/**
 * axis_tv_list_misplaced:
 * @only: 0, or one file: then judged alone against the series as it is (what
 *   placing that file alone does), and only it is listed
 * @list: (out): the files "Place all" would act on first (those identification
 *   linked, named under the numbering the series follows), then the files
 *   whose names are probably only the old number (@earlier)
 * @left: (out) (optional): the episodes "Place all" would leave without a
 *   file, which of them are wanted, a sentence; %NULL when none
 * @digest: (out) (optional): of what "Place all" would do, as listed
 *
 * The series' files whose names say another episode than the one they are on.
 */
gboolean
axis_tv_list_misplaced(
	AxisTv      *self,
	gint64       series_id,
	gint64       only,
	GPtrArray  **list,
	gchar      **left,
	gchar      **digest,
	GError     **error
);

/**
 * axis_tv_place_by_name:
 * @file_id: one file of axis_tv_list_misplaced(), or 0 for every one that
 *   can be placed (never an @earlier one)
 * @digest: the digest the page shown had (axis_tv_list_misplaced(), with
 *   the same file); %NULL: not checked
 * @placed: (out) (optional): how many were
 * @changed: (out) (optional): what can be placed is not what the digest
 *   says: nothing was done
 *
 * Moves the file's link (never the file) to the episode its name says, in
 * one transaction under the series' lock; records it in the history of both
 * episodes; marks the link as read under the numbering the series follows.
 * Files are placed one after the other, each against what the one before
 * left: never onto an episode that holds a file then.  Refused (INVALID, the
 * reason said plainly) for one file whose episode by name has a file of its
 * own, is not in the series' numbering, or is in an open grab, as is the
 * episode it is on, and while a manual import of the series runs.
 */
gboolean
axis_tv_place_by_name(
	AxisTv       *self,
	gint64        series_id,
	gint64        file_id,
	const gchar  *digest,
	const gchar  *user,
	guint        *placed,
	gboolean     *changed,
	GError      **error
);

/* ── Links by hand (axis-tv-links.c) ───────────────────────────── */

/* The most episodes one file can be linked to by hand. */
#define AXIS_TV_LINK_EPISODES_MAX 16

/* The most files with no link one page of the links page lists. */
#define AXIS_TV_LINK_PAGE 100

/**
 * AxisTvLinkFile:
 *
 * A video file a series' links page lists: one linked to the series, or one
 * in its folders with no link (never linked, or unlinked by hand).
 */
typedef struct
{
	gint64    file_id;
	gchar    *path;			/* stored form */
	gboolean  linked;		/* to this series */
	gchar    *on;			/* (nullable) the episodes it is on: "S01E03, S01E04" */
	gboolean  on_gone;		/* one of them is an episode the numbering lacks */
	gchar    *how;			/* how its link was made, or why it has none, a sentence */
	gboolean  hand;			/* a person made the link, or keeps it */
	gboolean  kept;
	gboolean  unlinked_by_hand;
	gchar    *other;		/* (nullable) the other series it is linked to */
	gchar    *name_says;		/* (nullable) the episode its name gives */
	gint64    import_id;		/* the import that linked it (its release is known), else 0 */
	gboolean  grab_open;		/* with import_id: the grab of its download is still open */
	/* With name_says, for an anime series: the absolute number its name
	 * states, which TheTVDB's numbers gave that episode; 0: its name states a
	 * season and an episode. */
	gint      name_absolute;
} AxisTvLinkFile;

void
axis_tv_link_file_free(AxisTvLinkFile *file);

/**
 * AxisTvLinkAsk:
 * @unlink: the file's link is removed
 * @to: (nullable): the episodes it is given, as a person writes them
 *   ("S01E03", "S01E03 S01E04" for a double episode), in the numbering the
 *   series follows
 *
 * What a person chose for one file.
 */
typedef struct
{
	gint64    file_id;
	gboolean  unlink;
	gchar    *to;
} AxisTvLinkAsk;

AxisTvLinkAsk *
axis_tv_link_ask_new(
	gint64       file_id,
	gboolean     unlink,
	const gchar *to
);

void
axis_tv_link_ask_free(AxisTvLinkAsk *ask);

/* A file of the change: where it is before and after. */
typedef struct
{
	gint64    file_id;
	gchar    *path;
	gchar    *before;	/* "S01E03", "No episode" */
	gchar    *after;
	gchar    *note;		/* (nullable) its name says another episode: what a media server shows */
	gboolean  other;	/* unlinked by an "unlink the other" tick */
} AxisTvLinkChange;

/* An episode the change touches: its files before and after. */
typedef struct
{
	gint64    episode_id;
	gchar    *label;
	gchar    *before;	/* its files' paths, "; " between; "No file" */
	gchar    *after;
	gchar    *what;		/* (nullable) left without a file and whether wanted; or two files now */
} AxisTvLinkTouched;

/* A file outside the change on an episode the change gives a file: the tick
 * "unlink it instead". */
typedef struct
{
	gint64    file_id;
	gchar    *path;
	gchar    *episodes;	/* the episodes it would be unlinked from */
	gchar    *left;		/* the episodes it is on then; "No episode" */
	gboolean  ticked;
} AxisTvLinkOffer;

/**
 * AxisTvLinks:
 * @files: (element-type AxisTvLinkFile): by path
 * @doubles: (element-type utf8): each episode that has more than one file
 *   linked now, a sentence
 * @busy: (nullable): why no link can be changed now (an open import)
 * @changes, @touched, @offers: the review of a plan (empty for a reading)
 * @refused: (nullable): why the plan cannot be applied now
 * @digest: (nullable): of everything the review shows
 */
typedef struct
{
	AxisTvSeries *series;
	GPtrArray    *files;
	GPtrArray    *doubles;
	gchar        *busy;
	GPtrArray    *changes;	/* AxisTvLinkChange */
	GPtrArray    *touched;	/* AxisTvLinkTouched */
	GPtrArray    *offers;	/* AxisTvLinkOffer */
	gchar        *refused;
	gchar        *digest;
	guint         page;		/* the page of files with no link listed (from 0) */
	guint         free_total;	/* every file with no link of the series' folders */
} AxisTvLinks;

void
axis_tv_links_free(AxisTvLinks *links);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvLinks, axis_tv_links_free)

/**
 * axis_tv_links_read:
 * @also: (nullable) (element-type gint64): files to list wherever they are in
 *   the series' reach (its linked files, the files of its folders, those
 *   identification linked to it); any other is not listed
 * @page: which page (of AXIS_TV_LINK_PAGE) of the files with no link
 *
 * The series' linked video files with how each link was made, and one page of
 * the video files of its folders that have no link.  Changes nothing.
 */
gboolean
axis_tv_links_read(
	AxisTv        *self,
	gint64         series_id,
	const GArray  *also,
	guint          page,
	AxisTvLinks  **links,
	GError       **error
);

/**
 * axis_tv_links_review:
 * @asks: (element-type AxisTvLinkAsk): the plan
 * @others: (nullable) (element-type gint64): the files ticked "unlink it
 *   instead"
 *
 * What the plan would do, judged as one (a swap is two files given each
 * other's episodes).  Changes nothing.  INVALID for a plan that names a file
 * or an episode that is not the series', or nothing at all.
 */
gboolean
axis_tv_links_review(
	AxisTv           *self,
	gint64            series_id,
	const GPtrArray  *asks,
	const GArray     *others,
	AxisTvLinks     **links,
	GError          **error
);

/**
 * axis_tv_links_apply:
 * @digest: the review's
 * @links: (out) (optional): the review as it stands (when @changed or
 *   refused, the page to show again)
 * @changed: (out): what the review showed is no longer so: nothing was done
 *
 * Applies the plan in one transaction under the series' lock: only links
 * change, never a file on disk.  Each link made is stamped as made by a
 * person and kept by hand; each file left with no episode is remembered as
 * unlinked by hand, so identification does not link it again; each episode
 * touched gets a line in the history.  IN_FLIGHT (nothing done, @links set)
 * while an import of the series is open or a grab claims an episode touched.
 */
gboolean
axis_tv_links_apply(
	AxisTv           *self,
	gint64            series_id,
	const GPtrArray  *asks,
	const GArray     *others,
	const gchar      *digest,
	const gchar      *user,
	AxisTvLinks     **links,
	gboolean         *changed,
	GError          **error
);

/* A seam for tests: called inside axis_tv_links_apply() once the plan is
 * judged, before any file is locked, to make a change that identification or
 * an import may make there meanwhile.  %NULL in service. */
extern void (*axis_tv_links_apply_hook)(gpointer data);
extern gpointer axis_tv_links_apply_hook_data;

/* What "Let Axis manage this file again" does to a file. */
typedef enum
{
	/* A file a person linked or keeps, that identification places by its name:
	 * its episodes are replaced by what the name gives, in one step. */
	AXIS_TV_MANAGE_PLACE,
	/* An import's link: it stays as it is (identification never changes one);
	 * only the marks of a person go. */
	AXIS_TV_MANAGE_RELEASE,
	/* Identification gives the file no episode: its link stays as it is, still
	 * marked by hand.  Nothing can be confirmed. */
	AXIS_TV_MANAGE_NOTHING,
	/* A file unlinked by hand: the mark goes, and the next run of
	 * identification places it by its name (@after, when it does). */
	AXIS_TV_MANAGE_UNLINKED
} AxisTvManageKind;

/**
 * AxisTvLinkManage:
 * @before: the episodes the file is on now ("No episode")
 * @after: (nullable): the episodes identification gives it
 * @touched: (element-type AxisTvLinkTouched): the episodes that change, with
 *   whether each left without a file is wanted
 * @outcome: what happens, a sentence
 * @refused: (nullable): why it cannot be done now
 */
typedef struct
{
	AxisTvSeries     *series;
	gint64            file_id;
	gchar            *path;
	AxisTvManageKind  kind;
	gchar            *before;
	gchar            *after;
	GPtrArray        *touched;
	gchar            *outcome;
	gchar            *refused;
	gchar            *digest;
} AxisTvLinkManage;

void
axis_tv_link_manage_free(AxisTvLinkManage *manage);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvLinkManage, axis_tv_link_manage_free)

/**
 * axis_tv_links_manage_review:
 *
 * What "Let Axis manage this file again" would do to the file, before it
 * does it.  Changes nothing.  NOT_FOUND when the file is not one a person
 * linked, keeps or unlinked, or is not one the series' links page reaches.
 */
gboolean
axis_tv_links_manage_review(
	AxisTv             *self,
	gint64              series_id,
	gint64              file_id,
	AxisTvLinkManage  **manage,
	GError            **error
);

/**
 * axis_tv_links_manage:
 * @digest: the review's
 * @manage: (out) (optional): the review as it stands (when @changed or
 *   refused)
 * @changed: (out): what the review showed is no longer so: nothing was done
 *
 * "Let Axis manage this file again": the file's link is no longer a
 * person's and no longer kept by hand, and a file unlinked by hand is no
 * longer remembered as such (the mark is the file's, whichever series it was
 * unlinked from).  A file linked or kept by a person is handed back to
 * identification, which places it by its name: its episodes are replaced by
 * what the name gives, never added to, in one step.  When the name gives
 * nothing, the link stays as it is, still marked by hand (IN_FLIGHT, with
 * @manage set, as for an import open or a grab in flight).
 */
gboolean
axis_tv_links_manage(
	AxisTv             *self,
	gint64              series_id,
	gint64              file_id,
	const gchar        *digest,
	const gchar        *user,
	AxisTvLinkManage  **manage,
	gboolean           *changed,
	GError            **error
);

/* As axis_tv_change_numbering(); with @place_by_names, in the same
 * transaction, after the change, every file identification linked whose name
 * was read under the new numbering and says another episode is placed by it
 * where it can be (AxisTvRenumberPlan.names says which).  A file an import by
 * Axis linked, or whose name was read under another numbering, never is. */
gboolean
axis_tv_change_numbering_full(
	AxisTv               *self,
	gint64                series_id,
	const gchar          *to,
	const gchar          *digest,
	gboolean              leave_new_unmonitored,
	gboolean              place_by_names,
	const gchar          *user,
	AxisTvRenumberPlan  **plan,
	gboolean             *changed,
	GError              **error
);

/**
 * axis_tv_numbering_not_refreshed:
 *
 * Why the numbering @series follows is not refreshed now (its provider has
 * no key, refused it, or does not answer), a sentence; %NULL when it is.
 */
gchar *
axis_tv_numbering_not_refreshed(
	AxisTv             *self,
	const AxisTvSeries *series
);

/**
 * AxisTvScope:
 * @AXIS_TV_SCOPE_EPISODE: one episode (@item: its id)
 * @AXIS_TV_SCOPE_SEASON: a season (@item: its number): a whole-season release
 *   first, then each wanted episode
 * @AXIS_TV_SCOPE_SERIES: every wanted episode: a pack of several seasons
 *   first (or of the whole series), then each season
 */
typedef enum
{
	AXIS_TV_SCOPE_EPISODE,
	AXIS_TV_SCOPE_SEASON,
	AXIS_TV_SCOPE_SERIES
} AxisTvScope;

/* "episode", "season", "series". */
const gchar *
axis_tv_scope_name(AxisTvScope scope);

gboolean
axis_tv_scope_from_name(
	const gchar *name,
	AxisTvScope *scope
);

/**
 * axis_tv_request_search:
 * @cause: "manual", "added", "scheduled", "retry"
 * @created: (out) (optional): %FALSE when that search was queued already
 *
 * Queues a search.  "manual" of the whole series queues what the scheduler
 * would for every wanted episode (seasons first, by the pack threshold).
 */
gboolean
axis_tv_request_search(
	AxisTv       *self,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *cause,
	const gchar  *user,
	gboolean     *created,
	GError      **error
);

/* The search job's work for one target (axis_acquire_run_search()); a
 * season or series search that took nothing queues the narrower ones. */
gboolean
axis_tv_run_search(
	AxisTv                   *self,
	gint64                    series_id,
	AxisTvScope               scope,
	gint64                    item,
	const gchar              *cause,
	const gchar              *user,
	GCancellable             *cancellable,
	AxisAcquireSearchReport  *report,
	GError                  **error
);

/* An administrator's grab of a listed release of one of the series'
 * searches. */
gboolean
axis_tv_grab(
	AxisTv       *self,
	gint64        series_id,
	gint64        candidate_id,
	const gchar  *user,
	gint64       *grab_id,
	GError      **error
);

/**
 * axis_tv_request_manual_search:
 * @scope, @item: the series (item 0), a season (its number) or an episode
 *   (its id)
 * @created: (out) (optional): %FALSE when one of that level was queued or
 *   running already
 *
 * Queues a manual search (cause "list": it lists and grabs nothing), with a
 * job key of its own, whatever is wanted now; the history says who asked.
 * NOT_FOUND for no such series, season or episode.
 */
gboolean
axis_tv_request_manual_search(
	AxisTv       *self,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	const gchar  *user,
	gboolean     *created,
	GError      **error
);

/**
 * AxisTvAsked:
 * @AXIS_TV_ASKED_QUEUED: searches were queued
 * @AXIS_TV_ASKED_IN_HAND: nothing new: a search that does the work (of the
 *   same thing, or of a wider scope holding it) is queued or running
 * @AXIS_TV_ASKED_RECENT: nothing new: such a search ended less than
 *   %AXIS_ACQUIRE_SEARCH_AGAIN_S ago
 * @AXIS_TV_ASKED_NOTHING: nothing of it is wanted (Search all wanted)
 *
 * What a person's click on a search came to.
 */
typedef enum
{
	AXIS_TV_ASKED_QUEUED,
	AXIS_TV_ASKED_IN_HAND,
	AXIS_TV_ASKED_RECENT,
	AXIS_TV_ASKED_NOTHING
} AxisTvAsked;

/**
 * axis_tv_ask_search:
 * @scope, @item: an episode (its id), a season (its number) or the series
 *   (Search all wanted: what the scheduler would queue now)
 * @list: a manual search (it lists and grabs nothing) rather than an
 *   automatic one; the two are separate families, neither holding the other
 * @asked: (out): what came of it
 * @queued: (out) (optional): searches queued
 *
 * A person's search, made unless that very search (the same key: scope,
 * item and family) is queued or running, which answers
 * %AXIS_TV_ASKED_IN_HAND (a queued one at the scheduler's priority raised to
 * a person's), or ended less than %AXIS_ACQUIRE_SEARCH_AGAIN_S ago, which
 * answers %AXIS_TV_ASKED_RECENT.  A wider search in hand holds nothing back
 * (a season's search takes whole-season packs only).  Search all wanted plans
 * as the scheduler does, leaves out each episode searched a moment ago and
 * raises the series' queued searches.  The history says what was queued,
 * and nothing when nothing was.  NOT_FOUND for no such series, season or
 * episode.
 */
gboolean
axis_tv_ask_search(
	AxisTv       *self,
	gint64        series_id,
	AxisTvScope   scope,
	gint64        item,
	gboolean      list,
	const gchar  *user,
	AxisTvAsked  *asked,
	guint        *queued,
	GError      **error
);

/* As axis_tv_request_undated(), by the rules of axis_tv_ask_search(): each
 * episode whose own search is in hand or was searched a moment ago is left
 * out (the season's and the series' searches never take one). */
gboolean
axis_tv_ask_undated(
	AxisTv       *self,
	gint64        series_id,
	gint          season,
	const gchar  *user,
	AxisTvAsked  *asked,
	guint        *queued,
	GError      **error
);

/* The key of the search of @scope/@item (@list: the manual one); free with
 * g_free(). */
gchar *
axis_tv_ask_key(
	gint64       series_id,
	AxisTvScope  scope,
	gint64       item,
	gboolean     list
);

/**
 * AxisTvEnded:
 * @key: its job's key
 * @list: a manual search
 * @finished_us: when it ended
 * @outcome: (nullable): how ("grabbed", "nothing_found", ...)
 * @outcome_text: (nullable): what it said
 * @n_found, @n_accepted: releases found, and accepted by the profile
 *
 * A search that ended a while ago, for the line of what it found.
 */
typedef struct
{
	gchar    *key;
	gboolean  list;
	gint64    finished_us;
	gchar    *outcome;
	gchar    *outcome_text;
	guint     n_found;
	guint     n_accepted;
} AxisTvEnded;

/**
 * AxisTvWork:
 * @live: (element-type AxisJobLive): the series' searches queued or running
 * @ended: (element-type AxisTvEnded): of each key, the latest search that
 *   ended within the time asked for, the latest first
 * @now_us: the database's clock when they were read
 *
 * What is in hand for a series, and what ended a while ago: two statements,
 * whatever the number of its episodes.
 */
typedef struct
{
	GPtrArray *live;
	GPtrArray *ended;
	gint64     now_us;
} AxisTvWork;

void
axis_tv_work_free(AxisTvWork *work);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvWork, axis_tv_work_free)

gboolean
axis_tv_get_work(
	AxisTv       *self,
	gint64        series_id,
	guint         ended_within_s,
	AxisTvWork  **work,
	GError      **error
);

/* The search of @scope/@item in hand, or %NULL.  Transfer none. */
const AxisJobLive *
axis_tv_work_in_hand(
	const AxisTvWork *work,
	gint64            series_id,
	AxisTvScope       scope,
	gint64            item,
	gboolean          list
);

/* The latest search of @scope/@item that ended, or %NULL. */
const AxisTvEnded *
axis_tv_work_ended(
	const AxisTvWork *work,
	gint64            series_id,
	AxisTvScope       scope,
	gint64            item,
	gboolean          list
);

/* What a key is for: the scope and item of "tv.search:12:season:1" (and
 * whether it is a manual search's); %FALSE for a key of no series search. */
gboolean
axis_tv_key_parse(
	const gchar  *key,
	gint64       *series_id,
	AxisTvScope  *scope,
	gint64       *item,
	gboolean     *list
);

/* The last manual search of a level with its releases (@releases->search
 * %NULL for none yet), and whether one is queued or running. */
gboolean
axis_tv_get_manual(
	AxisTv           *self,
	gint64            series_id,
	AxisTvScope       scope,
	gint64            item,
	AxisTvReleases  **releases,
	gboolean         *running,
	GError          **error
);

/* What a grab by hand of a stored release would do (NOT_FOUND: not one of
 * the series' searches'). */
gboolean
axis_tv_hand_preview(
	AxisTv                   *self,
	gint64                    series_id,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
);

/* Grab this one (axis_acquire_grab_by_hand()); @grabbed, @result: (out)
 * (optional) what was recorded and what came of it, the error in the
 * series' words. */
gboolean
axis_tv_grab_by_hand(
	AxisTv                      *self,
	gint64                       series_id,
	gint64                       candidate_id,
	const AxisAcquireHandTicks  *ticks,
	const gchar                 *user,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result,
	GError                     **error
);

/**
 * AxisTvForcePick:
 * @named: the person named what the release is for (the confirmation's
 *   picker, or its post); unset, the preview proposes what the release's name
 *   and the search suggest
 * @episodes: (element-type gint64) (nullable): the episodes named
 * @whole_season, @season: a whole season named instead
 *
 * What a person binds a grab by force to (add-force-grab-and-assign, F2).
 */
typedef struct
{
	gboolean      named;
	const GArray *episodes;
	gboolean      whole_season;
	gint          season;
} AxisTvForcePick;

/* What a grab by force of a stored release would do, bound to @pick
 * (axis_acquire_force_preview(); NOT_FOUND as above).  The preview's choices
 * are the episodes and seasons of the level searched; unnamed when nothing
 * is picked.  Refused while an import of the series is open, when an episode
 * picked is in flight, and when the release was found under another
 * numbering than the series follows now. */
gboolean
axis_tv_force_preview(
	AxisTv                   *self,
	gint64                    series_id,
	gint64                    candidate_id,
	const AxisTvForcePick    *pick,
	AxisAcquireHandPreview  **preview,
	GError                  **error
);

/* Grab it by force (axis_acquire_grab_by_force()): the grab claims exactly
 * the episodes of @pick (every episode of the season, for a season, which is
 * recorded as bound_season) and is bound to them.  *@result: CHANGED when
 * what the confirmation showed is not so now. */
gboolean
axis_tv_grab_by_force(
	AxisTv                       *self,
	gint64                        series_id,
	gint64                        candidate_id,
	const AxisTvForcePick        *pick,
	const AxisAcquireForceTicks  *ticks,
	const gchar                  *user,
	AxisAcquireHandGrabbed       *grabbed,
	AxisAcquireGrabResult        *result,
	GError                      **error
);

/* Sets or clears "kept by hand" on one file's link (@kept: no automatic
 * search and no Search now replaces it), and says so in the history of the
 * episodes it holds.  NOT_FOUND when the file is not the series'. */
gboolean
axis_tv_set_kept(
	AxisTv       *self,
	gint64        series_id,
	gint64        file_id,
	gboolean      kept,
	const gchar  *user,
	GError      **error
);

/* Gives up on an open grab (it is dealt with as a failure). */
gboolean
axis_tv_give_up(
	AxisTv       *self,
	gint64        series_id,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
);

/* Block this release (add-block-release): a release one of the series'
 * searches found is put on its blocklist by @user
 * (axis_acquire_block_candidate()).  Nothing else changes. */
gboolean
axis_tv_block_candidate(
	AxisTv       *self,
	gint64        series_id,
	gint64        candidate_id,
	const gchar  *user,
	GError      **error
);

/* The grab Block and search again is about, by its id, the download it sent
 * or an import of that download (the first that is not 0), with its series;
 * %AXIS_TV_ERROR_NOT_FOUND when there is none.  Read only. */
gboolean
axis_tv_block_find(
	AxisTv                 *self,
	gint64                  grab_id,
	gint64                  download_id,
	gint64                  import_id,
	AxisAcquireBlockGrab  **grab,
	AxisTvSeries          **series,
	GError                **error
);

/* Block and search again (axis_acquire_block_grab()): an open grab is given
 * up on as axis_tv_give_up() does, with the release blocked by @user
 * (*@cancelled); a closed one is only put on the blocklist, and no file or
 * link is touched. */
gboolean
axis_tv_block_grab(
	AxisTv       *self,
	gint64        series_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
);

/* Removes one blocklist entry (@entry_id), or all of the series' (0). */
gboolean
axis_tv_clear_blocklist(
	AxisTv       *self,
	gint64        series_id,
	gint64        entry_id,
	const gchar  *user,
	guint        *removed,
	GError      **error
);

/* ── The follow-up and the scheduler ────────────────────────────── */

gboolean
axis_tv_run_follow(
	AxisTv                   *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
);

/* The follow-up's prune of old searches, now and not at its hour
 * (axis_acquire_prune_searches()): one time's batches.  For the tests. */
gboolean
axis_tv_prune_searches(
	AxisTv  *self,
	guint   *deleted,
	GError **error
);

/**
 * AxisTvScheduleReport:
 * @synced: series whose episodes followed the metadata
 * @considered: wanted episodes looked at: those of the series that have an
 *   episode that can be due (the others' are not read)
 * @due: of those, episodes that may be searched now (every one there is)
 * @queued: searches queued (episodes and seasons)
 * @seasons: of those, season searches
 * @series_read: series whose episodes were read
 */
typedef struct
{
	guint synced;
	guint considered;
	guint due;
	guint queued;
	guint seasons;
	guint series_read;
} AxisTvScheduleReport;

gboolean
axis_tv_run_schedule(
	AxisTv                *self,
	AxisTvScheduleReport  *report,
	GError               **error
);

/* How many series the scheduler reads at a time (AXIS_TV_SCHEDULE_PAGE; 0:
 * that).  For the tests, which have few series. */
void
axis_tv_set_schedule_page(
	AxisTv *self,
	guint   series
);

/* ── Series on disk that Axis does not manage ───────────────────── */

/**
 * AxisTvOnDisk:
 *
 * A series on disk: a title of kind series that identification has linked
 * (in a root for TV) to a folder holding videos that are present, and that
 * no series row manages.  Read at query time from identification's links
 * and the file index; nothing is written for it, so nothing is searched
 * for, grabbed, renamed or moved for it, and the scheduler never sees it.
 * One per series, whatever the number of its folders and disks.
 *
 * @found_us: when identification first linked one of its folders
 * @n_folders: its folders (on several roots or disks: each a copy)
 * @n_files, @size: its videos present, and their size
 * @n_away: of those, on a disk that is offline (frozen as present: a disk
 *   away never means a series is gone); @away_volume: one such disk
 * @root_id, @root_name: its first folder's root
 *
 * Read for a page's series alone (axis_tv_store_on_disk_counts()), when
 * @counted: @episodes the regular episodes of the series' data that have
 * aired or are on disk, @on_disk of those on disk, @specials the specials
 * on disk, @copies the episodes with more than one file, @unplaced the
 * files that hold no episode of the series' data (with their reasons on
 * its page), @seasons_unknown the seasons whose episodes are not fetched yet.
 */
typedef struct
{
	gint64          title_id;
	gchar          *title;
	gchar          *original_title;
	gint            year;
	AxisMetaStatus  status;
	gchar          *poster_ref;
	gchar          *provider;
	gint64          found_us;
	guint           n_folders;
	guint           n_files;
	gint64          size;
	guint           n_away;
	gchar          *away_volume;
	gint64          root_id;
	gchar          *root_name;
	gboolean        counted;
	guint           episodes;
	guint           on_disk;
	guint           specials;
	guint           copies;
	guint           unplaced;
	guint           seasons_unknown;
} AxisTvOnDisk;

void
axis_tv_on_disk_free(AxisTvOnDisk *disk);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvOnDisk, axis_tv_on_disk_free)

/* "47 of 62 episodes", "No episode known yet". */
gchar *
axis_tv_on_disk_episodes_text(const AxisTvOnDisk *disk);

/**
 * AxisTvDiskFile:
 *
 * A video of a series' folder on disk: where it is, whether its disk is
 * away, its size and quality as identification read it from its name and
 * its probe.  @reason: (nullable) for a file that holds no episode of the
 * series' data, why (a sentence).
 */
typedef struct
{
	gint64   file_id;
	gint64   identity_id;
	gchar   *rel_path;	/* stored form, relative to its root */
	gchar   *root_name;
	gchar   *volume_name;
	gboolean away;
	gint64   size;
	gchar   *quality;	/* ladder name, or NULL */
	gchar   *resolution;	/* with no quality, or NULL */
	gchar   *quality_text;
	gchar   *holds;	/* what its name holds, in words ("S01E02-E03") */
	gchar   *reason;
} AxisTvDiskFile;

void
axis_tv_disk_file_free(AxisTvDiskFile *file);

/* "BluRay 1080p", "an unknown source at 1080p", "Unknown". */
const gchar *
axis_tv_disk_file_quality(const AxisTvDiskFile *file);

/**
 * AxisTvDiskEpisode:
 *
 * An episode of the series' data with the files on disk that hold it
 * (@files, borrowed from the detail's files: none, one, or copies).
 */
typedef struct
{
	gint       season;
	gint       number;
	gchar     *title;
	gchar     *air_date;	/* "YYYY-MM-DD" or %NULL */
	gint       runtime;
	GPtrArray *files;
} AxisTvDiskEpisode;

void
axis_tv_disk_episode_free(AxisTvDiskEpisode *episode);

/**
 * AxisTvDiskFolder:
 *
 * A folder identification linked to the series: its root, its volume (and
 * whether that is away), how it was linked, how many of its videos are
 * present.
 */
typedef struct
{
	gint64   identity_id;
	gchar   *root_name;
	gchar   *rel_path;
	gchar   *volume_name;
	gboolean away;
	gchar   *linked_how;
	guint    n_files;
} AxisTvDiskFolder;

void
axis_tv_disk_folder_free(AxisTvDiskFolder *folder);

/**
 * AxisTvDiskDetail:
 * @facts: (nullable): the series on disk, %NULL when it is managed (or not
 *   on disk)
 * @episodes: (element-type AxisTvDiskEpisode): every episode of the
 *   series' data, by season and number
 * @files: (element-type AxisTvDiskFile): its videos present, by path
 * @unplaced: (element-type AxisTvDiskFile): of @files, those that hold no
 *   episode of the series' data (borrowed), each with its reason
 * @folders: (element-type AxisTvDiskFolder)
 * @seasons_unknown: (element-type gint): seasons whose episodes are not
 *   fetched yet
 */
typedef struct
{
	AxisTvOnDisk *facts;
	GPtrArray    *episodes;
	GPtrArray    *files;
	GPtrArray    *unplaced;
	GPtrArray    *folders;
	GArray       *seasons_unknown;
} AxisTvDiskDetail;

void
axis_tv_disk_detail_free(AxisTvDiskDetail *detail);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisTvDiskDetail, axis_tv_disk_detail_free)

/**
 * axis_tv_get_disk_detail:
 *
 * What identification found on disk of the series @title_id, its files put
 * against its episodes as the metadata has them now, in a few statements
 * whatever their number; for a managed series too (then @facts is %NULL),
 * whose pages show the files that could not be placed and the folders.
 * Writes nothing.
 */
gboolean
axis_tv_get_disk_detail(
	AxisTv             *self,
	gint64              title_id,
	AxisTvDiskDetail  **detail,
	GError            **error
);

/* The series on disk among @title_ids (gint64), counted (AxisTvOnDisk). */
gboolean
axis_tv_get_on_disk_by_titles(
	AxisTv      *self,
	GArray      *title_ids,
	GPtrArray  **series,
	GError     **error
);

/* ── Identification ─────────────────────────────────────────────── */

/* Registers the series as the owner of what identification finds in the
 * roots for TV: a managed series' links to the episode files identified as
 * it (series_file.identity_id), made when a folder is linked or the series
 * is added, and when its episodes follow the metadata; dropped when the
 * link is undone.  An import's link of a file wins.  Main thread. */
void
axis_tv_set_identify(
	AxisTv       *self,
	AxisIdentify *identify
);

/* ── Brought over from another app (axis-tv-brought.c) ──────────── */

/*
 * import-from-arr, slice S2.  A series brought over from Sonarr is added by
 * the code a person's Add uses, under the numbering Sonarr's seasons, episodes
 * and monitoring are in (TheTVDB's aired order) from the start; its monitoring
 * is copied season by season and episode by episode; its episode files are
 * linked where Axis's own index holds them; and its searches are held until a
 * person starts them.  Nothing here opens, moves, renames or deletes a file:
 * only rows change.
 *
 * The decisions on the proposal's questions that concern a series are each in one
 * place here: a brought file is kept (AXIS_TV_BROUGHT_KEPT), a
 * brought series is held (axis_tv_bring() sets the hold, axis_tv_held() is
 * the one test of what a hold stops), and an episode whose file Axis cannot
 * see is held apart (AXIS_TV_HOLD_FILE_UNSEEN, set there too).
 */

/* series.search_hold. */
#define AXIS_TV_HOLD_BROUGHT      "brought"
#define AXIS_TV_HOLD_DOWNLOADING  "arr_downloading"
/* series_episode.search_hold. */
#define AXIS_TV_HOLD_FILE_UNSEEN  "file_unseen"

/* Whether a file linked by bringing over is kept (never upgraded or replaced
 * by Axis itself).  Decided on 2026-11-21 (the proposal's question 2):
 * yes, such files are not upgraded automatically.  Such a link has kept
 * true and kept_how 'brought' (0073_arr_brought_kept.sql), with hand_how
 * 'brought' as before; a person lets one file go on the series' page, or all
 * of a connection's on its page (axis_tv_release_brought()). */
#define AXIS_TV_BROUGHT_KEPT      TRUE
/* series_file.kept_how of such a link: kept because it was brought over, by
 * no person's choice. */
#define AXIS_TV_KEPT_HOW_BROUGHT  "brought"

/*
 * How an episode the other app lists is matched to an episode of the series
 * (SQL, a condition over a series_episode row @e of the series row @s): by
 * its season and episode number in the numbering the series follows, among
 * the episodes that numbering has now; and where the app gives TheTVDB's id of
 * the episode (@tvdb, a bigint expression; NULL or 0 for none) and Axis holds
 * the episode's own id, the two must agree.  Anything else is no match:
 * nothing is guessed from a title, a date or a file's name.  The one rule,
 * used by axis_tv_bring() and by the bring-over preview.
 */
#define AXIS_TV_BROUGHT_EPISODE_MATCH(e, s, season, number, tvdb) \
	"(" e ".series_id = " s ".id AND " e ".gone_at IS NULL AND " e ".source = " s ".episode_source" \
	" AND " e ".season_number = " season " AND " e ".number = " number \
	" AND (" tvdb " IS NULL OR " tvdb " <= 0 OR " e ".provider_id IS NULL OR " e ".provider_id = (" tvdb ")::text))"

/*
 * Whether the series_episode row @e is held apart now (SQL): its hold stands
 * only while the episode has no present file that is kept or was linked by a
 * person or an import.  An episode that got such a file is not held, for
 * every reader, from that moment; one that only identification linked, and
 * nobody kept, stays held (Axis would upgrade the other app's file by itself)
 * until a later run of the bringing over keeps the file or a person lets it
 * go.  The scheduler's next run takes the mark
 * off its row (axis_tv_run_schedule()).  The episode facts read the same
 * (axis_tv_store_episodes() gives no hold for an episode with a file).
 */
#define AXIS_TV_EPISODE_HELD_SQL(e) \
	"(" e ".search_hold IS NOT NULL AND NOT EXISTS (SELECT 1 FROM series_file_episode held_sfe JOIN library_file held_lf" \
	" ON held_lf.id = held_sfe.file_id AND held_lf.state = 'present'" \
	" JOIN series_file held_sf ON held_sf.file_id = held_sfe.file_id" \
	" WHERE held_sfe.episode_id = " e ".id AND (held_sf.kept OR held_sf.hand_at IS NOT NULL OR held_sf.import_id IS NOT NULL)))"

/* Whether @hold (a series' or an episode's search_hold, nullable) stops
 * automatic searches. */
gboolean
axis_tv_hold_stops(const gchar *hold);

/* Whether a search with @cause is a person's own: "manual" (Search now) or
 * "list" (a manual search, which lists and grabs nothing by itself). */
gboolean
axis_tv_cause_is_person(const gchar *cause);

/**
 * axis_tv_held:
 * @series: the series
 * @episode: (nullable): one of its episodes
 *
 * THE gate of a brought series' searches: whether no automatic search may be
 * queued or run for @series (and, with @episode, for that episode).  Every
 * way a search starts passes it: the queueing of any search job
 * (axis_tv_store_enqueue_search()), what the scheduler and every plan count
 * as wanted (axis-tv-follow.c), and the search job itself when it runs
 * (load() in axis-tv-search.c).
 *
 * A person's own search (axis_tv_cause_is_person()) passes the series' hold
 * by its cause, at each of those places, and ends nothing of it: the series
 * stays held, and the scheduler stays quiet.  It never takes an episode held
 * apart, except the one episode a person's Search now names, whose own hold
 * that ends (axis_tv_store_end_hold()).  Only Search now on the whole series,
 * "Let Axis search for it" and "Start searching" end a series' hold.
 */
gboolean
axis_tv_held(
	const AxisTvSeries  *series,
	const AxisTvEpisode *episode
);

/* What a held series' page says (%NULL for no hold), and a held episode's. */
const gchar *
axis_tv_hold_sentence(const gchar *hold);

/* The word of its mark in a list: "Held". */
const gchar *
axis_tv_hold_short(const gchar *hold);

/* A season as the other app monitors it. */
typedef struct
{
	gint      number;
	gboolean  monitored;
} AxisTvBringSeason;

/**
 * AxisTvBringEpisode:
 * @season, @number: its place in TheTVDB's aired order, as the app has it
 * @tvdb_id: TheTVDB's id of the episode as the app gives it, 0 for none
 * @monitored: whether the app monitors it
 * @file_ref: the app's own number of the file that holds it, 0 for none
 * @odd: only its place could be read: it is no episode to match or link; the
 *   episode at its place is left unmonitored and held as if the app had a
 *   file for it
 * @absolute: the app's own absolute number of it, 0 for none.  Only compared
 *   with TheTVDB's, for a series brought as anime, and reported
 *   (AxisTvAbsoluteCompare): no episode is matched, monitored, linked or held
 *   by it
 */
typedef struct
{
	gint      season;
	gint      number;
	gint64    tvdb_id;
	gboolean  monitored;
	gint64    file_ref;
	gboolean  odd;
	gint      absolute;
} AxisTvBringEpisode;

/**
 * AxisTvBringFile:
 * @file_ref: the app's own number of the file (one number on several
 *   episodes is a file that holds them all)
 * @file_id: its row of Axis's index (library_file.id) at the mapped path, of
 *   the size the app recorded; 0 when Axis does not hold it so
 * @other_id: with @file_id 0: its row of the index when Axis holds a file at
 *   the mapped path of another size than the app recorded, else 0.  Never
 *   linked: only looked at, to say when it is linked on other episodes
 * @quality: (nullable): its quality, a name of Axis's ladder
 * @release: (nullable): the release it came from, as the app recorded
 */
typedef struct
{
	gint64       file_ref;
	gint64       file_id;
	gint64       other_id;
	const gchar *quality;
	const gchar *release;
} AxisTvBringFile;

/**
 * AxisTvBring:
 * @spec: the series, as for axis_tv_add(); its brought_from names the app and
 *   the connection, its source is the numbering to follow, and its monitor,
 *   specials and search_now are ignored
 * @monitored: the app monitors the series; when it does not, the series
 *   comes with every season and episode unmonitored and nothing monitored
 *   later
 * @monitor_new: the app monitors seasons that come later
 */
typedef struct
{
	AxisTvSpec                 spec;
	gboolean                   monitored;
	gboolean                   monitor_new;
	const AxisTvBringSeason   *seasons;
	guint                      n_seasons;
	const AxisTvBringEpisode  *episodes;
	guint                      n_episodes;
	const AxisTvBringFile     *files;
	guint                      n_files;
} AxisTvBring;

/**
 * AxisTvBrought:
 * @series_id: the series
 * @n_linked: files linked now, marked as brought over
 * @n_already: files linked to this series already, on the episodes the app
 *   says: left as they are
 * @n_differs: files linked to this series on other episodes than the app
 *   says, and left there: a person's or an import's link; or a link by the
 *   file's name that cannot be moved safely, because an episode the app has
 *   the file on is not in Axis's numbering, or the file is of another size
 *   than the app recorded.  Not counted in @n_already
 * @n_moved: files identification had linked by their names to other episodes
 *   than the app has them on: moved to the app's episodes and marked as
 *   brought over
 * @other_numbering: a later run only: the series no longer follows the
 *   numbering the app's places are in, and nothing was linked
 * @n_elsewhere: files linked to another series: left there
 * @n_unlinked: files a person unlinked by hand: left so
 * @n_gone: files the index does not hold as present videos (any more)
 * @n_unmatched_files: files the app has on an episode Axis's numbering does
 *   not have (or on none): not linked
 * @n_unmatched_episodes: episodes the app has that Axis's numbering does not
 * @n_held_episodes: episodes held because the app has a file for them and
 *   Axis has none on them
 * @hold: (nullable) (static): the hold the series has afterwards
 * @compared: the series was brought as anime, and the app's absolute numbers
 *   were compared with TheTVDB's (what came of it is in
 *   series.brought_absolute and in the series' history)
 * @n_absolute_agree, @n_absolute_differ, @n_absolute_one_side: what came of
 *   it, as AxisTvAbsoluteCompare counts
 */
typedef struct
{
	gboolean      compared;
	guint         n_absolute_agree;
	guint         n_absolute_differ;
	guint         n_absolute_one_side;
	gint64        series_id;
	guint         n_linked;
	guint         n_already;
	guint         n_differs;
	guint         n_moved;
	gboolean      other_numbering;
	guint         n_elsewhere;
	guint         n_unlinked;
	guint         n_gone;
	guint         n_unmatched_files;
	guint         n_unmatched_episodes;
	guint         n_held_episodes;
	const gchar  *hold;
} AxisTvBrought;

/**
 * axis_tv_bring_prepare:
 *
 * Before the transaction of axis_tv_bring(), as axis_tv_add() does before
 * its own: the series' record and the episodes of @source, fetched when they
 * are not fresh (the provider may be slow; nothing of the series is written).
 */
void
axis_tv_bring_prepare(
	AxisTv      *self,
	gint64       title_id,
	const gchar *source
);

/**
 * axis_tv_bring:
 * @conn: a connection with a transaction open (the caller's: what it records
 *   of the series commits with it)
 *
 * Adds the series through the code a person's Add uses (so its title, profile
 * and library folder are checked as there, its seasons and episodes are made
 * as the metadata store has them under @bring's numbering, and the files
 * identification holds for the title are linked at once); copies the app's
 * monitoring onto each season and onto each episode at a place the app lists
 * (by place alone: an episode of Axis's whose TheTVDB id is not the app's at
 * that place is monitored as the app monitors that place all the same; a
 * place the app lists twice is monitored when both are; a place whose entry
 * could not be read is not; an episode of Axis's the app does not list takes
 * its season's, a season the app does not list is monitored as seasons that
 * come later are); links each file to every episode the app says it holds,
 * when every one of them matched (AXIS_TV_BROUGHT_EPISODE_MATCH, the strict
 * rule: place and id), the file is linked to nothing and no person unlinked
 * it; moves a file identification linked by its name to the episodes the app
 * has it on; holds the series (%AXIS_TV_HOLD_BROUGHT) and each episode at a
 * place the app has a file for (or could not be read at) that has no file in
 * Axis (%AXIS_TV_HOLD_FILE_UNSEEN; by place alone too, so that an episode
 * whose id disagrees is not fetched a second time).  History lines are
 * written.  Fails with
 * %AXIS_TV_ERROR_EXISTS for a title managed already (nothing is changed then)
 * and %AXIS_TV_ERROR_INVALID as axis_tv_add().
 */
gboolean
axis_tv_bring(
	AxisTv             *self,
	AxisDbConn         *conn,
	const AxisTvBring  *bring,
	AxisTvBrought      *brought,
	GError            **error
);

/**
 * axis_tv_bring_files:
 * @series_id: a series brought before
 * @source: the numbering the app's places are in (the one the series was
 *   brought under)
 * @origin: the app and the connection, for the history line
 *
 * A later run: links each of @files that is linked to nothing and that no
 * person unlinked, to the episodes the app says it holds (all matched, as
 * axis_tv_bring()), and lets go of the hold of each episode that has a file
 * now.  The series' monitoring, profile, library folder, numbering, its own
 * hold and its other links are not read or changed.  When the series follows
 * another numbering than @source by now (a person changed it), the app's
 * places are not its rows' places: nothing is linked, and @brought says so
 * (other_numbering).  In the caller's transaction, with the series locked
 * here.  NOT_FOUND when it is gone.
 */
gboolean
axis_tv_bring_files(
	AxisTv                    *self,
	AxisDbConn                *conn,
	gint64                     series_id,
	const gchar               *source,
	const AxisTvBringEpisode  *episodes,
	guint                      n_episodes,
	const AxisTvBringFile     *files,
	guint                      n_files,
	const gchar               *origin,
	const gchar               *user,
	AxisTvBrought             *brought,
	GError                   **error
);

/* After series were brought (their transactions committed): the metadata
 * layer is told which sources of numbering the managed series follow now, so
 * that it keeps them fresh.  One statement. */
void
axis_tv_brought_refresh(AxisTv *self);

/**
 * axis_tv_start_searching:
 * @unseen_too: also end the holds of its episodes the app has files for that
 *   Axis does not see (a second copy of each may be downloaded): only with a
 *   person's tick
 * @was_held: (out) (optional): the series or one of its episodes was held
 *
 * "Let Axis search for it": ends the series' hold and, with @unseen_too, its
 * episodes' holds, and says so in its history.  Nothing is searched at once:
 * the scheduler takes it at its ordinary pace.  NOT_FOUND for no such series.
 */
gboolean
axis_tv_start_searching(
	AxisTv       *self,
	gint64        series_id,
	gboolean      unseen_too,
	const gchar  *user,
	gboolean     *was_held,
	GError      **error
);

/**
 * axis_tv_start_brought:
 * @conn: a connection with a transaction open
 * @start: (element-type gint64): series whose hold ends
 * @downloading: (element-type gint64): series the other app is downloading
 *   for: they are held as %AXIS_TV_HOLD_DOWNLOADING
 * @started: (out): how many holds ended
 *
 * "Start searching for the series brought over".  Only a series' own hold is
 * touched: an episode whose file Axis cannot see stays held.
 */
gboolean
axis_tv_start_brought(
	AxisTv        *self,
	AxisDbConn    *conn,
	const GArray  *start,
	const GArray  *downloading,
	const gchar   *origin,
	const gchar   *user,
	guint         *started,
	GError       **error
);

/**
 * axis_tv_release_brought:
 * @conn: a connection with a transaction open
 * @series: (element-type gint64): the series a connection brought
 * @origin: the app and the connection, for the history lines
 * @released: (out): how many episode files were let go
 *
 * "Let Axis upgrade these files": the episode files of @series that are kept
 * because they were brought over (hand_how "brought", kept, kept_how
 * "brought") are no longer kept, and each series' history says so.  A link a
 * person kept or let go on the series' page, or linked by hand since, is
 * never touched.  No hold is touched, and nothing is searched at once.
 */
gboolean
axis_tv_release_brought(
	AxisTv        *self,
	AxisDbConn    *conn,
	const GArray  *series,
	const gchar   *origin,
	const gchar   *user,
	guint         *released,
	GError       **error
);

/* The scheduler's pace: searches a run, and seconds between runs. */
void
axis_tv_search_pace(
	AxisTv *self,
	guint  *per_run,
	guint  *interval_s
);

/* ── For tests ──────────────────────────────────────────────────── */

/* EXPLAIN (ANALYZE, FORMAT JSON) of the list's one pass over every series on
 * disk (@titles %NULL), or of the counts of the series on disk @titles (a
 * bigint[], a page's). */
gboolean
axis_tv_explain_on_disk(
	AxisTv       *self,
	const gchar  *titles,
	gchar       **plan,
	GError      **error
);

/* Called (on the search's thread) just before the claim transaction, with
 * the series' id. */
void
axis_tv_set_claim_hook(
	AxisTv               *self,
	AxisAcquireClaimHook  hook,
	gpointer              user_data
);

G_END_DECLS

#endif /* AXIS_TV_H */
