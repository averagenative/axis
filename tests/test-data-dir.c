/*
 * test-data-dir.c - the daemon's data directory (core/axis-data-dir.h)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (daemon-core): Created (absent with a writable parent: made,
 * mode 0700), Not writable (refused, naming the directory), and that it is
 * worked beneath by descriptor: once opened, renaming it or putting
 * something else at its path changes nothing for the daemon.
 */

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-storage.h"
#include "core/axis-data-dir.h"
#include "core/axis-error.h"

typedef struct
{
	gchar *dir;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	fx->dir = axis_test_storage_dir_new();
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_test_storage_dir_free(fx->dir);
}

static void
assert_refused(
	const gchar *path,
	const gchar *words
){
	g_autoptr(GError) error = NULL;
	AxisDataDir *data = axis_data_dir_open(path, &error);

	g_assert_null(data);
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR);
	/* The message names the directory, and says why. */
	g_assert_nonnull(strstr(error->message, path));
	if (strstr(error->message, words) == NULL)
		g_error("expected \"%s\" in: %s", words, error->message);
}

static void
test_created(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fx->dir, "data", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDataDir) dir = NULL;
	GStatBuf st;
	GDir *listing;

	dir = axis_data_dir_open(path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(dir);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_true(S_ISDIR(st.st_mode));
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	g_assert_cmpstr(axis_data_dir_get_path(dir), ==, path);
	/* The write check leaves nothing behind. */
	listing = g_dir_open(path, 0, NULL);
	g_assert_null(g_dir_read_name(listing));
	g_dir_close(listing);

	/* Opened again, it is the same directory. */
	g_clear_pointer(&dir, axis_data_dir_unref);
	dir = axis_data_dir_open(path, &error);
	g_assert_no_error(error);
}

static void
test_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *orphan = g_build_filename(fx->dir, "missing", "data", NULL);
	g_autofree gchar *file = g_build_filename(fx->dir, "file", NULL);
	g_autofree gchar *link = g_build_filename(fx->dir, "link", NULL);
	g_autofree gchar *target = g_build_filename(fx->dir, "target", NULL);
	g_autofree gchar *open_to_all = g_build_filename(fx->dir, "everyone", NULL);
	g_autofree gchar *locked = g_build_filename(fx->dir, "locked", NULL);

	assert_refused(orphan, "parent");
	g_assert_false(g_file_test(orphan, G_FILE_TEST_EXISTS));

	axis_test_storage_write(file, "not a directory\n");
	assert_refused(file, "not a directory");

	axis_test_storage_mkdir(target);
	g_assert_cmpint(symlink(target, link), ==, 0);
	assert_refused(link, "symbolic link");

	axis_test_storage_mkdir(open_to_all);
	g_assert_cmpint(g_chmod(open_to_all, 0777), ==, 0);
	assert_refused(open_to_all, "every user");

	if (geteuid() == 0) {
		g_test_message("running as root: the permission case is skipped");
		return;
	}
	/* Spec: Not writable */
	axis_test_storage_mkdir(locked);
	g_assert_cmpint(g_chmod(locked, 0500), ==, 0);
	assert_refused(locked, "cannot be written");
	g_assert_cmpint(g_chmod(locked, 0700), ==, 0);
}

/* Once opened, the directory is the descriptor, not the path. */
static void
test_by_descriptor(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fx->dir, "data", NULL);
	g_autofree gchar *moved = g_build_filename(fx->dir, "moved", NULL);
	g_autofree gchar *elsewhere = g_build_filename(fx->dir, "elsewhere", NULL);
	g_autofree gchar *in_moved = g_build_filename(moved, "artwork", "probe", NULL);
	g_autofree gchar *in_elsewhere = g_build_filename(elsewhere, "artwork", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDataDir) dir = axis_data_dir_open(path, &error);
	gint art;
	gint fd;

	g_assert_no_error(error);
	/* Something else is put where it was. */
	g_assert_cmpint(g_rename(path, moved), ==, 0);
	axis_test_storage_mkdir(elsewhere);
	g_assert_cmpint(symlink(elsewhere, path), ==, 0);

	art = axis_data_dir_subdir(dir, "artwork", &error);
	g_assert_no_error(error);
	g_assert_cmpint(art, >=, 0);
	g_assert_cmpint(axis_data_dir_subdir(dir, "artwork", NULL), ==, art);
	fd = openat(art, "probe", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	g_assert_cmpint(fd, >=, 0);
	close(fd);
	g_assert_true(g_file_test(in_moved, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(in_elsewhere, G_FILE_TEST_EXISTS));

	/* A name that is not one is refused, and nothing is made. */
	g_assert_cmpint(axis_data_dir_subdir(dir, "../escape", &error), ==, -1);
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR);
	g_clear_error(&error);
	g_assert_cmpint(axis_data_dir_subdir(dir, "Artwork", NULL), ==, -1);
	g_assert_cmpint(axis_data_dir_subdir(dir, "", NULL), ==, -1);
}

