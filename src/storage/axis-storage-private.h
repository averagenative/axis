/*
 * axis-storage-private.h - shared by the files of src/storage
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef AXIS_STORAGE_PRIVATE_H
#define AXIS_STORAGE_PRIVATE_H

#include "storage/axis-storage.h"
#include "storage/axis-storage-fs.h"
#include "storage/axis-storage-probe.h"
#include "storage/axis-volume-marker.h"

G_BEGIN_DECLS

/* The one lock every change of volumes takes in the database, so that two
 * changes (in this daemon or another on the same database) never decide
 * on the same picture. */
#define AXIS_STORAGE_ADVISORY_LOCK_SQL "SELECT pg_advisory_xact_lock(hashtext('axis.storage.volumes'))"

/* The columns read into an AxisVolumeInfo, in order. */
#define AXIS_VOLUME_COLUMNS \
	"id, uuid::text, name, path, mount_root, fs_type, fsid, device, state, state_reason, " \
	"(extract(epoch FROM state_changed_at) * 1000000)::bigint, " \
	"(extract(epoch FROM last_checked_at) * 1000000)::bigint, " \
	"(extract(epoch FROM last_online_at) * 1000000)::bigint, " \
	"total_bytes, free_bytes, available_bytes, " \
	"(extract(epoch FROM space_checked_at) * 1000000)::bigint, " \
	"(extract(epoch FROM marker_created_at) * 1000000)::bigint, " \
	"(extract(epoch FROM created_at) * 1000000)::bigint, " \
	"(SELECT count(*) FROM library_root r WHERE r.volume_id = volume.id), " \
	"identity, ident_magic, ident_fsid, ident_fs_uuid, ident_ino, ident_bytes, marker_problem, fs_uuid, " \
	"(extract(epoch FROM unusable_since) * 1000000)::bigint"

typedef struct
{
	guint                id;
	AxisStorageListener  func;
	gpointer             user_data;
} AxisStorageListenerEntry;

/* The looks at one volume's top for stale temporary marker files. */
typedef struct
{
	gint64      last_us;	/* the latest look (monotonic) */
	GHashTable *seen;		/* what it found (axis_volume_marker_seen_new()); %NULL while a look runs */
} AxisStorageSweep;

void
axis_storage_sweep_free(gpointer sweep);

struct _AxisStorage
{
	gint                  ref_count;	/* atomic */
	AxisDbPool           *pool;
	AxisSettings         *settings;	/* nullable, owned reference */
	AxisStorageProbe     *probe;
	GMainContext         *context;	/* where listeners are called */
	gint                  check_timeout_ms;	/* atomic */
	gint                  write_timeout_ms;	/* atomic */
	gint                  stopped;	/* atomic */

	GMutex                lock;
	GHashTable           *volumes;	/* (lock) gint64* id -> AxisVolumeInfo* */
	GHashTable           *swept;	/* (lock) gint64* id -> AxisStorageSweep*: the looks at its top for
					 * stale temporary marker files */
	gint                  sweep_age_s;	/* atomic: how far apart those looks must be */
	AxisStorageCheckHook  hook;		/* (lock) */
	gpointer              hook_data;	/* (lock) */

	GMutex                check_lock;	/* one check result stored at a time */

	GArray               *listeners;	/* main context only */
	guint                 next_listener;
};

/*
 * A look at a directory, made on a helper thread.  The inputs are set by
 * the caller; axis_volume_probe_run() fills the rest and touches nothing
 * else.  It owns its descriptors: @held_fd is a dup() made for it, and @fd
 * is closed when the probe is freed unless the caller took it.
 */
