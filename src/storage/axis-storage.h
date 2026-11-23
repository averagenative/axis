/*
 * axis-storage.h - volumes, their presence, and library roots
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A volume is one filesystem, known by the UUID in its marker file
 * (axis-volume-marker.h), not by its path.  A disk added while it is
 * mounted read-only, without a marker, is known until its marker can be
 * written by what can be read without writing (AxisVolumeFsIdentity: the
 * filesystem's own identifier, its type and size, and the directory's
 * inode), never by its path either.  An AxisStorage holds what the
 * daemon knows about every volume (loaded from the database at start and
 * kept current by every change and check), checks whether each is there,
 * and is the only way to add, remove or re-point volumes and roots.  Code
 * that touches files on a volume goes through a guard
 * (axis-volume-guard.h), never through a path from here.
 *
 * The promises it keeps:
 *
 *  - A volume is online only while the directory at its path carries its
 *    marker.  An empty directory (an unmounted disk's mount point) is
 *    offline, never an empty volume; a directory that was a mount point
 *    and no longer is, is offline whatever it contains; another marker,
 *    or files without a marker, is a mismatch and is never adopted.  A
 *    volume that has no marker yet is online only while the directory is
 *    not empty and its filesystem identity is exactly the one recorded;
 *    anything else is offline, and its marker is never written there.
 *  - The marker of a volume added without one is written the first time a
 *    check (or a write guard, before anything else is written) finds the
 *    volume recognised and its filesystem mounted read-write: the write the
 *    administrator confirmed when adding it, made late.  From then on the
 *    marker identifies the volume, as for any other.
 *  - No touch of a filesystem can hang the caller: every one runs on a
 *    bounded helper with a deadline (axis-storage-probe.h), and one that
 *    misses it makes the volume offline.
 *  - Nothing here removes or changes a record because a volume is not
 *    online.  Records are removed only by an administrator's request, and
 *    a root only while its volume is online.
 *  - Nothing here writes to a volume except the marker, and only when an
 *    administrator adds a volume and confirms exactly what was shown, when
 *    the marker of a volume so added is written late (above), or when a
 *    deleted marker is restored while storage is not read-only.  No file
 *    is ever deleted, the marker included, but the temporary files an
 *    interrupted marker write left, which a write guard removes once two
 *    looks ten minutes apart found them unchanged.
 *
 * Every function that touches the database or a filesystem blocks: call
 * it from a worker thread.  The others (records, listeners) are for any
 * thread unless said otherwise.
 */

#ifndef AXIS_STORAGE_H
#define AXIS_STORAGE_H

#include <gio/gio.h>
#include <glib.h>

#include "core/axis-setting.h"
#include "core/axis-settings.h"
#include "db/axis-db-pool.h"
#include "storage/axis-storage-error.h"

G_BEGIN_DECLS

#define AXIS_STORAGE_MODULE_ID              "storage"
#define AXIS_STORAGE_SETTING_READ_ONLY      "storage.read_only"
#define AXIS_STORAGE_SETTING_LOW_SPACE      "storage.low_space_percent"
#define AXIS_STORAGE_CHECK_KIND            "storage.check"
#define AXIS_STORAGE_CHECK_SCHEDULE         "storage.check"
#define AXIS_STORAGE_CHECK_INTERVAL_SECONDS 60

/* A check (or guard) that takes longer than this makes a volume offline;
 * writing the marker gets longer. */
#define AXIS_STORAGE_CHECK_TIMEOUT_MS       10000
#define AXIS_STORAGE_WRITE_TIMEOUT_MS       30000

/* Helper threads that may run at once, stuck ones included, and how many
 * volumes a scheduled check looks at at once (the rest of the helpers stay
 * free for guards). */
#define AXIS_STORAGE_MAX_HELPERS            16
#define AXIS_STORAGE_CHECK_WAVE             (AXIS_STORAGE_MAX_HELPERS / 2)

/* Bounds on what an administrator can create. */
#define AXIS_STORAGE_MAX_VOLUMES            128
#define AXIS_STORAGE_MAX_ROOTS_PER_VOLUME   256

typedef struct _AxisStorage AxisStorage;

/* ── States ─────────────────────────────────────────────────────── */

/**
 * AxisVolumeState:
 * @AXIS_VOLUME_ONLINE: the marker is present and matches
 * @AXIS_VOLUME_READ_ONLY: as online, on a filesystem mounted read-only:
 *   read guards are granted, write guards refused
 * @AXIS_VOLUME_OFFLINE: not there now (see the reason); retry later
 * @AXIS_VOLUME_MISMATCH: something else is at the path; a person has to
 *   look
 */
