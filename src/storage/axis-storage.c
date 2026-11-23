/*
 * axis-storage.c - the daemon's knowledge of its volumes, and checking them
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What the daemon knows about every volume is kept here, in memory, behind
 * one lock: loaded from the database at start, replaced whenever a check
 * or an administrator's change has been stored.  Checks look at the disk
 * on a helper thread with a deadline and store their result; the lock is
 * never held while anything blocks.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "storage/axis-storage-private.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "core/axis-size.h"

/* ── Names ──────────────────────────────────────────────────────── */

static const gchar *const state_names[] = { "online", "read_only", "offline", "mismatch" };
static const gchar *const state_labels[] = { "Online", "Mounted read-only", "Offline", "Not recognised" };

static const gchar *const reason_names[] = {
	"none", "unchecked", "path_missing", "not_mounted", "empty", "timeout", "io_error",
	"no_marker", "other_marker", "bad_marker", "not_directory", "other_filesystem"
};

static const gchar *const reason_sentences[] = {
	"The disk is there and its marker matches.",
	"This disk has not been checked since Axis started.",
	"There is no directory at the disk's path.",
	"The directory is no longer a mount point: the disk is not mounted.",
	"The directory is empty and has no marker: the disk is not mounted.",
	"The filesystem did not answer in time; it may be hung.",
	"The directory or its marker could not be read.",
	"The directory has files but no marker: another disk, or the marker was deleted.",
	"The directory carries the marker of another disk.",
	"The marker file is damaged or not an Axis marker.",
	"The path is, or passes through, a symbolic link, or is not a directory.",
	"This disk has no marker yet, and the directory at its path is not the filesystem or directory "
		"recorded when it was added: another disk, or the disk changed."
};

static const gchar *const media_names[] = {
	"movies", "tv", "music", "audiobooks", "books", "downloads", "other", "youtube"
};
static const gchar *const media_labels[] = {
	"Movies", "TV", "Music", "Audiobooks", "Books", "Downloads", "Other", "YouTube"
};

static const gchar *const plan_names[] = { "refuse", "write", "adopt", "identify" };

G_STATIC_ASSERT(G_N_ELEMENTS(reason_names) == AXIS_VOLUME_REASON_OTHER_FILESYSTEM + 1);
G_STATIC_ASSERT(G_N_ELEMENTS(plan_names) == AXIS_VOLUME_PLAN_IDENTIFY + 1);
G_STATIC_ASSERT(G_N_ELEMENTS(reason_sentences) == G_N_ELEMENTS(reason_names));
G_STATIC_ASSERT(G_N_ELEMENTS(media_names) == AXIS_MEDIA_TYPE_COUNT);

static gboolean
name_lookup(
	const gchar        *text,
	const gchar *const *names,
	guint               n,
	guint              *index
){
	guint i;

	for (i = 0; text != NULL && i < n; i++) {
		if (strcmp(text, names[i]) == 0) {
			*index = i;
			return TRUE;
		}
	}

	return FALSE;
}

const gchar *
axis_volume_state_to_string(AxisVolumeState state)
{
	return (guint)state < G_N_ELEMENTS(state_names) ? state_names[state] : "offline";
}

gboolean
axis_volume_state_from_string(
	const gchar     *text,
	AxisVolumeState *state
){
	guint index;

	if (!name_lookup(text, state_names, G_N_ELEMENTS(state_names), &index))
		return FALSE;
	*state = (AxisVolumeState)index;
	return TRUE;
}

const gchar *
axis_volume_state_label(AxisVolumeState state)
{
	return (guint)state < G_N_ELEMENTS(state_labels) ? state_labels[state] : "Offline";
}

gboolean
axis_volume_state_is_usable(AxisVolumeState state)
{
	return state == AXIS_VOLUME_ONLINE || state == AXIS_VOLUME_READ_ONLY;
}

gboolean
axis_volume_state_is_writable(AxisVolumeState state)
{
	return state == AXIS_VOLUME_ONLINE;
}

const gchar *
axis_volume_state_unwritable_why(AxisVolumeState state)
{
	switch (state) {
	case AXIS_VOLUME_ONLINE:
		return NULL;
	case AXIS_VOLUME_READ_ONLY:
		return "its disk is mounted read-only";
	case AXIS_VOLUME_MISMATCH:
		return "its disk is not the one Axis knows";
	default:
		return "its disk is offline";
	}
}

const gchar *
axis_volume_reason_to_string(AxisVolumeReason reason)
{
	return (guint)reason < G_N_ELEMENTS(reason_names) ? reason_names[reason] : "io_error";
}

gboolean
axis_volume_reason_from_string(
	const gchar      *text,
	AxisVolumeReason *reason
){
	guint index;

	if (!name_lookup(text, reason_names, G_N_ELEMENTS(reason_names), &index))
		return FALSE;
	*reason = (AxisVolumeReason)index;
	return TRUE;
}

const gchar *
axis_volume_reason_describe(AxisVolumeReason reason)
{
	return (guint)reason < G_N_ELEMENTS(reason_sentences) ? reason_sentences[reason] :
		reason_sentences[AXIS_VOLUME_REASON_IO_ERROR];
}

const gchar *
axis_media_type_to_string(AxisMediaType type)
{
	return (guint)type < G_N_ELEMENTS(media_names) ? media_names[type] : "other";
}

gboolean
axis_media_type_from_string(
	const gchar   *text,
	AxisMediaType *type
){
	guint index;

	if (!name_lookup(text, media_names, G_N_ELEMENTS(media_names), &index))
		return FALSE;
	*type = (AxisMediaType)index;
	return TRUE;
}

const gchar *
axis_media_type_label(AxisMediaType type)
{
	return (guint)type < G_N_ELEMENTS(media_labels) ? media_labels[type] : "Other";
}

const gchar *
axis_volume_plan_to_string(AxisVolumePlan plan)
{
	return (guint)plan < G_N_ELEMENTS(plan_names) ? plan_names[plan] : "refuse";
}

gboolean
axis_volume_plan_from_string(
	const gchar    *text,
	AxisVolumePlan *plan
){
	guint index;

	if (!name_lookup(text, plan_names, G_N_ELEMENTS(plan_names), &index))
		return FALSE;
	*plan = (AxisVolumePlan)index;
	return TRUE;
}

/* ── Records ────────────────────────────────────────────────────── */

AxisVolumeInfo *
axis_volume_info_copy(const AxisVolumeInfo *info)
{
	AxisVolumeInfo *copy;

	g_return_val_if_fail(info != NULL, NULL);

	copy = g_new(AxisVolumeInfo, 1);
	*copy = *info;
	copy->uuid = g_strdup(info->uuid);
	copy->name = g_strdup(info->name);
	copy->path = g_strdup(info->path);
	copy->fs_type = g_strdup(info->fs_type);
	copy->fsid = g_strdup(info->fsid);
	copy->device = g_strdup(info->device);
	copy->marker_problem = g_strdup(info->marker_problem);
	copy->fs_uuid = g_strdup(info->fs_uuid);

	return copy;
}

