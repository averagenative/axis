/*
 * axis-import-private.h - shared by the files of the import pipeline
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 *   axis-import-name.c     names (pure; fuzzed)
 *   axis-import-decide.c   the decision for every file found, quality and
 *                          the names' tokens (pure)
 *   axis-import-walk.c     walking a download through its guard, opening
 *                          what it found, probing
 *   axis-import-run.c      the job: the steps, what is recorded, recovery
 *   axis-import-cleanup.c  the recycle folders and leftover temporaries; a
 *                          download's files gone before (or, seeding, after)
 *                          its import; the download's files a move across
 *                          volumes copied
 *   axis-import-folder.c   a download's leftover folder after its import
 *                          (or after a move): the rules (pure) and the
 *                          removal
 *   axis-import-delete.c   "Delete this download" on an import that waits
 *                          for a person: the confirmation's look, the
 *                          request, and the step that finishes it
 *   axis-import-leftovers.c  what is left in the clients' folders for Axis
 *                          (the Leftovers page), and its deletion
 *   axis-import-duplicates.c  titles that hold more than one linked file
 *                          (the Duplicates page): the recommendation (pure)
 *                          and the removal, by the run's own retirement
 *   axis-import.c          lifetime, settings, owners, requests, actions,
 *                          reading
 */

#ifndef AXIS_IMPORT_PRIVATE_H
#define AXIS_IMPORT_PRIVATE_H

#include "import/axis-import.h"

#include "core/axis-size.h"
#include "db/axis-db-conn.h"
#include "probe/axis-probe-model.h"
#include "storage/axis-volume-guard.h"

G_BEGIN_DECLS

typedef struct
{
	const AxisImportOwnerInfo *info;
	gpointer                   data;
} AxisImportOwner;

struct _AxisImport
{
	gint            refs;	/* atomic */
	AxisDbPool     *pool;
	AxisSettings   *settings;	/* (nullable) owned */
	AxisStorage    *storage;	/* owned */
	AxisDownloads  *downloads;	/* owned */
	AxisProbe      *probe;	/* owned */
	GArray         *owners;	/* AxisImportOwner; set before use */

	GMutex          hook_lock;
	AxisImportHook  hook;	/* (hook_lock) */
	gpointer        hook_data;

	/* Where the hourly repair of dismissed imports goes on from: the last
	 * import it looked at, so that one it cannot settle yet does not hold
	 * back those after it (0: from the start). */
	gint64          settle_after;	/* (hook_lock) */
	guint           settle_batch;	/* at most this many a run; 0: the default (tests make it smaller) */
};

/* ── Settings (axis-import.c) ───────────────────────────────────── */

typedef struct
{
	gboolean          rename;
	AxisImportLayout  layout;
	gchar            *format;
	gboolean          hardlink_only;
	gint64            retention_s;
	guint             dir_mode;
	gint64            free_floor;
	gboolean          subtitles;
	gboolean          extras;
	gint              runtime_tolerance;	/* percent */
	gboolean          torrents_remove;	/* import.torrents_after is "remove" */
	gboolean          move;		/* import.take_files is "move" */
} AxisImportSettings;

void
axis_import_settings_read(
	AxisImport         *self,
	AxisImportSettings *settings
);

void
axis_import_settings_clear(AxisImportSettings *settings);

/* "0755" as a mode, or %FALSE. */
gboolean
axis_import_parse_mode(
	const gchar *text,
	guint       *mode
);

const AxisImportOwner *
axis_import_find_owner(
	AxisImport  *self,
	const gchar *module
);

/* The hook's verdict at @step (GO without one). */
AxisImportHookVerdict
axis_import_hook(
	AxisImport  *self,
	const gchar *step,
	gint64       import_id,
	gint64       file_id
);

gchar *
axis_import_int_text(gint64 value);

/* Cut to @max characters on a character boundary, valid UTF-8, control
 * characters made spaces; %NULL for %NULL. */
gchar *
axis_import_text_cut(
	const gchar *text,
	guint        max
);

/* Whether a library file is on its disk now as recorded (axis-import-run.c;
 * for the Duplicates page, which must keep a file it can see). */
gboolean
axis_import_library_file_seen(
	AxisImport                *self,
	const AxisImportExisting  *file
);

/* Queues the import's job on @conn. */
gboolean
axis_import_enqueue(
	AxisDbConn  *conn,
	gint64       import_id,
	gint         priority,
	GError     **error
);

/* ── What the walk found (axis-import-walk.c) ───────────────────── */

