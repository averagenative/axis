/*
 * test-volume-write.c - the volume write layer proves the volume again at
 * the start of every operation: with the disk moved away after a guard was
 * taken and another directory put at its path (carrying another volume's
 * marker, or a copy of this volume's own), every operation that changes a
 * file is refused through that guard, and neither the old tree nor the new
 * one changes
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Disks are directories under the test's scratch directory, as in the
 * import and storage tests, and, in a user and mount namespace of the
 * test's own (as test-import-mounts.c), small tmpfs filesystems unmounted
 * lazily or mounted over; nothing is mounted on the host and nothing
 * outside the scratch directory is touched.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-library.h"
#include "axis-test-storage.h"
#include "storage/axis-storage-error.h"
#include "storage/axis-volume-guard.h"
#include "storage/axis-volume-marker.h"
#include "storage/axis-volume-write.h"

#define OTHER_UUID  "5d4c3b2a-1f0e-4d9c-8b7a-6f5e4d3c2b1a"
#define FOLDER      "Movies/Movie (2001)"
#define MOVIE       "Movie (2001).mkv"
#define SECOND_NAME AXIS_VOLUME_TEMP_PREFIX "5"
#define OTHER_FILE  AXIS_VOLUME_TEMP_PREFIX "6"
#define WORK_FILE   "1-1.tmp"

typedef enum
{
	OP_MKDIR,
	OP_RMDIR,
	OP_UNLINK_TEMP,
	OP_LINK_TEMP,
	OP_COPY_TEMP,
	OP_PUBLISH,
	OP_SWAP,
	OP_RETIRE_LINK,
	OP_RETIRE_MOVE,
	OP_REMOVE_RECYCLED,
	OP_SETTLE,
	OP_ADOPT,
	OP_RENAME_WORK,
	OP_UNLINK_WORK,
	OP_REMOVE_FOLDER,
	OP_REMOVE_FILE,
	OP_MOVE_TEMP,
	OP_MOVE_BACK,
	OP_REMOVE_MOVED,
	OP_UNLINK_BESIDE,
	N_OPS
} Op;

static const gchar *const op_names[] = {
	"mkdir", "rmdir_created", "unlink_temp", "link_temp", "copy_temp", "publish", "swap", "retire (link)",
	"retire (move)", "remove_recycled", "settle", "adopt", "rename_work", "unlink_work", "remove_folder",
	"remove_file", "move_temp", "move_back", "remove_moved", "unlink_temp_beside"
};

/* A finished download's leftover folder, for remove_folder, and a job's file
 * straight in the downloads folder, for remove_file. */
#define JOB_FOLDER  "Downloads/Some.Job.2001"
#define LOOSE_FILE  "Loose.Job.2002.mkv"
/* A download's file to move into the library (move_temp); a file moved into
 * the movie's folder under a temporary name, to move back (move_back); a
 * download's file whose verified copy is in the library (remove_moved). */
#define MOVE_FILE   "Move.Job.2003.mkv"
#define MOVED_TEMP  AXIS_VOLUME_TEMP_PREFIX "11"
#define MOVED_BACK  "Moved.Back.2004.mkv"
#define COPIED_FILE "Copied.Job.2005.mkv"
#define COPIED_TEXT "a download copied across volumes"

/* How the disk is replaced under the guard. */
typedef enum
{
	REPLACE_RENAME,	/* a directory: moved away, another put at its path */
	REPLACE_DETACH,	/* a tmpfs: unmounted lazily (still reachable through a bind elsewhere) */
	REPLACE_OVER	/* a tmpfs: another tmpfs mounted over it */
} Replace;

typedef struct
{
	AxisTestLibrary *t;
	gboolean         mounts;	/* the disk is a tmpfs of its own (in a namespace) */
	gchar           *disk;
	gchar           *moved;
	gchar           *stash;
	gchar           *source;	/* a file off the volume, for copy_temp */
	gint64           volume;
	guint64          empty_ino;
	guint64          recycled_ino;
	guint64          recycled_size;
	gchar           *recycled_name;
} Fixture;

static void
mount_tmpfs(const gchar *where)
{
	if (mount("axis-test", where, "tmpfs", 0, "size=16m,mode=0755") != 0)
		g_error("mount tmpfs on %s: %s", where, g_strerror(errno));
}

static guint64
inode_of(const gchar *path)
{
	struct stat st;

	g_assert_cmpint(lstat(path, &st), ==, 0);
	return (guint64)st.st_ino;
}

/* A file's times, in nanoseconds since the epoch. */
static gint64
mtime_of(const struct stat *st)
{
	return (gint64)st->st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) + st->st_mtim.tv_nsec;
}

static gint64
ctime_of(const struct stat *st)
{
	return (gint64)st->st_ctim.tv_sec * G_GINT64_CONSTANT(1000000000) + st->st_ctim.tv_nsec;
}

/* 0 when nothing is there (an earlier operation of the in-place test moved
 * it). */
static guint64
maybe_inode(const gchar *path)
{
	struct stat st;

	return lstat(path, &st) == 0 ? (guint64)st.st_ino : 0;
}

static gint
open_dir(
	AxisVolumeGuard *guard,
	const gchar     *relative
){
	g_autoptr(GError) error = NULL;
	gint fd = axis_volume_guard_open_dir(guard, relative, &error);

	g_assert_no_error(error);
	g_assert_cmpint(fd, >=, 0);
	return fd;
}

static AxisVolumeGuard *
guard_of(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	AxisVolumeGuard *guard = axis_volume_guard_acquire(fx->t->storage, fx->volume, AXIS_VOLUME_INTENT_WRITE, &error);

	g_assert_no_error(error);
	g_assert_nonnull(guard);
	return guard;
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autofree gchar *movie = NULL;
	g_autofree gchar *old = NULL;
	gint folder;
	gint movies;
	gint top;
	gint dir;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	fx->mounts = GPOINTER_TO_INT(data);
	fx->t = axis_test_library_new(4);
	if (fx->t == NULL)
		return;
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	if (fx->mounts) {
		g_autofree gchar *where = g_build_filename(fx->t->media, "disk1", NULL);

		axis_test_storage_mkdir(where);
		mount_tmpfs(where);
	}
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->moved = g_strconcat(fx->disk, ".moved", NULL);
	fx->stash = g_build_filename(fx->t->dir, "stash-mount", NULL);
	fx->volume = axis_test_library_add_volume(fx->t, fx->disk, "Disk");
	axis_test_library_add_root(fx->t, fx->volume, fx->disk, "Movies", AXIS_MEDIA_MOVIES);

	/* A movie, a second name of it, another file beside it, a folder made by
	 * the write layer, a file in the recycle folder, a file in the work
	 * area. */
	movie = g_build_filename(fx->disk, FOLDER, MOVIE, NULL);
	old = g_build_filename(fx->disk, "Movies", "Old.mkv", NULL);
	{
		g_autofree gchar *folder_path = g_build_filename(fx->disk, FOLDER, NULL);
		g_autofree gchar *other = g_build_filename(fx->disk, FOLDER, OTHER_FILE, NULL);

		axis_test_storage_mkdir(folder_path);
		axis_test_storage_write(movie, "the movie");
		axis_test_storage_write(other, "another file");
	}
	axis_test_storage_write(old, "an old movie");
	fx->source = g_build_filename(fx->t->dir, "source.mkv", NULL);
	axis_test_storage_write(fx->source, "a download, off the volume");
	{
		g_autofree gchar *job = g_build_filename(fx->disk, JOB_FOLDER, NULL);
		g_autofree gchar *inner = g_build_filename(job, "Sample", NULL);
		g_autofree gchar *nfo = g_build_filename(job, "Some.Job.2001.nfo", NULL);
		g_autofree gchar *sample = g_build_filename(inner, "sample.mkv", NULL);

		g_autofree gchar *loose = g_build_filename(fx->disk, "Downloads", LOOSE_FILE, NULL);
		g_autofree gchar *move = g_build_filename(fx->disk, "Downloads", MOVE_FILE, NULL);
		g_autofree gchar *moved = g_build_filename(fx->disk, FOLDER, MOVED_TEMP, NULL);
		g_autofree gchar *copied = g_build_filename(fx->disk, "Downloads", COPIED_FILE, NULL);

		axis_test_storage_mkdir(inner);
		axis_test_storage_write(nfo, "an nfo");
		axis_test_storage_write(sample, "a sample");
		axis_test_storage_write(loose, "a job's one file");
		axis_test_storage_write(move, "a download to move");
		axis_test_storage_write(moved, "a download moved in");
		axis_test_storage_write(copied, COPIED_TEXT);
	}

	guard = guard_of(fx);
	folder = open_dir(guard, FOLDER);
	movies = open_dir(guard, "Movies");
	g_assert_true(axis_volume_write_link_temp(guard, folder, MOVIE, inode_of(movie), folder, SECOND_NAME, &error));
	g_assert_no_error(error);
	dir = axis_volume_write_mkdir(guard, movies, "Empty", 0755, NULL, &fx->empty_ino, &error);
	g_assert_no_error(error);
	close(dir);
	{
		const gchar *const where[] = { "Movies", NULL };
		gchar *name = NULL;

		fx->recycled_size = 12;
		fx->recycled_ino = inode_of(old);
		g_assert_true(axis_volume_write_retire(guard, movies, "Old.mkv", fx->recycled_ino, where, 0700,
			AXIS_VOLUME_RETIRE_MOVE, &name, &error));
		g_assert_no_error(error);
		fx->recycled_name = name;
	}
	top = axis_volume_guard_get_fd(guard);
	dir = axis_volume_write_mkdir(guard, top, AXIS_VOLUME_WORK_DIR, 0700, NULL, NULL, &error);
	g_assert_no_error(error);
	close(dir);
	{
		g_autofree gchar *work_file = g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, WORK_FILE, NULL);

		axis_test_storage_write(work_file, "a converted file");
	}
	close(folder);
	close(movies);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t != NULL) {
		if (fx->mounts)
			(void)umount2(fx->disk, MNT_DETACH);
		axis_test_library_free(fx->t);
	}
	g_free(fx->disk);
	g_free(fx->moved);
	g_free(fx->stash);
	g_free(fx->source);
	g_free(fx->recycled_name);
	axis_test_log_reset();
}

