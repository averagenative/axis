/*
 * axis-test-library.c - test support for the scanner
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "axis-test-library.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-jobs.h"
#include "axis-test-storage.h"

static void
default_tuning(AxisLibraryTuning *tuning)
{
	memset(tuning, 0, sizeof(*tuning));
	tuning->racy_window_ns = 0;
	tuning->guard_interval_ms = 1;
}

AxisTestLibrary *
axis_test_library_new(guint pool_size)
{
	AxisTestLibrary *t;
	AxisLibraryTuning tuning;
	g_autoptr(GError) error = NULL;

	t = g_new0(AxisTestLibrary, 1);
	t->dir = axis_test_storage_dir_new();
	t->media = g_build_filename(t->dir, "media", NULL);
	t->stash = g_build_filename(t->dir, "stash", NULL);
	axis_test_storage_mkdir(t->media);
	axis_test_storage_mkdir(t->stash);

	t->db = axis_test_db_new();
	if (t->db == NULL) {
		axis_test_storage_dir_free(t->dir);
		g_free(t->media);
		g_free(t->stash);
		g_free(t);
		return NULL;
	}
	t->pool = axis_test_jobs_pool(t->db, pool_size);
	t->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(t->settings, AXIS_STORAGE_MODULE_ID,
		axis_storage_get_setting_table(), &error));
	g_assert_true(axis_settings_load(t->settings, NULL, &error));
	g_assert_no_error(error);
	/* No notice about a disk that is almost full: the scratch disk is the
	 * machine's own, and how full that is must not decide a test. */
	g_assert_true(axis_settings_set(t->settings, AXIS_STORAGE_SETTING_LOW_SPACE, "0", &error));
	g_assert_no_error(error);
	t->storage = axis_storage_new(t->pool, t->settings);
	g_assert_true(axis_storage_load(t->storage, &error));
	g_assert_no_error(error);
	axis_storage_set_timeouts(t->storage, 30000, 30000);
	t->library = axis_library_new(t->pool, t->storage);
	default_tuning(&tuning);
	axis_library_set_tuning(t->library, &tuning);

	return t;
}

