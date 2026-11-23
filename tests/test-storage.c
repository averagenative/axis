/*
 * test-storage.c - volumes, presence, the guard and roots, against a real
 * database and real directories
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A "disk" is a directory under the test's scratch directory, at
 * media/<name>.  Unmounting it is simulated the way it looks to the
 * daemon: the directory is moved away (to stash/) and an empty one is left
 * at its path; mounting it again moves it back.  A different disk at the
 * same path is another directory moved into place.  Real mounts, in a user
 * namespace, are in test-storage-mounts.c.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "core/axis-settings.h"
#include "storage/axis-storage.h"
#include "storage/axis-storage-fs.h"
#include "storage/axis-volume-guard.h"
#include "storage/axis-volume-marker.h"

#define OTHER_UUID "11111111-2222-4333-8444-555555555555"

typedef struct
{
	AxisTestDb   *db;
	AxisDbPool   *pool;
	AxisSettings *settings;
	AxisStorage  *storage;
	gchar        *dir;
	gchar        *media;
	gchar        *stash;
	AxisTestGate *gate;
	gchar        *hang_path;	/* the path the hook holds up */
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");

	fx->dir = axis_test_storage_dir_new();
	fx->media = g_build_filename(fx->dir, "media", NULL);
	fx->stash = g_build_filename(fx->dir, "stash", NULL);
	axis_test_storage_mkdir(fx->media);
	axis_test_storage_mkdir(fx->stash);
	fx->gate = axis_test_gate_new();

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 6);

	fx->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_STORAGE_MODULE_ID,
		axis_storage_get_setting_table(), &error));
	g_assert_true(axis_settings_load(fx->settings, NULL, &error));
	g_assert_no_error(error);

	fx->storage = axis_storage_new(fx->pool, fx->settings);
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	axis_storage_set_timeouts(fx->storage, 30000, 30000);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	gint64 deadline;

	if (fx->db == NULL) {
		axis_test_storage_dir_free(fx->dir);
		g_free(fx->media);
		g_free(fx->stash);
		axis_test_gate_free(fx->gate);
		axis_test_log_reset();
		return;
	}

	/* Whatever a test left hanging is let go, and every helper ends. */
	axis_test_gate_open(fx->gate);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (axis_storage_get_helpers_running(fx->storage) > 0 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_cmpuint(axis_storage_get_helpers_running(fx->storage), ==, 0);
	while (g_main_context_iteration(NULL, FALSE))
		;

	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->media);
	g_free(fx->stash);
	g_free(fx->hang_path);
	axis_test_gate_free(fx->gate);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* A disk with something on it, at media/<name>. */
static gchar *
make_disk(
	Fixture     *fx,
	const gchar *name
){
	gchar *path = g_build_filename(fx->media, name, NULL);
	g_autofree gchar *file = g_build_filename(path, "readme.txt", NULL);

	axis_test_storage_mkdir(path);
	axis_test_storage_write(file, "this disk holds media\n");
	return path;
}

static AxisVolumeInspection *
inspect(
	Fixture     *fx,
	const gchar *path
){
	AxisVolumeInspection *inspection = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_storage_inspect_volume(fx->storage, path, &inspection, &error));
	g_assert_no_error(error);
	g_assert_nonnull(inspection);
	return inspection;
}

static void
request_from(
	AxisVolumeAddRequest       *request,
	const AxisVolumeInspection *inspection,
	const gchar                *name
){
	memset(request, 0, sizeof(*request));
	request->path = inspection->path;
	request->name = name;
	request->plan = inspection->plan;
	request->marker_uuid = inspection->marker_uuid;
	request->dev = inspection->dev;
	request->ino = inspection->ino;
	request->fsid = inspection->fsid;
	request->total_bytes = inspection->total_bytes;
	request->not_mount_point_ok = TRUE;
}

