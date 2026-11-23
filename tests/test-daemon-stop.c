/*
 * test-daemon-stop.c - a real axisd stopped (SIGTERM) while it copies a
 * download into the library, then started again on the same database and
 * disks
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Task 5.3 of refine-container-deployment.  The download is on one
 * filesystem and the movies' root on another, so the import copies the
 * film through the volume write layer (axis_volume_write_copy_temp(), the
 * write layer's one copy: a conversion's placement only renames and links,
 * and is stopped at every step by tests/test-conversion.c).  Two ways the
 * stop meets the copy:
 *
 *  - the copy is half done: it notices the stop at its next progress step,
 *    removes its temporary file and gives the job back; the daemon started
 *    again copies the film once more and imports it;
 *  - the copy has just begun (it waits for its first progress record): it
 *    finishes within the grace period, the film gets its name, and the run
 *    stops before the film is linked; the daemon started again links it.
 *
 * Every test runs in a child process in its own user and mount namespace,
 * with two small tmpfs filesystems as the disks (as test-import-mounts.c
 * does), and axisd runs in it too; nothing is mounted on the host.  The
 * film is a small file whose first line tells the fake ffprobe
 * (tests/fixtures/import/fake-ffprobe.sh) what it is.  A copy of a few
 * megabytes is over in milliseconds, so for the first case axisd runs under
 * strace, which delays every read of the download's file (pread64 on that
 * path only) by READ_DELAY: the copy then takes seconds.  For the second, a
 * trigger in the test's own database holds the copy's first progress record
 * until the test lets it go.  Where unprivileged namespaces or strace are
 * not available, the tests skip.
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
#include <signal.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-storage.h"
#include "axis-test-worker.h"
#include "core/axis-settings.h"
#include "import/axis-import.h"
#include "import/axis-import-private.h"
#include "probe/axis-probe.h"
#include "storage/axis-storage.h"

#ifndef AXISD_PATH
#error "AXISD_PATH must be defined by the build"
#endif

#define TOOL       "tests/fixtures/import/fake-ffprobe.sh"
#define RELEASE    "Hearth.1995.1080p.BluRay.x264-GRP"
#define FILM_SIZE  (12 * 1024 * 1024 + 4321)
/* Each 1 MiB read of the copy waits this long under strace: the copy of
 * the film takes about five seconds, and checks for a stop every second. */
#define READ_DELAY "400ms"
#define GRACE_S    15
#define LONG_MS    180000
#define HOLD_KEY   "770055"

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

static const gchar *const strace_options[] = {
	"strace", "-f", "--seccomp-bpf", "--kill-on-exit", "-qq", "-o", "/dev/null", "-e", "trace=pread64", NULL
};

