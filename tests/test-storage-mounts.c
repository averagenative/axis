/*
 * test-storage-mounts.c - volumes on real mounts: unmounting, another disk
 * at the same path, read-only remounts, lazy unmounts under a held guard,
 * a root crossing onto another mount, and a disk added read-only without a
 * marker (recognised by its filesystem until the marker can be written)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test runs in a child process that enters a new user and mount
 * namespace (as `unshare --user --map-root-user --mount` does), where it
 * may mount tmpfs filesystems and bind mounts privately: nothing is
 * mounted on the host, and everything disappears with the child.  Where
 * the kernel does not allow unprivileged user namespaces the tests skip.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "core/axis-settings.h"
#include "storage/axis-storage.h"
#include "storage/axis-storage-fs.h"
#include "storage/axis-volume-guard.h"
#include "storage/axis-volume-marker.h"
#include "storage/axis-volume-write.h"

#define OTHER_UUID "11111111-2222-4333-8444-555555555555"

/* ── Entering a namespace ───────────────────────────────────────── */

/* Whether a child can enter a user and mount namespace here. */
static gboolean
namespaces_available(void)
{
	pid_t pid = fork();
	gint status = 0;

	if (pid == 0)
		_exit(unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0 ? 0 : 1);
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return FALSE;

	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void
write_proc(
	const gchar *path,
	const gchar *text
){
	gint fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		g_error("open %s: %s", path, g_strerror(errno));
	if (write(fd, text, strlen(text)) != (gssize)strlen(text))
		g_error("write %s: %s", path, g_strerror(errno));
	close(fd);
}

/* In the child, before any thread exists: become root of a new user
 * namespace with a private mount namespace. */
static void
enter_namespace(void)
{
	g_autofree gchar *uid_map = g_strdup_printf("0 %u 1\n", (guint)getuid());
	g_autofree gchar *gid_map = g_strdup_printf("0 %u 1\n", (guint)getgid());

	/* GLib makes a test's child process non-dumpable (no core files),
	 * which also gives its /proc/self files to root; the maps below could
	 * not be written. */
	if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0)
		g_error("prctl: %s", g_strerror(errno));
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
		g_error("unshare: %s", g_strerror(errno));
	write_proc("/proc/self/setgroups", "deny");
	write_proc("/proc/self/uid_map", uid_map);
	write_proc("/proc/self/gid_map", gid_map);
	/* Nothing mounted here propagates anywhere. */
	g_assert_cmpint(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), ==, 0);
}

static void
mount_tmpfs(const gchar *where)
{
	if (mount("axis-test", where, "tmpfs", 0, "size=4m,mode=0755") != 0)
		g_error("mount tmpfs on %s: %s", where, g_strerror(errno));
}

static void
mount_bind(
	const gchar *from,
	const gchar *to
){
	if (mount(from, to, NULL, MS_BIND, NULL) != 0)
		g_error("bind %s on %s: %s", from, to, g_strerror(errno));
}

static void
remount(
	const gchar *where,
	gboolean     read_only
){
	if (mount(NULL, where, NULL, MS_REMOUNT | MS_BIND | (read_only ? MS_RDONLY : 0), NULL) != 0)
		g_error("remount %s: %s", where, g_strerror(errno));
}

static void
unmount(
	const gchar *where,
	gint         flags
){
	if (umount2(where, flags) != 0)
		g_error("umount %s: %s", where, g_strerror(errno));
}

/* ── The world inside the child ─────────────────────────────────── */

typedef struct
{
	AxisTestDb   *db;
	AxisDbPool   *pool;
	AxisSettings *settings;
	AxisStorage  *storage;
	gchar        *dir;
	gchar        *mnt;		/* where disks are mounted */
	gchar        *disk_a;	/* a tmpfs, the first disk */
	gchar        *disk_b;	/* another tmpfs */
	GPtrArray    *mounts;	/* to undo, last first */
} World;

static World *
world_new(void)
{
	World *world = g_new0(World, 1);
	g_autoptr(GError) error = NULL;

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");

	world->dir = axis_test_storage_dir_new();
	world->mnt = g_build_filename(world->dir, "mnt", NULL);
	world->disk_a = g_build_filename(world->dir, "disk-a", NULL);
	world->disk_b = g_build_filename(world->dir, "disk-b", NULL);
	world->mounts = g_ptr_array_new_with_free_func(g_free);
	axis_test_storage_mkdir(world->mnt);
	axis_test_storage_mkdir(world->disk_a);
	axis_test_storage_mkdir(world->disk_b);
	mount_tmpfs(world->disk_a);
	g_ptr_array_add(world->mounts, g_strdup(world->disk_a));
	mount_tmpfs(world->disk_b);
	g_ptr_array_add(world->mounts, g_strdup(world->disk_b));

	world->db = axis_test_db_new();
	g_assert_nonnull(world->db);
	world->pool = axis_test_jobs_pool(world->db, 4);
	world->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(world->settings, AXIS_STORAGE_MODULE_ID,
		axis_storage_get_setting_table(), &error));
	g_assert_true(axis_settings_load(world->settings, NULL, &error));
	world->storage = axis_storage_new(world->pool, world->settings);
	g_assert_true(axis_storage_load(world->storage, &error));
	g_assert_no_error(error);

	return world;
}

static void
world_free(World *world)
{
	guint i;

	axis_storage_stop(world->storage);
	axis_storage_unref(world->storage);
	axis_settings_unref(world->settings);
	axis_db_pool_free(world->pool);
	axis_test_db_free(world->db);
	while (g_main_context_iteration(NULL, FALSE))
		;

	/* Everything mounted, detached, before the scratch directory goes. */
	for (i = world->mounts->len; i > 0; i--)
		umount2(g_ptr_array_index(world->mounts, i - 1), MNT_DETACH);
	g_ptr_array_unref(world->mounts);
	axis_test_storage_dir_free(world->dir);
	g_free(world->mnt);
	g_free(world->disk_a);
	g_free(world->disk_b);
	g_free(world);
	axis_test_log_reset();
}

static void
put_file(
	const gchar *dir,
	const gchar *name,
	const gchar *content
){
	g_autofree gchar *path = g_build_filename(dir, name, NULL);

	axis_test_storage_write(path, content);
}

static gint64
add_volume(
	World       *world,
	const gchar *path,
	const gchar *name,
	gboolean     expect_mount_point
){
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	g_assert_true(axis_storage_inspect_volume(world->storage, path, &inspection, &error));
	g_assert_no_error(error);
	if (inspection->plan == AXIS_VOLUME_PLAN_REFUSE)
		g_error("cannot add %s: %s", path, inspection->refusal);
	g_assert_cmpint(inspection->mount_root, ==, expect_mount_point);

	memset(&request, 0, sizeof(request));
	request.path = inspection->path;
	request.name = name;
	request.plan = inspection->plan;
	request.marker_uuid = inspection->marker_uuid;
	request.dev = inspection->dev;
	request.ino = inspection->ino;
	request.fsid = inspection->fsid;
	request.total_bytes = inspection->total_bytes;
	/* A mount point needs no extra confirmation. */
	request.not_mount_point_ok = FALSE;
	g_assert_true(axis_storage_add_volume(world->storage, &request, &id, &error));
	g_assert_no_error(error);

	return id;
}

