/*
 * test-library-mounts.c - the scanner on real mounts: another filesystem
 * inside a root, a bind mount that would make a loop, a mount appearing
 * in the middle of a scan, and the volume detached mid-scan
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * As in test-storage-mounts.c, each test runs in a child process that
 * enters a new user and mount namespace, where it may mount tmpfs
 * filesystems and bind mounts privately; nothing is mounted on the host.
 * Where unprivileged user namespaces are not available the tests skip.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "library/axis-library.h"

/* ── Entering a namespace (as test-storage-mounts.c) ────────────── */

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

static void
enter_namespace(void)
{
	g_autofree gchar *uid_map = g_strdup_printf("0 %u 1\n", (guint)getuid());
	g_autofree gchar *gid_map = g_strdup_printf("0 %u 1\n", (guint)getgid());

	if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0)
		g_error("prctl: %s", g_strerror(errno));
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
		g_error("unshare: %s", g_strerror(errno));
	write_proc("/proc/self/setgroups", "deny");
	write_proc("/proc/self/uid_map", uid_map);
	write_proc("/proc/self/gid_map", gid_map);
	g_assert_cmpint(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), ==, 0);
}

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

/* ── The world inside the child ─────────────────────────────────── */

typedef struct
{
	AxisTestLibrary *t;
	gchar           *disk;
	gchar           *films;
	gint64           volume;
	gint64           root;
	GPtrArray       *mounts;	/* to undo, last first */
	gboolean         detached;
	gint             calls;		/* atomic */
} World;

static void
mount_tmpfs(
	World       *world,
	const gchar *where
){
	if (mount("axis-test", where, "tmpfs", 0, "size=4m,mode=0755") != 0)
		g_error("mount tmpfs on %s: %s", where, g_strerror(errno));
	g_ptr_array_add(world->mounts, g_strdup(where));
}

static void
mount_bind(
	World       *world,
	const gchar *from,
	const gchar *to
){
	if (mount(from, to, NULL, MS_BIND, NULL) != 0)
		g_error("bind %s on %s: %s", from, to, g_strerror(errno));
	g_ptr_array_add(world->mounts, g_strdup(to));
}

/* @as_mount: the disk is a tmpfs of its own (a mount point). */
static World *
world_new(gboolean as_mount)
{
	World *world = g_new0(World, 1);

	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");

	world->mounts = g_ptr_array_new_with_free_func(g_free);
	world->t = axis_test_library_new(6);
	g_assert_nonnull(world->t);
	world->disk = g_build_filename(world->t->media, "disk", NULL);
	axis_test_storage_mkdir(world->disk);
	if (as_mount)
		mount_tmpfs(world, world->disk);
	{
		g_autofree gchar *readme = g_build_filename(world->disk, "readme.txt", NULL);

		axis_test_storage_write(readme, "this disk holds media\n");
	}
	world->volume = axis_test_library_add_volume(world->t, world->disk, "Disk");
	world->root = axis_test_library_add_root(world->t, world->volume, world->disk, "Films",
		AXIS_MEDIA_MOVIES);
	world->films = g_build_filename(world->disk, "Films", NULL);

	return world;
}

static void
world_free(World *world)
{
	guint i;

	/* Everything mounted, detached, before the scratch directory goes. */
	for (i = world->mounts->len; i > 0; i--)
		umount2(g_ptr_array_index(world->mounts, i - 1), MNT_DETACH);
	g_ptr_array_unref(world->mounts);
	axis_test_library_free(world->t);
	g_free(world->disk);
	g_free(world->films);
	g_free(world);
	axis_test_log_reset();
}

static void
put(
	const gchar *dir,
	const gchar *rel,
	const gchar *content
){
	g_autofree gchar *path = g_build_filename(dir, rel, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);

	axis_test_storage_mkdir(parent);
	axis_test_storage_write(path, content);
}

static gchar *
state_of(
	World       *world,
	const gchar *rel
){
	return axis_test_library_q(world->t, "SELECT state FROM library_file WHERE root_id = %" G_GINT64_FORMAT
		" AND rel_path = '%s'", world->root, rel);
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Spec: Other filesystem inside a root; a bind mount of the root inside
 * itself (a loop no symbolic link could make) */
static void
other_filesystem(void)
{
	World *world = world_new(FALSE);
	g_autofree gchar *extras = g_build_filename(world->disk, "Films/Extras", NULL);
	g_autofree gchar *loop = g_build_filename(world->disk, "Films/Loop", NULL);
	g_autofree gchar *findings = NULL;
	g_autofree gchar *state = NULL;
	AxisScanResult *result;

	put(world->films, "a.mkv", "a");
	put(world->films, "Extras/e.mkv", "under the mount point");
	axis_test_storage_mkdir(loop);
	axis_scan_result_free(axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));

	/* A disk mounted over a folder hides what is under it: not gone. */
	mount_tmpfs(world, extras);
	put(extras, "on-the-other-disk.mkv", "not ours");
	mount_bind(world, world->films, loop);

	result = axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	g_assert_cmpuint(result->walk.other_fs, ==, 2);
	g_assert_cmpuint(result->suspected, ==, 0);
	g_assert_cmpuint(result->walk.files, ==, 1);
	findings = axis_test_library_q(world->t, "SELECT string_agg(kind || ':' || rel_path, ','"
		" ORDER BY rel_path) FROM library_scan_finding WHERE scan_id = %" G_GINT64_FORMAT,
		result->scan_id);
	g_assert_cmpstr(findings, ==, "other_filesystem:Extras,other_filesystem:Loop");
	axis_scan_result_free(result);
	state = state_of(world, "Extras/e.mkv");
	g_assert_cmpstr(state, ==, "present");
	g_assert_cmpint(axis_test_library_n(world->t, "SELECT count(*) FROM library_file"
		" WHERE rel_path LIKE '%%other-disk%%' OR rel_path LIKE 'Loop/%%'"), ==, 0);

	world_free(world);
}