void
axis_test_library_free(AxisTestLibrary *t)
{
	gint64 deadline;

	if (t == NULL)
		return;

	/* Every helper and walk thread ends before what they use goes. */
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while ((axis_storage_get_helpers_running(t->storage) > 0 || axis_library_walk_get_live() > 0) &&
	       g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_cmpuint(axis_storage_get_helpers_running(t->storage), ==, 0);
	g_assert_cmpuint(axis_library_walk_get_live(), ==, 0);
	while (g_main_context_iteration(NULL, FALSE))
		;

	axis_library_unref(t->library);
	axis_storage_stop(t->storage);
	axis_storage_unref(t->storage);
	axis_settings_unref(t->settings);
	axis_db_pool_free(t->pool);
	axis_test_db_free(t->db);
	axis_test_storage_dir_free(t->dir);
	g_free(t->media);
	g_free(t->stash);
	g_free(t);
}

void
axis_test_library_tune(
	AxisTestLibrary         *t,
	const AxisLibraryTuning *tuning
){
	AxisLibraryTuning merged;

	default_tuning(&merged);
	if (tuning->batch_entries > 0)
		merged.batch_entries = tuning->batch_entries;
	if (tuning->racy_window_ns > 0)
		merged.racy_window_ns = tuning->racy_window_ns;
	if (tuning->guard_interval_ms > 0)
		merged.guard_interval_ms = tuning->guard_interval_ms;
	if (tuning->stall_ms > 0)
		merged.stall_ms = tuning->stall_ms;
	merged.before_directory = tuning->before_directory;
	merged.after_last = tuning->after_last;
	merged.after_batch = tuning->after_batch;
	merged.before_commit = tuning->before_commit;
	merged.hook_data = tuning->hook_data;
	axis_library_set_tuning(t->library, &merged);
}

gchar *
axis_test_library_disk(
	AxisTestLibrary *t,
	const gchar     *name
){
	gchar *path = g_build_filename(t->media, name, NULL);
	g_autofree gchar *file = g_build_filename(path, "readme.txt", NULL);

	axis_test_storage_mkdir(path);
	axis_test_storage_write(file, "this disk holds media\n");
	return path;
}

gint64
axis_test_library_add_volume(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *name
){
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	g_assert_true(axis_storage_inspect_volume(t->storage, path, &inspection, &error));
	g_assert_no_error(error);
	if (inspection->plan == AXIS_VOLUME_PLAN_REFUSE)
		g_error("cannot add %s: %s", path, inspection->refusal);

	memset(&request, 0, sizeof(request));
	request.path = inspection->path;
	request.name = name;
	request.plan = inspection->plan;
	request.marker_uuid = inspection->marker_uuid;
	request.dev = inspection->dev;
	request.ino = inspection->ino;
	request.fsid = inspection->fsid;
	request.total_bytes = inspection->total_bytes;
	request.not_mount_point_ok = TRUE;
	/* The reason first: a failure here must say why. */
	if (!axis_storage_add_volume(t->storage, &request, &id, &error))
		g_error("cannot add the volume at %s: %s", path, error != NULL ? error->message : "no reason given");
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

gint64
axis_test_library_add_volume_beside(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *name
){
	axis_test_db_exec(t->db, "UPDATE volume SET fsid = '', device = ''");
	return axis_test_library_add_volume(t, path, name);
}

gint64
axis_test_library_add_root(
	AxisTestLibrary *t,
	gint64           volume_id,
	const gchar     *volume_path,
	const gchar     *rel,
	AxisMediaType    type
){
	g_autofree gchar *dir = g_build_filename(volume_path, rel, NULL);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	axis_test_storage_mkdir(dir);
	g_assert_true(axis_storage_add_root(t->storage, volume_id, rel, type, NULL, &id, &error));
	g_assert_no_error(error);
	return id;
}

AxisScanResult *
axis_test_library_scan(
	AxisTestLibrary *t,
	gint64           root_id
){
	AxisScanResult *result = NULL;
	g_autoptr(GError) error = NULL;

	if (!axis_library_scan_root(t->library, root_id, NULL, &result, &error))
		g_error("the scan did not run: %s", error != NULL ? error->message : "?");
	g_assert_nonnull(result);
	return result;
}

AxisScanResult *
axis_test_library_scan_expect(
	AxisTestLibrary *t,
	gint64           root_id,
	AxisScanStatus   status,
	AxisScanReason   reason
){
	AxisScanResult *result = axis_test_library_scan(t, root_id);

	if (result->status != status || result->reason != reason)
		g_error("expected a scan %s/%s, got %s/%s: %s", axis_scan_status_to_string(status),
			reason != AXIS_SCAN_REASON_NONE ? axis_scan_reason_to_string(reason) : "-",
			axis_scan_status_to_string(result->status),
			result->reason != AXIS_SCAN_REASON_NONE ? axis_scan_reason_to_string(result->reason) : "-",
			result->message != NULL ? result->message : "");
	return result;
}

void
axis_test_library_unplug(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *as
){
	g_autofree gchar *away = g_build_filename(t->stash, as, NULL);

	g_assert_cmpint(g_rename(path, away), ==, 0);
	axis_test_storage_mkdir(path);
}

void
axis_test_library_plug(
	AxisTestLibrary *t,
	const gchar     *path,
	const gchar     *as
){
	g_autofree gchar *away = g_build_filename(t->stash, as, NULL);

	g_assert_cmpint(g_rmdir(path), ==, 0);
	g_assert_cmpint(g_rename(away, path), ==, 0);
}

gchar *
axis_test_library_q(
	AxisTestLibrary *t,
	const gchar     *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);

	return axis_test_db_query(t->db, sql);
}

gint64
axis_test_library_n(
	AxisTestLibrary *t,
	const gchar     *format,
	...
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *value = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);

	value = axis_test_db_query(t->db, sql);
	g_assert_nonnull(value);
	return g_ascii_strtoll(value, NULL, 10);
}

gchar *
axis_test_library_index(
	AxisTestLibrary *t,
	gint64           root_id
){
	return axis_test_library_q(t,
		"SELECT coalesce(string_agg(concat_ws('|', id, rel_path, name_invalid_utf8, kind, size,"
		" mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, link_target, pinned,"
		" pinned_links, pinned_downloads, state, first_seen_generation, changed, changed_generation,"
		" content_changed_at, created_at, moved_at, suspected_at, suspected_scan_id, missing_at),"
		" E'\\n' ORDER BY rel_path), '') FROM library_file WHERE root_id = %" G_GINT64_FORMAT, root_id);
}

void
axis_test_write_named(
	const gchar *dir,
	const gchar *name,
	const gchar *content
){
	gint dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	gint fd;
	gsize length = strlen(content);

	g_assert_cmpint(dfd, >=, 0);
	fd = openat(dfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0)
		g_error("cannot create a file in %s: %s", dir, g_strerror(errno));
	g_assert_cmpint(write(fd, content, length), ==, (gssize)length);
	close(fd);
	close(dfd);
}

void
axis_test_set_mtime(
	const gchar *path,
	gint64       seconds,
	glong        nanoseconds
){
	struct timespec times[2];

	times[0].tv_sec = 0;
	times[0].tv_nsec = UTIME_OMIT;
	times[1].tv_sec = (time_t)seconds;
	times[1].tv_nsec = nanoseconds;
	g_assert_cmpint(utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW), ==, 0);
}

void
axis_test_make_files(
	const gchar *dir,
	guint        n
){
	gint dfd;
	guint i;

	axis_test_storage_mkdir(dir);
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	g_assert_cmpint(dfd, >=, 0);
	for (i = 0; i < n; i++) {
		gchar name[32];
		gint fd;

		g_snprintf(name, sizeof(name), "f%u", i);
		fd = openat(dfd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		g_assert_cmpint(fd, >=, 0);
		close(fd);
	}
	close(dfd);
}
