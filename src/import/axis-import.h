/*
 * axis-import.h - the import pipeline: a completed download (or a folder an
 * administrator names) taken into the library for the title it is for
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This is the first code that writes into a library.  Its promises:
 *
 *  - Nothing is written while "storage.read_only" is on (the default), or
 *    while the destination volume is read-only or offline: not a folder, not
 *    a temporary file.  The import waits, and says which of these is in the
 *    way.
 *  - Every path is reached through a volume guard (axis-volume-guard.h), and
 *    every change through axis-volume-write.h, the whole of the write
 *    surface.  A symbolic link in a download is never followed, and a
 *    download whose location is not beneath a known volume is refused.
 *  - A file is placed under a temporary dot-name in its destination folder
 *    (".axis-partial-<n>", which both media servers ignore), and then gets its
 *    final name by a rename that never replaces one.  With import.take_files
 *    "move" (the default) a download's file is moved there: by one rename
 *    when the download is on the destination's volume, else by a copy that
 *    is fsynced and read back, its size and SHA-256 compared, the download's
 *    file being deleted once the import is recorded; a torrent's only after
 *    its client has let go of it.  A torrent left seeding, a manual import of
 *    a folder, and every import with import.take_files "hardlink" leave the
 *    download's files where they are: a hardlink on the same volume (the
 *    file untouched, so a torrent keeps seeding), else the verified copy.
 *    How each file was placed, and why, is recorded.  A setting forbids the
 *    copy.
 *  - A failure at any step removes the temporary file (and a folder made for
 *    it, when empty) and leaves the library as it was.
 *  - An upgrade places the new file and verifies it first; only then is the
 *    old file, with its subtitles, renamed into the recycle folder at the
 *    top of its own volume.  When the new file has the old one's name, the
 *    old one is first hardlinked into the recycle folder and the two names
 *    are swapped atomically.  If retiring the old file fails, both stay and
 *    that is recorded.  Recycled files are removed only by the cleanup job,
 *    after import.recycle_retention, and only the ones its table records.
 *  - Whose download it is comes from the grab; the file's name and the
 *    probe only confirm it or block the import.  Every file found gets its
 *    decision with every reason, and an import that cannot go on is blocked
 *    with its reasons and the actions that resolve it (administrators only,
 *    each recorded with the user).  "Import anyway" can override what the
 *    name, the runtime and the quality say, never a volume that is offline
 *    or read-only, a failed verification, or a path outside a volume.
 *  - A job killed at any point and run again completes once and records
 *    once: what is on disk and what import_file recorded tell it where it
 *    was.  A job told to stop (the daemon stopping) while it places a file
 *    is not a failure: a copy half made is removed, nothing is counted
 *    against the import, and the job is given back to run again.
 *
 * The core knows nothing of films: a module registers itself as an owner
 * (AxisImportOwnerInfo) and answers for its titles (who a download is for,
 * what they are called, linking the file).  An owner that answers match()
 * (the series) has every video file of a download imported for itself,
 * each with the same discipline; one that fails leaves the others in place,
 * and the import ends "partial" (partly imported) with the reason on that
 * file and "retry" to take it up again.
 *
 * Threads: everything that touches the database or a filesystem blocks:
 * call it from a worker thread.  Create, register and free on the main
 * thread.
 */

#ifndef AXIS_IMPORT_H
#define AXIS_IMPORT_H

#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "download/axis-download.h"
#include "import/axis-import-name.h"
#include "jobs/axis-job-context.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality.h"
#include "release/axis-release.h"
#include "storage/axis-storage.h"

G_BEGIN_DECLS

#define AXIS_IMPORT_MODULE_ID           "import"

/* One job per import (de-duplicated by AXIS_IMPORT_KEY_PREFIX and its id),
 * on the downloads queue; the cleanup of the recycle folders and of
 * temporary files left behind, every hour, on the maintenance queue. */
#define AXIS_IMPORT_KIND                "import.run"
#define AXIS_IMPORT_KEY_PREFIX          "import:"
#define AXIS_IMPORT_CLEANUP_KIND        "import.cleanup"
#define AXIS_IMPORT_CLEANUP_SCHEDULE    "import.cleanup"
#define AXIS_IMPORT_CLEANUP_INTERVAL_S  3600

#define AXIS_IMPORT_SETTING_RENAME      "import.rename"
#define AXIS_IMPORT_SETTING_LAYOUT      "import.layout"
#define AXIS_IMPORT_SETTING_FORMAT      "import.file_format"
#define AXIS_IMPORT_SETTING_HARDLINK    "import.hardlink_policy"
#define AXIS_IMPORT_SETTING_RETENTION   "import.recycle_retention"
#define AXIS_IMPORT_SETTING_DIR_MODE    "import.directory_mode"
#define AXIS_IMPORT_SETTING_FREE_FLOOR  "import.free_space_floor"
#define AXIS_IMPORT_SETTING_SUBTITLES   "import.subtitles"
#define AXIS_IMPORT_SETTING_EXTRAS      "import.extras"
#define AXIS_IMPORT_SETTING_RUNTIME     "import.runtime_tolerance"
#define AXIS_IMPORT_SETTING_TORRENTS    "import.torrents_after"
#define AXIS_IMPORT_SETTING_TAKE        "import.take_files"

/* What happens to a torrent once every wanted file of it is in the library:
 * "remove" (the default; its client lets go of it, and its files are moved
 * into the library, or, with import.take_files "hardlink", its client removes
 * it with its downloaded files) or "seed" (nothing is asked of the client,
 * and its files are hardlinked). */
#define AXIS_IMPORT_TORRENTS_REMOVE     "remove"
#define AXIS_IMPORT_TORRENTS_SEED       "seed"

/* How a download's files get into the library: "move" (the default: a Usenet
 * download's files are moved, a torrent's too once its client has let go of
 * it, unless torrents are left seeding) or "hardlink" (every file hardlinked,
 * or copied across volumes, and left where the client put it, as before). */
#define AXIS_IMPORT_TAKE_MOVE           "move"
#define AXIS_IMPORT_TAKE_HARDLINK       "hardlink"

/* Said where it is chosen: the import settings, the Download clients page and
 * the setup guide's download-client step. */
#define AXIS_IMPORT_TORRENTS_HELP \
	"Axis is not a seedbox, so by default a torrent is removed from its client as soon as every wanted file of " \
	"it is in the library: its client is asked to let go of it (without its files), and its files are then " \
	"moved into the library, so nothing of it is left in the client's folder. A private tracker that requires " \
	"a ratio needs the torrent left seeding: set \"After a torrent is imported\" in Settings, under Imports, to " \
	"seed (its files then stay in the client's folder and the library gets hardlinks of them, which take no " \
	"extra space)."

/* The walk of a download: how deep, and how many entries in all. */
#define AXIS_IMPORT_WALK_DEPTH          4
#define AXIS_IMPORT_WALK_ENTRIES        2000
/* At most this many files are kept with their decisions, and probed. */
#define AXIS_IMPORT_MAX_FILES           200
#define AXIS_IMPORT_MAX_PROBES          20

/* A probe that fails is tried again this many times, then a person asks. */
#define AXIS_IMPORT_PROBE_ATTEMPTS      3
/* A placement that fails is tried again this many times (then blocked). */
#define AXIS_IMPORT_PLACE_ATTEMPTS      3

#define AXIS_IMPORT_ERROR (axis_import_error_quark())

/**
 * AxisImportError:
 * @AXIS_IMPORT_ERROR_NOT_FOUND: no such import (or file of it, or title)
 * @AXIS_IMPORT_ERROR_INVALID: the request cannot be made (the message says
 *   why)
 * @AXIS_IMPORT_ERROR_REFUSED: the action is not allowed for this import (an
 *   objection "import anyway" cannot override, an import that is not
 *   blocked)
 * @AXIS_IMPORT_ERROR_BUSY: another import of the download or the title is
 *   open
 * @AXIS_IMPORT_ERROR_IN_HAND: nothing new to do: what was asked for is
 *   queued already (Look again on an import waiting for its turn); nothing
 *   was written
 * @AXIS_IMPORT_ERROR_CHANGED: what a confirmation showed is not so any more
 *   ("Delete this download": the import, the download or its place changed
 *   since the page was made); nothing was written
 */
typedef enum
{
	AXIS_IMPORT_ERROR_NOT_FOUND,
	AXIS_IMPORT_ERROR_INVALID,
	AXIS_IMPORT_ERROR_REFUSED,
	AXIS_IMPORT_ERROR_BUSY,
	AXIS_IMPORT_ERROR_IN_HAND,
	AXIS_IMPORT_ERROR_CHANGED
} AxisImportError;

GQuark
axis_import_error_quark(void);

/* ── What an owner answers ──────────────────────────────────────── */

/**
 * AxisImportExisting:
 *
 * A file of the library the title has now (an upgrade replaces it).
 * @root_path is its root's directory on the volume (storage form) and
 * @rel_path its path in the root (stored form).  @resolution: for a file of
 * unknown quality found in the library, the resolution known for it (an
 * unknown source at that resolution: see axis_quality_unknown_source_label()).
 */
typedef struct
{
	gint64                 library_file_id;
	gint64                 root_id;
	gint64                 volume_id;
	gchar                 *root_path;
	gchar                 *rel_path;
	gint64                 inode;
	gint64                 size;
	AxisQuality            quality;
	gchar                 *release_name;
	AxisReleaseResolution  resolution;
	gboolean               kept;	/* kept by hand: replaced only by a person's own grab */
	/* (nullable) (static) With @kept: it is kept because it was brought over
	 * from another app, by no person's choice: that app's name ("Radarr"). */
	const gchar           *kept_from;
} AxisImportExisting;