typedef enum
{
	AXIS_IMPORT_FOUND_FILE,
	AXIS_IMPORT_FOUND_LINK,
	AXIS_IMPORT_FOUND_SPECIAL,
	AXIS_IMPORT_FOUND_FOLDER	/* a folder not looked into */
} AxisImportFoundType;

/**
 * AxisImportFound:
 *
 * One entry of a download.  @path is the stored form relative to the
 * source; @dir and @name are its raw bytes (the folder relative to the
 * source, "" for its top, and the last component).  For a download that is
 * one file, @dir and @path are "" and @name its name.
 */
typedef struct
{
	gint                 position;
	AxisImportFoundType  type;
	AxisImportFileKind   kind;
	gchar               *path;
	gchar               *dir;
	gchar               *name;
	guint64              size;
	guint64              inode;
	AxisImportFolder     folder;	/* the kind of folder it is in */
	const gchar         *extra_folder;	/* "Trailers", when in one */
	const gchar         *folder_note;	/* why a folder was not looked into */

	/* The probe (video files). */
	gboolean             probed;
	gboolean             probe_ok;
	gchar               *probe_text;	/* the tool's error, or a summary */
	gint64               duration_ms;
	gint                 width;
	gint                 height;
	gint                 video_streams;
	gchar               *video_codec;
	gchar               *hdr;
	gchar               *audio_codec;
	gchar               *audio_profile;
	gint                 audio_channels;

	/* The decision (axis-import-decide.c). */
	const gchar         *role;	/* "film", "episode", "subtitle", "extra", NULL */
	gboolean             accepted;
	GPtrArray           *reasons;	/* AxisImportReason */
	gchar               *dest_subdir;	/* an extra's folder in the film's */
	gchar               *dest_name;	/* a subtitle's or an extra's name */
	gint64               row_id;	/* its import_file row, once stored */
	gint                 unit_of;	/* file by file: a subtitle's video (its position), -1 */
} AxisImportFound;

void
axis_import_found_free(AxisImportFound *found);

/* Adds a reason to @found (and to the import's blockers when @blocks). */
void
axis_import_found_reason(
	AxisImportFound *found,
	const gchar     *code,
	gboolean         overridable,
	const gchar     *format,
	...
) G_GNUC_PRINTF(4, 5);

/**
 * axis_import_walk:
 * @guard: a guard of the source's volume
 * @source_path: the download's location (storage form, relative to the
 *   volume's top): a folder or one file
 * @found: (out) (transfer full) (element-type AxisImportFound)
 * @is_file: (out): the download is one file
 * @problem: (out) (transfer full): when it cannot be walked at all, why
 *   (a link, nothing there, not a file or folder, another filesystem)
 *
 * Bounded (AXIS_IMPORT_WALK_DEPTH, AXIS_IMPORT_WALK_ENTRIES,
 * AXIS_IMPORT_MAX_FILES), never following a link or leaving the volume's
 * filesystem.  Returns %FALSE only for an error of the guard (the volume
 * went away) or of the system.
 */
gboolean
axis_import_walk(
	AxisVolumeGuard  *guard,
	const gchar      *source_path,
	GPtrArray       **found,
	gboolean         *is_file,
	gchar           **problem,
	GError          **error
);

/**
 * axis_import_open_found:
 * @dir: (out): its folder, open (the caller closes it)
 * @fd: (out) (optional): the file itself, open for reading, checked to be
 *   the regular file with @found's inode and size
 *
 * Opens what the walk found again, the same way.  Fails with
 * %AXIS_STORAGE_ERROR_NOT_CONTAINED when it is no longer the file found.
 */
gboolean
axis_import_open_found(
	AxisVolumeGuard        *guard,
	const gchar            *source_path,
	const AxisImportFound  *found,
	gint                   *dir,
	gint                   *fd,
	GError                **error
);

/* Probes every video file found (at most AXIS_IMPORT_MAX_PROBES) that is
 * not in a folder left out.  %FALSE with %AXIS_PROBE_ERROR_TOOL when ffprobe
 * is not there; a file the tool cannot read is a result. */
gboolean
axis_import_probe_found(
	AxisProbe        *probe,
	AxisVolumeGuard  *guard,
	const gchar      *source_path,
	GPtrArray        *found,
	GError          **error
);

/* The resolution the picture is, from its size. */
AxisReleaseResolution
axis_import_resolution_of(
	gint width,
	gint height
);

/* ── Deciding (axis-import-decide.c) ────────────────────────────── */