/* Whether strace, as this test runs it, works in a namespace here. */
static gboolean
strace_usable(void)
{
	g_autofree gchar *strace = g_find_program_in_path("strace");
	pid_t pid;
	gint status = 0;

	if (strace == NULL)
		return FALSE;
	pid = fork();
	if (pid == 0) {
		g_autoptr(GPtrArray) argv = g_ptr_array_new();
		gint child = 1;
		guint i;

		if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
			_exit(1);
		for (i = 0; strace_options[i] != NULL; i++)
			g_ptr_array_add(argv, (gpointer)strace_options[i]);
		g_ptr_array_add(argv, (gpointer)"-e");
		g_ptr_array_add(argv, (gpointer)"inject=pread64:delay_exit=1ms");
		g_ptr_array_add(argv, (gpointer)"--");
		g_ptr_array_add(argv, (gpointer)"/bin/true");
		g_ptr_array_add(argv, NULL);
		if (!g_spawn_sync(NULL, (gchar **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
		                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &child, NULL))
			_exit(1);
		_exit(g_spawn_check_wait_status(child, NULL) ? 0 : 1);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid)
		return FALSE;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void
in_namespace(
	void   (*body)(void),
	gboolean traced
){
	if (g_test_subprocess()) {
		enter_namespace();
		body();
		return;
	}
	if (!namespaces_available()) {
		g_test_skip("unprivileged user and mount namespaces are not available here");
		return;
	}
	if (traced && !strace_usable()) {
		g_test_skip("strace (with --seccomp-bpf and --kill-on-exit) cannot trace a process here");
		return;
	}
	if (g_getenv("AXIS_TEST_DATABASE_URL") == NULL && g_strcmp0(g_getenv("AXIS_SKIP_DB_TESTS"), "1") == 0) {
		g_test_skip("AXIS_SKIP_DB_TESTS=1");
		return;
	}
	g_test_trap_subprocess(NULL, 300 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_INHERIT_STDERR);
	g_test_trap_assert_passed();
}

/* ── The world inside the child ─────────────────────────────────── */

typedef struct
{
	AxisTestDb *db;
	AxisDbPool *pool;
	gchar      *dir;
	gchar      *disk_a;	/* the downloads' volume */
	gchar      *disk_b;	/* the library's volume */
	gchar      *films;	/* the movies' root, on disk B */
	gchar      *source_dir;
	gchar      *source;
	gchar      *source_sha;
	gchar      *source_tree;
	gchar      *data_dir;
	gchar      *ffprobe;
	gchar      *port;
	gint64      import_id;
	gint64      movie_id;
} World;

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

static gint64
q_int(
	World       *world,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(world->db, sql);

	if (value == NULL)
		g_error("%s: no value", sql);
	return g_ascii_strtoll(value, NULL, 10);
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

static gint64
add_volume(
	AxisStorage *storage,
	const gchar *path,
	const gchar *name
){
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	g_autoptr(GError) error = NULL;
	AxisVolumeAddRequest request;
	gint64 id = 0;

	g_assert_true(axis_storage_inspect_volume(storage, path, &inspection, &error));
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
	g_assert_true(axis_storage_add_volume(storage, &request, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gchar *
sha_of(const gchar *path)
{
	g_autofree gchar *contents = NULL;
	gsize length = 0;

	g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
	return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
}

static gint64
size_of(const gchar *path)
{
	GStatBuf st;

	return g_lstat(path, &st) == 0 ? (gint64)st.st_size : -1;
}

static gchar *
free_port(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
		&error);
	g_autoptr(GSocketAddress) address = g_inet_socket_address_new_from_string("127.0.0.1", 0);
	g_autoptr(GSocketAddress) bound = NULL;
	guint16 port;

	g_assert_no_error(error);
	g_assert_true(g_socket_bind(socket, address, FALSE, &error));
	bound = g_socket_get_local_address(socket, &error);
	g_assert_no_error(error);
	port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
	g_socket_close(socket, NULL);
	return g_strdup_printf("%u", (guint)port);
}

/* The download of Hearth on disk A, the film on the movies' root of disk B,
 * and a manual import of the one into the other, queued. */
static World *
world_new(void)
{
	World *world = g_new0(World, 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisSettings) settings = NULL;
	g_autofree gchar *tool = g_canonicalize_filename(TOOL, NULL);
	g_autofree gchar *wrapper = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *movie = NULL;
	g_autofree gchar *import = NULL;
	AxisStorage *storage;
	AxisDbConn *conn;
	gint64 volume_a;
	gint64 volume_b;
	gint64 root = 0;

	axis_test_log_init();
	axis_test_log_reset();
	world->dir = axis_test_storage_dir_new();
	world->disk_a = g_build_filename(world->dir, "disk-a", NULL);
	world->disk_b = g_build_filename(world->dir, "disk-b", NULL);
	world->data_dir = g_build_filename(world->dir, "data", NULL);
	axis_test_storage_mkdir(world->disk_a);
	axis_test_storage_mkdir(world->disk_b);
	if (mount("axis-test", world->disk_a, "tmpfs", 0, "size=48m,mode=0755") != 0 ||
	    mount("axis-test", world->disk_b, "tmpfs", 0, "size=48m,mode=0755") != 0)
		g_error("mount tmpfs: %s", g_strerror(errno));
	world->port = free_port();

	/* The fake ffprobe, with a version for the probe's check. */
	world->ffprobe = g_build_filename(world->dir, "ffprobe", NULL);
	wrapper = g_strdup_printf("#!/bin/sh\nif [ \"$1\" = -version ]; then echo 'ffprobe version 8.1.3 (a test)'; "
		"exit 0; fi\nexec /bin/sh '%s' \"$@\"\n", tool);
	g_assert_true(g_file_set_contents(world->ffprobe, wrapper, -1, NULL));
	g_assert_cmpint(g_chmod(world->ffprobe, 0755), ==, 0);

	world->db = axis_test_db_new();
	g_assert_nonnull(world->db);
	world->pool = axis_test_jobs_pool(world->db, 4);
	settings = axis_settings_new();
	g_assert_true(axis_settings_add_table(settings, AXIS_STORAGE_MODULE_ID, axis_storage_get_setting_table(), &error));
	g_assert_true(axis_settings_add_table(settings, AXIS_IMPORT_MODULE_ID, axis_import_get_setting_table(), &error));
	g_assert_true(axis_settings_add_table(settings, AXIS_PROBE_MODULE_ID, axis_probe_get_setting_table(), &error));
	g_assert_true(axis_settings_load(settings, world->pool, &error));
	g_assert_no_error(error);
	/* Stored for the daemon: writing allowed, a floor the small disks
	 * have, the fake ffprobe. */
	g_assert_true(axis_settings_set(settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_true(axis_settings_set(settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "1MiB", &error));
	g_assert_true(axis_settings_set(settings, AXIS_PROBE_SETTING_PATH, world->ffprobe, &error));
	g_assert_no_error(error);

	storage = axis_storage_new(world->pool, settings);
	g_assert_true(axis_storage_load(storage, &error));
	g_assert_no_error(error);
	axis_storage_set_timeouts(storage, 30000, 30000);
	while (g_main_context_iteration(NULL, FALSE))
		;
	volume_a = add_volume(storage, world->disk_a, "Downloads disk");
	volume_b = add_volume(storage, world->disk_b, "Films disk");
	world->films = g_build_filename(world->disk_b, "films", NULL);
	axis_test_storage_mkdir(world->films);
	g_assert_true(axis_storage_add_root(storage, volume_b, "films", AXIS_MEDIA_MOVIES, NULL, &root, &error));
	g_assert_no_error(error);
	axis_storage_stop(storage);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_storage_unref(storage);

	/* The download: one film, its first line for the fake ffprobe. */
	world->source_dir = g_build_filename(world->disk_a, "complete", RELEASE, NULL);
	world->source = g_build_filename(world->source_dir, RELEASE ".mkv", NULL);
	{
		static const gchar line[] = "AXISFAKE duration=10200 width=1920 height=800 vcodec=h264\n";
		g_autofree guchar *bytes = g_malloc(FILM_SIZE);
		gsize i;

		axis_test_storage_mkdir(world->source_dir);
		memcpy(bytes, line, strlen(line));
		for (i = strlen(line); i < FILM_SIZE; i++)
			bytes[i] = (guchar)((i * 131 + 17) & 0xff);
		g_assert_true(g_file_set_contents(world->source, (const gchar *)bytes, FILM_SIZE, NULL));
	}
	world->source_sha = sha_of(world->source);
	world->source_tree = axis_test_storage_digest(world->source_dir);

	/* The film, and its import by hand from the download's folder. */
	title = q(world, "INSERT INTO meta_title (kind, title, year, provider, provider_id, status) VALUES ('film', 'Hearth',"
		" 1995, 'tmdb', '6001', 'released') RETURNING id");
	movie = q(world, "INSERT INTO movie (title_id, quality_profile_id, root_id) SELECT %s, id, %" G_GINT64_FORMAT
		" FROM quality_profile WHERE slug = 'balanced' RETURNING id", title, root);
	g_assert_nonnull(movie);
	world->movie_id = g_ascii_strtoll(movie, NULL, 10);
	import = q(world, "INSERT INTO import (module, owner_id, title_id, title_label, source_volume_id, source_path,"
		" release_name, kind, state, requested_by) VALUES ('movies', %s, %s, 'Hearth (1995)', %" G_GINT64_FORMAT ","
		" 'complete/" RELEASE "', '" RELEASE "', 'manual', 'waiting', 'root') RETURNING id", movie, title, volume_a);
	world->import_id = g_ascii_strtoll(import, NULL, 10);
	conn = axis_db_pool_acquire(world->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_import_enqueue(conn, world->import_id, 10, &error));
	g_assert_no_error(error);
	axis_db_pool_release(world->pool, conn);
	return world;
}

static void
world_free(World *world)
{
	axis_db_pool_free(world->pool);
	axis_test_db_free(world->db);
	while (g_main_context_iteration(NULL, FALSE))
		;
	umount2(world->disk_a, MNT_DETACH);
	umount2(world->disk_b, MNT_DETACH);
	axis_test_storage_dir_free(world->dir);
	g_free(world->disk_a);
	g_free(world->disk_b);
	g_free(world->films);
	g_free(world->source_dir);
	g_free(world->source);
	g_free(world->source_sha);
	g_free(world->source_tree);
	g_free(world->data_dir);
	g_free(world->ffprobe);
	g_free(world->port);
	g_free(world);
	axis_test_log_reset();
}

/* ── The daemon ─────────────────────────────────────────────────── */

/* axisd on the test's database and disks, under strace when @traced (each
 * read of the download's file delayed); waits until it serves. */
static AxisTestWorker *
start_daemon(
	World    *world,
	gboolean  traced
){
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *asan = NULL;
	const gchar *env[16];
	AxisTestWorker *daemon;
	guint n = 0;
	guint i;

	if (traced) {
		for (i = 0; strace_options[i] != NULL; i++)
			g_ptr_array_add(argv, g_strdup(strace_options[i]));
		g_ptr_array_add(argv, g_strdup("-P"));
		g_ptr_array_add(argv, g_strdup(world->source));
		g_ptr_array_add(argv, g_strdup("-e"));
		g_ptr_array_add(argv, g_strdup("inject=pread64:delay_exit=" READ_DELAY));
		g_ptr_array_add(argv, g_strdup("--"));
		/* LeakSanitizer cannot look at a process that is traced. */
		asan = g_strconcat(g_getenv("ASAN_OPTIONS") != NULL ? g_getenv("ASAN_OPTIONS") : "",
			g_getenv("ASAN_OPTIONS") != NULL ? ":" : "", "detect_leaks=0", NULL);
	}
	g_ptr_array_add(argv, g_strdup(AXISD_PATH));
	g_ptr_array_add(argv, NULL);
	env[n++] = "AXIS_DATABASE_URL";
	env[n++] = axis_test_db_get_url(world->db);
	env[n++] = "AXIS_DATA_DIR";
	env[n++] = world->data_dir;
	env[n++] = "AXIS_LISTEN_HOST";
	env[n++] = "127.0.0.1";
	env[n++] = "AXIS_LISTEN_PORT";
	env[n++] = world->port;
	env[n++] = "AXIS_LOG_LEVEL";
	env[n++] = "info";
	env[n++] = "AXIS_JOBS_GRACE_PERIOD";
	env[n++] = G_STRINGIFY(GRACE_S);
	if (asan != NULL) {
		env[n++] = "ASAN_OPTIONS";
		env[n++] = asan;
	}
	env[n] = NULL;
	daemon = axis_test_process_spawn((const gchar *const *)argv->pdata, env);
	if (!axis_test_worker_wait_log(daemon, "listening on", LONG_MS))
		g_error("axisd did not start; its log:\n%s", axis_test_worker_get_log(daemon));
	return daemon;
}

typedef struct
{
	GPid parent;
	GPid found;
} ChildWait;

static gboolean
child_found(gpointer data)
{
	ChildWait *wait = data;

	wait->found = axis_test_find_child(wait->parent, "axisd");
	return wait->found != 0;
}

/* SIGTERM to axisd itself (under strace, strace's child); waits for it to
 * end: within the grace period, with status 0. */
static void
stop_daemon(
	AxisTestWorker *daemon,
	gboolean        traced
){
	GPid pid = axis_test_worker_get_pid(daemon);
	gint64 sent;
	gint64 took_ms;

	if (traced) {
		ChildWait wait;

		wait.parent = pid;
		wait.found = 0;
		g_assert_true(axis_test_pump_until(child_found, &wait, AXIS_TEST_WAIT_MS));
		pid = wait.found;
	}
	sent = g_get_monotonic_time();
	g_assert_cmpint(kill(pid, SIGTERM), ==, 0);
	if (!axis_test_worker_wait_exit(daemon, (GRACE_S + 30) * 1000))
		g_error("axisd did not stop; its log:\n%s", axis_test_worker_get_log(daemon));
	took_ms = (g_get_monotonic_time() - sent) / 1000;
	/* (The test runs in a child whose standard output is not shown.) */
	g_printerr("# axisd stopped %" G_GINT64_FORMAT " ms after SIGTERM\n", took_ms);
	if (axis_test_worker_get_status(daemon) != 0)
		g_error("axisd ended with %d; its log:\n%s", axis_test_worker_get_status(daemon),
			axis_test_worker_get_log(daemon));
	if (took_ms >= GRACE_S * 1000)
		g_error("axisd took %" G_GINT64_FORMAT " ms to stop, more than its grace period; its log:\n%s", took_ms,
			axis_test_worker_get_log(daemon));
	g_assert_null(strstr(axis_test_worker_get_log(daemon), "did not finish within the shutdown grace period"));
}

/* ── What is on the disks ───────────────────────────────────────── */

typedef struct
{
	GPtrArray *files;	/* every regular file under the root */
	GPtrArray *temporaries;	/* the ones with a temporary name */
} Tree;

static void
walk(
	const gchar *path,
	Tree        *tree
){
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *name;

	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *child = g_build_filename(path, name, NULL);
		GStatBuf st;

		if (g_lstat(child, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			walk(child, tree);
			continue;
		}
		g_ptr_array_add(tree->files, g_strdup(child));
		if (g_str_has_prefix(name, ".axis-partial-"))
			g_ptr_array_add(tree->temporaries, g_strdup(child));
	}
}

static void
tree_clear(Tree *tree)
{
	g_ptr_array_unref(tree->files);
	g_ptr_array_unref(tree->temporaries);
}

static void
tree_read(
	World *world,
	Tree  *tree
){
	tree->files = g_ptr_array_new_with_free_func(g_free);
	tree->temporaries = g_ptr_array_new_with_free_func(g_free);
	walk(world->films, tree);
}

/* Nothing partial is in the library: no temporary name is left, and every
 * file is a complete copy of the download's film. */
static void
assert_nothing_partial(World *world)
{
	Tree tree;
	guint i;

	tree_read(world, &tree);
	if (tree.temporaries->len > 0)
		g_error("a temporary file is left in the library: %s", (const gchar *)g_ptr_array_index(tree.temporaries, 0));
	for (i = 0; i < tree.files->len; i++) {
		const gchar *path = g_ptr_array_index(tree.files, i);
		g_autofree gchar *sha = sha_of(path);
		GStatBuf st;

		g_assert_cmpint(g_lstat(path, &st), ==, 0);
		if ((gint64)st.st_size != FILM_SIZE || strcmp(sha, world->source_sha) != 0)
			g_error("a partial file is under a library name: %s (%" G_GINT64_FORMAT " bytes)", path,
				(gint64)st.st_size);
	}
	tree_clear(&tree);
}

/* The download is as it was. */
static void
assert_source_intact(World *world)
{
	g_autofree gchar *tree = axis_test_storage_digest(world->source_dir);

	g_assert_cmpstr(tree, ==, world->source_tree);
}

typedef struct
{
	World   *world;
	gint64   at_least;
	gchar   *path;
} CopyWait;

/* The import's temporary file is there, with at least @at_least bytes. */
static gboolean
copying(gpointer data)
{
	CopyWait *wait = data;
	Tree tree;
	gboolean found = FALSE;
	guint i;

	tree_read(wait->world, &tree);
	for (i = 0; i < tree.temporaries->len && !found; i++) {
		const gchar *path = g_ptr_array_index(tree.temporaries, i);
		GStatBuf st;

		if (g_lstat(path, &st) == 0 && (gint64)st.st_size >= wait->at_least) {
			g_free(wait->path);
			wait->path = g_strdup(path);
			found = TRUE;
		}
	}
	tree_clear(&tree);
	return found;
}

typedef struct
{
	World       *world;
	const gchar *sql;
	const gchar *expected;
} QueryWait;

static gboolean
query_is(gpointer data)
{
	QueryWait *wait = data;
	g_autofree gchar *value = axis_test_db_query(wait->world->db, wait->sql);

	return g_strcmp0(value, wait->expected) == 0;
}

/* Waits (serving the daemons' logs) until @sql gives @expected. */
static void
wait_query(
	World          *world,
	AxisTestWorker *daemon,
	const gchar    *sql,
	const gchar    *expected
){
	QueryWait wait;

	wait.world = world;
	wait.sql = sql;
	wait.expected = expected;
	if (!axis_test_pump_until(query_is, &wait, LONG_MS)) {
		g_autofree gchar *value = axis_test_db_query(world->db, sql);

		g_error("%s is %s, not %s; axisd's log:\n%s", sql, value != NULL ? value : "NULL", expected,
			daemon != NULL ? axis_test_worker_get_log(daemon) : "");
	}
}

/* After the daemon started again: the film imported once, its records and
 * the disk agreeing. */
static void
assert_imported_once(World *world)
{
	g_autofree gchar *sql = NULL;
	g_autofree gchar *placed = NULL;
	g_autofree gchar *sha = NULL;
	g_autofree gchar *recorded = NULL;
	GStatBuf st;
	Tree tree;

	assert_nothing_partial(world);
	tree_read(world, &tree);
	g_assert_cmpuint(tree.files->len, ==, 1);
	placed = g_strdup(g_ptr_array_index(tree.files, 0));
	tree_clear(&tree);
	g_assert_true(g_str_has_suffix(placed, ".mkv"));
	g_assert_cmpint(g_lstat(placed, &st), ==, 0);
	g_assert_cmpint((gint64)st.st_size, ==, FILM_SIZE);
	g_assert_cmpuint((guint)st.st_nlink, ==, 1);
	sha = sha_of(placed);
	g_assert_cmpstr(sha, ==, world->source_sha);
	assert_source_intact(world);

	/* The import: imported, a verified copy, no failure counted. */
	sql = g_strdup_printf("SELECT state || ' ' || mode || ' ' || attempts || ' ' || coalesce(reason, '-') FROM import"
		" WHERE id = %" G_GINT64_FORMAT, world->import_id);
	assert_q(world, "imported copy 0 -", sql);
	g_free(sql);
	sql = g_strdup_printf("SELECT checksum FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'",
		world->import_id);
	recorded = axis_test_db_query(world->db, sql);
	g_assert_cmpstr(recorded, ==, world->source_sha);
	/* One file row, the film's, of that inode; one link to the film. */
	g_free(sql);
	sql = g_strdup_printf("SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND step = 'published'"
		" AND dest_inode = %" G_GUINT64_FORMAT, world->import_id, (guint64)st.st_ino);
	assert_q(world, "1", sql);
	g_free(sql);
	sql = g_strdup_printf("SELECT count(*) || ' ' || min(lf.inode) FROM movie_file mf JOIN library_file lf ON"
		" lf.id = mf.file_id WHERE mf.movie_id = %" G_GINT64_FORMAT, world->movie_id);
	{
		g_autofree gchar *expected = g_strdup_printf("1 %" G_GUINT64_FORMAT, (guint64)st.st_ino);

		assert_q(world, expected, sql);
	}
	g_assert_cmpint(q_int(world, "SELECT count(*) FROM library_file WHERE state = 'present' AND kind = 'video'"), ==, 1);
	/* Its history says it once, and nothing about waiting or failing. */
	g_free(sql);
	sql = g_strdup_printf("SELECT string_agg(kind, ' ' ORDER BY id) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT
		" AND kind LIKE 'import%%'", world->movie_id);
	assert_q(world, "imported", sql);
	assert_q(world, "0", "SELECT count(*) FROM job WHERE kind = 'import.run' AND state <> 'succeeded'");
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Spec: SIGTERM while the copy is half done.  The daemon stops within its
 * grace period, with status 0; the copy's temporary file is gone, the
 * download is as it was, and the stop is not counted against the import (a
 * stop is not a failure); started again, the daemon imports the film once. */
static void
stop_while_copying(void)
{
	World *world = world_new();
	g_autofree gchar *sql = NULL;
	AxisTestWorker *daemon;
	CopyWait wait;

	daemon = start_daemon(world, TRUE);
	/* A megabyte of the copy written: it is under way, and takes seconds
	 * more. */
	wait.world = world;
	wait.at_least = 1024 * 1024;
	wait.path = NULL;
	if (!axis_test_pump_until(copying, &wait, LONG_MS))
		g_error("the import did not start copying; axisd's log:\n%s", axis_test_worker_get_log(daemon));
	g_printerr("# copying, %" G_GINT64_FORMAT " of %d bytes written, when SIGTERM is sent\n", size_of(wait.path),
		FILM_SIZE);
	stop_daemon(daemon, TRUE);
	g_assert_nonnull(strstr(axis_test_worker_get_log(daemon), "stopping the job queue: "));
	axis_test_worker_free(daemon);
	g_free(wait.path);

	assert_nothing_partial(world);
	{
		Tree tree;

		/* The copy was stopped half way: nothing has a library name yet. */
		tree_read(world, &tree);
		g_assert_cmpuint(tree.files->len, ==, 0);
		tree_clear(&tree);
	}
	assert_source_intact(world);
	/* Nothing counted against it: no failure, no wait, the job given back. */
	sql = g_strdup_printf("SELECT state || ' ' || attempts || ' ' || coalesce(reason, '-') FROM import WHERE id = %"
		G_GINT64_FORMAT, world->import_id);
	assert_q(world, "running 0 -", sql);
	assert_q(world, "queued", "SELECT string_agg(state::text, ' ') FROM job WHERE kind = 'import.run'");
	g_free(sql);
	sql = g_strdup_printf("SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT, world->movie_id);
	assert_q(world, "0", sql);

	/* Started again: the film is copied once more, and imported. */
	daemon = start_daemon(world, FALSE);
	g_free(sql);
	sql = g_strdup_printf("SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, world->import_id);
	wait_query(world, daemon, sql, "imported");
	stop_daemon(daemon, FALSE);
	axis_test_worker_free(daemon);
	assert_imported_once(world);
	world_free(world);
}

static void
test_stop_while_copying(void)
{
	in_namespace(stop_while_copying, TRUE);
}

/* Spec: SIGTERM just after the copy began (its first progress record is
 * held until the daemon is stopping).  The copy finishes within the grace
 * period and the film gets its name, complete; the run stops there, before
 * the film is linked; started again, the daemon links it, once. */
static void
stop_as_copy_begins(void)
{
	World *world = world_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	AxisTestWorker *daemon;
	AxisDbConn *hold;
	CopyWait wait;
	gint64 sent;
	gint64 took_ms;

	/* The test's database holds the copy's first progress record (the
	 * temporary file's inode) while the test holds an advisory lock. */
	axis_test_db_exec(world->db, "CREATE FUNCTION test_hold_copy() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN"
		" IF NEW.mode = 'copy' AND NEW.temp_inode IS NOT NULL AND OLD.temp_inode IS NULL THEN"
		" PERFORM pg_advisory_xact_lock_shared(" HOLD_KEY "); END IF; RETURN NEW; END $$");
	axis_test_db_exec(world->db, "CREATE TRIGGER test_hold_copy BEFORE UPDATE ON import_file FOR EACH ROW"
		" EXECUTE FUNCTION test_hold_copy()");
	hold = axis_db_pool_acquire(world->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_db_conn_exec(hold, "SELECT pg_advisory_lock(" HOLD_KEY ")", 0, NULL, NULL, &error));
	g_assert_no_error(error);

	daemon = start_daemon(world, FALSE);
	/* The temporary file made, the copy waiting for its record. */
	wait.world = world;
	wait.at_least = 0;
	wait.path = NULL;
	if (!axis_test_pump_until(copying, &wait, LONG_MS))
		g_error("the import did not start copying; axisd's log:\n%s", axis_test_worker_get_log(daemon));
	wait_query(world, daemon, "SELECT (count(*) > 0)::text FROM pg_stat_activity WHERE datname = current_database()"
		" AND wait_event_type = 'Lock' AND wait_event = 'advisory'", "true");
	sent = g_get_monotonic_time();
	g_assert_cmpint(kill(axis_test_worker_get_pid(daemon), SIGTERM), ==, 0);
	/* The daemon stopping, its job told to stop: the copy goes on. */
	if (!axis_test_worker_wait_log(daemon, "stopping the job queue: ", LONG_MS))
		g_error("axisd did not stop its job queue; its log:\n%s", axis_test_worker_get_log(daemon));
	g_assert_true(axis_db_conn_exec(hold, "SELECT pg_advisory_unlock(" HOLD_KEY ")", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(world->pool, hold);
	if (!axis_test_worker_wait_exit(daemon, (GRACE_S + 30) * 1000))
		g_error("axisd did not stop; its log:\n%s", axis_test_worker_get_log(daemon));
	took_ms = (g_get_monotonic_time() - sent) / 1000;
	g_printerr("# axisd stopped %" G_GINT64_FORMAT " ms after SIGTERM (the copy held until it was stopping)\n",
		took_ms);
	if (axis_test_worker_get_status(daemon) != 0)
		g_error("axisd ended with %d; its log:\n%s", axis_test_worker_get_status(daemon),
			axis_test_worker_get_log(daemon));
	g_assert_cmpint(took_ms, <, GRACE_S * 1000);
	g_assert_null(strstr(axis_test_worker_get_log(daemon), "did not finish within the shutdown grace period"));
	axis_test_worker_free(daemon);
	g_free(wait.path);
	axis_test_db_exec(world->db, "DROP TRIGGER test_hold_copy ON import_file");

	/* The film complete under its name, not linked yet; nothing counted
	 * against the import. */
	assert_nothing_partial(world);
	{
		Tree tree;

		tree_read(world, &tree);
		g_assert_cmpuint(tree.files->len, ==, 1);
		tree_clear(&tree);
	}
	assert_source_intact(world);
	sql = g_strdup_printf("SELECT state || ' ' || attempts || ' ' || coalesce(reason, '-') FROM import WHERE id = %"
		G_GINT64_FORMAT, world->import_id);
	assert_q(world, "running 0 -", sql);
	g_free(sql);
	sql = g_strdup_printf("SELECT step FROM import_file WHERE import_id = %" G_GINT64_FORMAT " AND role = 'film'",
		world->import_id);
	assert_q(world, "published", sql);
	g_free(sql);
	sql = g_strdup_printf("SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, world->movie_id);
	assert_q(world, "0", sql);
	assert_q(world, "queued", "SELECT string_agg(state::text, ' ') FROM job WHERE kind = 'import.run'");

	/* Started again: linked, once. */
	daemon = start_daemon(world, FALSE);
	g_free(sql);
	sql = g_strdup_printf("SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, world->import_id);
	wait_query(world, daemon, sql, "imported");
	stop_daemon(daemon, FALSE);
	axis_test_worker_free(daemon);
	assert_imported_once(world);
	world_free(world);
}

static void
test_stop_as_copy_begins(void)
{
	in_namespace(stop_as_copy_begins, FALSE);
}

int
main(int argc, char **argv)
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/daemon/stop/while-copying", test_stop_while_copying);
	g_test_add_func("/daemon/stop/as-copy-begins", test_stop_as_copy_begins);
	status = g_test_run();
	axis_test_worker_cleanup();
	return status;
}