void
axis_volume_info_free(AxisVolumeInfo *info)
{
	if (info == NULL)
		return;

	g_free(info->uuid);
	g_free(info->name);
	g_free(info->path);
	g_free(info->fs_type);
	g_free(info->fsid);
	g_free(info->device);
	g_free(info->marker_problem);
	g_free(info->fs_uuid);
	g_free(info);
}

gchar *
axis_volume_fs_identity_describe(const AxisVolumeFsIdentity *identity)
{
	g_autofree gchar *size = axis_size_format(identity->bytes);
	g_autofree gchar *uuid = identity->fs_uuid[0] != '\0' ?
		g_strdup_printf(" (UUID %s)", identity->fs_uuid) : g_strdup("");

	return g_strdup_printf("%s filesystem %s%s, %s, directory inode %" G_GUINT64_FORMAT,
		axis_storage_fs_type_name(identity->magic), identity->fsid, uuid, size, identity->ino);
}

void
axis_root_info_free(AxisRootInfo *info)
{
	if (info == NULL)
		return;

	g_free(info->rel_path);
	g_free(info->name);
	g_free(info);
}

static gint64
column_int64(
	AxisDbResult *result,
	guint         row,
	guint         column
){
	gint64 value = 0;

	axis_db_result_get_int64(result, row, column, &value);
	return value;
}

AxisVolumeInfo *
axis_volume_info_from_row(
	AxisDbResult *result,
	guint         row
){
	AxisVolumeInfo *info = g_new0(AxisVolumeInfo, 1);
	gboolean flag = FALSE;
	gint64 total;
	gint64 free_bytes;
	gint64 available;

	info->id = column_int64(result, row, 0);
	info->uuid = g_strdup(axis_db_result_get_text(result, row, 1));
	info->name = g_strdup(axis_db_result_get_text(result, row, 2));
	info->path = g_strdup(axis_db_result_get_text(result, row, 3));
	if (axis_db_result_get_boolean(result, row, 4, &flag))
		info->mount_root = flag;
	info->fs_type = g_strdup(axis_db_result_get_text(result, row, 5));
	info->fsid = g_strdup(axis_db_result_get_text(result, row, 6));
	info->device = g_strdup(axis_db_result_get_text(result, row, 7));
	if (!axis_volume_state_from_string(axis_db_result_get_text(result, row, 8), &info->state))
		info->state = AXIS_VOLUME_OFFLINE;
	if (!axis_volume_reason_from_string(axis_db_result_get_text(result, row, 9), &info->reason))
		info->reason = AXIS_VOLUME_REASON_IO_ERROR;
	info->state_changed_us = column_int64(result, row, 10);
	info->last_checked_us = column_int64(result, row, 11);
	info->last_online_us = column_int64(result, row, 12);
	if (axis_db_result_get_int64(result, row, 13, &total) &&
	    axis_db_result_get_int64(result, row, 14, &free_bytes) &&
	    axis_db_result_get_int64(result, row, 15, &available)) {
		info->has_space = TRUE;
		info->total_bytes = (guint64)MAX(total, 0);
		info->free_bytes = (guint64)MAX(free_bytes, 0);
		info->available_bytes = (guint64)MAX(available, 0);
	}
	info->space_checked_us = column_int64(result, row, 16);
	info->marker_created_us = column_int64(result, row, 17);
	info->created_us = column_int64(result, row, 18);
	info->n_roots = (guint)CLAMP(column_int64(result, row, 19), 0, G_MAXUINT);
	/* Anything but a volume known to have no marker yet is one with.  What
	 * one added without a marker was recognised by is kept in its row once
	 * its marker is written, as a record of what it was first recognised
	 * by, and read no more: which filesystem it is on now is what its last
	 * check found (fsid, device, fs_uuid). */
	info->has_marker = g_strcmp0(axis_db_result_get_text(result, row, 20), "filesystem") != 0;
	if (!info->has_marker) {
		const gchar *fsid = axis_db_result_get_text(result, row, 22);
		const gchar *fs_uuid = axis_db_result_get_text(result, row, 23);

		info->fs_identity.magic = (guint64)column_int64(result, row, 21);
		g_strlcpy(info->fs_identity.fsid, fsid != NULL ? fsid : "", sizeof(info->fs_identity.fsid));
		g_strlcpy(info->fs_identity.fs_uuid, fs_uuid != NULL ? fs_uuid : "",
			sizeof(info->fs_identity.fs_uuid));
		info->fs_identity.ino = (guint64)column_int64(result, row, 24);
		info->fs_identity.bytes = (guint64)MAX(column_int64(result, row, 25), 0);
		info->marker_problem = g_strdup(axis_db_result_get_text(result, row, 26));
	}
	info->fs_uuid = g_strdup(axis_db_result_get_text(result, row, 27));
	info->unusable_since_us = column_int64(result, row, 28);

	if (info->uuid == NULL)
		info->uuid = g_strdup("");
	if (info->name == NULL)
		info->name = g_strdup("");
	if (info->path == NULL)
		info->path = g_strdup("");
	return info;
}

void
axis_storage_put_volume(
	AxisStorage          *self,
	const AxisVolumeInfo *info
){
	gint64 *key = g_new(gint64, 1);

	*key = info->id;
	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->volumes, key, axis_volume_info_copy(info));
	g_mutex_unlock(&self->lock);
}

void
axis_storage_forget_volume(
	AxisStorage *self,
	gint64       id
){
	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->volumes, &id);
	g_mutex_unlock(&self->lock);
}

void
axis_storage_count_roots(
	AxisStorage *self,
	gint64       id,
	gint         delta
){
	AxisVolumeInfo *info;

	g_mutex_lock(&self->lock);
	info = g_hash_table_lookup(self->volumes, &id);
	if (info != NULL)
		info->n_roots = (guint)MAX((gint)info->n_roots + delta, 0);
	g_mutex_unlock(&self->lock);
}

static gint
compare_by_name(
	gconstpointer a,
	gconstpointer b
){
	const AxisVolumeInfo *left = *(AxisVolumeInfo *const *)a;
	const AxisVolumeInfo *right = *(AxisVolumeInfo *const *)b;
	gint order = g_utf8_collate(left->name, right->name);

	if (order != 0)
		return order;
	return left->id < right->id ? -1 : left->id > right->id;
}

GPtrArray *
axis_storage_list_volumes(AxisStorage *self)
{
	GPtrArray *list;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(self != NULL, NULL);

	list = g_ptr_array_new_with_free_func((GDestroyNotify)axis_volume_info_free);
	g_mutex_lock(&self->lock);
	g_hash_table_iter_init(&iter, self->volumes);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(list, axis_volume_info_copy(value));
	g_mutex_unlock(&self->lock);

	g_ptr_array_sort(list, compare_by_name);
	return list;
}