static AxisVolumeInfo *
check(
	World  *world,
	gint64  id
){
	AxisVolumeInfo *info = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_storage_check_volume(world->storage, id, &info, &error));
	g_assert_no_error(error);
	return info;
}

static void
assert_state(
	World            *world,
	gint64            id,
	AxisVolumeState   state,
	AxisVolumeReason  reason
){
	g_autoptr(AxisVolumeInfo) info = check(world, id);

	g_assert_cmpstr(axis_volume_state_to_string(info->state), ==, axis_volume_state_to_string(state));
	g_assert_cmpstr(axis_volume_reason_to_string(info->reason), ==, axis_volume_reason_to_string(reason));
}

static void
set_read_only(
	World    *world,
	gboolean  on
){
	g_assert_true(axis_settings_set(world->settings, AXIS_STORAGE_SETTING_READ_ONLY,
		on ? "true" : "false", NULL));
}

/* Runs @body in a child inside its own namespaces. */
static void
in_namespace(void (*body)(void))
{
	if (g_test_subprocess()) {
		enter_namespace();
		body();
		return;
	}

	if (!namespaces_available()) {
		g_test_skip("unprivileged user and mount namespaces are not available here");
		return;
	}
	if (g_getenv("AXIS_TEST_DATABASE_URL") == NULL && g_strcmp0(g_getenv("AXIS_SKIP_DB_TESTS"), "1") == 0) {
		g_test_skip("AXIS_SKIP_DB_TESTS=1");
		return;
	}

	g_test_trap_subprocess(NULL, 120 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Spec: Unmounted disk is offline, not empty; Volume returns */
static void
unmount_and_return(void)
{
	World *world = world_new();
	g_autofree gchar *marker = g_build_filename(world->mnt, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *copy = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	id = add_volume(world, world->mnt, "Disk", TRUE);
	info = check(world, id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_true(info->mount_root);
	g_assert_cmpstr(info->fs_type, ==, "tmpfs");
	copy = axis_test_storage_read(marker);

	/* Unmounted: the mount point is an empty directory again. */
	unmount(world->mnt, 0);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_NOT_MOUNTED);
	guard = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_null(guard);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);

	/* Even a copy of the disk, marker included, left in the mount point is
	 * not the volume: the directory is not a mount point any more. */
	axis_test_storage_write(marker, copy);
	put_file(world->mnt, "film.mkv", "frames");
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_NOT_MOUNTED);
	guard = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_null(guard);
	g_clear_error(&error);
	g_assert_cmpint(g_unlink(marker), ==, 0);
	{
		g_autofree gchar *film = g_build_filename(world->mnt, "film.mkv", NULL);

		g_assert_cmpint(g_unlink(film), ==, 0);
	}

	/* Mounted again: online at the next check. */
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	g_assert_true(axis_storage_check_all(world->storage, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&info, axis_volume_info_free);
	info = axis_storage_get_volume(world->storage, id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);

	world_free(world);
}

static void
test_unmount_and_return(void)
{
	in_namespace(unmount_and_return);
}

/* Spec: Different disk at the same path */
static void
other_disk(void)
{
	World *world = world_new();
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	id = add_volume(world, world->mnt, "Disk", TRUE);
	unmount(world->mnt, 0);

	/* Another filesystem, with its own marker, at the same mount point. */
	axis_test_storage_write_marker(world->disk_b, OTHER_UUID);
	put_file(world->disk_b, "other.mkv", "other frames");
	mount_bind(world->disk_b, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	before = axis_test_storage_digest(world->disk_b);
	assert_state(world, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_OTHER_MARKER);
	g_assert_null(axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, NULL));
	after = axis_test_storage_digest(world->disk_b);
	g_assert_cmpstr(before, ==, after);

	/* Without a marker but with files: a mismatch, never adopted. */
	{
		g_autofree gchar *marker = g_build_filename(world->disk_b, AXIS_VOLUME_MARKER_NAME, NULL);

		g_assert_cmpint(g_unlink(marker), ==, 0);
	}
	assert_state(world, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NO_MARKER);

	world_free(world);
}

static void
test_other_disk(void)
{
	in_namespace(other_disk);
}

/* Spec: Read-only mount */
static void
read_only_mount(void)
{
	World *world = world_new();
	g_autofree gchar *fresh = g_build_filename(world->dir, "fresh", NULL);
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 second;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	id = add_volume(world, world->mnt, "Disk", TRUE);
	set_read_only(world, FALSE);

	/* Mounted read-only: reading yes, writing no, whatever the setting. */
	remount(world->mnt, TRUE);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);
	reader = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reader);
	writer = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_null(writer);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);

	/* Read-write again: both. */
	remount(world->mnt, FALSE);
	assert_state(world, id, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);
	writer = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(writer);
	/* A write guard held across a read-only remount fails its check. */
	remount(world->mnt, TRUE);
	g_assert_false(axis_volume_guard_check(writer, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);
	g_assert_true(axis_volume_guard_check(reader, &error));
	g_assert_no_error(error);

	/* A read-only filesystem with no marker is added without one (the
	 * identify plan, tests below); with one, it is adopted as it is. */
	axis_test_storage_mkdir(fresh);
	put_file(world->disk_b, "film.mkv", "frames");
	mount_bind(world->disk_b, fresh);
	g_ptr_array_add(world->mounts, g_strdup(fresh));
	remount(fresh, TRUE);
	g_assert_true(axis_storage_inspect_volume(world->storage, fresh, &inspection, &error));
	g_assert_cmpint(inspection->plan, !=, AXIS_VOLUME_PLAN_WRITE_MARKER);
	remount(fresh, FALSE);
	axis_test_storage_write_marker(fresh, OTHER_UUID);
	remount(fresh, TRUE);
	second = add_volume(world, fresh, "Archive", TRUE);
	assert_state(world, second, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);

	world_free(world);
}

static void
test_read_only_mount(void)
{
	in_namespace(read_only_mount);
}

/* Spec: Guard fails mid-operation (a lazy unmount under a held guard);
 * Path escaping the volume refused (onto another mount) */