/**
 * AxisImportAssigned:
 *
 * One file a person assigned by hand (import_assignment; add-force-grab-
 * and-assign, slice B).
 * @position, @path: the file, as the review showed it: taken only when the
 *   file found at @position still has @path
 * @ids: (element-type gint64): the owner's ids of what it holds (a series'
 *   episodes); empty for a title that is one file
 * @replaces: (element-type gint64): the library files ticked to be replaced
 * @replaces_quality: for each, its quality when it was ticked
 *   (axis_import_quality_key()): a tick holds only while it is the same
 * @runtime_ok: the runtime tick
 * @all_ticks: the review's dry run: every tick counts as given, so that the
 *   review learns what the file becomes and which ticks it needs
 */
typedef struct
{
	gint       position;
	gchar     *path;
	GArray    *ids;
	GArray    *replaces;
	gchar    **replaces_quality;
	gboolean   runtime_ok;
	gboolean   all_ticks;
} AxisImportAssigned;

void
axis_import_assigned_free(AxisImportAssigned *assigned);

/* The assignments of @import_id (%NULL when it has none) and who made them,
 * read on @conn when given, else on the pool. */
gboolean
axis_import_assigned_load(
	AxisImport   *self,
	AxisDbConn   *conn,
	gint64        import_id,
	GPtrArray   **assigned,
	gchar       **by_user,
	GError      **error
);

/* A quality as an assignment's tick records it ("unknown" for none). */
const gchar *
axis_import_quality_key(AxisQuality quality);

/* Whether a file that runs @duration_ms is further from @runtime_minutes
 * than the settings allow (the runtime check of every import). */
gboolean
axis_import_runtime_off(
	const AxisImportSettings *settings,
	gint64                    duration_ms,
	gint                      runtime_minutes
);

/**
 * AxisImportExpect:
 *
 * What the decision is made against.
 */
typedef struct
{
	const AxisImportTarget    *target;
	const AxisImportOwnerInfo *owner;
	gpointer                   owner_data;
	const AxisImportSettings  *settings;
	const AxisRelease         *grab;	/* the grab's release, parsed (nullable) */
	gint64                     grab_size;	/* -1 not known */
	AxisDownloadProtocol       protocol;
	gint                       chosen;	/* a position a person chose, or -1 */
	gchar                    **overrides;	/* codes a person overrode (nullable) */
	gboolean                   probe_tool_missing;
	gint64                     now_us;
	/* File by file, a run taken up again: the paths (stored form) an earlier
	 * run decided to import, which keep that decision (nullable). */
	GHashTable                *kept;
	/* (element-type AxisImportAssigned) (nullable) what a person assigned by
	 * hand: those files are taken as assigned, their names not judged, and
	 * every other file is left alone. */
	const GPtrArray           *assigned;
} AxisImportExpect;

/**
 * AxisImportVerdict:
 * @film: the film's file, or %NULL
 * @blocked: a person must decide (the reasons are on the files and in
 *   @blockers)
 * @retry_probe: a probe failed and the time to ask a person has not come
 * @blockers: (element-type AxisImportReason): what blocks, every reason
 * @summary: one sentence for the import's reason
 * @code: its code
 * @quality, @quality_text, @upgrade: the film's
 * @replace: (element-type AxisImportExisting): the files it replaces
 *   (borrowed from the target)
 */
typedef struct
{
	AxisImportFound *film;
	gboolean         blocked;
	gboolean         retry_probe;
	GPtrArray       *blockers;
	gchar           *summary;
	const gchar     *code;
	AxisQuality      quality;
	gchar           *quality_text;
	gboolean         upgrade;
	gboolean         proper;
	GPtrArray       *replace;
	AxisRelease     *file_release;	/* the film file's own name, parsed */
} AxisImportVerdict;

void
axis_import_verdict_clear(AxisImportVerdict *verdict);

/* Every file's decision, and the import's.  Pure. */
void
axis_import_decide(
	const AxisImportExpect *expect,
	GPtrArray              *found,
	gint                    attempts,
	AxisImportVerdict      *verdict
);

/**
 * AxisImportPlan:
 *
 * File by file (an owner with match()): one video file to import, with what
 * it holds and its quality.
 */
typedef struct
{
	AxisImportFound *found;		/* borrowed */
	AxisImportUnit  *unit;		/* owned */
	AxisQuality      quality;
	gchar           *quality_text;
	gboolean         proper;
	gboolean         upgrade;
	AxisRelease     *file_release;	/* the file's own name, parsed */
	AxisRelease     *fields;	/* the release's fields with the file's put in */
	gboolean         assigned;	/* a person assigned it by hand */
} AxisImportPlan;

void
axis_import_plan_free(AxisImportPlan *plan);

/**
 * AxisImportUnitsVerdict:
 * @plans: (element-type AxisImportPlan): the files to import, in the order
 *   found
 * @blocked, @retry_probe, @blockers, @summary, @code: the import's, as in
 *   AxisImportVerdict: blocked only when no file is to be imported
 * @left: video files left in the download (each with its reason)
 */