typedef enum
{
	AXIS_VOLUME_ONLINE,
	AXIS_VOLUME_READ_ONLY,
	AXIS_VOLUME_OFFLINE,
	AXIS_VOLUME_MISMATCH
} AxisVolumeState;

/**
 * AxisVolumeReason:
 * @AXIS_VOLUME_REASON_NONE: online or read-only
 * @AXIS_VOLUME_REASON_UNCHECKED: offline: not checked since it was loaded
 * @AXIS_VOLUME_REASON_PATH_MISSING: offline: no directory at the path
 * @AXIS_VOLUME_REASON_NOT_MOUNTED: offline: the directory was a mount point
 *   when the marker last matched and is not one now
 * @AXIS_VOLUME_REASON_EMPTY: offline: no marker and an empty directory,
 *   which is what an unmounted disk leaves behind
 * @AXIS_VOLUME_REASON_TIMEOUT: offline: the filesystem did not answer in
 *   time
 * @AXIS_VOLUME_REASON_IO_ERROR: offline: an I/O or permission error
 * @AXIS_VOLUME_REASON_NO_MARKER: mismatch: no marker, but the directory
 *   has files (another disk, or the marker was deleted)
 * @AXIS_VOLUME_REASON_OTHER_MARKER: mismatch: another volume's marker
 * @AXIS_VOLUME_REASON_BAD_MARKER: mismatch: a marker that is not valid
 * @AXIS_VOLUME_REASON_NOT_DIRECTORY: mismatch: the path is, or passes
 *   through, a symbolic link, or is not a directory
 * @AXIS_VOLUME_REASON_OTHER_FILESYSTEM: offline: a volume that has no
 *   marker yet, and the directory at its path is not the filesystem and
 *   directory recorded (its identifier, type, size or inode differ): another
 *   disk, or the disk changed; never guessed to be the volume
 */
typedef enum
{
	AXIS_VOLUME_REASON_NONE,
	AXIS_VOLUME_REASON_UNCHECKED,
	AXIS_VOLUME_REASON_PATH_MISSING,
	AXIS_VOLUME_REASON_NOT_MOUNTED,
	AXIS_VOLUME_REASON_EMPTY,
	AXIS_VOLUME_REASON_TIMEOUT,
	AXIS_VOLUME_REASON_IO_ERROR,
	AXIS_VOLUME_REASON_NO_MARKER,
	AXIS_VOLUME_REASON_OTHER_MARKER,
	AXIS_VOLUME_REASON_BAD_MARKER,
	AXIS_VOLUME_REASON_NOT_DIRECTORY,
	AXIS_VOLUME_REASON_OTHER_FILESYSTEM
} AxisVolumeReason;

/* "online", "read_only", "offline", "mismatch" (as stored) */
const gchar *
axis_volume_state_to_string(AxisVolumeState state);

gboolean
axis_volume_state_from_string(
	const gchar     *text,
	AxisVolumeState *state
);

/* "Online", "Read-only", "Offline", "Mismatch" */
const gchar *
axis_volume_state_label(AxisVolumeState state);

/* Returns: %TRUE for online and read-only: the states a guard accepts */
gboolean
axis_volume_state_is_usable(AxisVolumeState state);

/* Returns: %TRUE when a new title may be put on a volume in @state now:
 * online, not mounted read-only (a disk that is away, or one Axis can
 * only read, would take a title's searches and downloads and never its
 * import).  The setting "storage.read_only" is not a state: an import waits
 * for it to be off (axis_storage_is_read_only_mode()). */
gboolean
axis_volume_state_is_writable(AxisVolumeState state);

/* Why a volume in @state cannot take a new title now, in words to follow
 * "it cannot be written to now: " ("its disk is offline"); %NULL when it
 * can. */
const gchar *
axis_volume_state_unwritable_why(AxisVolumeState state);

/* "none", "path_missing", ... (as stored) */
const gchar *
axis_volume_reason_to_string(AxisVolumeReason reason);

gboolean
axis_volume_reason_from_string(
	const gchar      *text,
	AxisVolumeReason *reason
);

/* One sentence for a page or a log line. */
const gchar *
axis_volume_reason_describe(AxisVolumeReason reason);