/* Another volume's marker at @dir, or (@same) a copy of this volume's own,
 * read from @from. */
static void
put_marker(
	const gchar *dir,
	const gchar *from_dir,
	gboolean     same
){
	if (same) {
		g_autofree gchar *from = g_build_filename(from_dir, AXIS_VOLUME_MARKER_NAME, NULL);
		g_autofree gchar *to = g_build_filename(dir, AXIS_VOLUME_MARKER_NAME, NULL);
		g_autofree gchar *content = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(from, &content, &length, NULL));
		g_assert_true(g_file_set_contents(to, content, (gssize)length, NULL));
	} else {
		axis_test_storage_write_marker(dir, OTHER_UUID);
	}
}

/* The disk goes from its path and another takes the path, as @how says,
 * with another volume's marker or (@same) a copy of this one's. */
static void
replace_disk(
	Fixture  *fx,
	Replace   how,
	gboolean  same
){
	switch (how) {
	case REPLACE_RENAME:
		g_assert_cmpint(g_rename(fx->disk, fx->moved), ==, 0);
		axis_test_storage_mkdir(fx->disk);
		put_marker(fx->disk, fx->moved, same);
		break;
	case REPLACE_DETACH:
		/* Unmounted lazily: what the guard holds stays reachable (here
		 * through a bind mount, for the test to look at), the path shows
		 * the directory it was mounted on, with a marker put there. */
		axis_test_storage_mkdir(fx->stash);
		g_assert_cmpint(mount(fx->disk, fx->stash, NULL, MS_BIND, NULL), ==, 0);
		g_assert_cmpint(umount2(fx->disk, MNT_DETACH), ==, 0);
		put_marker(fx->disk, fx->stash, same);
		break;
	case REPLACE_OVER:
		{
			g_autofree gchar *marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);
			g_autofree gchar *content = NULL;
			gsize length = 0;

			g_assert_true(g_file_get_contents(marker, &content, &length, NULL));
			mount_tmpfs(fx->disk);
			if (same)
				g_assert_true(g_file_set_contents(marker, content, (gssize)length, NULL));
			else
				axis_test_storage_write_marker(fx->disk, OTHER_UUID);
		}
		break;
	default:
		g_assert_not_reached();
	}
}

/* The old disk's tree, as it is while it is away (%NULL: only once it is
 * back). */
static gchar *
old_tree(
	Fixture *fx,
	Replace  how
){
	switch (how) {
	case REPLACE_RENAME:
		return axis_test_storage_digest(fx->moved);
	case REPLACE_DETACH:
		return axis_test_storage_digest(fx->stash);
	default:
		return NULL;
	}
}

static void
restore_disk(
	Fixture *fx,
	Replace  how
){
	g_autofree gchar *marker = g_build_filename(fx->disk, AXIS_VOLUME_MARKER_NAME, NULL);

	switch (how) {
	case REPLACE_RENAME:
		g_assert_cmpint(g_unlink(marker), ==, 0);
		g_assert_cmpint(g_rmdir(fx->disk), ==, 0);
		g_assert_cmpint(g_rename(fx->moved, fx->disk), ==, 0);
		break;
	case REPLACE_DETACH:
		g_assert_cmpint(g_unlink(marker), ==, 0);
		g_assert_cmpint(mount(fx->stash, fx->disk, NULL, MS_BIND, NULL), ==, 0);
		g_assert_cmpint(umount2(fx->stash, MNT_DETACH), ==, 0);
		g_assert_cmpint(g_rmdir(fx->stash), ==, 0);
		break;
	case REPLACE_OVER:
		g_assert_cmpint(umount2(fx->disk, MNT_DETACH), ==, 0);
		break;
	default:
		g_assert_not_reached();
	}
}