void
axis_import_existing_free(AxisImportExisting *existing);

/**
 * AxisImportTarget:
 * @owner_id: the owner's record of the title (a film)
 * @grab_id: the grab the download came from, 0 for none
 * @title_id: its metadata title, 0 for none
 * @label: "Hearth (1995)"
 * @title, @original_title, @year: for its names and the checks
 * @tmdb_id, @imdb_id: (nullable): for its folder's name
 * @runtime_minutes: 0 when not known
 * @grab_release: (nullable): the release name that was grabbed
 * @grab_set_aside: a person set the profile aside for this grab (Grab this
 *   one, Grab it anyway on a rejected release): its objections (qualities,
 *   sizes, rules, whether it upgrades and its cutoff) are set aside, for a
 *   film's file and for an episode file whose episodes the grab claimed
 *   (the unit's @by_hand)
 * @grab_ticked: (element-type gint64) (nullable): the library files the
 *   person ticked replacing although the release is not better than them:
 *   "not better than the file on disk" is set aside against these alone
 * @grab_numbering_ok: the person ticked taking a release numbered otherwise
 *   than the data (the owner's match() reads it)
 * @grab_bound: the grab was made by force and is bound to this title (the
 *   grab's "bound"): the title in the file's name is not judged again
 *   (name_matches() is not asked, "wrong_name" is never raised), and a file
 *   in the library is replaced only when it is in @grab_ticked, whether or
 *   not the new file is better.  Every other check stands (a readable
 *   video, the runtime, samples, archives, the volume guards).
 * @grab_by: (nullable): who made the grab, for the link of a bound grab
 *   (marked as made by that person, by force)
 * @root_id: where it goes (0: it has no root)
 * @profile: its quality profile, when @profile_ok
 * @languages: (nullable): the languages wanted, for the profile
 * @existing: (element-type AxisImportExisting): its files in the library
 * @owner_data: (nullable): the owner's own, for its callbacks
 * @owner_free: (nullable): frees @owner_data
 */
typedef struct
{
	gint64              owner_id;
	gint64              grab_id;
	gint64              title_id;
	gchar              *label;
	gchar              *title;
	gchar              *original_title;
	gint                year;
	gchar              *tmdb_id;
	gchar              *imdb_id;
	gchar              *tvdb_id;	/* (nullable) a series': its folder's */
	gint                runtime_minutes;
	gchar              *grab_release;
	gboolean            grab_set_aside;
	GArray             *grab_ticked;
	gboolean            grab_numbering_ok;
	gboolean            grab_bound;
	gchar              *grab_by;
	/* For match() of a bound grab, set by the core while it decides file by
	 * file: how many video files of the download are candidates (samples and
	 * what is no video left out).  A bound grab's one video file is what the
	 * person named, whatever its name says; several are read by their own
	 * names. */
	guint               grab_videos;
	/* Also set by the core: (element-type utf8) (nullable) the names of the
	 * candidate video files, for a match() that must read what the other
	 * files of the download state.  Not owned. */
	const GPtrArray    *grab_names;
	/* (nullable) Why the download is held for a person although the title is
	 * known (a series: it was grabbed under another numbering than the series
	 * follows now).  The import is blocked with it, unless a person assigned
	 * its files by hand: they then named the episodes as they are now. */
	gchar              *hold;
	/* (nullable) What a review of an assignment by hand must also find
	 * unchanged when it is applied (a series: the numbering it follows). */
	gchar              *stamp;
	/* Set by the core for a file a person assigned by hand (never by an
	 * owner): it is taken as a bound grab's file is (@grab_bound,
	 * @grab_set_aside, @grab_ticked the files ticked on the review), and a
	 * library file that was not ticked is never replaced, whatever was
	 * overridden for the import. */
	gboolean            assigned;
	gint64              root_id;
	AxisQualityProfile  profile;
	gboolean            profile_ok;
	gchar             **languages;
	GPtrArray          *existing;
	gpointer            owner_data;
	GDestroyNotify      owner_free;
} AxisImportTarget;

AxisImportTarget *
axis_import_target_new(void);

/* The library file ids of "12,40" (others left out), for @grab_ticked; an
 * empty array for %NULL. */
GArray *
axis_import_file_ids(const gchar *list);

void
axis_import_target_free(AxisImportTarget *target);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImportTarget, axis_import_target_free)

/**
 * AxisImportLinked:
 *
 * What the owner records when its title's file is in the library, inside
 * the import's own transaction.
 */
/**
 * AxisImportUnit:
 *
 * What one video file of a download holds, as an owner that imports file
 * by file (match()) reads it: one episode of a series, or several (a double
 * episode).  The core names and places the file from it, decides its
 * quality against @existing, and hands it back to link().
 *
 * @season: the season's number (0: specials)
 * @episodes: the episodes' numbers in the season, @n_episodes of them
 * @label: "S01E02", "S01E02-E03": for the pages and the history
 * @episode_title: (nullable): the first episode's title, for the name
 * @air_date: (nullable): "YYYY-MM-DD", for the name's airdate token
 * @runtime_minutes: what the file should run (their sum), 0 not known
 * @existing: (element-type AxisImportExisting): the files of these
 *   episodes in the library now (an upgrade replaces them)
 * @by_hand: every episode it holds is one a grab with the profile set aside
 *   claimed (the target's @grab_set_aside): only then is the profile set
 *   aside for this file
 * @note: (nullable): what its record says besides ("its name also says
 *   S01E06, which the series' data does not have")
 * @owner_data: (nullable): the owner's own (its episodes' ids)
 * @owner_free: (nullable): frees @owner_data
 */
typedef struct
{
	gint            season;
	gint            episodes[AXIS_IMPORT_EPISODES_MAX];
	guint           n_episodes;
	gchar          *label;
	gchar          *episode_title;
	gchar          *air_date;
	gint            runtime_minutes;
	GPtrArray      *existing;
	gboolean        by_hand;
	gchar          *note;
	gpointer        owner_data;
	GDestroyNotify  owner_free;
} AxisImportUnit;

AxisImportUnit *
axis_import_unit_new(void);

void
axis_import_unit_free(AxisImportUnit *unit);

/* Whether two units hold an episode in common. */
gboolean
axis_import_unit_overlaps(
	const AxisImportUnit *a,
	const AxisImportUnit *b
);

/**
 * AxisImportLinked:
 * @unit: (nullable): for an owner that imports file by file, what this file
 *   holds (%NULL for a film)
 */
typedef struct
{
	gint64                import_id;
	gint64                library_file_id;
	AxisQuality           quality;
	const gchar          *quality_text;
	const gchar          *release_name;
	const gchar          *mode;
	gboolean              upgrade;
	const gchar          *user;
	const AxisImportUnit *unit;
	/* (nullable) "assigned": a person assigned this file by hand, @hand_by:
	 * the link is theirs (hand_how, hand_by, hand_at) and kept by hand. */
	const gchar          *hand_how;
	const gchar          *hand_by;
} AxisImportLinked;

/* One title a person can choose, for a manual import or "assign to another". */
typedef struct
{
	gint64  owner_id;
	gchar  *label;
} AxisImportChoice;

void
axis_import_choice_free(AxisImportChoice *choice);

