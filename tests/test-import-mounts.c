/*
 * test-import-mounts.c - the import pipeline between two real filesystems:
 * a download on one volume and the films' root on another, so every file is
 * copied and verified
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-import-pipeline:
 *   6.1  a Usenet download on another volume: copied, fsynced, read back and
 *        compared (its SHA-256 recorded), then removed from its client; the
 *        download's file untouched; the volume warning (4.4)
 *   6.2  a failure while copying, and a copy that reads back wrong, leave no
 *        temporary file and the library as it was
 *   6.10 "import anyway" cannot override a copy that failed to verify
 *   6.11 a run that dies with the copy made is carried on by the next
 *   6.8  a library volume mounted read-only makes the import wait, with
 *        nothing written; too little room makes it wait
 *   6.12 import.hardlink_policy = hardlink_only blocks the copy
 *
 * Each test runs in a child process in its own user and mount namespace
 * (as test-storage-mounts.c does), with two small tmpfs filesystems as the
 * disks: nothing is mounted on the host.  Where unprivileged namespaces are
 * not available the tests skip.  The pipeline's owner here is a stand-in
 * for the movies module: one film, Hearth (1995).
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
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "core/axis-settings.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality.h"
#include "storage/axis-storage.h"
#include "storage/axis-volume-write.h"

#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD
#define TOOL     "tests/fixtures/import/fake-ffprobe.sh"
#define RELEASE  "Hearth.1995.1080p.BluRay.x264-GRP"
#define FOLDER   "Hearth (1995) {tmdb-6001}"
#define FILE_NAME "Hearth (1995) {tmdb-6001} [BluRay 1080p][x264]-GRP.mkv"
#define MODULE   "testfilms"

/* ── Entering a namespace ───────────────────────────────────────── */

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
mount_tmpfs(const gchar *where)
{
	if (mount("axis-test", where, "tmpfs", 0, "size=16m,mode=0755") != 0)
		g_error("mount tmpfs on %s: %s", where, g_strerror(errno));
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
	AxisTestDb           *db;
	AxisDbPool           *pool;
	AxisSettings         *storage_settings;
	AxisSettings         *settings;
	AxisStorage          *storage;
	AxisDownloads        *downloads;
	AxisTestDownloadFake *fake;
	AxisProbe            *probe;
	AxisImport           *import;
	gchar                *tool[3];
	gchar                *dir;
	gchar                *disk_a;	/* the downloads' volume */
	gchar                *disk_b;	/* the library's volume */
	gchar                *films;
	gint64                volume_a;
	gint64                volume_b;
	gint64                root_id;
	gint64                client_id;
	guint                 linked;
	/* The hook. */
	const gchar          *at;
	AxisImportHookVerdict verdict;
	gboolean              corrupt;
	guint                 seen;
	/* When set: deleted at @at (something else deleting a download's file). */
	gchar                *unlink_at;
	/* When set: written over in place at @at, the same size (a client
	 * repairing a piece, a tag editor). */
	gchar                *rewrite_at;
	/* When set: another file renamed over it at @at (a tool that writes a
	 * new file and renames it into place). */
	gchar                *replace_at;
} World;

static gboolean
owner_resolve(
	gpointer            data,
	AxisDbConn         *conn,
	gint64              download_id,
	gint64              owner_id,
	AxisImportTarget  **target_out,
	gchar             **problem,
	GError            **error
){
	World *world = data;
	AxisImportTarget *target = axis_import_target_new();

	*problem = NULL;
	target->owner_id = 1;
	target->label = g_strdup("Hearth (1995)");
	target->title = g_strdup("Hearth");
	target->year = 1995;
	target->tmdb_id = g_strdup("6001");
	target->grab_release = g_strdup(RELEASE);
	target->root_id = world->root_id;
	target->profile_ok = axis_quality_builtin_profile(AXIS_QUALITY_DEFAULT_SLUG, &target->profile);
	*target_out = target;
	return TRUE;
}

static gboolean
owner_name_matches(
	gpointer                data,
	const AxisImportTarget *target,
	const AxisRelease      *parsed,
	gchar                 **reason
){
	*reason = NULL;
	return TRUE;
}

static gboolean
owner_link(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const AxisImportLinked *linked,
	GError                **error
){
	World *world = data;

	world->linked++;
	return TRUE;
}

static gboolean
owner_note(
	gpointer                data,
	AxisDbConn             *conn,
	const AxisImportTarget *target,
	const gchar            *kind,
	const gchar            *text,
	const gchar            *user,
	GError                **error
){
	return TRUE;
}

static const AxisImportOwnerInfo owner_info = {
	MODULE, "film", owner_resolve, owner_name_matches, owner_link, owner_note, NULL, NULL, NULL, NULL, NULL
};

static gchar *
temp_path(
	World  *world,
	gint64  file_id
){
	g_autofree gchar *name = g_strdup_printf(".axis-partial-%" G_GINT64_FORMAT, file_id);

	return g_build_filename(world->films, FOLDER, name, NULL);
}

static AxisImportHookVerdict
hook_fn(
	const gchar *step_name,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
){
	World *world = data;

	if (world->at == NULL || strcmp(step_name, world->at) != 0 || world->seen++ > 0)
		return AXIS_IMPORT_HOOK_GO;
	if (world->unlink_at != NULL) {
		g_assert_cmpint(g_unlink(world->unlink_at), ==, 0);
		return AXIS_IMPORT_HOOK_GO;
	}
	if (world->rewrite_at != NULL) {
		static const guchar other[] = "AXISFAKE written over";
		gint fd;

		/* A tick later than the file's last change, so that its times
		 * show it on any kernel. */
		g_usleep(20000);
		fd = open(world->rewrite_at, O_WRONLY | O_CLOEXEC);
		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(pwrite(fd, other, sizeof(other) - 1, 100), ==, (gssize)(sizeof(other) - 1));
		close(fd);
		return AXIS_IMPORT_HOOK_GO;
	}
	if (world->replace_at != NULL) {
		g_autofree gchar *next = g_strconcat(world->replace_at, ".new", NULL);

		g_usleep(20000);
		g_assert_true(g_file_set_contents(next, "a newer download of the film\n", -1, NULL));
		g_assert_cmpint(g_rename(next, world->replace_at), ==, 0);
		return AXIS_IMPORT_HOOK_GO;
	}
	if (world->corrupt) {
		/* What was written changes on the disk before it is read back. */
		g_autofree gchar *path = temp_path(world, file_id);
		gint fd = open(path, O_WRONLY | O_CLOEXEC);
		guchar byte = 0x5a;

		g_assert_cmpint(fd, >=, 0);
		g_assert_cmpint(pwrite(fd, &byte, 1, 4000), ==, 1);
		close(fd);
		return AXIS_IMPORT_HOOK_GO;
	}
	return world->verdict;
}