static void
lazy_unmount_and_crossing(void)
{
	World *world = world_new();
	g_autofree gchar *inner = g_build_filename(world->mnt, "inner", NULL);
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint fd;

	put_file(world->disk_a, "film.mkv", "frames");
	axis_test_storage_mkdir(world->disk_a);
	{
		g_autofree gchar *inner_on_a = g_build_filename(world->disk_a, "inner", NULL);

		axis_test_storage_mkdir(inner_on_a);
	}
	mount_bind(world->disk_a, world->mnt);
	id = add_volume(world, world->mnt, "Disk", TRUE);

	/* Another filesystem mounted inside the volume is not part of it. */
	mount_bind(world->disk_b, inner);
	g_assert_false(axis_storage_add_root(world->storage, id, "inner", AXIS_MEDIA_MOVIES, NULL, NULL,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_clear_error(&error);
	unmount(inner, 0);
	g_assert_true(axis_storage_add_root(world->storage, id, "inner", AXIS_MEDIA_MOVIES, NULL, NULL,
		&error));
	g_assert_no_error(error);

	/* A guard is held while the disk is detached lazily: its descriptor
	 * still reads the old filesystem, but the check finds the path leads
	 * to the empty mount point. */
	guard = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	unmount(world->mnt, MNT_DETACH);
	fd = openat(axis_volume_guard_get_fd(guard), "film.mkv", O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	g_assert_false(axis_volume_guard_check(guard, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	g_clear_pointer(&guard, axis_volume_guard_release);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_NOT_MOUNTED);

	world_free(world);
}

static void
test_lazy_unmount_and_crossing(void)
{
	in_namespace(lazy_unmount_and_crossing);
}

/*
 * Storage hunt, H2: a write guard's check proves what the volume's check
 * proves, the mount included (as axis-volume-proof.c does).  The disk is
 * unmounted lazily under a held write guard and the same filesystem mounted
 * at the path again, read-only: the volume reads read-only, the guard's
 * check fails, and nothing is written through it onto the detached mount.
 */
static void
guard_over_read_only_mount(void)
{
	World *world = world_new();
	g_autofree gchar *stash = g_build_filename(world->dir, "stash", NULL);
	g_autofree gchar *written = g_build_filename(world->disk_a, "Written", NULL);
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint fd;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	id = add_volume(world, world->mnt, "Disk", TRUE);
	set_read_only(world, FALSE);
	guard = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(guard);
	g_assert_true(axis_volume_guard_check(guard, &error));
	g_assert_no_error(error);

	/* Unmounted lazily (the guard keeps it busy), and the same filesystem
	 * mounted at the path again, read-only.  The stash keeps it reachable. */
	axis_test_storage_mkdir(stash);
	mount_bind(world->mnt, stash);
	g_ptr_array_add(world->mounts, g_strdup(stash));
	unmount(world->mnt, MNT_DETACH);
	mount_bind(stash, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);

	/* The volume, checked now: read-only.  The guard taken before: its
	 * check fails, and so does every write through it. */
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);
	g_assert_false(axis_volume_guard_check(guard, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	fd = axis_volume_write_mkdir(guard, axis_volume_guard_get_fd(guard), "Written", 0755, NULL, NULL, &error);
	g_assert_cmpint(fd, <, 0);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(g_file_test(written, G_FILE_TEST_EXISTS));

	/* Asked for now: a read guard, no write guard. */
	g_assert_null(axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);
	reader = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	g_assert_true(axis_volume_guard_check(reader, &error));
	g_assert_no_error(error);
	g_clear_pointer(&guard, axis_volume_guard_release);
	g_clear_pointer(&reader, axis_volume_guard_release);

	world_free(world);
}

static void
test_guard_over_read_only_mount(void)
{
	in_namespace(guard_over_read_only_mount);
}

/* ── A disk mounted read-only without a marker ──────────────────── */

/*
 * These stand for a container's ":ro" bind mount of a host disk: a tmpfs
 * (the disk) bind-mounted read-only at the volume's path.  tmpfs reports an
 * identifier of its own (from a UUID it makes at each mount) on Linux 6.6
 * and later; where it reports none, a disk without a marker cannot be told
 * from another and the tests skip.
 */

/* Whether a tmpfs mounted in a new namespace reports an identifier of its
 * own, as these tests need. */
static gboolean
tmpfs_has_identifier(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("axis-fsid-XXXXXX", NULL);
	gint status = 0;
	pid_t pid;

	if (dir == NULL)
		return FALSE;
	/* The child makes system calls only: nothing that could need a lock
	 * another thread of this process held at the fork. */
	pid = fork();
	if (pid == 0) {
		struct statfs fs;
		gint ok;

		if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0 ||
		    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
		    mount("axis-test", dir, "tmpfs", 0, "size=1m") != 0)
			_exit(1);
		ok = statfs(dir, &fs) == 0 && (fs.f_fsid.__val[0] != 0 || fs.f_fsid.__val[1] != 0);
		_exit(ok ? 0 : 1);
	}
	if (pid > 0 && waitpid(pid, &status, 0) != pid)
		status = 1;
	g_rmdir(dir);
	return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void
in_namespace_identified(void (*body)(void))
{
	if (!g_test_subprocess() && namespaces_available() && !tmpfs_has_identifier()) {
		g_test_skip("tmpfs reports no filesystem identifier on this kernel (Linux 6.6 or later does)");
		return;
	}
	in_namespace(body);
}

/* Adds @path, which must be a read-only filesystem without a marker, by the
 * identify plan; @mount_point: whether it is one. */
static gint64
add_identified(
	World       *world,
	const gchar *path,
	const gchar *name,
	gboolean     mount_point
){
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	g_assert_true(axis_storage_inspect_volume(world->storage, path, &inspection, &error));
	g_assert_no_error(error);
	if (inspection->plan != AXIS_VOLUME_PLAN_IDENTIFY)
		g_error("cannot add %s without a marker: plan %s, %s", path, axis_volume_plan_to_string(inspection->plan),
			inspection->refusal != NULL ? inspection->refusal : "");
	g_assert_true(inspection->read_only);
	g_assert_null(inspection->marker_uuid);
	g_assert_cmpint(inspection->mount_root, ==, mount_point);

	memset(&request, 0, sizeof(request));
	request.path = inspection->path;
	request.name = name;
	request.plan = inspection->plan;
	request.dev = inspection->dev;
	request.ino = inspection->ino;
	request.fsid = inspection->fsid;
	request.total_bytes = inspection->total_bytes;
	request.not_mount_point_ok = !mount_point;
	g_assert_true(axis_storage_add_volume(world->storage, &request, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gboolean
has_marker_file(const gchar *dir)
{
	g_autofree gchar *path = g_build_filename(dir, AXIS_VOLUME_MARKER_NAME, NULL);

	return g_file_test(path, G_FILE_TEST_EXISTS);
}

/* The UUID in the marker at the top of @dir (NULL when there is none). */
static gchar *
marker_uuid_in(const gchar *dir)
{
	AxisVolumeMarker marker;
	gint fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	AxisVolumeMarkerStatus status;

	g_assert_cmpint(fd, >=, 0);
	status = axis_volume_marker_read(fd, &marker, NULL);
	close(fd);
	return status == AXIS_VOLUME_MARKER_FOUND ? g_strdup(marker.uuid) : NULL;
}

static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The names in @dir, sorted, one string. */
static gchar *
names_in(const gchar *dir)
{
	g_autoptr(GDir) d = g_dir_open(dir, 0, NULL);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	const gchar *name;

	g_assert_nonnull(d);
	while ((name = g_dir_read_name(d)) != NULL)
		g_ptr_array_add(names, g_strdup(name));
	g_ptr_array_sort(names, compare_names);
	g_ptr_array_add(names, NULL);
	return g_strjoinv(" ", (gchar **)names->pdata);
}

/* Fills the filesystem of @dir, so that no file can be written there. */
static void
fill_up(const gchar *dir)
{
	g_autofree gchar *path = g_build_filename(dir, "filler.bin", NULL);
	g_autofree gchar *chunk = g_malloc0(65536);
	gint fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

	g_assert_cmpint(fd, >=, 0);
	for (;;) {
		ssize_t n = write(fd, chunk, 65536);

		if (n < 0) {
			g_assert_cmpint(errno, ==, ENOSPC);
			break;
		}
		if (n < 65536)
			continue;
	}
	close(fd);
}

/* Spec: A read-only disk without a marker is added */
static void
read_only_add(void)
{
	World *world = world_new();
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeGuard) reader = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	struct stat top;
	gint64 id;
	gint fd;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);
	before = axis_test_storage_digest(world->disk_a);
	g_assert_cmpint(stat(world->disk_a, &top), ==, 0);

	id = add_identified(world, world->mnt, "Disk", TRUE);
	info = check(world,id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_READ_ONLY);
	g_assert_cmpint(info->reason, ==, AXIS_VOLUME_REASON_NONE);
	g_assert_false(info->has_marker);
	g_assert_cmpint(info->marker_created_us, ==, 0);
	g_assert_cmpstr(axis_storage_fs_type_name(info->fs_identity.magic), ==, "tmpfs");
	g_assert_cmpuint(info->fs_identity.ino, ==, (guint64)top.st_ino);
	g_assert_cmpuint(info->fs_identity.bytes, ==, 4 * 1024 * 1024);
	g_assert_cmpstr(info->fs_identity.fsid, !=, "0000000000000000");

	/* Read: yes, through a guard, as a scan does. */
	reader = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reader);
	fd = openat(axis_volume_guard_get_fd(reader), "film.mkv", O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	g_assert_true(axis_volume_guard_check(reader, &error));
	g_assert_no_error(error);

	/* Write: no, whatever the setting, while it is mounted read-only. */
	set_read_only(world, FALSE);
	writer = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_null(writer);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	g_clear_error(&error);

	/* Nothing was written to find this out, the marker included. */
	g_assert_false(has_marker_file(world->disk_a));
	after = axis_test_storage_digest(world->disk_a);
	g_assert_cmpstr(before, ==, after);

	world_free(world);
}

static void
test_read_only_add(void)
{
	in_namespace_identified(read_only_add);
}

/* Spec: Recognised after a restart and a remount */
static void
restart_and_remount(void)
{
	World *world = world_new();
	g_autofree gchar *elsewhere = g_build_filename(world->dir, "elsewhere", NULL);
	g_autoptr(GError) error = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	remount(world->mnt, TRUE);
	id = add_identified(world, world->mnt, "Disk", TRUE);

	/* The daemon restarts: what it knows comes from the database. */
	axis_storage_stop(world->storage);
	axis_storage_unref(world->storage);
	world->storage = axis_storage_new(world->pool, world->settings);
	g_assert_true(axis_storage_load(world->storage, &error));
	g_assert_no_error(error);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);

	/* The container is made again: the bind mount goes and comes back, of
	 * the same disk. */
	unmount(world->mnt, 0);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_NOT_MOUNTED);
	mount_bind(world->disk_a, world->mnt);
	remount(world->mnt, TRUE);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);

	/* Mounted at another path: re-pointed there by what it is. */
	axis_test_storage_mkdir(elsewhere);
	mount_bind(world->disk_a, elsewhere);
	g_ptr_array_add(world->mounts, g_strdup(elsewhere));
	remount(elsewhere, TRUE);
	unmount(world->mnt, 0);
	g_assert_true(axis_storage_relocate_volume(world->storage, id, elsewhere, FALSE, &error));
	g_assert_no_error(error);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);
	g_assert_false(has_marker_file(world->disk_a));

	world_free(world);
}

static void
test_restart_and_remount(void)
{
	in_namespace_identified(restart_and_remount);
}

/* Spec: Another filesystem at the same path; another directory of the
 * same filesystem */
static void
other_filesystem(void)
{
	World *world = world_new();
	g_autofree gchar *sub = g_build_filename(world->disk_a, "Other", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	remount(world->mnt, TRUE);
	id = add_identified(world, world->mnt, "Disk", TRUE);
	unmount(world->mnt, 0);

	/* Another disk at the same path, with the same files: not this one. */
	put_file(world->disk_b, "film.mkv", "frames");
	mount_bind(world->disk_b, world->mnt);
	remount(world->mnt, TRUE);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_OTHER_FILESYSTEM);
	guard = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, &error);
	g_assert_null(guard);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);

	/* Mounted read-write, it is still not the volume, and its marker is
	 * never written there, whatever asks. */
	remount(world->mnt, FALSE);
	set_read_only(world, FALSE);
	before = axis_test_storage_digest(world->disk_b);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_OTHER_FILESYSTEM);
	guard = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_null(guard);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_clear_error(&error);
	after = axis_test_storage_digest(world->disk_b);
	g_assert_cmpstr(before, ==, after);
	g_assert_false(has_marker_file(world->disk_b));
	unmount(world->mnt, 0);

	/* Another directory of the volume's own filesystem: its inode differs. */
	axis_test_storage_mkdir(sub);
	put_file(sub, "film.mkv", "frames");
	mount_bind(sub, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_OTHER_FILESYSTEM);
	unmount(world->mnt, 0);

	/* Its own again: back, with nothing recorded about it changed. */
	mount_bind(world->disk_a, world->mnt);
	remount(world->mnt, TRUE);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);

	world_free(world);
}