/**
 * AxisImportOwnerInfo:
 * @module: the module's id ("movies"), as import.module stores it
 * @noun: what its titles are ("film")
 * @resolve: who the import is for: by the grab of @download_id when
 *   @owner_id is 0, or the title @owner_id a person chose (then
 *   @download_id may be 0).  %TRUE with *@target %NULL and *@problem when
 *   there is no such title or grab; %FALSE only for a database error.
 * @name_matches: whether a name the release parser read is the title
 *   (%FALSE with *@reason when it reads as another)
 * @link: in the import's transaction: the file is the title's now (and
 *   the grab, if any, is closed)
 * @note: in the caller's transaction: the title's history gains an event
 *   ("imported", "upgraded", "import_blocked", "import_waiting",
 *   "import_action")
 * @dismissed: in the caller's transaction: a person dismissed the import;
 *   the grab, if any, is closed
 * @list: the titles a person can choose
 * @match: (nullable) an owner whose downloads hold several files, each its
 *   own (a series' episodes): what the video file @file_name, its name read
 *   as @parsed, holds.  *@unit when it is to be imported; else *@reason (it
 *   names another series, an episode the series' data does not have, one
 *   that is not wanted).  Pure: no database, no file.  With it, every video
 *   file of a download is imported for itself (see "Imports file by file"
 *   in axis-import-run.c).
 * @finished: (nullable) in the import's last transaction, for an owner with
 *   @match: @complete when every file it was to import is in the library
 *   (the grab is done with); %FALSE when it ended partly imported
 * @stuck: (nullable) on @conn, when the import was just blocked because no
 *   file in the download can be taken ("nothing_wanted", "no_video",
 *   "archive": @code, @text) and nothing can be imported anyway: an owner
 *   whose grab a person chose by hand lets go of what it claimed, and says
 *   so in the title's history
 * @assign_read: (nullable) for an owner with @match: what a person wrote
 *   that a file holds ("S01E03 S01E04"), as the owner's ids of it (its
 *   episodes'), in *@ids (gint64).  %FALSE with *@problem when it cannot be
 *   read or the title has no such thing.  Pure.
 * @assigned: (nullable) for an owner with @match: the unit of the ids a
 *   person assigned to a file (its @by_hand set), instead of match().
 *   %FALSE with *@reason when they are not the title's any more.  Pure.
 * @assign_lock: (nullable) in the transaction that records an assignment by
 *   hand, before the title is read: the title @owner_id is locked
 * @assign_claimed: (nullable) in that transaction: *@problem when a grab
 *   other than the import's own is open for what was assigned (@ids: every
 *   assigned file's; empty for a title that is one file)
 * @dup_list: (nullable) on @conn: the owner's titles that hold more than
 *   one linked video file that is present, each as an AxisImportDupGroup
 *   appended to @groups (a movie; an episode, never one the numbering
 *   lacks), for the title @owner_id or, with 0, for all of them.  The owner
 *   fills what it knows (the title, its profile, each file and how it was
 *   linked, and a file it would never let go: AxisImportDupFile.held); the
 *   core fills the rest (see "Duplicates" below).
 * @dup_guard: (nullable) in the caller's transaction, before a group's
 *   files are judged or removed: the title @owner_id is locked, and
 *   *@refusal says why nothing of its group @item_id (0 for a movie, an
 *   episode's id) may be removed now (a grab of it is open).  With @remove
 *   (library file ids, nullable) and @others: the rows that link the files
 *   of @remove and the files that would stay are locked (FOR UPDATE), so
 *   that none is linked elsewhere, or unlinked, between the look and the
 *   removal (the retirement deletes the links of @remove's files on this
 *   very connection, which does not wait on its own lock), and *@refusal
 *   says so when a file of @remove is no longer linked to the group, read
 *   under that lock: it may be another title's file by now.  Then one
 *   AxisImportDupGroup is appended to @others for the movie, or for each
 *   episode that a file of @remove holds, the group's own among them (its
 *   @label: "the movie", "S01E02"), with the present video files that would
 *   still be linked to it, read under that lock (their place, inode, size
 *   and probe; none when it would be left with nothing).  The layer decides
 *   whether one of them is a file that can stay: the owner never judges that
 *   from the index alone.
 */
typedef struct _AxisImportDupGroup AxisImportDupGroup;

typedef struct
{
	const gchar *module;
	const gchar *noun;
	gboolean   (*resolve)(gpointer data, AxisDbConn *conn, gint64 download_id, gint64 owner_id,
	                      AxisImportTarget **target, gchar **problem, GError **error);
	gboolean   (*name_matches)(gpointer data, const AxisImportTarget *target, const AxisRelease *parsed,
	                           gchar **reason);
	gboolean   (*link)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, const AxisImportLinked *linked,
	                   GError **error);
	gboolean   (*note)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, const gchar *kind,
	                   const gchar *text, const gchar *user, GError **error);
	gboolean   (*dismissed)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, const gchar *user,
	                        GError **error);
	gboolean   (*list)(gpointer data, AxisDbConn *conn, GPtrArray **choices, GError **error);
	gboolean   (*match)(gpointer data, const AxisImportTarget *target, const AxisRelease *parsed,
	                    const gchar *file_name, AxisImportUnit **unit, gchar **reason);
	gboolean   (*finished)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, gboolean complete,
	                       GError **error);
	gboolean   (*stuck)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, const gchar *code,
	                    const gchar *text, GError **error);
	gboolean   (*assign_read)(gpointer data, const AxisImportTarget *target, const gchar *said, GArray **ids,
	                          gchar **problem);
	gboolean   (*assigned)(gpointer data, const AxisImportTarget *target, const GArray *ids, AxisImportUnit **unit,
	                       gchar **reason);
	gboolean   (*assign_lock)(gpointer data, AxisDbConn *conn, gint64 owner_id, GError **error);
	gboolean   (*assign_claimed)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, const GArray *ids,
	                             gchar **problem, GError **error);
	gboolean   (*dup_list)(gpointer data, AxisDbConn *conn, gint64 owner_id, GPtrArray *groups, GError **error);
	gboolean   (*dup_guard)(gpointer data, AxisDbConn *conn, gint64 owner_id, gint64 item_id, const GArray *remove,
	                        GPtrArray *others, gchar **refusal, GError **error);
	/* (nullable; the last member, so that an owner that does not name it has
	 * none) In the caller's transaction: a person asked for the import's
	 * download to be deleted ("Delete this download").  The grab, if any, is
	 * closed with the reason "deleted" and no removal of its own (the import
	 * module asks the client, after judging the place), the title's history
	 * says who deleted it, and with @block the release is on the title's
	 * blocklist as blocked by that person.  No search is queued.  An owner
	 * without it is told dismissed(). */
	gboolean   (*download_deleted)(gpointer data, AxisDbConn *conn, const AxisImportTarget *target, const gchar *user,
	                               gboolean block, GError **error);
} AxisImportOwnerInfo;

/* ── The layer ──────────────────────────────────────────────────── */

typedef struct _AxisImport AxisImport;

/**
 * axis_import_new:
 * @pool: the database, which must outlive the layer
 * @settings: (nullable): the settings store (%NULL: the defaults, and storage
 *   read-only, as without a store)
 * @storage, @downloads, @probe: the layers it joins
 *
 * Returns: (transfer full): the layer, reference counted
 */
AxisImport *
axis_import_new(
	AxisDbPool    *pool,
	AxisSettings  *settings,
	AxisStorage   *storage,
	AxisDownloads *downloads,
	AxisProbe     *probe
);

AxisImport *
axis_import_ref(AxisImport *self);

void
axis_import_unref(AxisImport *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImport, axis_import_unref)

/* The module's settings, ended by an entry whose key is %NULL. */
const AxisSettingInfo *
axis_import_get_setting_table(void);

/* Main thread, before the layer is used; @info and @data must outlive it. */
void
axis_import_add_owner(
	AxisImport                *self,
	const AxisImportOwnerInfo *info,
	gpointer                   data
);

AxisStorage *
axis_import_get_storage(AxisImport *self);

AxisDbPool *
axis_import_get_pool(AxisImport *self);

/* ── Asking for imports ─────────────────────────────────────────── */

/**
 * axis_import_request:
 * @conn: a connection (in the caller's transaction or not)
 * @module: the owner of the download (download.module)
 * @download_id: a completed download
 * @import_id: (out) (optional): its open import
 * @created: (out) (optional): whether the import is new
 *
 * Opens an import for a completed download unless one is open (or it was
 * imported or dismissed already), and queues its job when it is waiting and
 * due.  What the movies module's follow-up calls for each completed grab,
 * every minute: an import that waits (storage read-only, a volume offline, a
 * probe to retry) is looked at again this way.
 */
gboolean
axis_import_request(
	AxisImport   *self,
	AxisDbConn   *conn,
	const gchar  *module,
	gint64        download_id,
	gint64       *import_id,
	gboolean     *created,
	GError      **error
);

/**
 * axis_import_requeue_due:
 * @queued: (out) (optional): how many imports were looked at
 *
 * Queues the job of every import that waits and is due (a manual import,
 * or one whose grab is closed, has no follow-up of its own to look at it
 * again), and of every import left 'running' for ten minutes (a job that
 * gave up; the next run carries on from what is on disk).  A job that is
 * queued or running already is not queued twice.  What the movies module's
 * follow-up calls every minute.
 */
gboolean
axis_import_requeue_due(
	AxisImport  *self,
	AxisDbConn  *conn,
	guint       *queued,
	GError     **error
);

/**
 * AxisImportReport:
 * @state: the import's state afterwards ("waiting", "blocked", "imported",
 *   ...), static
 * @reason: (nullable): its reason code, static
 * @placed: files placed by this run
 * @recycled: files retired into a recycle folder by this run
 */
typedef struct
{
	const gchar *state;
	const gchar *reason;
	guint        placed;
	guint        recycled;
} AxisImportReport;

/**
 * axis_import_run:
 * @context: (nullable): the job, for cancellation
 *
 * The job's work for one import (see the promises above).  Fails only for
 * a database error or cancellation, and when a step it was at cannot be
 * known (the job is tried again); everything else is the import's state.
 */
gboolean
axis_import_run(
	AxisImport        *self,
	gint64             import_id,
	AxisJobContext    *context,
	AxisImportReport  *report,
	GError           **error
);

/**
 * AxisImportCleanupReport:
 * @removed: recycled files removed after their retention
 * @gone: recorded files no longer there
 * @changed: places where something else is now (left alone)
 * @temporaries: temporary files left by a job that died, removed
 * @kept: temporary files kept (the only copy of something), recorded
 * @removals: downloads removed from their clients
 * @folders: leftover folders of downloads removed after their import
 * @moved: download files deleted after a move across volumes copied them
 * @files_gone: downloads whose files something else deleted before their
 *   import (the import ended so), or after it while they seeded (recorded)
 * @recovered: recycle rows with no import that were left "moving" (a
 *   duplicate removed by hand whose record was cut short), finished or
 *   forgotten
 */
typedef struct
{
	guint removed;
	guint gone;
	guint changed;
	guint temporaries;
	guint kept;
	guint removals;
	guint folders;
	guint moved;
	guint files_gone;
	guint recovered;
} AxisImportCleanupReport;

/* ── Leftovers in the clients' folders for Axis ─────────────────── */

/* At most this many entries of one client's folder are shown, at most this
 * many counted, and a folder's size is added up over at most this many
 * names. */
