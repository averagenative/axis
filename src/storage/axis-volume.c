/*
 * axis-volume.c - adding, removing and re-pointing volumes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every change of volumes runs in one database transaction that first
 * takes the storage advisory lock, then reads the registered volumes
 * afresh, then looks at the directory (on a helper, with a deadline) and
 * decides.  So two changes, in this daemon or another, never decide on the
 * same picture: two administrators adding the same disk at once get one
 * volume and one refusal.  The directory descriptor the decision was made
 * on is the one the marker is written through.
 *
 * A disk mounted read-only without a marker is added without writing
 * anything (the identify plan): the volume gets its identifier now and is
 * recognised by its filesystem identity until axis_storage_write_late_marker()
 * puts that identifier in its marker, through the descriptor a check or a
 * write guard has just recognised, the first time the disk is found mounted
 * read-write.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-storage-private.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-volume-guard.h"
#include "core/axis-size.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <unistd.h>

void
axis_volume_inspection_free(AxisVolumeInspection *inspection)
{
	if (inspection == NULL)
		return;

	g_free(inspection->path);
	g_free(inspection->suggested_name);
	g_free(inspection->refusal);
	g_ptr_array_unref(inspection->warnings);
	g_free(inspection->fs_type);
	g_free(inspection->device);
	g_free(inspection->fsid);
	g_free(inspection->marker_uuid);
	g_free(inspection->fs_uuid);
	g_free(inspection);
}

void
axis_marker_restore_inspection_free(AxisMarkerRestoreInspection *inspection)
{
	if (inspection == NULL)
		return;

	if (inspection->roots != NULL)
		g_ptr_array_unref(inspection->roots);
	if (inspection->root_present != NULL)
		g_array_unref(inspection->root_present);
	g_free(inspection->filesystem_text);
	g_free(inspection);
}

/* ── Database helpers ───────────────────────────────────────────── */

/* Every registered volume, read in the caller's transaction. */
static GPtrArray *
read_volumes(
	AxisDbConn  *conn,
	GError     **error
){
	g_autoptr(AxisDbResult) result = NULL;
	GPtrArray *rows;
	guint i;

	if (!axis_db_conn_exec(conn, "SELECT " AXIS_VOLUME_COLUMNS " FROM volume ORDER BY id", 0, NULL,
	                       &result, error))
		return NULL;

	rows = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(rows, axis_volume_info_from_row(result, i));

	return rows;
}

/* Begins a transaction holding the storage lock. */
static AxisDbConn *
begin_locked(
	AxisStorage  *self,
	GError      **error
){
	AxisDbConn *conn = axis_db_pool_acquire(self->pool, error);

	if (conn == NULL)
		return NULL;

	if (!axis_db_conn_begin(conn, error) ||
	    !axis_db_conn_exec(conn, AXIS_STORAGE_ADVISORY_LOCK_SQL, 0, NULL, NULL, error)) {
		axis_db_conn_rollback(conn, NULL);
		axis_db_pool_release(self->pool, conn);
		return NULL;
	}

	return conn;
}

static void
end_locked(
	AxisStorage *self,
	AxisDbConn  *conn
){
	axis_db_conn_rollback(conn, NULL);
	axis_db_pool_release(self->pool, conn);
}

/* ── Looking at a directory ─────────────────────────────────────── */

static gchar *
path_key(const gchar *path)
{
	return g_strconcat("path:", path, NULL);
}

/* Probes @path; the call (or %NULL and @error on a missed deadline). */
static AxisStorageProbeCall *
probe_path(
	AxisStorage  *self,
	const gchar  *path,
	gboolean      keep_fd,
	GError      **error
){
	g_autofree gchar *key = path_key(path);
	AxisVolumeProbe *probe = axis_volume_probe_new(path);

	probe->want_empty = TRUE;
	probe->want_fs_uuid = TRUE;
	probe->keep_fd = keep_fd;
	axis_storage_get_hook(self, &probe->hook, &probe->hook_data);

	return axis_storage_run_probe(self, key, probe, axis_storage_check_timeout(self), error);
}

/* Whether @path overlaps a registered volume other than @exclude and @also
 * (0: none); sets a refusal sentence when it does. */
static gboolean
overlaps_except(
	GPtrArray    *rows,
	const gchar  *path,
	gint64        exclude,
	gint64        also,
	gchar       **refusal
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisVolumeInfo *row = g_ptr_array_index(rows, i);

		if (row->id == exclude || (also != 0 && row->id == also))
			continue;
		if (strcmp(row->path, path) == 0) {
			*refusal = g_strdup_printf("This is already the path of disk '%s'.", row->name);
			return TRUE;
		}
		if (axis_storage_path_contains(row->path, path)) {
			*refusal = g_strdup_printf("This directory is inside disk '%s' (at %s). A disk "
				"cannot be inside another.", row->name, row->path);
			return TRUE;
		}
		if (axis_storage_path_contains(path, row->path)) {
			*refusal = g_strdup_printf("Disk '%s' (at %s) is inside this directory. A disk "
				"cannot contain another.", row->name, row->path);
			return TRUE;
		}
	}

	return FALSE;
}

/* Whether @path overlaps a registered volume other than @exclude. */
static gboolean
overlaps(
	GPtrArray    *rows,
	const gchar  *path,
	gint64        exclude,
	gchar       **refusal
){
	return overlaps_except(rows, path, exclude, 0, refusal);
}

static gboolean
fsid_is_meaningful(const gchar *fsid)
{
	return fsid != NULL && fsid[0] != '\0' && strspn(fsid, "0") != strlen(fsid);
}

AxisVolumeFsSameness
axis_volume_probe_same_filesystem(
	const AxisVolumeInfo  *row,
	const AxisVolumeProbe *probe
){
	g_autofree gchar *device = axis_storage_fs_device_text(probe->identity.dev);

	if (probe->info_ok && fsid_is_meaningful(probe->info.fsid)) {
		if (!axis_storage_fs_fsid_is_device(probe->info.fsid, probe->identity.dev)) {
			/* The filesystem's own identifier (from its UUID, its dataset,
			 * its subvolume): the one the volume was last seen with, or, for
			 * one without a marker, is recognised by. */
			if (g_strcmp0(row->fsid, probe->info.fsid) == 0 ||
			    (!row->has_marker && strcmp(row->fs_identity.fsid, probe->info.fsid) == 0))
				return AXIS_VOLUME_FS_PROVEN;
		} else if (probe->fs_uuid[0] != '\0' &&
		           (g_strcmp0(row->fs_uuid, probe->fs_uuid) == 0 ||
		            (!row->has_marker && strcmp(row->fs_identity.fs_uuid, probe->fs_uuid) == 0))) {
			/* Only the device's number for an identifier (xfs, f2fs): its
			 * UUID decides, as it does for recognising a volume without a
			 * marker.  For one with a marker it is what its last check
			 * found, evidence of its filesystem, not what it is recognised
			 * by. */
			return AXIS_VOLUME_FS_PROVEN;
		}
		/* An identifier that is the device's number, the same: the same
		 * device number. */
		if (g_strcmp0(row->fsid, probe->info.fsid) == 0)
			return AXIS_VOLUME_FS_SAME_DEVICE;
	}
	if (g_strcmp0(row->device, device) == 0)
		return AXIS_VOLUME_FS_SAME_DEVICE;
	return AXIS_VOLUME_FS_OTHER;
}

/*
 * Whether the directory @probe opened is on the filesystem of a volume
 * other than @exclude: refused, as one filesystem is one volume.  Refused
 * whether or not that volume is online when what is recorded of it proves
 * the filesystem the same (axis_volume_probe_same_filesystem()); when only
 * the device number is the same, refused while that volume is online, and
 * only a warning while it is not (another disk may have its device number
 * now).  @offline (optional): where the refusal of a volume proven but not
 * online goes instead, for the caller to give after a more precise one (the
 * directory carries that volume's marker).
 */