static void
test_other_filesystem(void)
{
	in_namespace_identified(other_filesystem);
}

/* Spec: An empty directory never matches */
static void
empty_directory(void)
{
	World *world = world_new();
	g_autofree gchar *film = g_build_filename(world->disk_a, "film.mkv", NULL);
	g_autofree gchar *films = g_build_filename(world->disk_b, "Films", NULL);
	g_autofree gchar *films_film = g_build_filename(films, "b.mkv", NULL);
	g_autofree gchar *second = g_build_filename(world->dir, "second", NULL);
	g_autofree gchar *second_films = g_build_filename(second, "Films", NULL);
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 inner;

	/* An empty disk mounted read-only is not added without a marker. */
	mount_bind(world->disk_a, world->mnt);
	remount(world->mnt, TRUE);
	g_assert_true(axis_storage_inspect_volume(world->storage, world->mnt, &inspection, &error));
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_REFUSE);
	g_assert_nonnull(strstr(inspection->refusal, "empty"));
	unmount(world->mnt, 0);

	/* Added with a file, then the mount point left empty: offline. */
	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);
	id = add_identified(world, world->mnt, "Disk", TRUE);
	/* The same filesystem and directory with nothing in it (as a bind
	 * mount of a host's empty mount point would be): never the volume. */
	g_assert_cmpint(g_unlink(film), ==, 0);
	assert_state(world, id, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_EMPTY);
	g_assert_null(axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_READ, NULL));
	put_file(world->disk_a, "film.mkv", "frames");
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);

	/* A directory that is not a mount point, emptied: offline too. */
	axis_test_storage_mkdir(films);
	axis_test_storage_write(films_film, "frames");
	axis_test_storage_mkdir(second);
	mount_bind(world->disk_b, second);
	g_ptr_array_add(world->mounts, g_strdup(second));
	remount(second, TRUE);
	inner = add_identified(world, second_films, "Films", FALSE);
	assert_state(world, inner, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);
	g_assert_cmpint(g_unlink(films_film), ==, 0);
	assert_state(world, inner, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_EMPTY);
	unmount(second, 0);
	assert_state(world, inner, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_PATH_MISSING);

	world_free(world);
}