/**
 * AxisMediaType:
 *
 * What a library root holds.
 */
typedef enum
{
	AXIS_MEDIA_MOVIES,
	AXIS_MEDIA_TV,
	AXIS_MEDIA_MUSIC,
	AXIS_MEDIA_AUDIOBOOKS,
	AXIS_MEDIA_BOOKS,
	AXIS_MEDIA_DOWNLOADS,
	AXIS_MEDIA_OTHER,
	AXIS_MEDIA_YOUTUBE	/* what the YouTube downloader saves (youtube/axis-youtube.h) */
} AxisMediaType;

#define AXIS_MEDIA_TYPE_COUNT 8

/* "movies", "tv", ... (as stored and posted) */
const gchar *
axis_media_type_to_string(AxisMediaType type);

gboolean
axis_media_type_from_string(
	const gchar   *text,
	AxisMediaType *type
);

/* "Movies", "TV", ... */
const gchar *
axis_media_type_label(AxisMediaType type);

/* ── Records ────────────────────────────────────────────────────── */

/**
 * AxisVolumeFsIdentity:
 * @magic: the filesystem's type (statfs f_type)
 * @fsid: its identifier (statfs f_fsid) in hexadecimal; only one the
 *   filesystem reports of its own, never a device number
 *   (axis_storage_fs_fsid_is_device()), unless @fs_uuid is known
 * @fs_uuid: its UUID as the kernel gives it (axis_storage_fs_read_uuid()),
 *   "" when the kernel gave none when it was added
 * @ino: the inode number of the volume's directory
 * @bytes: the filesystem's size (blocks times fragment size)
 *
 * What a volume added without a marker is recognised by until its marker
 * is written: all of it must be the same at a check, @fs_uuid too when it
 * was known, and the directory must not be empty.  The path, the device
 * number and the mount's id are not part of it: the first never identifies
 * a disk, and the other two change at every boot or mount.
 */
typedef struct
{
	guint64 magic;
	gchar   fsid[17];
	gchar   fs_uuid[33];
	guint64 ino;
	guint64 bytes;
} AxisVolumeFsIdentity;

/**
 * AxisVolumeInfo:
 * @id: the volume's row
 * @uuid: its marker's identifier
 * @name: its name
 * @path: where it is looked for; for showing, never for opening files
 * @mount_root: the directory was a mount point when the marker last
 *   matched (once set, a directory that is no longer a mount point is
 *   offline)
 * @fs_type: the filesystem type when the marker last matched
 * @fsid: its filesystem id then (advisory)
 * @device: its device ("major:minor") then (advisory)
 * @state: the last check's result
 * @reason: why, when not online
 * @state_changed_us: since when (microseconds since 1970)
 * @last_checked_us: the last check, 0 for never
 * @last_online_us: the last check that found it online, 0 for never
 * @has_space: whether the space fields are known
 * @total_bytes: the filesystem's size at the last check that found it
 * @free_bytes: free space then
 * @available_bytes: free space the daemon may use then
 * @space_checked_us: when the space was read
 * @marker_created_us: when its marker was written, 0 while it has none
 * @created_us: when it was added
 * @n_roots: how many library roots are on it
 * @has_marker: it is recognised by its marker (every volume but one added
 *   from a read-only disk without a marker, until the marker is written)
 * @fs_identity: what it is recognised by while it has no marker
 * @marker_problem: (nullable): why the last attempt to write its marker
 *   failed, while it has none
 * @fs_uuid: (nullable): its filesystem's UUID as the kernel gave it when it
 *   was last found (advisory, as @fsid and @device: evidence of which
 *   filesystem it is on, never what a volume with a marker is recognised by)
 *
 * A copy of what the daemon knows about a volume.
 */
typedef struct
{
	gint64            id;
	gchar            *uuid;
	gchar            *name;
	gchar            *path;
	gboolean          mount_root;
	gchar            *fs_type;
	gchar            *fsid;
	gchar            *device;
	AxisVolumeState   state;
	AxisVolumeReason  reason;
	gint64            state_changed_us;
	gint64            last_checked_us;
	gint64            last_online_us;
	gboolean          has_space;
	guint64           total_bytes;
	guint64           free_bytes;
	guint64           available_bytes;
	gint64            space_checked_us;
	gint64                marker_created_us;
	gint64                created_us;
	guint                 n_roots;
	gboolean              has_marker;
	AxisVolumeFsIdentity  fs_identity;
	gchar                *marker_problem;
	gchar                *fs_uuid;
	/* Since when it has been offline or not recognised (microseconds since
	 * 1970), 0 while it is usable.  Unlike @state_changed_us it is kept
	 * while the disk stays away, whatever the reason becomes. */
	gint64                unusable_since_us;
} AxisVolumeInfo;