typedef struct
{
	GPtrArray   *plans;
	gboolean     blocked;
	gboolean     retry_probe;
	GPtrArray   *blockers;
	gchar       *summary;
	const gchar *code;
	guint        left;
} AxisImportUnitsVerdict;

void
axis_import_units_verdict_clear(AxisImportUnitsVerdict *verdict);

/* File by file: every file's decision, and the import's.  Pure (the owner's
 * match() is pure too). */
void
axis_import_decide_units(
	const AxisImportExpect *expect,
	GPtrArray              *found,
	gint                    attempts,
	AxisImportUnitsVerdict *verdict
);

/* Whether @code is in a person's overrides. */
gboolean
axis_import_overridden(
	gchar      **overrides,
	const gchar *code
);

/* The tokens of axis-import-name.h from a release's fields. */
gchar *
axis_import_edition_text(guint editions);

gchar *
axis_import_audio_text(
	const AxisRelease *release,
	const gchar       *codec,
	const gchar       *profile,
	gint               channels
);

const gchar *
axis_import_codec_text(
	const AxisRelease *release,
	const gchar       *probe_codec
);

/* ── Reasons as stored ──────────────────────────────────────────── */

gchar *
axis_import_reasons_to_json(const GPtrArray *reasons);

GPtrArray *
axis_import_reasons_from_json(const gchar *json);

AxisImportReason *
axis_import_reason_new(
	const gchar *code,
	const gchar *text,
	gboolean     overridable
);

/* ── A download's files gone, and moves (axis-import-cleanup.c) ─── */

/* Whether nothing is at @source_path on the guard's volume: 1 nothing
 * there (it, or a folder on the way to it, is not there); 0 something is
 * (*@inode, optional: its inode); -1 it cannot be told (a symbolic link or
 * another filesystem on the way, the volume not answering). */
gint
axis_import_source_absent(
	AxisVolumeGuard *guard,
	const gchar     *source_path,
	guint64         *inode
);

/* How many files of @import_id were placed or moved (a step, a library file,
 * or what became of the download's file recorded); -1 with @error. */
gint
axis_import_placed_count(
	AxisImport  *self,
	gint64       import_id,
	GError     **error
);

/* The download's files disappeared before they were imported (deleted or
 * moved by something else): the import ends dismissed with the reason
 * files_gone, saying where they were and when Axis last saw them there, and
 * its title is wanted again (the follow-up closes the grab).  Only an import
 * of which nothing was placed or moved, decided in the one statement that
 * ends it: by the import's own run (@by_sweep %FALSE: it is running, and the
 * run holds it), or by the hourly sweep (@by_sweep %TRUE: only one that waits
 * or is blocked, with no job running it); *@done whether it was. */
gboolean
axis_import_files_gone(
	AxisImport  *self,
	gint64       import_id,
	gboolean     by_sweep,
	gboolean    *done,
	GError     **error
);

/*
 * What the other download clients have in or around a client's folder for
 * Axis (@folder_rel on @volume), by what each says now of everything it has,
 * in every category or label (axis_downloads_neighbours(): the clients
 * configured, switched off in Axis too, one of whose path mappings reach it):
 *  - @places: where their downloads are, inside the folder (paths on the
 *    volume);
 *  - @loose: the names of those kept at the folder itself or around it (any
 *    entry of the folder may be one of their files);
 *  - @names: the exact names of those whose place they do not say;
 *  - @unplaced: the release names of downloads Axis follows at them whose
 *    place nobody says yet (one being sent);
 *  - @unknown: why what they have is not all known (one could not be asked,
 *    its list is not whole, it holds an entry Axis cannot read: which one is
 *    said): nothing in the folder may go;
 *  - @shared: another client's folder for Axis is this folder, or one holds
 *    the other: two clients' downloads are not told apart there, and nothing
 *    in it may go;
 *  - @undetermined: another client's folder for Axis cannot be worked out (it
 *    keeps Axis's downloads in no folder of their own, or in none Axis can
 *    use): whether it is this folder cannot be told, and nothing in it may go.
 */
typedef struct
{
	GPtrArray *places;
	GPtrArray *loose;
	GPtrArray *names;
	GPtrArray *unplaced;
	gchar     *unknown;
	gchar     *shared;
	gchar     *undetermined;
} AxisImportOthers;

gboolean
axis_import_others(
	AxisImport        *self,
	gint64             client_id,
	gint64             volume,
	const gchar       *folder_rel,
	AxisImportOthers  *others,
	GError           **error
);