#define AXIS_IMPORT_LEFTOVERS_SHOWN      200
#define AXIS_IMPORT_LEFTOVERS_COUNTED    10000
#define AXIS_IMPORT_LEFTOVER_SIZE_NAMES  2000

/**
 * AxisImportLeftover:
 * @name: its name, for the page (text)
 * @stored: its name in the stored form, what a deletion posts
 * @folder: a folder (else a file)
 * @inode: its inode (a deletion must find this very one)
 * @size_bytes: what it holds (a folder's over at most
 *   %AXIS_IMPORT_LEFTOVER_SIZE_NAMES names; @size_partial then)
 * @modified_us: its modification time
 * @deletable: Axis's rules let a person delete it now
 * @why: (nullable) when not, why
 *
 * One entry of a client's folder for Axis that no download Axis follows and
 * no import still open accounts for.
 */
typedef struct
{
	gchar    *name;
	gchar    *stored;
	gboolean  folder;
	guint64   inode;
	guint64   size_bytes;
	gboolean  size_partial;
	gint64    modified_us;
	gboolean  deletable;
	gchar    *why;
} AxisImportLeftover;

void
axis_import_leftover_free(AxisImportLeftover *entry);

/**
 * AxisImportLeftoverFolder:
 * @client_id, @client_name: the client
 * @folder: (nullable) its folder for Axis, as it names it
 * @volume_name: (nullable) the volume that folder is on
 * @problem: (nullable) why it cannot be looked in (no folder of Axis's own,
 *   not mapped, inside the library, the volume offline, ...)
 * @entries: (element-type AxisImportLeftover) by name, at most
 *   %AXIS_IMPORT_LEFTOVERS_SHOWN
 * @n_total: the entries not accounted for that a person can act on (what
 *   Axis would let go, storage allowing), counted up to
 *   %AXIS_IMPORT_LEFTOVERS_COUNTED with @n_kept (@more: there are more); the
 *   count the Downloads page and the dashboard show
 * @n_kept: the entries not accounted for that are not offered: in use at the
 *   client or not to be told from what it has, or not Axis's to delete (a
 *   link, another filesystem, a library root)
 * @caution: (nullable) why nothing at all is offered for this client now
 *   (its list could not be had, was not whole, or held what Axis could not
 *   read)
 */
typedef struct
{
	gint64     client_id;
	gchar     *client_name;
	gchar     *folder;
	gchar     *volume_name;
	gchar     *problem;
	GPtrArray *entries;
	guint      n_total;
	gboolean   more;
	guint      n_kept;
	gchar     *caution;
} AxisImportLeftoverFolder;

void
axis_import_leftover_folder_free(AxisImportLeftoverFolder *folder);

/**
 * axis_import_leftovers:
 * @client_id: one client, or 0 for every one
 * @detail: each entry looked at (its size, whether it may go); without, only
 *   counted
 * @folders: (out) (element-type AxisImportLeftoverFolder)
 *
 * What is in each client's folder for Axis that no download Axis follows and
 * no import still open accounts for (see axis-import-leftovers.c: only a
 * folder of Axis's own, through the client's mapping, never in the library,
 * through a read guard, never following a link, bounded).  Asks each client
 * where its folder for Axis is.  Records the count for the Downloads page and
 * the dashboard.  Nothing is deleted.
 */
gboolean
axis_import_leftovers(
	AxisImport  *self,
	gint64       client_id,
	gboolean     detail,
	GPtrArray  **folders,
	GError     **error
);

typedef enum
{
	AXIS_IMPORT_LEFTOVER_DELETED,
	AXIS_IMPORT_LEFTOVER_GONE,	/* it was not there any more */
	AXIS_IMPORT_LEFTOVER_CHANGED,	/* something else has its name now */
	AXIS_IMPORT_LEFTOVER_REFUSED	/* a rule, or the volume, refused */
} AxisImportLeftoverOutcome;

/**
 * axis_import_leftover_delete:
 * @stored: the entry's name in the stored form, as the list gave it
 * @inode: its inode, as the list gave it
 * @outcome: (out): what happened
 * @text: (out) (transfer full): the page's words for it
 *
 * The one deletion a person asks for on the Leftovers page: every rule of
 * the listing asked again, through a write guard (never while storage is
 * read-only or the volume is offline or mounted read-only), the entry being
 * the very inode listed, one name directly in the client's folder for Axis,
 * not a link, holding no library root; then the write layer's own removal of
 * a file or a whole folder.  Fails only for the database.
 */
gboolean
axis_import_leftover_delete(
	AxisImport                  *self,
	gint64                       client_id,
	const gchar                 *stored,
	guint64                      inode,
	AxisImportLeftoverOutcome   *outcome,
	gchar                      **text,
	GError                     **error
);

/* The cleanup job's work (import.cleanup); @import_id 0 for everything due,
 * else only that import's recycled files that are due. */
gboolean
axis_import_run_cleanup(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
);

/* ── Acting (administrators) ────────────────────────────────────── */

/**
 * AxisImportAction:
 * @AXIS_IMPORT_ACT_ANYWAY: override the objections that can be overridden
 *   (name, runtime, quality, an unreadable file) and import
 * @AXIS_IMPORT_ACT_CHOOSE: the file at @position is the title's
 * @AXIS_IMPORT_ACT_ASSIGN: the download is another title's (@owner_id):
 *   this import is dismissed and a manual one opened for that title
 * @AXIS_IMPORT_ACT_DISMISS: give up on this import; the download stays
 *   where it is, at its client
 * @AXIS_IMPORT_ACT_RETRY: look again (after a failed placement, or once
 *   what blocked it was put right)
 */
typedef enum
{
	AXIS_IMPORT_ACT_ANYWAY,
	AXIS_IMPORT_ACT_CHOOSE,
	AXIS_IMPORT_ACT_ASSIGN,
	AXIS_IMPORT_ACT_DISMISS,
	AXIS_IMPORT_ACT_RETRY
} AxisImportAction;

/* "import_anyway", "choose_file", "assign", "dismiss", "retry" (posted,
 * stored). */
const gchar *
axis_import_action_name(AxisImportAction action);

gboolean
axis_import_action_from_name(
	const gchar      *name,
	AxisImportAction *action
);

/**
 * axis_import_act:
 * @user: who (recorded)
 * @new_import: (out) (optional): for assign, the import opened
 *
 * Refused with %AXIS_IMPORT_ERROR_REFUSED for an import that is not blocked
 * (dismiss and retry also take a waiting one), for "import anyway" when an
 * objection that cannot be overridden stands, and for a position that is
 * not a video of the import's.  The action is recorded, the title's history
 * gains it, and the import's job is queued.
 */
gboolean
axis_import_act(
	AxisImport        *self,
	gint64             import_id,
	AxisImportAction   action,
	gint               position,
	gint64             owner_id,
	const gchar       *user,
	gint64            *new_import,
	GError           **error
);

/* ── Delete this download ───────────────────────────────────────── */

/**
 * AxisImportDeleteBy:
 * @AXIS_IMPORT_DELETE_BY_CLIENT: a torrent its client still has: the client
 *   deletes its files when Axis asks it to remove the torrent
 * @AXIS_IMPORT_DELETE_BY_AXIS_KEPT: a Usenet job: the client removes the job
 *   and keeps its folder, and Axis deletes the folder
 * @AXIS_IMPORT_DELETE_BY_AXIS_GONE: the client no longer has the download:
 *   Axis deletes what it left
 */
typedef enum
{
	AXIS_IMPORT_DELETE_BY_CLIENT,
	AXIS_IMPORT_DELETE_BY_AXIS_KEPT,
	AXIS_IMPORT_DELETE_BY_AXIS_GONE
} AxisImportDeleteBy;

/**
 * AxisImportDeletePreview:
 * @import_id, @title_label, @release_name, @module: the import
 * @state: its state now ("blocked", "waiting", "partial")
 * @allowed: every rule allows the deletion now
 * @refusal: (nullable): when not @allowed, the rule in words (a sentence)
 * @waits: the refusal is one that passes by itself (the client does not
 *   answer, the disk is offline or cannot be written, storage is read-only),
 *   not a rule that refuses for good
 * @gone: nothing is at the download's place any more (it is @allowed: the
 *   import is closed and the client asked, and nothing is left to delete)
 * @volume_name, @path: what would go: the place on its volume (@path shown
 *   as text, not the stored form)
 * @client_path: the place as the client names it
 * @file: it is one file, not a folder
 * @n_files, @bytes: the files in it and their total size
 * @by: who deletes them
 * @placed: (element-type utf8): the library places of the files this import
 *   already placed, which stay
 * @n_linked: how many of those are other names of files of the download
 *   (hard links): the names and their content stay
 * @has_grab: the download came from a grab (the blocklist tick is offered)
 * @digest: what the confirmation showed, for the post to compare
 *
 * What "Delete this download" would do, for its confirmation page.
 */
typedef struct
{
	gint64              import_id;
	gchar              *title_label;
	gchar              *release_name;
	gchar              *module;
	gchar              *state;
	gboolean            allowed;
	gchar              *refusal;
	gboolean            waits;
	gboolean            gone;
	gchar              *volume_name;
	gchar              *path;
	gchar              *client_path;
	gboolean            file;
	guint               n_files;
	guint64             bytes;
	AxisImportDeleteBy  by;
	GPtrArray          *placed;
	guint               n_linked;
	gboolean            has_grab;
	gchar              *digest;
} AxisImportDeletePreview;