static gboolean
same_filesystem_deferred(
	GPtrArray             *rows,
	const AxisVolumeProbe *probe,
	gint64                 exclude,
	gchar                **refusal,
	GPtrArray             *warnings,
	gchar                **offline
){
	g_autofree gchar *device = axis_storage_fs_device_text(probe->identity.dev);
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisVolumeInfo *row = g_ptr_array_index(rows, i);
		AxisVolumeFsSameness same;

		if (row->id == exclude)
			continue;
		same = axis_volume_probe_same_filesystem(row, probe);
		if (same == AXIS_VOLUME_FS_OTHER)
			continue;

		if (axis_volume_state_is_usable(row->state)) {
			*refusal = g_strdup_printf("This directory is on the same filesystem as disk "
				"'%s', which is online. One filesystem can only be one disk.", row->name);
			return TRUE;
		}
		if (same == AXIS_VOLUME_FS_PROVEN) {
			gchar *text = g_strdup_printf("This directory is on the filesystem of disk '%s', which is not "
				"online now: the filesystem's %s is the one recorded for that disk when it was last found. One "
				"filesystem can only be one disk. If that disk is mounted here now, point that disk "
				"here instead. If it is somewhere else, point it there first (or remove it, if it is "
				"gone for good): what is recorded of it is then of that disk, and this filesystem no longer "
				"counts as its.", row->name,
				axis_storage_fs_fsid_is_device(probe->info.fsid, probe->identity.dev) ? "UUID" : "identifier");

			if (offline == NULL) {
				*refusal = text;
				return TRUE;
			}
			if (*offline == NULL)
				*offline = text;
			else
				g_free(text);
			continue;
		}
		if (warnings != NULL)
			g_ptr_array_add(warnings, g_strdup_printf("Disk '%s', which is not online now, was "
				"last seen on the same device (%s). If this is the same disk, that disk should "
				"be pointed here instead.", row->name, device));
	}

	return FALSE;
}

static gboolean
same_filesystem(
	GPtrArray             *rows,
	const AxisVolumeProbe *probe,
	gint64                 exclude,
	gchar                **refusal,
	GPtrArray             *warnings
){
	return same_filesystem_deferred(rows, probe, exclude, refusal, warnings, NULL);
}

static const AxisVolumeInfo *
find_uuid(
	GPtrArray   *rows,
	const gchar *uuid
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisVolumeInfo *row = g_ptr_array_index(rows, i);

		if (strcmp(row->uuid, uuid) == 0)
			return row;
	}

	return NULL;
}

/*
 * The plan for adding @path as a new volume, from what the probe found
 * (@probe %NULL when it missed its deadline) and the registered @rows.
 */
static AxisVolumeInspection *
plan_new_volume(
	AxisStorage           *self,
	const gchar           *path,
	const AxisVolumeProbe *probe,
	GPtrArray             *rows
){
	AxisVolumeInspection *in = g_new0(AxisVolumeInspection, 1);
	g_autofree gchar *proven = NULL;
	gchar *refusal = NULL;

	in->path = g_strdup(path);
	in->suggested_name = axis_storage_path_last_segment(path);
	in->warnings = g_ptr_array_new_with_free_func(g_free);
	in->plan = AXIS_VOLUME_PLAN_REFUSE;

	if (overlaps(rows, path, 0, &refusal))
		goto refuse;

	if (probe == NULL) {
		refusal = g_strdup_printf("The filesystem did not answer within %u seconds; it may not be "
			"responding.", MAX(axis_storage_check_timeout(self) / 1000, 1));
		goto refuse;
	}

	if (probe->open_errno == ENOENT) {
		refusal = g_strdup("There is no directory at this path.");
		goto refuse;
	}
	if (probe->open_errno == ELOOP || probe->open_errno == ENOTDIR) {
		refusal = g_strdup("The path is, or passes through, a symbolic link, or is not a "
			"directory. Give the real path of the directory.");
		goto refuse;
	}
	if (probe->open_errno != 0) {
		refusal = g_strdup_printf("The directory cannot be opened: %s.",
			g_strerror(probe->open_errno));
		goto refuse;
	}

	in->mount_root = probe->identity.mount_root;
	in->dev = probe->identity.dev;
	in->ino = probe->identity.ino;
	in->device = axis_storage_fs_device_text(probe->identity.dev);
	if (probe->info_ok) {
		in->read_only = probe->info.read_only;
		in->fs_type = g_strdup(axis_storage_fs_type_name(probe->info.magic));
		in->fsid = g_strdup(probe->info.fsid);
		in->has_space = TRUE;
		in->total_bytes = probe->info.total_bytes;
		in->free_bytes = probe->info.free_bytes;
		in->available_bytes = probe->info.available_bytes;
		if (probe->fs_uuid[0] != '\0')
			in->fs_uuid = g_strdup(probe->fs_uuid);
	}
	in->empty = probe->empty_known && probe->empty;

	/* Another volume's filesystem: refused.  When that volume is not online
	 * and the directory carries its marker, the marker's refusal says more
	 * (it is that volume's disk), and is given instead. */
	if (same_filesystem_deferred(rows, probe, 0, &refusal, in->warnings, &proven))
		goto refuse;
	if (proven != NULL && !(probe->marker_status == AXIS_VOLUME_MARKER_FOUND &&
	                        find_uuid(rows, probe->marker.uuid) != NULL)) {
		refusal = g_steal_pointer(&proven);
		goto refuse;
	}

	switch (probe->marker_status) {
	case AXIS_VOLUME_MARKER_FOUND: {
		const AxisVolumeInfo *owner = find_uuid(rows, probe->marker.uuid);

		in->marker_uuid = g_strdup(probe->marker.uuid);
		in->marker_created_us = probe->marker.created * G_USEC_PER_SEC;
		if (owner != NULL) {
			refusal = g_strdup_printf("This directory carries the marker of disk '%s'. If that "
				"disk is mounted here now, change that disk's path instead of adding "
				"it again.", owner->name);
			goto refuse;
		}
		in->plan = AXIS_VOLUME_PLAN_ADOPT;
		break;
	}
	case AXIS_VOLUME_MARKER_INVALID:
		refusal = g_strdup(probe->marker_errno == EPROTO ?
			"This directory has a marker written by a later version of Axis. Update Axis, then add the disk again." :
			"There is a file called .axis-volume here, but it is not a valid Axis marker. Axis "
			"leaves it alone; move it away first if it is not needed.");
		goto refuse;
	case AXIS_VOLUME_MARKER_UNREADABLE:
		refusal = g_strdup_printf("The marker file here cannot be read: %s.",
			g_strerror(probe->marker_errno));
		goto refuse;
	case AXIS_VOLUME_MARKER_MISSING:
	default:
		if (!probe->empty_known) {
			refusal = g_strdup_printf("The directory cannot be read: %s.",
				g_strerror(probe->empty_errno));
			goto refuse;
		}
		if (probe->empty && !probe->identity.mount_root) {
			refusal = g_strdup("The directory is empty and is not a mount point: this is what "
				"a disk that is not mounted looks like. Mount the disk first.");
			goto refuse;
		}
		if (!probe->info_ok) {
			refusal = g_strdup_printf("The filesystem's details cannot be read: %s.",
				g_strerror(probe->info_errno));
			goto refuse;
		}
		if (probe->info.read_only) {
			AxisVolumeFsIdentity identity;
			gchar *why;

			/* Nothing can be written: recognised by what can be read,
			 * where that tells this disk from any other. */
			if (probe->empty) {
				refusal = g_strdup("The filesystem is mounted read-only, has no marker yet, and this "
					"directory is empty. Without a marker Axis never takes an empty directory for a "
					"disk, because that is also what a disk that is not mounted looks like. Add it "
					"once it holds your files, or mount it read-write once so that Axis can write its "
					"marker.");
				goto refuse;
			}
			why = axis_volume_probe_fs_identity(probe, &identity);
			if (why != NULL) {
				refusal = g_strdup_printf("The filesystem is mounted read-only and has no marker yet. %s "
					"Mount it read-write once so that Axis can write its marker; it may be read-only "
					"after that.", why);
				g_free(why);
				goto refuse;
			}
			in->plan = AXIS_VOLUME_PLAN_IDENTIFY;
			break;
		}
		in->plan = AXIS_VOLUME_PLAN_WRITE_MARKER;
		break;
	}

	if (!probe->info_ok) {
		refusal = g_strdup_printf("The filesystem's details cannot be read: %s.",
			g_strerror(probe->info_errno));
		in->plan = AXIS_VOLUME_PLAN_REFUSE;
		goto refuse;
	}

	if (!probe->identity.mount_root)
		g_ptr_array_add(in->warnings, g_strdup("This directory is not a mount point: it belongs "
			"to the filesystem of the directory above it. If a disk should be mounted here, mount "
			"it first. Otherwise confirm that this directory is meant to be a disk; Axis will "
			"then not be able to tell when a disk mounted here later is missing."));
	return in;

refuse:
	in->plan = AXIS_VOLUME_PLAN_REFUSE;
	in->refusal = refusal;
	return in;
}

gboolean
axis_storage_inspect_volume(
	AxisStorage           *self,
	const gchar           *path,
	AxisVolumeInspection **inspection,
	GError               **error
){
	g_autofree gchar *normalised = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	AxisStorageProbeCall *call;
	const gchar *problem = NULL;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(inspection != NULL, FALSE);

	*inspection = NULL;
	if (!axis_storage_path_normalize(path, TRUE, &normalised, &problem)) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID, problem);
		return FALSE;
	}

	if (!axis_db_pool_exec(self->pool, "SELECT " AXIS_VOLUME_COLUMNS " FROM volume ORDER BY id",
	                       0, NULL, &result, error))
		return FALSE;
	rows = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_info_free);
	for (i = 0; i < axis_db_result_get_n_rows(result); i++)
		g_ptr_array_add(rows, axis_volume_info_from_row(result, i));

	call = probe_path(self, normalised, FALSE, NULL);
	*inspection = plan_new_volume(self, normalised,
		call != NULL ? axis_storage_probe_call_get_data(call) : NULL, rows);
	axis_storage_probe_call_unref(call);

	return TRUE;
}