void
axis_import_others_clear(AxisImportOthers *others);

/*
 * What settling an import's temporary names found:
 * @AXIS_IMPORT_SETTLED: nothing of it is left under a temporary name with
 *   nothing that will ever move it
 * @AXIS_IMPORT_UNSETTLED: proven by what is on disk now: a file is under its
 *   temporary name that is not put back and may be its file's only name (a
 *   moved file whose download's folder is gone or whose name there is taken,
 *   a link whose download's name leads elsewhere, a copy whose download's
 *   file is gone or is another file now), or its library folder is gone
 * @AXIS_IMPORT_UNKNOWN: it cannot be told now (a volume away or mounted
 *   read-only, storage read-only, a folder or a file that cannot be looked at
 *   for another reason than its absence, a copy whose times were never
 *   recorded): looked at again later, never taken for either
 */
typedef enum
{
	AXIS_IMPORT_SETTLED,
	AXIS_IMPORT_UNSETTLED,
	AXIS_IMPORT_UNKNOWN
} AxisImportSettle;

/*
 * The files of @import_id that an interrupted run left under their temporary
 * names (a row at the step "temp" whose temporary name is there, the file the
 * row placed), settled so that none is left with nothing that will ever move
 * it: a file moved out of the download is put back under its download name
 * (never over a name taken; *@put_back counts them, their rows forget the
 * step); a link whose download's name still leads to its file, and a copy
 * whose download's file is still there, the very file (inode, size and both
 * times as recorded), need nothing (the hourly cleanup removes their
 * temporary names).  *@outcome says what was found and, unless settled,
 * *@why says it for a person.  Its rows are read and written on @conn when
 * given (the caller's transaction: no second connection is taken while the
 * caller holds one), else on the pool.  %FALSE only with @error, for the
 * database.
 */
gboolean
axis_import_settle_temporaries(
	AxisImport        *self,
	AxisDbConn        *conn,
	gint64             import_id,
	guint             *put_back,
	AxisImportSettle  *outcome,
	gchar            **why,
	GError           **error
);

/* After a move across volumes: the download's files whose verified copies
 * are in the library, deleted once the import is recorded and its client
 * removal is settled (import_file.source_state pending); @import_id 0 for
 * all.  Each only while the library's copy is in place and the download's
 * file is the very file copied. */
gboolean
axis_import_moved_sources(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
);

/* ── Cleaning up (axis-import-cleanup.c) ────────────────────────── */

/* Removes the recycled files of @import_id that are due now (retention 0). */
gboolean
axis_import_cleanup_due(
	AxisImport  *self,
	gint64       import_id,
	GError     **error
);

/* The client removals still to make (Usenet, and torrents unless they are left
 * seeding), then the leftover folders of Usenet downloads whose client removal
 * is done (axis_import_folder_removals()); @import_id 0 for all. */
gboolean
axis_import_client_removals(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
);

/* ── A Usenet download's leftover folder (axis-import-folder.c) ─── */

/**
 * AxisImportFolderRoot:
 * @rel_path: a library root on the folder's volume (stored form; "" for the
 *   volume's top)
 * @downloads: it is a downloads root (where downloads are; every other kind
 *   holds the library)
 * @name: its name, for the reason
 */
typedef struct
{
	const gchar *rel_path;
	gboolean     downloads;
	const gchar *name;
} AxisImportFolderRoot;

/**
 * AxisImportFolderFacts:
 * @client_path: where the client said the download's files were when it
 *   completed (download.client_path), as it said it
 * @category_folder: where the client puts Axis's category (or label), as it
 *   says now (normalised; %NULL when it was not asked, or, for a torrent,
 *   when the client keeps Axis's torrents in no folder of their own)
 * @map_prefix: the client prefix of the mapping that reaches @client_path now
 * @mapped_volume, @mapped_path: where that mapping puts @client_path now
 * @recorded_volume, @recorded_path: where the download was recorded when it
 *   completed (download.volume_id, rel_path)
 * @source_volume, @source_path: where the import took its files from (the
 *   recorded place, or, for a file the client reported inside a job's
 *   folder, that job's folder: axis_import_job_folder())
 * @roots: (element-type AxisImportFolderRoot): the library roots of the
 *   volume
 * @others: (element-type utf8): places on the volume (stored form) of the
 *   other downloads still at their clients and of the imports still open
 * @torrent: the download is a torrent its client no longer has (the client
 *   said so) and whose files the import settings do not keep seeding
 * @no_folder_why: (nullable): for a torrent with no @category_folder, why
 *   the client has none of Axis's own (the client's words), for the page
 * @client_deletes: the download is a torrent its client still has, and the
 *   client is to be asked to remove it with its data ("Delete this download"
 *   on an import's page): what goes is the reported place itself, a folder
 *   or a file, and the one rule left out is the one about a folder of Axis's
 *   own (%AXIS_IMPORT_FOLDER_NO_OWN_FOLDER), which is there for a deletion
 *   Axis makes itself in a folder shared with other downloads.  Every other
 *   rule stands, those of the category's folder too when the client has one
 *
 * What the rules for removing a download's leftover folder look at: text
 * only, gathered by the caller.
 */