/* Inspects and adds @path as the administrator would; the new id. */
static gint64
add_volume(
	Fixture     *fx,
	const gchar *path,
	const gchar *name
){
	g_autoptr(AxisVolumeInspection) inspection = inspect(fx, path);
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	if (inspection->plan == AXIS_VOLUME_PLAN_REFUSE)
		g_error("cannot add %s: %s", path, inspection->refusal);
	request_from(&request, inspection, name);
	g_assert_true(axis_storage_add_volume(fx->storage, &request, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* Adds @path as a further volume, as axis_test_library_add_volume_beside()
 * does: these disks are directories of one filesystem, which Axis refuses
 * as two volumes, so the volumes added before are first made to say nothing
 * of their filesystem (as disks last seen elsewhere). */
static gint64
add_volume_beside(
	Fixture     *fx,
	const gchar *path,
	const gchar *name
){
	axis_test_db_exec(fx->db, "UPDATE volume SET fsid = '', device = ''");
	return add_volume(fx, path, name);
}

static gchar *
stash_path(
	Fixture     *fx,
	const gchar *as
){
	return g_build_filename(fx->stash, as, NULL);
}

/* The disk at @path goes away, leaving an empty mount point. */
static void
unmount_disk(
	Fixture     *fx,
	const gchar *path,
	const gchar *as
){
	g_autofree gchar *away = stash_path(fx, as);

	g_assert_cmpint(g_rename(path, away), ==, 0);
	axis_test_storage_mkdir(path);
}

/* ...and comes back. */
static void
mount_disk(
	Fixture     *fx,
	const gchar *path,
	const gchar *as
){
	g_autofree gchar *away = stash_path(fx, as);

	g_assert_cmpint(g_rmdir(path), ==, 0);
	g_assert_cmpint(g_rename(away, path), ==, 0);
}

static AxisVolumeInfo *
check(
	Fixture *fx,
	gint64   id
){
	AxisVolumeInfo *info = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_storage_check_volume(fx->storage, id, &info, &error));
	g_assert_no_error(error);
	g_assert_nonnull(info);
	return info;
}

static void
assert_state(
	Fixture          *fx,
	gint64            id,
	AxisVolumeState   state,
	AxisVolumeReason  reason
){
	g_autoptr(AxisVolumeInfo) info = check(fx, id);
	g_autofree gchar *stored = axis_test_db_queryf(fx->db,
		"SELECT state || '/' || state_reason FROM volume WHERE id = %" G_GINT64_FORMAT, id);
	g_autofree gchar *expected = g_strdup_printf("%s/%s", axis_volume_state_to_string(state),
		axis_volume_reason_to_string(reason));

	g_assert_cmpstr(axis_volume_state_to_string(info->state), ==, axis_volume_state_to_string(state));
	g_assert_cmpstr(axis_volume_reason_to_string(info->reason), ==, axis_volume_reason_to_string(reason));
	g_assert_cmpstr(stored, ==, expected);
}

static void
set_read_only(
	Fixture  *fx,
	gboolean  on
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->settings, AXIS_STORAGE_SETTING_READ_ONLY, on ? "true" : "false",
		&error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
}

static gchar *
marker_path(const gchar *dir)
{
	return g_build_filename(dir, AXIS_VOLUME_MARKER_NAME, NULL);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack == NULL || strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in: %s", needle, haystack != NULL ? haystack : "(null)");
}

/* Whether the filesystem of @path reports an identifier of its own (ext4,
 * btrfs, tmpfs since Linux 6.6), not only its device's number (xfs): what
 * proves the scratch directory's disks one filesystem, whatever their
 * state. */
static gboolean
fsid_is_own(const gchar *path)
{
	g_autofree gchar *fsid = NULL;
	struct statfs fs;
	struct stat st;

	g_assert_cmpint(statfs(path, &fs), ==, 0);
	g_assert_cmpint(stat(path, &st), ==, 0);
	fsid = g_strdup_printf("%08x%08x", (guint)fs.f_fsid.__val[0], (guint)fs.f_fsid.__val[1]);
	return !axis_storage_fs_fsid_is_device(fsid, (guint64)st.st_dev);
}

/* Every hang is for one path only, so other volumes are not held up. */
static void
hang_hook(
	const gchar *path,
	gpointer     user_data
){
	Fixture *fx = user_data;

	if (fx->hang_path != NULL && g_str_has_prefix(path, fx->hang_path))
		axis_test_gate_pass(fx->gate);
}

/* ── Identity ───────────────────────────────────────────────────── */

/* Spec: Adding a volume writes its marker */
static void
test_add_writes_marker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *marker_file = NULL;
	g_autofree gchar *content = NULL;
	g_autofree gchar *stored_uuid = NULL;
	g_autofree gchar *stored_space = NULL;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	AxisVolumeMarker marker;
	gint64 id = 0;
	guint i;
	gboolean warned = FALSE;

	SKIP_WITHOUT_DB(fx);
	disk = make_disk(fx, "disk1");
	marker_file = marker_path(disk);

	/* Read-only mode is the default; writing the marker is its one
	 * exception. */
	g_assert_true(axis_storage_is_read_only_mode(fx->storage));

	/* Looking writes nothing. */
	before = axis_test_storage_digest(disk);
	inspection = inspect(fx, disk);
	after = axis_test_storage_digest(disk);
	g_assert_cmpstr(before, ==, after);
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_WRITE_MARKER);
	g_assert_null(inspection->marker_uuid);
	g_assert_false(inspection->mount_root);
	g_assert_true(inspection->has_space);
	g_assert_cmpuint(inspection->ino, >, 0);
	g_assert_cmpstr(inspection->suggested_name, ==, "disk1");
	for (i = 0; i < inspection->warnings->len; i++)
		warned = warned || strstr(g_ptr_array_index(inspection->warnings, i), "not a mount point") != NULL;
	g_assert_true(warned);

	/* A directory that is not a mount point needs that confirmed too. */
	request_from(&request, inspection, "Disk one");
	request.not_mount_point_ok = FALSE;
	g_assert_false(axis_storage_add_volume(fx->storage, &request, &id, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(g_file_test(marker_file, G_FILE_TEST_EXISTS));

	request.not_mount_point_ok = TRUE;
	g_assert_true(axis_storage_add_volume(fx->storage, &request, &id, &error));
	g_assert_no_error(error);

	/* The marker is there, with the identifier the database has. */
	content = axis_test_storage_read(marker_file);
	g_assert_nonnull(content);
	g_assert_true(axis_volume_marker_parse((const guint8 *)content, strlen(content), &marker, NULL));
	stored_uuid = axis_test_db_queryf(fx->db, "SELECT uuid FROM volume WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(stored_uuid, ==, marker.uuid);
	g_assert_cmpuint(axis_test_storage_count_temporaries(disk), ==, 0);

	/* Recorded online, with its space. */
	info = axis_storage_get_volume(fx->storage, id);
	g_assert_cmpstr(info->uuid, ==, marker.uuid);
	g_assert_cmpstr(info->name, ==, "Disk one");
	g_assert_cmpstr(info->path, ==, disk);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_true(info->has_space);
	stored_space = axis_test_db_queryf(fx->db, "SELECT (total_bytes > 0 AND free_bytes >= 0)::text"
		" FROM volume WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(stored_space, ==, "true");
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*added at*wrote the new marker*"), ==, 1);
}

/* Spec: Existing marker is adopted */
static void
test_adopt_existing_marker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk1");
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_write_marker(disk, OTHER_UUID);
	before = axis_test_storage_digest(disk);

	inspection = inspect(fx, disk);
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_ADOPT);
	g_assert_cmpstr(inspection->marker_uuid, ==, OTHER_UUID);

	id = add_volume(fx, disk, "Adopted");
	info = axis_storage_get_volume(fx->storage, id);
	g_assert_cmpstr(info->uuid, ==, OTHER_UUID);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);

	/* Not a byte written: not even the directory changed. */
	after = axis_test_storage_digest(disk);
	g_assert_cmpstr(before, ==, after);
}

static void
assert_refusal(
	Fixture     *fx,
	const gchar *path,
	const gchar *words
){
	g_autoptr(AxisVolumeInspection) inspection = inspect(fx, path);

	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_REFUSE);
	assert_contains(inspection->refusal, words);
}