void
axis_import_delete_preview_free(AxisImportDeletePreview *preview);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImportDeletePreview, axis_import_delete_preview_free)

/**
 * axis_import_delete_preview:
 *
 * What deleting the download of import @import_id would do.  Nothing is
 * written, and nothing is asked of a client but what it has and where.  The
 * decision is the one the deletion makes (the first half of the removal of
 * a download's leftover, axis-import-folder.c).  Fails with
 * %AXIS_IMPORT_ERROR_NOT_FOUND for no such import, and with
 * %AXIS_IMPORT_ERROR_REFUSED for an import that is not blocked, waiting or
 * partly imported, one a job is placing, or one no download of Axis's
 * brought (a manual import of a folder: the Leftovers page is where that
 * can be deleted).
 */
gboolean
axis_import_delete_preview(
	AxisImport                *self,
	gint64                     import_id,
	AxisImportDeletePreview  **preview,
	GError                   **error
);

/**
 * AxisImportDeleteOutcome:
 * @AXIS_IMPORT_DELETE_DELETED: the download's files are deleted (by the
 *   client, or by Axis)
 * @AXIS_IMPORT_DELETE_GONE: nothing was left to delete
 * @AXIS_IMPORT_DELETE_LEFT: a rule left the files where they are; the
 *   import's page says which and where
 * @AXIS_IMPORT_DELETE_WAITING: it could not be finished now (the client did
 *   not answer, the disk could not be written); the hourly cleanup finishes
 *   it, and the import's page says what is still to be done
 */
typedef enum
{
	AXIS_IMPORT_DELETE_DELETED,
	AXIS_IMPORT_DELETE_GONE,
	AXIS_IMPORT_DELETE_LEFT,
	AXIS_IMPORT_DELETE_WAITING
} AxisImportDeleteOutcome;

/**
 * axis_import_delete_download:
 * @digest: the digest of the confirmation the person saw
 * @block: also put the release on the title's blocklist, as blocked by @user
 * @user: who (recorded)
 * @outcome: (out): what became of the download's files
 *
 * Deletes the download of an import that is blocked, waiting or partly
 * imported.  The decision is made again; when it refuses, or is not what the
 * confirmation showed (@digest), nothing is written and the call fails with
 * %AXIS_IMPORT_ERROR_CHANGED.  Then, in order: the import is closed as
 * dismissed with the reason download_deleted and @user's name, its grab
 * closed and the request recorded, in one transaction; the client is asked
 * to remove the download with its data (a torrent's only after its place was
 * judged, and only when the client says it is at the recorded place); and
 * what the client kept, or left when it no longer had the download, is
 * deleted by the removal of a download's leftover, under that removal's
 * rules.  Nothing in a library folder is ever deleted, and no search is
 * queued.  What could not be finished is finished by the hourly cleanup.
 */
gboolean
axis_import_delete_download(
	AxisImport               *self,
	gint64                    import_id,
	const gchar              *digest,
	gboolean                  block,
	const gchar              *user,
	AxisImportDeleteOutcome  *outcome,
	GError                  **error
);

/**
 * AxisImportManualSpec:
 * @module: the owner's module ("movies")
 * @owner_id: the title chosen
 * @download_id: a completed download, or 0
 * @root_id: with no download, a downloads root
 * @path: with no download, a folder or file in that root (storage form,
 *   relative to the root)
 * @position: the file chosen (-1: let the import decide)
 * @user: who
 * @assign: the import is opened to be assigned by hand: its run reads the
 *   files and then waits for the assignment (blocked, with the reason
 *   "assign"), instead of deciding by the names
 */
typedef struct
{
	const gchar *module;
	gint64       owner_id;
	gint64       download_id;
	gint64       root_id;
	const gchar *path;
	gint         position;
	const gchar *user;
	gboolean     assign;
} AxisImportManualSpec;

/* Opens a manual import and queues its job.  Fails with
 * %AXIS_IMPORT_ERROR_INVALID (no such title, root or download, a path that
 * is not valid or not beneath the root) and %AXIS_IMPORT_ERROR_BUSY. */
gboolean
axis_import_manual(
	AxisImport                 *self,
	const AxisImportManualSpec *spec,
	gint64                     *import_id,
	GError                    **error
);

/* ── Assigning a download's files by hand ───────────────────────── */

/**
 * AxisImportAssignAsk:
 * @positions: (element-type gint): the files a person chose
 * @said: (element-type utf8): for each, what they said it holds, as they
 *   wrote it ("S01E03 S01E04"); ignored for a title that is one file (a
 *   movie), where being chosen is the whole choice
 * @runtime_ok: (element-type gint): the positions ticked "assign it all the
 *   same" for a runtime far from the expected one
 * @replace: (element-type gint64): the library files ticked to be replaced
 *
 * What the form and the confirmation of "Assign by hand" post.
 */
typedef struct
{
	GArray    *positions;
	GPtrArray *said;
	GArray    *runtime_ok;
	GArray    *replace;
} AxisImportAssignAsk;

AxisImportAssignAsk *
axis_import_assign_ask_new(void);

void
axis_import_assign_ask_free(AxisImportAssignAsk *ask);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImportAssignAsk, axis_import_assign_ask_free)

/**
 * AxisImportAssignReplace:
 * @file_id: a library file the chosen file would replace
 * @path: where it is (the root's name and its path, for the page)
 * @quality: its quality, as a person reads it
 * @compare: the new file against it: 1 better, 0 the same quality, -1 worse
 * @kept: it is kept by hand
 * @ticked: the person ticked replacing it
 */
typedef struct
{
	gint64    file_id;
	gchar    *path;
	gchar    *quality;
	gint      compare;
	gboolean  kept;
	gboolean  ticked;
} AxisImportAssignReplace;

/**
 * AxisImportAssignFile:
 * @position, @path, @size, @duration_ms: a video file of the import, as its
 *   last run recorded it (@path the stored form; "" when the download is
 *   that one file)
 * @imported: an earlier run of this import placed it already: no choice
 * @read: (nullable): what Axis reads from its name, in the form a person
 *   writes ("S01E03 S01E04"); %NULL for a movie's file or when nothing is
 *   read
 * @chosen, @said: the person's choice
 * @label: (nullable): what it becomes ("S01E03", "Hearth (1995)")
 * @name: (nullable): the name it gets in the library
 * @quality: (nullable): its quality as the import will record it
 * @expected_ms: what it should run, 0 when not known
 * @runtime_off: its runtime is further off than the settings allow: it needs
 *   its tick (@runtime_ok: given)
 * @replaces: (element-type AxisImportAssignReplace)
 * @problem: (nullable): why this choice cannot be made as it stands
 */
typedef struct
{
	gint       position;
	gchar     *path;
	gint64     size;
	gint64     duration_ms;
	gboolean   imported;
	gchar     *read;
	gboolean   chosen;
	gchar     *said;
	gchar     *label;
	gchar     *name;
	gchar     *quality;
	gint64     expected_ms;
	gboolean   runtime_off;
	gboolean   runtime_ok;
	GPtrArray *replaces;
	gchar     *problem;
} AxisImportAssignFile;

/**
 * AxisImportAssignment:
 * @by_file: every file is something of its own (a series' episodes); else
 *   one file is the title (a movie)
 * @files: (element-type AxisImportAssignFile): the import's video files
 * @n_chosen, @n_left: how many were chosen, and how many are left alone
 * @refused: (nullable): why nothing can be assigned at all (the import is
 *   not waiting for a person, its title is gone, no file was read yet)
 * @missing: (nullable): the ticks still missing, in words
 * @ready: something is chosen, no choice has a problem and no tick is
 *   missing: it can be applied
 * @digest: what the review showed, for the post that applies it
 */
typedef struct
{
	gint64     import_id;
	gchar     *module;
	gchar     *title_label;
	gchar     *state;
	gboolean   by_file;
	GPtrArray *files;
	guint      n_chosen;
	guint      n_left;
	gchar     *refused;
	gchar     *missing;
	gboolean   ready;
	gchar     *digest;
} AxisImportAssignment;

void
axis_import_assignment_free(AxisImportAssignment *assignment);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImportAssignment, axis_import_assignment_free)

/**
 * axis_import_assign_preview:
 * @ask: (nullable): the choices posted; %NULL for the form as it opens (what
 *   Axis reads from each name is then what a person starts from, and nothing
 *   is chosen)
 *
 * What assigning the files of an import by hand would do: each video file,
 * what it becomes and is named, what it replaces, its runtime beside the
 * expected one, the ticks it needs.  It is decided by the import's own rules
 * (the decision a run makes, on the files as the last run recorded them), so
 * the review and the import cannot disagree.  Writes nothing.  Fails with
 * %AXIS_IMPORT_ERROR_NOT_FOUND.
 */
gboolean
axis_import_assign_preview(
	AxisImport                 *self,
	gint64                      import_id,
	const AxisImportAssignAsk  *ask,
	AxisImportAssignment      **assignment,
	GError                    **error
);

/**
 * axis_import_assign:
 * @digest: the review's, as posted
 * @applied: (out): whether the assignment was recorded
 * @changed: (out): not applied because what the review showed is out of date
 * @assignment: (out) (optional): the review as it is now (to show again when
 *   not applied)
 *
 * Records the assignment and queues the import's job, in one transaction
 * with the import's row and the title locked: the import must be blocked or
 * partly imported and not being placed, no other grab may be open for what
 * was assigned, the review must be unchanged (@digest) and every tick it
 * needs given.  The import's earlier assignment, if any, is replaced.  The
 * action and the title's history are recorded with @user.  %TRUE with
 * *@applied %FALSE when it is refused (the review says why); %FALSE only for
 * the database and %AXIS_IMPORT_ERROR_NOT_FOUND.
 */