AxisVolumeInfo *
axis_storage_get_volume(
	AxisStorage *self,
	gint64       id
){
	AxisVolumeInfo *info;
	AxisVolumeInfo *copy = NULL;

	g_return_val_if_fail(self != NULL, NULL);

	g_mutex_lock(&self->lock);
	info = g_hash_table_lookup(self->volumes, &id);
	if (info != NULL)
		copy = axis_volume_info_copy(info);
	g_mutex_unlock(&self->lock);

	return copy;
}

void
axis_storage_force_volume_state(
	AxisStorage     *self,
	gint64           id,
	AxisVolumeState  state
){
	AxisVolumeInfo *info;

	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	info = g_hash_table_lookup(self->volumes, &id);
	if (info != NULL)
		info->state = state;
	g_mutex_unlock(&self->lock);
}

/* ── Lifetime and settings ──────────────────────────────────────── */

static const AxisSettingInfo setting_table[] = {
	{
		AXIS_STORAGE_SETTING_READ_ONLY, AXIS_SETTING_BOOLEAN,
		"Read-only storage",
		"While this is on, Axis changes nothing on any disk. The one exception is the small "
		".axis-volume marker file it writes for a disk you add and confirm: when you add it, or, for a "
		"disk you added while it was mounted read-only, the first time Axis finds it mounted "
		"read-write. Downloads still arrive at the download client, but nothing is imported into the "
		"library until this is off.",
		0, "true",
		0, 0, 0, 0, NULL
	},
	{
		AXIS_STORAGE_SETTING_LOW_SPACE, AXIS_SETTING_INTEGER,
		"Tell me when a disk has less free space than (percent)",
		"When the free space of a disk falls below this share of its size, the Needs attention page and the "
		"bell in the header say so. For example, 5 tells you about a disk that is more than 95 percent full. "
		"Enter 0 to never be told.",
		0, "5",
		0, 100, 0, 0, NULL
	},
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

const AxisSettingInfo *
axis_storage_get_setting_table(void)
{
	return setting_table;
}

gboolean
axis_storage_is_read_only_mode(AxisStorage *self)
{
	g_return_val_if_fail(self != NULL, TRUE);

	/* No store, or one that does not know the setting: read-only. */
	if (self->settings == NULL ||
	    axis_settings_lookup(self->settings, AXIS_STORAGE_SETTING_READ_ONLY, NULL) == NULL)
		return TRUE;

	return axis_settings_get_boolean(self->settings, AXIS_STORAGE_SETTING_READ_ONLY);
}

guint
axis_storage_get_low_space_percent(AxisStorage *self)
{
	gint64 percent;

	g_return_val_if_fail(self != NULL, 0);

	/* No store, or one that does not know the setting: never told. */
	if (self->settings == NULL ||
	    axis_settings_lookup(self->settings, AXIS_STORAGE_SETTING_LOW_SPACE, NULL) == NULL)
		return 0;

	percent = axis_settings_get_integer(self->settings, AXIS_STORAGE_SETTING_LOW_SPACE);
	return (guint)CLAMP(percent, 0, 100);
}

AxisDbPool *
axis_storage_get_pool(AxisStorage *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	return self->pool;
}

AxisStorage *
axis_storage_new(
	AxisDbPool   *pool,
	AxisSettings *settings
){
	AxisStorage *self;

	g_return_val_if_fail(pool != NULL, NULL);

	self = g_new0(AxisStorage, 1);
	self->ref_count = 1;
	self->pool = pool;
	self->settings = settings != NULL ? axis_settings_ref(settings) : NULL;
	self->probe = axis_storage_probe_new(AXIS_STORAGE_MAX_HELPERS);
	self->context = g_main_context_ref_thread_default();
	self->check_timeout_ms = AXIS_STORAGE_CHECK_TIMEOUT_MS;
	self->write_timeout_ms = AXIS_STORAGE_WRITE_TIMEOUT_MS;
	g_mutex_init(&self->lock);
	g_mutex_init(&self->check_lock);
	self->volumes = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
		(GDestroyNotify)axis_volume_info_free);
	self->swept = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, axis_storage_sweep_free);
	self->sweep_age_s = AXIS_VOLUME_MARKER_STALE_SECONDS;
	self->listeners = g_array_new(FALSE, FALSE, sizeof(AxisStorageListenerEntry));

	return self;
}