/* "ext4 filesystem 03350eabfcdf1e9a (UUID 121e...), 1.9 GiB, directory
 * inode 2": what a volume without a marker is recognised by, for pages and
 * logs. */
gchar *
axis_volume_fs_identity_describe(const AxisVolumeFsIdentity *identity);

AxisVolumeInfo *
axis_volume_info_copy(const AxisVolumeInfo *info);

void
axis_volume_info_free(AxisVolumeInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVolumeInfo, axis_volume_info_free)

/**
 * AxisRootInfo:
 * @id: the root's row
 * @volume_id: its volume
 * @rel_path: its directory relative to the volume's top ("" for the top)
 * @media_type: what it holds
 * @name: its name
 * @created_us: when it was added
 */
typedef struct
{
	gint64         id;
	gint64         volume_id;
	gchar         *rel_path;
	AxisMediaType  media_type;
	gchar         *name;
	gint64         created_us;
} AxisRootInfo;

void
axis_root_info_free(AxisRootInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisRootInfo, axis_root_info_free)

/* ── Lifetime ───────────────────────────────────────────────────── */

/**
 * axis_storage_new:
 * @pool: the database; must outlive the result
 * @settings: (nullable): where "storage.read_only" is read; %NULL (tests)
 *   is read-only mode, always
 *
 * Create on the main thread: listeners are called on its main context.
 * Reference counted.
 *
 * Returns: (transfer full): the storage, with no volumes until
 *   axis_storage_load()
 */
AxisStorage *
axis_storage_new(
	AxisDbPool   *pool,
	AxisSettings *settings
);

AxisStorage *
axis_storage_ref(AxisStorage *self);

void
axis_storage_unref(AxisStorage *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisStorage, axis_storage_unref)

/**
 * axis_storage_load:
 *
 * Reads the volumes from the database, each in the state its last check
 * stored.  Blocks; call it once, before anything else uses @self.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_load(
	AxisStorage  *self,
	GError      **error
);

/**
 * axis_storage_stop:
 *
 * No listener is called after this.  Checks still running on helper
 * threads finish on their own; nothing waits for a hung one.
 */
void
axis_storage_stop(AxisStorage *self);

/* The setting declarations of the storage module, for its AxisModuleInfo. */
const AxisSettingInfo *
axis_storage_get_setting_table(void);

/* Whether "storage.read_only" is on (always, without a settings store). */
gboolean
axis_storage_is_read_only_mode(AxisStorage *self);

/* "storage.low_space_percent": a disk with less free space than this share
 * of its size has a notice on the page "Needs attention"; 0 for never (also
 * without a settings store).  Any thread. */
guint
axis_storage_get_low_space_percent(AxisStorage *self);

/* The database the storage was made with (for pages that read beside it). */
AxisDbPool *
axis_storage_get_pool(AxisStorage *self);

/* ── For tests ──────────────────────────────────────────────────── */

/* How far apart, in seconds by the monotonic clock, two looks at a volume's
 * top must have found a temporary marker file the same before a write guard
 * removes it (AXIS_VOLUME_MARKER_STALE_SECONDS; 0: the next look). */
void
axis_storage_set_marker_sweep_age(
	AxisStorage *self,
	guint        seconds
);

/* Sets a known volume's state as a check would have found it, until the
 * next check: a disk mounted read-only, which a test cannot mount without
 * privileges. */
void
axis_storage_force_volume_state(
	AxisStorage     *self,
	gint64           id,
	AxisVolumeState  state
);

/* Shorter deadlines (milliseconds, 0 keeps the current one). */
void
axis_storage_set_timeouts(
	AxisStorage *self,
	guint        check_ms,
	guint        write_ms
);

/**
 * AxisStorageCheckHook:
 * @path: the volume path being checked
 * @user_data: (closure): as given
 *
 * Called on the helper thread of every check, after the directory was
 * opened and before its marker is read: a test makes a check hang by
 * blocking here.
 */
typedef void (*AxisStorageCheckHook)(
	const gchar *path,
	gpointer     user_data
);

void
axis_storage_set_check_hook(
	AxisStorage          *self,
	AxisStorageCheckHook  hook,
	gpointer              user_data
);