static void
test_other_filesystem(void)
{
	in_namespace(other_filesystem);
}

static void
mount_late(
	const gchar *path,
	gpointer     data
){
	World *world = data;
	g_autofree gchar *where = NULL;

	if (strcmp(path, "Late") != 0 || g_atomic_int_add(&world->calls, 1) != 0)
		return;
	/* Between the listing and the opening. */
	where = g_build_filename(world->films, "Late", NULL);
	mount_tmpfs(world, where);
}

/* A mount that appears between a folder being listed and being opened is
 * seen for what it is. */
static void
mount_mid_scan(void)
{
	World *world = world_new(FALSE);
	AxisLibraryTuning tuning;
	g_autofree gchar *state = NULL;
	g_autofree gchar *detail = NULL;
	AxisScanResult *result;

	put(world->films, "Late/l.mkv", "late");
	put(world->films, "b.mkv", "b");
	axis_scan_result_free(axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));

	memset(&tuning, 0, sizeof(tuning));
	tuning.before_directory = mount_late;
	tuning.hook_data = world;
	axis_test_library_tune(world->t, &tuning);
	result = axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	g_assert_cmpuint(result->walk.other_fs, ==, 1);
	g_assert_cmpuint(result->suspected, ==, 0);
	detail = axis_test_library_q(world->t, "SELECT detail FROM library_scan_finding WHERE scan_id = %"
		G_GINT64_FORMAT, result->scan_id);
	g_assert_cmpstr(detail, ==, "a mount appeared while it was walked");
	axis_scan_result_free(result);
	state = state_of(world, "Late/l.mkv");
	g_assert_cmpstr(state, ==, "present");

	world_free(world);
}

static void
test_mount_mid_scan(void)
{
	in_namespace(mount_mid_scan);
}

static void
detach_volume(
	const gchar *path,
	gpointer     data
){
	World *world = data;

	if (world->detached || g_atomic_int_add(&world->calls, 1) < 2)
		return;
	if (umount2(world->disk, MNT_DETACH) != 0)
		g_error("umount %s: %s", world->disk, g_strerror(errno));
	world->detached = TRUE;
}

/* Spec: Volume disappears mid-scan, for real: the disk is detached while
 * the walk holds its descriptors.  What the walk still reads is the disk,
 * but nothing is concluded. */
static void
detached_mid_scan(void)
{
	World *world = world_new(TRUE);
	AxisLibraryTuning tuning;
	g_autofree gchar *gone = NULL;
	AxisScanResult *result;
	guint i;

	for (i = 0; i < 6; i++) {
		g_autofree gchar *rel = g_strdup_printf("d%u/f.mkv", i);

		put(world->films, rel, "frames");
	}
	axis_scan_result_free(axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
	gone = g_build_filename(world->films, "d5/f.mkv", NULL);
	g_assert_cmpint(g_unlink(gone), ==, 0);

	memset(&tuning, 0, sizeof(tuning));
	tuning.batch_entries = 1;
	tuning.before_directory = detach_volume;
	tuning.hook_data = world;
	axis_test_library_tune(world->t, &tuning);
	result = axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_UNAVAILABLE,
		AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);
	g_assert_true(world->detached);
	g_assert_cmpuint(result->suspected, ==, 0);
	axis_scan_result_free(result);
	g_assert_cmpint(axis_test_library_n(world->t, "SELECT count(*) FROM library_file WHERE state <> "
		"'present'"), ==, 0);
	g_assert_cmpint(axis_test_library_n(world->t, "SELECT count(*) FROM library_file"), ==, 6);

	/* Still detached: not scanned at all. */
	result = axis_test_library_scan_expect(world->t, world->root, AXIS_SCAN_UNAVAILABLE,
		AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);
	axis_scan_result_free(result);

	world_free(world);
}

static void
test_detached_mid_scan(void)
{
	in_namespace(detached_mid_scan);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/library/mounts/other-filesystem", test_other_filesystem);
	g_test_add_func("/library/mounts/mount-mid-scan", test_mount_mid_scan);
	g_test_add_func("/library/mounts/detached-mid-scan", test_detached_mid_scan);

	return g_test_run();
}