AxisStorage *
axis_storage_ref(AxisStorage *self)
{
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
axis_storage_unref(AxisStorage *self)
{
	if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
		return;

	axis_storage_probe_unref(self->probe);
	if (self->settings != NULL)
		axis_settings_unref(self->settings);
	g_main_context_unref(self->context);
	g_hash_table_unref(self->volumes);
	g_hash_table_unref(self->swept);
	g_array_free(self->listeners, TRUE);
	g_mutex_clear(&self->lock);
	g_mutex_clear(&self->check_lock);
	g_free(self);
}

void
axis_storage_stop(AxisStorage *self)
{
	g_return_if_fail(self != NULL);

	g_atomic_int_set(&self->stopped, 1);
}

gboolean
axis_storage_load(
	AxisStorage  *self,
	GError      **error
){
	g_autoptr(AxisDbResult) result = NULL;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	if (!axis_db_pool_exec(self->pool, "SELECT " AXIS_VOLUME_COLUMNS " FROM volume ORDER BY id",
	                       0, NULL, &result, error))
		return FALSE;

	for (i = 0; i < axis_db_result_get_n_rows(result); i++) {
		g_autoptr(AxisVolumeInfo) info = axis_volume_info_from_row(result, i);

		axis_storage_put_volume(self, info);
	}
	g_info("storage: %u volume(s) known", axis_db_result_get_n_rows(result));

	return TRUE;
}

void
axis_storage_set_timeouts(
	AxisStorage *self,
	guint        check_ms,
	guint        write_ms
){
	g_return_if_fail(self != NULL);

	if (check_ms > 0)
		g_atomic_int_set(&self->check_timeout_ms, (gint)check_ms);
	if (write_ms > 0)
		g_atomic_int_set(&self->write_timeout_ms, (gint)write_ms);
}

void
axis_storage_set_marker_sweep_age(
	AxisStorage *self,
	guint        seconds
){
	g_return_if_fail(self != NULL);

	g_atomic_int_set(&self->sweep_age_s, (gint)MIN(seconds, (guint)G_MAXINT));
}

void
axis_storage_sweep_free(gpointer data)
{
	AxisStorageSweep *sweep = data;

	if (sweep == NULL)
		return;
	if (sweep->seen != NULL)
		g_hash_table_unref(sweep->seen);
	g_free(sweep);
}

guint
axis_storage_check_timeout(AxisStorage *self)
{
	return (guint)g_atomic_int_get(&self->check_timeout_ms);
}

guint
axis_storage_write_timeout(AxisStorage *self)
{
	return (guint)g_atomic_int_get(&self->write_timeout_ms);
}

void
axis_storage_set_check_hook(
	AxisStorage          *self,
	AxisStorageCheckHook  hook,
	gpointer              user_data
){
	g_return_if_fail(self != NULL);

	g_mutex_lock(&self->lock);
	self->hook = hook;
	self->hook_data = user_data;
	g_mutex_unlock(&self->lock);
}

void
axis_storage_get_hook(
	AxisStorage          *self,
	AxisStorageCheckHook *hook,
	gpointer             *hook_data
){
	g_mutex_lock(&self->lock);
	*hook = self->hook;
	*hook_data = self->hook_data;
	g_mutex_unlock(&self->lock);
}

guint
axis_storage_get_helpers_running(AxisStorage *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return axis_storage_probe_get_running(self->probe);
}

guint
axis_storage_get_helpers_abandoned(AxisStorage *self)
{
	g_return_val_if_fail(self != NULL, 0);

	return axis_storage_probe_get_abandoned(self->probe);
}

gchar *
axis_storage_volume_key(gint64 id)
{
	return g_strdup_printf("volume:%" G_GINT64_FORMAT, id);
}

/* ── Probes ─────────────────────────────────────────────────────── */

AxisVolumeProbe *
axis_volume_probe_new(const gchar *path)
{
	AxisVolumeProbe *probe = g_new0(AxisVolumeProbe, 1);

	probe->path = g_strdup(path);
	probe->held_fd = -1;
	probe->fd = -1;
	probe->marker_status = AXIS_VOLUME_MARKER_UNREADABLE;
	return probe;
}

void
axis_volume_probe_free(gpointer data)
{
	AxisVolumeProbe *probe = data;

	if (probe == NULL)
		return;

	if (probe->held_fd >= 0)
		close(probe->held_fd);
	if (probe->fd >= 0)
		close(probe->fd);
	g_free(probe->path);
	g_free(probe);
}

/* On the helper thread: touches only @data. */
static void
probe_run(gpointer data)
{
	AxisVolumeProbe *probe = data;
	gint fresh;
	gint target;
	gint err = 0;

	fresh = axis_storage_fs_open_path(probe->path, &probe->open_errno);
	if (fresh < 0) {
		if (probe->open_errno == 0)
			probe->open_errno = EIO;
		return;
	}

	if (!axis_storage_fs_identify(fresh, &probe->identity, &err)) {
		probe->open_errno = err != 0 ? err : EIO;
		close(fresh);
		return;
	}

	/* A guard's directory is verified through its own descriptor, as long
	 * as the path still leads to it: the same directory on the same mount,
	 * as a proof compares (axis-volume-proof.c).  The same filesystem
	 * mounted at the path again (after a lazy unmount, read-only, or a bind
	 * over it) has the guard's device and inode, not its mount; the guard's
	 * descriptor is then on a mount the path no longer leads to. */
	target = fresh;
	if (probe->held_fd >= 0) {
		if (probe->identity.dev != probe->held_dev || probe->identity.ino != probe->held_ino ||
		    (probe->identity.has_mnt_id && probe->held_has_mnt_id && probe->identity.mnt_id != probe->held_mnt_id))
			probe->replaced = TRUE;
		else
			target = probe->held_fd;
	}

	if (probe->hook != NULL)
		probe->hook(probe->path, probe->hook_data);

	probe->marker_status = axis_volume_marker_read(target, &probe->marker, &probe->marker_errno);
	if (probe->marker_status == AXIS_VOLUME_MARKER_MISSING && probe->want_empty)
		probe->empty_known = axis_storage_fs_is_empty(target, &probe->empty, &probe->empty_errno);
	probe->info_ok = axis_storage_fs_info(target, &probe->info, &probe->info_errno);
	/* Read-only through the path, too: a kernel that gives no mount id
	 * cannot tell the guard's mount from a read-only one of the same
	 * directory at the path, and what the path leads to decides. */
	if (probe->info_ok && target != fresh && !probe->info.read_only) {
		AxisStorageFsInfo now;

		if (!axis_storage_fs_info(fresh, &now, &probe->info_errno))
			probe->info_ok = FALSE;
		else if (now.read_only)
			probe->info.read_only = TRUE;
	}
	/* Never asked of a FUSE filesystem, whose program would get the
	 * request. */
	if (probe->want_fs_uuid && probe->info_ok && axis_storage_fs_type_is_local(probe->info.magic))
		axis_storage_fs_read_uuid(target, probe->fs_uuid);

	if (probe->keep_fd)
		probe->fd = fresh;
	else
		close(fresh);
}

AxisStorageProbeCall *
axis_storage_run_probe(
	AxisStorage      *self,
	const gchar      *key,
	AxisVolumeProbe  *probe,
	guint             timeout_ms,
	GError          **error
){
	AxisStorageProbeCall *call;
	GError *local = NULL;
	gint64 deadline;

	call = axis_storage_probe_start(self->probe, key, probe_run, probe, axis_volume_probe_free,
		&local);
	if (call == NULL) {
		/* Stuck or full: either way the directory cannot be looked at. */
		g_set_error_literal(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
			local->message);
		g_error_free(local);
		return NULL;
	}

	deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	if (!axis_storage_probe_wait(call, deadline)) {
		axis_storage_probe_call_unref(call);
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
			"The filesystem did not answer within %u seconds; it may not be responding.",
			timeout_ms / 1000);
		return NULL;
	}

	return call;
}

gchar *
axis_volume_probe_fs_identity(
	const AxisVolumeProbe *probe,
	AxisVolumeFsIdentity  *identity
){
	const gchar *type;

	memset(identity, 0, sizeof(*identity));
	if (!probe->info_ok)
		return g_strdup_printf("The filesystem's details cannot be read: %s.", g_strerror(probe->info_errno));

	type = axis_storage_fs_type_name(probe->info.magic);
	if (!axis_storage_fs_type_is_local(probe->info.magic))
		return g_strdup_printf("This is a network, FUSE or layered filesystem (%s), such as an NFS or SMB "
			"share, mergerfs, ntfs-3g or the file sharing of Docker Desktop: what Axis can read of it does not "
			"tell one disk from another, so Axis can recognise it only by its marker.", type);
	if (probe->fs_uuid[0] == '\0' && axis_storage_fs_fsid_is_device(probe->info.fsid, probe->identity.dev))
		return g_strdup_printf("This filesystem (%s) reports no identifier of its own, only the number of the "
			"device it is on, which another disk gets when disks are found in another order. Axis can "
			"recognise it only by its marker.", type);

	identity->magic = probe->info.magic;
	g_strlcpy(identity->fsid, probe->info.fsid, sizeof(identity->fsid));
	g_strlcpy(identity->fs_uuid, probe->fs_uuid, sizeof(identity->fs_uuid));
	identity->ino = probe->identity.ino;
	identity->bytes = probe->info.total_bytes;
	return NULL;
}