/* Helper threads whose work has not returned, and those of them that
 * missed their deadline. */
guint
axis_storage_get_helpers_running(AxisStorage *self);

guint
axis_storage_get_helpers_abandoned(AxisStorage *self);

/* ── Volumes ────────────────────────────────────────────────────── */

/**
 * axis_storage_list_volumes:
 *
 * No I/O: what the daemon knows, as of the last check.
 *
 * Returns: (transfer full) (element-type AxisVolumeInfo): copies, by name
 */
GPtrArray *
axis_storage_list_volumes(AxisStorage *self);

/* Returns: (transfer full) (nullable): a copy, or %NULL when there is none */
AxisVolumeInfo *
axis_storage_get_volume(
	AxisStorage *self,
	gint64       id
);

/**
 * axis_storage_check_volume:
 * @self: the storage
 * @id: a volume
 * @info: (out) (optional) (transfer full): the volume afterwards
 * @error: (nullable): return location for an error
 *
 * Checks the volume now (a helper with a deadline), stores the result and
 * its free space, logs a change of state and tells the listeners.  A
 * volume without a marker that is recognised and mounted read-write gets
 * its marker now (axis_storage_write_late_marker()); one whose marker is
 * found there, written by an earlier attempt, is recognised by it from now
 * on.  A volume that turns out not to be online is not an error.  Fails with
 * %AXIS_STORAGE_ERROR_NOT_FOUND for an unknown id and with a database
 * error when the result could not be stored (the daemon's own view is
 * updated anyway).
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_check_volume(
	AxisStorage     *self,
	gint64           id,
	AxisVolumeInfo **info,
	GError         **error
);

/**
 * axis_storage_check_all:
 *
 * As axis_storage_check_volume() for every volume, the checks running in
 * parallel; what the scheduled storage.check job runs.  Stops early (with
 * %G_IO_ERROR_CANCELLED) when @cancellable is cancelled before the results
 * are stored.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_check_all(
	AxisStorage   *self,
	GCancellable  *cancellable,
	GError       **error
);

/**
 * AxisVolumePlan:
 * @AXIS_VOLUME_PLAN_REFUSE: the directory cannot become a volume
 * @AXIS_VOLUME_PLAN_WRITE_MARKER: a marker will be written
 * @AXIS_VOLUME_PLAN_ADOPT: a marker is there and will be used as it is
 * @AXIS_VOLUME_PLAN_IDENTIFY: the filesystem is mounted read-only and has
 *   no marker: nothing is written now, the volume is recognised by its
 *   filesystem identity (AxisVolumeFsIdentity), and its marker is written
 *   the first time it is found mounted read-write
 */
typedef enum
{
	AXIS_VOLUME_PLAN_REFUSE,
	AXIS_VOLUME_PLAN_WRITE_MARKER,
	AXIS_VOLUME_PLAN_ADOPT,
	AXIS_VOLUME_PLAN_IDENTIFY
} AxisVolumePlan;

/* "refuse", "write", "adopt", "identify" (as posted) */
const gchar *
axis_volume_plan_to_string(AxisVolumePlan plan);

gboolean
axis_volume_plan_from_string(
	const gchar    *text,
	AxisVolumePlan *plan
);

/**
 * AxisVolumeInspection:
 * @path: the normalised path
 * @suggested_name: a name for it (its last segment)
 * @plan: what adding it would do
 * @refusal: (nullable): why it cannot be added, when @plan is REFUSE
 * @warnings: (element-type utf8): what the administrator should know
 * @mount_root: the directory is a mount point
 * @read_only: its filesystem is mounted read-only
 * @empty: it has no entries (only meaningful without a marker)
 * @fs_type: its filesystem type, when it could be opened
 * @device: its device ("major:minor"), when it could be opened
 * @dev: its device number, for the confirmation
 * @ino: its inode number, for the confirmation
 * @fsid: (nullable): its filesystem id, for the confirmation (the top of
 *   every filesystem of one type has the same inode, and device numbers
 *   are reused, so these tell two disks apart where those cannot)
 * @has_space: whether the space fields are known
 * @total_bytes: the filesystem's size
 * @free_bytes: its free space
 * @available_bytes: free space the daemon may use
 * @marker_uuid: (nullable): the identifier of the marker found there
 * @marker_created_us: when that marker was written
 * @fs_uuid: (nullable): the filesystem's UUID, when the kernel gave one
 *
 * What is at a path an administrator wants to add, and what adding it
 * would do.  Nothing is written to find this out.
 */