/* ── Writing the marker ─────────────────────────────────────────── */

typedef struct
{
	gint              fd;
	AxisVolumeMarker  marker;
	gboolean          ok;
	GError           *error;
} WriteJob;

static void
write_job_free(gpointer data)
{
	WriteJob *job = data;

	if (job->fd >= 0)
		close(job->fd);
	g_clear_error(&job->error);
	g_free(job);
}

/* On the helper thread: write, then read back through the same
 * descriptor. */
static void
write_job_run(gpointer data)
{
	WriteJob *job = data;
	AxisVolumeMarker back;

	job->ok = axis_volume_marker_write(job->fd, &job->marker, &job->error);
	if (!job->ok)
		return;

	if (axis_volume_marker_read(job->fd, &back, NULL) != AXIS_VOLUME_MARKER_FOUND ||
	    strcmp(back.uuid, job->marker.uuid) != 0 || back.created != job->marker.created) {
		job->ok = FALSE;
		g_set_error_literal(&job->error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"cannot write the marker: the marker read back is not the one written");
	}
}

/* Writes @marker into the directory @fd (borrowed; a dup is used). */
static gboolean
write_marker(
	AxisStorage             *self,
	const gchar             *path,
	gint                     fd,
	const AxisVolumeMarker  *marker,
	GError                 **error
){
	g_autofree gchar *key = g_strconcat("write:", path, NULL);
	AxisStorageProbeCall *call;
	WriteJob *job = g_new0(WriteJob, 1);
	GError *local = NULL;
	gboolean ok;

	job->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	job->marker = *marker;
	if (job->fd < 0) {
		gint err = errno;

		write_job_free(job);
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_FAILED,
			"The marker could not be written: %s.", g_strerror(err));
		return FALSE;
	}

	call = axis_storage_probe_start(self->probe, key, write_job_run, job, write_job_free, &local);
	if (call == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
			"The marker was not written: %s.", local->message);
		g_error_free(local);
		return FALSE;
	}
	if (!axis_storage_probe_wait(call, g_get_monotonic_time() +
	                             (gint64)axis_storage_write_timeout(self) * 1000)) {
		axis_storage_probe_call_unref(call);
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
			"Writing the marker did not finish within %u seconds; the filesystem may not be "
			"responding. If the marker appears later, adding the directory again uses it.",
			axis_storage_write_timeout(self) / 1000);
		return FALSE;
	}

	job = axis_storage_probe_call_get_data(call);
	ok = job->ok;
	/* The writer's own messages say what could not be done. */
	if (!ok)
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_FAILED, "%s",
			job->error != NULL ? job->error->message : "The marker could not be written: unknown error.");
	axis_storage_probe_call_unref(call);

	return ok;
}

/* ── Temporary marker files left behind ─────────────────────────── */

typedef struct
{
	gint        fd;
	GHashTable *seen;	/* the volume's, lent for the look */
	gint64      now_us;
	gint64      age_us;
	guint       removed;
	gint        saved_errno;
} SweepJob;

static void
sweep_job_free(gpointer data)
{
	SweepJob *job = data;

	if (job->fd >= 0)
		close(job->fd);
	if (job->seen != NULL)
		g_hash_table_unref(job->seen);
	g_free(job);
}

/* On the helper thread. */
static void
sweep_job_run(gpointer data)
{
	SweepJob *job = data;

	job->removed = axis_volume_marker_remove_stale(job->fd, job->seen, job->now_us, job->age_us,
		&job->saved_errno);
}

void
axis_storage_sweep_marker_temporaries(
	AxisStorage          *self,
	const AxisVolumeInfo *record,
	gint                  fd
){
	g_autofree gchar *key = NULL;
	AxisStorageProbeCall *call;
	AxisStorageSweep *sweep;
	GHashTable *seen = NULL;
	SweepJob *job;
	gint64 now = g_get_monotonic_time();
	gint64 age;

	g_return_if_fail(self != NULL && record != NULL);

	age = (gint64)g_atomic_int_get(&self->sweep_age_s) * G_USEC_PER_SEC;

	/* One look at a time per volume, at most one in the age: the table of
	 * what the looks found is lent to the look that runs. */
	g_mutex_lock(&self->lock);
	sweep = g_hash_table_lookup(self->swept, &record->id);
	if (sweep == NULL) {
		gint64 *id = g_new(gint64, 1);

		*id = record->id;
		sweep = g_new0(AxisStorageSweep, 1);
		sweep->last_us = G_MININT64;
		sweep->seen = axis_volume_marker_seen_new();
		g_hash_table_replace(self->swept, id, sweep);
	}
	if (sweep->seen != NULL && (sweep->last_us == G_MININT64 || now - sweep->last_us >= age)) {
		sweep->last_us = now;
		seen = g_steal_pointer(&sweep->seen);
	}
	g_mutex_unlock(&self->lock);
	if (seen == NULL)
		return;

	job = g_new0(SweepJob, 1);
	job->seen = seen;
	job->now_us = now;
	job->age_us = age;
	job->fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (job->fd < 0) {
		g_mutex_lock(&self->lock);
		sweep = g_hash_table_lookup(self->swept, &record->id);
		if (sweep != NULL && sweep->seen == NULL)
			sweep->seen = g_steal_pointer(&job->seen);
		g_mutex_unlock(&self->lock);
		sweep_job_free(job);
		return;
	}
	key = g_strconcat("write:", record->path, NULL);
	call = axis_storage_probe_start(self->probe, key, sweep_job_run, job, sweep_job_free, NULL);
	if (call == NULL) {
		/* The table went with the job: the next look starts afresh. */
		g_mutex_lock(&self->lock);
		sweep = g_hash_table_lookup(self->swept, &record->id);
		if (sweep != NULL && sweep->seen == NULL)
			sweep->seen = axis_volume_marker_seen_new();
		g_mutex_unlock(&self->lock);
		return;
	}
	if (!axis_storage_probe_wait(call, g_get_monotonic_time() + (gint64)axis_storage_write_timeout(self) * 1000)) {
		axis_storage_probe_call_unref(call);
		g_mutex_lock(&self->lock);
		sweep = g_hash_table_lookup(self->swept, &record->id);
		if (sweep != NULL && sweep->seen == NULL)
			sweep->seen = axis_volume_marker_seen_new();
		g_mutex_unlock(&self->lock);
		return;
	}
	job = axis_storage_probe_call_get_data(call);
	g_mutex_lock(&self->lock);
	sweep = g_hash_table_lookup(self->swept, &record->id);
	if (sweep != NULL && sweep->seen == NULL)
		sweep->seen = g_steal_pointer(&job->seen);
	g_mutex_unlock(&self->lock);
	if (job->removed > 0)
		g_message("volume '%s' (id %" G_GINT64_FORMAT "): removed %u temporary marker file(s) at %s that an "
			"interrupted write of its marker left", record->name, record->id, job->removed, record->path);
	if (job->saved_errno != 0)
		g_debug("volume '%s' (id %" G_GINT64_FORMAT "): looking for temporary marker files: %s", record->name,
			record->id, g_strerror(job->saved_errno));
	axis_storage_probe_call_unref(call);
}

/* ── Adding ─────────────────────────────────────────────────────── */

static gboolean
refused(
	GError      **error,
	gint          code,
	const gchar  *message
){
	g_set_error_literal(error, AXIS_STORAGE_ERROR, code, message);
	return FALSE;
}

/* @identity: what the volume is recognised by until its marker is written
 * (the identify plan), or %NULL when @marker is on the disk. */