/*
 * What each part is worth.  The type must be the same.  A UUID recorded
 * must be given again, the same.  The identifier (statfs f_fsid) is
 * compared unless a UUID was recorded and the identifier is the device's
 * number (xfs, f2fs): that number changes when disks are found in another
 * order, so after a reboot the volume would be offline for good; the UUID
 * decides then.  The size is evidence only where nothing stronger exists
 * (no UUID, and an identifier that is the device's number, which adding
 * refuses): a filesystem whose size moves as its pool is written (ZFS) or
 * that was grown is still the same filesystem, so where the UUID or an
 * identifier of its own matched, another size is a note in the log
 * (check_result_fill()), never another filesystem.  The directory's inode
 * must be the same.
 */
gboolean
axis_volume_fs_identity_matches(
	const AxisVolumeFsIdentity  *identity,
	const AxisVolumeProbe       *probe,
	const gchar                **what
){
	const gchar *differs = NULL;
	gboolean by_uuid = identity->fs_uuid[0] != '\0';
	gboolean device_fsid = probe->info_ok && axis_storage_fs_fsid_is_device(probe->info.fsid, probe->identity.dev);

	if (!probe->info_ok)
		differs = "the filesystem's details cannot be read";
	else if (probe->info.magic != identity->magic)
		differs = "the filesystem's type";
	else if (!(by_uuid && device_fsid) && strcmp(probe->info.fsid, identity->fsid) != 0)
		differs = "the filesystem's identifier";
	else if (by_uuid && strcmp(probe->fs_uuid, identity->fs_uuid) != 0)
		differs = probe->fs_uuid[0] == '\0' ? "the filesystem's UUID, which the kernel does not give now" :
			"the filesystem's UUID";
	else if (!by_uuid && device_fsid && probe->info.total_bytes != identity->bytes)
		differs = "the filesystem's size";
	else if (probe->identity.ino != identity->ino)
		differs = "the directory (its inode)";

	if (what != NULL)
		*what = differs;
	return differs == NULL;
}

void
axis_volume_probe_classify(
	const AxisVolumeProbe *probe,
	const AxisVolumeInfo  *record,
	gboolean               expect_mount_root,
	AxisVolumeState       *state,
	AxisVolumeReason      *reason
){
	const gchar *uuid = record->uuid;

	*state = AXIS_VOLUME_OFFLINE;

	if (probe->open_errno != 0) {
		if (probe->open_errno == ENOENT) {
			*reason = AXIS_VOLUME_REASON_PATH_MISSING;
		} else if (probe->open_errno == ELOOP || probe->open_errno == ENOTDIR) {
			*state = AXIS_VOLUME_MISMATCH;
			*reason = AXIS_VOLUME_REASON_NOT_DIRECTORY;
		} else {
			*reason = AXIS_VOLUME_REASON_IO_ERROR;
		}
		return;
	}

	/* A disk that was mounted here and is not: whatever the mount point
	 * holds now (even a copy of the marker) is not the volume. */
	if (expect_mount_root && !probe->identity.mount_root) {
		*reason = AXIS_VOLUME_REASON_NOT_MOUNTED;
		return;
	}

	switch (probe->marker_status) {
	case AXIS_VOLUME_MARKER_MISSING:
		if (!probe->empty_known) {
			*reason = AXIS_VOLUME_REASON_IO_ERROR;
		} else if (probe->empty) {
			/* Never an empty volume: an unmounted one.  Without a
			 * marker too: an empty directory is never recognised. */
			*reason = AXIS_VOLUME_REASON_EMPTY;
		} else if (record->has_marker) {
			*state = AXIS_VOLUME_MISMATCH;
			*reason = AXIS_VOLUME_REASON_NO_MARKER;
		} else if (!probe->info_ok) {
			*reason = AXIS_VOLUME_REASON_IO_ERROR;
		} else if (!axis_volume_fs_identity_matches(&record->fs_identity, probe, NULL)) {
			/* Not guessed: offline until the recorded filesystem is
			 * back, and its marker is never written here. */
			*reason = AXIS_VOLUME_REASON_OTHER_FILESYSTEM;
		} else {
			*state = probe->info.read_only ? AXIS_VOLUME_READ_ONLY : AXIS_VOLUME_ONLINE;
			*reason = AXIS_VOLUME_REASON_NONE;
		}
		return;
	case AXIS_VOLUME_MARKER_UNREADABLE:
		*reason = AXIS_VOLUME_REASON_IO_ERROR;
		return;
	case AXIS_VOLUME_MARKER_INVALID:
		*state = AXIS_VOLUME_MISMATCH;
		*reason = AXIS_VOLUME_REASON_BAD_MARKER;
		return;
	case AXIS_VOLUME_MARKER_FOUND:
	default:
		break;
	}

	if (uuid == NULL || strcmp(probe->marker.uuid, uuid) != 0) {
		*state = AXIS_VOLUME_MISMATCH;
		*reason = AXIS_VOLUME_REASON_OTHER_MARKER;
		return;
	}

	if (!probe->info_ok) {
		*reason = AXIS_VOLUME_REASON_IO_ERROR;
		return;
	}

	*state = probe->info.read_only ? AXIS_VOLUME_READ_ONLY : AXIS_VOLUME_ONLINE;
	*reason = AXIS_VOLUME_REASON_NONE;
}

const gchar *
axis_volume_probe_detail(const AxisVolumeProbe *probe)
{
	gint err = probe->open_errno;

	if (err == 0)
		err = probe->marker_errno;
	if (err == 0 && probe->marker_status == AXIS_VOLUME_MARKER_MISSING && !probe->empty_known)
		err = probe->empty_errno;
	if (err == 0 && !probe->info_ok)
		err = probe->info_errno;

	return err != 0 ? g_strerror(err) : "";
}

/* ── Listeners ──────────────────────────────────────────────────── */

typedef struct
{
	AxisStorage    *storage;
	AxisVolumeInfo *info;
	gboolean        state_changed;
} Event;

static void
event_free(gpointer data)
{
	Event *event = data;

	axis_volume_info_free(event->info);
	axis_storage_unref(event->storage);
	g_free(event);
}

static gboolean
deliver(gpointer data)
{
	Event *event = data;
	AxisStorage *self = event->storage;
	GArray *copy;
	guint i;

	if (g_atomic_int_get(&self->stopped))
		return G_SOURCE_REMOVE;

	/* A listener may remove itself (or another) while being called. */
	copy = g_array_sized_new(FALSE, FALSE, sizeof(AxisStorageListenerEntry), self->listeners->len);
	g_array_append_vals(copy, self->listeners->data, self->listeners->len);
	for (i = 0; i < copy->len; i++) {
		const AxisStorageListenerEntry *entry = &g_array_index(copy, AxisStorageListenerEntry, i);

		entry->func(self, event->info, event->state_changed, entry->user_data);
	}
	g_array_free(copy, TRUE);

	return G_SOURCE_REMOVE;
}