typedef struct
{
	const gchar *client_path;
	const gchar *category_folder;
	const gchar *map_prefix;
	gint64       mapped_volume;
	const gchar *mapped_path;
	gint64       recorded_volume;
	const gchar *recorded_path;
	gint64       source_volume;
	const gchar *source_path;
	GPtrArray   *roots;
	GPtrArray   *others;
	gboolean     torrent;
	const gchar *no_folder_why;
	gboolean     client_deletes;
} AxisImportFolderFacts;

/**
 * AxisImportFolderRule:
 * The rule that refuses a folder (%AXIS_IMPORT_FOLDER_OK: none does).
 */
typedef enum
{
	AXIS_IMPORT_FOLDER_OK,
	AXIS_IMPORT_FOLDER_BAD_PATH,		/* not a full path, or a "." or ".." part */
	AXIS_IMPORT_FOLDER_IS_CATEGORY,		/* the category's folder itself */
	AXIS_IMPORT_FOLDER_OUTSIDE_CATEGORY,	/* not inside the category's folder */
	AXIS_IMPORT_FOLDER_MAP_INSIDE,		/* the mapping used begins inside the category's folder */
	AXIS_IMPORT_FOLDER_NOT_IMPORTED_FROM,	/* it maps elsewhere than the import's source */
	AXIS_IMPORT_FOLDER_AXIS_OWN,		/* the volume's top, or Axis's own folders */
	AXIS_IMPORT_FOLDER_IN_LIBRARY,		/* inside a library root */
	AXIS_IMPORT_FOLDER_HOLDS_ROOT,		/* is or holds a library root */
	AXIS_IMPORT_FOLDER_SHARED,		/* another download or open import is in it, or it in one */
	AXIS_IMPORT_FOLDER_NO_OWN_FOLDER	/* a torrent's folder, its client keeping Axis's in no folder of their own */
} AxisImportFolderRule;

/**
 * AxisImportFolderShape:
 * @AXIS_IMPORT_FOLDER_SHAPE_FOLDER: a folder is at the reported place (the
 *   job's folder, as SABnzbd is believed to report a job of several files)
 * @AXIS_IMPORT_FOLDER_SHAPE_FILE: a regular file is (as SABnzbd was seen to
 *   report a job of one file: the file inside the job's folder)
 */
typedef enum
{
	AXIS_IMPORT_FOLDER_SHAPE_FOLDER,
	AXIS_IMPORT_FOLDER_SHAPE_FILE
} AxisImportFolderShape;

/**
 * AxisImportFolderTarget:
 * @file: what goes is one file (a job's file the client put straight into
 *   the category's folder, with no job folder; a torrent's one file when its
 *   client keeps Axis's torrents in no folder of their own), not a folder
 * @path: its place on the volume (stored form)
 * @client_path: its place as the client names it, for the page
 * @inner: within the folder that goes, where the import's source is (""
 *   when the source is the folder itself, or for @file): the import's files
 *   are recorded relative to its source
 * @loose: @file is a torrent's file in a folder that is not Axis's own (no
 *   category folder): the library's copy of it is looked for first
 *
 * What is removed: the reported folder; for a reported file, the job's
 * folder (the first part beneath the category's folder on the way to it);
 * or the reported file alone when it is straight in the category's folder,
 * or is a torrent's one file and its client has no folder of Axis's own.
 */
typedef struct
{
	gboolean  file;
	gchar    *path;
	gchar    *client_path;
	gchar    *inner;
	gboolean  loose;
} AxisImportFolderTarget;

void
axis_import_folder_target_clear(AxisImportFolderTarget *target);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(AxisImportFolderTarget, axis_import_folder_target_clear)