/* Spec: Same filesystem twice */
static void
test_same_filesystem_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *a = make_disk(fx, "a");
	g_autofree gchar *b = make_disk(fx, "b");
	g_autofree gchar *c = make_disk(fx, "c");
	g_autofree gchar *nested = g_build_filename(a, "sub", NULL);
	g_autofree gchar *a_marker = NULL;
	g_autofree gchar *a_marker_content = NULL;
	g_autofree gchar *c_marker = NULL;
	g_autoptr(AxisVolumeInspection) for_b = NULL;
	g_autoptr(AxisVolumeInspection) b_again = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id;
	gint64 other = 0;
	guint i;
	gboolean warned = FALSE;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(nested);
	id = add_volume(fx, a, "A");

	/* Another directory on the same filesystem, inside it, around it, or
	 * the same path. */
	assert_refusal(fx, b, "same filesystem as disk 'A'");
	assert_refusal(fx, nested, "inside disk 'A'");
	assert_refusal(fx, fx->media, "Disk 'A'");
	assert_refusal(fx, fx->media, "is inside this directory");
	assert_refusal(fx, a, "already the path of disk 'A'");

	/* Adding it anyway, with what an inspection would have given, is
	 * refused the same way and writes nothing. */
	for_b = inspect(fx, b);
	request_from(&request, for_b, "B");
	request.plan = AXIS_VOLUME_PLAN_WRITE_MARKER;
	{
		g_autofree gchar *b_marker = marker_path(b);
		struct stat st;

		g_assert_cmpint(g_stat(b, &st), ==, 0);
		request.dev = (guint64)st.st_dev;
		request.ino = (guint64)st.st_ino;
		g_assert_false(axis_storage_add_volume(fx->storage, &request, &other, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
		g_clear_error(&error);
		g_assert_false(g_file_test(b_marker, G_FILE_TEST_EXISTS));
	}

	/* A copy of A's marker elsewhere is A, not a new volume: shown once A
	 * is offline (while it is online the filesystem rule comes first). */
	a_marker = marker_path(a);
	a_marker_content = axis_test_storage_read(a_marker);
	c_marker = marker_path(c);
	axis_test_storage_write(c_marker, a_marker_content);
	unmount_disk(fx, a, "a");
	info = check(fx, id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_OFFLINE);
	assert_refusal(fx, c, "carries the marker of disk 'A'");

	/* A directory of the filesystem A was last seen on, while A is not
	 * there: refused when what is recorded of A proves it the same
	 * filesystem (an identifier of its own: storage hunt, H3); a warning
	 * when only the device's number is the same, which another disk can
	 * have (xfs). */
	b_again = inspect(fx, b);
	if (fsid_is_own(b)) {
		g_assert_cmpint(b_again->plan, ==, AXIS_VOLUME_PLAN_REFUSE);
		assert_contains(b_again->refusal, "filesystem of disk 'A', which is not online now");
	} else {
		g_assert_cmpint(b_again->plan, ==, AXIS_VOLUME_PLAN_WRITE_MARKER);
		for (i = 0; i < b_again->warnings->len; i++)
			warned = warned || strstr(g_ptr_array_index(b_again->warnings, i), "Disk 'A'") != NULL;
		g_assert_true(warned);
	}
}

typedef struct
{
	AxisStorage          *storage;
	AxisVolumeAddRequest  request;
	AxisTestGate         *start;
	gboolean              ok;
	GError               *error;
	gint64                id;
} AddThread;

static gpointer
add_thread(gpointer data)
{
	AddThread *thread = data;

	axis_test_gate_pass(thread->start);
	thread->ok = axis_storage_add_volume(thread->storage, &thread->request, &thread->id,
		&thread->error);
	return NULL;
}

static void
race(
	Fixture              *fx,
	AxisVolumeInspection *first,
	AxisVolumeInspection *second,
	AddThread            *threads
){
	AxisTestGate *start = axis_test_gate_new();
	GThread *running[2];
	gint64 deadline;
	guint i;

	for (i = 0; i < 2; i++) {
		threads[i].storage = fx->storage;
		threads[i].start = start;
		request_from(&threads[i].request, i == 0 ? first : second, i == 0 ? "First" : "Second");
		running[i] = g_thread_new("add", add_thread, &threads[i]);
	}
	/* Both are waiting at the line before either goes. */
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (axis_test_gate_get_arrived(start) < 2 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	axis_test_gate_open(start);
	for (i = 0; i < 2; i++)
		g_thread_join(running[i]);
	axis_test_gate_free(start);
}

/* Spec: Same filesystem twice (two administrators at once) */
static void
test_concurrent_adds(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *x = NULL;
	g_autofree gchar *y = NULL;
	g_autofree gchar *count = NULL;
	g_autoptr(AxisVolumeInspection) one = NULL;
	g_autoptr(AxisVolumeInspection) two = NULL;
	AddThread threads[2];
	guint i;

	SKIP_WITHOUT_DB(fx);

	/* The same directory twice at once: one volume, one marker. */
	memset(threads, 0, sizeof(threads));
	one = inspect(fx, disk);
	race(fx, one, one, threads);
	g_assert_true(threads[0].ok != threads[1].ok);
	for (i = 0; i < 2; i++) {
		if (!threads[i].ok) {
			g_assert_true(g_error_matches(threads[i].error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED) ||
				g_error_matches(threads[i].error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM));
			g_clear_error(&threads[i].error);
		}
	}
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM volume");
	g_assert_cmpstr(count, ==, "1");
	g_assert_cmpuint(axis_test_storage_count_temporaries(disk), ==, 0);
	g_clear_pointer(&count, g_free);

	/* Two directories of one filesystem at once: one volume. */
	{
		g_autoptr(AxisVolumeInfo) first = axis_storage_get_volume(fx->storage,
			threads[0].ok ? threads[0].id : threads[1].id);
		g_autoptr(GError) error = NULL;

		g_assert_true(axis_storage_remove_volume(fx->storage, first->id, FALSE, &error));
		g_assert_no_error(error);
	}
	x = make_disk(fx, "x");
	y = make_disk(fx, "y");
	g_clear_pointer(&one, axis_volume_inspection_free);
	one = inspect(fx, x);
	two = inspect(fx, y);
	memset(threads, 0, sizeof(threads));
	race(fx, one, two, threads);
	g_assert_true(threads[0].ok != threads[1].ok);
	for (i = 0; i < 2; i++) {
		if (!threads[i].ok) {
			g_assert_error(threads[i].error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
			assert_contains(threads[i].error->message, "same filesystem");
			g_clear_error(&threads[i].error);
		}
	}
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM volume");
	g_assert_cmpstr(count, ==, "1");
}

/* ── Presence ───────────────────────────────────────────────────── */

/* Spec: Unmounted disk is offline, not empty */
static void
test_unmounted_is_offline(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "usb");
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "USB");
	assert_state(fx, id, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);

	/* The mount point left behind is empty: offline, never an empty
	 * volume. */
	unmount_disk(fx, disk, "usb");
	assert_state(fx, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_EMPTY);
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_null(guard);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*'USB'*is offline at*"), ==, 1);

	/* No directory at all: offline as well. */
	g_assert_cmpint(g_rmdir(disk), ==, 0);
	assert_state(fx, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_PATH_MISSING);
}

/* Spec: Different disk at the same path */
static void
test_different_disk_is_mismatch(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *other = g_build_filename(fx->dir, "other", NULL);
	g_autofree gchar *other_file = g_build_filename(other, "movie.mkv", NULL);
	g_autofree gchar *other_marker = marker_path(other);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");
	axis_test_storage_mkdir(other);
	axis_test_storage_write(other_file, "another disk's file");
	axis_test_storage_write_marker(other, OTHER_UUID);

	/* Another disk, with its own marker, mounted at the same path. */
	unmount_disk(fx, disk, "disk");
	g_assert_cmpint(g_rmdir(disk), ==, 0);
	g_assert_cmpint(g_rename(other, disk), ==, 0);
	before = axis_test_storage_digest(disk);
	assert_state(fx, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_OTHER_MARKER);
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_null(guard);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	/* It is not used, and nothing was written to it. */
	after = axis_test_storage_digest(disk);
	g_assert_cmpstr(before, ==, after);

	/* Files but no marker: a mismatch too, never adopted. */
	g_assert_false(g_file_test(other_marker, G_FILE_TEST_EXISTS));
	{
		g_autofree gchar *here = marker_path(disk);

		g_assert_cmpint(g_unlink(here), ==, 0);
	}
	assert_state(fx, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NO_MARKER);

	/* A damaged marker. */
	{
		g_autofree gchar *here = marker_path(disk);

		axis_test_storage_write(here, "axis-volume 1\nuuid garbage\n");
	}
	assert_state(fx, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_BAD_MARKER);

	/* A symbolic link at the path, even to the right disk. */
	{
		g_autofree gchar *away = stash_path(fx, "disk");
		g_autofree gchar *moved = g_build_filename(fx->dir, "wrong-disk", NULL);

		g_assert_cmpint(g_rename(disk, moved), ==, 0);
		g_assert_cmpint(symlink(away, disk), ==, 0);
		assert_state(fx, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NOT_DIRECTORY);
		g_assert_cmpint(g_unlink(disk), ==, 0);
	}
}

/* Spec: Volume returns */
static void
test_volume_returns(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "usb");
	g_autoptr(AxisVolumeInfo) gone = NULL;
	g_autoptr(AxisVolumeInfo) back = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "USB");
	unmount_disk(fx, disk, "usb");
	gone = check(fx, id);
	g_assert_cmpint(gone->state, ==, AXIS_VOLUME_OFFLINE);

	/* Mounted again: the next scheduled check (what the job runs) finds
	 * it, with no one doing anything. */
	mount_disk(fx, disk, "usb");
	g_assert_true(axis_storage_check_all(fx->storage, NULL, &error));
	g_assert_no_error(error);
	back = axis_storage_get_volume(fx->storage, id);
	g_assert_cmpint(back->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_cmpint(back->last_online_us, >=, gone->last_checked_us);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*'USB'*is online at*"), ==, 1);
}

