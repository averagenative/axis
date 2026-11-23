/*
 * axis-acquire.h - what films and series have in common in getting a
 * release: the search-and-grab job's work, following a grab to its end, the
 * blocklist and its key, and the pacing of automatic searches
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A module (films, series) owns its titles, their states, its tables and
 * its pages.  What it hands this layer is one AxisAcquireOwnerInfo: the
 * names of its tables (each of the same shape: see 0023_movies.sql, whose
 * movie_search, movie_candidate, movie_grab and movie_blocklist are the
 * model), the settings it keeps the shared values under, and a few
 * callbacks: what is wanted (load), what a release must be better than
 * (read_disk), how to ask the indexers and what to match the names against
 * (prepare), what a release is for (judge), whether a claim may be written
 * (claim_check, claimed), its history (event), and what to do after a
 * failure (retry) and on completion (follow_more).
 *
 * The promises the shared code keeps, for every module:
 *
 *  - A release is grabbed only when the matcher says it is the title asked
 *    for (with its other names, against every title of that name the
 *    provider knows; ambiguous is never a match), the module's judge agrees,
 *    the profile accepts it and it is not on the owner's blocklist.  Every
 *    release a search considered is stored with its verdict and reasons.
 *  - A grab is a claim first: in one transaction the owner's row is locked,
 *    the module says whether anything it is for is in flight, and the grab
 *    is written; only then is the release fetched and sent, outside any
 *    transaction.  The module's own unique index is the database's guard
 *    (a unique violation at the claim is "in flight").
 *  - A download that fails, stalls beyond the give-up setting or is gone is
 *    put on the blocklist (by its info-hash when known, and its folded
 *    name), removed from its client with its data, and the module is asked
 *    to search for the next best once nothing of it is at the client.
 *  - Automatic searches are paced per item: a floor that doubles while
 *    searches find nothing or nothing acceptable, up to its maximum, and is
 *    set back by a grab.
 *
 * Threads: everything here blocks (database, indexers, clients): call it
 * from a worker thread.  Create and free on the main thread.
 */

#ifndef AXIS_ACQUIRE_H
#define AXIS_ACQUIRE_H

#include <glib.h>

#include "core/axis-settings.h"
#include "db/axis-db-conn.h"
#include "db/axis-db-pool.h"
#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "meta/axis-meta-model.h"
#include "quality/axis-quality.h"
#include "release/axis-release.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

/* The queue of the films' and series' searches, apart from the downloads
 * queue: following the downloads and importing what finished (finishing an
 * acquisition) never waits behind searches (starting one), however many a
 * series just added queued and however long a slow indexer takes. */
#define AXIS_ACQUIRE_SEARCH_QUEUE    "searches"

/* How many of a search's acceptable releases are tried before it stops. */
#define AXIS_ACQUIRE_MAX_TRIES       5

/* A claim not handed to a client after this long, with no search job of
 * its owner running, is settled by the follow-up. */
#define AXIS_ACQUIRE_CLAIM_STALE_S   600

/* A search that has not finished after this long is taken as failed. */
#define AXIS_ACQUIRE_SEARCH_STALE_S  3600

/* A person asking again for a search of something that a search ended less
 * than this long ago is answered with what that one found, not with another:
 * clicking never reaches an indexer's limits. */
#define AXIS_ACQUIRE_SEARCH_AGAIN_S  60

/* An owner keeps its latest searches (with every release they considered),
 * and any of the last week. */
#define AXIS_ACQUIRE_SEARCHES_KEPT   5
#define AXIS_ACQUIRE_SEARCH_KEEP_S   (7 * 86400)

/* ── Small helpers ──────────────────────────────────────────────── */

/* "%" G_GINT64_FORMAT */
gchar *
axis_acquire_int_text(gint64 value);

/* A text cut to @max characters on a character boundary, valid UTF-8, with
 * control characters made spaces; %NULL for %NULL or nothing left. */
gchar *
axis_acquire_text_cut(
	const gchar *text,
	guint        max
);

/* "manual", "added", "scheduled", "retry", "profile" (the owner's quality
 * profile changed), "list" (a manual search: a person's look, which grabs
 * nothing by itself): the causes of a search. */
gboolean
axis_acquire_cause_valid(const gchar *cause);

/* ── Why something needs a person ───────────────────────────────── */

/**
 * AxisAcquireAttention:
 * @AXIS_ACQUIRE_ATTENTION_NONE: nothing needs a person
 * @AXIS_ACQUIRE_ATTENTION_IMPORT: the import of its download needs a person
 * @AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT: a download Axis gave up on is still
 *   at its client
 * @AXIS_ACQUIRE_ATTENTION_NO_ROOT: it has no destination root
 * @AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE: its destination is not online
 * @AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE: its quality profile cannot be used
 * @AXIS_ACQUIRE_ATTENTION_REJECTED: the last search found releases and the
 *   profile rejected them all ("none_acceptable")
 * @AXIS_ACQUIRE_ATTENTION_EXHAUSTED: every acceptable release failed before or
 *   could not be had ("exhausted")
 * @AXIS_ACQUIRE_ATTENTION_NO_INDEXER, @AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR:
 *   no indexer, or none answered
 * @AXIS_ACQUIRE_ATTENTION_NO_CLIENT, @AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR: no
 *   usable download client takes it, or the client failed
 * @AXIS_ACQUIRE_ATTENTION_METADATA: its record could not be read
 * @AXIS_ACQUIRE_ATTENTION_FAILED: the last search did not finish
 *
 * Why a title (or an episode) needs attention, for the pages: what each
 * kind is offered to put it right.
 */
typedef enum
{
	AXIS_ACQUIRE_ATTENTION_NONE,
	AXIS_ACQUIRE_ATTENTION_IMPORT,
	AXIS_ACQUIRE_ATTENTION_LEFT_AT_CLIENT,
	AXIS_ACQUIRE_ATTENTION_NO_ROOT,
	AXIS_ACQUIRE_ATTENTION_ROOT_OFFLINE,
	AXIS_ACQUIRE_ATTENTION_PROFILE_UNUSABLE,
	AXIS_ACQUIRE_ATTENTION_REJECTED,
	AXIS_ACQUIRE_ATTENTION_EXHAUSTED,
	AXIS_ACQUIRE_ATTENTION_NO_INDEXER,
	AXIS_ACQUIRE_ATTENTION_INDEXER_ERROR,
	AXIS_ACQUIRE_ATTENTION_NO_CLIENT,
	AXIS_ACQUIRE_ATTENTION_CLIENT_ERROR,
	AXIS_ACQUIRE_ATTENTION_METADATA,
	AXIS_ACQUIRE_ATTENTION_FAILED
} AxisAcquireAttention;

/* The kind of a last search's @outcome that needs attention, NONE for one
 * that does not. */
AxisAcquireAttention
axis_acquire_attention_of_outcome(const gchar *outcome);

/* What the kind is, short, for a count ("the profile rejected what was
 * found"). */
const gchar *
axis_acquire_attention_phrase(AxisAcquireAttention kind);

/*
 * Whether the kind is the title's own, for the page "Needs attention"
 * (add-attention-notices): every acceptable release failed, the profile
 * rejected what was found, no destination, a profile that cannot be used, a
 * download left at its client, a record that could not be read.  The others
 * are consequences of something that has a notice of its own (no indexer or
 * its error, no download client or its error, the destination offline, a
 * blocked import) or are tried again by Axis (a search that did not finish):
 * a title that needs attention only for one of those is not counted there.
 */
gboolean
axis_acquire_attention_is_own(AxisAcquireAttention kind);

/* A kind that is the title's own, as a clause to follow "because" ("every
 * acceptable release failed", "no library folder is set as the
 * destination"). */
const gchar *
axis_acquire_attention_because(AxisAcquireAttention kind);

/**
 * axis_acquire_name_key:
 * @source_id: the indexer source that returned the release
 * @indexer_id: the source's own number of the indexer, 0 when not stated
 * @indexer: (nullable): the indexer's name, used when @indexer_id is 0
 * @name: the release's name
 *
 * The folded form a release is known by on a blocklist without its hash:
 * "<source>:<indexer>:<name>", the name case-folded with every run of
 * characters that are not letters or digits made one '.'.
 *
 * Returns: (transfer full): the key, at most 400 bytes
 */
gchar *
axis_acquire_name_key(
	gint64       source_id,
	gint64       indexer_id,
	const gchar *indexer,
	const gchar *name
);

/* ── What is stored, read back ──────────────────────────────────── */

/**
 * AxisAcquireReason:
 * @kind: "safety", "profile", "upgrade", "blocklist", or the module's own
 * @code: the quality layer's code, "blocklisted", or the module's own
 * @text: the sentence
 */
typedef struct
{
	gchar *kind;
	gchar *code;
	gchar *text;
} AxisAcquireReason;

AxisAcquireReason *
axis_acquire_reason_new(
	const gchar *kind,
	const gchar *code,
	const gchar *text
);

void
axis_acquire_reason_free(AxisAcquireReason *reason);

/* Reasons as stored ([{"k": kind, "c": code, "t": text}]) and back. */
gchar *
axis_acquire_reasons_to_json(const GPtrArray *reasons);

