/*
 * axis-volume-guard.c - the only way to touch files on a volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Acquiring and checking both run the same probe as a presence check, on
 * a helper with a deadline.  A check hands the probe a dup() of the
 * guard's descriptor: the path is opened afresh and must lead to the same
 * directory on the same mount (device, inode and mount id, as a proof
 * compares), and the marker is then read through the guard's own
 * descriptor, so a disk unmounted (even lazily), replaced, or mounted at
 * the path again (read-only, say) under a held guard fails the check.
 * Whether it is mounted read-only is read through both.
 *
 * A volume added without a marker is recognised by its filesystem identity
 * for reading.  Nothing is ever written to it before its marker: a write
 * guard on it writes the marker first (axis_storage_write_late_marker(),
 * through the descriptor the guard then holds) and is refused when that
 * fails, and every write guard demands the marker at each of its checks.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-volume-guard.h"
#include "storage/axis-storage-path.h"
#include "storage/axis-storage-private.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <unistd.h>

struct _AxisVolumeGuard
{
	AxisStorage      *storage;
	gint64            volume_id;
	gchar            *uuid;
	gchar            *name;
	gchar            *path;
	gboolean          mount_root;
	AxisVolumeInfo   *recognised;	/* what checks recognise it by: a write guard's demands its marker */
	AxisVolumeIntent  intent;
	gint              fd;
	guint64           dev;
	guint64           ino;
	gboolean          has_mnt_id;
	guint64           mnt_id;
	gboolean          failed;
};

static gboolean
unavailable(
	GError      **error,
	const gchar  *name,
	const gchar  *why
){
	g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
		"Disk '%s' is offline: %s Try again later.", name, why);
	return FALSE;
}

static gboolean
read_only_mode(
	GError      **error,
	const gchar  *name
){
	g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY,
		"Read-only storage is on: nothing on disk '%s' may be changed.", name);
	return FALSE;
}

AxisVolumeGuard *
axis_volume_guard_acquire(
	AxisStorage       *storage,
	gint64             volume_id,
	AxisVolumeIntent   intent,
	GError           **error
){
	g_autoptr(AxisVolumeInfo) record = NULL;
	g_autoptr(AxisVolumeInfo) marked = NULL;
	g_autofree gchar *key = NULL;
	AxisStorageProbeCall *call;
	AxisVolumeProbe *probe;
	AxisVolumeGuard *guard;
	AxisVolumeState state;
	AxisVolumeReason reason;
	GError *local = NULL;

	g_return_val_if_fail(storage != NULL, NULL);

	record = axis_storage_get_volume(storage, volume_id);
	if (record == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, volume_id);
		return NULL;
	}
	if (intent == AXIS_VOLUME_INTENT_WRITE && axis_storage_is_read_only_mode(storage)) {
		read_only_mode(error, record->name);
		return NULL;
	}

	key = axis_storage_volume_key(volume_id);
	probe = axis_volume_probe_new(record->path);
	probe->want_empty = TRUE;
	probe->want_fs_uuid = !record->has_marker;
	probe->keep_fd = TRUE;
	axis_storage_get_hook(storage, &probe->hook, &probe->hook_data);

	call = axis_storage_run_probe(storage, key, probe, axis_storage_check_timeout(storage), &local);
	if (call == NULL) {
		g_autofree gchar *why = g_strdup_printf("%s.", local->message);

		g_error_free(local);
		unavailable(error, record->name, why);
		return NULL;
	}

	probe = axis_storage_probe_call_get_data(call);
	axis_volume_probe_classify(probe, record, record->mount_root, &state, &reason);
	if (!axis_volume_state_is_usable(state) || probe->fd < 0) {
		axis_storage_probe_call_unref(call);
		unavailable(error, record->name, axis_volume_reason_describe(reason));
		return NULL;
	}
	if (intent == AXIS_VOLUME_INTENT_WRITE && state == AXIS_VOLUME_READ_ONLY) {
		axis_storage_probe_call_unref(call);
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY,
			"Disk '%s' is mounted read-only: nothing on it may be changed.", record->name);
		return NULL;
	}
	/* First the marker: nothing is written to a volume that has none, and
	 * from here this guard recognises the volume by it alone. */
	if (intent == AXIS_VOLUME_INTENT_WRITE && !record->has_marker) {
		if (!axis_storage_write_late_marker(storage, record, probe, &marked, &local)) {
			axis_storage_probe_call_unref(call);
			g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
				"Disk '%s' has no marker yet, and Axis could not write it now (%s). Nothing is written to a "
				"disk before its marker. Try again later.", record->name, local->message);
			g_error_free(local);
			return NULL;
		}
		axis_volume_info_free(record);
		record = g_steal_pointer(&marked);
	}

	guard = g_new0(AxisVolumeGuard, 1);
	guard->storage = axis_storage_ref(storage);
	guard->volume_id = volume_id;
	guard->uuid = g_strdup(record->uuid);
	guard->name = g_strdup(record->name);
	guard->path = g_strdup(record->path);
	guard->mount_root = record->mount_root;
	guard->recognised = axis_volume_info_copy(record);
	if (intent == AXIS_VOLUME_INTENT_WRITE)
		guard->recognised->has_marker = TRUE;
	guard->intent = intent;
	guard->fd = probe->fd;
	guard->dev = probe->identity.dev;
	guard->ino = probe->identity.ino;
	guard->has_mnt_id = probe->identity.has_mnt_id;
	guard->mnt_id = probe->identity.mnt_id;
	probe->fd = -1;
	axis_storage_probe_call_unref(call);

	/* The volume is about to be written: what an interrupted write of its
	 * marker left at its top goes first (the marker's own business, as the
	 * one write storage makes besides the write layer's). */
	if (intent == AXIS_VOLUME_INTENT_WRITE)
		axis_storage_sweep_marker_temporaries(storage, record, guard->fd);

	return guard;
}