typedef struct
{
	gchar          *path;
	gchar          *suggested_name;
	AxisVolumePlan  plan;
	gchar          *refusal;
	GPtrArray      *warnings;
	gboolean        mount_root;
	gboolean        read_only;
	gboolean        empty;
	gchar          *fs_type;
	gchar          *device;
	guint64         dev;
	guint64         ino;
	gchar          *fsid;
	gboolean        has_space;
	guint64         total_bytes;
	guint64         free_bytes;
	guint64         available_bytes;
	gchar          *marker_uuid;
	gint64          marker_created_us;
	gchar          *fs_uuid;
} AxisVolumeInspection;

void
axis_volume_inspection_free(AxisVolumeInspection *inspection);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisVolumeInspection, axis_volume_inspection_free)

/**
 * axis_storage_inspect_volume:
 * @self: the storage
 * @path: the path as typed
 * @inspection: (out) (transfer full): what is there
 * @error: (nullable): return location for an error
 *
 * Looks at the directory (on a helper, with a deadline) and compares it
 * with the registered volumes.  Fails with %AXIS_STORAGE_ERROR_INVALID for
 * a malformed path and with a database error; anything about the
 * directory itself is reported in the inspection's plan.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_inspect_volume(
	AxisStorage           *self,
	const gchar           *path,
	AxisVolumeInspection **inspection,
	GError               **error
);

/**
 * AxisVolumeAddRequest:
 * @path: the path as typed (normalised again)
 * @name: the name as typed
 * @plan: what the administrator was shown and confirmed (write, adopt or
 *   identify)
 * @marker_uuid: (nullable): for adopt, the marker that was shown
 * @dev: the directory that was shown (AxisVolumeInspection.dev)
 * @ino: and its inode
 * @fsid: (nullable): and its filesystem id
 * @total_bytes: and its filesystem's size
 * @not_mount_point_ok: the administrator confirmed that the directory is
 *   not a mount point and should be a volume anyway
 */
typedef struct
{
	const gchar    *path;
	const gchar    *name;
	AxisVolumePlan  plan;
	const gchar    *marker_uuid;
	guint64         dev;
	guint64         ino;
	const gchar    *fsid;
	guint64         total_bytes;
	gboolean        not_mount_point_ok;
} AxisVolumeAddRequest;