GPtrArray *
axis_acquire_reasons_from_json(const gchar *json);

/**
 * AxisAcquireCandidate:
 *
 * One release a search considered, as stored.
 */
typedef struct
{
	gint64                id;
	gint64                search_id;
	gint                  position;
	gint64                release_id;
	gint64                source_id;
	gchar                *name;
	AxisDownloadProtocol  protocol;
	gint64                size_bytes;	/* -1: not stated */
	gint64                published_us;
	gint                  seeders;		/* -1: not stated */
	gchar                *indexer;
	gint64                indexer_id;
	gchar                *info_hash;
	gchar                *name_key;
	gchar                *quality;		/* ladder name, "unknown" */
	AxisReleaseMatch      match;
	gchar                *match_text;
	gboolean              accepted;
	gboolean              safety;
	gboolean              blocklisted;
	guint                 rank;
	gint                  score;
	GPtrArray            *reasons;	/* AxisAcquireReason */
	gchar                *note;
	/* Set by axis_acquire_candidates_mark_blocked() alone, for a page: a
	 * person put it on the blocklist (add-block-release), and who. */
	gboolean              blocked_by_person;
	gchar                *blocked_by;	/* (nullable) */
} AxisAcquireCandidate;

void
axis_acquire_candidate_free(AxisAcquireCandidate *candidate);

/* One entry of an owner's blocklist, as stored. */
typedef struct
{
	gint64  id;
	gchar  *release_name;
	gchar  *indexer;
	gchar  *reason;
	gchar  *reason_text;
	gchar  *created_by;
	gint64  created_us;
	gchar  *info_hash;
} AxisAcquireBlock;

void
axis_acquire_block_free(AxisAcquireBlock *block);

/* A stored search, finished or not. */
typedef struct
{
	gint64  id;
	gchar  *cause;
	gchar  *requested_by;
	gint64  started_us;
	gint64  finished_us;
	gchar  *outcome;
	gchar  *outcome_text;
	gchar  *how;
	guint   n_sources;
	guint   n_found;
	guint   n_accepted;
	guint   n_compared;
} AxisAcquireSearchInfo;

void
axis_acquire_search_info_free(AxisAcquireSearchInfo *search);

/* ── What a module answers ──────────────────────────────────────── */

/**
 * AxisAcquireTarget:
 * @owner_id: the owner's row (movie.id, series.id): what a claim locks, a
 *   blocklist belongs to and the history is written for
 * @scope: (nullable): the module's own word for what is searched within
 *   the owner (%NULL for a film; "episode", "season", "series")
 * @item_id: the module's own identifier within @scope, 0 for none
 * @by_hand: a person's own look or grab (a manual search, Grab this one):
 *   the module's load() skips only an owner (or item) that is gone, not one
 *   its automatic rules do not want now (no air date, not monitored, not
 *   available yet, in flight)
 * @person: set by the module for a search or a grab a person asked for
 *   themselves (Search now, a manual search, a grab of a listed release): the
 *   module's load() lets it pass a hold that stops only automatic searches (a
 *   series brought over and held).  The shared code only carries it.
 * @named_set: by force, the person named what the release is for (@named,
 *   @named_whole): the module binds the grab to exactly that; unset, the
 *   module's force preview proposes what the name and the search suggest
 * @named: (element-type gint64) (nullable): with @named_set, the module's
 *   items the person named (a series' episodes)
 * @named_whole, @named_part: with @named_set, the person named a whole part
 *   (a series' season @named_part)
 *
 * What one search (or a manual grab) is for, as the module's job names it.
 */
typedef struct
{
	gint64        owner_id;
	const gchar  *scope;
	gint64        item_id;
	gboolean      by_hand;
	gboolean      person;
	gboolean      named_set;
	const GArray *named;
	gboolean      named_whole;
	gint64        named_part;
} AxisAcquireTarget;

/**
 * AxisAcquireWant:
 *
 * What the module's load() says is wanted, with what the shared code needs
 * of it.  The module allocates it with axis_acquire_want_new() and keeps its
 * own facts in @data.
 *
 * @owner_id, @title_id: the owner, and the metadata title a download is sent
 *   for (download.title_id)
 * @label: "Hearth (1995)": a download's and the history's label
 * @in_flight: a grab of it is in flight (a manual grab is refused with that)
 * @profile_id, @profile_name, @profile_usable: its quality profile
 * @root_id, @root_name, @volume_id, @volume_name: where it goes (0: none)
 * @runtime_minutes: what a release of it should run, 0 when not known
 * @pace_ids: (element-type gint64): the rows of the owner info's pace table
 *   whose pacing this search sets
 * @on_disk, @disk: what a release must be better than (read_disk(), or
 *   axis_acquire_want_set_disk()); unused with decide_each
 * @asks: (element-type AxisAcquireAsk) (nullable): what the indexers are asked
 *   besides prepare()'s request (axis_acquire_want_add_ask()); %NULL for none,
 *   which is every search but an anime series' (add-anime-support, A3)
 * @ask_label: (nullable): what prepare()'s own request is called in the
 *   search's record ("by season 6 episode 12, in TV and Anime"); %NULL: the
 *   record names the indexers and how each was searched, and no more
 */
typedef struct
{
	gint64             owner_id;
	gint64             title_id;
	gchar             *label;
	gboolean           in_flight;
	gint64             profile_id;
	gchar             *profile_name;
	gboolean           profile_usable;
	gint64             root_id;
	gchar             *root_name;
	gint64             volume_id;
	gchar             *volume_name;
	gint               runtime_minutes;
	GArray            *pace_ids;
	gboolean           on_disk;
	AxisQualityOnDisk  disk;
	AxisRelease       *disk_fields;	/* owned: what disk.fields points at */
	gchar             *disk_name;	/* owned: what disk.name points at */
	GPtrArray         *asks;
	gchar             *ask_label;
	gpointer           data;
	GDestroyNotify     data_free;
} AxisAcquireWant;

/* Requests a search makes besides its own, at most. */
#define AXIS_ACQUIRE_MAX_ASKS 3

/**
 * AxisAcquireAsk:
 * @request: what to ask every indexer; its title is @words
 * @words: the words asked
 * @label: what the request is called in the search's record ("by the words
 *   "Tenkuu Rangers 137", in TV and Anime")
 *
 * One more request of a search.  Each is a request of its own to the indexer
 * layer: it is kept and reused as any search is, counts against each
 * indexer's own limits, and passes over an indexer that is held or near its
 * allowance exactly as the first request does.
 */
typedef struct
{
	AxisIndexerRequest  request;
	gchar              *words;
	gchar              *label;
} AxisAcquireAsk;

/**
 * axis_acquire_want_add_ask:
 * @request: the request; its title is replaced by a copy of @words
 *
 * For prepare(): one more request for the search to make after its own.  What
 * the requests find is read as one list, a release that several return once.
 * The same words (whatever their case) are asked once, and no more than
 * %AXIS_ACQUIRE_MAX_ASKS are kept.
 */
void
axis_acquire_want_add_ask(
	AxisAcquireWant          *want,
	const AxisIndexerRequest *request,
	const gchar              *words,
	const gchar              *label
);

AxisAcquireWant *
axis_acquire_want_new(void);

void
axis_acquire_want_free(AxisAcquireWant *want);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAcquireWant, axis_acquire_want_free)

/**
 * axis_acquire_disk_set:
 * @disk: (out): what a release must be better than
 * @fields: (out) (transfer full): what @disk->fields points at
 * @name_out: (out) (transfer full): what @disk->name points at
 * @quality: the file's recorded quality (unknown when none)
 * @resolution: for an unknown quality, the resolution known for it
 * @name: (nullable): the name it is read by (the release it came in as, else
 *   its own name)
 *
 * The file Axis knows as @quality (or an unknown source at @resolution),
 * read by @name for its score and revision.  It is set as not converted;
 * a caller whose file has a done replacement (the conversion record) sets
 * @disk->converted and @disk->source_quality after.
 */
void
axis_acquire_disk_set(
	AxisQualityOnDisk      *disk,
	AxisRelease           **fields,
	gchar                 **name_out,
	AxisQuality             quality,
	AxisReleaseResolution   resolution,
	const gchar            *name
);

/**
 * AxisAcquireConsidered:
 *
 * One release a search considers.  The module's judge() sees it after the
 * matcher has read it against the titles from prepare(), and may change
 * @match and @match_text (a release that is not what was asked for: a
 * fixed rule, which no one may override by hand), set @rejected with
 * @reject_code and @reject_text (not wanted: the profile's kind of reason,
 * which an administrator may override by grabbing it), @reject_phrase (the
 * summary's words for either), @data (what the release holds, for
 * claim_check(), claimed() and candidate_stored()) and, with decide_each,
 * @disk, @runtime_minutes and @noun (what the release is decided against).
 * The rest is the shared code's.
 */