void
axis_storage_notify(
	AxisStorage          *self,
	const AxisVolumeInfo *info,
	gboolean              state_changed
){
	Event *event;

	if (g_atomic_int_get(&self->stopped))
		return;

	event = g_new0(Event, 1);
	event->storage = axis_storage_ref(self);
	event->info = info != NULL ? axis_volume_info_copy(info) : NULL;
	event->state_changed = state_changed;
	g_main_context_invoke_full(self->context, G_PRIORITY_DEFAULT, deliver, event, event_free);
}

guint
axis_storage_add_listener(
	AxisStorage         *self,
	AxisStorageListener  func,
	gpointer             user_data
){
	AxisStorageListenerEntry entry;

	g_return_val_if_fail(self != NULL, 0);
	g_return_val_if_fail(func != NULL, 0);

	entry.id = ++self->next_listener;
	entry.func = func;
	entry.user_data = user_data;
	g_array_append_val(self->listeners, entry);

	return entry.id;
}

void
axis_storage_remove_listener(
	AxisStorage *self,
	guint        listener_id
){
	guint i;

	g_return_if_fail(self != NULL);

	for (i = 0; i < self->listeners->len; i++) {
		if (g_array_index(self->listeners, AxisStorageListenerEntry, i).id == listener_id) {
			g_array_remove_index(self->listeners, i);
			return;
		}
	}
}

/* ── Checks ─────────────────────────────────────────────────────── */

/* What one check found. */
typedef struct
{
	AxisVolumeState    state;
	AxisVolumeReason   reason;
	gchar             *detail;	/* for the log */
	gboolean           mount_root;
	gchar             *fs_type;
	gchar             *fsid;
	gchar             *device;
	gchar             *fs_uuid;		/* "" when the kernel gave none */
	guint64            total_bytes;
	guint64            free_bytes;
	guint64            available_bytes;
} CheckResult;

static void
check_result_clear(CheckResult *result)
{
	g_free(result->detail);
	g_free(result->fs_type);
	g_free(result->fsid);
	g_free(result->device);
	g_free(result->fs_uuid);
	memset(result, 0, sizeof(*result));
}

/* The result of a probe that returned (@probe), or of one that did not
 * (@probe %NULL, @why the reason). */
static void
check_result_fill(
	CheckResult           *result,
	const AxisVolumeInfo  *record,
	const AxisVolumeProbe *probe,
	const GError          *why
){
	memset(result, 0, sizeof(*result));

	if (probe == NULL) {
		result->state = AXIS_VOLUME_OFFLINE;
		result->reason = AXIS_VOLUME_REASON_TIMEOUT;
		result->detail = g_strdup(why != NULL ? why->message : "");
		return;
	}

	axis_volume_probe_classify(probe, record, record->mount_root, &result->state, &result->reason);
	if (result->reason == AXIS_VOLUME_REASON_OTHER_FILESYSTEM) {
		const gchar *what = NULL;

		axis_volume_fs_identity_matches(&record->fs_identity, probe, &what);
		result->detail = g_strdup_printf("%s differs", what != NULL ? what : "the filesystem");
	} else {
		result->detail = g_strdup(axis_volume_probe_detail(probe));
	}
	/* Recognised by its filesystem with another size than when it was
	 * added (a pool written, a filesystem grown): still the volume, said
	 * once when it first differs. */
	if (axis_volume_state_is_usable(result->state) && !record->has_marker &&
	    probe->info.total_bytes != record->fs_identity.bytes &&
	    (!record->has_space || record->total_bytes == record->fs_identity.bytes)) {
		g_autofree gchar *was = axis_size_format(record->fs_identity.bytes);
		g_autofree gchar *now = axis_size_format(probe->info.total_bytes);

		g_message("volume '%s' (id %" G_GINT64_FORMAT "): its filesystem reports a size of %s now, not %s as when it "
			"was added; it is recognised by its %s, which is the same, so it is the volume still", record->name,
			record->id, now, was, record->fs_identity.fs_uuid[0] != '\0' ? "UUID" : "identifier");
	}
	if (axis_volume_state_is_usable(result->state)) {
		result->mount_root = probe->identity.mount_root;
		result->fs_type = g_strdup(axis_storage_fs_type_name(probe->info.magic));
		result->fsid = g_strdup(probe->info.fsid);
		result->device = axis_storage_fs_device_text(probe->identity.dev);
		result->fs_uuid = g_strdup(probe->fs_uuid);
		result->total_bytes = probe->info.total_bytes;
		result->free_bytes = probe->info.free_bytes;
		result->available_bytes = probe->info.available_bytes;
	}
}

/* The record as the daemon sees it after @result, without the database. */
static AxisVolumeInfo *
apply_locally(
	const AxisVolumeInfo *record,
	const CheckResult    *result
){
	AxisVolumeInfo *info = axis_volume_info_copy(record);
	gint64 now = g_get_real_time();

	if (info->state != result->state || info->reason != result->reason)
		info->state_changed_us = now;
	info->state = result->state;
	info->reason = result->reason;
	info->last_checked_us = now;
	/* Since when it has been away: kept while it stays away, whatever the
	 * reason becomes. */
	if (axis_volume_state_is_usable(result->state))
		info->unusable_since_us = 0;
	else if (info->unusable_since_us <= 0)
		info->unusable_since_us = now;
	if (axis_volume_state_is_usable(result->state)) {
		info->last_online_us = now;
		info->has_space = TRUE;
		info->total_bytes = result->total_bytes;
		info->free_bytes = result->free_bytes;
		info->available_bytes = result->available_bytes;
		info->space_checked_us = now;
		info->mount_root = info->mount_root || result->mount_root;
		g_free(info->fs_type);
		info->fs_type = g_strdup(result->fs_type);
		g_free(info->fsid);
		info->fsid = g_strdup(result->fsid);
		g_free(info->device);
		info->device = g_strdup(result->device);
		/* Evidence of the filesystem it is on now, not where it once was. */
		g_free(info->fs_uuid);
		info->fs_uuid = result->fs_uuid != NULL && result->fs_uuid[0] != '\0' ? g_strdup(result->fs_uuid) : NULL;
	}

	return info;
}