static gboolean
insert_volume(
	AxisDbConn                 *conn,
	const gchar                *path,
	const gchar                *name,
	const AxisVolumeMarker     *marker,
	const AxisVolumeProbe      *probe,
	const AxisVolumeFsIdentity *identity,
	AxisVolumeInfo            **info,
	GError                    **error
){
	g_autofree gchar *device = axis_storage_fs_device_text(probe->identity.dev);
	g_autofree gchar *total = g_strdup_printf("%" G_GUINT64_FORMAT, probe->info.total_bytes);
	g_autofree gchar *free_bytes = g_strdup_printf("%" G_GUINT64_FORMAT, probe->info.free_bytes);
	g_autofree gchar *available = g_strdup_printf("%" G_GUINT64_FORMAT, probe->info.available_bytes);
	g_autofree gchar *created = g_strdup_printf("%" G_GINT64_FORMAT, marker->created);
	g_autofree gchar *magic = NULL;
	g_autofree gchar *ino = NULL;
	g_autofree gchar *bytes = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[19];

	if (identity != NULL) {
		/* As stored: the bits of each number in a bigint. */
		magic = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)identity->magic);
		ino = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)identity->ino);
		bytes = g_strdup_printf("%" G_GINT64_FORMAT, (gint64)MIN(identity->bytes, (guint64)G_MAXINT64));
	}

	params[0] = marker->uuid;
	params[1] = name;
	params[2] = path;
	params[3] = probe->identity.mount_root ? "true" : "false";
	params[4] = axis_storage_fs_type_name(probe->info.magic);
	params[5] = probe->info.fsid;
	params[6] = device;
	params[7] = axis_volume_state_to_string(probe->info.read_only ? AXIS_VOLUME_READ_ONLY :
		AXIS_VOLUME_ONLINE);
	params[8] = total;
	params[9] = free_bytes;
	params[10] = available;
	params[11] = identity == NULL ? created : NULL;
	params[12] = identity == NULL ? "marker" : "filesystem";
	params[13] = magic;
	params[14] = identity != NULL ? identity->fsid : NULL;
	params[15] = identity != NULL && identity->fs_uuid[0] != '\0' ? identity->fs_uuid : NULL;
	params[16] = ino;
	params[17] = bytes;
	/* For every volume: evidence of the filesystem it is on (fs_uuid). */
	params[18] = probe->fs_uuid[0] != '\0' ? probe->fs_uuid : NULL;

	if (!axis_db_conn_exec(conn,
	                       "INSERT INTO volume (uuid, name, path, mount_root, fs_type, fsid, device,"
	                       " state, state_reason, last_checked_at, last_online_at, total_bytes,"
	                       " free_bytes, available_bytes, space_checked_at, marker_created_at,"
	                       " identity, ident_magic, ident_fsid, ident_fs_uuid, ident_ino, ident_bytes, fs_uuid)"
	                       " VALUES ($1::uuid, $2, $3, $4::boolean, $5, $6, $7, $8, 'none', now(),"
	                       " now(), $9::bigint, $10::bigint, $11::bigint, now(),"
	                       " to_timestamp($12::double precision), $13, $14::bigint, $15, $16,"
	                       " $17::bigint, $18::bigint, $19)"
	                       " RETURNING " AXIS_VOLUME_COLUMNS,
	                       G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	*info = axis_volume_info_from_row(result, 0);
	return TRUE;
}

static const AxisVolumeInfo *
find_name(
	GPtrArray   *rows,
	const gchar *name,
	gint64       exclude
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisVolumeInfo *row = g_ptr_array_index(rows, i);

		if (row->id != exclude && strcmp(row->name, name) == 0)
			return row;
	}

	return NULL;
}

gboolean
axis_storage_add_volume(
	AxisStorage                 *self,
	const AxisVolumeAddRequest  *request,
	gint64                      *id,
	GError                     **error
){
	g_autofree gchar *path = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(AxisVolumeInspection) plan = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	AxisStorageProbeCall *call = NULL;
	AxisVolumeProbe *probe;
	AxisVolumeMarker marker;
	AxisVolumeFsIdentity identity;
	const gchar *problem = NULL;
	AxisDbConn *conn;
	GError *local = NULL;
	gboolean ok = FALSE;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(request != NULL, FALSE);

	memset(&identity, 0, sizeof(identity));
	if (id != NULL)
		*id = 0;
	if (!axis_storage_path_normalize(request->path, TRUE, &path, &problem))
		return refused(error, AXIS_STORAGE_ERROR_INVALID, problem);
	if (!axis_storage_label_normalize(request->name, &name, &problem))
		return refused(error, AXIS_STORAGE_ERROR_INVALID, problem);
	if (request->plan != AXIS_VOLUME_PLAN_WRITE_MARKER && request->plan != AXIS_VOLUME_PLAN_ADOPT &&
	    request->plan != AXIS_VOLUME_PLAN_IDENTIFY)
		return refused(error, AXIS_STORAGE_ERROR_INVALID, "Nothing was confirmed.");
	if (request->plan == AXIS_VOLUME_PLAN_ADOPT && !axis_volume_uuid_is_valid(request->marker_uuid))
		return refused(error, AXIS_STORAGE_ERROR_INVALID, "The confirmed marker is not valid.");

	conn = begin_locked(self, error);
	if (conn == NULL)
		return FALSE;

	rows = read_volumes(conn, error);
	if (rows == NULL)
		goto out;
	if (rows->len >= AXIS_STORAGE_MAX_VOLUMES) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, "There are as many disks as Axis allows.");
		goto out;
	}
	if (find_name(rows, name, 0) != NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
			"A disk called '%s' exists already.", name);
		goto out;
	}

	/* Looked at again, through the descriptor the marker is written
	 * through: this, not what was shown, is what is decided on. */
	call = probe_path(self, path, TRUE, &local);
	if (call == NULL) {
		g_propagate_error(error, local);
		goto out;
	}
	probe = axis_storage_probe_call_get_data(call);
	plan = plan_new_volume(self, path, probe, rows);

	if (plan->plan == AXIS_VOLUME_PLAN_REFUSE) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, plan->refusal);
		goto out;
	}
	/* The same plan, on the same directory of the same filesystem: the top
	 * of every filesystem of a type has one inode number and device
	 * numbers are reused, so the filesystem's id counts too, and its size
	 * where that id is only the device's number (nothing stronger is in the
	 * confirmation; a size moves as a pool is written, so it is not
	 * compared where the filesystem has an id of its own). */
	if (plan->plan != request->plan || plan->dev != request->dev || plan->ino != request->ino ||
	    g_strcmp0(plan->fsid, request->fsid) != 0 ||
	    (axis_storage_fs_fsid_is_device(plan->fsid, plan->dev) && plan->total_bytes != request->total_bytes) ||
	    (plan->plan == AXIS_VOLUME_PLAN_ADOPT && g_strcmp0(plan->marker_uuid, request->marker_uuid) != 0)) {
		refused(error, AXIS_STORAGE_ERROR_CONFIRM, "What is at this path has changed since it was "
			"shown. Nothing was done; look at it again before confirming.");
		goto out;
	}
	if (!plan->mount_root && !request->not_mount_point_ok) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, "The directory is not a mount point. Confirm "
			"that it is meant to be a disk anyway.");
		goto out;
	}

	if (plan->plan == AXIS_VOLUME_PLAN_ADOPT) {
		marker = probe->marker;
	} else {
		/* For the identify plan, the identifier its marker gets later. */
		if (!axis_volume_marker_new(&marker, &local)) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_FAILED, "%s", local->message);
			g_clear_error(&local);
			goto out;
		}
		/* The one write storage allows in read-only mode. */
		if (plan->plan == AXIS_VOLUME_PLAN_WRITE_MARKER &&
		    !write_marker(self, path, probe->fd, &marker, error))
			goto out;
	}
	if (plan->plan == AXIS_VOLUME_PLAN_IDENTIFY) {
		gchar *why = axis_volume_probe_fs_identity(probe, &identity);

		/* The plan was decided on this very probe. */
		if (why != NULL) {
			refused(error, AXIS_STORAGE_ERROR_REFUSED, why);
			g_free(why);
			goto out;
		}
	}

	if (!insert_volume(conn, path, name, &marker, probe,
	                   plan->plan == AXIS_VOLUME_PLAN_IDENTIFY ? &identity : NULL, &info, error) ||
	    !axis_db_conn_commit(conn, error))
		goto out;
	ok = TRUE;

out:
	end_locked(self, conn);
	axis_storage_probe_call_unref(call);
	if (!ok)
		return FALSE;

	axis_storage_put_volume(self, info);
	if (request->plan == AXIS_VOLUME_PLAN_IDENTIFY) {
		g_autofree gchar *described = axis_volume_fs_identity_describe(&info->fs_identity);

		g_message("volume '%s' (id %" G_GINT64_FORMAT ") added at %s without a marker: the disk is mounted "
			"read-only, so nothing was written; it is recognised by its %s until its marker %s is written, "
			"the first time Axis finds it mounted read-write", info->name, info->id, info->path, described,
			info->uuid);
	} else {
		g_message("volume '%s' (id %" G_GINT64_FORMAT ") added at %s: %s marker %s", info->name,
			info->id, info->path, request->plan == AXIS_VOLUME_PLAN_ADOPT ? "adopted the existing" :
			"wrote the new", info->uuid);
	}
	axis_storage_notify(self, NULL, FALSE);
	if (id != NULL)
		*id = info->id;
	return TRUE;
}