typedef struct
{
	const AxisIndexerRelease *release;	/* borrowed from its result */
	gint64                    source_id;
	AxisRelease              *parsed;
	AxisReleaseMatch          match;
	gchar                    *match_text;
	gint                      matched;	/* the title matched (prepare()'s), -1 */
	gchar                    *name_key;
	gboolean                  blocklisted;
	const gchar              *block_reason;	/* (nullable) borrowed: the entry's reason */
	const gchar              *block_by;	/* (nullable) borrowed: who made the entry */
	const AxisQualityVerdict *verdict;	/* borrowed from the decision */
	gboolean                  accepted;	/* by the profile, and not blocklisted */
	guint                     rank;
	gint64                    candidate_id;
	gchar                    *note;
	/* The module's. */
	gboolean                  rejected;
	const gchar              *reject_code;	/* static */
	gchar                    *reject_text;	/* owned */
	const gchar              *reject_phrase;	/* (nullable) static */
	gpointer                  data;
	GDestroyNotify            data_free;
	const AxisQualityOnDisk  *disk;		/* decide_each: (nullable) borrowed */
	gint                      runtime_minutes;	/* decide_each: 0 not known */
	const gchar              *noun;		/* decide_each: (nullable) static */
	AxisQualityVerdict       *own_verdict;	/* decide_each: the shared code's */
} AxisAcquireConsidered;

/**
 * AxisAcquireGrabSpec:
 *
 * What a claim is written for.  @considered is the search's, for a grab a
 * search makes (%NULL for an administrator's grab of a stored candidate:
 * the module reads what it holds by @candidate_id).
 */
typedef struct
{
	const AxisAcquireConsidered *considered;
	gint64                       search_id;
	gint64                       candidate_id;
	gint64                       release_id;
	gint64                       source_id;
	const gchar                 *name;
	AxisDownloadProtocol         protocol;
	gint64                       size_bytes;
	const gchar                 *indexer;
	gint64                       indexer_id;
	const gchar                 *quality;
	const gchar                 *name_key;
	const gchar                 *info_hash;
	const gchar                 *why;
	gboolean                     manual;
	const gchar                 *set_aside;	/* (nullable) "anyway", "hand": the profile set aside */
	const gchar                 *replaces_files;	/* (nullable) JSON: the files ticked to be replaced */
	gboolean                     numbering_ok;	/* the numbering ticked */
	const gchar                 *forced;	/* (nullable) JSON: the fixed rules a person set aside */
	gboolean                     bound;	/* its import does not judge the name again */
	const gchar                 *user;
} AxisAcquireGrabSpec;

/**
 * AxisAcquireSearchReport:
 * @searched: a search was made (or decided at once) and stored
 * @skipped: why it was not, when it was not (static text)
 * @search_id: the stored search
 * @outcome: its outcome code (static)
 * @grab_id: the grab, when one was sent
 * @tries: releases it tried to send
 */
typedef struct
{
	gboolean     searched;
	const gchar *skipped;
	gint64       search_id;
	const gchar *outcome;
	gint64       grab_id;
	guint        tries;
} AxisAcquireSearchReport;

/**
 * AxisAcquireFollowReport:
 * @settled: claims never handed over, settled
 * @failed: grabs whose download failed, stalled beyond the limit, was gone
 *   or not taken, dealt with
 * @removed: their downloads removed from the clients
 * @removal_failed: removals that failed (tried again next time)
 * @retries: searches queued for the next best
 * @completed: completions recorded
 * @imports: imports opened for completed downloads (follow_more())
 */
typedef struct
{
	guint settled;
	guint failed;
	guint removed;
	guint removal_failed;
	guint retries;
	guint completed;
	guint imports;
} AxisAcquireFollowReport;

/**
 * AxisAcquireReplaced:
 * @file_id: its library_file
 * @label: what the file in the library is ("S01E04's file", its name)
 * @quality, @resolution: its quality (a converted file's: its source's), and
 *   for an unknown source the resolution known for it
 * @better: set by the shared code: the release against it, > 0 better, 0
 *   the same, < 0 worse or not known to be better
 * @kept_by: (nullable): who keeps the file by hand ("a person" when no name
 *   was recorded); set by the module.  Such a file is replaced only with a
 *   tick that names it, whatever the kind of grab and whether or not the
 *   release is better
 * @kept_from: (nullable) (static): with @kept_by, for a file that is kept
 *   because it was brought over from another app and by no person's choice:
 *   that app's name ("Radarr"); set by the module.  The words then name the
 *   app and no person
 *
 * A file a grab by hand would replace.
 */
typedef struct
{
	gint64                 file_id;
	gchar                 *label;
	AxisQuality            quality;
	AxisReleaseResolution  resolution;
	gint                   better;
	gchar                 *kept_by;
	const gchar           *kept_from;
} AxisAcquireReplaced;

/* How @replaced is kept, for a sentence ("the file X is ..."): "kept by hand
 * by root", or "kept as it was brought over from Radarr". */
gchar *
axis_acquire_replaced_kept_words(const AxisAcquireReplaced *replaced);

/**
 * AxisAcquireChoice:
 * @id: the module's item (a series' episode), or with @whole the part's
 *   number (a season)
 * @label: what it is ("S01E03, Neap", "Season 1, every episode (5)")
 * @whole: a whole part, not one item
 * @picked: the grab would be bound to it as the page stands
 *
 * One thing a grab by force can be bound to, for the confirmation's picker.
 */
typedef struct
{
	gint64    id;
	gchar    *label;
	gboolean  whole;
	gboolean  picked;
} AxisAcquireChoice;

AxisAcquireChoice *
axis_acquire_choice_new(
	gint64       id,
	const gchar *label,
	gboolean     whole,
	gboolean     picked
);

void
axis_acquire_choice_free(AxisAcquireChoice *choice);

AxisAcquireReplaced *
axis_acquire_replaced_new(
	gint64                 file_id,
	const gchar           *label,
	AxisQuality            quality,
	AxisReleaseResolution  resolution
);

void
axis_acquire_replaced_free(AxisAcquireReplaced *replaced);

/**
 * AxisAcquireStanding:
 * @grab_id: a grab that stands in the way of the release chosen
 * @release: (nullable): its release
 * @open: it is not closed (a closed one stands in the way while its download
 *   is still being removed from its client)
 * @download_id: its download (0: not handed to a client yet)
 * @at_client: the download has the client's identifier
 * @client: (nullable): the client's name
 * @state: (nullable): the download's state, as recorded
 * @size_bytes, @done_bytes: -1 when not known
 * @completed: the grab or its download has completed
 * @import_id: the latest import of the download (0: none)
 * @same: it is a grab of the very release chosen (replacing a download with
 *   itself is not offered)
 * @same_torrent: its torrent is the one the release chosen is (the same info
 *   hash, both known), whatever indexer each came from: not offered either
 * @holds: (nullable): what it holds, in the module's words ("S01E01 to
 *   S01E10")
 * @others: (nullable): what it holds that the release chosen does not
 *   ("S01E02, S01E04"): those stop downloading too when it is replaced
 *
 * One grab in the way of a grab by hand (add-replace-grab-and-delete-
 * download, slice A).  The module's standing() gives @grab_id, @holds and
 * @others; the layer reads the rest.
 */
typedef struct
{
	gint64    grab_id;
	gchar    *release;
	gboolean  open;
	gint64    download_id;
	gboolean  at_client;
	gchar    *client;
	gchar    *state;
	gint64    size_bytes;
	gint64    done_bytes;
	gboolean  completed;
	gint64    import_id;
	gboolean  same;
	gboolean  same_torrent;
	gchar    *holds;
	gchar    *others;
} AxisAcquireStanding;

AxisAcquireStanding *
axis_acquire_standing_new(
	gint64       grab_id,
	const gchar *holds,
	const gchar *others
);

void
axis_acquire_standing_free(AxisAcquireStanding *standing);

/* The most items a grab by force's picker offers, and a form may name (the
 * episodes of a series, the files a grab replaces): one limit for both, so
 * what a page shows is what a post carries.  Above it a page refuses plainly. */
#define AXIS_ACQUIRE_FORCE_PICK_MAX 10000