/**
 * axis_storage_add_volume:
 * @self: the storage
 * @request: what was confirmed
 * @id: (out) (optional): the new volume
 * @error: (nullable): return location for an error
 *
 * Adds a volume, serialised with every other change of volumes (also
 * across daemons sharing the database).  The directory is inspected again
 * through the descriptor that is then used; if that does not lead to
 * exactly the plan, directory and marker that were confirmed, nothing is
 * done and it fails with %AXIS_STORAGE_ERROR_CONFIRM.  Writing a marker
 * is the one write allowed in read-only mode.  With the identify plan
 * nothing is written: the volume gets a new identifier, recorded with its
 * filesystem identity, and that identifier goes into the marker when it is
 * written later.  Fails with
 * %AXIS_STORAGE_ERROR_INVALID for malformed input,
 * %AXIS_STORAGE_ERROR_REFUSED when the directory cannot become a volume
 * (duplicate, nested, same filesystem, the name is taken, a directory
 * that is not a mount point without @not_mount_point_ok),
 * %AXIS_STORAGE_ERROR_UNAVAILABLE when the filesystem does not answer in
 * time, %AXIS_STORAGE_ERROR_FAILED when the marker cannot be written, and
 * with a database error.  A marker written before a later failure is left
 * in place; adding the directory again adopts it.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_add_volume(
	AxisStorage                 *self,
	const AxisVolumeAddRequest  *request,
	gint64                      *id,
	GError                     **error
);

/**
 * axis_storage_remove_volume:
 * @self: the storage
 * @id: a volume
 * @allow_unavailable: the administrator confirmed removing a volume that
 *   is not online (a disk that is gone for good)
 * @error: (nullable): return location for an error
 *
 * Forgets the volume and its roots.  Touches no file: its marker stays on
 * the disk, so adding it again later adopts the same identity.  Unless
 * @allow_unavailable, the volume is verified as a read guard verifies it,
 * and it fails with %AXIS_STORAGE_ERROR_UNAVAILABLE, removing nothing,
 * when that does not succeed now (offline, mismatched, hung, or the
 * helper too busy to look); with %AXIS_STORAGE_ERROR_NOT_FOUND for an
 * unknown id; and with a database error.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_remove_volume(
	AxisStorage  *self,
	gint64        id,
	gboolean      allow_unavailable,
	GError      **error
);

/**
 * axis_storage_relocate_volume:
 * @self: the storage
 * @id: a volume
 * @path: the new path as typed
 * @not_mount_point_ok: as for adding
 * @error: (nullable): return location for an error
 *
 * Points a volume at a new path, after its disk was mounted somewhere
 * else.  The new path must carry this volume's marker (a volume without
 * one: its recorded filesystem identity, and not be empty), must not overlap
 * another volume, and the volume must not also still be present at its
 * old path (two copies of one marker).  Writes nothing.  Fails as
 * axis_storage_add_volume() does, and with %AXIS_STORAGE_ERROR_REFUSED
 * when the marker at the new path is missing or another.
 *
 * Two disks swapped between their mount points: when @path is exactly
 * another volume's path, and that volume is proven at this one's path (its
 * marker there, or its recorded filesystem identity), the two change paths
 * in one statement, so that no moment has two volumes at one path.  When it
 * is not proven there, refused, naming it.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_relocate_volume(
	AxisStorage  *self,
	gint64        id,
	const gchar  *path,
	gboolean      not_mount_point_ok,
	GError      **error
);

/**
 * AxisMarkerRestoreFs:
 * @AXIS_MARKER_RESTORE_FS_SAME: the filesystem at the path is the one the
 *   volume was last seen on: the same type and an identifier of its own
 *   (statfs f_fsid, not a device number) that is the one recorded
 * @AXIS_MARKER_RESTORE_FS_DEVICE_ONLY: the same type and size, and the same
 *   identifier, but that identifier is only the device's number (xfs,
 *   f2fs): Axis cannot tell this disk from another found at that number
 * @AXIS_MARKER_RESTORE_FS_DIFFERS: another type, identifier or size
 * @AXIS_MARKER_RESTORE_FS_UNKNOWN: nothing is recorded to compare, or the
 *   filesystem's details cannot be read
 *
 * What the filesystem at a volume's path is, against what the volume was
 * last seen on, for restoring its marker.  Only %AXIS_MARKER_RESTORE_FS_SAME
 * needs no confirmation that it is the volume's disk.
 */
typedef enum
{
	AXIS_MARKER_RESTORE_FS_SAME,
	AXIS_MARKER_RESTORE_FS_DEVICE_ONLY,
	AXIS_MARKER_RESTORE_FS_DIFFERS,
	AXIS_MARKER_RESTORE_FS_UNKNOWN
} AxisMarkerRestoreFs;

/**
 * AxisMarkerRestoreInspection:
 * @reason: why the volume is not online now
 * @possible: whether a marker can be restored (only when the directory has
 *   files and no marker)
 * @dev: the directory, for the confirmation
 * @ino: and its inode
 * @fsid: and its filesystem id ("" when unknown)
 * @filesystem: the filesystem there against the one recorded
 * @filesystem_text: (nullable): what that means, in a sentence or two, for
 *   the page (%NULL when nothing could be looked at)
 * @roots: (element-type AxisRootInfo): the volume's roots
 * @root_present: (element-type gboolean): for each root, whether its
 *   directory is there
 */
typedef struct
{
	AxisVolumeReason     reason;
	gboolean             possible;
	guint64              dev;
	guint64              ino;
	gchar                fsid[17];
	AxisMarkerRestoreFs  filesystem;
	gchar               *filesystem_text;
	GPtrArray           *roots;
	GArray              *root_present;
} AxisMarkerRestoreInspection;

void
axis_marker_restore_inspection_free(AxisMarkerRestoreInspection *inspection);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AxisMarkerRestoreInspection, axis_marker_restore_inspection_free)