/**
 * axis_import_folder_judge:
 * @shape: what is at the reported place (the caller looked)
 * @target: (out caller-allocates): when allowed, what is to be removed
 *   (cleared by the caller)
 * @why: (out) (transfer full): when refused, the rule in words (a sentence
 *   fragment, for "Its folder was left where it is: ...")
 *
 * Whether a download's leftover folder (or file) may be removed after its
 * import, by the rules of the text alone, and what that is; the rules about
 * the place (roots, other downloads, Axis's own folders) apply to what is
 * removed.  A Usenet job's and a torrent's with a folder of Axis's own are
 * judged alike; a torrent whose client has no folder of Axis's own
 * (@facts->category_folder %NULL) has its one file removed alone, and its
 * folder never (%AXIS_IMPORT_FOLDER_NO_OWN_FOLDER, saying where it is).  The
 * caller then looks at it through a write guard.  Pure.
 */
AxisImportFolderRule
axis_import_folder_judge(
	const AxisImportFolderFacts  *facts,
	AxisImportFolderShape         shape,
	AxisImportFolderTarget       *target,
	gchar                       **why
);

/**
 * AxisImportJobPlace:
 * @AXIS_IMPORT_JOB_FOLDER: the file is inside a job's folder
 * @AXIS_IMPORT_JOB_NONE: the file is straight in the category's folder
 * @AXIS_IMPORT_JOB_UNTOLD: the job's folder cannot be told (the file is not
 *   strictly inside the category's folder, or its place on the volume does
 *   not end as the client's path does)
 */
typedef enum
{
	AXIS_IMPORT_JOB_FOLDER,
	AXIS_IMPORT_JOB_NONE,
	AXIS_IMPORT_JOB_UNTOLD
} AxisImportJobPlace;

/**
 * axis_import_job_folder:
 * @category_folder: the client's folder for Axis's category (normalised)
 * @client_path: a file's place as the client reported it (normalised)
 * @mapped_path: its place on its volume, through a mapping that holds the
 *   category's folder too
 * @job_client: (out) (transfer full) (optional): the job's folder at the
 *   client: the category's folder and the first part beneath it
 * @job_rel: (out) (transfer full) (optional): the job's folder on the
 *   volume: @mapped_path without the parts below the job's folder
 * @inner: (out) (transfer full) (optional): those parts ("<...>/<file>")
 *
 * The job's folder of a file a client reported inside it (SABnzbd reports a
 * job of one file so: <category>/<job>/<file>), by the one definition the
 * import and the leftover folder's removal share.  Pure.
 */
AxisImportJobPlace
axis_import_job_folder(
	const gchar  *category_folder,
	const gchar  *client_path,
	const gchar  *mapped_path,
	gchar       **job_client,
	gchar       **job_rel,
	gchar       **inner
);

/* The leftover folders still to be looked at (pending or waiting) of imported
 * Usenet downloads whose client removal is done or gone, and of torrents
 * gone from their clients; @import_id 0 for all. */
gboolean
axis_import_folder_removals(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
);

/* What downloads removed from their clients with their data outside an
 * import (a person giving up on one, a failure put on the blocklist) left,
 * still to be looked at (download.folder_removal pending or waiting): by the
 * same rules, the place's inode recorded when the download completed
 * standing in for an imported file, and never without a folder of Axis's
 * own.  The hourly cleanup (@download_id 0: all of them), and "Delete this
 * download" for the one download it is about, at once.  For a download a
 * person asked to be deleted on the page of its import (that import is
 * dismissed with the reason download_deleted), the import setting that
 * leaves torrents seeding is set aside, and nothing else. */
gboolean
axis_import_download_folder_removals(
	AxisImport               *self,
	gint64                    download_id,
	AxisImportCleanupReport  *report,
	GError                  **error
);

/**
 * AxisImportFolderLook:
 * @state: (nullable): %NULL when it may go; "left" when a rule refuses, for
 *   good; "waiting" when it cannot be told or done now (the client does not
 *   answer, the volume is offline or cannot be written, storage is
 *   read-only); "gone" when nothing is at its place any more
 * @text: (nullable): the rule, or why it waits, in words (a fragment)
 * @torrent: the download is a torrent
 * @at_client: its client lists it now
 * @client_deletes: a torrent its client still has: the client is asked to
 *   remove it with its data and Axis deletes nothing itself
 * @volume, @path: what would go (the reported folder or file; for a Usenet
 *   job reported by a file inside its folder, that folder), on its volume
 * @client_path: its place as the client names it
 * @file: it is one file, not a folder
 * @inode: of what is at the reported place now (0: nothing)
 * @n_files, @bytes: the regular files in it and their sizes
 * @download_state, @client_ref: the download's state and its identifier at
 *   its client, as recorded
 * @client_agrees: for a torrent its client still has: the client said, at
 *   this look, that the torrent is at the place recorded when it completed,
 *   in a folder or file named as the torrent is
 *
 * What deleting a download would do, decided by the first half of the
 * leftover's removal (folder_decide() in axis-import-folder.c) and nothing
 * else, with what would go counted through a read guard and the write guard
 * tried.  Nothing is written or deleted.
 */