/**
 * AxisAcquireHandPreview:
 * @candidate: the release, as stored
 * @refused: (nullable): why it cannot be taken by hand, a sentence
 * @fills: (element-type utf8): what it would fill, each with its notes
 *   ("S01E03 (no air date, not monitored)", "the movie")
 * @replaces: (element-type AxisAcquireReplaced): the files in the library
 *   it would replace
 * @missing: (nullable): what it names that the owner's data does not have
 *   ("S01E06")
 * @numbering: (nullable): with @missing, the warning that the release
 *   numbers its parts differently from the data, and what its import does
 *   then; taking it needs a tick of its own
 * @needs_tick: it replaces a file it is not better than: a second tick
 * @profile_name: the owner's quality profile, which is set aside
 * @force: this is what a grab by force would do (axis_acquire_force_preview())
 * @digest: (nullable): with @force and not @refused, the digest of what the
 *   confirmation shows; a grab by force is made only while it is the same
 * @owner_label: (nullable): with @force, what the grab is bound to ("Hearth
 *   (1995)")
 * @choices: (element-type AxisAcquireChoice) (nullable): with @force, what
 *   the person may bind the grab to within the owner (a series' episodes and
 *   seasons), each marked when it is picked as the page stands; %NULL when
 *   the owner is the only thing to bind to (a movie)
 * @unnamed: with @choices, nothing is picked: the grab cannot be made until
 *   the person names what the release is for
 * @pick_help: (nullable): with @unnamed, why what was named cannot be taken
 * @pick_link, @pick_link_text: (nullable): with @pick_help, a page (a path on
 *   this site) where it can be done, and what to call the link
 * @bound_text: (nullable): with @choices and not @unnamed, what is picked, in
 *   words ("S01E03 of Lantern Bay (2019)")
 * @import_text: (nullable): the module's own words for what the import of
 *   this bound grab does
 * @digest_more: (nullable): what else of the module's the digest covers (a
 *   series: the numbering it follows, the season or episodes picked, their
 *   monitoring)
 *
 * @flying: (nullable): the module's own sentence for a part the release is
 *   for that is in flight (a series: "It holds S01E03, whose grab is in
 *   flight ..."); kept apart from @refused, which is every other reason
 * @in_flight: something stands in the way: a grab of what the release is
 *   for is open, or a download Axis gave up on is still at its client
 * @standing: (element-type AxisAcquireStanding) (nullable): with
 *   @in_flight, the grabs in the way, as far as they are known
 * @standing_text: (nullable): with @in_flight, what stands in the way and
 *   what to do, in sentences; it is @refused too unless @replaceable
 * @standing_import: with @in_flight, the import to resolve first (0: none)
 * @standing_downloads: with @in_flight, the Downloads page is where it is
 *   dealt with
 * @replaceable: with @in_flight and not @refused: exactly one grab is in the
 *   way (@standing's one), it is open, and its download is at its client,
 *   has not completed and has no import: the confirmation offers "Replace
 *   the download in flight with this one"
 * @replace_digest: (nullable): with @replaceable, the digest of what the
 *   offer shows (the release chosen, the grab, its download, what it holds
 *   and what of that the release does not hold); a replace is made only
 *   while it is the same
 *
 * What a grab by hand of a stored release would do, for its confirmation.
 * The module's hand_preview() fills @fills, @replaces, @missing, @numbering
 * and, for what only it knows, @refused and @flying.
 */
typedef struct
{
	AxisAcquireCandidate *candidate;
	gchar                *refused;
	GPtrArray            *fills;
	GPtrArray            *replaces;
	gchar                *missing;
	gchar                *numbering;
	gboolean              needs_tick;
	gchar                *profile_name;
	gboolean              force;
	gchar                *digest;
	gchar                *owner_label;
	GPtrArray            *choices;
	gboolean              unnamed;
	gchar                *pick_help;
	gchar                *pick_link;
	gchar                *pick_link_text;
	gchar                *bound_text;
	gchar                *import_text;
	gchar                *digest_more;
	gchar                *flying;
	gboolean              in_flight;
	GPtrArray            *standing;
	gchar                *standing_text;
	gint64                standing_import;
	gboolean              standing_downloads;
	gboolean              replaceable;
	gchar                *replace_digest;
} AxisAcquireHandPreview;

void
axis_acquire_hand_preview_free(AxisAcquireHandPreview *preview);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAcquireHandPreview, axis_acquire_hand_preview_free)

/**
 * AxisAcquireOwnerInfo:
 *
 * A module's side.  The table and column names are fixed strings of the
 * module's (never input), put into the shared statements, and
 * axis_acquire_new() asserts that each (and @module) matches
 * ^[a-z_][a-z0-9_]*$; the two job-key expressions must be string literals
 * in the module's own source.  Each table has
 * the columns of its films counterpart in 0023_movies.sql, with
 * @owner_column in place of movie_id.  The SQL fragments are expressions of
 * the de-duplication key of the search job that may be working on a stored
 * search (alias s of @search_table) or a grab (alias g of @grab_table).
 *
 * @module: download.module and import.module ("movies")
 * @noun: what an owner is, in sentences ("film")
 * @owner_table: locked by every claim and every failure ("movie"); has
 *   title_id
 * @owner_column: the owner's column in the tables below ("movie_id")
 * @pace_table: has last_search_at, next_search_at and search_floor_s
 * @setting_*: the module's settings for the shared values (defaults: floor
 *   6 h, its maximum 7 d, give up 6 h, protocol any, retention 0, languages
 *   none)
 * @decide_each: each release is decided against its own file on disk and
 *   runtime (judge() sets them) instead of the want's
 *
 * Callbacks (all but those marked nullable are required):
 * @load: what @target wants, read on @conn.  %TRUE with *@want %NULL and
 *   *@skip (static) when it is gone; with *@want and *@skip when nothing
 *   is to be asked now (in flight, not available).  %FALSE only for a
 *   database error.
 * @read_disk: (nullable) what a release must be better than (sets
 *   want->on_disk and want->disk)
 * @prepare: fills @request and *@titles (element-type AxisMetaTitle,
 *   borrowed from the want's data; [0] is what was asked for, the others are
 *   the titles of its name it must not be mistaken for).  %TRUE with
 *   *@problem when its record cannot be read (the search ends with
 *   "metadata_missing").
 * @judge: (nullable) a release the matcher says is titles[0]: what it is
 *   for (see AxisAcquireConsidered)
 * @search_stored, @candidate_stored: (nullable) after the search's and each
 *   candidate's row is written, in the same transaction: the module's own
 *   columns
 * @claim_check: in the claim's transaction, the owner's row locked:
 *   *@gone when the owner was removed, *@in_flight when anything the grab is
 *   for is in flight
 * @claimed: (nullable) in the claim's transaction, after the grab's row:
 *   the module's own rows (a unique violation is "in flight")
 * @event: the owner's history (@want is %NULL when the caller has none); an
 *   owner that is gone writes nothing and succeeds
 * @retry: queue a search for the next best after a failed grab (never for
 *   a grab by hand: Grab this one's failure is the person's to follow)
 * @follow_more: (nullable) the module's part of the follow-up (imports for
 *   completed grabs)
 * @monitored: (nullable) whether what the grab was for is monitored (a film
 *   monitored, an episode of the grab monitored), for a retry that is asked
 *   only then; without it, it is
 * @hand_preview: (nullable) what a grab by hand of a stored release would
 *   fill and replace (see AxisAcquireHandPreview), read on @conn with the
 *   want load() gave for a target by hand; without it a grab by hand fills
 *   "the title" and replaces nothing known
 */
typedef struct
{
	const gchar *module;
	const gchar *noun;
	const gchar *owner_table;
	const gchar *owner_column;
	const gchar *search_table;
	const gchar *candidate_table;
	const gchar *grab_table;
	const gchar *blocklist_table;
	const gchar *pace_table;
	const gchar *search_job_sql;
	const gchar *grab_job_sql;
	const gchar *setting_floor;
	const gchar *setting_floor_max;
	const gchar *setting_give_up;
	const gchar *setting_protocol;
	const gchar *setting_retention;
	const gchar *setting_languages;
	gboolean     decide_each;

	gboolean (*load)(gpointer data, AxisDbConn *conn, const AxisAcquireTarget *target, AxisAcquireWant **want,
	                 const gchar **skip, GError **error);
	gboolean (*read_disk)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want, GError **error);
	gboolean (*prepare)(gpointer data, AxisAcquireWant *want, GCancellable *cancellable,
	                    AxisIndexerRequest *request, GPtrArray **titles, gchar **problem, GError **error);
	void     (*judge)(gpointer data, AxisAcquireWant *want, AxisAcquireConsidered *considered);
	gboolean (*search_stored)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want, gint64 search_id,
	                          GError **error);
	gboolean (*candidate_stored)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want,
	                             const AxisAcquireConsidered *considered, gint64 candidate_id, GError **error);
	gboolean (*claim_check)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want,
	                        const AxisAcquireGrabSpec *grab, gboolean *gone, gboolean *in_flight, GError **error);
	gboolean (*claimed)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want, const AxisAcquireGrabSpec *grab,
	                    gint64 grab_id, GError **error);
	gboolean (*event)(gpointer data, AxisDbConn *conn, gint64 owner_id, const AxisAcquireWant *want,
	                  const gchar *kind, const gchar *user, gint64 search_id, gint64 grab_id, const gchar *text,
	                  GError **error);
	gboolean (*retry)(gpointer data, AxisDbConn *conn, gint64 owner_id, gint64 grab_id, const gchar *user,
	                  GError **error);
	gboolean (*follow_more)(gpointer data, AxisAcquireFollowReport *report, GError **error);
	gboolean (*monitored)(gpointer data, AxisDbConn *conn, gint64 owner_id, gint64 grab_id, gboolean *monitored,
	                      GError **error);
	gboolean (*hand_preview)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want,
	                         const AxisAcquireCandidate *candidate, AxisAcquireHandPreview *preview, GError **error);
	/* Whether the module's manual search offers Grab it by force (its page
	 * answers force_preview() and force_grab()): only then do the words say that
	 * an administrator may grab a release a rule about its name rules out. */
	gboolean     force;
	/* (nullable) A release whose name, read by itself, is none of the titles
	 * from prepare() (never one that is another of those titles, and never an
	 * ambiguous one): the module may know it for titles[0] by a fixed rule of
	 * its own that needs what the shared matcher does not have (an anime
	 * series' name followed by a bare number, which reading by itself leaves
	 * in the title), set @match to a match with @matched 0 and @match_text,
	 * and judge it; or leave it as it is, with a better @match_text.  Called
	 * by a search only; a stored release keeps the verdict it was stored
	 * with. */
	void     (*unmatched)(gpointer data, AxisAcquireWant *want, AxisAcquireConsidered *considered);
	/* (nullable) The grabs that stand in the way of a grab by hand of
	 * @candidate as @preview stands (what it is bound to, by force): each open
	 * grab that holds a part the release is for, and each closed one whose
	 * download is still being removed from its client, added to @standing
	 * (AxisAcquireStanding, with what it holds and what of that the release
	 * does not).  Read only.  Without it (an owner with no parts: a movie)
	 * they are the owner's open grabs and its grabs still being removed.  The
	 * last member, so that an owner that does not name it has none. */
	gboolean (*standing)(gpointer data, AxisDbConn *conn, AxisAcquireWant *want,
	                     const AxisAcquireCandidate *candidate, const AxisAcquireHandPreview *preview,
	                     GPtrArray *standing, GError **error);
} AxisAcquireOwnerInfo;