static gboolean
store_result(
	AxisStorage           *self,
	const AxisVolumeInfo  *record,
	const AxisVolumeInfo  *local,
	AxisVolumeInfo       **stored,
	GError               **error
){
	g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, record->id);
	g_autofree gchar *total = g_strdup_printf("%" G_GUINT64_FORMAT, local->total_bytes);
	g_autofree gchar *free_bytes = g_strdup_printf("%" G_GUINT64_FORMAT, local->free_bytes);
	g_autofree gchar *available = g_strdup_printf("%" G_GUINT64_FORMAT, local->available_bytes);
	gboolean usable = axis_volume_state_is_usable(local->state);
	g_autoptr(AxisDbResult) result = NULL;
	const gchar *params[13];

	params[0] = id;
	params[1] = record->path;
	params[2] = axis_volume_state_to_string(local->state);
	params[3] = axis_volume_reason_to_string(local->reason);
	params[4] = usable ? "true" : "false";
	params[5] = usable ? total : NULL;
	params[6] = usable ? free_bytes : NULL;
	params[7] = usable ? available : NULL;
	params[8] = local->mount_root ? "true" : "false";
	params[9] = local->fs_type;
	params[10] = local->fsid;
	params[11] = local->device;
	params[12] = local->fs_uuid != NULL ? local->fs_uuid : "";

	*stored = NULL;
	/* The row is only updated while it still describes the volume that
	 * was checked: one removed or re-pointed meanwhile is left alone. */
	if (!axis_db_pool_exec(self->pool,
	                       "UPDATE volume SET"
	                       " state = $3,"
	                       " state_reason = $4,"
	                       " state_changed_at = CASE WHEN state <> $3 OR state_reason <> $4"
	                       "   THEN now() ELSE state_changed_at END,"
	                       " unusable_since = CASE WHEN $5::boolean THEN NULL"
	                       "   ELSE coalesce(unusable_since, now()) END,"
	                       " last_checked_at = now(),"
	                       " last_online_at = CASE WHEN $5::boolean THEN now() ELSE last_online_at END,"
	                       " total_bytes = CASE WHEN $5::boolean THEN $6::bigint ELSE total_bytes END,"
	                       " free_bytes = CASE WHEN $5::boolean THEN $7::bigint ELSE free_bytes END,"
	                       " available_bytes = CASE WHEN $5::boolean THEN $8::bigint"
	                       "   ELSE available_bytes END,"
	                       " space_checked_at = CASE WHEN $5::boolean THEN now()"
	                       "   ELSE space_checked_at END,"
	                       " mount_root = CASE WHEN $5::boolean THEN mount_root OR $9::boolean"
	                       "   ELSE mount_root END,"
	                       " fs_type = CASE WHEN $5::boolean THEN coalesce($10, fs_type) ELSE fs_type END,"
	                       " fsid = CASE WHEN $5::boolean THEN coalesce($11, fsid) ELSE fsid END,"
	                       " device = CASE WHEN $5::boolean THEN coalesce($12, device) ELSE device END,"
	                       " fs_uuid = CASE WHEN $5::boolean THEN nullif($13, '') ELSE fs_uuid END"
	                       " WHERE id = $1::bigint AND path = $2"
	                       " RETURNING " AXIS_VOLUME_COLUMNS,
	                       G_N_ELEMENTS(params), params, &result, error))
		return FALSE;

	if (axis_db_result_get_n_rows(result) == 1)
		*stored = axis_volume_info_from_row(result, 0);
	return TRUE;
}

/* Logs a change of state (when @state_changed) and a change of the
 * filesystem a matching marker was found on (when @before is given). */
static void
log_change(
	const AxisVolumeInfo *before,
	const AxisVolumeInfo *after,
	const gchar          *detail,
	gboolean              state_changed
){
	const gchar *sentence = axis_volume_reason_describe(after->reason);
	const gchar *sep = detail != NULL && detail[0] != '\0' ? " (" : "";
	const gchar *end = detail != NULL && detail[0] != '\0' ? ")" : "";

	if (detail == NULL)
		detail = "";

	if (!state_changed)
		goto identity;

	switch (after->state) {
	case AXIS_VOLUME_ONLINE:
		g_message("volume '%s' (id %" G_GINT64_FORMAT ") is online at %s", after->name, after->id,
			after->path);
		break;
	case AXIS_VOLUME_READ_ONLY:
		g_message("volume '%s' (id %" G_GINT64_FORMAT ") is online at %s, mounted read-only",
			after->name, after->id, after->path);
		break;
	case AXIS_VOLUME_OFFLINE:
		g_warning("volume '%s' (id %" G_GINT64_FORMAT ") is offline at %s: %s%s%s%s Nothing "
			"recorded about it changes until it is back.", after->name, after->id, after->path,
			sentence, sep, detail, end);
		break;
	case AXIS_VOLUME_MISMATCH:
	default:
		g_warning("volume '%s' (id %" G_GINT64_FORMAT ") does not match what is at %s: %s%s%s%s It "
			"is not used until an administrator looks at it.", after->name, after->id,
			after->path, sentence, sep, detail, end);
		break;
	}

identity:
	if (before != NULL)
		g_message("volume '%s' (id %" G_GINT64_FORMAT "): its marker matches, on a different "
			"device or filesystem id than before (%s, %s -> %s, %s)", after->name, after->id,
			before->device, before->fsid, after->device, after->fsid);
}

/* Stores and publishes what a check of @record found. */
static gboolean
record_result(
	AxisStorage           *self,
	const AxisVolumeInfo  *record,
	const CheckResult     *result,
	AxisVolumeInfo       **after,
	GError               **error
){
	g_autoptr(AxisVolumeInfo) local = apply_locally(record, result);
	g_autoptr(AxisVolumeInfo) stored = NULL;
	g_autoptr(AxisVolumeInfo) previous = NULL;
	AxisVolumeInfo *before;
	AxisVolumeInfo *now;
	gboolean changed = FALSE;
	gboolean identity_changed = FALSE;
	gboolean ok;
	gboolean present;
	GError *local_error = NULL;

	/* One result at a time, so the stored and the remembered state are
	 * those of the same, latest check. */
	g_mutex_lock(&self->check_lock);

	ok = store_result(self, record, local, &stored, &local_error);
	now = stored != NULL ? stored : local;

	g_mutex_lock(&self->lock);
	before = g_hash_table_lookup(self->volumes, &record->id);
	/* Removed or re-pointed while the check ran: its result is about a
	 * volume that is not there any more. */
	present = before != NULL && strcmp(before->path, record->path) == 0 &&
		(ok ? stored != NULL : TRUE);
	if (present) {
		gint64 *key = g_new(gint64, 1);

		changed = before->state != now->state || before->reason != now->reason;
		identity_changed = axis_volume_state_is_usable(now->state) &&
			axis_volume_state_is_usable(before->state) &&
			(g_strcmp0(before->device, now->device) != 0 || g_strcmp0(before->fsid, now->fsid) != 0);
		previous = axis_volume_info_copy(before);
		/* The root count is the daemon's own when nothing was read. */
		if (stored == NULL)
			now->n_roots = before->n_roots;

		*key = record->id;
		g_hash_table_replace(self->volumes, key, axis_volume_info_copy(now));
	}
	g_mutex_unlock(&self->lock);
	g_mutex_unlock(&self->check_lock);

	if (present) {
		if (changed || identity_changed)
			log_change(identity_changed ? previous : NULL, now, result->detail, changed);
		axis_storage_notify(self, now, changed);
	}

	if (after != NULL)
		*after = present ? axis_volume_info_copy(now) : axis_storage_get_volume(self, record->id);

	if (!ok) {
		g_propagate_prefixed_error(error, local_error, "cannot store the check of volume '%s': ",
			record->name);
		return FALSE;
	}
	return TRUE;
}

/*
 * Starts a check of @record.  When it cannot start, *@why says why and
 * *@busy whether that says nothing about the volume (the helper is full of
 * other work, or no thread could be made), as opposed to the volume's own
 * filesystem being stuck.
 */