static void
test_empty_directory(void)
{
	in_namespace_identified(empty_directory);
}

/* Spec: The marker is written when the disk is writable, and takes over */
static void
marker_when_writable(void)
{
	World *world = world_new();
	g_autofree gchar *second = g_build_filename(world->dir, "second", NULL);
	g_autofree gchar *film = g_build_filename(world->disk_a, "film.mkv", NULL);
	g_autofree gchar *marker = g_build_filename(world->disk_a, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *written = NULL;
	g_autofree gchar *content = NULL;
	g_autofree gchar *names = NULL;
	g_autofree gchar *uuid = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;
	gint64 other;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);
	id = add_identified(world, world->mnt, "Disk", TRUE);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);
	g_assert_false(has_marker_file(world->disk_a));

	/* Mounted read-write, storage still read-only (the default): the next
	 * check writes the marker, the one write that setting allows. */
	g_assert_true(axis_storage_is_read_only_mode(world->storage));
	remount(world->mnt, FALSE);
	info = check(world,id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_true(info->has_marker);
	g_assert_cmpint(info->marker_created_us, >, 0);
	g_assert_null(info->marker_problem);
	uuid = marker_uuid_in(world->disk_a);
	g_assert_cmpstr(uuid, ==, info->uuid);
	/* Only the marker: the film as it was, no temporary file left. */
	names = names_in(world->disk_a);
	g_assert_cmpstr(names, ==, ".axis-volume film.mkv");
	content = axis_test_storage_read(film);
	g_assert_cmpstr(content, ==, "frames");
	g_assert_cmpuint(axis_test_storage_count_temporaries(world->disk_a), ==, 0);

	/* From now on the marker identifies it, as any volume: read-only
	 * again, recognised by it; it survives a restart; without it, the
	 * directory is a mismatch, never recognised by its filesystem again. */
	remount(world->mnt, TRUE);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);
	axis_storage_stop(world->storage);
	axis_storage_unref(world->storage);
	world->storage = axis_storage_new(world->pool, world->settings);
	g_assert_true(axis_storage_load(world->storage, &error));
	g_assert_no_error(error);
	g_clear_pointer(&info, axis_volume_info_free);
	info = check(world,id);
	g_assert_true(info->has_marker);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_READ_ONLY);
	written = axis_test_storage_read(marker);
	g_assert_cmpint(g_unlink(marker), ==, 0);
	assert_state(world, id, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NO_MARKER);
	axis_test_storage_write(marker, written);
	assert_state(world, id, AXIS_VOLUME_READ_ONLY, AXIS_VOLUME_REASON_NONE);

	/* A write guard asked for first, before any check: the marker is
	 * written before the guard is given, through the directory it holds. */
	put_file(world->disk_b, "film.mkv", "frames");
	axis_test_storage_mkdir(second);
	mount_bind(world->disk_b, second);
	g_ptr_array_add(world->mounts, g_strdup(second));
	remount(second, TRUE);
	other = add_identified(world, second, "Second", TRUE);
	set_read_only(world, FALSE);
	remount(second, FALSE);
	g_assert_false(has_marker_file(world->disk_b));
	writer = axis_volume_guard_acquire(world->storage, other, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(writer);
	g_free(uuid);
	uuid = marker_uuid_in(world->disk_b);
	g_clear_pointer(&info, axis_volume_info_free);
	info = axis_storage_get_volume(world->storage, other);
	g_assert_true(info->has_marker);
	g_assert_cmpstr(uuid, ==, info->uuid);
	g_assert_true(axis_volume_guard_writable(writer, &error));
	g_assert_true(axis_volume_guard_check(writer, &error));
	g_assert_no_error(error);

	world_free(world);
}

static void
test_marker_when_writable(void)
{
	in_namespace_identified(marker_when_writable);
}

/* Spec: Nothing is written to a volume before its marker */
static void
no_write_without_marker(void)
{
	World *world = world_new();
	g_autofree gchar *filler = g_build_filename(world->disk_a, "filler.bin", NULL);
	g_autofree gchar *film = g_build_filename(world->disk_a, "film.mkv", NULL);
	g_autofree gchar *names = NULL;
	g_autofree gchar *content = NULL;
	g_autofree gchar *uuid = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeGuard) writer = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*its marker could not be written*");
	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);
	id = add_identified(world, world->mnt, "Disk", TRUE);

	/* Read-write, but full: the marker cannot be written. */
	fill_up(world->disk_a);
	remount(world->mnt, FALSE);
	info = check(world,id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_false(info->has_marker);
	g_assert_nonnull(info->marker_problem);
	g_assert_nonnull(strstr(info->marker_problem, g_strerror(ENOSPC)));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*its marker could not be written*"), ==, 1);
	/* Said once, not at every check. */
	g_clear_pointer(&info, axis_volume_info_free);
	info = check(world,id);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*its marker could not be written*"), ==, 1);

	/* So no write guard is given, storage writable or not: nothing is
	 * written to it before its marker. */
	set_read_only(world, FALSE);
	writer = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_null(writer);
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_UNAVAILABLE);
	g_assert_nonnull(strstr(error->message, "no marker yet"));
	g_clear_error(&error);
	names = names_in(world->disk_a);
	g_assert_cmpstr(names, ==, "filler.bin film.mkv");
	content = axis_test_storage_read(film);
	g_assert_cmpstr(content, ==, "frames");

	/* Room again: the write guard writes the marker first, then is given. */
	g_assert_cmpint(g_unlink(filler), ==, 0);
	writer = axis_volume_guard_acquire(world->storage, id, AXIS_VOLUME_INTENT_WRITE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(writer);
	uuid = marker_uuid_in(world->disk_a);
	g_clear_pointer(&info, axis_volume_info_free);
	info = axis_storage_get_volume(world->storage, id);
	g_assert_true(info->has_marker);
	g_assert_null(info->marker_problem);
	g_assert_cmpstr(uuid, ==, info->uuid);

	world_free(world);
}