/* ── Removing ───────────────────────────────────────────────────── */

gboolean
axis_storage_remove_volume(
	AxisStorage  *self,
	gint64        id,
	gboolean      allow_unavailable,
	GError      **error
){
	g_autoptr(AxisVolumeInfo) record = NULL;
	g_autoptr(AxisVolumeInfo) now = NULL;
	g_autoptr(AxisDbResult) result = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, id);
	const gchar *params[1];
	AxisDbConn *conn;
	guint64 roots = 0;
	GError *local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	record = axis_storage_get_volume(self, id);
	if (record == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	/* Not online is not a reason to forget a volume; only a person who
	 * says the disk is gone for good is.  Without that, the volume must be
	 * verified there now, as a guard verifies it (a check that could not
	 * run counts as not there). */
	if (!allow_unavailable) {
		g_autoptr(AxisVolumeGuard) guard = axis_volume_guard_acquire(self, id,
			AXIS_VOLUME_INTENT_READ, &local);

		if (guard == NULL) {
			/* So that the page asking shows why. */
			axis_storage_check_record(self, record, &now, NULL);
			if (g_error_matches(local, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE))
				g_propagate_prefixed_error(error, local, "A disk is only removed while it is "
					"online, unless you confirm that its disk is gone for good. ");
			else
				g_propagate_error(error, local);
			return FALSE;
		}
	}

	conn = begin_locked(self, error);
	if (conn == NULL)
		return FALSE;

	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "DELETE FROM library_root WHERE volume_id = $1::bigint", 1, params,
	                       &result, error))
		goto fail;
	roots = axis_db_result_get_affected_rows(result);
	g_clear_pointer(&result, axis_db_result_free);
	if (!axis_db_conn_exec(conn, "DELETE FROM volume WHERE id = $1::bigint", 1, params, &result,
	                       error))
		goto fail;
	if (axis_db_result_get_affected_rows(result) != 1) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, id);
		goto fail;
	}
	if (!axis_db_conn_commit(conn, error))
		goto fail;
	axis_db_pool_release(self->pool, conn);

	axis_storage_forget_volume(self, id);
	g_message("volume '%s' (id %" G_GINT64_FORMAT ") at %s removed with its %" G_GUINT64_FORMAT
		" root(s); nothing on the disk was touched and its marker stays in place", record->name, id,
		record->path, roots);
	axis_storage_notify(self, NULL, FALSE);
	return TRUE;

fail:
	end_locked(self, conn);
	return FALSE;
}

/* ── Re-pointing ────────────────────────────────────────────────── */

/* The volume @id as the database has it now. */
static const AxisVolumeInfo *
find_id(
	GPtrArray *rows,
	gint64     id
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisVolumeInfo *row = g_ptr_array_index(rows, i);

		if (row->id == id)
			return row;
	}

	return NULL;
}

/* The volume other than @exclude recorded at exactly @path, if any. */
static const AxisVolumeInfo *
find_path(
	GPtrArray   *rows,
	const gchar *path,
	gint64       exclude
){
	guint i;

	for (i = 0; i < rows->len; i++) {
		const AxisVolumeInfo *row = g_ptr_array_index(rows, i);

		if (row->id != exclude && strcmp(row->path, path) == 0)
			return row;
	}

	return NULL;
}

/* One volume's new place, as the probe of it found it. */
typedef struct
{
	gint64                 id;
	const gchar           *path;
	const AxisVolumeProbe *probe;
	AxisVolumeState        state;
} Place;

/*
 * Records each of @places (one, or the two of a swap) at its path, in one
 * statement: the path's uniqueness is checked at its end (0050), so two
 * volumes change paths without a moment, even inside the transaction, with
 * two at one path.  What the probe found of the filesystem there (its
 * identifier, device and UUID) replaces what was recorded of the old place.
 * *@infos: the rows afterwards, in the order given.
 */
static gboolean
record_places(
	AxisDbConn   *conn,
	const Place  *places,
	guint         n,
	GPtrArray   **infos,
	GError      **error
){
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) sql = g_string_new("UPDATE volume SET path = n.npath, mount_root = n.nmount_root,"
		" fs_type = n.nfs_type, fsid = n.nfsid, device = n.ndevice, fs_uuid = nullif(n.nfs_uuid, ''),"
		" state = n.nstate, state_reason = 'none',"
		" state_changed_at = CASE WHEN state <> n.nstate OR state_reason <> 'none' THEN now()"
		"   ELSE state_changed_at END, unusable_since = NULL,"
		" last_checked_at = now(), last_online_at = now(), total_bytes = n.ntotal, free_bytes = n.nfree,"
		" available_bytes = n.navailable, space_checked_at = now() FROM (VALUES ");
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[22];
	guint i;
	guint r;

	g_return_val_if_fail(n >= 1 && n <= 2, FALSE);

	for (i = 0; i < n; i++) {
		const AxisVolumeProbe *probe = places[i].probe;
		guint base = i * 11;

		g_ptr_array_add(owned, g_strdup_printf("%" G_GINT64_FORMAT, places[i].id));
		params[base] = g_ptr_array_index(owned, owned->len - 1);
		params[base + 1] = places[i].path;
		params[base + 2] = probe->identity.mount_root ? "true" : "false";
		params[base + 3] = axis_storage_fs_type_name(probe->info.magic);
		params[base + 4] = probe->info.fsid;
		g_ptr_array_add(owned, axis_storage_fs_device_text(probe->identity.dev));
		params[base + 5] = g_ptr_array_index(owned, owned->len - 1);
		params[base + 6] = axis_volume_state_to_string(places[i].state);
		g_ptr_array_add(owned, g_strdup_printf("%" G_GUINT64_FORMAT, probe->info.total_bytes));
		params[base + 7] = g_ptr_array_index(owned, owned->len - 1);
		g_ptr_array_add(owned, g_strdup_printf("%" G_GUINT64_FORMAT, probe->info.free_bytes));
		params[base + 8] = g_ptr_array_index(owned, owned->len - 1);
		g_ptr_array_add(owned, g_strdup_printf("%" G_GUINT64_FORMAT, probe->info.available_bytes));
		params[base + 9] = g_ptr_array_index(owned, owned->len - 1);
		params[base + 10] = probe->fs_uuid;
		g_string_append_printf(sql, "%s($%u::bigint, $%u, $%u::boolean, $%u, $%u, $%u, $%u, $%u::bigint,"
			" $%u::bigint, $%u::bigint, $%u)", i > 0 ? ", " : "", base + 1, base + 2, base + 3, base + 4,
			base + 5, base + 6, base + 7, base + 8, base + 9, base + 10, base + 11);
	}
	g_string_append(sql, ") AS n(nid, npath, nmount_root, nfs_type, nfsid, ndevice, nstate, ntotal, nfree,"
		" navailable, nfs_uuid) WHERE volume.id = n.nid RETURNING " AXIS_VOLUME_COLUMNS);
	if (!axis_db_conn_exec(conn, sql->str, n * 11, params, &result, error))
		return FALSE;
	if (axis_db_result_get_n_rows(result) != n) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"the volume is no longer registered");
		return FALSE;
	}
	*infos = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_info_free);
	for (i = 0; i < n; i++) {
		for (r = 0; r < n; r++) {
			gint64 got = 0;

			axis_db_result_get_int64(result, r, 0, &got);
			if (got == places[i].id)
				g_ptr_array_add(*infos, axis_volume_info_from_row(result, r));
		}
	}
	return TRUE;
}