typedef struct
{
	/* In */
	gchar                  *path;
	gint                    held_fd;	/* -1, or a guard's directory to verify */
	guint64                 held_dev;
	guint64                 held_ino;
	gboolean                held_has_mnt_id;	/* and the mount it was on */
	guint64                 held_mnt_id;
	gboolean                keep_fd;	/* keep the opened directory in @fd */
	gboolean                want_empty;	/* without a marker, look if it is empty */
	gboolean                want_fs_uuid;	/* read the filesystem's UUID (a local one) */
	AxisStorageCheckHook    hook;
	gpointer                hook_data;

	/* Out */
	gint                    open_errno;	/* 0 when the directory was opened */
	gint                    fd;		/* -1 unless kept */
	AxisStorageFsIdentity   identity;
	gboolean                replaced;	/* the path leads elsewhere than @held_fd (or onto another mount) */
	AxisVolumeMarkerStatus  marker_status;
	AxisVolumeMarker        marker;
	gint                    marker_errno;
	gboolean                empty_known;
	gboolean                empty;
	gint                    empty_errno;
	gboolean                info_ok;
	AxisStorageFsInfo       info;
	gint                    info_errno;
	gchar                   fs_uuid[AXIS_STORAGE_FS_UUID_CHARS + 1];	/* "" when none */
} AxisVolumeProbe;

/* A probe of @path with nothing held; the caller sets the other inputs. */
AxisVolumeProbe *
axis_volume_probe_new(const gchar *path);

void
axis_volume_probe_free(gpointer probe);

/*
 * Runs @probe on a helper, keyed by @key, and waits up to @timeout_ms.
 * Returns the call (read the probe with axis_storage_probe_call_get_data(),
 * then unref the call), or %NULL with %AXIS_STORAGE_ERROR_UNAVAILABLE when
 * it missed the deadline or could not start (@probe is the call's then).
 */
AxisStorageProbeCall *
axis_storage_run_probe(
	AxisStorage      *self,
	const gchar      *key,
	AxisVolumeProbe  *probe,
	guint             timeout_ms,
	GError          **error
);

/*
 * What a probe of a volume's path means for the volume @record: its marker
 * (@record->uuid) when it has one, else its filesystem identity, which
 * needs a probe made with want_empty and want_fs_uuid.  @expect_mount_root:
 * a directory that is not a mount point is offline.  A matching marker of
 * a volume that has none recorded counts, too: it is the volume's own.
 */
void
axis_volume_probe_classify(
	const AxisVolumeProbe *probe,
	const AxisVolumeInfo  *record,
	gboolean               expect_mount_root,
	AxisVolumeState       *state,
	AxisVolumeReason      *reason
);

/*
 * Whether what @probe found can recognise a disk without a marker, and its
 * identity in *@identity when it can; otherwise a sentence why not (the
 * caller frees it).  Needs a probe that read the filesystem's details and
 * was made with want_fs_uuid.
 */
gchar *
axis_volume_probe_fs_identity(
	const AxisVolumeProbe *probe,
	AxisVolumeFsIdentity  *identity
);

/* Whether what @probe found is the filesystem and directory @identity
 * records; when not, *@what names the part that differs. */
gboolean
axis_volume_fs_identity_matches(
	const AxisVolumeFsIdentity  *identity,
	const AxisVolumeProbe       *probe,
	const gchar                **what
);

/*
 * How sure Axis is that the directory @probe opened is on the filesystem
 * the volume @row was last seen on (or was added with, without a marker):
 *
 *   PROVEN       an identifier of the filesystem's own (statfs f_fsid, not
 *                a device number) is the one recorded; or, where the
 *                identifier is only the device's number, the filesystem's
 *                UUID is the one recorded
 *   SAME_DEVICE  only the device number is the one last seen, which another
 *                disk can have after a reboot
 *   OTHER        neither
 *
 * One filesystem is one volume: PROVEN refuses another volume on it whether
 * or not @row is online; SAME_DEVICE refuses it only while @row is online.
 * Needs a probe made with want_fs_uuid.
 */
typedef enum
{
	AXIS_VOLUME_FS_OTHER,
	AXIS_VOLUME_FS_SAME_DEVICE,
	AXIS_VOLUME_FS_PROVEN
} AxisVolumeFsSameness;