/* ── The layer ──────────────────────────────────────────────────── */

typedef struct _AxisAcquire AxisAcquire;

/* Defined in compat/axis-compat.h. */
typedef struct _AxisCompat AxisCompat;

/**
 * axis_acquire_new:
 * @pool: the database, which must outlive it
 * @settings: (nullable): the settings store
 * @indexers, @downloads, @storage: the layers it joins (referenced)
 * @info: the module's side, which must outlive it
 * @data: the module, for the callbacks (borrowed)
 *
 * Returns: (transfer full): the layer for one module
 */
AxisAcquire *
axis_acquire_new(
	AxisDbPool                 *pool,
	AxisSettings               *settings,
	AxisIndexers               *indexers,
	AxisDownloads              *downloads,
	AxisStorage                *storage,
	const AxisAcquireOwnerInfo *info,
	gpointer                    data
);

void
axis_acquire_free(AxisAcquire *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAcquire, axis_acquire_free)

/* The household's players, for the profiles' players preference (%NULL:
 * none known; borrowed: it outlives the layer). */
void
axis_acquire_set_compat(
	AxisAcquire *self,
	AxisCompat  *compat
);

const AxisAcquireOwnerInfo *
axis_acquire_get_info(AxisAcquire *self);

/* The shared settings, through the module's keys (or the defaults). */
gint64
axis_acquire_setting_duration(
	AxisAcquire *self,
	const gchar *key,
	gint64       fallback
);

gint64
axis_acquire_setting_integer(
	AxisAcquire *self,
	const gchar *key,
	gint64       fallback
);

gchar *
axis_acquire_setting_string(
	AxisAcquire *self,
	const gchar *key,
	const gchar *fallback
);

/* The module's languages setting: the ISO 639-1 codes in it (others are
 * left out), as a %NULL-ended vector; empty for none. */
gchar **
axis_acquire_setting_languages(AxisAcquire *self);

/* ── The search-and-grab job ────────────────────────────────────── */

/**
 * axis_acquire_run_search:
 * @cause: "manual", "added", "scheduled" or "retry"
 * @user: (nullable)
 * @cancellable: (nullable)
 * @report: (out) (optional)
 *
 * The job's work, for one target: load and the checks that need no one
 * (in flight, availability, the profile, the destination, a usable client),
 * the metadata (prepare), every enabled indexer source, then read, match,
 * judge, blocklist, decide (against the file on disk), rank and store every
 * release; then claim, fetch and send the best acceptable release whose
 * protocol has a usable client, trying the next on a release that cannot be
 * had, up to AXIS_ACQUIRE_MAX_TRIES.  Fails only for a database error or
 * cancellation; everything else is the search's outcome.
 *
 * With @cause "list" (a manual search) the target is by hand, every indexer
 * is asked (as for "manual"), every release is stored with its verdict, and
 * the search ends there with the outcome "listed": nothing is claimed or
 * sent, and no item's pacing changes.
 */
gboolean
axis_acquire_run_search(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	const gchar              *cause,
	const gchar              *user,
	GCancellable             *cancellable,
	AxisAcquireSearchReport  *report,
	GError                  **error
);

/**
 * AxisAcquireGrabResult:
 * @AXIS_ACQUIRE_GRAB_SENT: handed to a client (or maybe: uncertain)
 * @AXIS_ACQUIRE_GRAB_NOT_FOUND: no such owner (or target)
 * @AXIS_ACQUIRE_GRAB_NO_CANDIDATE: the candidate is not one of its searches'
 * @AXIS_ACQUIRE_GRAB_REFUSED: a fixed safety rule rejects the release
 * @AXIS_ACQUIRE_GRAB_BLOCKLISTED: it is on the owner's blocklist
 * @AXIS_ACQUIRE_GRAB_IN_FLIGHT: a grab of it is in flight
 * @AXIS_ACQUIRE_GRAB_OFFLINE: the destination is not online
 * @AXIS_ACQUIRE_GRAB_EXPIRED: the search that found it has expired
 * @AXIS_ACQUIRE_GRAB_NO_CLIENT: no usable client takes its protocol
 * @AXIS_ACQUIRE_GRAB_GONE: the owner was removed meanwhile
 * @AXIS_ACQUIRE_GRAB_BAD_RELEASE: this release cannot be had
 * @AXIS_ACQUIRE_GRAB_CLIENT_ERROR: the client failed
 * @AXIS_ACQUIRE_GRAB_SOURCE_ERROR: the indexer source failed
 * @AXIS_ACQUIRE_GRAB_CHANGED: by force, what its confirmation showed is not
 *   what is so now (its digest differs): nothing was sent
 * @AXIS_ACQUIRE_GRAB_NEEDS_TICK: by hand, it would replace a file it is not
 *   better than, and that was not ticked
 * @AXIS_ACQUIRE_GRAB_REPLACE_STUCK: a replace of the download in flight was
 *   asked for and nothing was sent: the download could not be removed from
 *   its client (the grab is closed, its removal is asked again by the
 *   follow-up), or Axis will not ask the client to delete it where it is
 *   (nothing was ended); the words say which
 */
typedef enum
{
	AXIS_ACQUIRE_GRAB_SENT,
	AXIS_ACQUIRE_GRAB_NOT_FOUND,
	AXIS_ACQUIRE_GRAB_NO_CANDIDATE,
	AXIS_ACQUIRE_GRAB_REFUSED,
	AXIS_ACQUIRE_GRAB_BLOCKLISTED,
	AXIS_ACQUIRE_GRAB_IN_FLIGHT,
	AXIS_ACQUIRE_GRAB_OFFLINE,
	AXIS_ACQUIRE_GRAB_EXPIRED,
	AXIS_ACQUIRE_GRAB_NO_CLIENT,
	AXIS_ACQUIRE_GRAB_GONE,
	AXIS_ACQUIRE_GRAB_BAD_RELEASE,
	AXIS_ACQUIRE_GRAB_CLIENT_ERROR,
	AXIS_ACQUIRE_GRAB_SOURCE_ERROR,
	AXIS_ACQUIRE_GRAB_NEEDS_TICK,
	AXIS_ACQUIRE_GRAB_CHANGED,
	AXIS_ACQUIRE_GRAB_REPLACE_STUCK
} AxisAcquireGrabResult;

/**
 * axis_acquire_grab:
 * @target: the owner (and what the module loads for it)
 * @candidate_id: a release one of the owner's searches considered
 * @user: (nullable): who
 * @grab_id: (out) (optional)
 * @result: (out): what came of it
 * @problem: (out) (transfer full) (nullable): the words for it: the safety
 *   reasons for REFUSED, "no usable download client takes ... releases" for
 *   NO_CLIENT before a claim, the indexer's or client's words after one
 *
 * An administrator's grab of a listed release, accepted or rejected by the
 * profile, checked in this order: the candidate, the safety rules, the
 * blocklist, in flight, the destination, the release still known, a usable
 * client; then claimed, fetched and sent as a manual grab.  A release the
 * profile rejected (Grab it anyway) is sent with set_aside "anyway", so its
 * import sets the profile aside; an accepted one (Grab) is not.  Fails only
 * for a database error.
 */
gboolean
axis_acquire_grab(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	const gchar              *user,
	gint64                   *grab_id,
	AxisAcquireGrabResult    *result,
	gchar                   **problem,
	GError                  **error
);

/* ── A person's own look, and a grab by hand ────────────────────── */

/**
 * AxisAcquireGroup:
 * @AXIS_ACQUIRE_GROUP_ACCEPTABLE: the profile accepts it and nothing stands in
 *   its way
 * @AXIS_ACQUIRE_GROUP_PROFILE: the profile rejects it (its qualities, sizes,
 *   rules, or not wanted over the file on disk)
 * @AXIS_ACQUIRE_GROUP_NOT_ASKED: not what the search asked for (the module's
 *   own rejection: another episode, not a whole season, not wanted now)
 * @AXIS_ACQUIRE_GROUP_RULED_OUT: a fixed rule rules it out (not this title,
 *   a sample, ...), or it is on the blocklist
 *
 * Where a stored release stands, for a manual search's page; a person may
 * take any of the first three by hand.
 */