/**
 * axis_storage_inspect_marker_restore:
 *
 * What is at a mismatched volume's path that has files but no marker, and
 * which of the volume's roots are there, so that an administrator can tell
 * whether it is the right disk.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_inspect_marker_restore(
	AxisStorage                  *self,
	gint64                        id,
	AxisMarkerRestoreInspection **inspection,
	GError                      **error
);

/**
 * axis_storage_restore_marker:
 * @self: the storage
 * @id: a volume whose directory has files and no marker
 * @dev: the directory that was shown
 * @ino: and its inode
 * @fsid: and its filesystem id
 * @other_filesystem_ok: the administrator confirmed that this is the
 *   volume's disk although its filesystem is not proven the one recorded
 *   (anything but %AXIS_MARKER_RESTORE_FS_SAME)
 * @error: (nullable): return location for an error
 *
 * Writes the volume's marker (its own UUID) again.  Refused with
 * %AXIS_STORAGE_ERROR_READ_ONLY in read-only mode, with
 * %AXIS_STORAGE_ERROR_REFUSED unless the directory has files and no
 * marker (never into an empty directory) and when it is on the filesystem
 * of another volume (as adding refuses it), and with
 * %AXIS_STORAGE_ERROR_CONFIRM when it is not the directory that was shown,
 * or its filesystem is not proven the one recorded and that was not
 * confirmed.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_restore_marker(
	AxisStorage  *self,
	gint64        id,
	guint64       dev,
	guint64       ino,
	const gchar  *fsid,
	gboolean      other_filesystem_ok,
	GError      **error
);

/* ── Roots ──────────────────────────────────────────────────────── */

/**
 * axis_storage_list_roots:
 * @self: the storage
 * @volume_id: a volume, or 0 for every root
 * @roots: (out) (transfer full) (element-type AxisRootInfo): by volume
 *   and path
 * @error: (nullable): return location for an error
 *
 * Reads the database.  Returns: %TRUE on success
 */
gboolean
axis_storage_list_roots(
	AxisStorage  *self,
	gint64        volume_id,
	GPtrArray   **roots,
	GError      **error
);

/**
 * axis_storage_get_root:
 *
 * Fails with %AXIS_STORAGE_ERROR_NOT_FOUND when there is no such root.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_get_root(
	AxisStorage   *self,
	gint64         id,
	AxisRootInfo **root,
	GError       **error
);

/**
 * axis_storage_add_root:
 * @self: the storage
 * @volume_id: the volume
 * @path: the directory as typed: relative to the volume, or absolute and
 *   inside the volume's path
 * @media_type: what it holds
 * @name: (nullable): the name as typed; %NULL or empty names it after its
 *   directory (or the volume, for its top)
 * @id: (out) (optional): the new root
 * @error: (nullable): return location for an error
 *
 * The volume must be online (a read guard is taken), and the directory
 * must exist and be reached from the volume's top without a symbolic link
 * or another filesystem; it must not contain or lie inside another root
 * of the volume.  Fails with %AXIS_STORAGE_ERROR_INVALID,
 * %AXIS_STORAGE_ERROR_UNAVAILABLE, %AXIS_STORAGE_ERROR_NOT_CONTAINED,
 * %AXIS_STORAGE_ERROR_REFUSED (missing, nested, duplicate, too many) or
 * %AXIS_STORAGE_ERROR_NOT_FOUND (no such volume), and with a database
 * error.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_add_root(
	AxisStorage    *self,
	gint64          volume_id,
	const gchar    *path,
	AxisMediaType   media_type,
	const gchar    *name,
	gint64         *id,
	GError        **error
);

/**
 * axis_storage_remove_root:
 *
 * Forgets a root.  Touches no file.  Refused with
 * %AXIS_STORAGE_ERROR_UNAVAILABLE while its volume is not online (a check
 * is made now): nothing about a volume is forgotten while it is away.
 *
 * Returns: %TRUE on success
 */
gboolean
axis_storage_remove_root(
	AxisStorage  *self,
	gint64        id,
	GError      **error
);

/* ── Listening ──────────────────────────────────────────────────── */

/**
 * AxisStorageListener:
 * @storage: the storage
 * @volume: (nullable): the volume after a check, or %NULL when volumes
 *   were added or removed (read the list again)
 * @state_changed: whether the check changed its state
 * @user_data: (closure): as given
 *
 * Called on the main context @storage was created on.
 */
typedef void (*AxisStorageListener)(
	AxisStorage          *storage,
	const AxisVolumeInfo *volume,
	gboolean              state_changed,
	gpointer              user_data
);

/* Main thread only.  Returns: an id for axis_storage_remove_listener() */
guint
axis_storage_add_listener(
	AxisStorage         *self,
	AxisStorageListener  func,
	gpointer             user_data
);

void
axis_storage_remove_listener(
	AxisStorage *self,
	guint        listener_id
);

G_END_DECLS

#endif /* AXIS_STORAGE_H */