/* @op through @guard, with the descriptors opened before the disk went. */
static gboolean
run_op(
	Fixture         *fx,
	Op               op,
	AxisVolumeGuard *guard,
	gint             movies,
	gint             folder,
	gint             work,
	gint             downloads,
	guint64          movie_ino,
	guint64          second_ino,
	guint64          other_ino,
	guint64          work_ino,
	GError         **error
){
	const gchar *const where[] = { "Movies", "Movie (2001)", NULL };
	gboolean ok = FALSE;

	switch (op) {
	case OP_MKDIR:
		{
			gint fd = axis_volume_write_mkdir(guard, movies, "New", 0755, NULL, NULL, error);

			ok = fd >= 0;
			if (fd >= 0)
				close(fd);
		}
		break;
	case OP_RMDIR:
		ok = axis_volume_write_rmdir_created(guard, movies, "Empty", fx->empty_ino, error);
		break;
	case OP_UNLINK_TEMP:
		ok = axis_volume_write_unlink_temp(guard, folder, SECOND_NAME, second_ino, FALSE, NULL, error);
		break;
	case OP_LINK_TEMP:
		ok = axis_volume_write_link_temp(guard, folder, MOVIE, movie_ino, folder, AXIS_VOLUME_TEMP_PREFIX "7", error);
		break;
	case OP_COPY_TEMP:
		{
			gint source = open(fx->source, O_RDONLY | O_CLOEXEC);
			g_autofree gchar *checksum = NULL;
			guint64 ino = 0;

			g_assert_cmpint(source, >=, 0);
			ok = axis_volume_write_copy_temp(guard, source, -1, NULL, 26, folder, AXIS_VOLUME_TEMP_PREFIX "8", NULL,
				NULL, &ino, NULL, &checksum, error);
			close(source);
		}
		break;
	case OP_PUBLISH:
		ok = axis_volume_write_publish(guard, folder, SECOND_NAME, second_ino, "Movie (2001) copy.mkv", error);
		break;
	case OP_SWAP:
		ok = axis_volume_write_swap(guard, folder, OTHER_FILE, other_ino, MOVIE, movie_ino, error);
		break;
	case OP_RETIRE_LINK:
	case OP_RETIRE_MOVE:
		{
			gchar *name = NULL;

			ok = axis_volume_write_retire(guard, folder, MOVIE, movie_ino, where, 0700, op == OP_RETIRE_LINK ?
				AXIS_VOLUME_RETIRE_LINK : AXIS_VOLUME_RETIRE_MOVE, &name, error);
			g_free(name);
		}
		break;
	case OP_REMOVE_RECYCLED:
		{
			const gchar *parts[4];
			AxisVolumeRecycledOutcome outcome;

			parts[0] = AXIS_VOLUME_RECYCLE_DIR;
			parts[1] = "Movies";
			parts[2] = fx->recycled_name;
			parts[3] = NULL;
			ok = axis_volume_write_remove_recycled(guard, parts, fx->recycled_ino, fx->recycled_size, &outcome, error);
		}
		break;
	case OP_SETTLE:
		{
			struct stat st;

			ok = axis_volume_write_settle(guard, work, WORK_FILE, work_ino, 0644, getuid(), getgid(), 1600000000, 0,
				TRUE, &st, error);
		}
		break;
	case OP_ADOPT:
		ok = axis_volume_write_adopt(guard, work, WORK_FILE, work_ino, folder, AXIS_VOLUME_TEMP_PREFIX "9", error);
		break;
	case OP_RENAME_WORK:
		ok = axis_volume_write_rename_work(guard, work, WORK_FILE, work_ino, "trial-1.mkv", error);
		break;
	case OP_UNLINK_WORK:
		ok = axis_volume_write_unlink_work(guard, work, WORK_FILE, work_ino, NULL, error);
		break;
	case OP_REMOVE_FOLDER:
		{
			AxisVolumeFolderOutcome outcome;

			ok = axis_volume_write_remove_folder(guard, downloads, "Some.Job.2001", 0, &outcome, NULL, error);
			/* With the volume in place it is removed (a refusal below is the
			 * guard's, never an outcome). */
			if (ok)
				g_assert_cmpint(outcome, ==, AXIS_VOLUME_FOLDER_REMOVED);
		}
		break;
	case OP_REMOVE_FILE:
		{
			AxisVolumeRecycledOutcome outcome;
			struct stat st;

			g_assert_cmpint(fstatat(downloads, LOOSE_FILE, &st, AT_SYMLINK_NOFOLLOW), ==, 0);
			ok = axis_volume_write_remove_file(guard, downloads, LOOSE_FILE, (guint64)st.st_ino, &outcome, error);
			if (ok)
				g_assert_cmpint(outcome, ==, AXIS_VOLUME_RECYCLED_REMOVED);
		}
		break;
	case OP_MOVE_TEMP:
		{
			struct stat st;

			g_assert_cmpint(fstatat(downloads, MOVE_FILE, &st, AT_SYMLINK_NOFOLLOW), ==, 0);
			ok = axis_volume_write_move_temp(guard, downloads, MOVE_FILE, (guint64)st.st_ino, folder,
				AXIS_VOLUME_TEMP_PREFIX "10", error);
		}
		break;
	case OP_MOVE_BACK:
		{
			struct stat st;

			g_assert_cmpint(fstatat(folder, MOVED_TEMP, &st, AT_SYMLINK_NOFOLLOW), ==, 0);
			ok = axis_volume_write_move_back(guard, folder, MOVED_TEMP, (guint64)st.st_ino, downloads, MOVED_BACK,
				error);
		}
		break;
	case OP_REMOVE_MOVED:
		{
			AxisVolumeRecycledOutcome outcome;
			struct stat st;

			g_assert_cmpint(fstatat(downloads, COPIED_FILE, &st, AT_SYMLINK_NOFOLLOW), ==, 0);
			ok = axis_volume_write_remove_moved(guard, downloads, COPIED_FILE, (guint64)st.st_ino, (guint64)st.st_size,
				mtime_of(&st), ctime_of(&st), &outcome, error);
			if (ok)
				g_assert_cmpint(outcome, ==, AXIS_VOLUME_RECYCLED_REMOVED);
		}
		break;
	case OP_UNLINK_BESIDE:
		ok = axis_volume_write_unlink_temp_beside(guard, folder, SECOND_NAME, second_ino, folder, MOVIE, NULL, error);
		break;
	default:
		g_assert_not_reached();
	}
	return ok;
}

/* One operation: a guard and its descriptors taken; with @replace the disk
 * is replaced before the operation, which must be refused with both trees
 * as they were; without, it must succeed (the setup is one where it
 * would). */