typedef enum
{
	AXIS_ACQUIRE_GROUP_ACCEPTABLE,
	AXIS_ACQUIRE_GROUP_PROFILE,
	AXIS_ACQUIRE_GROUP_NOT_ASKED,
	AXIS_ACQUIRE_GROUP_RULED_OUT
} AxisAcquireGroup;

#define AXIS_ACQUIRE_N_GROUPS 4

AxisAcquireGroup
axis_acquire_candidate_group(const AxisAcquireCandidate *candidate);

/**
 * axis_acquire_hand_preview:
 * @target: the owner and what the search that found it was for
 * @candidate_id: a release one of the owner's searches considered
 * @preview: (out) (transfer full): what a grab by hand would do
 *
 * Read only: the candidate, the fixed rules and the blocklist (@refused),
 * the module's hand_preview(), and whether the release is better than each
 * file it would replace (by the on-disk order of qualities; a stored "not
 * better than the file on disk" counts as not).  Fails for a database
 * error, or NOT_FOUND (a #G_IO_ERROR) when the owner is gone or the
 * candidate is not one of its searches'.
 */
gboolean
axis_acquire_hand_preview(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
);

/**
 * AxisAcquireReplaceTicks:
 * @grab_id: the grab in flight the confirmation offered to replace (0: no
 *   replace was asked for)
 * @ok: the person ticked "Replace the download in flight with this one"
 * @others: the person ticked that the other parts it holds stop downloading
 *   too (asked only when it holds any the release does not)
 * @digest: (nullable): the offer's digest, as posted
 *
 * What a confirmation that offered to replace the download in flight was
 * posted with (add-replace-grab-and-delete-download, slice A).
 */
typedef struct
{
	gint64       grab_id;
	gboolean     ok;
	gboolean     others;
	const gchar *digest;
} AxisAcquireReplaceTicks;

/**
 * AxisAcquireHandTicks:
 * @replace_better: the person ticked replacing the files the release is not
 *   better than
 * @numbering: the person ticked taking a release numbered differently from
 *   the data
 * @replace: (element-type gint64) (nullable): the library files the person
 *   ticked by name: a file kept by hand is replaced only when it is here,
 *   whether or not the release is better
 * @in_flight: (nullable): the replace of the download in flight, as posted
 *
 * What the confirmation of a grab by hand was ticked with.
 */
typedef struct
{
	gboolean                       replace_better;
	gboolean                       numbering;
	const GArray                  *replace;
	const AxisAcquireReplaceTicks *in_flight;
} AxisAcquireHandTicks;

/**
 * AxisAcquireHandGrabbed:
 * @grab_id: the grab
 * @n_replaced: the files the tick was recorded for (replaces_files)
 * @numbering: the numbering tick was recorded
 * @replaced_grab: the grab in flight that was ended for it (0: none)
 *
 * What a grab by hand recorded, for the audit.
 */
typedef struct
{
	gint64   grab_id;
	guint    n_replaced;
	gboolean numbering;
	guint    n_forced;	/* by force: the fixed rules set aside, as recorded */
	gint64   replaced_grab;
} AxisAcquireHandGrabbed;

/**
 * axis_acquire_grab_by_hand:
 * @ticks: what the confirmation was ticked with
 * @grabbed: (out) (optional): what was recorded
 *
 * Grab this one: axis_acquire_grab() for a target by hand, whatever the
 * profile and the module's own rules say, refused NEEDS_TICK when the
 * release would replace a file it is not better than and that was not
 * ticked, or numbers its parts differently from the data and that was not
 * ticked.  Sent with set_aside "hand", the files the tick covered (each
 * with its quality, as the confirmation showed it) and the numbering tick
 * recorded, its reason saying it was chosen by @user by hand with the
 * profile set aside.  Fails only for a database error.
 */
gboolean
axis_acquire_grab_by_hand(
	AxisAcquire                 *self,
	const AxisAcquireTarget     *target,
	gint64                       candidate_id,
	const gchar                 *user,
	const AxisAcquireHandTicks  *ticks,
	AxisAcquireHandGrabbed      *grabbed,
	AxisAcquireGrabResult       *result,
	gchar                      **problem,
	GError                     **error
);

/* ── A grab by force (add-force-grab-and-assign) ────────────────── */

/**
 * axis_acquire_force_allowed:
 * @candidate: a stored release
 * @kept: (out) (optional) (transfer full) (nullable): when not, the words of
 *   the fixed rules that no one sets aside
 *
 * Whether an administrator may grab @candidate by force.  A fixed rule is
 * of one of two kinds.  Some are about the release's name and say "Axis
 * cannot tell that this is the title": it is not the title asked for
 * ("no_match": another name, another year, another stored title, another
 * title's identifier at the indexer, no episode readable), it could be more
 * than one title ("ambiguous"), no title can be read ("no_title").  A person
 * who has looked can tell, so these may be set aside.  The others protect
 * the system and stay for everyone: a sample, a password-protected release,
 * a name that ends in an archive's or a program's extension, one part of a
 * multi-part post; Axis could not import what they deliver.  A release is
 * allowed only when it has a fixed rule and every one of them is of the
 * first kind.  The blocklist is not asked here: a blocklisted release is
 * refused by the preview and the grab, as for any grab.
 *
 * The one place that decides it: to let another rule be forced (or to stop
 * one), change the list in axis-acquire-hand.c.  Pure.
 */
gboolean
axis_acquire_force_allowed(
	const AxisAcquireCandidate  *candidate,
	gchar                      **kept
);

/**
 * axis_acquire_force_preview:
 * @preview: (out) (transfer full): what a grab by force would do
 *
 * As axis_acquire_hand_preview(), for a release a fixed rule rules out:
 * @refused when it has no fixed rule (Grab this one takes it), when a rule
 * that protects the system stands, when it is on the blocklist, or when a
 * grab is in flight; else what it fills and every file it would replace,
 * and @digest.  Read only.
 */
gboolean
axis_acquire_force_preview(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	gint64                    candidate_id,
	AxisAcquireHandPreview  **preview,
	GError                  **error
);

/**
 * AxisAcquireForceTicks:
 * @checked: the person ticked having checked that the release is what they
 *   say it is
 * @replace: (element-type gint64) (nullable): the library files the person
 *   ticked replacing
 * @digest: (nullable): the confirmation's digest, as posted
 * @one_file: the person ticked that one file holds the several parts they
 *   named (a series: one video for the episodes picked); recorded with the
 *   grab as a choice, for its import
 * @in_flight: (nullable): the replace of the download in flight, as posted
 *
 * What the confirmation of a grab by force was posted with.
 */
typedef struct
{
	gboolean                       checked;
	const GArray                  *replace;
	const gchar                   *digest;
	gboolean                       one_file;
	const AxisAcquireReplaceTicks *in_flight;
} AxisAcquireForceTicks;

/**
 * axis_acquire_grab_by_force:
 * @ticks: what the confirmation was posted with
 * @grabbed: (out) (optional): what was recorded
 *
 * Grab it by force: the checks of axis_acquire_grab() with the fixed rules
 * asked through axis_acquire_force_allowed() (REFUSED when one that protects
 * the system stands, or when the release has no fixed rule), then the
 * blocklist, in flight, the digest (CHANGED when what the confirmation
 * showed is not what is so now), the ticks (NEEDS_TICK without @checked, or
 * when a file it would replace was not ticked: every one needs its tick,
 * whether or not the release is better), the destination, the release still
 * known, a usable client.  Sent as a grab by hand (set_aside "hand") that
 * is bound, with the rules set aside (forced) and every ticked file
 * (replaces_files) recorded.  Fails only for a database error.
 */
gboolean
axis_acquire_grab_by_force(
	AxisAcquire                  *self,
	const AxisAcquireTarget      *target,
	gint64                        candidate_id,
	const gchar                  *user,
	const AxisAcquireForceTicks  *ticks,
	AxisAcquireHandGrabbed       *grabbed,
	AxisAcquireGrabResult        *result,
	gchar                       **problem,
	GError                      **error
);

/* ── Another profile, tried on what a search found ──────────────── */

/**
 * AxisAcquireProfileTry:
 * @profile_id: the profile
 * @usable: it could be read and its definition validates
 * @accepted: (element-type gboolean): one per release asked about, in that
 *   order: the profile accepts it and nothing else stands in its way (the
 *   blocklist, a fixed rule, what the module wants of it now)
 * @n_accepted: how many it accepts
 * @reason: (nullable): why the first release it does not accept is not
 *   accepted under it, a sentence
 */
typedef struct
{
	gint64    profile_id;
	gboolean  usable;
	GArray   *accepted;
	guint     n_accepted;
	gchar    *reason;
} AxisAcquireProfileTry;

void
axis_acquire_profile_try_free(AxisAcquireProfileTry *attempt);

/**
 * axis_acquire_try_profiles:
 * @target: the owner (and what the module loads for it), as its search was
 * @candidates: (element-type AxisAcquireCandidate): releases of the owner's
 *   searches, as stored
 * @profile_ids: (element-type gint64): the profiles to try
 * @tries: (out) (element-type AxisAcquireProfileTry): one per profile, in
 *   that order
 *
 * What each profile would make of each release now: the decision a search
 * makes (axis_quality_decide_one() with the same title, file on disk,
 * languages, protocol, retention and players), after the module's judge()
 * has read the release against what the owner wants now.  Nothing is
 * written, and no indexer or provider is asked.  Fails only for a database
 * error, or NOT_FOUND (a #G_IO_ERROR) when the owner is gone.
 */