/* A subdirectory is checked as the directory is. */
static void
test_subdir_checked(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fx->dir, "data", NULL);
	g_autofree gchar *art = g_build_filename(path, "artwork", NULL);
	g_autofree gchar *target = g_build_filename(fx->dir, "target", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisDataDir) dir = axis_data_dir_open(path, &error);

	g_assert_no_error(error);
	axis_test_storage_mkdir(target);
	g_assert_cmpint(symlink(target, art), ==, 0);
	g_assert_cmpint(axis_data_dir_subdir(dir, "artwork", &error), ==, -1);
	g_assert_error(error, AXIS_ERROR, AXIS_ERROR_DATA_DIR);
	g_assert_nonnull(strstr(error->message, "symbolic link"));
}

/*
 * Spec (deployment): Arbitrary user -- an arbitrary user id with group 0
 * may use a directory that group 0 may write (the image's, and a new volume
 * made from it); a Kubernetes fsGroup is the same rule with a supplementary
 * group.  Ownership alone is not enough when everyone may write, and a group
 * that may not write gives nothing.
 */
static void
test_owner_rule(void)
{
	static const guint group_zero[] = { 0 };
	static const guint with_fs_group[] = { 1000650000, 2000 };
	static const guint plain[] = { 1000 };

	/* Its own user, as before. */
	g_assert_cmpint(axis_data_dir_owner_check(1000, 1000, S_IFDIR | 0700, 1000, plain, 1), ==, AXIS_DATA_DIR_OWN);
	g_assert_cmpint(axis_data_dir_owner_check(1000, 1000, S_IFDIR | 0750, 1000, plain, 1), ==, AXIS_DATA_DIR_OWN);
	/* The image's directory (1000:0, 0770) for an arbitrary id in group 0. */
	g_assert_cmpint(axis_data_dir_owner_check(1000, 0, S_IFDIR | 0770, 1000650000, group_zero, 1), ==,
		AXIS_DATA_DIR_GROUP);
	/* A Kubernetes volume given to the pod's fsGroup (setgid, 2770). */
	g_assert_cmpint(axis_data_dir_owner_check(0, 2000, S_IFDIR | 02770, 1000650000, with_fs_group, 2), ==,
		AXIS_DATA_DIR_GROUP);
	/* Group may read but not write, or not search: refused. */
	g_assert_cmpint(axis_data_dir_owner_check(1000, 0, S_IFDIR | 0750, 1000650000, group_zero, 1), ==,
		AXIS_DATA_DIR_OTHER);
	g_assert_cmpint(axis_data_dir_owner_check(1000, 0, S_IFDIR | 0760, 1000650000, group_zero, 1), ==,
		AXIS_DATA_DIR_OTHER);
	/* Another user's directory in a group the process is not in. */
	g_assert_cmpint(axis_data_dir_owner_check(1000, 1000, S_IFDIR | 0770, 4242, plain + 0, 0), ==,
		AXIS_DATA_DIR_OTHER);
	g_assert_cmpint(axis_data_dir_owner_check(1000, 1000, S_IFDIR | 0770, 4242, group_zero, 1), ==,
		AXIS_DATA_DIR_OTHER);
	/* Everyone may write: refused, even for its owner or group. */
	g_assert_cmpint(axis_data_dir_owner_check(1000, 1000, S_IFDIR | 0777, 1000, plain, 1), ==,
		AXIS_DATA_DIR_EVERYONE);
	g_assert_cmpint(axis_data_dir_owner_check(1000, 0, S_IFDIR | 01777, 1000650000, group_zero, 1), ==,
		AXIS_DATA_DIR_EVERYONE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/data-dir/owner-rule", test_owner_rule);
	g_test_add("/data-dir/created", Fixture, NULL, fixture_setup, test_created, fixture_teardown);
	g_test_add("/data-dir/refused", Fixture, NULL, fixture_setup, test_refused, fixture_teardown);
	g_test_add("/data-dir/by-descriptor", Fixture, NULL, fixture_setup, test_by_descriptor, fixture_teardown);
	g_test_add("/data-dir/subdir-checked", Fixture, NULL, fixture_setup, test_subdir_checked, fixture_teardown);
	return g_test_run();
}