typedef struct
{
	gchar    *state;
	gchar    *text;
	gboolean  torrent;
	gboolean  at_client;
	gboolean  client_deletes;
	gint64    volume;
	gchar    *path;
	gchar    *client_path;
	gboolean  file;
	guint64   inode;
	guint     n_files;
	guint64   bytes;
	gchar    *download_state;
	gchar    *client_ref;
	gboolean  client_agrees;
} AxisImportFolderLook;

#define AXIS_IMPORT_FOLDER_LOOK_INIT { NULL, NULL, FALSE, FALSE, FALSE, 0, NULL, NULL, FALSE, 0, 0, 0, NULL, NULL, FALSE }

void
axis_import_folder_look_clear(AxisImportFolderLook *look);

/*
 * The one rule for asking a client to remove a download WITH ITS DATA, read
 * from a look made at that moment (axis_import_download_folder_look()): the
 * client lists the download, the place recorded when it completed holds
 * something and passes every rule of a leftover's removal (@state is %NULL:
 * not in or holding a library root, and the rest), and, for a torrent, whose
 * client deletes it wherever it is, the client says the torrent is at that
 * recorded place.  Everything else is asked without its data or not at all.
 * axis-import-delete.c asks through this and nowhere else.
 */
gboolean
axis_import_delete_with_data(const AxisImportFolderLook *look);

/* The first key of the advisory lock that claims an import's step of "Delete
 * this download" (hashtext() of it; the second key is the import's id modulo
 * 2147483647): one step for an import at a time. */
#define AXIS_IMPORT_DELETE_CLAIM "axis.import.delete_download"

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(AxisImportFolderLook, axis_import_folder_look_clear)

/* Looks at what deleting download @download_id would do, for its import
 * @import_id, which may still be open (it is not counted as another open
 * import in the download's place).  %FALSE only with @error, for the
 * database. */
gboolean
axis_import_download_folder_look(
	AxisImport            *self,
	gint64                 download_id,
	gint64                 import_id,
	AxisImportFolderLook  *look,
	GError               **error
);

/* ── "Delete this download" (axis-import-delete.c, axis-import.c) ── */

/* The grab that brought @download_id for the owner's title @owner_id of
 * @module, as its owner resolves it (0: none, or no such owner); read
 * only. */
gboolean
axis_import_grab_of(
	AxisImport   *self,
	const gchar  *module,
	gint64        download_id,
	gint64        owner_id,
	gint64       *grab_id,
	GError      **error
);

/*
 * In one transaction, with the owner's title locked first (its assign_lock())
 * and then the import's row, the order in which a grab's failure takes them
 * (axis_acquire_handle_failure()); "Dismiss this import" (axis_import_act())
 * locks the import's row alone first, as before.  The import is still in
 * @expect_state (one a dismissal takes:
 * blocked, waiting or partial; "running" with no job counts as waiting) and
 * came from a download; files an interrupted run left under a temporary name
 * are put back, as for a dismissal; the import becomes dismissed with the
 * reason download_deleted, @user's name and client_removal pending; the
 * action delete_download is recorded; and the owner's download_deleted()
 * closes the grab (with @block, the release is blocked by that person).
 * Nothing is asked of a client and nothing is deleted here.  Fails with
 * %AXIS_IMPORT_ERROR_CHANGED when the import is not in @expect_state any
 * more, and as axis_import_act() does otherwise.
 */
gboolean
axis_import_close_deleted(
	AxisImport   *self,
	gint64        import_id,
	const gchar  *expect_state,
	const gchar  *user,
	gboolean      block,
	gint64       *download_id,
	GError      **error
);

/*
 * The requests "Delete this download" recorded that are still to be made
 * (imports dismissed with the reason download_deleted whose client removal is
 * pending or failed; never a dismissed import of another reason), for
 * @import_id or, with 0, for all: the download's place is judged again, the
 * client is asked to remove it (with its data only under
 * axis_import_delete_with_data()), the outcome is recorded on the import
 * (done, gone, kept, failed), and what the client kept is deleted by
 * axis_import_download_folder_removals() for that download.  Called by the
 * post, and by the hourly cleanup; an import whose step another caller is
 * running now is passed over (AXIS_IMPORT_DELETE_CLAIM).
 */
gboolean
axis_import_deleted_downloads(
	AxisImport               *self,
	gint64                    import_id,
	AxisImportCleanupReport  *report,
	GError                  **error
);

G_END_DECLS

#endif /* AXIS_IMPORT_PRIVATE_H */