gboolean
axis_acquire_try_profiles(
	AxisAcquire              *self,
	const AxisAcquireTarget  *target,
	const GPtrArray          *candidates,
	const GArray             *profile_ids,
	GPtrArray               **tries,
	GError                  **error
);

/* ── Following grabs ────────────────────────────────────────────── */

/**
 * AxisAcquireFailure:
 * @close_reason: the grab's close_reason
 * @block_reason: (nullable): the blocklist's reason, %NULL: none
 * @text: why, in words
 * @remove: ask the client to remove it with its data
 * @retry: ask the module for the next best (retry())
 * @keep_data: with @remove, ask the client to remove it without its data
 *   (its files are gone already, or are not Axis's to delete)
 * @retry_if_monitored: with @retry, only when the owner is monitored (the
 *   module's monitored(); an owner that is not is wanted again, and
 *   searched for when a person asks)
 * @still_downloading: take the grab only while its download has not
 *   completed and has no import, looked at under the owner's lock (a
 *   replace: a download that has arrived is its import's)
 * @judge_place: with @remove, ask the client only when the place its files
 *   are at now may be deleted (axis_acquire_removal_place()); the follow-up
 *   judges the same way each time it asks again (a grab closed "replaced")
 */
typedef struct
{
	const gchar *close_reason;
	const gchar *block_reason;
	gchar       *text;
	gboolean     remove;
	gboolean     retry;
	gboolean     keep_data;
	gboolean     retry_if_monitored;
	gboolean     still_downloading;
	gboolean     judge_place;
} AxisAcquireFailure;

/**
 * AxisAcquirePlace:
 * @AXIS_ACQUIRE_PLACE_WITH_DATA: the client may be asked to remove the
 *   download with its data
 * @AXIS_ACQUIRE_PLACE_WITHOUT_DATA: its client does not list it now: it may
 *   be asked to remove it without its data, which deletes nothing
 * @AXIS_ACQUIRE_PLACE_REFUSED: the client is not asked: where it says the
 *   files are now is not a place Axis lets it delete (or cannot be judged)
 * @AXIS_ACQUIRE_PLACE_UNKNOWN: the client could not be asked where the
 *   download is, or does not list it while part of its list could not be
 *   read (it may be there all the same): nothing is asked of it now
 */
typedef enum
{
	AXIS_ACQUIRE_PLACE_WITH_DATA,
	AXIS_ACQUIRE_PLACE_WITHOUT_DATA,
	AXIS_ACQUIRE_PLACE_REFUSED,
	AXIS_ACQUIRE_PLACE_UNKNOWN
} AxisAcquirePlace;

/**
 * axis_acquire_place_judge:
 * @place: (nullable): where the client says a torrent's files are now (its
 *   own path)
 * @mapped: whether @place maps to a volume through the client's path
 *   mappings; then @rel (the path on it) and @roots
 * @roots: (element-type utf8) (nullable): the library roots of that volume,
 *   as pairs: each root's path on the volume, then "d" for a downloads root
 *   and "" for any other
 * @why: (out) (transfer full) (nullable): when refused, the reason, to follow
 *   "Axis does not ask the client to delete it: "
 *
 * The rule for asking a torrent's client to delete a torrent wherever it is
 * now.  Refused when the client names no place, or one that is not a plain
 * full path; when no path mapping of the client reaches the place (Axis then
 * cannot tell that it is not a library folder: a client often sees the
 * library under a path of its own); when the place is a volume's top, is a
 * library root, is inside one (a downloads root aside, which is where
 * downloads are) or holds one.  Pure.
 *
 * Returns: %TRUE when the client may be asked
 */
gboolean
axis_acquire_place_judge(
	const gchar      *place,
	gboolean          mapped,
	const gchar      *rel,
	const GPtrArray  *roots,
	gchar           **why
);

/**
 * axis_acquire_place_in_library:
 *
 * The positive half of axis_acquire_place_judge(), with its parameters: %TRUE
 * only when the place is known to be one a client must not delete at: it
 * maps to a volume and is the volume's top, a library root, inside one (a
 * downloads root aside) or holds one.  A place that is not named, is not a
 * plain full path or is reached by no mapping is %FALSE here (not known),
 * though axis_acquire_place_judge() refuses it.  Pure.
 */
gboolean
axis_acquire_place_in_library(
	const gchar      *place,
	gboolean          mapped,
	const gchar      *rel,
	const GPtrArray  *roots,
	gchar           **why
);

/**
 * axis_acquire_removal_place:
 * @download_id: a download whose client is about to be asked to remove it
 *   with its data
 * @place: (out): whether to ask
 * @why: (out) (transfer full) (nullable): for REFUSED and UNKNOWN, why, a
 *   sentence
 *
 * Looks, now, at where the download is.  A Usenet job is its client's to
 * delete: an unfinished job's files are in the client's own temporary
 * folder, which no setting of a single job moves, and what the client keeps
 * of a completed one is deleted by the import module's cleanup under its
 * folder rules.  A torrent's client deletes a torrent wherever it is, and a
 * person can move one (into a library folder): the client is asked what it
 * lists, and the place it gives is judged by axis_acquire_place_judge().
 * Read only.  Fails only for a database error.
 */
gboolean
axis_acquire_removal_place(
	AxisAcquire       *self,
	gint64             download_id,
	AxisAcquirePlace  *place,
	gchar            **why,
	GError           **error
);

/* axis_acquire_removal_place(), telling apart why a place is refused.
 * *@in_library: (optional) with REFUSED, the place is known to be one a
 * client must not delete at (axis_acquire_place_in_library()); %FALSE when it
 * is refused only because it cannot be judged (no place, no mapping). */
gboolean
axis_acquire_removal_place_full(
	AxisAcquire       *self,
	gint64             download_id,
	AxisAcquirePlace  *place,
	gboolean          *in_library,
	gchar            **why,
	GError           **error
);

/**
 * axis_acquire_handle_failure:
 * @grab_id: the open grab
 * @user: (nullable): who, for "give up"
 * @completed_too: also a completed grab ("give up"); else only one still
 *   open and not completed (the follow-up)
 * @handled: (out): %FALSE when the grab was dealt with already
 * @report: (nullable)
 *
 * In one transaction with the owner's row locked: the release on the
 * blocklist, the grab closed, a waiting or blocked import of it dismissed,
 * the history written.  Then the download's removal and, when nothing of it
 * is at the client, retry().
 */
gboolean
axis_acquire_handle_failure(
	AxisAcquire               *self,
	gint64                     owner_id,
	gint64                     grab_id,
	const AxisAcquireFailure  *failure,
	const gchar               *user,
	gboolean                   completed_too,
	gboolean                  *handled,
	AxisAcquireFollowReport   *report,
	GError                   **error
);

/**
 * axis_acquire_run_follow:
 *
 * The follow-up's work (every minute): searches that never finished marked
 * failed, claims never handed over settled (the download sent for one
 * adopted, else closed), failed, stalled, gone and removed downloads dealt
 * with, failed removals tried again, completions recorded, follow_more(),
 * then old searches pruned.  It works from facts alone: a restart or two
 * runs at once change nothing.
 */
gboolean
axis_acquire_run_follow(
	AxisAcquire              *self,
	AxisAcquireFollowReport  *report,
	GError                  **error
);

/* ── Pacing ─────────────────────────────────────────────────────── */

/**
 * axis_acquire_pace:
 * @ids: (element-type gint64): rows of the pace table
 * @outcome: the search's outcome
 *
 * After a search: "grabbed" sets the floor back to its base and waits it;
 * "nothing_found", "none_acceptable" and "exhausted" wait the floor and
 * double it (up to its maximum); "in_flight" and "failed" change nothing;
 * anything else (nothing could be asked) waits the floor without doubling.
 */
gboolean
axis_acquire_pace(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	GArray       *ids,
	const gchar  *outcome,
	GError      **error
);

/* ── The blocklist and the stored searches ──────────────────────── */

/* The reason of an entry a person made (add-block-release): Block this
 * release on a manual search, Block and search again on a download. */
#define AXIS_ACQUIRE_BLOCK_PERSON "blocked"

/* The keys an owner's blocklist matches by, with why each entry is there. */
typedef struct
{
	gchar *key;
	gchar *name_key;
	gchar *info_hash;
	gchar *reason;
	gchar *created_by;	/* (nullable) */
} AxisAcquireBlockKey;

void
axis_acquire_block_key_free(AxisAcquireBlockKey *key);

gboolean
axis_acquire_store_block_keys(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	GPtrArray   **keys,
	GError      **error
);

/**
 * axis_acquire_blocked_by:
 *
 * The entry of the blocklist @keys that a release with @name_key and
 * @info_hash (nullable) is on, %NULL when it is on none.  An entry matches
 * by its info-hash, or by its name key ("<source>:<indexer>:<name>": the same
 * name from the same indexer).  An entry a person made
 * (%AXIS_ACQUIRE_BLOCK_PERSON) also matches the same folded name from any
 * indexer: a person blocks the release, not one indexer's copy of it.  An
 * entry a person made is returned before a failure's.
 *
 * Returns: (transfer none) (nullable)
 */