gboolean
axis_storage_relocate_volume(
	AxisStorage  *self,
	gint64        id,
	const gchar  *path,
	gboolean      not_mount_point_ok,
	GError      **error
){
	g_autoptr(GPtrArray) infos = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *normalised = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *other_name = NULL;
	g_autofree gchar *was = NULL;
	const AxisVolumeInfo *record;
	const AxisVolumeInfo *other = NULL;
	AxisStorageProbeCall *call = NULL;
	AxisStorageProbeCall *old_call = NULL;
	AxisVolumeProbe *probe;
	AxisVolumeProbe *old = NULL;
	AxisVolumeState state;
	AxisVolumeReason reason;
	Place places[2];
	const gchar *problem = NULL;
	AxisDbConn *conn;
	GError *local = NULL;
	gboolean ok = FALSE;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!axis_storage_path_normalize(path, TRUE, &normalised, &problem))
		return refused(error, AXIS_STORAGE_ERROR_INVALID, problem);

	conn = begin_locked(self, error);
	if (conn == NULL)
		return FALSE;

	/* Both volumes of a swap as the database has them now, under the lock,
	 * never as the daemon remembered them before it. */
	rows = read_volumes(conn, error);
	if (rows == NULL)
		goto out;
	record = find_id(rows, id);
	if (record == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, id);
		goto out;
	}
	was = g_strdup(record->path);
	if (strcmp(normalised, record->path) == 0) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, "That is the disk's path already.");
		goto out;
	}
	/* Exactly another volume's path: the two disks may have been swapped
	 * between their mount points (decided below, on what is there).  Either
	 * way, no new path may hold or be inside a third volume. */
	other = find_path(rows, normalised, id);
	if (other == NULL && overlaps(rows, normalised, id, &refusal)) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, refusal);
		goto out;
	}
	if (other != NULL && (overlaps_except(rows, normalised, id, other->id, &refusal) ||
	                      overlaps_except(rows, record->path, id, other->id, &refusal))) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, refusal);
		goto out;
	}

	call = probe_path(self, normalised, FALSE, &local);
	if (call == NULL) {
		g_propagate_error(error, local);
		goto out;
	}
	probe = axis_storage_probe_call_get_data(call);
	axis_volume_probe_classify(probe, record, FALSE, &state, &reason);
	if (!axis_volume_state_is_usable(state)) {
		if (other != NULL)
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"This is already the path of disk '%s', and the directory there is not this disk: %s",
				other->name, axis_volume_reason_describe(reason));
		else
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
				"The directory at the new path is not this disk: %s",
				axis_volume_reason_describe(reason));
		goto out;
	}
	if (same_filesystem(rows, probe, id, &refusal, NULL)) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, refusal);
		goto out;
	}
	if (!probe->identity.mount_root && !not_mount_point_ok) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, "The directory is not a mount point. Confirm "
			"that the disk should be pointed at it anyway.");
		goto out;
	}

	old_call = probe_path(self, record->path, FALSE, NULL);
	if (old_call != NULL)
		old = axis_storage_probe_call_get_data(old_call);
	places[0].id = id;
	places[0].path = normalised;
	places[0].probe = probe;
	places[0].state = state;
	if (other == NULL) {
		/* Two directories carrying one marker: a copy, not a move. */
		if (old != NULL) {
			AxisVolumeState old_state;
			AxisVolumeReason old_reason;

			axis_volume_probe_classify(old, record, record->mount_root, &old_state, &old_reason);
			if (axis_volume_state_is_usable(old_state) &&
			    (old->identity.dev != probe->identity.dev || old->identity.ino != probe->identity.ino)) {
				refused(error, AXIS_STORAGE_ERROR_REFUSED, "The disk is still present at its current "
					"path, so the directory at the new path is a copy. Nothing was changed.");
				goto out;
			}
		}
	} else {
		/* The new path is the other volume's, and this volume is there: the
		 * other must be proven at this one's path, where it goes, or the
		 * two would be at one path.  Nothing is changed otherwise. */
		AxisVolumeState other_state = AXIS_VOLUME_OFFLINE;
		AxisVolumeReason other_reason = AXIS_VOLUME_REASON_TIMEOUT;

		if (old != NULL)
			axis_volume_probe_classify(old, other, FALSE, &other_state, &other_reason);
		if (!axis_volume_state_is_usable(other_state)) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED, "This is already the path of "
				"disk '%s', which is not there (this disk is). It is not at this disk's path either, "
				"so the two cannot change places: %s Point '%s' at the path where its disk is mounted first. "
				"Nothing was changed.", other->name, axis_volume_reason_describe(other_reason), other->name);
			goto out;
		}
		if (same_filesystem(rows, old, other->id, &refusal, NULL)) {
			refused(error, AXIS_STORAGE_ERROR_REFUSED, refusal);
			goto out;
		}
		if (!old->identity.mount_root && !not_mount_point_ok) {
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED, "This disk's directory, "
				"where disk '%s' would go, is not a mount point. Confirm that the disks should be pointed "
				"at them anyway.", other->name);
			goto out;
		}
		places[1].id = other->id;
		places[1].path = record->path;
		places[1].probe = old;
		places[1].state = other_state;
		other_name = g_strdup(other->name);
	}

	if (!record_places(conn, places, other != NULL ? 2 : 1, &infos, error) ||
	    !axis_db_conn_commit(conn, error))
		goto out;
	ok = TRUE;

out:
	end_locked(self, conn);
	axis_storage_probe_call_unref(call);
	axis_storage_probe_call_unref(old_call);
	if (!ok)
		return FALSE;

	for (i = 0; i < infos->len; i++) {
		const AxisVolumeInfo *info = g_ptr_array_index(infos, i);

		axis_storage_put_volume(self, info);
		if (other_name == NULL)
			g_message("volume '%s' (id %" G_GINT64_FORMAT ") now looked for at %s (was %s)", info->name,
				info->id, info->path, was);
		else
			g_message("volume '%s' (id %" G_GINT64_FORMAT ") now looked for at %s (was %s): its disk and "
				"another volume's were swapped between their mount points", info->name, info->id, info->path,
				i == 0 ? was : normalised);
	}
	axis_storage_notify(self, NULL, FALSE);
	return TRUE;
}

/* ── Restoring a deleted marker ─────────────────────────────────── */

/* A device as the volume's row records it ("major:minor"). */
static gboolean
device_of(
	const gchar *text,
	guint64     *dev
){
	guint major_number = 0;
	guint minor_number = 0;
	gchar extra;

	if (text == NULL || sscanf(text, "%u:%u%c", &major_number, &minor_number, &extra) != 2)
		return FALSE;
	*dev = (guint64)makedev(major_number, minor_number);
	return TRUE;
}

AxisMarkerRestoreFs
axis_volume_restore_compare(
	const AxisVolumeInfo  *record,
	guint64                magic,
	const gchar           *fsid,
	const gchar           *fs_uuid,
	guint64                dev,
	guint64                bytes,
	gchar                **text
){
	const gchar *type = axis_storage_fs_type_name(magic);
	g_autofree gchar *device = axis_storage_fs_device_text(dev);
	guint64 was_dev = 0;
	gboolean device_number;

	*text = NULL;
	if (!fsid_is_meaningful(record->fsid) || record->fs_type == NULL || record->fs_type[0] == '\0') {
		*text = g_strdup("Axis has no record of the filesystem this disk was last seen on, so it cannot tell "
			"whether this is its disk.");
		return AXIS_MARKER_RESTORE_FS_UNKNOWN;
	}
	if (strcmp(record->fs_type, type) != 0) {
		*text = g_strdup_printf("The filesystem at its path is not the one this disk was last seen on: that was "
			"%s, and this is %s. It may be another disk with the same folders.", record->fs_type, type);
		return AXIS_MARKER_RESTORE_FS_DIFFERS;
	}
	/* For xfs and f2fs the identifier is the device's number: the UUID the
	 * volume's last check recorded decides, where both are known. */
	device_number = (device_of(record->device, &was_dev) && axis_storage_fs_fsid_is_device(record->fsid, was_dev)) ||
		axis_storage_fs_fsid_is_device(fsid, dev);
	if (device_number && record->fs_uuid != NULL && fs_uuid != NULL && fs_uuid[0] != '\0') {
		if (strcmp(record->fs_uuid, fs_uuid) == 0) {
			*text = g_strdup_printf("The filesystem at its path is the one this disk was last seen on (%s, "
				"UUID %s).", type, fs_uuid);
			return AXIS_MARKER_RESTORE_FS_SAME;
		}
		*text = g_strdup_printf("The filesystem at its path is not the one this disk was last seen on: its UUID "
			"was %s, and it is %s now. It may be another disk with the same folders.", record->fs_uuid, fs_uuid);
		return AXIS_MARKER_RESTORE_FS_DIFFERS;
	}
	if (g_strcmp0(fsid, record->fsid) != 0) {
		*text = g_strdup_printf("The filesystem at its path is not the one this disk was last seen on: its "
			"identifier was %s, and it is %s now. It may be another disk with the same folders.%s", record->fsid,
			fsid != NULL ? fsid : "", device_number ? " For this kind of filesystem the identifier is only the "
			"number of the device it is on, which changes when disks are found in another order, so it may "
			"still be this disk." : "");
		return AXIS_MARKER_RESTORE_FS_DIFFERS;
	}
	if (!device_number) {
		*text = g_strdup_printf("The filesystem at its path is the one this disk was last seen on (%s, "
			"identifier %s).", type, fsid);
		return AXIS_MARKER_RESTORE_FS_SAME;
	}
	if (record->has_space && record->total_bytes != bytes) {
		g_autofree gchar *was = axis_size_format(record->total_bytes);
		g_autofree gchar *now = axis_size_format(bytes);

		*text = g_strdup_printf("The filesystem at its path is not the one this disk was last seen on: its "
			"size was %s, and it is %s now. It may be another disk with the same folders.", was, now);
		return AXIS_MARKER_RESTORE_FS_DIFFERS;
	}
	*text = g_strdup_printf("Axis cannot tell whether the filesystem at its path is the one this disk was last "
		"seen on: this filesystem (%s) reports only the number of the device it is on (%s), the same as then, "
		"and another disk can be found at that number.", type, device);
	return AXIS_MARKER_RESTORE_FS_DEVICE_ONLY;
}