static AxisStorageProbeCall *
start_check(
	AxisStorage           *self,
	const AxisVolumeInfo  *record,
	gboolean              *busy,
	GError               **why
){
	g_autofree gchar *key = axis_storage_volume_key(record->id);
	AxisVolumeProbe *probe = axis_volume_probe_new(record->path);
	AxisStorageProbeCall *call;
	GError *local = NULL;

	probe->want_empty = TRUE;
	/* The filesystem's UUID, for every volume: for one without a marker it
	 * is part of what it is recognised by; for any, it is recorded with what
	 * the check found (fs_uuid), as evidence of which filesystem the volume
	 * is on (axis_volume_probe_same_filesystem()).  A volume without a
	 * marker gets its marker through the directory the check looked at. */
	probe->want_fs_uuid = TRUE;
	probe->keep_fd = !record->has_marker;
	axis_storage_get_hook(self, &probe->hook, &probe->hook_data);

	*busy = FALSE;
	call = axis_storage_probe_start(self->probe, key, probe_run, probe, axis_volume_probe_free,
		&local);
	if (call == NULL) {
		*busy = !g_error_matches(local, AXIS_STORAGE_PROBE_ERROR, AXIS_STORAGE_PROBE_ERROR_STUCK);
		g_propagate_error(why, local);
	}

	return call;
}

static GError *
missed_deadline(AxisStorage *self)
{
	return g_error_new(AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE,
		"The filesystem did not answer within %u seconds; it may not be responding.",
		axis_storage_check_timeout(self) / 1000);
}

/* Records what @call found about @record (or, without a call, that it
 * missed its deadline or is stuck, @why).  Consumes @call. */
static gboolean
finish_check(
	AxisStorage           *self,
	const AxisVolumeInfo  *record,
	AxisStorageProbeCall  *call,
	const GError          *why,
	AxisVolumeInfo       **after,
	GError               **error
){
	const AxisVolumeProbe *probe = call != NULL ? axis_storage_probe_call_get_data(call) : NULL;
	g_autoptr(AxisVolumeInfo) marked = NULL;
	CheckResult result;
	gboolean ok;

	check_result_fill(&result, record, probe, why);
	/* A volume without a marker, recognised: its own marker found there
	 * (an earlier attempt), or written now that it is mounted read-write. */
	if (probe != NULL && !record->has_marker && axis_volume_state_is_usable(result.state) &&
	    (probe->marker_status == AXIS_VOLUME_MARKER_FOUND || result.state == AXIS_VOLUME_ONLINE))
		axis_storage_write_late_marker(self, record, probe, &marked, NULL);
	axis_storage_probe_call_unref(call);
	ok = record_result(self, marked != NULL ? marked : record, &result, after, error);
	check_result_clear(&result);

	return ok;
}

gboolean
axis_storage_check_record(
	AxisStorage           *self,
	const AxisVolumeInfo  *record,
	AxisVolumeInfo       **after,
	GError               **error
){
	AxisStorageProbeCall *call;
	GError *why = NULL;
	gboolean busy;
	gboolean ok;

	call = start_check(self, record, &busy, &why);
	if (call == NULL && busy) {
		/* Nothing was learnt about this volume: its state stands. */
		g_debug("storage: volume %" G_GINT64_FORMAT " not checked now: %s", record->id, why->message);
		g_error_free(why);
		if (after != NULL)
			*after = axis_storage_get_volume(self, record->id);
		return TRUE;
	}
	if (call != NULL && !axis_storage_probe_wait(call, g_get_monotonic_time() +
	                                             (gint64)axis_storage_check_timeout(self) * 1000)) {
		axis_storage_probe_call_unref(call);
		call = NULL;
		why = missed_deadline(self);
	}

	ok = finish_check(self, record, call, why, after, error);
	g_clear_error(&why);
	return ok;
}

gboolean
axis_storage_check_volume(
	AxisStorage     *self,
	gint64           id,
	AxisVolumeInfo **info,
	GError         **error
){
	g_autoptr(AxisVolumeInfo) record = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (info != NULL)
		*info = NULL;
	record = axis_storage_get_volume(self, id);
	if (record == NULL) {
		g_set_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND,
			"there is no volume %" G_GINT64_FORMAT, id);
		return FALSE;
	}

	return axis_storage_check_record(self, record, info, error);
}

gboolean
axis_storage_check_all(
	AxisStorage   *self,
	GCancellable  *cancellable,
	GError       **error
){
	g_autoptr(GPtrArray) records = NULL;
	AxisStorageProbeCall *calls[AXIS_STORAGE_CHECK_WAVE];
	GError *why[AXIS_STORAGE_CHECK_WAVE];
	gboolean busy[AXIS_STORAGE_CHECK_WAVE];
	GError *first = NULL;
	guint base;
	guint i;

	g_return_val_if_fail(self != NULL, FALSE);

	records = axis_storage_list_volumes(self);

	/* In waves of volumes checked at once, so that one hung disk costs one
	 * deadline rather than one per volume, while the helper keeps room
	 * for the guards that other work takes meanwhile. */
	for (base = 0; base < records->len; base += AXIS_STORAGE_CHECK_WAVE) {
		guint n = MIN(AXIS_STORAGE_CHECK_WAVE, records->len - base);
		gint64 deadline;

		for (i = 0; i < n; i++) {
			why[i] = NULL;
			calls[i] = start_check(self, g_ptr_array_index(records, base + i), &busy[i], &why[i]);
		}

		deadline = g_get_monotonic_time() + (gint64)axis_storage_check_timeout(self) * 1000;
		for (i = 0; i < n; i++) {
			if (calls[i] != NULL && !axis_storage_probe_wait(calls[i], deadline)) {
				axis_storage_probe_call_unref(calls[i]);
				calls[i] = NULL;
				why[i] = missed_deadline(self);
			}
		}

		if (g_cancellable_is_cancelled(cancellable)) {
			for (i = 0; i < n; i++) {
				axis_storage_probe_call_unref(calls[i]);
				g_clear_error(&why[i]);
			}
			g_clear_error(&first);
			g_cancellable_set_error_if_cancelled(cancellable, error);
			return FALSE;
		}

		for (i = 0; i < n; i++) {
			const AxisVolumeInfo *record = g_ptr_array_index(records, base + i);
			GError *local = NULL;

			if (calls[i] == NULL && busy[i]) {
				/* Nothing learnt: the state stands until the next check. */
				g_debug("storage: volume %" G_GINT64_FORMAT " not checked now: %s", record->id,
					why[i]->message);
			} else if (!finish_check(self, record, calls[i], why[i], NULL, &local)) {
				if (first == NULL)
					first = local;
				else
					g_error_free(local);
			}
			g_clear_error(&why[i]);
		}
	}

	if (first != NULL) {
		g_propagate_error(error, first);
		return FALSE;
	}
	return TRUE;
}