static gboolean
fail(
	AxisVolumeGuard *self
){
	self->failed = TRUE;
	return FALSE;
}

gboolean
axis_volume_guard_check(
	AxisVolumeGuard  *self,
	GError          **error
){
	g_autoptr(AxisVolumeInfo) record = NULL;
	g_autofree gchar *key = NULL;
	AxisStorageProbeCall *call;
	AxisVolumeProbe *probe;
	AxisVolumeState state;
	AxisVolumeReason reason;
	GError *local = NULL;
	gint held;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->failed)
		return unavailable(error, self->name, "an earlier check of this guard failed.");

	record = axis_storage_get_volume(self->storage, self->volume_id);
	if (record == NULL || g_strcmp0(record->uuid, self->uuid) != 0) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"Disk '%s' is no longer registered.", self->name);
		return fail(self);
	}
	if (self->intent == AXIS_VOLUME_INTENT_WRITE && axis_storage_is_read_only_mode(self->storage)) {
		read_only_mode(error, self->name);
		return fail(self);
	}

	held = fcntl(self->fd, F_DUPFD_CLOEXEC, 0);
	if (held < 0) {
		g_autofree gchar *why = g_strdup_printf("%s.", g_strerror(errno));

		unavailable(error, self->name, why);
		return fail(self);
	}

	key = axis_storage_volume_key(self->volume_id);
	probe = axis_volume_probe_new(self->path);
	probe->held_fd = held;
	probe->held_dev = self->dev;
	probe->held_ino = self->ino;
	probe->held_has_mnt_id = self->has_mnt_id;
	probe->held_mnt_id = self->mnt_id;
	probe->want_empty = TRUE;
	probe->want_fs_uuid = !self->recognised->has_marker;
	axis_storage_get_hook(self->storage, &probe->hook, &probe->hook_data);

	call = axis_storage_run_probe(self->storage, key, probe, axis_storage_check_timeout(self->storage),
		&local);
	if (call == NULL) {
		g_autofree gchar *why = g_strdup_printf("%s.", local->message);

		g_error_free(local);
		unavailable(error, self->name, why);
		return fail(self);
	}

	probe = axis_storage_probe_call_get_data(call);
	if (probe->replaced) {
		axis_storage_probe_call_unref(call);
		unavailable(error, self->name, "its path no longer leads to the directory this guard "
			"holds: the disk was unmounted, or something else was put in its place.");
		return fail(self);
	}
	axis_volume_probe_classify(probe, self->recognised, self->mount_root, &state, &reason);
	axis_storage_probe_call_unref(call);
	if (!axis_volume_state_is_usable(state)) {
		unavailable(error, self->name, axis_volume_reason_describe(reason));
		return fail(self);
	}
	if (self->intent == AXIS_VOLUME_INTENT_WRITE && state == AXIS_VOLUME_READ_ONLY) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY,
			"Disk '%s' is mounted read-only now:nothing on it may be changed.", self->name);
		return fail(self);
	}

	return TRUE;
}

gboolean
axis_volume_guard_writable(
	AxisVolumeGuard  *self,
	GError          **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	if (self->intent != AXIS_VOLUME_INTENT_WRITE) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY,
			"This guard of disk '%s' was taken for reading: nothing may be written through it.", self->name);
		return FALSE;
	}
	if (self->failed)
		return unavailable(error, self->name, "an earlier check of this guard failed.");
	if (axis_storage_is_read_only_mode(self->storage))
		return read_only_mode(error, self->name);
	/* Never without a marker (a write guard is only granted once it is
	 * there, so this does not happen; it is said here as the rule). */
	if (!self->recognised->has_marker)
		return unavailable(error, self->name, "it has no marker yet.");
	return TRUE;
}