gboolean
axis_import_assign(
	AxisImport                 *self,
	gint64                      import_id,
	const AxisImportAssignAsk  *ask,
	const gchar                *digest,
	const gchar                *user,
	gboolean                   *applied,
	gboolean                   *changed,
	AxisImportAssignment      **assignment,
	GError                    **error
);

/* ── Reading ────────────────────────────────────────────────────── */

/**
 * AxisImportReason:
 * @code: a stable identifier ("several", "runtime", ...)
 * @text: the sentence
 * @overridable: "import anyway" can override it
 */
typedef struct
{
	gchar    *code;
	gchar    *text;
	gboolean  overridable;
} AxisImportReason;

void
axis_import_reason_free(AxisImportReason *reason);

/**
 * AxisImportFileInfo:
 *
 * One file an import found, as stored.  @path is the stored form, relative
 * to the import's source ("" when the source is that one file).
 */
typedef struct
{
	gint64     id;
	gint       position;
	gchar     *path;
	gchar     *kind;
	gint64     size;
	gchar     *role;
	gboolean   accepted;
	GPtrArray *reasons;	/* AxisImportReason */
	gint64     duration_ms;
	gint       width;
	gint       height;
	gchar     *video_codec;
	gchar     *probe_text;
	gchar     *dest_dir;
	gchar     *dest_name;
	gchar     *step;
	gchar     *mode;
	gchar     *checksum;
	gchar     *note;
} AxisImportFileInfo;

void
axis_import_file_info_free(AxisImportFileInfo *info);

/**
 * AxisImportInfo:
 *
 * One import, as stored, with what pages show.  Times are microseconds
 * since 1970, 0 for none.
 */
typedef struct
{
	gint64     id;
	gchar     *module;
	gint64     owner_id;
	gint64     grab_id;
	gint64     title_id;
	gchar     *title_label;
	gint64     download_id;
	gint64     source_volume_id;
	gchar     *source_volume;
	gchar     *source_path;
	gchar     *release_name;
	gchar     *protocol;
	gchar     *kind;
	gchar     *state;
	gchar     *reason;
	gchar     *reason_text;
	gint64     root_id;
	gchar     *root_name;
	gint64     dest_volume_id;
	gchar     *dest_volume;
	gchar     *dest_dir;
	gchar     *dest_file;
	gchar     *mode;
	gchar     *mode_text;
	gchar     *quality;
	gchar     *quality_text;
	gboolean   upgrade;
	gint       chosen_position;
	gchar    **override_codes;
	gchar     *override_by;
	gchar     *client_removal;
	gchar     *client_removal_text;
	gchar     *folder_removal;	/* a Usenet download's leftover folder: pending, waiting, done, gone, left */
	gchar     *folder_removal_text;
	gint       attempts;
	gint64     retry_after_us;
	gchar     *requested_by;
	gint64     created_us;
	gint64     updated_us;
	gint64     finished_us;
	guint      n_recycled;
	GPtrArray *files;	/* AxisImportFileInfo, by position; %NULL in lists */
	GPtrArray *actions;	/* AxisImportActionInfo; %NULL in lists */
	GPtrArray *recycled;	/* AxisImportRecycledInfo; %NULL in lists */
} AxisImportInfo;

void
axis_import_info_free(AxisImportInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImportInfo, axis_import_info_free)

typedef struct
{
	gchar  *action;
	gchar  *by_user;
	gchar  *text;
	gint64  created_us;
} AxisImportActionInfo;

void
axis_import_action_info_free(AxisImportActionInfo *info);

typedef struct
{
	gchar  *original_path;
	gchar  *recycle_path;
	gchar  *how;
	gchar  *state;
	gint64  size;
	gint64  retired_us;
	gint64  remove_after_us;
	gchar  *note;
	/* For a duplicate a person removed (axis_import_duplicates_recycled()). */
	gchar  *label;
	gchar  *by_user;
	gchar  *volume_name;
	gint64  volume_id;
	gint64  removed_us;
} AxisImportRecycledInfo;

void
axis_import_recycled_info_free(AxisImportRecycledInfo *info);

/**
 * AxisImportList:
 * @AXIS_IMPORT_LIST_WAITING: waiting or running, oldest first
 * @AXIS_IMPORT_LIST_BLOCKED: blocked, oldest first
 * @AXIS_IMPORT_LIST_RECENT: imported or dismissed, the latest first
 */
typedef enum
{
	AXIS_IMPORT_LIST_WAITING,
	AXIS_IMPORT_LIST_BLOCKED,
	AXIS_IMPORT_LIST_RECENT
} AxisImportList;

gboolean
axis_import_list(
	AxisImport      *self,
	AxisImportList   which,
	guint            limit,
	GPtrArray      **imports,
	GError         **error
);

/* One import with its files, actions and recycled files.  Fails with
 * %AXIS_IMPORT_ERROR_NOT_FOUND. */
gboolean
axis_import_get(
	AxisImport      *self,
	gint64           import_id,
	AxisImportInfo **info,
	GError         **error
);

/* The titles of @module a person can choose. */
gboolean
axis_import_list_choices(
	AxisImport   *self,
	const gchar  *module,
	GPtrArray   **choices,
	GError      **error
);

/* How many imports are waiting and blocked (the dashboard). */
gboolean
axis_import_count(
	AxisImport  *self,
	guint       *waiting,
	guint       *blocked,
	GError     **error
);

/**
 * AxisImportVolumeWarning:
 *
 * A download client's mapped folder and a library root of @media_type on
 * different volumes: every import between them would be a copy.
 */
typedef struct
{
	gint64  client_id;
	gchar  *client_name;
	gchar  *client_prefix;
	gint64  download_volume_id;
	gchar  *download_volume;
	gint64  root_id;
	gchar  *root_name;
	gint64  root_volume_id;
	gchar  *root_volume;
} AxisImportVolumeWarning;

void
axis_import_volume_warning_free(AxisImportVolumeWarning *warning);

/* Every such pair, for the Download clients and Storage pages (the
 * database only: no volume is touched). */
gboolean
axis_import_volume_warnings(
	AxisDbPool  *pool,
	GPtrArray  **warnings,
	GError     **error
);

/* The sentence a page shows for a warning, with what it costs. */
gchar *
axis_import_volume_warning_text(const AxisImportVolumeWarning *warning);

/* ── Manual import ──────────────────────────────────────────────── */

/**
 * AxisImportSource:
 * @download_id: a completed download, or 0
 * @root_id: else a downloads root
 * @path: in the root (storage form), or the download's place
 * @label: what it is ("Hearth (1995)", a folder's name)
 * @where: "Volume: path"
 * @when_us: when it completed (downloads), 0 for a folder
 */
typedef struct
{
	gint64  download_id;
	gint64  root_id;
	gchar  *path;
	gchar  *label;
	gchar  *where;
	gint64  when_us;
} AxisImportSource;

void
axis_import_source_free(AxisImportSource *source);

/* The completed downloads not imported, and what is at the top of every
 * downloads root (through a read guard; nothing is written). */
gboolean
axis_import_list_sources(
	AxisImport  *self,
	GPtrArray  **sources,
	GError     **error
);

/**
 * AxisImportPeek:
 *
 * One entry of a source as the walk and the probe read it, for the manual
 * import page; @path is the stored form relative to the source.
 */
typedef struct
{
	gint    position;
	gchar  *path;
	gchar  *kind;
	gint64  size;
	gint64  duration_ms;
	gint    width;
	gint    height;
	gchar  *note;
} AxisImportPeek;

void
axis_import_peek_free(AxisImportPeek *peek);

/* What Axis reads from a source (@download_id, or @root_id and @path):
 * the walk and the probe, through guards, writing nothing.  %TRUE with
 * @problem when it cannot be read. */
gboolean
axis_import_peek(
	AxisImport   *self,
	gint64        download_id,
	gint64        root_id,
	const gchar  *path,
	GPtrArray   **files,
	gchar       **problem,
	GError      **error
);

/* ── The Naming page ────────────────────────────────────────────── */

/**
 * AxisImportPreview:
 * @label: the sample film ("Hearth (1995)")
 * @release: the release name it is shown for
 * @folder, @file: what an import would name it now
 */
typedef struct
{
	gchar *label;
	gchar *release;
	gchar *folder;
	gchar *file;
} AxisImportPreview;

void
axis_import_preview_free(AxisImportPreview *preview);

/**
 * axis_import_preview:
 * @rename: as import.rename
 * @layout: as import.layout
 * @format: as import.file_format
 * @previews: (out) (transfer full) (element-type AxisImportPreview): three
 *   sample films, named as an import names them (axis_import_names_for())
 *
 * Pure.  Returns: %FALSE with @problem when the format is not valid
 */
gboolean
axis_import_preview(
	gboolean      rename,
	const gchar  *layout,
	const gchar  *format,
	GPtrArray   **previews,
	gchar       **problem
);

/**
 * AxisImportNaming:
 *
 * What the names of a title's file are made from: the settings, the title,
 * and what the release name, the file's own name and the probe say.  Pure:
 * the preview and the import both use axis_import_names_for().
 */