/* Spec: Hung filesystem */
static void
test_hung_check(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "nfs");
	g_autofree gchar *spare = make_disk(fx, "spare");
	g_autoptr(AxisVolumeInfo) hung = NULL;
	g_autoptr(AxisVolumeInfo) later = NULL;
	g_autoptr(AxisVolumeInfo) other = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 spare_id;
	gint64 started;
	gint64 deadline;
	guint i;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "NFS");
	/* A second volume on another "filesystem" (add_volume_beside()), added
	 * while the first is away, so both are known. */
	unmount_disk(fx, disk, "nfs");
	g_clear_pointer(&hung, axis_volume_info_free);
	hung = check(fx, id);
	spare_id = add_volume_beside(fx, spare, "Spare");
	mount_disk(fx, disk, "nfs");
	g_clear_pointer(&hung, axis_volume_info_free);
	hung = check(fx, id);
	g_assert_cmpint(hung->state, ==, AXIS_VOLUME_ONLINE);

	axis_storage_set_timeouts(fx->storage, 300, 0);
	fx->hang_path = g_strdup(disk);
	axis_storage_set_check_hook(fx->storage, hang_hook, fx);

	/* The check gives up at its deadline and reports the volume offline. */
	started = g_get_monotonic_time();
	g_clear_pointer(&hung, axis_volume_info_free);
	hung = check(fx, id);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3 * G_USEC_PER_SEC);
	g_assert_cmpint(hung->state, ==, AXIS_VOLUME_OFFLINE);
	g_assert_cmpint(hung->reason, ==, AXIS_VOLUME_REASON_TIMEOUT);

	/* Asking again and again costs no more threads and no waiting. */
	started = g_get_monotonic_time();
	for (i = 0; i < 25; i++) {
		g_autoptr(AxisVolumeInfo) again = check(fx, id);
		g_autoptr(AxisVolumeGuard) guard = axis_volume_guard_acquire(fx->storage, id,
			AXIS_VOLUME_INTENT_READ, &error);

		g_assert_cmpint(again->state, ==, AXIS_VOLUME_OFFLINE);
		g_assert_null(guard);
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
		g_clear_error(&error);
	}
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3 * G_USEC_PER_SEC);
	g_assert_cmpuint(axis_storage_get_helpers_running(fx->storage), ==, 1);
	g_assert_cmpuint(axis_storage_get_helpers_abandoned(fx->storage), ==, 1);

	/* One hung volume does not hold up the others' checks. */
	g_assert_true(axis_storage_check_all(fx->storage, NULL, &error));
	g_assert_no_error(error);
	other = axis_storage_get_volume(fx->storage, spare_id);
	g_assert_cmpint(other->state, ==, AXIS_VOLUME_ONLINE);

	/* When the filesystem answers again, the stuck thread ends and the
	 * volume is online at the next check. */
	axis_test_gate_open(fx->gate);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (axis_storage_get_helpers_running(fx->storage) > 0 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_cmpuint(axis_storage_get_helpers_running(fx->storage), ==, 0);
	later = check(fx, id);
	g_assert_cmpint(later->state, ==, AXIS_VOLUME_ONLINE);
}

/* Hung filesystems elsewhere fill the helper: a volume that cannot be
 * looked at for that reason keeps its state (nothing was learnt about it),
 * while a guard on it is still refused. */