gint
axis_volume_guard_get_fd(AxisVolumeGuard *self)
{
	g_return_val_if_fail(self != NULL, -1);

	return self->fd;
}

/* ── Opening a directory beneath the top ────────────────────────── */

typedef struct
{
	gint   top;		/* a dup of the guard's descriptor */
	gchar *relative;
	gint   fd;		/* out */
	gint   saved_errno;	/* out */
} OpenJob;

static void
open_job_free(gpointer data)
{
	OpenJob *job = data;

	if (job->top >= 0)
		close(job->top);
	if (job->fd >= 0)
		close(job->fd);
	g_free(job->relative);
	g_free(job);
}

static void
open_job_run(gpointer data)
{
	OpenJob *job = data;

	job->fd = axis_storage_fs_open_beneath(job->top, job->relative, &job->saved_errno);
}

gint
axis_volume_guard_open_dir(
	AxisVolumeGuard  *self,
	const gchar      *relative,
	GError          **error
){
	g_autofree gchar *normalised = NULL;
	g_autofree gchar *key = NULL;
	const gchar *problem = NULL;
	AxisStorageProbeCall *call;
	OpenJob *job;
	GError *local = NULL;
	gint fd;
	gint err;

	g_return_val_if_fail(self != NULL, -1);

	if (!axis_storage_path_normalize(relative, FALSE, &normalised, &problem)) {
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID, problem);
		return -1;
	}
	if (self->failed) {
		unavailable(error, self->name, "an earlier check of this guard failed.");
		return -1;
	}

	job = g_new0(OpenJob, 1);
	job->top = fcntl(self->fd, F_DUPFD_CLOEXEC, 0);
	job->relative = g_strdup(normalised);
	job->fd = -1;
	if (job->top < 0) {
		g_autofree gchar *why = g_strdup_printf("%s.", g_strerror(errno));

		open_job_free(job);
		unavailable(error, self->name, why);
		return -1;
	}

	key = axis_storage_volume_key(self->volume_id);
	call = axis_storage_probe_start(self->storage->probe, key, open_job_run, job, open_job_free,
		&local);
	if (call == NULL || !axis_storage_probe_wait(call, g_get_monotonic_time() +
	                                             (gint64)axis_storage_check_timeout(self->storage) * 1000)) {
		g_autofree gchar *why = g_strdup_printf("%s.", local != NULL ? local->message :
			"the filesystem did not answer in time; it may not be responding");

		g_clear_error(&local);
		axis_storage_probe_call_unref(call);
		unavailable(error, self->name, why);
		self->failed = TRUE;
		return -1;
	}

	job = axis_storage_probe_call_get_data(call);
	fd = job->fd;
	err = job->saved_errno;
	job->fd = -1;
	axis_storage_probe_call_unref(call);
	if (fd >= 0)
		return fd;

	/* Whatever went wrong may be the volume going away: only an error
	 * about a volume that is still there is an error about the path. */
	if (!axis_volume_guard_check(self, error))
		return -1;

	if (err == ELOOP)
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
			"The path passes through a symbolic link, which Axis does not follow.");
	else if (err == EXDEV)
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED,
			"The path crosses onto another filesystem or mount, outside disk '%s'.", self->name);
	else if (err == EINVAL || err == ENAMETOOLONG)
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID,
			"The path is not valid.");
	else
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Cannot open %s on disk '%s': %s",
			normalised[0] != '\0' ? normalised : "the top directory", self->name, g_strerror(err));
	return -1;
}

/* ── Accessors ──────────────────────────────────────────────────── */

gint64
axis_volume_guard_get_volume_id(AxisVolumeGuard *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->volume_id;
}

const gchar *
axis_volume_guard_get_uuid(AxisVolumeGuard *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->uuid;
}

AxisVolumeIntent
axis_volume_guard_get_intent(AxisVolumeGuard *self)
{
	g_return_val_if_fail(self != NULL, AXIS_VOLUME_INTENT_READ);

	return self->intent;
}

guint64
axis_volume_guard_get_dev(AxisVolumeGuard *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return self->dev;
}

void
axis_volume_guard_release(AxisVolumeGuard *self)
{
	if (self == NULL)
		return;

	if (self->fd >= 0)
		close(self->fd);
	g_free(self->uuid);
	g_free(self->name);
	g_free(self->path);
	axis_volume_info_free(self->recognised);
	axis_storage_unref(self->storage);
	g_free(self);
}