static gint64
add_volume(
	World       *world,
	const gchar *path,
	const gchar *name
){
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	g_assert_true(axis_storage_inspect_volume(world->storage, path, &inspection, &error));
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
	g_assert_true(axis_storage_add_volume(world->storage, &request, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
write_media(
	const gchar *path,
	const gchar *line,
	gsize        size
){
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree guchar *bytes = g_malloc(size);
	gsize n = strlen(line);
	gsize i;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	memcpy(bytes, line, n);
	bytes[n] = '\n';
	for (i = n + 1; i < size; i++)
		bytes[i] = (guchar)((i * 131 + 17) & 0xff);
	g_assert_true(g_file_set_contents(path, (const gchar *)bytes, (gssize)size, NULL));
}

static World *
world_new(void)
{
	World *world = g_new0(World, 1);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *films = NULL;
	AxisDownloadClientSpec client;
	AxisProbeTuning tuning;

	axis_test_log_init();
	axis_test_log_reset();
	world->dir = axis_test_storage_dir_new();
	world->disk_a = g_build_filename(world->dir, "disk-a", NULL);
	world->disk_b = g_build_filename(world->dir, "disk-b", NULL);
	axis_test_storage_mkdir(world->disk_a);
	axis_test_storage_mkdir(world->disk_b);
	mount_tmpfs(world->disk_a);
	mount_tmpfs(world->disk_b);

	world->db = axis_test_db_new();
	g_assert_nonnull(world->db);
	world->pool = axis_test_jobs_pool(world->db, 8);
	world->storage_settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(world->storage_settings, AXIS_STORAGE_MODULE_ID,
		axis_storage_get_setting_table(), &error));
	g_assert_true(axis_settings_load(world->storage_settings, NULL, &error));
	g_assert_true(axis_settings_set(world->storage_settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	world->storage = axis_storage_new(world->pool, world->storage_settings);
	g_assert_true(axis_storage_load(world->storage, &error));
	g_assert_no_error(error);
	axis_storage_set_timeouts(world->storage, 30000, 30000);
	while (g_main_context_iteration(NULL, FALSE))
		;
	world->volume_a = add_volume(world, world->disk_a, "Downloads disk");
	world->volume_b = add_volume(world, world->disk_b, "Films disk");
	films = g_build_filename(world->disk_b, "films", NULL);
	axis_test_storage_mkdir(films);
	world->films = g_steal_pointer(&films);
	g_assert_true(axis_storage_add_root(world->storage, world->volume_b, "films", AXIS_MEDIA_MOVIES, NULL,
		&world->root_id, &error));
	g_assert_no_error(error);

	world->settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(world->settings, AXIS_DOWNLOAD_MODULE_ID, axis_downloads_get_setting_table(),
		&error));
	g_assert_true(axis_settings_add_table(world->settings, AXIS_IMPORT_MODULE_ID, axis_import_get_setting_table(),
		&error));
	g_assert_true(axis_settings_load(world->settings, world->pool, &error));
	/* The disks are small: a floor that fits them. */
	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "1MiB", &error));
	g_assert_no_error(error);
	/* These tests are of the copies that leave the download's file
	 * (import.take_files "hardlink"); the moves across volumes of the default
	 * have tests of their own, which say "move". */
	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_TAKE, AXIS_IMPORT_TAKE_HARDLINK, &error));
	g_assert_no_error(error);
	world->downloads = axis_downloads_new(world->pool, world->storage, world->settings);
	g_assert_true(axis_downloads_add_kind(world->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	g_assert_no_error(error);
	world->fake = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(world->fake);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(world->downloads, 0, &client, &world->client_id, &error));
	g_assert_no_error(error);

	world->probe = axis_probe_new(world->pool, world->storage, NULL);
	world->tool[0] = g_strdup("/bin/sh");
	world->tool[1] = g_canonicalize_filename(TOOL, NULL);
	memset(&tuning, 0, sizeof(tuning));
	tuning.tool_command = (const gchar *const *)world->tool;
	axis_probe_set_tuning(world->probe, &tuning);
	world->import = axis_import_new(world->pool, world->settings, world->storage, world->downloads, world->probe);
	axis_import_add_owner(world->import, &owner_info, world);
	axis_import_set_hook(world->import, hook_fn, world);
	return world;
}

static void
world_free(World *world)
{
	axis_import_set_hook(world->import, NULL, NULL);
	axis_import_unref(world->import);
	axis_probe_unref(world->probe);
	axis_downloads_stop(world->downloads);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_downloads_unref(world->downloads);
	axis_test_download_fake_free(world->fake);
	axis_settings_unref(world->settings);
	axis_storage_stop(world->storage);
	axis_storage_unref(world->storage);
	axis_settings_unref(world->storage_settings);
	axis_db_pool_free(world->pool);
	axis_test_db_free(world->db);
	while (g_main_context_iteration(NULL, FALSE))
		;
	umount2(world->disk_a, MNT_DETACH);
	umount2(world->disk_b, MNT_DETACH);
	axis_test_storage_dir_free(world->dir);
	g_free(world->tool[0]);
	g_free(world->tool[1]);
	g_free(world->disk_a);
	g_free(world->disk_b);
	g_free(world->films);
	g_free(world->unlink_at);
	g_free(world->rewrite_at);
	g_free(world->replace_at);
	g_free(world);
	axis_test_log_reset();
}

static gchar *
q(
	World       *world,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	World       *world,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(world->db, sql);
}