typedef struct
{
	gchar                  *path;
	GPtrArray              *rel_paths;	/* of gchar* */
	gint                    open_errno;
	AxisStorageFsIdentity   identity;
	gboolean                info_ok;
	AxisStorageFsInfo       info;
	gchar                   fs_uuid[AXIS_STORAGE_FS_UUID_CHARS + 1];	/* "" when none */
	AxisVolumeMarkerStatus  marker_status;
	AxisVolumeMarker        marker;
	gboolean                empty_known;
	gboolean                empty;
	GArray                 *present;	/* gboolean per rel path */
} RestoreLook;

static void
restore_look_free(gpointer data)
{
	RestoreLook *look = data;

	g_free(look->path);
	g_ptr_array_unref(look->rel_paths);
	g_array_unref(look->present);
	g_free(look);
}

static void
restore_look_run(gpointer data)
{
	RestoreLook *look = data;
	gint fd;
	gint err = 0;
	guint i;

	fd = axis_storage_fs_open_path(look->path, &look->open_errno);
	if (fd < 0)
		return;
	if (!axis_storage_fs_identify(fd, &look->identity, &err)) {
		look->open_errno = err != 0 ? err : EIO;
		close(fd);
		return;
	}
	look->info_ok = axis_storage_fs_info(fd, &look->info, NULL);
	/* Never asked of a FUSE filesystem, whose program would get it. */
	if (look->info_ok && axis_storage_fs_type_is_local(look->info.magic))
		axis_storage_fs_read_uuid(fd, look->fs_uuid);
	look->marker_status = axis_volume_marker_read(fd, &look->marker, NULL);
	if (look->marker_status == AXIS_VOLUME_MARKER_MISSING)
		look->empty_known = axis_storage_fs_is_empty(fd, &look->empty, NULL);

	for (i = 0; i < look->rel_paths->len; i++) {
		gint dir = axis_storage_fs_open_beneath(fd, g_ptr_array_index(look->rel_paths, i), NULL);
		gboolean present = dir >= 0;

		if (dir >= 0)
			close(dir);
		g_array_append_val(look->present, present);
	}
	close(fd);
}

gboolean
axis_storage_inspect_marker_restore(
	AxisStorage                  *self,
	gint64                        id,
	AxisMarkerRestoreInspection **inspection,
	GError                      **error
){
	g_autoptr(AxisVolumeInfo) record = NULL;
	g_autofree gchar *key = NULL;
	AxisMarkerRestoreInspection *in;
	AxisStorageProbeCall *call;
	RestoreLook *look;
	GError *local = NULL;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(inspection != NULL, FALSE);

	*inspection = NULL;
	record = axis_storage_get_volume(self, id);
	if (record == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	in = g_new0(AxisMarkerRestoreInspection, 1);
	in->filesystem = AXIS_MARKER_RESTORE_FS_UNKNOWN;
	in->root_present = g_array_new(FALSE, TRUE, sizeof(gboolean));
	if (!axis_storage_list_roots(self, id, &in->roots, error)) {
		axis_marker_restore_inspection_free(in);
		return FALSE;
	}

	look = g_new0(RestoreLook, 1);
	look->path = g_strdup(record->path);
	look->rel_paths = g_ptr_array_new_with_free_func(g_free);
	look->present = g_array_new(FALSE, TRUE, sizeof(gboolean));
	for (i = 0; i < in->roots->len; i++) {
		const AxisRootInfo *root = g_ptr_array_index(in->roots, i);

		g_ptr_array_add(look->rel_paths, g_strdup(root->rel_path));
	}

	key = axis_storage_volume_key(id);
	call = axis_storage_probe_start(self->probe, key, restore_look_run, look, restore_look_free,
		&local);
	if (call == NULL || !axis_storage_probe_wait(call, g_get_monotonic_time() +
	                                             (gint64)axis_storage_check_timeout(self) * 1000)) {
		axis_storage_probe_call_unref(call);
		g_clear_error(&local);
		in->reason = AXIS_VOLUME_REASON_TIMEOUT;
		for (i = 0; i < in->roots->len; i++) {
			gboolean absent = FALSE;

			g_array_append_val(in->root_present, absent);
		}
		*inspection = in;
		return TRUE;
	}

	look = axis_storage_probe_call_get_data(call);
	if (look->open_errno == ENOENT)
		in->reason = AXIS_VOLUME_REASON_PATH_MISSING;
	else if (look->open_errno == ELOOP || look->open_errno == ENOTDIR)
		in->reason = AXIS_VOLUME_REASON_NOT_DIRECTORY;
	else if (look->open_errno != 0)
		in->reason = AXIS_VOLUME_REASON_IO_ERROR;
	else if (record->mount_root && !look->identity.mount_root)
		in->reason = AXIS_VOLUME_REASON_NOT_MOUNTED;
	else if (look->marker_status == AXIS_VOLUME_MARKER_FOUND)
		in->reason = strcmp(look->marker.uuid, record->uuid) == 0 ? AXIS_VOLUME_REASON_NONE :
			AXIS_VOLUME_REASON_OTHER_MARKER;
	else if (look->marker_status == AXIS_VOLUME_MARKER_INVALID)
		in->reason = AXIS_VOLUME_REASON_BAD_MARKER;
	else if (look->marker_status == AXIS_VOLUME_MARKER_MISSING && look->empty_known)
		in->reason = look->empty ? AXIS_VOLUME_REASON_EMPTY : AXIS_VOLUME_REASON_NO_MARKER;
	else
		in->reason = AXIS_VOLUME_REASON_IO_ERROR;

	/* A volume that never had its marker gets it from a check, once it is
	 * recognised and mounted read-write; nothing is restored. */
	in->possible = in->reason == AXIS_VOLUME_REASON_NO_MARKER && look->info_ok && record->has_marker;
	in->dev = look->identity.dev;
	in->ino = look->identity.ino;
	if (look->info_ok)
		g_strlcpy(in->fsid, look->info.fsid, sizeof(in->fsid));
	/* Not the roots' names alone: the filesystem there against the one the
	 * volume was last seen on. */
	if (look->open_errno == 0 && look->info_ok)
		in->filesystem = axis_volume_restore_compare(record, look->info.magic, look->info.fsid, look->fs_uuid,
			look->identity.dev, look->info.total_bytes, &in->filesystem_text);
	else if (look->open_errno == 0)
		in->filesystem_text = g_strdup("The details of the filesystem at its path cannot be read, so Axis cannot "
			"compare it with the one this disk was last seen on.");
	for (i = 0; i < in->roots->len; i++) {
		gboolean present = i < look->present->len && g_array_index(look->present, gboolean, i);

		g_array_append_val(in->root_present, present);
	}
	axis_storage_probe_call_unref(call);

	*inspection = in;
	return TRUE;
}

gboolean
axis_storage_restore_marker(
	AxisStorage  *self,
	gint64        id,
	guint64       dev,
	guint64       ino,
	const gchar  *fsid,
	gboolean      other_filesystem_ok,
	GError      **error
){
	g_autoptr(AxisVolumeInfo) record = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *refusal = NULL;
	g_autofree gchar *compared = NULL;
	AxisStorageProbeCall *call = NULL;
	AxisVolumeProbe *probe;
	AxisVolumeState state;
	AxisVolumeReason reason;
	AxisVolumeMarker marker;
	AxisDbConn *conn;
	GError *local = NULL;
	gboolean ok = FALSE;

	g_return_val_if_fail(self != NULL, FALSE);

	if (axis_storage_is_read_only_mode(self))
		return refused(error, AXIS_STORAGE_ERROR_READ_ONLY, "Read-only storage is on, so "
			"Axis does not restore markers. Turn off Read-only storage in Settings first, or "
			"remove this disk and add it again.");

	record = axis_storage_get_volume(self, id);
	if (record == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	conn = begin_locked(self, error);
	if (conn == NULL)
		return FALSE;

	call = probe_path(self, record->path, TRUE, &local);
	if (call == NULL) {
		g_propagate_error(error, local);
		goto out;
	}
	probe = axis_storage_probe_call_get_data(call);
	axis_volume_probe_classify(probe, record, record->mount_root, &state, &reason);
	if (state != AXIS_VOLUME_MISMATCH || reason != AXIS_VOLUME_REASON_NO_MARKER) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED,
			"A marker is only restored into a directory that has files and no marker. Now: %s",
			axis_volume_reason_describe(reason));
		goto out;
	}
	if (probe->identity.dev != dev || probe->identity.ino != ino || !probe->info_ok ||
	    g_strcmp0(probe->info.fsid, fsid) != 0) {
		refused(error, AXIS_STORAGE_ERROR_CONFIRM, "What is at this disk's path has changed since "
			"it was shown. Nothing was done; look at it again before confirming.");
		goto out;
	}
	/* One filesystem is one volume, as for adding: a directory of another
	 * volume's filesystem never gets this one's marker. */
	rows = read_volumes(conn, error);
	if (rows == NULL)
		goto out;
	if (same_filesystem(rows, probe, id, &refusal, NULL)) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, refusal);
		goto out;
	}
	/* Not proven the filesystem the volume was last seen on: only when the
	 * administrator, told so, says it is the volume's disk. */
	if (axis_volume_restore_compare(record, probe->info.magic, probe->info.fsid, probe->fs_uuid, probe->identity.dev,
	                                probe->info.total_bytes, &compared) != AXIS_MARKER_RESTORE_FS_SAME &&
	    !other_filesystem_ok) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM, "%s Nothing was done. Write the marker "
			"only once you are sure this is this disk, and confirm that.", compared);
		goto out;
	}

	memset(&marker, 0, sizeof(marker));
	g_strlcpy(marker.uuid, record->uuid, sizeof(marker.uuid));
	marker.created = CLAMP(record->marker_created_us / G_USEC_PER_SEC, 0, AXIS_VOLUME_MARKER_MAX_TIME);
	if (!write_marker(self, record->path, probe->fd, &marker, error))
		goto out;
	ok = TRUE;