typedef struct
{
	gboolean             rename;
	AxisImportLayout     layout;
	const gchar         *format;
	const gchar         *title;
	gint                 year;
	const gchar         *tmdb_id;
	const gchar         *imdb_id;
	const AxisRelease   *release;	/* the grab's (nullable) */
	const AxisRelease   *file;	/* the file's own name (nullable) */
	const gchar         *file_name;	/* the file's own name, for renaming off */
	const gchar         *extension;
	AxisQuality          quality;
	gboolean             proper;
	const gchar         *video_codec;	/* the probe's (nullable): "hevc" */
	const gchar         *hdr;	/* the probe's (nullable): "HDR10" */
	const gchar         *audio_codec;	/* the probe's (nullable): "dts" */
	const gchar         *audio_profile;	/* the probe's (nullable): "DTS-HD MA" */
	gint                 audio_channels;	/* the probe's, 0 when not known */
	/* An episode's file (axis-import-name.h, EPISODES). */
	gboolean             episode;
	const gchar         *tvdb_id;
	gint                 season;
	const gint          *episodes;
	guint                n_episodes;
	const gchar         *episode_title;
	const gchar         *air_date;
} AxisImportNaming;

gboolean
axis_import_names_for(
	const AxisImportNaming  *naming,
	AxisImportName          *name,
	const gchar            **problem
);

/* ── Duplicates (axis-import-duplicates.c) ──────────────────────── */

/*
 * A movie or an episode that holds more than one linked video file (slice D
 * of add-force-grab-and-assign).  The owners list them (dup_list); the layer
 * adds what the disks say, recommends which file to keep, and removes the
 * files a person ticked, by the one way a library file ever leaves the
 * library: retired into the recycle folder of its own disk, as an upgrade's
 * old file is (axis_import_retire_duplicate()).
 */

/**
 * AxisImportDupAdvice:
 * @AXIS_IMPORT_DUP_NONE: Axis cannot tell which file is better: nothing is
 *   recommended for it
 * @AXIS_IMPORT_DUP_KEEP: the file to keep
 * @AXIS_IMPORT_DUP_REMOVE: recommended for removal (ticked on the review)
 * @AXIS_IMPORT_DUP_REMOVE_HAND: Axis would remove it, but a person keeps it
 *   or linked it by hand: said so, and never ticked for the person
 * @AXIS_IMPORT_DUP_STAYS: never offered for removal (AxisImportDupFile.fixed)
 */
typedef enum
{
	AXIS_IMPORT_DUP_NONE,
	AXIS_IMPORT_DUP_KEEP,
	AXIS_IMPORT_DUP_REMOVE,
	AXIS_IMPORT_DUP_REMOVE_HAND,
	AXIS_IMPORT_DUP_STAYS
} AxisImportDupAdvice;

/**
 * AxisImportDupFile:
 * @file: the library file (its quality, size, place, kept by hand)
 * @hand_how: (nullable): "forced", "assigned", "linked": a person made its
 *   link; @hand_by: (nullable) who
 * @by_import: an import of Axis linked it
 * @pinned_links: it has another name on its disk (a hard link)
 * @pinned_downloads: its download still holds it (it seeds)
 * @held: (nullable): the owner's reason never to let it go ("it is the only
 *   file of S01E04")
 * @also: (nullable): what else the owner says of it ("It also holds S01E04.")
 * @probe: (nullable): what the analysis recorded of the file as it is now:
 *   "ok", "failed", "timeout"; %NULL when it was never analysed, or was
 *   analysed before it last changed (media_info's inode and size are not the
 *   file's)
 * @duration_ms: its running time by that analysis; 0 when not known
 *
 * The rest is the layer's:
 * @ranked: @revision and @score are known (the title's profile is usable)
 * @revision, @score: as an upgrade reads them from the name the file came in
 *   with (a file with no such name: the original, score 0)
 * @volume_name: (nullable): its disk
 * @unseen: its disk is offline or mismatched, or not known: the file cannot
 *   be looked at now
 * @fixed: (nullable): why it is never offered for removal (@held, pinned,
 *   a disk that cannot be written)
 * @advice, @advice_text: the recommendation, and its words
 * @ticked: on a review, its tick as it starts; after a post, as posted
 * @removed: a removal retired it; @failure: (nullable) why it could not
 * @recycle_path: (nullable): after a removal, where it is in the recycle
 *   folder of its disk (stored form); @recycle_row: its import_recycle row
 * @unrecorded: it was moved into the recycle folder, but the records could
 *   not be updated (the database was lost twice): it is still listed and
 *   linked until the cleanup job finishes the record; @failure says so
 */
typedef struct
{
	AxisImportExisting   *file;
	gchar                *hand_how;
	gchar                *hand_by;
	gboolean              by_import;
	gboolean              pinned_links;
	gboolean              pinned_downloads;
	gchar                *held;
	gchar                *also;
	gchar                *probe;
	gint64                duration_ms;
	gboolean              ranked;
	gint                  revision;
	gint                  score;
	gchar                *recycle_path;
	gint64                recycle_row;
	gboolean              unrecorded;
	gchar                *volume_name;
	gboolean              unseen;
	gchar                *fixed;
	AxisImportDupAdvice   advice;
	gchar                *advice_text;
	gboolean              ticked;
	gboolean              removed;
	gchar                *failure;
} AxisImportDupFile;

AxisImportDupFile *
axis_import_dup_file_new(void);

void
axis_import_dup_file_free(AxisImportDupFile *file);

/**
 * axis_import_dup_file_set_probe:
 * @status: (nullable): media_info.status of the file, %NULL when it has no
 *   row
 * @duration_ms: media_info.duration_ms, 0 when it is not known
 * @current: the row is of the file as it is now (media_info's inode and
 *   size are library_file's)
 *
 * For the owners: sets @probe and @duration_ms.  An analysis of the file as
 * it was before it changed says nothing of the file there now, and counts as
 * none.
 */
void
axis_import_dup_file_set_probe(
	AxisImportDupFile *file,
	const gchar       *status,
	gint64             duration_ms,
	gboolean           current
);

/**
 * AxisImportDupGroup:
 * @module, @noun: the owner's (set by the layer)
 * @owner_id: the owner's title (a movie, a series)
 * @item_id: what of it holds the files: 0 for a movie, the episode's id
 * @title_id: its metadata title, 0 for none
 * @title_label: "Hearth (1995)", "Harbour Lights (2024)"
 * @label: what the group is: "Hearth (1995)", "Harbour Lights (2024) S01E03"
 * @href: (nullable): the title's page
 * @profile: its quality profile, when @profile_ok
 * @files: (element-type AxisImportDupFile): two or more, by library file id
 *
 * The layer's:
 * @keep: the index of the file to keep, -1 when Axis cannot tell
 * @summary: the recommendation in a sentence
 * @busy: (nullable): why nothing of it can be removed now (an import or a
 *   grab of the title is open)
 * @retention_s: import.recycle_retention; 0: a removed file is deleted for
 *   good at the next cleanup
 * @digest: what a removal must find unchanged
 * @history_lost: after a removal: the files went, but the title's history
 *   could not be written (the database was lost twice); the log has the event
 */
struct _AxisImportDupGroup
{
	const gchar         *module;
	const gchar         *noun;
	gint64               owner_id;
	gint64               item_id;
	gint64               title_id;
	gchar               *title_label;
	gchar               *label;
	gchar               *href;
	AxisQualityProfile   profile;
	gboolean             profile_ok;
	GPtrArray           *files;
	gint                 keep;
	gchar               *summary;
	gchar               *busy;
	gint64               retention_s;
	gchar               *digest;
	gboolean             history_lost;
};

/* At most this many files are removed by one request: the title stays locked
 * while they move. */
#define AXIS_IMPORT_DUP_MAX_REMOVE 20

/* Two running times further apart than this share of the shorter one are not
 * taken for copies of one thing. */
#define AXIS_IMPORT_DUP_DURATION_PERCENT 5

AxisImportDupGroup *
axis_import_dup_group_new(void);

void
axis_import_dup_group_free(AxisImportDupGroup *group);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisImportDupGroup, axis_import_dup_group_free)

/**
 * axis_import_dup_recommend:
 * @group: a group whose files have their facts (the owner's, and @unseen
 *   and @fixed)
 *
 * Which file to keep, and what to say of each: fills @keep, @summary and
 * every file's @advice, @advice_text and @ticked (only a file recommended
 * for removal that no person keeps or linked).  Pure.
 *
 * Nothing is recommended, and nothing ticked, unless the files are known to
 * be copies of one thing as far as Axis can tell: every file has a readable
 * analysis of the file as it is now (@probe "ok", a running time, a size
 * above 0), no two running times are further apart than
 * %AXIS_IMPORT_DUP_DURATION_PERCENT, and no name states a part of a set
 * ("CD1", "Part 2").  The summary says which of these held it back.
 *
 * Then two files are told apart as an upgrade tells them (check_upgrade() in
 * src/quality/axis-quality-decide.c): the profile's order when it lists both
 * qualities, else the standard ladder (an unknown source at a known
 * resolution has a place on it); within one quality the later revision (a
 * proper or repack), then the higher score, then the larger size, then the
 * one kept or linked by hand.  When the revision and the score are not known
 * (@ranked) and the sizes differ, Axis cannot tell.  The file to keep wins
 * against every other; when there is none, or it cannot be seen on its disk,
 * nothing is recommended.
 */