static void
one_op(
	Fixture  *fx,
	Op        op,
	gboolean  replace,
	Replace   how,
	gboolean  same
){
	static const gchar *const ways[] = {
		"the disk moved away and another directory put at its path",
		"the disk unmounted lazily, its mount point left at its path",
		"another disk mounted over it"
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
	g_autofree gchar *movie = g_build_filename(fx->disk, FOLDER, MOVIE, NULL);
	g_autofree gchar *second = g_build_filename(fx->disk, FOLDER, SECOND_NAME, NULL);
	g_autofree gchar *other = g_build_filename(fx->disk, FOLDER, OTHER_FILE, NULL);
	g_autofree gchar *work_file = g_build_filename(fx->disk, AXIS_VOLUME_WORK_DIR, WORK_FILE, NULL);
	g_autofree gchar *old_before = NULL;
	g_autofree gchar *old_after = NULL;
	g_autofree gchar *new_before = NULL;
	g_autofree gchar *new_after = NULL;
	guint64 movie_ino = maybe_inode(movie);
	guint64 second_ino = maybe_inode(second);
	guint64 other_ino = maybe_inode(other);
	guint64 work_ino = maybe_inode(work_file);
	gint movies = open_dir(guard, "Movies");
	gint folder = open_dir(guard, FOLDER);
	gint downloads = open_dir(guard, "Downloads");
	gint work = openat(axis_volume_guard_get_fd(guard), AXIS_VOLUME_WORK_DIR, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
		O_CLOEXEC);
	gboolean ok;

	g_assert_cmpint(work, >=, 0);
	g_test_message("%s, %s%s", op_names[op], !replace ? "the volume in place" : ways[how], !replace ? "" : same ?
		", with a copy of its marker" : ", with another volume's marker");
	if (replace) {
		old_before = axis_test_storage_digest(fx->disk);
		replace_disk(fx, how, same);
		new_before = axis_test_storage_digest(fx->disk);
	}
	ok = run_op(fx, op, guard, movies, folder, work, downloads, movie_ino, second_ino, other_ino, work_ino, &error);
	close(movies);
	close(folder);
	close(downloads);
	close(work);
	if (!replace) {
		if (!ok)
			g_error("%s failed with the volume in place: %s", op_names[op], error->message);
		return;
	}
	if (ok)
		g_error("%s wrote through a guard whose disk was replaced", op_names[op]);
	g_assert_nonnull(error);
	old_after = old_tree(fx, how);
	new_after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(new_before, ==, new_after);
	/* Everything after it through the same guard is refused too. */
	g_clear_error(&error);
	g_assert_false(axis_volume_guard_writable(guard, &error));
	g_assert_nonnull(error);
	restore_disk(fx, how);
	if (old_after == NULL)
		old_after = axis_test_storage_digest(fx->disk);
	g_assert_cmpstr(old_before, ==, old_after);
}

/* Spec (storage-volumes): every write operation refuses through a guard
 * whose disk was replaced after it was taken, whoever calls it (the import
 * pipeline, the TV series' per-file path, the conversions), and changes
 * neither tree. */
static void
test_replaced_under_guard(Fixture *fx, gconstpointer data)
{
	guint op;

	if (fx->t == NULL)
		return;
	for (op = 0; op < N_OPS; op++) {
		one_op(fx, (Op)op, TRUE, REPLACE_RENAME, FALSE);
		one_op(fx, (Op)op, TRUE, REPLACE_RENAME, TRUE);
	}
}

/* ── On real filesystems ────────────────────────────────────────── */

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

/* As test-import-mounts.c: a user and mount namespace of the test's own, in
 * which the disk is a small tmpfs; nothing is mounted on the host. */
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
mounted_body(void)
{
	Fixture fx;
	guint op;

	fixture_setup(&fx, GINT_TO_POINTER(TRUE));
	g_assert_nonnull(fx.t);
	for (op = 0; op < N_OPS; op++) {
		one_op(&fx, (Op)op, TRUE, REPLACE_DETACH, FALSE);
		one_op(&fx, (Op)op, TRUE, REPLACE_DETACH, TRUE);
		one_op(&fx, (Op)op, TRUE, REPLACE_OVER, FALSE);
		one_op(&fx, (Op)op, TRUE, REPLACE_OVER, TRUE);
	}
	g_print("refused: %u operations, four ways each\n", (guint)N_OPS);

	/* A filesystem mounted inside a download's folder: nothing is removed. */
	{
		g_autofree gchar *job = g_build_filename(fx.disk, JOB_FOLDER, NULL);
		g_autofree gchar *inner = g_build_filename(job, "Sample", NULL);
		g_autofree gchar *mounted_file = g_build_filename(inner, "on-another-fs.mkv", NULL);
		g_autofree gchar *before = NULL;
		g_autofree gchar *after = NULL;
		g_autoptr(AxisVolumeGuard) guard = guard_of(&fx);
		g_autoptr(GError) error = NULL;
		AxisVolumeFolderOutcome outcome;
		guint removed = 99;
		gint downloads;

		mount_tmpfs(inner);
		axis_test_storage_write(mounted_file, "a file of another filesystem");
		before = axis_test_storage_digest(job);
		downloads = open_dir(guard, "Downloads");
		g_assert_true(axis_volume_write_remove_folder(guard, downloads, "Some.Job.2001", 0, &outcome, &removed,
			&error));
		g_assert_no_error(error);
		g_assert_cmpint(outcome, ==, AXIS_VOLUME_FOLDER_OTHER_FS);
		g_assert_cmpuint(removed, ==, 0);
		close(downloads);
		after = axis_test_storage_digest(job);
		g_assert_cmpstr(after, ==, before);
		g_print("a filesystem inside a download's folder: nothing removed\n");

		/* A move onto that other filesystem: refused, the download's file
		 * where it was. */
		{
			g_autofree gchar *source = g_build_filename(fx.disk, "Downloads", MOVE_FILE, NULL);
			gint other;

			downloads = open_dir(guard, "Downloads");
			other = open(inner, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			g_assert_cmpint(other, >=, 0);
			g_assert_false(axis_volume_write_move_temp(guard, downloads, MOVE_FILE, inode_of(source), other,
				AXIS_VOLUME_TEMP_PREFIX "12", &error));
			g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
			g_clear_error(&error);
			g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
			close(other);
			close(downloads);
			g_print("a move onto another filesystem: refused\n");
		}
		g_assert_cmpint(umount2(inner, MNT_DETACH), ==, 0);
	}
	fixture_teardown(&fx, NULL);
}

/* Spec (storage-volumes): the same with the disk a filesystem of its own,
 * unmounted lazily under the guard (its mount point, with a marker, left at
 * the path) or with another filesystem mounted over it. */
static void
test_replaced_mounts(void)
{
	if (g_test_subprocess()) {
		enter_namespace();
		mounted_body();
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
	g_test_trap_subprocess(NULL, 300 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
	/* Every case ran in the child. */
	g_test_trap_assert_stdout("*refused: 20 operations, four ways each*");
	g_test_trap_assert_stdout("*a move onto another filesystem: refused*");
	g_test_trap_assert_stdout("*a filesystem inside a download's folder: nothing removed*");
}

/* The same operations succeed with the volume in place (so the refusals
 * above are the volume's, not the setup's). */
static void
test_in_place(Fixture *fx, gconstpointer data)
{
	static const Op ops[] = {
		OP_MKDIR, OP_RMDIR, OP_LINK_TEMP, OP_COPY_TEMP, OP_PUBLISH, OP_SWAP, OP_REMOVE_RECYCLED, OP_SETTLE, OP_ADOPT,
		OP_REMOVE_FOLDER, OP_REMOVE_FILE, OP_MOVE_TEMP, OP_MOVE_BACK, OP_REMOVE_MOVED
	};
	guint i;

	if (fx->t == NULL)
		return;
	for (i = 0; i < G_N_ELEMENTS(ops); i++)
		one_op(fx, ops[i], FALSE, REPLACE_RENAME, FALSE);
}

/* ── A download's leftover folder ───────────────────────────────── */

static AxisVolumeFolderOutcome
remove_folder(
	Fixture     *fx,
	const gchar *name,
	guint64      ino,
	guint       *removed
){
	g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
	g_autoptr(GError) error = NULL;
	AxisVolumeFolderOutcome outcome = AXIS_VOLUME_FOLDER_CHANGED;
	gint downloads = open_dir(guard, "Downloads");

	g_assert_true(axis_volume_write_remove_folder(guard, downloads, name, ino, &outcome, removed, &error));
	g_assert_no_error(error);
	close(downloads);
	return outcome;
}

static void
set_read_only(
	Fixture  *fx,
	gboolean  on
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, on ? "true" : "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* Spec (storage-volumes): a finished download's leftover folder is removed
 * whole, by its own names only: a file with another name keeps it and its
 * data, a symbolic link is removed as a name and never followed; something
 * that is not that folder, or a tree beyond the bounds, is left whole; and
 * nothing is removed while storage is read-only. */
static void
test_remove_folder(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *job = NULL;
	g_autofree gchar *film = NULL;
	g_autofree gchar *library_name = NULL;
	g_autofree gchar *outside = NULL;
	g_autofree gchar *outside_dir = NULL;
	g_autofree gchar *outside_kept = NULL;
	g_autofree gchar *file_link = NULL;
	g_autofree gchar *folder_link = NULL;
	g_autofree gchar *downloads = NULL;
	g_autofree gchar *deep = NULL;
	g_autofree gchar *deep_digest = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	struct stat st;
	guint removed = 0;
	guint i;

	if (fx->t == NULL)
		return;
	job = g_build_filename(fx->disk, JOB_FOLDER, NULL);
	downloads = g_build_filename(fx->disk, "Downloads", NULL);
	film = g_build_filename(job, "Some.Job.2001.mkv", NULL);
	library_name = g_build_filename(fx->disk, FOLDER, "Some Job (2001).mkv", NULL);
	outside = g_build_filename(fx->t->dir, "not-the-volume.txt", NULL);
	outside_dir = g_build_filename(fx->t->dir, "not-the-volume", NULL);
	outside_kept = g_build_filename(outside_dir, "kept.txt", NULL);
	file_link = g_build_filename(job, "a-link-out", NULL);
	folder_link = g_build_filename(job, "Sample", "a-folder-link-out", NULL);

	/* The film, with a second name in the library; a link out of the volume
	 * to a file, and one to a folder. */
	axis_test_storage_write(film, "the film");
	g_assert_cmpint(link(film, library_name), ==, 0);
	axis_test_storage_write(outside, "not the volume's");
	axis_test_storage_mkdir(outside_dir);
	axis_test_storage_write(outside_kept, "not the volume's either");
	g_assert_cmpint(symlink(outside, file_link), ==, 0);
	g_assert_cmpint(symlink(outside_dir, folder_link), ==, 0);

	/* Not the folder looked at (another inode): left whole. */
	before = axis_test_storage_digest(job);
	g_assert_cmpint(remove_folder(fx, "Some.Job.2001", inode_of(film), &removed), ==, AXIS_VOLUME_FOLDER_CHANGED);
	g_assert_cmpuint(removed, ==, 0);
	after = axis_test_storage_digest(job);
	g_assert_cmpstr(after, ==, before);
	g_clear_pointer(&after, g_free);

	/* Read-only: refused, nothing removed. */
	set_read_only(fx, TRUE);
	{
		g_autoptr(AxisVolumeGuard) guard = NULL;
		g_autoptr(GError) error = NULL;

		guard = axis_volume_guard_acquire(fx->t->storage, fx->volume, AXIS_VOLUME_INTENT_WRITE, &error);
		g_assert_null(guard);
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
	}
	set_read_only(fx, FALSE);
	{
		/* A guard taken before storage was made read-only refuses too. */
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		g_autoptr(GError) error = NULL;
		AxisVolumeFolderOutcome outcome;
		gint dir = open_dir(guard, "Downloads");

		set_read_only(fx, TRUE);
		g_assert_false(axis_volume_write_remove_folder(guard, dir, "Some.Job.2001", 0, &outcome, NULL, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
		close(dir);
		set_read_only(fx, FALSE);
	}
	after = axis_test_storage_digest(job);
	g_assert_cmpstr(after, ==, before);

	/* Too deep for a download's folder: nothing removed. */
	deep = g_build_filename(downloads, "Deep.Job", NULL);
	{
		g_autoptr(GString) path = g_string_new(deep);

		for (i = 0; i <= AXIS_VOLUME_FOLDER_DEPTH; i++)
			g_string_append_printf(path, "/d%u", i);
		axis_test_storage_mkdir(path->str);
		g_string_append(path, "/bottom.txt");
		axis_test_storage_write(path->str, "deep");
	}
	deep_digest = axis_test_storage_digest(deep);
	g_assert_cmpint(remove_folder(fx, "Deep.Job", 0, &removed), ==, AXIS_VOLUME_FOLDER_TOO_BIG);
	g_assert_cmpuint(removed, ==, 0);
	{
		g_autofree gchar *again = axis_test_storage_digest(deep);

		g_assert_cmpstr(again, ==, deep_digest);
	}

	/* The folder, whole: its names and nothing else. */
	g_assert_cmpint(remove_folder(fx, "Some.Job.2001", inode_of(job), &removed), ==, AXIS_VOLUME_FOLDER_REMOVED);
	/* The .nfo, the film's name, the link, the sample, the folder link and
	 * the Sample folder. */
	g_assert_cmpuint(removed, ==, 6);
	g_assert_cmpint(lstat(job, &st), ==, -1);
	g_assert_true(g_file_test(downloads, G_FILE_TEST_IS_DIR));
	g_assert_cmpint(lstat(library_name, &st), ==, 0);
	g_assert_cmpuint((guint)st.st_nlink, ==, 1);
	{
		g_autofree gchar *content = axis_test_storage_read(library_name);

		g_assert_cmpstr(content, ==, "the film");
	}
	g_assert_true(g_file_test(outside, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(outside_kept, G_FILE_TEST_IS_REGULAR));

	/* Again: nothing is there. */
	g_assert_cmpint(remove_folder(fx, "Some.Job.2001", 0, &removed), ==, AXIS_VOLUME_FOLDER_GONE);

	/* A link to a folder at its name, then a file: not a folder, left. */
	g_assert_cmpint(symlink(outside_dir, job), ==, 0);
	g_assert_cmpint(remove_folder(fx, "Some.Job.2001", 0, &removed), ==, AXIS_VOLUME_FOLDER_NOT_FOLDER);
	g_assert_cmpint(lstat(job, &st), ==, 0);
	g_assert_true(S_ISLNK(st.st_mode));
	g_assert_true(g_file_test(outside_kept, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(unlink(job), ==, 0);
	axis_test_storage_write(job, "a file, not a folder");
	g_assert_cmpint(remove_folder(fx, "Some.Job.2001", 0, &removed), ==, AXIS_VOLUME_FOLDER_NOT_FOLDER);
	g_assert_true(g_file_test(job, G_FILE_TEST_IS_REGULAR));

	/* A name that is not one folder's name is refused. */
	{
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		g_autoptr(GError) error = NULL;
		AxisVolumeFolderOutcome outcome;
		gint dir = open_dir(guard, "Downloads");

		g_assert_false(axis_volume_write_remove_folder(guard, dir, "..", 0, &outcome, NULL, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		g_clear_error(&error);
		g_assert_false(axis_volume_write_remove_folder(guard, dir, "Deep.Job/d0", 0, &outcome, NULL, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		close(dir);
	}
	g_assert_true(g_file_test(deep, G_FILE_TEST_IS_DIR));
}

static AxisVolumeRecycledOutcome
remove_file(
	Fixture     *fx,
	const gchar *name,
	guint64      ino
){
	g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
	g_autoptr(GError) error = NULL;
	AxisVolumeRecycledOutcome outcome = AXIS_VOLUME_RECYCLED_REMOVED;
	gint downloads = open_dir(guard, "Downloads");

	g_assert_true(axis_volume_write_remove_file(guard, downloads, name, ino, &outcome, &error));
	g_assert_no_error(error);
	close(downloads);
	return outcome;
}

/* Spec (storage-volumes): a job's one file put straight into the downloads
 * folder is removed by its name and inode only: another inode, a link at the
 * name, are left; a hardlinked file keeps its other name; nothing while
 * storage is read-only. */
static void
test_remove_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *loose = NULL;
	g_autofree gchar *library_name = NULL;
	g_autofree gchar *outside = NULL;
	struct stat st;
	guint64 ino;

	if (fx->t == NULL)
		return;
	loose = g_build_filename(fx->disk, "Downloads", LOOSE_FILE, NULL);
	library_name = g_build_filename(fx->disk, FOLDER, "Loose Job (2002).mkv", NULL);
	outside = g_build_filename(fx->t->dir, "not-the-volume.mkv", NULL);
	g_assert_cmpint(link(loose, library_name), ==, 0);
	ino = inode_of(loose);

	/* Another inode than the one recorded: left. */
	g_assert_cmpint(remove_file(fx, LOOSE_FILE, inode_of(library_name) + 1), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_true(g_file_test(loose, G_FILE_TEST_IS_REGULAR));

	/* Read-only: refused. */
	{
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		g_autoptr(GError) error = NULL;
		AxisVolumeRecycledOutcome outcome;
		gint dir = open_dir(guard, "Downloads");

		set_read_only(fx, TRUE);
		g_assert_false(axis_volume_write_remove_file(guard, dir, LOOSE_FILE, ino, &outcome, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
		close(dir);
		set_read_only(fx, FALSE);
	}
	g_assert_true(g_file_test(loose, G_FILE_TEST_IS_REGULAR));

	/* Its name goes; the library's name keeps the file. */
	g_assert_cmpint(remove_file(fx, LOOSE_FILE, ino), ==, AXIS_VOLUME_RECYCLED_REMOVED);
	g_assert_cmpint(lstat(loose, &st), ==, -1);
	g_assert_cmpint(lstat(library_name, &st), ==, 0);
	g_assert_cmpuint((guint)st.st_nlink, ==, 1);
	g_assert_cmpint(remove_file(fx, LOOSE_FILE, ino), ==, AXIS_VOLUME_RECYCLED_GONE);

	/* A link at its name is left, and what it points to too. */
	axis_test_storage_write(outside, "not the volume's");
	g_assert_cmpint(symlink(outside, loose), ==, 0);
	g_assert_cmpint(remove_file(fx, LOOSE_FILE, inode_of(loose)), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_cmpint(lstat(loose, &st), ==, 0);
	g_assert_true(S_ISLNK(st.st_mode));
	g_assert_true(g_file_test(outside, G_FILE_TEST_IS_REGULAR));

	/* A name that is not one component. */
	{
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		g_autoptr(GError) error = NULL;
		AxisVolumeRecycledOutcome outcome;
		gint dir = open_dir(guard, "Downloads");

		g_assert_false(axis_volume_write_remove_file(guard, dir, "../" LOOSE_FILE, ino, &outcome, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		close(dir);
	}
}

/* ── Moving a download's file ───────────────────────────────────── */

static gboolean
move_temp(
	Fixture      *fx,
	const gchar  *name,
	guint64       ino,
	const gchar  *temp,
	GError      **error
){
	g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
	gint downloads = open_dir(guard, "Downloads");
	gint folder = open_dir(guard, FOLDER);
	gboolean ok = axis_volume_write_move_temp(guard, downloads, name, ino, folder, temp, error);

	close(downloads);
	close(folder);
	return ok;
}

static gboolean
move_back(
	Fixture      *fx,
	const gchar  *temp,
	guint64       ino,
	const gchar  *name,
	GError      **error
){
	g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
	gint downloads = open_dir(guard, "Downloads");
	gint folder = open_dir(guard, FOLDER);
	gboolean ok = axis_volume_write_move_back(guard, folder, temp, ino, downloads, name, error);

	close(downloads);
	close(folder);
	return ok;
}

/* Spec (storage-volumes): a download's file is moved into the library by one
 * rename that never replaces a name, keeping its inode, data, mode and time,
 * and back again; another inode, a link, a taken name, a name that is not
 * allowed and read-only storage are refused with nothing moved. */
static void
test_move_temp(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *temp = NULL;
	g_autofree gchar *outside = NULL;
	struct stat before;
	struct stat after;
	guint64 ino;

	if (fx->t == NULL)
		return;
	source = g_build_filename(fx->disk, "Downloads", MOVE_FILE, NULL);
	temp = g_build_filename(fx->disk, FOLDER, AXIS_VOLUME_TEMP_PREFIX "20", NULL);
	outside = g_build_filename(fx->t->dir, "not-the-volume.mkv", NULL);
	g_assert_cmpint(lstat(source, &before), ==, 0);
	ino = (guint64)before.st_ino;

	/* Another inode than the one checked: refused, nothing moved. */
	g_assert_false(move_temp(fx, MOVE_FILE, ino + 1, AXIS_VOLUME_TEMP_PREFIX "20", &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_clear_error(&error);
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));

	/* A temporary name that is taken: never replaced. */
	g_assert_false(move_temp(fx, MOVE_FILE, ino, OTHER_FILE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error(&error);
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	{
		g_autofree gchar *other = g_build_filename(fx->disk, FOLDER, OTHER_FILE, NULL);
		g_autofree gchar *content = axis_test_storage_read(other);

		g_assert_cmpstr(content, ==, "another file");
	}

	/* Names that are not allowed: a final name as the destination, a
	 * temporary name as the download's, a path. */
	g_assert_false(move_temp(fx, MOVE_FILE, ino, "Movie (2001) moved.mkv", &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_false(move_temp(fx, "../" MOVE_FILE, ino, AXIS_VOLUME_TEMP_PREFIX "20", &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
	g_clear_error(&error);

	/* Read-only storage: refused. */
	{
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		gint downloads = open_dir(guard, "Downloads");
		gint folder = open_dir(guard, FOLDER);

		set_read_only(fx, TRUE);
		g_assert_false(axis_volume_write_move_temp(guard, downloads, MOVE_FILE, ino, folder,
			AXIS_VOLUME_TEMP_PREFIX "20", &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
		g_clear_error(&error);
		set_read_only(fx, FALSE);
		close(downloads);
		close(folder);
	}
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));

	/* Moved: the same inode under the temporary name, one name only, its
	 * contents, mode and time as they were. */
	g_assert_true(move_temp(fx, MOVE_FILE, ino, AXIS_VOLUME_TEMP_PREFIX "20", &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
	g_assert_cmpint(lstat(temp, &after), ==, 0);
	g_assert_cmpuint((guint64)after.st_ino, ==, ino);
	g_assert_cmpuint((guint)after.st_nlink, ==, 1);
	g_assert_cmpuint(after.st_mode, ==, before.st_mode);
	g_assert_cmpint((gint64)after.st_mtime, ==, (gint64)before.st_mtime);
	{
		g_autofree gchar *content = axis_test_storage_read(temp);

		g_assert_cmpstr(content, ==, "a download to move");
	}

	/* Back to its download name; not over a name that was taken meanwhile. */
	axis_test_storage_write(source, "a new download of that name");
	g_assert_false(move_back(fx, AXIS_VOLUME_TEMP_PREFIX "20", ino, MOVE_FILE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error(&error);
	g_assert_cmpuint(inode_of(temp), ==, ino);
	{
		g_autofree gchar *content = axis_test_storage_read(source);

		g_assert_cmpstr(content, ==, "a new download of that name");
	}
	g_assert_cmpint(unlink(source), ==, 0);
	/* Only a temporary name is moved back. */
	g_assert_false(move_back(fx, MOVIE, inode_of(temp), MOVE_FILE, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_true(move_back(fx, AXIS_VOLUME_TEMP_PREFIX "20", ino, MOVE_FILE, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));
	g_assert_cmpuint(inode_of(source), ==, ino);

	/* A link at the download's name is never moved (nor followed). */
	g_assert_cmpint(unlink(source), ==, 0);
	axis_test_storage_write(outside, "not the volume's");
	g_assert_cmpint(symlink(outside, source), ==, 0);
	g_assert_false(move_temp(fx, MOVE_FILE, inode_of(source), AXIS_VOLUME_TEMP_PREFIX "20", &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_clear_error(&error);
	g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));
	g_assert_cmpint(lstat(source, &after), ==, 0);
	g_assert_true(S_ISLNK(after.st_mode));
	g_assert_true(g_file_test(outside, G_FILE_TEST_IS_REGULAR));
}

static AxisVolumeRecycledOutcome
remove_moved(
	Fixture     *fx,
	const gchar *name,
	guint64      ino,
	guint64      size,
	gint64       mtime_ns,
	gint64       ctime_ns
){
	g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
	g_autoptr(GError) error = NULL;
	AxisVolumeRecycledOutcome outcome = AXIS_VOLUME_RECYCLED_REMOVED;
	gint downloads = open_dir(guard, "Downloads");

	g_assert_true(axis_volume_write_remove_moved(guard, downloads, name, ino, size, mtime_ns, ctime_ns, &outcome,
		&error));
	g_assert_no_error(error);
	close(downloads);
	return outcome;
}

/* Spec (storage-volumes): after a move across volumes the download's file
 * goes only while it is the very file copied (inode, size and times, on the
 * opened file); another one, one touched since (a write, a chmod), a link at
 * its name, read-only storage: left. */
static void
test_remove_moved(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *copied = NULL;
	struct stat st;
	guint64 ino;
	guint64 size = strlen(COPIED_TEXT);
	gint64 mtime;
	gint64 ctime;

	if (fx->t == NULL)
		return;
	copied = g_build_filename(fx->disk, "Downloads", COPIED_FILE, NULL);
	g_assert_cmpint(lstat(copied, &st), ==, 0);
	ino = (guint64)st.st_ino;
	mtime = mtime_of(&st);
	ctime = ctime_of(&st);

	/* Another size, inode, or time: left. */
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino, size + 1, mtime, ctime), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino + 1, size, mtime, ctime), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino, size, mtime + 1, ctime), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino, size, mtime, ctime + 1), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_true(g_file_test(copied, G_FILE_TEST_IS_REGULAR));
	/* No inode or time known, a temporary name: refused. */
	{
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		AxisVolumeRecycledOutcome outcome;
		gint downloads = open_dir(guard, "Downloads");

		g_assert_false(axis_volume_write_remove_moved(guard, downloads, COPIED_FILE, 0, size, mtime, ctime, &outcome,
			&error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		g_clear_error(&error);
		g_assert_false(axis_volume_write_remove_moved(guard, downloads, COPIED_FILE, ino, size, 0, 0, &outcome,
			&error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		g_clear_error(&error);
		g_assert_false(axis_volume_write_remove_moved(guard, downloads, AXIS_VOLUME_TEMP_PREFIX "3", ino, size, mtime,
			ctime, &outcome, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		g_clear_error(&error);
		set_read_only(fx, TRUE);
		g_assert_false(axis_volume_write_remove_moved(guard, downloads, COPIED_FILE, ino, size, mtime, ctime, &outcome,
			&error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_READ_ONLY);
		g_clear_error(&error);
		set_read_only(fx, FALSE);
		close(downloads);
	}
	g_assert_true(g_file_test(copied, G_FILE_TEST_IS_REGULAR));

	/* The same inode and size, but touched since: a chmod changes its change
	 * time, so it is not the file copied as it was. */
	g_assert_cmpint(chmod(copied, 0600), ==, 0);
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino, size, mtime, ctime), ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_true(g_file_test(copied, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(lstat(copied, &st), ==, 0);
	ctime = ctime_of(&st);

	/* The very file, untouched: removed; then gone. */
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino, size, mtime, ctime), ==, AXIS_VOLUME_RECYCLED_REMOVED);
	g_assert_cmpint(lstat(copied, &st), ==, -1);
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, ino, size, mtime, ctime), ==, AXIS_VOLUME_RECYCLED_GONE);

	/* A link at its name: left, and what it points to. */
	g_assert_cmpint(symlink(fx->source, copied), ==, 0);
	g_assert_cmpint(remove_moved(fx, COPIED_FILE, inode_of(copied), size, mtime, ctime), ==,
		AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_cmpint(lstat(copied, &st), ==, 0);
	g_assert_true(S_ISLNK(st.st_mode));
	g_assert_true(g_file_test(fx->source, G_FILE_TEST_IS_REGULAR));
}

/* The copy's first step, before its first byte is read: the download's
 * file's mode changes (its change time, nothing else). */
static gboolean
chmod_before_read(
	gpointer   data,
	guint64    ino,
	guint64    written,
	GError   **error
){
	const gchar *path = data;

	(void)ino;
	(void)error;
	if (written == 0) {
		/* A tick later than the file's last change, on any kernel. */
		g_usleep(20000);
		g_assert_cmpint(chmod(path, 0600), ==, 0);
	}
	return TRUE;
}

/* The same, and the download's file deleted (by something else). */
static gboolean
unlink_before_read(
	gpointer   data,
	guint64    ino,
	guint64    written,
	GError   **error
){
	const gchar *path = data;

	(void)ino;
	(void)error;
	if (written == 0) {
		g_usleep(20000);
		g_assert_cmpint(g_unlink(path), ==, 0);
	}
	return TRUE;
}

/* The same, and another file renamed over the download's file's name (a
 * tool that writes a new file and renames it into place). */
static gboolean
rename_over_before_read(
	gpointer   data,
	guint64    ino,
	guint64    written,
	GError   **error
){
	const gchar *path = data;
	g_autofree gchar *next = NULL;

	(void)ino;
	(void)error;
	if (written == 0) {
		next = g_strconcat(path, ".new", NULL);
		g_usleep(20000);
		axis_test_storage_write(next, "a download, rewritten");
		g_assert_cmpint(g_rename(next, path), ==, 0);
	}
	return TRUE;
}

/* Storage hunt, H1: the file copied is the same file throughout its read by
 * its inode, size, modification time and change time: a change of its mode
 * while it is read (its change time alone) refuses the copy, and nothing is
 * left under the temporary name.  The file deleted while it is read (its
 * change time moves with its last name), nothing at its name afterwards, is
 * still copied whole, as the import promises; one renamed over (the file
 * read unlinked, another at its name: review of the fixes, item 2) is a
 * change, and so is a deletion where the caller gives no name to look at. */
static void
test_copy_source_changed(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *temp = NULL;
	g_autofree gchar *deleted = NULL;
	g_autofree gchar *replaced = NULL;
	g_autofree gchar *content = NULL;
	AxisVolumeSourceId copied;
	struct stat before;
	guint64 ino = 0;
	gint folder;
	gint source;
	gint scratch;

	if (fx->t == NULL)
		return;
	temp = g_build_filename(fx->disk, FOLDER, AXIS_VOLUME_TEMP_PREFIX "12", NULL);
	guard = guard_of(fx);
	folder = open_dir(guard, FOLDER);
	scratch = open(fx->t->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(scratch, >=, 0);
	source = open(fx->source, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(source, >=, 0);
	g_assert_false(axis_volume_write_copy_temp(guard, source, -1, NULL, 26, folder, AXIS_VOLUME_TEMP_PREFIX "12",
		chmod_before_read, fx->source, &ino, NULL, &checksum, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_assert_nonnull(strstr(error->message, "changed while it was copied"));
	g_assert_null(checksum);
	g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));
	close(source);
	g_clear_error(&error);

	/* Renamed over while it is read: refused, the newer file kept. */
	replaced = g_build_filename(fx->t->dir, "replaced.mkv", NULL);
	axis_test_storage_write(replaced, "a download, off the volume");
	source = open(replaced, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(source, >=, 0);
	g_assert_false(axis_volume_write_copy_temp(guard, source, scratch, "replaced.mkv", 26, folder,
		AXIS_VOLUME_TEMP_PREFIX "12", rename_over_before_read, replaced, &ino, &copied, &checksum, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_assert_nonnull(strstr(error->message, "changed while it was copied"));
	g_clear_error(&error);
	g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));
	content = axis_test_storage_read(replaced);
	g_assert_cmpstr(content, ==, "a download, rewritten");
	close(source);

	/* Deleted while it is read, with no name to look at: refused too. */
	deleted = g_build_filename(fx->t->dir, "deleted.mkv", NULL);
	axis_test_storage_write(deleted, "a download, off the volume");
	source = open(deleted, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(source, >=, 0);
	g_assert_false(axis_volume_write_copy_temp(guard, source, -1, NULL, 26, folder, AXIS_VOLUME_TEMP_PREFIX "12",
		unlink_before_read, deleted, &ino, &copied, &checksum, &error));
	g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
	g_clear_error(&error);
	close(source);

	/* Deleted while it is read, nothing at its name since: copied whole. */
	axis_test_storage_write(deleted, "a download, off the volume");
	source = open(deleted, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(source, >=, 0);
	g_assert_cmpint(fstat(source, &before), ==, 0);
	g_assert_true(axis_volume_write_copy_temp(guard, source, scratch, "deleted.mkv", 26, folder,
		AXIS_VOLUME_TEMP_PREFIX "12", unlink_before_read, deleted, &ino, &copied, &checksum, &error));
	g_assert_no_error(error);
	g_assert_false(g_file_test(deleted, G_FILE_TEST_EXISTS));
	g_assert_cmpint(copied.ctime_ns, ==, ctime_of(&before));
	g_assert_cmpuint(inode_of(temp), ==, ino);
	close(source);
	close(scratch);
	close(folder);
}

#define CHANGED_FILE "Changed.Job.2006.mkv"
#define CHANGED_SIZE (64 * 1024)

typedef struct
{
	const gchar *path;	/* the download's file */
	gboolean     changed;
	guint        whole;	/* calls with the whole size */
} ChangeStep;

/* The copy's step with the whole size comes once the download's file was
 * read and found unchanged, before the copy is read back (for a large file
 * on a slow disk, minutes): here something writes the download's file over
 * in place, as a client repairing a piece or a tag editor would. */
static gboolean
change_before_read_back(
	gpointer   data,
	guint64    ino,
	guint64    written,
	GError   **error
){
	ChangeStep *cs = data;
	g_autofree guchar *newer = NULL;
	gint fd;

	(void)ino;
	(void)error;
	if (written != CHANGED_SIZE)
		return TRUE;
	cs->whole++;
	if (cs->changed)
		return TRUE;
	g_usleep(20000);
	newer = g_malloc(CHANGED_SIZE);
	memset(newer, 'B', CHANGED_SIZE);
	fd = open(cs->path, O_WRONLY | O_CLOEXEC);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(pwrite(fd, newer, CHANGED_SIZE, 0), ==, CHANGED_SIZE);
	close(fd);
	cs->changed = TRUE;
	return TRUE;
}

/*
 * Storage hunt, H1 (its demonstration, now the rule): the download's file is
 * removed only while it is the file copied as it was read.  The copy gives
 * what the file was before its first byte was read; a change made while the
 * copy is flushed and read back is after that, and the removal, given what
 * the copy gave, keeps the changed file (the import records those times,
 * never a look taken once the copy returned).
 */
static void
test_copy_changed_during_read_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisVolumeGuard) guard = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *placed = NULL;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *older = NULL;
	g_autofree gchar *content = NULL;
	ChangeStep cs;
	AxisVolumeSourceId copied;
	struct stat before;
	struct stat now;
	AxisVolumeRecycledOutcome outcome = AXIS_VOLUME_RECYCLED_REMOVED;
	guint64 ino = 0;
	gsize length = 0;
	gint downloads;
	gint movies;
	gint src;

	if (fx->t == NULL)
		return;
	source = g_build_filename(fx->disk, "Downloads", CHANGED_FILE, NULL);
	placed = g_build_filename(fx->disk, "Movies", "Changed (2006).mkv", NULL);
	older = g_malloc(CHANGED_SIZE + 1);
	memset(older, 'A', CHANGED_SIZE);
	older[CHANGED_SIZE] = '\0';
	axis_test_storage_write(source, older);

	guard = guard_of(fx);
	downloads = open_dir(guard, "Downloads");
	movies = open_dir(guard, "Movies");
	src = openat(downloads, CHANGED_FILE, O_RDONLY | O_CLOEXEC);
	g_assert_cmpint(src, >=, 0);
	g_assert_cmpint(fstat(src, &before), ==, 0);

	/* The copy, verified: what was read is what is in the copy, and what
	 * the file was then. */
	cs.path = source;
	cs.changed = FALSE;
	cs.whole = 0;
	g_assert_true(axis_volume_write_copy_temp(guard, src, downloads, CHANGED_FILE, CHANGED_SIZE, movies,
		AXIS_VOLUME_TEMP_PREFIX "7",
		change_before_read_back, &cs, &ino, &copied, &checksum, &error));
	g_assert_no_error(error);
	g_assert_true(cs.changed);
	g_assert_cmpuint(cs.whole, ==, 1);
	g_assert_cmpuint(copied.ino, ==, (guint64)before.st_ino);
	g_assert_cmpuint(copied.size, ==, CHANGED_SIZE);
	g_assert_cmpint(copied.mtime_ns, ==, mtime_of(&before));
	g_assert_cmpint(copied.ctime_ns, ==, ctime_of(&before));
	/* A look now sees the change. */
	g_assert_cmpint(fstat(src, &now), ==, 0);
	g_assert_cmpint(ctime_of(&now), !=, copied.ctime_ns);
	close(src);

	g_assert_true(axis_volume_write_publish(guard, movies, AXIS_VOLUME_TEMP_PREFIX "7", ino, "Changed (2006).mkv",
		&error));
	g_assert_no_error(error);
	g_assert_true(axis_volume_write_remove_moved(guard, downloads, CHANGED_FILE, copied.ino, copied.size,
		copied.mtime_ns, copied.ctime_ns, &outcome, &error));
	g_assert_no_error(error);
	close(downloads);
	close(movies);

	/* The library holds what was read; the changed file stays. */
	g_assert_cmpint(outcome, ==, AXIS_VOLUME_RECYCLED_CHANGED);
	g_assert_true(g_file_get_contents(placed, &content, &length, NULL));
	g_assert_cmpuint(length, ==, CHANGED_SIZE);
	g_assert_cmpint(content[0], ==, 'A');
	g_clear_pointer(&content, g_free);
	g_assert_true(g_file_get_contents(source, &content, &length, NULL));
	g_assert_cmpint(content[0], ==, 'B');
}

/* Spec (storage-volumes): a moved file's temporary name goes only beside
 * another name of the file that leads to it, looked at then: a second link
 * elsewhere (a cross-seed, a backup) is not taken for one. */
static void
test_unlink_beside(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *temp = NULL;
	g_autofree gchar *outside = NULL;
	g_autofree gchar *final = NULL;
	gboolean removed = TRUE;
	guint64 ino;

	if (fx->t == NULL)
		return;
	temp = g_build_filename(fx->disk, FOLDER, MOVED_TEMP, NULL);
	outside = g_build_filename(fx->disk, "Downloads", "backup-of-a-moved-file.mkv", NULL);
	final = g_build_filename(fx->disk, FOLDER, "Moved (2004).mkv", NULL);
	ino = inode_of(temp);
	/* A name outside, of the same file: two links, but no other name known. */
	g_assert_cmpint(link(temp, outside), ==, 0);
	{
		g_autoptr(AxisVolumeGuard) guard = guard_of(fx);
		gint folder = open_dir(guard, FOLDER);
		gint downloads = open_dir(guard, "Downloads");

		g_assert_false(axis_volume_write_unlink_temp_beside(guard, folder, MOVED_TEMP, ino, folder, "Moved (2004).mkv",
			&removed, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
		g_clear_error(&error);
		g_assert_false(removed);
		g_assert_cmpuint(inode_of(temp), ==, ino);
		/* Another file at the name that would be the other: left too. */
		axis_test_storage_write(final, "another file of that name");
		g_assert_false(axis_volume_write_unlink_temp_beside(guard, folder, MOVED_TEMP, ino, folder, "Moved (2004).mkv",
			&removed, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_NOT_CONTAINED);
		g_clear_error(&error);
		g_assert_cmpint(unlink(final), ==, 0);
		/* A temporary name as the other, no inode: refused. */
		g_assert_false(axis_volume_write_unlink_temp_beside(guard, folder, MOVED_TEMP, ino, folder, OTHER_FILE,
			&removed, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		g_clear_error(&error);
		g_assert_false(axis_volume_write_unlink_temp_beside(guard, folder, MOVED_TEMP, 0, downloads,
			"backup-of-a-moved-file.mkv", &removed, &error));
		g_assert_error(error, AXIS_STORAGE_ERROR, AXIS_STORAGE_ERROR_INVALID);
		g_clear_error(&error);
		g_assert_cmpuint(inode_of(temp), ==, ino);
		/* Beside its name in the library: removed, that name stays. */
		g_assert_cmpint(link(temp, final), ==, 0);
		g_assert_true(axis_volume_write_unlink_temp_beside(guard, folder, MOVED_TEMP, ino, folder, "Moved (2004).mkv",
			&removed, &error));
		g_assert_no_error(error);
		g_assert_true(removed);
		g_assert_false(g_file_test(temp, G_FILE_TEST_EXISTS));
		g_assert_cmpuint(inode_of(final), ==, ino);
		g_assert_cmpuint(inode_of(outside), ==, ino);
		close(folder);
		close(downloads);
	}
}

#define ADD(name, func) g_test_add("/storage/write/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	ADD("replaced-under-guard", test_replaced_under_guard);
	ADD("in-place", test_in_place);
	ADD("remove-folder", test_remove_folder);
	ADD("remove-file", test_remove_file);
	ADD("move-temp", test_move_temp);
	ADD("remove-moved", test_remove_moved);
	ADD("copy-source-changed", test_copy_source_changed);
	ADD("copy-changed-during-read-back", test_copy_changed_during_read_back);
	ADD("unlink-beside", test_unlink_beside);
	g_test_add_func("/storage/write/replaced-mounts", test_replaced_mounts);
	return g_test_run();
}