const AxisAcquireBlockKey *
axis_acquire_blocked_by(
	const GPtrArray *keys,
	const gchar     *name_key,
	const gchar     *info_hash
);

/* Whether axis_acquire_blocked_by() finds an entry. */
gboolean
axis_acquire_blocked(
	const GPtrArray *keys,
	const gchar     *name_key,
	const gchar     *info_hash
);

/* Whether an entry with @reason was made by a person ("blocked",
 * "given_up") and not by a failure. */
gboolean
axis_acquire_block_by_person(const gchar *reason);

/* Why a release is on the blocklist, to follow "It is on the blocklist: ":
 * "it failed before", "blocked by <person>", "<person> gave up on its
 * download". */
gchar *
axis_acquire_block_words(
	const gchar *reason,
	const gchar *created_by
);

/**
 * axis_acquire_candidates_mark_blocked:
 * @candidates: (element-type AxisAcquireCandidate): stored releases of
 *   @owner_id's searches
 *
 * For a page: sets each release's blocklisted, blocked_by_person and
 * blocked_by from the blocklist as it is now (a stored release keeps what
 * its search saw), with its "blocklist" reason rewritten to match.  A
 * release taken off the blocklist since is shown by its other reasons.
 */
gboolean
axis_acquire_candidates_mark_blocked(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	GPtrArray    *candidates,
	GError      **error
);

/**
 * axis_acquire_block_candidate:
 * @candidate_id: a release one of the owner's searches considered
 * @name: (out) (optional) (transfer full): the release's name
 *
 * Block this release: in one transaction with the owner's row locked, the
 * release is put on the owner's blocklist with the reason
 * %AXIS_ACQUIRE_BLOCK_PERSON by @user, and the history says so.  Nothing is
 * asked of a client, and no file is touched.  A release already on the
 * blocklist by the same key stays as it was.  Fails for a database error,
 * or NOT_FOUND (a #G_IO_ERROR) when the owner is gone or the candidate is
 * not one of its searches'.
 */
gboolean
axis_acquire_block_candidate(
	AxisAcquire  *self,
	gint64        owner_id,
	gint64        candidate_id,
	const gchar  *user,
	gchar       **name,
	GError      **error
);

/**
 * AxisAcquireBlockGrab:
 * @owner_id, @grab_id: the grab
 * @release_name: its release
 * @indexer: (nullable)
 * @open: the grab is open: its download is at a client, or waits for its
 *   import; blocking it gives up on it (axis_acquire_handle_failure())
 * @at_client: with @open, the client is asked to remove the download
 * @close_reason: (nullable): how a closed grab ended ("imported", ...)
 * @blocked: the release is on the owner's blocklist already
 * @searches: with @open, giving up on it is followed by a search for the next
 *   best (the owner is monitored, and the grab was not chosen by hand)
 *
 * A grab, for Block and search again.
 */
typedef struct
{
	gint64    owner_id;
	gint64    grab_id;
	gchar    *release_name;
	gchar    *indexer;
	gboolean  open;
	gboolean  at_client;
	gchar    *close_reason;
	gboolean  blocked;
	gboolean  searches;
} AxisAcquireBlockGrab;

void
axis_acquire_block_grab_free(AxisAcquireBlockGrab *grab);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisAcquireBlockGrab, axis_acquire_block_grab_free)

/**
 * axis_acquire_block_grab_find:
 * @grab_id, @download_id, @import_id: the first that is not 0 names the
 *   grab: itself, the download it sent, or an import of that download
 * @grab: (out) (transfer full) (nullable): %NULL when there is none
 *
 * Read only.
 */
gboolean
axis_acquire_block_grab_find(
	AxisAcquire            *self,
	gint64                  grab_id,
	gint64                  download_id,
	gint64                  import_id,
	AxisAcquireBlockGrab  **grab,
	GError                **error
);

/**
 * axis_acquire_block_grab:
 * @cancelled: (out): the grab was open, and was given up on: closed, its
 *   download removed from its client, the next best looked for (%FALSE: it
 *   was closed already, and the release was only put on the blocklist)
 *
 * Block and search again.  An open grab goes through the failure path
 * (axis_acquire_handle_failure(), as "Give up on this download" does) with
 * the blocklist's reason %AXIS_ACQUIRE_BLOCK_PERSON.  A closed one (its
 * release is in the library, or it ended some other way) is only put on the
 * blocklist, with the history written: no client is asked, and no file,
 * link or import is touched.  Fails for a database error, or NOT_FOUND (a
 * #G_IO_ERROR) when the grab is not the owner's.
 */
gboolean
axis_acquire_block_grab(
	AxisAcquire  *self,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *cancelled,
	GError      **error
);

/**
 * axis_acquire_grab_deleted:
 * @conn: the caller's open transaction (the import's, which closes the
 *   import in it)
 * @grab_id: the grab that brought the download, or 0 for none
 * @release: (nullable): the release's name, for the history of a download
 *   that no grab brought
 * @user: who deleted it
 * @block: the person also ticked putting the release on the blocklist
 *
 * A person deleted a download from its import's page ("Delete this
 * download"): the grab, when it is still open, is closed with the reason
 * "deleted" and no removal of its own (the import module asks the client,
 * after judging the download's place; the follow-up's own removals never
 * take a grab closed so), the owner's history says who deleted it (event
 * kind "removal"), and with @block the release is on the owner's blocklist
 * as blocked by that person (%AXIS_ACQUIRE_BLOCK_PERSON).  No search is
 * queued: the title is wanted again under the ordinary rules.
 */
gboolean
axis_acquire_grab_deleted(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        grab_id,
	const gchar  *release,
	const gchar  *user,
	gboolean      block,
	GError      **error
);

/**
 * axis_acquire_store_block:
 *
 * Puts a release on the owner's blocklist (key: the hash when known, else
 * the name key), once: a second time changes nothing.
 */
gboolean
axis_acquire_store_block(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	const gchar  *name_key,
	const gchar  *info_hash,
	const gchar  *release_name,
	const gchar  *indexer,
	const gchar  *reason,
	const gchar  *reason_text,
	gint64        grab_id,
	const gchar  *user,
	GError      **error
);

/**
 * axis_acquire_store_block_written:
 * @written: (out) (nullable): an entry was added or changed (%FALSE: it was
 *   there already, as it is, and nothing was written)
 *
 * As axis_acquire_store_block().  A person's block that takes the place of a
 * failure's entry keeps that entry's reason after its own ("Before: ...").
 */
gboolean
axis_acquire_store_block_written(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	const gchar  *name_key,
	const gchar  *info_hash,
	const gchar  *release_name,
	const gchar  *indexer,
	const gchar  *reason,
	const gchar  *reason_text,
	gint64        grab_id,
	const gchar  *user,
	gboolean     *written,
	GError      **error
);

/* Removes one entry (@entry_id) or all of an owner's (0); *@names: the
 * release names removed (element-type utf8). */
gboolean
axis_acquire_store_unblock(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	gint64        entry_id,
	GPtrArray   **names,
	GError      **error
);

/* The owner's blocklist (AxisAcquireBlock), the latest first. */
gboolean
axis_acquire_store_blocklist(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	GPtrArray   **blocks,
	GError      **error
);

/* Locks the owner's row in the open transaction; *@found %FALSE when there
 * is none. */
gboolean
axis_acquire_store_lock(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        owner_id,
	gboolean     *found,
	GError      **error
);

/* A stored search by its id (*@search %NULL when there is none), or the
 * owner's latest (@search_id 0). */
gboolean
axis_acquire_store_search(
	AxisAcquire            *self,
	AxisDbConn             *conn,
	gint64                  owner_id,
	gint64                  search_id,
	AxisAcquireSearchInfo **search,
	GError                **error
);

/* Every candidate of a search (AxisAcquireCandidate), by position. */
gboolean
axis_acquire_store_candidates(
	AxisAcquire  *self,
	AxisDbConn   *conn,
	gint64        search_id,
	GPtrArray   **candidates,
	GError      **error
);

/* A candidate by its id, if it is one of the owner's searches'. */
gboolean
axis_acquire_store_candidate(
	AxisAcquire           *self,
	AxisDbConn            *conn,
	gint64                 owner_id,
	gint64                 candidate_id,
	AxisAcquireCandidate **candidate,
	GError               **error
);

/* ── For tests ──────────────────────────────────────────────────── */

/* Called (on the search's thread) just before the claim transaction. */
typedef void (*AxisAcquireClaimHook)(gint64 owner_id, gpointer user_data);

void
axis_acquire_set_claim_hook(
	AxisAcquire          *self,
	AxisAcquireClaimHook  hook,
	gpointer              user_data
);

/* Called (on the grab's thread) in a replace of the download in flight,
 * after the look at where the download is and before its grab is ended: the
 * window in which a client can stop answering or a torrent be moved. */
void
axis_acquire_set_replace_hook(
	AxisAcquire          *self,
	AxisAcquireClaimHook  hook,
	gpointer              user_data
);

G_END_DECLS

#endif /* AXIS_ACQUIRE_H */