static void
assert_q(
	World       *world,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(world->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

/* The download of Hearth on disk A (a film and a subtitle), its client's
 * item, and its import.  Returns the import. */
static gint64
hearth_download(
	World   *world,
	gint64  *download_out
){
	g_autofree gchar *dir = g_build_filename(world->disk_a, "complete", RELEASE, NULL);
	g_autofree gchar *film = g_build_filename(dir, RELEASE ".mkv", NULL);
	g_autofree gchar *sub = g_build_filename(dir, RELEASE ".en.srt", NULL);
	g_autofree gchar *id = NULL;
	g_autoptr(GError) error = NULL;
	AxisDbConn *conn;
	gint64 download;
	gint64 import_id = 0;

	write_media(film, "AXISFAKE duration=10200 width=1920 height=800 vcodec=h264", 3 * 1024 * 1024 + 12345);
	g_assert_true(g_file_set_contents(sub, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n", -1, NULL));
	axis_test_download_fake_put(world->fake, "SABnzbd_nzo_hearth", RELEASE, "axis", NULL, "completed", 1000, 1000,
		"/data/complete/" RELEASE, NULL);
	id = q(world, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, module,"
		" state, client_path, volume_id, rel_path, sent_at, completed_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd',"
		" 'SABnzbd_nzo_hearth', 'usenet', 'file', '" RELEASE "', '" MODULE "', 'completed', '/data/complete/" RELEASE "',"
		" %" G_GINT64_FORMAT ", 'complete/" RELEASE "', now(), now()) RETURNING id", world->client_id, world->volume_a);
	download = g_ascii_strtoll(id, NULL, 10);
	if (download_out != NULL)
		*download_out = download;
	conn = axis_db_pool_acquire(world->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_import_request(world->import, conn, MODULE, download, &import_id, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(world->pool, conn);
	g_assert_cmpint(import_id, >, 0);
	return import_id;
}

static const gchar *
run(
	World  *world,
	gint64  import_id
){
	g_autoptr(GError) error = NULL;
	AxisImportReport report;

	memset(&report, 0, sizeof(report));
	if (!axis_import_run(world->import, import_id, NULL, &report, &error))
		g_error("the import run failed: %s", error != NULL ? error->message : "?");
	if (g_strcmp0(report.state, "imported") != 0) {
		g_autofree gchar *why = q(world, "SELECT coalesce(reason, '') || ': ' || coalesce(reason_text, '') FROM import"
			" WHERE id = %" G_GINT64_FORMAT, import_id);

		g_printerr("# import %" G_GINT64_FORMAT ": %s %s\n", import_id, report.state, why);
	}
	return report.state;
}

static gchar *
state_of(
	World  *world,
	gint64  import_id
){
	return q(world, "SELECT state || coalesce('/' || reason, '') FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
}

/* What a tree holds: names, kinds, modes, sizes and contents (not the
 * folders' times: a folder made and removed again changes its parent's). */
static void
tree_at(
	const gchar *path,
	const gchar *shown,
	GString     *out
){
	GStatBuf st;

	if (g_lstat(path, &st) != 0) {
		g_string_append_printf(out, "%s gone\n", shown);
		return;
	}
	if (S_ISDIR(st.st_mode)) {
		GDir *dir = g_dir_open(path, 0, NULL);
		g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
		const gchar *name;
		guint i;

		g_string_append_printf(out, "%s/ mode=%o\n", shown, (guint)(st.st_mode & 07777));
		while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
			g_ptr_array_add(names, g_strdup(name));
		if (dir != NULL)
			g_dir_close(dir);
		g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
		for (i = 0; i < names->len; i++) {
			g_autofree gchar *child = g_build_filename(path, g_ptr_array_index(names, i), NULL);
			g_autofree gchar *child_shown = g_strconcat(shown, "/", (const gchar *)g_ptr_array_index(names, i), NULL);

			tree_at(child, child_shown, out);
		}
	} else if (S_ISREG(st.st_mode)) {
		g_autofree gchar *contents = NULL;
		g_autofree gchar *sum = NULL;
		gsize length = 0;

		g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
		sum = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
		g_string_append_printf(out, "%s mode=%o size=%" G_GINT64_FORMAT " ino=%" G_GUINT64_FORMAT " sha=%s\n", shown,
			(guint)(st.st_mode & 07777), (gint64)st.st_size, (guint64)st.st_ino, sum);
	} else {
		g_string_append_printf(out, "%s other\n", shown);
	}
}

/* What the films' root holds. */
static gchar *
films_tree(World *world)
{
	GString *out = g_string_new(NULL);

	tree_at(world->films, ".", out);
	return g_string_free(out, FALSE);
}

static guint
count_temporaries(const gchar *dir)
{
	GDir *listing = g_dir_open(dir, 0, NULL);
	const gchar *name;
	guint n = 0;

	if (listing == NULL)
		return 0;
	while ((name = g_dir_read_name(listing)) != NULL) {
		if (g_str_has_prefix(name, ".axis-partial-"))
			n++;
	}
	g_dir_close(listing);
	return n;
}

static gchar *
sha_of(const gchar *path)
{
	g_autofree gchar *contents = NULL;
	gsize length = 0;

	g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
	return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
}

/* ── Tests ──────────────────────────────────────────────────────── */

static void
copy_usenet(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_dir = g_build_filename(world->disk_a, "complete", RELEASE, NULL);
	g_autofree gchar *placed = g_build_filename(world->films, FOLDER, FILE_NAME, NULL);
	g_autofree gchar *placed_sub = g_build_filename(world->films, FOLDER,
		"Hearth (1995) {tmdb-6001} [BluRay 1080p][x264]-GRP.en.srt", NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	g_autofree gchar *recorded = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *text = NULL;
	GStatBuf source_st;
	GStatBuf placed_st;
	gint64 download = 0;
	gint64 id;

	/* The client's folder is on disk A and the films' root on disk B: a
	 * warning on the pages that show them. */
	sql = g_strdup_printf("INSERT INTO download_path_map (client_id, client_prefix, volume_id, rel_path) VALUES (%"
		G_GINT64_FORMAT ", '/data', %" G_GINT64_FORMAT ", '')", world->client_id, world->volume_a);
	axis_test_db_exec(world->db, sql);
	g_assert_true(axis_import_volume_warnings(world->pool, &warnings, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(warnings->len, ==, 1);
	text = axis_import_volume_warning_text(g_ptr_array_index(warnings, 0));
	g_assert_nonnull(strstr(text, "copied"));
	g_assert_nonnull(strstr(text, "Films disk"));

	id = hearth_download(world, &download);
	before = axis_test_storage_digest(source_dir);
	source_sha = sha_of(source);
	g_assert_cmpstr(run(world, id), ==, "imported");

	/* A copy: another file, the same bytes, verified and recorded. */
	source_st.st_ino = 0;
	g_assert_cmpint(g_lstat(source, &source_st), ==, 0);
	g_assert_cmpint(g_lstat(placed, &placed_st), ==, 0);
	g_assert_cmpuint((guint)placed_st.st_nlink, ==, 1);
	g_assert_cmpuint((guint64)placed_st.st_size, ==, (guint64)source_st.st_size);
	g_assert_cmpuint(placed_st.st_mode & 0777, ==, 0644);
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	g_free(sql);
	sql = g_strdup_printf("SELECT checksum FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'",
		id);
	recorded = axis_test_db_query(world->db, sql);
	g_assert_cmpstr(recorded, ==, source_sha);
	g_assert_true(g_file_test(placed_sub, G_FILE_TEST_IS_REGULAR));
	g_free(sql);
	sql = g_strdup_printf("SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_q(world, "copy", sql);
	g_free(sql);
	sql = g_strdup_printf("SELECT mode_text LIKE '%%verified copy from Downloads disk to Films disk%%' FROM import"
		" WHERE id = %" G_GINT64_FORMAT, id);
	assert_q(world, "t", sql);
	g_assert_cmpuint(world->linked, ==, 1);
	{
		g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);

		g_assert_cmpuint(count_temporaries(folder), ==, 0);
	}
	/* The download's files as they were, until its client removed it. */
	after = axis_test_storage_digest(source_dir);
	g_assert_cmpstr(after, ==, before);
	g_free(sql);
	sql = g_strdup_printf("SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_q(world, "done", sql);
	g_assert_cmpuint(axis_test_download_fake_count(world->fake, "remove", "SABnzbd_nzo_hearth"), ==, 1);
	world_free(world);
}

static void
test_copy_usenet(void)
{
	in_namespace(copy_usenet);
}

/* A failure while copying, and a copy that reads back wrong. */
static void
copy_faults(void)
{
	World *world = world_new();
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *sql = NULL;
	gint64 id;

	id = hearth_download(world, NULL);
	before = films_tree(world);

	world->at = "copying";
	world->verdict = AXIS_IMPORT_HOOK_FAIL;
	world->seen = 0;
	g_assert_cmpstr(run(world, id), ==, "waiting");
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "waiting/place_failed");
	after = films_tree(world);
	g_assert_cmpstr(after, ==, before);
	g_assert_false(g_file_test(folder, G_FILE_TEST_EXISTS));

	/* Corrupted between writing and reading back: the checksums differ,
	 * nothing is left. */
	world->at = "verifying";
	world->corrupt = TRUE;
	world->seen = 0;
	g_assert_cmpstr(run(world, id), ==, "waiting");
	g_free(state);
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "waiting/place_failed");
	sql = g_strdup_printf("SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	text = axis_test_db_query(world->db, sql);
	g_assert_nonnull(strstr(text, "checksum differs"));
	g_free(after);
	after = films_tree(world);
	g_assert_cmpstr(after, ==, before);

	/* A third copy that does not verify: a person must look, and "import
	 * anyway" cannot override a failed verification; "look again" can try
	 * once more. */
	world->seen = 0;
	g_assert_cmpstr(run(world, id), ==, "blocked");
	g_free(state);
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "blocked/place_failed");
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_import_act(world->import, id, AXIS_IMPORT_ACT_ANYWAY, -1, 0, "root", NULL, &error));
		g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
		g_clear_error(&error);
		g_assert_true(axis_import_act(world->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
		g_assert_no_error(error);
	}
	g_free(after);
	after = films_tree(world);
	g_assert_cmpstr(after, ==, before);

	/* A run that dies with the copy made and not named yet: the next one
	 * removes it (the only name of its file, but its source is still there)
	 * and copies again. */
	world->corrupt = FALSE;
	world->at = "copied";
	world->verdict = AXIS_IMPORT_HOOK_CRASH;
	world->seen = 0;
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(axis_import_run(world->import, id, NULL, NULL, &error));
		g_assert_nonnull(error);
	}
	g_assert_cmpuint(count_temporaries(folder), ==, 1);
	world->at = NULL;
	g_assert_cmpstr(run(world, id), ==, "imported");
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	g_assert_cmpuint(world->linked, ==, 1);
	world_free(world);
}

static void
test_copy_faults(void)
{
	in_namespace(copy_faults);
}

static void
refusals(void)
{
	World *world = world_new();
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *state = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id;

	id = hearth_download(world, NULL);
	before = films_tree(world);

	/* hardlink_only: no copy, a person decides. */
	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_HARDLINK, "hardlink_only", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(run(world, id), ==, "blocked");
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "blocked/hardlink_only");
	after = films_tree(world);
	g_assert_cmpstr(after, ==, before);
	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_HARDLINK, "hardlink_or_copy", &error));
	g_assert_no_error(error);
	g_assert_true(axis_import_act(world->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);

	/* Not enough room for the copy and the floor. */
	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "1GiB", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(run(world, id), ==, "waiting");
	g_free(state);
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "waiting/no_space");
	g_free(after);
	after = films_tree(world);
	g_assert_cmpstr(after, ==, before);
	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "1MiB", &error));
	g_assert_no_error(error);

	/* The library's disk mounted read-only: the import waits; nothing is
	 * written. */
	remount(world->disk_b, TRUE);
	g_assert_cmpstr(run(world, id), ==, "waiting");
	g_free(state);
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "waiting/volume_read_only");
	g_free(after);
	after = films_tree(world);
	g_assert_cmpstr(after, ==, before);
	remount(world->disk_b, FALSE);
	g_assert_cmpstr(run(world, id), ==, "imported");
	world_free(world);
}

static void
test_refusals(void)
{
	in_namespace(refusals);
}

/* ── Moves across volumes (import-by-move) ──────────────────────── */

static void
moving(World *world)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_settings_set(world->settings, AXIS_IMPORT_SETTING_TAKE, AXIS_IMPORT_TAKE_MOVE, &error));
	g_assert_no_error(error);
}