AxisVolumeFsSameness
axis_volume_probe_same_filesystem(
	const AxisVolumeInfo  *row,
	const AxisVolumeProbe *probe
);

/*
 * The filesystem a probe or a look found (@magic, @fsid, @fs_uuid, "" or
 * %NULL when none, @dev, @bytes) against the one @record was last seen on,
 * for restoring its marker; with a sentence or two for the page in *@text
 * (the caller frees it).
 */
AxisMarkerRestoreFs
axis_volume_restore_compare(
	const AxisVolumeInfo  *record,
	guint64                magic,
	const gchar           *fsid,
	const gchar           *fs_uuid,
	guint64                dev,
	guint64                bytes,
	gchar                **text
);

/*
 * Writes the marker of @record, a volume added without one, through
 * @probe's kept descriptor (probe->fd: the directory a check or a guard
 * has just found to be the volume, mounted read-write), or records the
 * marker the probe found there that is the volume's own; then the volume
 * is recognised by its marker.  Serialised with every other change of
 * volumes; the row must still be the volume at that path without a
 * marker.  The marker holds the volume's own identifier.  On failure the
 * reason is recorded (AxisVolumeInfo.marker_problem) and logged when it
 * changes.  *@after (optional) is the volume afterwards.
 */
gboolean
axis_storage_write_late_marker(
	AxisStorage            *self,
	const AxisVolumeInfo   *record,
	const AxisVolumeProbe  *probe,
	AxisVolumeInfo        **after,
	GError                **error
);

/*
 * Looks at the top of @record's volume for the temporary marker files an
 * interrupted marker write left, and removes those the previous look found
 * the same (axis_volume_marker_remove_stale()), through @fd, the directory
 * a write guard has just been given for it, on a helper with the write
 * deadline; at most one look per volume in the sweep age (the looks are
 * that far apart).  What it removes is logged; a failure is not the
 * guard's.
 */
void
axis_storage_sweep_marker_temporaries(
	AxisStorage          *self,
	const AxisVolumeInfo *record,
	gint                  fd
);

/* A sentence about the errno a probe ended with, for messages. */
const gchar *
axis_volume_probe_detail(const AxisVolumeProbe *probe);

/* The key of a volume's helper calls. */
gchar *
axis_storage_volume_key(gint64 id);

guint
axis_storage_check_timeout(AxisStorage *self);

guint
axis_storage_write_timeout(AxisStorage *self);

/* The hook and its data, for a probe. */
void
axis_storage_get_hook(
	AxisStorage          *self,
	AxisStorageCheckHook *hook,
	gpointer             *hook_data
);

/* Reads one row of AXIS_VOLUME_COLUMNS. */
AxisVolumeInfo *
axis_volume_info_from_row(
	AxisDbResult *result,
	guint         row
);

/* Replaces (or adds) the daemon's record of a volume. */
void
axis_storage_put_volume(
	AxisStorage          *self,
	const AxisVolumeInfo *info
);

void
axis_storage_forget_volume(
	AxisStorage *self,
	gint64       id
);

/* Adds @delta to a volume's root count. */
void
axis_storage_count_roots(
	AxisStorage *self,
	gint64       id,
	gint         delta
);

/* Tells the listeners, on the main context; @info may be %NULL. */
void
axis_storage_notify(
	AxisStorage          *self,
	const AxisVolumeInfo *info,
	gboolean              state_changed
);

/*
 * Checks the volume described by @record now and stores the result, as
 * axis_storage_check_volume().  @check_lock must not be held.
 */
gboolean
axis_storage_check_record(
	AxisStorage           *self,
	const AxisVolumeInfo  *record,
	AxisVolumeInfo       **after,
	GError               **error
);

G_END_DECLS

#endif /* AXIS_STORAGE_PRIVATE_H */