void
axis_import_dup_recommend(AxisImportDupGroup *group);

/**
 * axis_import_duplicates_list:
 * @groups: (out) (transfer full) (element-type AxisImportDupGroup): every
 *   group of every owner, with its recommendation
 *
 * Reads only.  Blocks on the database.
 */
gboolean
axis_import_duplicates_list(
	AxisImport  *self,
	GPtrArray  **groups,
	GError     **error
);

/**
 * axis_import_duplicates_review:
 * @group: (out) (transfer full) (nullable): the group as it stands, with its
 *   digest and @busy; %NULL when the title has no such duplicate now
 *
 * What a removal would do.  Writes nothing (the title is locked for the
 * moment of the look, in a transaction that is rolled back).
 */
gboolean
axis_import_duplicates_review(
	AxisImport          *self,
	const gchar         *module,
	gint64               owner_id,
	gint64               item_id,
	AxisImportDupGroup **group,
	GError             **error
);

/**
 * AxisImportDupOutcome:
 * @AXIS_IMPORT_DUP_DONE: the ticked files were retired (each file's @removed
 *   or @failure says how it went)
 * @AXIS_IMPORT_DUP_REFUSED: nothing was done; *@refusal says why
 * @AXIS_IMPORT_DUP_CHANGED: nothing was done: the group is not as the review
 *   showed it (the digest differs)
 * @AXIS_IMPORT_DUP_GONE: nothing was done: there is no such duplicate now
 */
typedef enum
{
	AXIS_IMPORT_DUP_DONE,
	AXIS_IMPORT_DUP_REFUSED,
	AXIS_IMPORT_DUP_CHANGED,
	AXIS_IMPORT_DUP_GONE
} AxisImportDupOutcome;

/**
 * axis_import_duplicates_remove:
 * @remove: (element-type gint64): the library files ticked
 * @digest: the review's
 * @user: who asks
 * @outcome: (out): what happened
 * @refusal: (out) (transfer full) (nullable): for REFUSED
 * @group: (out) (transfer full) (nullable): the group as it was judged
 *
 * Removes the ticked files of one group, and nothing else.  In one
 * transaction with the title locked: refused while an import or a grab of
 * the title is open, when the digest differs, when a ticked file is not of
 * the group or is not offered for removal (pinned, a disk that cannot be
 * written, the only file of another episode), when nothing is ticked, when
 * more than %AXIS_IMPORT_DUP_MAX_REMOVE files are ticked, when every file is
 * ticked, and when no file that stays is one that can stay.  A file that can
 * stay is on a disk that is online, is on that disk now as the library
 * records it (a regular file, its inode and size), is not empty, and has a
 * readable analysis of the file as it is now.  The same holds for every
 * other episode a ticked file holds: each must keep such a file.
 * Then each ticked file is retired into the recycle folder of its disk
 * (axis_import_retire_duplicate()); one that cannot be stays as it was.
 * The title's history gets one event with @user, what went (where it is now,
 * and when it is deleted from there) and what stays; when the connection was
 * lost meanwhile it is written on another, and when that fails too the
 * group's @history_lost says so (the files went all the same, and the
 * outcome is DONE).
 */
gboolean
axis_import_duplicates_remove(
	AxisImport            *self,
	const gchar           *module,
	gint64                 owner_id,
	gint64                 item_id,
	const GArray          *remove,
	const gchar           *digest,
	const gchar           *user,
	AxisImportDupOutcome  *outcome,
	gchar                **refusal,
	AxisImportDupGroup   **group,
	GError               **error
);

/**
 * axis_import_retire_duplicate:
 * @file: a file of the library (its id, volume, root, paths, inode, size)
 *
 * Retires @file into the recycle folder of its own volume by the import's
 * own step for a replaced file (a recycle row first, a rename that never
 * replaces a name and never leaves the volume, through a write guard; then
 * the row marked kept, with its retention counted from now, and the file's
 * row dropped from the index, its link with it).  Refused, changing nothing,
 * when the file on disk is not the regular file recorded or has another name
 * (a hard link), and by the guard (read-only storage, a volume that is
 * offline or read-only).  Its subtitles are left.
 *
 * @conn: the transaction that holds the title and the links of the files,
 *   locked.  The record after the move (the recycle row kept, the index row
 *   deleted) is made on it, under a savepoint, and is committed with it:
 *   only that connection can delete a link it has locked without waiting on
 *   the lock.  The recycle row before the move is written on a connection of
 *   its own and committed at once, so that a crash leaves a row that says
 *   where the file may be.
 * @label: (nullable): what it was a file of, for the list of recycled
 *   duplicates; @user: (nullable): who removes it
 * @recycle_row: (out) (optional): its import_recycle row, whenever it was
 *   moved
 * @recycle_path: (out) (optional) (transfer full) (nullable): where the file
 *   is in the recycle folder (stored form), whenever it was moved
 * @moved: (out) (optional): the file is in the recycle folder.  %TRUE with a
 *   %FALSE return: it was moved, but the records could not be updated, though
 *   tried twice (its recycle row is still "moving", its index row and link
 *   remain); the cleanup job finishes the record
 *   (axis_import_recover_duplicates()).
 */
gboolean
axis_import_retire_duplicate(
	AxisImport                *self,
	AxisDbConn                *conn,
	const AxisImportExisting  *file,
	const gchar               *label,
	const gchar               *user,
	gint64                    *recycle_row,
	gchar                    **recycle_path,
	gboolean                  *moved,
	GError                   **error
);

/**
 * axis_import_record_duplicate:
 * @conn: (nullable): the title's transaction (see
 *   axis_import_retire_duplicate()); %NULL when that transaction is over
 *   (lost, or rolled back): the record is then made on a connection of its
 *   own and committed at once
 * @recycle_row, @recycle_path: as axis_import_retire_duplicate() gave them
 *
 * The record of a duplicate that was moved, once: its recycle row marked
 * kept at @recycle_path with its retention counted from now, and its row
 * dropped from the index.  A recycle row that is gone by then is written
 * again, so that the file stays listed and is deleted in time.  May be
 * repeated.
 */
gboolean
axis_import_record_duplicate(
	AxisImport                *self,
	AxisDbConn                *conn,
	gint64                     recycle_row,
	const gchar               *recycle_path,
	const AxisImportExisting  *file,
	const gchar               *label,
	const gchar               *user,
	GError                   **error
);

/**
 * axis_import_recover_duplicates:
 * @older_than_s: only rows retired at least this long ago (a removal that is
 *   under way has such a row for a moment)
 * @report: (nullable): its @recovered is raised for each row settled
 *
 * The recycle rows with no import that are still "moving": a duplicate
 * removed by hand whose record was cut short (the daemon died, or lost the
 * database, between the row, the move and the mark).  As recover_recycled()
 * does for an import: when the recorded file is in the recycle folder (its
 * inode, with no other name), the record is finished (the row kept, its
 * retention counted from now, the file's row dropped from the index); when
 * it is still at its place, the row is forgotten.  Anything else (a volume
 * that cannot be read now, a file in neither place) is left for the next
 * time.  Moves nothing.
 */
gboolean
axis_import_recover_duplicates(
	AxisImport               *self,
	gint64                    older_than_s,
	AxisImportCleanupReport  *report,
	GError                  **error
);

/**
 * axis_import_duplicates_recycled:
 * @items: (out) (transfer full) (element-type AxisImportRecycledInfo): the
 *   duplicates removed by hand, the latest first (at most 500): where each
 *   was and is, what it was a file of, who removed it and when, and when it
 *   is, or was, deleted from the recycle folder
 *
 * Reads only.
 */
gboolean
axis_import_duplicates_recycled(
	AxisImport  *self,
	GPtrArray  **items,
	GError     **error
);

/* ── For tests ──────────────────────────────────────────────────── */

/**
 * AxisImportHookVerdict:
 * @AXIS_IMPORT_HOOK_GO: carry on
 * @AXIS_IMPORT_HOOK_FAIL: the step fails here, as an I/O error would: the
 *   import takes its failure path (and removes what it made)
 * @AXIS_IMPORT_HOOK_CRASH: stop at once, cleaning nothing and recording
 *   nothing more, as a daemon that died here would; the run fails
 */
typedef enum
{
	AXIS_IMPORT_HOOK_GO,
	AXIS_IMPORT_HOOK_FAIL,
	AXIS_IMPORT_HOOK_CRASH
} AxisImportHookVerdict;

/**
 * AxisImportHook:
 * @step: "decided", "folder", "temp", "copying", "copied", "published",
 *   "linked", "retired", "recorded"
 * @import_id: the import
 * @file_id: the import_file being placed, 0 for none
 *
 * Called on the job's thread at each step of a run.
 *
 * And, with @import_id 0, by a removal on the Duplicates page: "duplicate_moved"
 * before each try to record a file that was moved into the recycle folder
 * (@file_id: its library file), and "duplicate_history" before each try to
 * write the title's history.  Anything but GO fails that try as a lost
 * database would.  And "duplicate_checked", once the ticks are accepted and
 * before the owner locks the files' links: its verdict is ignored (a test
 * changes a link there, as another session might).
 */
typedef AxisImportHookVerdict (*AxisImportHook)(
	const gchar *step,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
);

void
axis_import_set_hook(
	AxisImport     *self,
	AxisImportHook  hook,
	gpointer        data
);

G_END_DECLS

#endif /* AXIS_IMPORT_H */