static void
world_cleanup(World *world)
{
	g_autoptr(GError) error = NULL;
	AxisImportCleanupReport report;

	memset(&report, 0, sizeof(report));
	g_assert_true(axis_import_run_cleanup(world->import, 0, &report, &error));
	g_assert_no_error(error);
}

static void
assert_wf(
	World       *world,
	const gchar *expected,
	const gchar *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
assert_wf(
	World       *world,
	const gchar *expected,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	assert_q(world, expected, sql);
}

/* A run that dies at @step: it fails, saying where. */
static void
run_dies(
	World       *world,
	gint64       id,
	const gchar *step
){
	g_autoptr(GError) error = NULL;
	AxisImportReport report;

	world->at = step;
	world->verdict = AXIS_IMPORT_HOOK_CRASH;
	world->seen = 0;
	memset(&report, 0, sizeof(report));
	g_assert_false(axis_import_run(world->import, id, NULL, &report, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, step));
	world->at = NULL;
	world->verdict = AXIS_IMPORT_HOOK_GO;
}

/* Spec (import): a Usenet download on another volume than the library is
 * moved by a copy that is verified (read back, its SHA-256 compared) and
 * fsynced, and the download's files are deleted once the import is recorded
 * and the job removed from its client. */
static void
move_across(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".en.srt", NULL);
	g_autofree gchar *placed = g_build_filename(world->films, FOLDER, FILE_NAME, NULL);
	g_autofree gchar *placed_sub = g_build_filename(world->films, FOLDER,
		"Hearth (1995) {tmdb-6001} [BluRay 1080p][x264]-GRP.en.srt", NULL);
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	source_sha = sha_of(source);
	g_assert_cmpstr(run(world, id), ==, "imported");
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	g_assert_true(g_file_test(placed_sub, G_FILE_TEST_IS_REGULAR));
	assert_wf(world, "move_copy", "SELECT mode FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_wf(world, "t", "SELECT mode_text LIKE '%%a move from Downloads disk to Films disk%%' FROM import"
		" WHERE id = %" G_GINT64_FORMAT, id);
	assert_wf(world, source_sha, "SELECT checksum FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND role = 'film'", id);
	/* Out of the download, once the import was recorded. */
	assert_wf(world, "done", "SELECT client_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	assert_wf(world, "deleted|deleted", "SELECT string_agg(source_state, '|' ORDER BY position) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(source_sub, G_FILE_TEST_EXISTS));
	{
		g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);

		g_assert_cmpuint(count_temporaries(folder), ==, 0);
	}
	world_free(world);
}