static void
test_full_helper_keeps_state(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *hung_dir = g_build_filename(fx->dir, "hung", NULL);
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeInfo) after = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stored = NULL;
	gint64 id;
	gint64 deadline;
	guint i;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");
	info = check(fx, id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);

	/* Sixteen other directories whose filesystem hangs. */
	axis_storage_set_timeouts(fx->storage, 100, 0);
	fx->hang_path = g_strdup(hung_dir);
	axis_storage_set_check_hook(fx->storage, hang_hook, fx);
	for (i = 0; i < AXIS_STORAGE_MAX_HELPERS; i++) {
		g_autofree gchar *name = g_strdup_printf("d%02u", i);
		g_autofree gchar *path = g_build_filename(hung_dir, name, NULL);
		g_autoptr(AxisVolumeInspection) inspection = NULL;

		axis_test_storage_mkdir(path);
		inspection = inspect(fx, path);
		g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_REFUSE);
	}
	g_assert_cmpuint(axis_storage_get_helpers_running(fx->storage), ==, AXIS_STORAGE_MAX_HELPERS);

	/* The volume is not looked at, and keeps its state. */
	after = check(fx, id);
	g_assert_cmpint(after->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_cmpint(after->last_checked_us, ==, info->last_checked_us);
	g_assert_true(axis_storage_check_all(fx->storage, NULL, &error));
	g_assert_no_error(error);
	stored = axis_test_db_queryf(fx->db, "SELECT state FROM volume WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpstr(stored, ==, "online");
	g_assert_null(axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);

	/* When they answer again, all is as before. */
	axis_test_gate_open(fx->gate);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (axis_storage_get_helpers_running(fx->storage) > 0 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_clear_pointer(&after, axis_volume_info_free);
	after = check(fx, id);
	g_assert_cmpint(after->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_cmpint(after->last_checked_us, >, info->last_checked_us);
}

/* ── Nothing forgotten ──────────────────────────────────────────── */

static gchar *
records_snapshot(Fixture *fx)
{
	g_autofree gchar *volumes = axis_test_db_query(fx->db,
		"SELECT string_agg(id || ':' || uuid || ':' || name || ':' || path || ':' || mount_root || ':'"
		" || marker_created_at || ':' || created_at, ',' ORDER BY id) FROM volume");
	g_autofree gchar *roots = axis_test_db_query(fx->db,
		"SELECT string_agg(id || ':' || volume_id || ':' || rel_path || ':' || media_type || ':' ||"
		" name || ':' || created_at, ',' ORDER BY id) FROM library_root");

	return g_strdup_printf("%s|%s", volumes != NULL ? volumes : "", roots != NULL ? roots : "");
}

/* Spec: Records survive an outage */
static void
test_records_survive_outage(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *tv = g_build_filename(disk, "TV", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *during = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) roots = NULL;
	gint64 id;
	gint64 root_id = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	axis_test_storage_mkdir(tv);
	id = add_volume(fx, disk, "Disk");
	g_assert_true(axis_storage_add_root(fx->storage, id, "Movies", AXIS_MEDIA_MOVIES, NULL, &root_id,
		&error));
	g_assert_true(axis_storage_add_root(fx->storage, id, "TV", AXIS_MEDIA_TV, NULL, NULL, &error));
	g_assert_no_error(error);
	before = records_snapshot(fx);

	/* Away for a while, checked again and again. */
	unmount_disk(fx, disk, "disk");
	for (i = 0; i < 3; i++) {
		g_assert_true(axis_storage_check_all(fx->storage, NULL, &error));
		g_assert_no_error(error);
	}

	/* Nothing can be forgotten while it is away. */
	g_assert_false(axis_storage_remove_root(fx->storage, root_id, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	g_assert_false(axis_storage_remove_volume(fx->storage, id, FALSE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	during = records_snapshot(fx);
	g_assert_cmpstr(during, ==, before);

	/* Back: every record as it was. */
	mount_disk(fx, disk, "disk");
	g_assert_true(axis_storage_check_all(fx->storage, NULL, &error));
	g_assert_no_error(error);
	after = records_snapshot(fx);
	g_assert_cmpstr(after, ==, before);
	g_assert_true(axis_storage_list_roots(fx->storage, id, &roots, &error));
	g_assert_cmpuint(roots->len, ==, 2);
}

/* ── The guard ──────────────────────────────────────────────────── */

/* Spec: Guard refused when offline */
static void
test_guard_refused_when_unavailable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *other = make_disk(fx, "other");
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	g_assert_nonnull(guard);
	g_assert_cmpint(axis_volume_guard_get_fd(guard), >=, 0);
	g_assert_cmpint(axis_volume_guard_get_volume_id(guard), ==, id);
	g_clear_pointer(&guard, axis_volume_guard_release);

	/* Offline: unavailable, which is not "not found". */
	unmount_disk(fx, disk, "disk");
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_null(guard);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_assert_false(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND));
	assert_contains(error->message, "'Disk' is offline");
	g_clear_error(&error);

	/* Mismatched: the same. */
	g_assert_cmpint(g_rmdir(disk), ==, 0);
	axis_test_storage_write_marker(other, OTHER_UUID);
	g_assert_cmpint(g_rename(other, disk), ==, 0);
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_null(guard);
	g_assert_true(g_error_matches(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE) ||
		g_error_matches(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY));
	g_clear_error(&error);
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);

	/* A volume nobody registered is a different error. */
	guard = axis_volume_guard_acquire(fx->storage, 4242, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_FOUND);
}

/* Spec: Guard fails mid-operation */
static void
test_guard_fails_mid_operation(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint fd;
	gint stale;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	id = add_volume(fx, disk, "Disk");
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	fd = axis_volume_guard_open_dir(guard, "Movies", &error);
	g_assert_no_error(error);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	g_assert_true(axis_volume_guard_check(guard, &error));

	/* The disk goes while the guard is held.  Its descriptor still reads
	 * the old directory and its marker, yet the check fails: the path no
	 * longer leads there. */
	unmount_disk(fx, disk, "disk");
	stale = openat(axis_volume_guard_get_fd(guard), AXIS_VOLUME_MARKER_NAME, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(stale, >=, 0);
	close(stale);
	g_assert_false(axis_volume_guard_check(guard, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);

	/* And it stays failed, even when the disk is back. */
	mount_disk(fx, disk, "disk");
	g_assert_false(axis_volume_guard_check(guard, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	g_assert_cmpint(axis_volume_guard_open_dir(guard, "Movies", &error), ==, -1);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
}

typedef struct
{
	AxisVolumeGuard *guard;
	gint             passes;	/* atomic */
	gboolean         failed;
	GError          *error;
} CheckThread;

static gpointer
check_thread(gpointer data)
{
	CheckThread *thread = data;
	gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline) {
		if (!axis_volume_guard_check(thread->guard, &thread->error)) {
			thread->failed = TRUE;
			break;
		}
		g_atomic_int_inc(&thread->passes);
		g_usleep(2000);
	}

	return NULL;
}

/* Spec: Guard fails mid-operation (another thread holds the guard) */
static void
test_guard_held_while_offline(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	CheckThread thread;
	GThread *running;
	gint64 deadline;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");
	memset(&thread, 0, sizeof(thread));
	thread.guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, NULL);
	g_assert_nonnull(thread.guard);

	running = g_thread_new("holder", check_thread, &thread);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while (g_atomic_int_get(&thread.passes) < 3 && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_cmpint(g_atomic_int_get(&thread.passes), >=, 3);

	/* While the scheduled check runs on this thread, the disk goes. */
	unmount_disk(fx, disk, "disk");
	g_assert_true(axis_storage_check_all(fx->storage, NULL, NULL));
	g_thread_join(running);

	g_assert_true(thread.failed);
	g_assert_error(thread.error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&thread.error);
	axis_volume_guard_release(thread.guard);
}

/* After a failure, only an online volume's errors are about the path. */
static void
test_guard_open_dir(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *link = g_build_filename(disk, "Link", NULL);
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	g_assert_cmpint(symlink(movies, link), ==, 0);
	id = add_volume(fx, disk, "Disk");
	guard = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);

	/* The volume is there and the directory is not: not found. */
	g_assert_cmpint(axis_volume_guard_open_dir(guard, "Nothing", &error), ==, -1);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_cmpint(axis_volume_guard_open_dir(guard, "Link", &error), ==, -1);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_clear_error(&error);
	g_assert_cmpint(axis_volume_guard_open_dir(guard, "Movies/..", &error), ==, -1);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
	g_clear_error(&error);

	/* The volume went away: the same request is "unavailable", never
	 * "not found". */
	unmount_disk(fx, disk, "disk");
	g_assert_cmpint(axis_volume_guard_open_dir(guard, "Nothing", &error), ==, -1);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
}

/* Spec: Write refused in read-only mode */
static void
test_guard_read_only_mode(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(AxisStorage) bare = NULL;
	g_autoptr(AxisSettings) undeclared = NULL;
	g_autoptr(AxisStorage) unknowing = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");

	/* On (the default): reading is fine, writing is refused. */
	reader = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reader);
	writer = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_null(writer);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);

	/* Off: granted; on again: the held write guard fails its check. */
	set_read_only(fx, FALSE);
	writer = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(writer);
	g_assert_true(axis_volume_guard_check(writer, &error));
	set_read_only(fx, TRUE);
	g_assert_false(axis_volume_guard_check(writer, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);
	g_assert_true(axis_volume_guard_check(reader, &error));

	/* Without a settings store, or with one that does not know the
	 * setting, storage is read-only. */
	bare = axis_storage_new(fx->pool, NULL);
	g_assert_true(axis_storage_is_read_only_mode(bare));
	undeclared = axis_settings_new();
	g_assert_true(axis_settings_load(undeclared, NULL, NULL));
	unknowing = axis_storage_new(fx->pool, undeclared);
	g_assert_true(axis_storage_is_read_only_mode(unknowing));
}

/* ── Roots ──────────────────────────────────────────────────────── */

static void
assert_root_refused(
	Fixture     *fx,
	gint64       volume,
	const gchar *path,
	gint         code
){
	g_autoptr(GError) error = NULL;

	if (axis_storage_add_root(fx->storage, volume, path, AXIS_MEDIA_OTHER, NULL, NULL, &error))
		g_error("root '%s' was accepted", path);
	g_assert_error(error, AXIS_STORAGE_ERROR, code);
}

/* Spec: Add a root; Nested root refused; Path escaping the volume refused */
static void
test_roots(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies_hd = g_build_filename(disk, "Movies", "HD", NULL);
	g_autofree gchar *tv = g_build_filename(disk, "TV", NULL);
	g_autofree gchar *tv_abs = g_build_filename(disk, "TV", NULL);
	g_autofree gchar *outside = g_build_filename(fx->dir, "outside", NULL);
	g_autofree gchar *link_out = g_build_filename(disk, "Out", NULL);
	g_autofree gchar *link_in = g_build_filename(disk, "In", NULL);
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *count = NULL;
	g_autoptr(GPtrArray) roots = NULL;
	g_autoptr(GError) error = NULL;
	const AxisRootInfo *root;
	gint64 id;
	gint64 root_id = 0;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies_hd);
	axis_test_storage_mkdir(tv);
	axis_test_storage_mkdir(outside);
	g_assert_cmpint(symlink(outside, link_out), ==, 0);
	g_assert_cmpint(symlink(movies, link_in), ==, 0);
	id = add_volume(fx, disk, "Disk");

	/* Recorded relative to the volume, named after its directory. */
	g_assert_true(axis_storage_add_root(fx->storage, id, "Movies", AXIS_MEDIA_MOVIES, NULL, &root_id,
		&error));
	g_assert_no_error(error);
	g_assert_true(axis_storage_add_root(fx->storage, id, tv_abs, AXIS_MEDIA_TV, "Series", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_storage_list_roots(fx->storage, id, &roots, &error));
	g_assert_cmpuint(roots->len, ==, 2);
	root = g_ptr_array_index(roots, 0);
	g_assert_cmpstr(root->rel_path, ==, "Movies");
	g_assert_cmpstr(root->name, ==, "Movies");
	g_assert_cmpint(root->media_type, ==, AXIS_MEDIA_MOVIES);
	root = g_ptr_array_index(roots, 1);
	g_assert_cmpstr(root->rel_path, ==, "TV");
	g_assert_cmpstr(root->name, ==, "Series");

	/* Inside, around or the same as another root. */
	assert_root_refused(fx, id, "Movies/HD", AXIS_STORAGE_ERROR_REFUSED);
	assert_root_refused(fx, id, "", AXIS_STORAGE_ERROR_REFUSED);
	assert_root_refused(fx, id, "Movies", AXIS_STORAGE_ERROR_REFUSED);

	/* Out of the volume through "..", a link or an absolute path. */
	assert_root_refused(fx, id, "../outside", AXIS_STORAGE_ERROR_INVALID);
	assert_root_refused(fx, id, "Movies/../../outside", AXIS_STORAGE_ERROR_INVALID);
	assert_root_refused(fx, id, "Out", AXIS_STORAGE_ERROR_NOT_CONTAINED);
	assert_root_refused(fx, id, "In", AXIS_STORAGE_ERROR_NOT_CONTAINED);
	assert_root_refused(fx, id, "Out/x", AXIS_STORAGE_ERROR_NOT_CONTAINED);
	assert_root_refused(fx, id, outside, AXIS_STORAGE_ERROR_NOT_CONTAINED);

	/* Missing, not a directory, malformed. */
	assert_root_refused(fx, id, "Nothing", AXIS_STORAGE_ERROR_REFUSED);
	assert_root_refused(fx, id, "readme.txt", AXIS_STORAGE_ERROR_REFUSED);
	assert_root_refused(fx, id, "Bad\nName", AXIS_STORAGE_ERROR_INVALID);

	/* Only while the volume is online. */
	unmount_disk(fx, disk, "disk");
	assert_root_refused(fx, id, "Elsewhere", AXIS_STORAGE_ERROR_UNAVAILABLE);
	mount_disk(fx, disk, "disk");

	count = axis_test_db_query(fx->db, "SELECT count(*) FROM library_root");
	g_assert_cmpstr(count, ==, "2");
}

/* Spec: Remove a root (and the volume): files untouched */
static void
test_removal_leaves_files(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *film = g_build_filename(movies, "Film (2001).mkv", NULL);
	g_autofree gchar *subs = g_build_filename(movies, "Film (2001).en.srt", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after_root = NULL;
	g_autofree gchar *after_volume = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *count = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeInfo) again = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 root_id = 0;
	gint64 readded;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	axis_test_storage_write(film, "frames");
	axis_test_storage_write(subs, "1\n00:00:01,000 --> 00:00:02,000\nhello\n");
	id = add_volume(fx, disk, "Disk");
	info = axis_storage_get_volume(fx->storage, id);
	uuid = g_strdup(info->uuid);
	g_assert_true(axis_storage_add_root(fx->storage, id, "Movies", AXIS_MEDIA_MOVIES, NULL, &root_id,
		&error));
	before = axis_test_storage_digest(disk);

	g_assert_true(axis_storage_remove_root(fx->storage, root_id, &error));
	g_assert_no_error(error);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM library_root");
	g_assert_cmpstr(count, ==, "0");
	after_root = axis_test_storage_digest(disk);
	g_assert_cmpstr(after_root, ==, before);

	g_assert_true(axis_storage_remove_volume(fx->storage, id, FALSE, &error));
	g_assert_no_error(error);
	g_assert_null(axis_storage_get_volume(fx->storage, id));
	after_volume = axis_test_storage_digest(disk);
	g_assert_cmpstr(after_volume, ==, before);

	/* The marker stayed: adding the directory again is the same volume. */
	readded = add_volume(fx, disk, "Disk again");
	again = axis_storage_get_volume(fx->storage, readded);
	g_assert_cmpstr(again->uuid, ==, uuid);
}

/* An administrator may forget a volume that is gone for good. */
static void
test_remove_unavailable_needs_confirmation(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *count = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	id = add_volume(fx, disk, "Disk");
	g_assert_true(axis_storage_add_root(fx->storage, id, "Movies", AXIS_MEDIA_MOVIES, NULL, NULL,
		&error));
	unmount_disk(fx, disk, "disk");

	g_assert_false(axis_storage_remove_volume(fx->storage, id, FALSE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	g_assert_true(axis_storage_remove_volume(fx->storage, id, TRUE, &error));
	g_assert_no_error(error);
	count = axis_test_db_query(fx->db, "SELECT (SELECT count(*) FROM volume) + (SELECT count(*) FROM library_root)");
	g_assert_cmpstr(count, ==, "0");
}

/* ── Space ──────────────────────────────────────────────────────── */

/* Spec: Free space is shown (what is recorded) */
static void
test_free_space(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *stored = NULL;
	g_autoptr(AxisVolumeInfo) online = NULL;
	g_autoptr(AxisVolumeInfo) offline = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");
	online = check(fx, id);
	g_assert_true(online->has_space);
	g_assert_cmpuint(online->total_bytes, >, 0);
	g_assert_cmpuint(online->free_bytes, <=, online->total_bytes);
	g_assert_cmpuint(online->available_bytes, <=, online->free_bytes);
	g_assert_cmpint(online->space_checked_us, >, 0);
	stored = axis_test_db_queryf(fx->db, "SELECT total_bytes FROM volume WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_cmpint(g_ascii_strtoll(stored, NULL, 10), ==, (gint64)online->total_bytes);

	/* Offline keeps the last known space and when it was last seen. */
	unmount_disk(fx, disk, "disk");
	g_usleep(10000);
	offline = check(fx, id);
	g_assert_true(offline->has_space);
	g_assert_cmpuint(offline->total_bytes, ==, online->total_bytes);
	g_assert_cmpint(offline->space_checked_us, ==, online->space_checked_us);
	g_assert_cmpint(offline->last_online_us, ==, online->last_online_us);
	g_assert_cmpint(offline->last_checked_us, >, online->last_checked_us);
}

/* ── Re-pointing and restoring ──────────────────────────────────── */

static void
test_relocate(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "a");
	g_autofree gchar *moved = g_build_filename(fx->media, "b", NULL);
	g_autofree gchar *copy = make_disk(fx, "copy");
	g_autofree gchar *wrong = make_disk(fx, "wrong");
	g_autofree gchar *disk_marker = marker_path(disk);
	g_autofree gchar *marker = NULL;
	g_autofree gchar *uuid = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeInfo) after = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	id = add_volume(fx, disk, "Disk");
	info = axis_storage_get_volume(fx->storage, id);
	uuid = g_strdup(info->uuid);
	marker = axis_test_storage_read(disk_marker);

	/* Another marker at the new path is refused. */
	axis_test_storage_write_marker(wrong, OTHER_UUID);
	g_assert_false(axis_storage_relocate_volume(fx->storage, id, wrong, TRUE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_clear_error(&error);

	/* A copy of the marker while the volume is still present is a copy. */
	{
		g_autofree gchar *copy_marker = marker_path(copy);

		axis_test_storage_write(copy_marker, marker);
	}
	g_assert_false(axis_storage_relocate_volume(fx->storage, id, copy, TRUE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	assert_contains(error->message, "still present");
	g_clear_error(&error);

	/* The disk mounted somewhere else: offline at the old path, then
	 * pointed at the new one, the same volume. */
	g_assert_cmpint(g_rename(disk, moved), ==, 0);
	g_clear_pointer(&info, axis_volume_info_free);
	info = check(fx, id);
	g_assert_cmpint(info->reason, ==, AXIS_VOLUME_REASON_PATH_MISSING);
	g_assert_true(axis_storage_relocate_volume(fx->storage, id, moved, TRUE, &error));
	g_assert_no_error(error);
	after = check(fx, id);
	g_assert_cmpstr(after->path, ==, moved);
	g_assert_cmpstr(after->uuid, ==, uuid);
	g_assert_cmpint(after->state, ==, AXIS_VOLUME_ONLINE);
}

static void
test_restore_marker(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *movies = g_build_filename(disk, "Movies", NULL);
	g_autofree gchar *marker = NULL;
	g_autofree gchar *uuid = NULL;
	g_autoptr(AxisMarkerRestoreInspection) look = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeMarker read_back;
	g_autofree gchar *content = NULL;
	gboolean own;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(movies);
	id = add_volume(fx, disk, "Disk");
	g_assert_true(axis_storage_add_root(fx->storage, id, "Movies", AXIS_MEDIA_MOVIES, NULL, NULL,
		&error));
	info = axis_storage_get_volume(fx->storage, id);
	uuid = g_strdup(info->uuid);
	marker = marker_path(disk);

	/* Someone deletes the marker: files without a marker. */
	g_assert_cmpint(g_unlink(marker), ==, 0);
	assert_state(fx, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NO_MARKER);
	g_assert_true(axis_storage_inspect_marker_restore(fx->storage, id, &look, &error));
	g_assert_no_error(error);
	g_assert_true(look->possible);
	g_assert_cmpuint(look->roots->len, ==, 1);
	g_assert_true(g_array_index(look->root_present, gboolean, 0));

	/* The filesystem there is the one the volume was last seen on; where its
	 * identifier is only the device's number (xfs), Axis cannot tell, and
	 * asks for it to be confirmed. */
	own = fsid_is_own(disk);
	g_assert_cmpint(look->filesystem, ==, own ? AXIS_MARKER_RESTORE_FS_SAME : AXIS_MARKER_RESTORE_FS_DEVICE_ONLY);
	g_assert_nonnull(look->filesystem_text);

	/* Refused in read-only mode; writes nothing. */
	g_assert_false(axis_storage_restore_marker(fx->storage, id, look->dev, look->ino, look->fsid, !own,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* Not the directory that was shown, or not its filesystem. */
	set_read_only(fx, FALSE);
	g_assert_false(axis_storage_restore_marker(fx->storage, id, look->dev, look->ino + 1, look->fsid, TRUE,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	g_clear_error(&error);
	g_assert_false(axis_storage_restore_marker(fx->storage, id, look->dev, look->ino,
		"0000000000000001", TRUE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	g_clear_error(&error);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* The volume last seen on another filesystem (its identifier recorded
	 * then is another): said, and not written unless confirmed. */
	axis_test_db_exec(fx->db, "UPDATE volume SET fsid = '0123456789abcdef'");
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	g_clear_pointer(&look, axis_marker_restore_inspection_free);
	g_assert_true(axis_storage_inspect_marker_restore(fx->storage, id, &look, &error));
	g_assert_no_error(error);
	g_assert_true(look->possible);
	g_assert_cmpint(look->filesystem, ==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	assert_contains(look->filesystem_text, "not the one this disk was last seen on");
	assert_contains(look->filesystem_text, "0123456789abcdef");
	g_assert_false(axis_storage_restore_marker(fx->storage, id, look->dev, look->ino, look->fsid, FALSE,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	assert_contains(error->message, "not the one this disk was last seen on");
	g_clear_error(&error);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* Confirmed: the volume's own identifier, and it is online. */
	g_assert_true(axis_storage_restore_marker(fx->storage, id, look->dev, look->ino, look->fsid, TRUE,
		&error));
	g_assert_no_error(error);
	content = axis_test_storage_read(marker);
	g_assert_true(axis_volume_marker_parse((const guint8 *)content, strlen(content), &read_back, NULL));
	g_assert_cmpstr(read_back.uuid, ==, uuid);
	assert_state(fx, id, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);

	/* Never into an empty directory: that is an unmounted disk. */
	unmount_disk(fx, disk, "disk");
	g_clear_pointer(&look, axis_marker_restore_inspection_free);
	g_assert_true(axis_storage_inspect_marker_restore(fx->storage, id, &look, &error));
	g_assert_false(look->possible);
	g_assert_false(axis_storage_restore_marker(fx->storage, id, look->dev, look->ino, look->fsid, TRUE,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));
}

/* An empty directory that is not a mount point is what an unmounted disk
 * looks like: it is never made a volume. */
static void
test_add_empty_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *empty = g_build_filename(fx->media, "usb", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_storage_mkdir(empty);
	before = axis_test_storage_digest(empty);
	assert_refusal(fx, empty, "empty and is not a mount point");
	after = axis_test_storage_digest(empty);
	g_assert_cmpstr(before, ==, after);
}

/* What was confirmed must be what is there. */
static void
test_add_needs_what_was_confirmed(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *other = make_disk(fx, "other");
	g_autofree gchar *marker = marker_path(disk);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;

	SKIP_WITHOUT_DB(fx);
	inspection = inspect(fx, disk);
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_WRITE_MARKER);
	g_assert_nonnull(inspection->fsid);
	request_from(&request, inspection, "Disk");

	/* The same inode on another filesystem of the same size (the top of
	 * every disk of a type looks alike): not what was shown. */
	request.fsid = "0000000000000001";
	g_assert_false(axis_storage_add_volume(fx->storage, &request, NULL, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	g_clear_error(&error);
	request.fsid = inspection->fsid;
	/* The size counts where the identifier is only the device's number;
	 * where the filesystem has one of its own, the size may move (a pool
	 * written) and is not compared (storage hunt, H5; test_add_size_moves). */
	if (!fsid_is_own(disk)) {
		request.total_bytes++;
		g_assert_false(axis_storage_add_volume(fx->storage, &request, NULL, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
		g_clear_error(&error);
		request.total_bytes--;
	}
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* Another directory at the path since it was shown. */
	unmount_disk(fx, disk, "disk");
	g_assert_cmpint(g_rmdir(disk), ==, 0);
	g_assert_cmpint(g_rename(other, disk), ==, 0);
	g_assert_false(axis_storage_add_volume(fx->storage, &request, NULL, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	g_clear_error(&error);
	g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

	/* A marker appeared since: adopting it was never confirmed. */
	g_clear_pointer(&inspection, axis_volume_inspection_free);
	inspection = inspect(fx, disk);
	request_from(&request, inspection, "Disk");
	axis_test_storage_write_marker(disk, OTHER_UUID);
	before = axis_test_storage_digest(disk);
	g_assert_false(axis_storage_add_volume(fx->storage, &request, NULL, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	after = axis_test_storage_digest(disk);
	g_assert_cmpstr(before, ==, after);
}

/* A file at @dir/@name, last changed @age seconds ago. */
static void
aged_file(
	const gchar *dir,
	const gchar *name,
	gint64       age
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	struct timespec times[2];

	axis_test_storage_write(path, "a marker being written\n");
	times[0].tv_sec = (time_t)(g_get_real_time() / G_USEC_PER_SEC - age);
	times[0].tv_nsec = 0;
	times[1] = times[0];
	g_assert_cmpint(utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW), ==, 0);
}

/*
 * Storage hunt, minor (c), and review of the fixes, item 5: a crash while the
 * marker was written leaves its temporary file at the volume's top.  Write
 * guards remove those an interrupted write left: names of exactly that
 * form, regular files, found the very same at two looks the sweep age apart
 * (by the monotonic clock: 0 here, so the next guard's look), never by their
 * time against a clock (an hour old by its time, a file is kept at the first
 * look); one that changed between the looks (a write going on) stays;
 * anything else stays, and a read guard removes nothing.
 */
static void
test_stale_marker_temporaries(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "disk");
	g_autofree gchar *stale = NULL;
	g_autofree gchar *fresh = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *upper = NULL;
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	SKIP_WITHOUT_DB(fx);
	axis_storage_set_marker_sweep_age(fx->storage, 0);
	id = add_volume(fx, disk, "Disk");
	stale = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789abcdef", NULL);
	fresh = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME ".tmp-fedcba9876543210", NULL);
	other = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME ".tmp-not-axis-s", NULL);
	upper = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789ABCDEF", NULL);
	link = g_build_filename(disk, AXIS_VOLUME_MARKER_NAME ".tmp-aaaaaaaaaaaaaaaa", NULL);
	aged_file(disk, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789abcdef", 3600);
	aged_file(disk, AXIS_VOLUME_MARKER_NAME ".tmp-fedcba9876543210", 10);
	aged_file(disk, AXIS_VOLUME_MARKER_NAME ".tmp-not-axis-s", 3600);
	aged_file(disk, AXIS_VOLUME_MARKER_NAME ".tmp-0123456789ABCDEF", 3600);
	g_assert_cmpint(symlink("readme.txt", link), ==, 0);

	/* Read: nothing removed. */
	reader = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	g_assert_true(g_file_test(stale, G_FILE_TEST_EXISTS));

	/* The first write guard looks: nothing removed, however old. */
	set_read_only(fx, FALSE);
	writer = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(writer);
	g_assert_true(g_file_test(stale, G_FILE_TEST_EXISTS));
	g_clear_pointer(&writer, axis_volume_guard_release);

	/* A write goes on in one of them meanwhile. */
	axis_test_storage_write(fresh, "a marker being written, more of it\n");

	/* The next: the one found the same goes, the rest stays. */
	writer = axis_volume_guard_acquire(fx->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(writer);
	g_assert_false(g_file_test(stale, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(fresh, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(other, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(upper, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*removed 1 temporary marker file*"), ==, 1);
	assert_state(fx, id, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);
}

/* Storage hunt, H5: a filesystem with an identifier of its own is the same
 * filesystem whatever its size says (ZFS reports a dataset's size with its
 * pool's free space, which moves as the pool is written): a confirmation
 * whose size differs from what is there now still adds it. */
static void
test_add_size_moves(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *disk = make_disk(fx, "pool");
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	SKIP_WITHOUT_DB(fx);
	if (!fsid_is_own(disk)) {
		g_test_skip("the scratch directory's filesystem reports only its device's number");
		return;
	}
	inspection = inspect(fx, disk);
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_WRITE_MARKER);
	request_from(&request, inspection, "Pool");
	request.total_bytes += 1024 * 1024;
	g_assert_true(axis_storage_add_volume(fx->storage, &request, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
}

/* The schema refuses what the code never writes: control characters in
 * paths and names (which would forge log lines), relative or dotted
 * paths. */
static void
test_schema_refuses_bad_text(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *refused = NULL;

	SKIP_WITHOUT_DB(fx);
	refused = axis_test_db_query(fx->db,
		"SELECT string_agg(ok::text, ',' ORDER BY n) FROM (VALUES"
		" (1, E'/media/a\\nb' !~ '[\\x01-\\x1f\\x7f\\u0080-\\u009f]'),"
		" (2, E'/media/a' || chr(133) !~ '[\\x01-\\x1f\\x7f\\u0080-\\u009f]'),"
		" (3, E'/media/caf\\u00e9 x' !~ '[\\x01-\\x1f\\x7f\\u0080-\\u009f]'),"
		" (4, '/media/../etc' !~ '/\\.\\.?(/|$)'),"
		" (5, '/media/.hidden' !~ '/\\.\\.?(/|$)'),"
		" (6, 'media/a' ~ '^(/[^/]+)+$')) AS t(n, ok)");
	g_assert_cmpstr(refused, ==, "false,false,true,false,true,false");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD_TEST(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD_TEST("/storage/schema/refuses-bad-text", test_schema_refuses_bad_text);
	ADD_TEST("/storage/volume/add-writes-marker", test_add_writes_marker);
	ADD_TEST("/storage/volume/adopt-existing-marker", test_adopt_existing_marker);
	ADD_TEST("/storage/volume/same-filesystem-refused", test_same_filesystem_refused);
	ADD_TEST("/storage/volume/concurrent-adds", test_concurrent_adds);
	ADD_TEST("/storage/volume/add-empty-refused", test_add_empty_refused);
	ADD_TEST("/storage/volume/add-needs-what-was-confirmed", test_add_needs_what_was_confirmed);
	ADD_TEST("/storage/volume/add-size-moves", test_add_size_moves);
	ADD_TEST("/storage/guard/stale-marker-temporaries", test_stale_marker_temporaries);
	ADD_TEST("/storage/presence/unmounted-is-offline", test_unmounted_is_offline);
	ADD_TEST("/storage/presence/different-disk-is-mismatch", test_different_disk_is_mismatch);
	ADD_TEST("/storage/presence/volume-returns", test_volume_returns);
	ADD_TEST("/storage/presence/hung-check", test_hung_check);
	ADD_TEST("/storage/presence/full-helper-keeps-state", test_full_helper_keeps_state);
	ADD_TEST("/storage/records/survive-outage", test_records_survive_outage);
	ADD_TEST("/storage/guard/refused-when-unavailable", test_guard_refused_when_unavailable);
	ADD_TEST("/storage/guard/fails-mid-operation", test_guard_fails_mid_operation);
	ADD_TEST("/storage/guard/held-while-offline", test_guard_held_while_offline);
	ADD_TEST("/storage/guard/open-dir", test_guard_open_dir);
	ADD_TEST("/storage/guard/read-only-mode", test_guard_read_only_mode);
	ADD_TEST("/storage/roots/add-nested-escape", test_roots);
	ADD_TEST("/storage/remove/leaves-files", test_removal_leaves_files);
	ADD_TEST("/storage/remove/unavailable-needs-confirmation", test_remove_unavailable_needs_confirmation);
	ADD_TEST("/storage/space/recorded", test_free_space);
	ADD_TEST("/storage/volume/relocate", test_relocate);
	ADD_TEST("/storage/volume/restore-marker", test_restore_marker);

	return g_test_run();
}