out:
	end_locked(self, conn);
	axis_storage_probe_call_unref(call);
	if (!ok)
		return FALSE;

	g_message("volume '%s' (id %" G_GINT64_FORMAT "): its marker %s was written again at %s",
		record->name, id, record->uuid, record->path);
	/* Its state follows from the check, as always. */
	axis_storage_check_record(self, record, NULL, NULL);
	return TRUE;
}

/* ── The marker of a volume added without one ───────────────────── */

/* @text as the column takes it: valid UTF-8, no control character, at most
 * 300 characters. */
static gchar *
problem_text(const gchar *text)
{
	g_autofree gchar *valid = g_utf8_make_valid(text != NULL && text[0] != '\0' ? text : "unknown error", -1);
	gchar *p;

	for (p = valid; *p != '\0'; p++) {
		if ((guchar)*p < 0x20 || *p == 0x7f)
			*p = ' ';
	}
	if (g_utf8_strlen(valid, -1) > 300)
		return g_utf8_substring(valid, 0, 300);
	return g_steal_pointer(&valid);
}

/* Records why the marker could not be written, logging it when it is not
 * what was recorded already. */
static void
record_problem(
	AxisStorage          *self,
	const AxisVolumeInfo *record,
	const gchar          *why
){
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, record->id);
	g_autofree gchar *text = problem_text(why);
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *params[3];

	params[0] = id_text;
	params[1] = record->uuid;
	params[2] = text;
	if (!axis_db_pool_exec(self->pool, "UPDATE volume SET marker_problem = $3"
	                       " WHERE id = $1::bigint AND uuid = $2::uuid AND identity = 'filesystem'"
	                       " AND marker_problem IS DISTINCT FROM $3 RETURNING " AXIS_VOLUME_COLUMNS,
	                       G_N_ELEMENTS(params), params, &result, &error)) {
		g_warning("volume '%s' (id %" G_GINT64_FORMAT "): its marker could not be written (%s), and that could "
			"not be recorded: %s", record->name, record->id, text, error->message);
		return;
	}
	if (axis_db_result_get_n_rows(result) == 1) {
		g_autoptr(AxisVolumeInfo) now = axis_volume_info_from_row(result, 0);

		g_warning("volume '%s' (id %" G_GINT64_FORMAT ") is mounted read-write at %s, but its marker could not be "
			"written: %s. Nothing is written to it before its marker; Axis tries again at every check.",
			record->name, record->id, record->path, text);
		axis_storage_put_volume(self, now);
		axis_storage_notify(self, now, FALSE);
	}
}

gboolean
axis_storage_write_late_marker(
	AxisStorage            *self,
	const AxisVolumeInfo   *record,
	const AxisVolumeProbe  *probe,
	AxisVolumeInfo        **after,
	GError                **error
){
	g_autoptr(AxisDbResult) result = NULL;
	g_autoptr(AxisVolumeInfo) current = NULL;
	g_autoptr(AxisVolumeInfo) now = NULL;
	g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, record->id);
	g_autofree gchar *created = NULL;
	AxisVolumeMarker marker;
	const gchar *params[2];
	gboolean found;
	AxisDbConn *conn;
	GError *local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (after != NULL)
		*after = NULL;
	found = probe->marker_status == AXIS_VOLUME_MARKER_FOUND && strcmp(probe->marker.uuid, record->uuid) == 0;
	if (record->has_marker) {
		if (after != NULL)
			*after = axis_volume_info_copy(record);
		return TRUE;
	}
	/* Only through the directory just recognised, mounted read-write. */
	if (!found && (probe->fd < 0 || probe->marker_status != AXIS_VOLUME_MARKER_MISSING || !probe->info_ok ||
	               probe->info.read_only || !axis_volume_fs_identity_matches(&record->fs_identity, probe, NULL)))
		return refused(error, AXIS_STORAGE_ERROR_REFUSED, "This disk's directory is not the one recognised, "
			"mounted read-write.");

	conn = begin_locked(self, error);
	if (conn == NULL)
		return FALSE;

	/* Decided on the row as it is now: the same volume at the same path,
	 * still without its marker. */
	params[0] = id_text;
	if (!axis_db_conn_exec(conn, "SELECT " AXIS_VOLUME_COLUMNS " FROM volume WHERE id = $1::bigint FOR UPDATE",
	                       1, params, &result, error))
		goto fail;
	if (axis_db_result_get_n_rows(result) != 1) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, record->id);
		goto fail;
	}
	current = axis_volume_info_from_row(result, 0);
	g_clear_pointer(&result, axis_db_result_free);
	if (strcmp(current->uuid, record->uuid) != 0 || strcmp(current->path, record->path) != 0) {
		refused(error, AXIS_STORAGE_ERROR_REFUSED, "This disk was changed meanwhile; nothing was written.");
		goto fail;
	}
	if (current->has_marker) {
		/* Written meanwhile (another daemon on the same database). */
		end_locked(self, conn);
		if (after != NULL)
			*after = g_steal_pointer(&current);
		return TRUE;
	}

	if (found) {
		marker = probe->marker;
	} else {
		memset(&marker, 0, sizeof(marker));
		g_strlcpy(marker.uuid, record->uuid, sizeof(marker.uuid));
		marker.created = CLAMP(g_get_real_time() / G_USEC_PER_SEC, 0, AXIS_VOLUME_MARKER_MAX_TIME);
		/* The write the administrator confirmed when adding the volume;
		 * the one storage allows in read-only mode. */
		if (!write_marker(self, record->path, probe->fd, &marker, &local)) {
			end_locked(self, conn);
			record_problem(self, record, local->message);
			g_propagate_error(error, local);
			return FALSE;
		}
	}

	created = g_strdup_printf("%" G_GINT64_FORMAT, marker.created);
	params[1] = created;
	if (!axis_db_conn_exec(conn, "UPDATE volume SET identity = 'marker',"
	                       " marker_created_at = to_timestamp($2::double precision), marker_problem = NULL"
	                       " WHERE id = $1::bigint AND identity = 'filesystem' RETURNING " AXIS_VOLUME_COLUMNS,
	                       G_N_ELEMENTS(params), params, &result, error) ||
	    axis_db_result_get_n_rows(result) != 1) {
		if (error != NULL && *error == NULL)
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_FAILED, "the volume's row could not be updated");
		/* The marker on the disk is the volume's own: the next check
		 * finds it and records it. */
		goto fail;
	}
	now = axis_volume_info_from_row(result, 0);
	if (!axis_db_conn_commit(conn, error))
		goto fail;
	axis_db_pool_release(self->pool, conn);

	axis_storage_put_volume(self, now);
	if (found)
		g_message("volume '%s' (id %" G_GINT64_FORMAT "): its own marker %s is at %s (written by an earlier attempt); "
			"it is recognised by its marker from now on", now->name, now->id, now->uuid, now->path);
	else
		g_message("volume '%s' (id %" G_GINT64_FORMAT "): its marker %s was written at %s, the first time Axis found "
			"it mounted read-write; it is recognised by its marker from now on", now->name, now->id, now->uuid,
			now->path);
	axis_storage_notify(self, now, FALSE);
	if (after != NULL)
		*after = g_steal_pointer(&now);
	return TRUE;

fail:
	end_locked(self, conn);
	return FALSE;
}