static void
test_move_across(void)
{
	in_namespace(move_across);
}

/* Spec (import): the run dies once the import is recorded, before the
 * download's files are deleted: the hourly cleanup deletes them, each only
 * while it is the very file copied and its copy is in the library; one
 * replaced since by another file, and one whose copy is not in the library
 * any more, are kept, saying why. */
static void
move_across_after_crash(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".en.srt", NULL);
	g_autofree gchar *placed = g_build_filename(world->films, FOLDER, FILE_NAME, NULL);
	g_autofree gchar *note = NULL;
	g_autofree gchar *sql = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	run_dies(world, id, "recorded");
	assert_wf(world, "imported", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, id);
	/* Recorded, and the download's files still there, to be deleted. */
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	assert_wf(world, "pending|pending", "SELECT string_agg(source_state, '|' ORDER BY position) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	/* The subtitle replaced by another file; the film's copy gone from the
	 * library. */
	g_assert_cmpint(g_unlink(source_sub), ==, 0);
	g_assert_true(g_file_set_contents(source_sub, "1\n00:00:01,000 --> 00:00:02,000\nAnother.\n", -1, NULL));
	g_assert_cmpint(g_unlink(placed), ==, 0);
	world_cleanup(world);
	assert_wf(world, "kept|kept", "SELECT string_agg(source_state, '|' ORDER BY position) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	sql = g_strdup_printf("SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	note = axis_test_db_query(world->db, sql);
	g_assert_nonnull(strstr(note, "may be the one copy left"));
	g_free(note);
	g_free(sql);
	sql = g_strdup_printf("SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'subtitle'", id);
	note = axis_test_db_query(world->db, sql);
	g_assert_nonnull(strstr(note, "another file has its name now"));
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(source_sub, G_FILE_TEST_IS_REGULAR));
	world_free(world);
}

static void
test_move_across_after_crash(void)
{
	in_namespace(move_across_after_crash);
}

/* Spec (import): the run dies after the import is recorded, and the next
 * cleanup deletes the download's files; and something else deleting the
 * film's file while it is copied: the copy, read from the file opened, is
 * complete and verified, and the import completes (never a half-written
 * library file); the download's file is then "gone". */
static void
move_across_source_vanishes(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".en.srt", NULL);
	g_autofree gchar *placed = g_build_filename(world->films, FOLDER, FILE_NAME, NULL);
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	source_sha = sha_of(source);
	world->at = "copying";
	world->unlink_at = g_strdup(source);
	g_assert_cmpstr(run(world, id), ==, "imported");
	world->at = NULL;
	g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	assert_wf(world, source_sha, "SELECT checksum FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND role = 'film'", id);
	assert_wf(world, "gone|deleted", "SELECT string_agg(source_state, '|' ORDER BY role) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	g_assert_false(g_file_test(source_sub, G_FILE_TEST_EXISTS));
	{
		g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);

		g_assert_cmpuint(count_temporaries(folder), ==, 0);
	}
	world_free(world);
}

static void
test_move_across_source_vanishes(void)
{
	in_namespace(move_across_source_vanishes);
}

/* The client keeps Axis's category in /data/complete, mapped to the
 * downloads' volume, so that the folder of a download after its import is
 * looked at by the folder rules. */
static void
with_category(World *world)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_path_map(world->downloads, world->client_id, "/data/complete", world->volume_a,
		"complete", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(world->fake, "/data/complete");
}