static void
test_no_write_without_marker(void)
{
	in_namespace_identified(no_write_without_marker);
}

/* ── One filesystem, one volume ─────────────────────────────────── */

/* The request an administrator's confirmation of @inspection posts. */
static void
request_of(
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

/*
 * Storage hunt, H3 (its demonstration, now the rule): a directory on the
 * filesystem of a volume is refused as another volume whether that volume
 * is online or not, when what is recorded of it proves the filesystem the
 * same (here its identifier, which tmpfs makes its own; never a device
 * number alone).  While the first volume's disk is away from its mount
 * point, a folder of that filesystem mounted elsewhere is refused, and
 * posting the confirmation anyway adds nothing and writes nothing.
 */
static void
same_filesystem_while_offline(void)
{
	World *world = world_new();
	g_autofree gchar *movies = g_build_filename(world->disk_a, "Movies", NULL);
	g_autofree gchar *other = g_build_filename(world->dir, "other", NULL);
	g_autofree gchar *count = NULL;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 first;
	gint64 second = 0;

	axis_test_storage_mkdir(movies);
	put_file(movies, "Film (2006).mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	first = add_volume(world, world->mnt, "First", TRUE);

	/* The disk leaves its mount point. */
	unmount(world->mnt, 0);
	assert_state(world, first, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_NOT_MOUNTED);

	/* A folder of the same filesystem, mounted at another path. */
	axis_test_storage_mkdir(other);
	mount_bind(movies, other);
	g_ptr_array_add(world->mounts, g_strdup(other));
	g_assert_true(axis_storage_inspect_volume(world->storage, other, &inspection, &error));
	g_assert_no_error(error);
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_REFUSE);
	g_assert_nonnull(strstr(inspection->refusal, "disk 'First'"));
	g_assert_nonnull(strstr(inspection->refusal, "not online now"));

	/* The confirmation posted anyway, with what a look would have shown. */
	request_of(&request, inspection, "Second");
	request.plan = AXIS_VOLUME_PLAN_WRITE_MARKER;
	g_assert_false(axis_storage_add_volume(world->storage, &request, &second, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(has_marker_file(movies));
	count = axis_test_db_query(world->db, "SELECT count(*) FROM volume");
	g_assert_cmpstr(count, ==, "1");

	/* The disk back: the one volume. */
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	assert_state(world, first, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);

	world_free(world);
}

static void
test_same_filesystem_while_offline(void)
{
	in_namespace_identified(same_filesystem_while_offline);
}

/*
 * Storage hunt, H3: restoring a volume's marker runs the same check as
 * adding one.  The first volume's marker is gone and a folder of the second
 * volume's filesystem is mounted at its path (files, no marker): writing the
 * first volume's marker there would make it online over the second's files.
 * Refused, the second online or not, and nothing written.
 */
static void
restore_onto_another_volume(void)
{
	World *world = world_new();
	g_autofree gchar *second_path = g_build_filename(world->dir, "second", NULL);
	g_autofree gchar *films = g_build_filename(world->disk_b, "Films", NULL);
	g_autofree gchar *marker = g_build_filename(world->disk_a, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autoptr(AxisMarkerRestoreInspection) look = NULL;
	g_autoptr(GError) error = NULL;
	gint64 first;
	gint64 second;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	first = add_volume(world, world->mnt, "First", TRUE);
	axis_test_storage_mkdir(films);
	put_file(films, "Film (2007).mkv", "other frames");
	axis_test_storage_mkdir(second_path);
	mount_bind(world->disk_b, second_path);
	second = add_volume(world, second_path, "Second", TRUE);
	set_read_only(world, FALSE);

	/* The first volume's marker deleted, its disk gone, and a folder of the
	 * second's filesystem mounted at its path. */
	g_assert_cmpint(g_unlink(marker), ==, 0);
	unmount(world->mnt, 0);
	mount_bind(films, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	assert_state(world, first, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NO_MARKER);
	assert_state(world, second, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);
	g_assert_true(axis_storage_inspect_marker_restore(world->storage, first, &look, &error));
	g_assert_no_error(error);
	g_assert_false(axis_storage_restore_marker(world->storage, first, look->dev, look->ino, look->fsid, TRUE,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "disk 'Second'"));
	g_clear_error(&error);
	g_assert_false(has_marker_file(films));

	/* The second volume offline: its filesystem is still proven the same. */
	unmount(second_path, 0);
	assert_state(world, second, AXIS_VOLUME_OFFLINE, AXIS_VOLUME_REASON_NOT_MOUNTED);
	g_assert_false(axis_storage_restore_marker(world->storage, first, look->dev, look->ino, look->fsid, TRUE,
		&error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_clear_error(&error);
	g_assert_false(has_marker_file(films));
	assert_state(world, first, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_NO_MARKER);

	world_free(world);
}

static void
test_restore_onto_another_volume(void)
{
	in_namespace_identified(restore_onto_another_volume);
}

/*
 * The restore page's claim: the filesystem at the volume's path is compared
 * with the one the volume was last seen on.  The first volume's marker is
 * gone and another disk with the same folders is at its path: the
 * inspection says the filesystem differs, and the marker is written only
 * when that is confirmed.
 */
static void
restore_on_another_filesystem(void)
{
	World *world = world_new();
	g_autofree gchar *marker = g_build_filename(world->disk_a, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autoptr(AxisMarkerRestoreInspection) look = NULL;
	g_autoptr(AxisMarkerRestoreInspection) again = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	id = add_volume(world, world->mnt, "Disk", TRUE);
	set_read_only(world, FALSE);

	/* Its own disk without its marker: the same filesystem. */
	g_assert_cmpint(g_unlink(marker), ==, 0);
	g_assert_true(axis_storage_inspect_marker_restore(world->storage, id, &look, &error));
	g_assert_no_error(error);
	g_assert_true(look->possible);
	g_assert_cmpint(look->filesystem, ==, AXIS_MARKER_RESTORE_FS_SAME);
	g_clear_pointer(&look, axis_marker_restore_inspection_free);

	/* Another disk with the same files at its path: it differs, and the
	 * marker is not written unless that is confirmed. */
	unmount(world->mnt, 0);
	put_file(world->disk_b, "film.mkv", "frames");
	mount_bind(world->disk_b, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	g_assert_true(axis_storage_inspect_marker_restore(world->storage, id, &look, &error));
	g_assert_no_error(error);
	g_assert_true(look->possible);
	g_assert_cmpint(look->filesystem, ==, AXIS_MARKER_RESTORE_FS_DIFFERS);
	g_assert_nonnull(look->filesystem_text);
	g_assert_nonnull(strstr(look->filesystem_text, "not the one"));
	g_assert_false(axis_storage_restore_marker(world->storage, id, look->dev, look->ino, look->fsid, FALSE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_CONFIRM);
	g_clear_error(&error);
	g_assert_false(has_marker_file(world->disk_b));
	g_assert_true(axis_storage_restore_marker(world->storage, id, look->dev, look->ino, look->fsid, TRUE, &error));
	g_assert_no_error(error);
	g_assert_true(has_marker_file(world->disk_b));
	assert_state(world, id, AXIS_VOLUME_ONLINE, AXIS_VOLUME_REASON_NONE);

	world_free(world);
}

static void
test_restore_on_another_filesystem(void)
{
	in_namespace_identified(restore_on_another_filesystem);
}

/*
 * Two marked disks swapped between their mount points: each volume is a
 * mismatch at its path, with the other's marker.  Re-pointing one to the
 * other's path re-points both, in one change (never two volumes at one
 * path); without the other's disk proven at this one's path, it is refused
 * and nothing changes.
 */
static void
swapped_disks(void)
{
	World *world = world_new();
	g_autofree gchar *path_b = g_build_filename(world->dir, "mnt-b", NULL);
	g_autoptr(AxisVolumeInfo) a_after = NULL;
	g_autoptr(AxisVolumeInfo) b_after = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a;
	gint64 b;

	put_file(world->disk_a, "a.mkv", "frames of a");
	put_file(world->disk_b, "b.mkv", "frames of b");
	axis_test_storage_mkdir(path_b);
	mount_bind(world->disk_a, world->mnt);
	mount_bind(world->disk_b, path_b);
	a = add_volume(world, world->mnt, "A", TRUE);
	b = add_volume(world, path_b, "B", TRUE);

	/* Swapped. */
	unmount(world->mnt, 0);
	unmount(path_b, 0);
	mount_bind(world->disk_b, world->mnt);
	mount_bind(world->disk_a, path_b);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	g_ptr_array_add(world->mounts, g_strdup(path_b));
	assert_state(world, a, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_OTHER_MARKER);
	assert_state(world, b, AXIS_VOLUME_MISMATCH, AXIS_VOLUME_REASON_OTHER_MARKER);

	/* A pointed at B's path: both re-pointed. */
	g_assert_true(axis_storage_relocate_volume(world->storage, a, path_b, FALSE, &error));
	g_assert_no_error(error);
	a_after = check(world, a);
	b_after = check(world, b);
	g_assert_cmpstr(a_after->path, ==, path_b);
	g_assert_cmpstr(b_after->path, ==, world->mnt);
	g_assert_cmpint(a_after->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_cmpint(b_after->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*'B'*now looked for at*"), ==, 1);

	/* Swapped again, B's disk then taken away: A's re-point onto B's path
	 * is refused (B is not proven at A's path), and nothing changes. */
	unmount(world->mnt, 0);
	unmount(path_b, 0);
	mount_bind(world->disk_a, world->mnt);
	g_assert_false(axis_storage_relocate_volume(world->storage, a, world->mnt, FALSE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "disk 'B'"));
	g_clear_error(&error);
	g_clear_pointer(&a_after, axis_volume_info_free);
	g_clear_pointer(&b_after, axis_volume_info_free);
	a_after = axis_storage_get_volume(world->storage, a);
	b_after = axis_storage_get_volume(world->storage, b);
	g_assert_cmpstr(a_after->path, ==, path_b);
	g_assert_cmpstr(b_after->path, ==, world->mnt);

	world_free(world);
}

static void
test_swapped_disks(void)
{
	in_namespace(swapped_disks);
}

/*
 * Review of the fixes, item 4: a swap is decided on both volumes as the
 * database has them under the lock, never as this daemon remembered them,
 * and neither new path may hold or be inside a third volume.
 */
static void
swap_on_current_rows(void)
{
	World *world = world_new();
	g_autofree gchar *path_b = g_build_filename(world->dir, "mnt-b", NULL);
	g_autofree gchar *away = g_build_filename(world->dir, "away", NULL);
	g_autofree gchar *sql = NULL;
	g_autofree gchar *paths = NULL;
	g_autofree gchar *expected = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a;
	gint64 b;

	put_file(world->disk_a, "a.mkv", "frames of a");
	put_file(world->disk_b, "b.mkv", "frames of b");
	axis_test_storage_mkdir(path_b);
	mount_bind(world->disk_a, world->mnt);
	mount_bind(world->disk_b, path_b);
	a = add_volume(world, world->mnt, "A", TRUE);
	b = add_volume(world, path_b, "B", TRUE);
	g_assert_cmpint(b, >, a);
	unmount(world->mnt, 0);
	unmount(path_b, 0);
	mount_bind(world->disk_b, world->mnt);
	mount_bind(world->disk_a, path_b);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	g_ptr_array_add(world->mounts, g_strdup(path_b));

	/* Another daemon on the database pointed A elsewhere meanwhile; this one
	 * remembers A at its old path, where B's disk is. */
	sql = g_strdup_printf("UPDATE volume SET path = '%s' WHERE id = %" G_GINT64_FORMAT, away, a);
	axis_test_db_exec(world->db, sql);
	g_assert_false(axis_storage_relocate_volume(world->storage, a, path_b, FALSE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "disk 'B'"));
	g_clear_error(&error);
	paths = axis_test_db_query(world->db, "SELECT string_agg(path, '|' ORDER BY id) FROM volume");
	expected = g_strdup_printf("%s|%s", away, path_b);
	g_assert_cmpstr(paths, ==, expected);

	/* Back at its path, and a third volume recorded inside B's path: A
	 * would be around it. */
	g_free(sql);
	sql = g_strdup_printf("UPDATE volume SET path = '%s' WHERE id = %" G_GINT64_FORMAT, world->mnt, a);
	axis_test_db_exec(world->db, sql);
	g_free(sql);
	sql = g_strdup_printf("INSERT INTO volume (uuid, name, path, mount_root, state, marker_created_at) VALUES"
		" ('2f0c6b1e-9a8d-4c7b-8e6f-5d4c3b2a1908', 'C', '%s/inner', false, 'offline', now())", path_b);
	axis_test_db_exec(world->db, sql);
	g_assert_false(axis_storage_relocate_volume(world->storage, a, path_b, FALSE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "Disk 'C'"));
	g_clear_error(&error);
	g_clear_pointer(&paths, g_free);
	g_clear_pointer(&expected, g_free);
	paths = axis_test_db_query(world->db, "SELECT string_agg(path, '|' ORDER BY id) FROM volume");
	expected = g_strdup_printf("%s|%s|%s/inner", world->mnt, path_b, path_b);
	g_assert_cmpstr(paths, ==, expected);

	world_free(world);
}

static void
test_swap_on_current_rows(void)
{
	in_namespace(swap_on_current_rows);
}

/* The filesystem's UUID the kernel gives for @dir now ("" when none). */
static gchar *
uuid_of(const gchar *dir)
{
	gchar uuid[AXIS_STORAGE_FS_UUID_CHARS + 1];
	gint fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	g_assert_cmpint(fd, >=, 0);
	uuid[0] = '\0';
	axis_storage_fs_read_uuid(fd, uuid);
	close(fd);
	return g_strdup(uuid);
}

/*
 * Review of the fixes, item 1: the filesystem's UUID is recorded for every
 * volume, its marker or not, when it is added, at each check that finds
 * it, and when it is re-pointed: the evidence of which filesystem it is on
 * (where the identifier is only the device's number, the UUID is what
 * proves another directory on it).
 */
static void
uuid_recorded_for_marked_volume(void)
{
	World *world = world_new();
	g_autofree gchar *elsewhere = g_build_filename(world->dir, "elsewhere", NULL);
	g_autofree gchar *now = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *sql = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	id = add_volume(world, world->mnt, "Disk", TRUE);
	now = uuid_of(world->mnt);
	g_test_message("the kernel gives the test's tmpfs the UUID '%s'", now);
	sql = g_strdup_printf("SELECT coalesce(fs_uuid, '') FROM volume WHERE id = %" G_GINT64_FORMAT, id);
	stored = axis_test_db_query(world->db, sql);
	g_assert_cmpstr(stored, ==, now);

	/* A record of another filesystem (where the volume once was): the next
	 * check that finds it records the one it is on. */
	axis_test_db_exec(world->db, "UPDATE volume SET fs_uuid = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'");
	info = check(world, id);
	g_assert_cmpint(info->state, ==, AXIS_VOLUME_ONLINE);
	g_assert_cmpstr(info->fs_uuid != NULL ? info->fs_uuid : "", ==, now);
	g_free(stored);
	stored = axis_test_db_query(world->db, sql);
	g_assert_cmpstr(stored, ==, now);

	/* Re-pointed to the same disk mounted elsewhere: recorded again. */
	axis_test_db_exec(world->db, "UPDATE volume SET fs_uuid = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'");
	axis_test_storage_mkdir(elsewhere);
	mount_bind(world->disk_a, elsewhere);
	g_ptr_array_add(world->mounts, g_strdup(elsewhere));
	unmount(world->mnt, 0);
	g_assert_true(axis_storage_relocate_volume(world->storage, id, elsewhere, FALSE, NULL));
	g_free(stored);
	stored = axis_test_db_query(world->db, sql);
	g_assert_cmpstr(stored, ==, now);

	world_free(world);
}

static void
test_uuid_recorded_for_marked_volume(void)
{
	in_namespace(uuid_recorded_for_marked_volume);
}

/*
 * Review of the fixes, item 3: what a volume added without a marker was
 * first recognised by is not evidence once its marker is written and it
 * lives elsewhere.  The volume is moved to another disk (its marker with
 * it) and re-pointed there; its first disk is then free to be added as a
 * volume of its own.
 */
static void
moved_volume_frees_old_filesystem(void)
{
	World *world = world_new();
	g_autofree gchar *marker = g_build_filename(world->disk_a, AXIS_VOLUME_MARKER_NAME, NULL);
	g_autofree gchar *elsewhere = g_build_filename(world->dir, "elsewhere", NULL);
	g_autofree gchar *old = g_build_filename(world->dir, "old", NULL);
	g_autofree gchar *content = NULL;
	g_autoptr(AxisVolumeInfo) info = NULL;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	put_file(world->disk_a, "film.mkv", "frames");
	mount_bind(world->disk_a, world->mnt);
	g_ptr_array_add(world->mounts, g_strdup(world->mnt));
	remount(world->mnt, TRUE);
	id = add_identified(world, world->mnt, "Moved", TRUE);
	/* Mounted read-write: its marker is written. */
	remount(world->mnt, FALSE);
	info = check(world, id);
	g_assert_true(info->has_marker);

	/* Moved to another disk, its marker with it, and pointed there. */
	content = axis_test_storage_read(marker);
	put_file(world->disk_b, AXIS_VOLUME_MARKER_NAME, content);
	put_file(world->disk_b, "film.mkv", "frames");
	g_assert_cmpint(g_unlink(marker), ==, 0);
	unmount(world->mnt, 0);
	axis_test_storage_mkdir(elsewhere);
	mount_bind(world->disk_b, elsewhere);
	g_ptr_array_add(world->mounts, g_strdup(elsewhere));
	g_assert_true(axis_storage_relocate_volume(world->storage, id, elsewhere, FALSE, &error));
	g_assert_no_error(error);

	/* Its first disk, without the marker, is another volume's to be now. */
	axis_test_storage_mkdir(old);
	mount_bind(world->disk_a, old);
	g_ptr_array_add(world->mounts, g_strdup(old));
	g_assert_true(axis_storage_inspect_volume(world->storage, old, &inspection, &error));
	g_assert_no_error(error);
	if (inspection->plan == AXIS_VOLUME_PLAN_REFUSE)
		g_error("the first disk was refused: %s", inspection->refusal);
	g_assert_cmpint(inspection->plan, ==, AXIS_VOLUME_PLAN_WRITE_MARKER);

	world_free(world);
}

static void
test_moved_volume_frees_old_filesystem(void)
{
	in_namespace_identified(moved_volume_frees_old_filesystem);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/storage/mounts/unmount-and-return", test_unmount_and_return);
	g_test_add_func("/storage/mounts/other-disk", test_other_disk);
	g_test_add_func("/storage/mounts/read-only", test_read_only_mount);
	g_test_add_func("/storage/mounts/lazy-unmount-and-crossing", test_lazy_unmount_and_crossing);
	g_test_add_func("/storage/mounts/guard-over-read-only-mount", test_guard_over_read_only_mount);
	g_test_add_func("/storage/mounts/no-marker/read-only-add", test_read_only_add);
	g_test_add_func("/storage/mounts/no-marker/restart-and-remount", test_restart_and_remount);
	g_test_add_func("/storage/mounts/no-marker/other-filesystem", test_other_filesystem);
	g_test_add_func("/storage/mounts/no-marker/empty-directory", test_empty_directory);
	g_test_add_func("/storage/mounts/no-marker/marker-when-writable", test_marker_when_writable);
	g_test_add_func("/storage/mounts/no-marker/no-write-without-marker", test_no_write_without_marker);
	g_test_add_func("/storage/mounts/same-filesystem-while-offline", test_same_filesystem_while_offline);
	g_test_add_func("/storage/mounts/restore-onto-another-volume", test_restore_onto_another_volume);
	g_test_add_func("/storage/mounts/restore-on-another-filesystem", test_restore_on_another_filesystem);
	g_test_add_func("/storage/mounts/swapped-disks", test_swapped_disks);
	g_test_add_func("/storage/mounts/swap-on-current-rows", test_swap_on_current_rows);
	g_test_add_func("/storage/mounts/uuid-recorded-for-marked-volume", test_uuid_recorded_for_marked_volume);
	g_test_add_func("/storage/mounts/no-marker/moved-volume-frees-old-filesystem",
		test_moved_volume_frees_old_filesystem);

	return g_test_run();
}