/* Review of import-by-move, finding 2: a move across volumes whose
 * subtitle's copy is not in the library when the download's files are to be
 * deleted: the subtitle is kept, and the download's folder with it, the page
 * naming the file and why. */
static void
move_across_kept(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".en.srt", NULL);
	g_autofree gchar *placed_sub = g_build_filename(world->films, FOLDER,
		"Hearth (1995) {tmdb-6001} [BluRay 1080p][x264]-GRP.en.srt", NULL);
	g_autofree gchar *text = NULL;
	gint64 id;

	moving(world);
	with_category(world);
	id = hearth_download(world, NULL);
	run_dies(world, id, "recorded");
	g_assert_cmpint(g_unlink(placed_sub), ==, 0);
	world_cleanup(world);
	assert_wf(world, "deleted|kept", "SELECT string_agg(source_state, '|' ORDER BY role) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(source_sub, G_FILE_TEST_IS_REGULAR));
	assert_wf(world, "left", "SELECT folder_removal FROM import WHERE id = %" G_GINT64_FORMAT, id);
	text = q(world, "SELECT folder_removal_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(text, RELEASE ".en.srt"));
	g_assert_nonnull(strstr(text, "may be the one copy left"));
	world_free(world);
}

static void
test_move_across_kept(void)
{
	in_namespace(move_across_kept);
}

/* Review of import-by-move, finding 7: a move across volumes whose copy got
 * its name but whose folder could not be flushed to the disk: the download's
 * file is kept, saying why. */
static void
move_across_unsynced(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *note = NULL;
	gint64 id;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*could not be flushed to the disk*");
	moving(world);
	id = hearth_download(world, NULL);
	axis_volume_write_test_fail_publish_sync(TRUE);
	g_assert_cmpstr(run(world, id), ==, "imported");
	axis_volume_write_test_fail_publish_sync(FALSE);
	assert_wf(world, "kept|kept", "SELECT string_agg(source_state, '|' ORDER BY role) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	note = q(world, "SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	g_assert_nonnull(note);
	g_assert_nonnull(strstr(note, "could not be flushed to the disk"));
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	world_free(world);
}

static void
test_move_across_unsynced(void)
{
	in_namespace(move_across_unsynced);
}

/* Review of import-by-move, finding 8: the download's file touched after it
 * was copied (its modification time changed; its inode and size the same):
 * it is not the file copied as it was, and is kept. */
static void
move_across_touched(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".en.srt", NULL);
	g_autofree gchar *note = NULL;
	struct timespec times[2];
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	run_dies(world, id, "recorded");
	times[0].tv_sec = 0;
	times[0].tv_nsec = UTIME_OMIT;
	times[1].tv_sec = 1500000000;
	times[1].tv_nsec = 0;
	g_assert_cmpint(utimensat(AT_FDCWD, source, times, 0), ==, 0);
	world_cleanup(world);
	assert_wf(world, "kept|deleted", "SELECT string_agg(source_state, '|' ORDER BY role) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	note = q(world, "SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	g_assert_nonnull(note);
	g_assert_nonnull(strstr(note, "not the file copied"));
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(source_sub, G_FILE_TEST_EXISTS));
	world_free(world);
}

static void
test_move_across_touched(void)
{
	in_namespace(move_across_touched);
}

/* Storage hunt, H1: the film's file is written over in place after it was
 * read for its copy, while the copy is flushed and read back (minutes for a
 * large file).  What the removal requires is the file as it was when its
 * first byte was read, not as it is once the copy is verified: the changed
 * file is kept, saying why, and the library holds what was read. */
static void
move_across_changed_during_read_back(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *source_sub = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".en.srt", NULL);
	g_autofree gchar *placed = g_build_filename(world->films, FOLDER, FILE_NAME, NULL);
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	g_autofree gchar *changed_sha = NULL;
	g_autofree gchar *note = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	source_sha = sha_of(source);
	world->at = "verifying";
	world->rewrite_at = g_strdup(source);
	g_assert_cmpstr(run(world, id), ==, "imported");
	world->at = NULL;
	g_clear_pointer(&world->rewrite_at, g_free);
	/* The copy is what was read; the download's file is the newer one. */
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	changed_sha = sha_of(source);
	g_assert_cmpstr(changed_sha, !=, source_sha);
	assert_wf(world, "kept|deleted", "SELECT string_agg(source_state, '|' ORDER BY role) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	note = q(world, "SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	g_assert_nonnull(note);
	g_assert_nonnull(strstr(note, "not the file copied"));
	g_assert_false(g_file_test(source_sub, G_FILE_TEST_EXISTS));
	/* The hourly cleanup after it keeps it too. */
	world_cleanup(world);
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	world_free(world);
}

static void
test_move_across_changed_during_read_back(void)
{
	in_namespace(move_across_changed_during_read_back);
}

/* Review of the fixes, item 2: another file renamed over the film's name
 * while the film is read for its copy leaves the file read without a name,
 * unwritten, as a deletion would; but its name leads to the newer file.
 * The copy is of what was replaced: discarded, the import says so and waits
 * for another try, nothing is in the library, and the newer file stays. */
static void
move_across_replaced_while_copied(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *placed = g_build_filename(world->films, FOLDER, FILE_NAME, NULL);
	g_autofree gchar *why = NULL;
	g_autofree gchar *content = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	world->at = "copying";
	world->replace_at = g_strdup(source);
	g_assert_cmpstr(run(world, id), ==, "waiting");
	world->at = NULL;
	g_clear_pointer(&world->replace_at, g_free);
	why = q(world, "SELECT reason || ': ' || reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(why);
	g_assert_nonnull(strstr(why, "changed while it was copied"));
	g_assert_false(g_file_test(placed, G_FILE_TEST_EXISTS));
	content = axis_test_storage_read(source);
	g_assert_cmpstr(content, ==, "a newer download of the film\n");
	{
		g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);

		if (g_file_test(folder, G_FILE_TEST_IS_DIR))
			g_assert_cmpuint(count_temporaries(folder), ==, 0);
	}
	world_free(world);
}

static void
test_move_across_replaced_while_copied(void)
{
	in_namespace(move_across_replaced_while_copied);
}

/* Second review, item 4: a copy (hardlink mode across volumes) whose record
 * fails right after the copy was verified: the temporary copy is not left in
 * the library hidden from every later run. */
static void
copy_record_fails(void)
{
	World *world = world_new();
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autoptr(GError) error = NULL;
	AxisImportReport report;
	gint64 id;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_db_exec(world->db, "CREATE FUNCTION axis_test_refuse() RETURNS trigger AS $$ BEGIN RAISE EXCEPTION"
		" 'refused by the test'; END $$ LANGUAGE plpgsql");
	axis_test_db_exec(world->db, "CREATE TRIGGER axis_test_refuse BEFORE UPDATE OF source_mtime_ns ON import_file"
		" FOR EACH ROW EXECUTE FUNCTION axis_test_refuse()");
	id = hearth_download(world, NULL);
	memset(&report, 0, sizeof(report));
	(void)axis_import_run(world->import, id, NULL, &report, &error);
	g_clear_error(&error);
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	assert_wf(world, "0", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND step IS NOT NULL",
		id);
	/* Without the fault, imported once. */
	axis_test_db_exec(world->db, "DROP TRIGGER axis_test_refuse ON import_file");
	axis_test_db_exec(world->db, "UPDATE import SET state = 'waiting', retry_after = NULL WHERE state <> 'imported'");
	g_assert_cmpstr(run(world, id), ==, "imported");
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	world_free(world);
}

static void
test_copy_record_fails(void)
{
	in_namespace(copy_record_fails);
}

/* Second review, item 7: a move across volumes into a folder made for it,
 * whose own entry could not be flushed to the disk: the download's file is
 * kept, saying why. */
static void
move_across_new_folder_unsynced(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *note = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	axis_volume_write_test_fail_folder_sync(TRUE);
	g_assert_cmpstr(run(world, id), ==, "imported");
	axis_volume_write_test_fail_folder_sync(FALSE);
	assert_wf(world, "kept|kept", "SELECT string_agg(source_state, '|' ORDER BY role) FROM import_file"
		" WHERE import_id = %" G_GINT64_FORMAT " AND role IN ('film', 'subtitle')", id);
	note = q(world, "SELECT note FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	g_assert_nonnull(note);
	g_assert_nonnull(strstr(note, "could not be flushed to the disk"));
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	world_free(world);
}

static void
test_move_across_new_folder_unsynced(void)
{
	in_namespace(move_across_new_folder_unsynced);
}

static void
retry_import(
	World  *world,
	gint64  id
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_import_act(world->import, id, AXIS_IMPORT_ACT_RETRY, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
}

static gchar *
film_temp(
	World  *world,
	gint64  id
){
	g_autofree gchar *row = q(world, "SELECT id FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'",
		id);

	g_assert_nonnull(row);
	return temp_path(world, g_ascii_strtoll(row, NULL, 10));
}

/* Third review, item 1: a move across volumes whose download's file
 * something else deletes while it is copied, and whose copy, verified, then
 * cannot be given its name (a person put a file there): the copy, now the
 * only one, keeps its temporary name and its record (the step, the inode, the
 * checksum); the import stops for a person, naming it, and the cleanup keeps
 * it; once the name is free, a retry gives it its name. */
static void
copy_only_copy(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, FILE_NAME, NULL);
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *temp_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *temp = NULL;
	gint64 id;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*a temporary file could not be removed*");
	moving(world);
	id = hearth_download(world, NULL);
	source_sha = sha_of(source);
	axis_test_storage_mkdir(folder);
	g_assert_true(g_file_set_contents(placed, "a file a person put at the film's name\n", -1, NULL));
	world->at = "copying";
	world->seen = 0;
	world->unlink_at = g_strdup(source);
	g_assert_cmpstr(run(world, id), ==, "blocked");
	world->at = NULL;
	g_assert_false(g_file_test(source, G_FILE_TEST_EXISTS));
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "blocked/moved_file_kept");
	temp = film_temp(world, id);
	temp_sha = sha_of(temp);
	g_assert_cmpstr(temp_sha, ==, source_sha);
	text = q(world, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(text, ".axis-partial-"));
	g_assert_nonnull(strstr(text, "may be the only copy"));
	assert_wf(world, "temp|t|t", "SELECT step || '|' || (temp_inode IS NOT NULL)::text::char || '|' ||"
		" (checksum = '%s')::text::char FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'",
		source_sha, id);
	/* The hourly cleanup keeps it, recorded. */
	world_cleanup(world);
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	assert_wf(world, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	/* Room made, a retry: the verified copy gets its name. */
	g_assert_cmpint(g_unlink(placed), ==, 0);
	retry_import(world, id);
	g_assert_cmpstr(run(world, id), ==, "imported");
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	world_free(world);
}

static void
test_copy_only_copy(void)
{
	in_namespace(copy_only_copy);
}

/* Third review, item 1, after a crash: the run died with the verified copy
 * under its temporary name, and the download's file was then deleted by
 * something else: the copy, the only one now, is given its name by the next
 * run, never forgotten. */
static void
copy_crash_source_gone(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, FILE_NAME, NULL);
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	source_sha = sha_of(source);
	run_dies(world, id, "copied");
	g_assert_cmpint(g_unlink(source), ==, 0);
	g_assert_cmpstr(run(world, id), ==, "imported");
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	world_free(world);
}

static void
test_copy_crash_source_gone(void)
{
	in_namespace(copy_crash_source_gone);
}

/* Third review, item 1, a copy not known to be verified (the run died before
 * it recorded the check) whose download's file is not in the download any
 * more: Axis cannot tell that it is whole, so it is neither given its name nor
 * removed; the import stops for a person, naming it; with the download's file
 * back, a retry copies again and imports it once. */
static void
copy_crash_unverified(void)
{
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *away = g_build_filename(world->disk_a, "elsewhere.mkv", NULL);
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autofree gchar *placed = g_build_filename(folder, FILE_NAME, NULL);
	g_autofree gchar *source_sha = NULL;
	g_autofree gchar *placed_sha = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *temp = NULL;
	gint64 id;

	moving(world);
	id = hearth_download(world, NULL);
	source_sha = sha_of(source);
	run_dies(world, id, "copied");
	/* As a run killed before it recorded the check. */
	axis_test_db_exec(world->db, "UPDATE import_file SET checksum = NULL");
	g_assert_cmpint(g_rename(source, away), ==, 0);
	temp = film_temp(world, id);
	g_assert_cmpstr(run(world, id), ==, "blocked");
	state = state_of(world, id);
	g_assert_cmpstr(state, ==, "blocked/moved_file_kept");
	text = q(world, "SELECT reason_text FROM import WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_nonnull(strstr(text, "not yet read back"));
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	g_assert_false(g_file_test(placed, G_FILE_TEST_EXISTS));
	assert_wf(world, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	/* The download's file back: copied again, imported once. */
	g_assert_cmpint(g_rename(away, source), ==, 0);
	retry_import(world, id);
	g_assert_cmpstr(run(world, id), ==, "imported");
	placed_sha = sha_of(placed);
	g_assert_cmpstr(placed_sha, ==, source_sha);
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	world_free(world);
}

static void
test_copy_crash_unverified(void)
{
	in_namespace(copy_crash_unverified);
}

/* Spec (import): the hourly cleanup removes the temporary copy of an import no
 * run takes up (dismissed, blocked) while the download's file it was copied
 * from is still there, as it promises (finding 7 of the stage 2 bug hunt: it
 * kept every copy as if it were the file's only name, so a film's worth of
 * space stayed taken in a hidden file of the library for good).  A copy whose
 * download's file is gone may be the only copy: Dismiss is then refused, and
 * the cleanup keeps it. */
static void
dismissed_copy_removed(void)
{
	g_autoptr(GError) error = NULL;
	World *world = world_new();
	g_autofree gchar *download = g_build_filename(world->disk_a, "complete", RELEASE, NULL);
	g_autofree gchar *source = g_build_filename(download, RELEASE ".mkv", NULL);
	g_autofree gchar *away = g_build_filename(world->disk_a, "elsewhere", NULL);
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autofree gchar *temp = NULL;
	gint64 id;

	id = hearth_download(world, NULL);
	run_dies(world, id, "copied");
	temp = film_temp(world, id);
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	/* The download's folder elsewhere (its file untouched, its times as
	 * they were): the copy may be the only one, so the dismissal is refused
	 * and the cleanup keeps it. */
	g_assert_cmpint(g_rename(download, away), ==, 0);
	g_assert_false(axis_import_act(world->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_error(error, AXIS_IMPORT_ERROR, AXIS_IMPORT_ERROR_REFUSED);
	g_assert_nonnull(strstr(error->message, "only copy"));
	g_clear_error(&error);
	world_cleanup(world);
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	/* Back: dismissed, and the copy removed by the cleanup. */
	g_assert_cmpint(g_rename(away, download), ==, 0);
	g_assert_true(axis_import_act(world->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	world_cleanup(world);
	g_assert_true(g_file_test(source, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(count_temporaries(folder), ==, 0);
	assert_wf(world, NULL, "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	world_free(world);
}

static void
test_dismissed_copy_removed(void)
{
	in_namespace(dismissed_copy_removed);
}

/* Review of the fixes, item 6: the download's file a copy was made from is
 * that file only with its inode, its size and both its times as recorded when
 * it was copied (a file deleted and another of the same size made at its name
 * can have its inode number; not its times).  One whose times changed since is
 * not taken for it: the copy, which may be the only one, is kept. */
static void
dismissed_copy_times(void)
{
	g_autoptr(GError) error = NULL;
	World *world = world_new();
	g_autofree gchar *source = g_build_filename(world->disk_a, "complete", RELEASE, RELEASE ".mkv", NULL);
	g_autofree gchar *folder = g_build_filename(world->films, FOLDER, NULL);
	g_autofree gchar *temp = NULL;
	gint64 id;

	id = hearth_download(world, NULL);
	run_dies(world, id, "copied");
	temp = film_temp(world, id);
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	g_assert_true(axis_import_act(world->import, id, AXIS_IMPORT_ACT_DISMISS, -1, 0, "root", NULL, &error));
	g_assert_no_error(error);
	/* The download's file touched after the dismissal: the same inode and
	 * size, other times. */
	g_usleep(20 * 1000);
	g_assert_cmpint(g_utime(source, NULL), ==, 0);
	world_cleanup(world);
	g_assert_true(g_file_test(temp, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(count_temporaries(folder), ==, 1);
	assert_wf(world, "temp", "SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'", id);
	world_free(world);
}

static void
test_dismissed_copy_times(void)
{
	in_namespace(dismissed_copy_times);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/import/mounts/copy-usenet", test_copy_usenet);
	g_test_add_func("/import/mounts/copy-faults", test_copy_faults);
	g_test_add_func("/import/mounts/refusals", test_refusals);
	g_test_add_func("/import/mounts/move-across", test_move_across);
	g_test_add_func("/import/mounts/move-across-after-crash", test_move_across_after_crash);
	g_test_add_func("/import/mounts/move-across-source-vanishes", test_move_across_source_vanishes);
	g_test_add_func("/import/mounts/move-across-kept", test_move_across_kept);
	g_test_add_func("/import/mounts/move-across-unsynced", test_move_across_unsynced);
	g_test_add_func("/import/mounts/move-across-touched", test_move_across_touched);
	g_test_add_func("/import/mounts/move-across-changed-during-read-back", test_move_across_changed_during_read_back);
	g_test_add_func("/import/mounts/move-across-replaced-while-copied", test_move_across_replaced_while_copied);
	g_test_add_func("/import/mounts/copy-record-fails", test_copy_record_fails);
	g_test_add_func("/import/mounts/move-across-new-folder-unsynced", test_move_across_new_folder_unsynced);
	g_test_add_func("/import/mounts/copy-only-copy", test_copy_only_copy);
	g_test_add_func("/import/mounts/copy-crash-source-gone", test_copy_crash_source_gone);
	g_test_add_func("/import/mounts/copy-crash-unverified", test_copy_crash_unverified);
	g_test_add_func("/import/mounts/dismissed-copy-removed", test_dismissed_copy_removed);
	g_test_add_func("/import/mounts/dismissed-copy-times", test_dismissed_copy_times);
	return g_test_run();
}
