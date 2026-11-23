/*
 * test-web-probe.c - the probe module: a file's page, a root's probe page,
 * the administrators' actions, and the module's jobs on the real queue
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell, storage, library and probe modules on an ephemeral
 * loopback port, with sessions opened in an auth without a database, as in
 * test-web-library.c.  The media are made with ffmpeg's test sources
 * (tests/axis-test-media.h); without ffmpeg these tests skip.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-media.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "axis-test-case.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-library.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-storage.h"

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisStorage        *storage;
	AxisLibrary        *library;
	AxisProbe          *probe;
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
	AxisWebProbe       *probe_ui;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	AxisWebHub         *hub;
	SoupSession        *session;
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
	gchar              *dir;
	gchar              *disk;
	gchar              *films;
	gint64              volume;
	gint64              root;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)
#define SKIP_WITHOUT_MEDIA(fx) \
	do { \
		if ((fx)->db == NULL) \
			return; \
		if (!axis_test_media_available()) { \
			g_test_skip("ffmpeg and ffprobe are not installed"); \
			return; \
		} \
	} while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisLibraryTuning tuning;
	g_autoptr(AxisVolumeInspection) inspection = NULL;
	AxisVolumeAddRequest request;
	g_autofree gchar *readme = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_session_store_set_idle_seconds(30 * 24 * 3600);

	fx->dir = axis_test_storage_dir_new();
	fx->disk = g_build_filename(fx->dir, "disk", NULL);
	fx->films = g_build_filename(fx->disk, "Films", NULL);
	axis_test_storage_mkdir(fx->films);
	readme = g_build_filename(fx->disk, "readme.txt", NULL);
	axis_test_storage_write(readme, "media\n");

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	fx->hub = axis_web_hub_new(NULL);
	fx->storage = axis_storage_new(fx->pool, fx->settings);
	fx->library = axis_library_new(fx->pool, fx->storage);
	memset(&tuning, 0, sizeof(tuning));
	tuning.racy_window_ns = 0;
	tuning.guard_interval_ms = 1;
	axis_library_set_tuning(fx->library, &tuning);
	fx->probe = axis_probe_new(fx->pool, fx->storage, fx->settings);
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(),
		fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(),
		fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_library_get_module(),
		fx->library_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_probe_get_module(),
		fx->probe_ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->pool, &error));
	g_assert_true(axis_storage_load(fx->storage, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);

	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);

	/* A volume with a root. */
	g_assert_true(axis_storage_inspect_volume(fx->storage, fx->disk, &inspection, &error));
	memset(&request, 0, sizeof(request));
	request.path = inspection->path;
	request.name = "Disk";
	request.plan = inspection->plan;
	request.marker_uuid = inspection->marker_uuid;
	request.dev = inspection->dev;
	request.ino = inspection->ino;
	request.fsid = inspection->fsid;
	request.total_bytes = inspection->total_bytes;
	request.not_mount_point_ok = TRUE;
	g_assert_true(axis_storage_add_volume(fx->storage, &request, &fx->volume, &error));
	g_assert_true(axis_storage_add_root(fx->storage, fx->volume, "Films", AXIS_MEDIA_MOVIES, NULL,
		&fx->root, &error));
	g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	if (fx->db == NULL) {
		axis_test_storage_dir_free(fx->dir);
		g_free(fx->disk);
		g_free(fx->films);
		axis_test_log_reset();
		return;
	}

	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_probe_stop(fx->probe_ui);
	axis_web_library_stop(fx->library_ui);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_probe_free(fx->probe_ui);
	axis_web_library_free(fx->library_ui);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_probe_unref(fx->probe);
	axis_library_unref(fx->library);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->disk);
	g_free(fx->films);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply page = get_as(fx, cookie, "/storage");
	g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
	const gchar *extra[3];

	g_assert_nonnull(csrf);
	axis_test_reply_clear(&page);
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf;
	extra[2] = NULL;
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, extra, form);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain == NULL || strstr(plain, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
assert_query(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL",
			value != NULL ? value : "NULL");
}

static void
install(
	Fixture     *fx,
	const gchar *clip,
	const gchar *name
){
	g_autofree gchar *source = axis_test_media_path(clip);
	g_autofree gchar *path = g_build_filename(fx->films, name, NULL);
	g_autofree gchar *bytes = NULL;
	gsize length = 0;

	g_assert_true(g_file_get_contents(source, &bytes, &length, NULL));
	g_assert_true(g_file_set_contents(path, bytes, (gssize)length, NULL));
}

static void
scan_now(Fixture *fx)
{
	g_autoptr(AxisScanResult) result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_library_scan_root(fx->library, fx->root, NULL, &result, &error));
	g_assert_no_error(error);
	g_assert_cmpint(result->status, ==, AXIS_SCAN_COMPLETE);
}

static gint64
id_of(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *sql = g_strdup_printf("SELECT id FROM library_file WHERE rel_path = '%s'", name);
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	g_assert_nonnull(value);
	return g_ascii_strtoll(value, NULL, 10);
}

static void
probe_all(Fixture *fx)
{
	g_autofree gchar *ids = axis_test_db_query(fx->db,
		"SELECT coalesce(string_agg(id::text, ' ' ORDER BY id), '') FROM library_file"
		" WHERE kind IN ('video', 'audio')");
	g_auto(GStrv) list = g_strsplit(ids, " ", -1);
	guint i;

	for (i = 0; list[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;
		AxisProbeOutcome outcome;

		if (list[i][0] == '\0')
			continue;
		g_assert_true(axis_probe_file(fx->probe, g_ascii_strtoll(list[i], NULL, 10), NULL, &outcome, &error));
		g_assert_no_error(error);
	}
}

static gchar *
file_path(gint64 id)
{
	return g_strdup_printf("/library/file/%" G_GINT64_FORMAT, id);
}

/* ── The file page ──────────────────────────────────────────────── */

/* Task 3.1: container, streams table, HDR class, probe status. */
static void
test_file_page_of_a_video(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "multi.mkv", "Movie.mkv");
	scan_now(fx);
	probe_all(fx);
	path = file_path(id_of(fx, "Movie.mkv"));

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1><bdi>Movie.mkv</bdi></h1>");
	assert_contains(page.body, "Read at");
	assert_contains(page.body, "matroska,webm");
	assert_contains(page.body, "Format and length (container)");
	assert_contains(page.body, "Main video");
	assert_contains(page.body, "SDR");
	assert_contains(page.body, "Video, audio and subtitle tracks (streams)");
	/* Each kind of stream, with what matters of it. */
	assert_contains(page.body, "<th class=\"col-wide\" scope=\"col\">Codec</th>");
	axis_test_web_assert_tables(page.body);
	assert_contains(page.body, "h264 (High)");
	assert_contains(page.body, "160 x 120");
	assert_contains(page.body, "aac (LC)");
	assert_contains(page.body, "mono");
	assert_contains(page.body, "<bdi>eng</bdi>");
	assert_contains(page.body, "<bdi>fra</bdi>");
	assert_contains(page.body, "Lossy");
	assert_contains(page.body, "Subtitles");
	assert_contains(page.body, "subrip");
	assert_contains(page.body, "Text");
	/* Bitrates the muxer did not state are labelled as estimates. */
	assert_contains(page.body, "(estimated)");
	assert_contains(page.body, "Present");
	/* A viewer may look, not act; nothing inline. */
	assert_lacks(page.body, "Read this file again");
	assert_lacks(page.body, "Link to a movie or episode by hand");
	assert_lacks(page.body, "style=\"");
	assert_lacks(page.body, "<script>");
	assert_lacks(page.body, "onclick");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Read this file again");
	/* A video file of a movies folder: linked by hand (an administrator's). */
	assert_contains(page.body, "Link to a movie or episode by hand");
	assert_contains(page.body, "action=\"/library/file/");
	assert_contains(page.body, "csrf");
	axis_test_reply_clear(&page);
}

static void
test_file_page_of_a_music_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "tone1.flac", "01 One.flac");
	scan_now(fx);
	probe_all(fx);
	path = file_path(id_of(fx, "01 One.flac"));

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Lossless");
	assert_contains(page.body, "Tags and cover art");
	assert_contains(page.body, "<bdi>Track One</bdi>");
	assert_contains(page.body, "<bdi>The Band</bdi>");
	assert_contains(page.body, "<bdi>The Album</bdi>");
	assert_contains(page.body, "Present, 64 x 64");
	assert_lacks(page.body, "<h2>Main video</h2>");
	axis_test_reply_clear(&page);
}

static void
test_file_page_of_an_uncertain_hdr_file(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *good = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "pq-bare.mkv", "pq.mkv");
	install(fx, "hdr10.mkv", "hdr10.mkv");
	scan_now(fx);
	probe_all(fx);

	path = file_path(id_of(fx, "pq.mkv"));
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Uncertain");
	assert_contains(page.body, "Not classified");
	assert_contains(page.body, "PQ transfer function without mastering display metadata.");
	assert_contains(page.body, "must not convert");
	axis_test_reply_clear(&page);

	good = file_path(id_of(fx, "hdr10.mkv"));
	page = get_as(fx, fx->viewer, good);
	assert_contains(page.body, "HDR10");
	assert_lacks(page.body, "Uncertain");
	axis_test_reply_clear(&page);
}

/* Spec: Probe failure: the page shows the recorded error. */
static void
test_file_page_of_a_failed_probe(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_MEDIA(fx);
	axis_test_write_named(fx->films, "broken.mkv", "this is not a video\n");
	scan_now(fx);
	probe_all(fx);
	path = file_path(id_of(fx, "broken.mkv"));

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Could not be read");
	assert_contains(page.body, "ffprobe could not read the file");
	assert_contains(page.body, "Invalid data found when processing input");
	assert_contains(page.body, "does not read the file again until it changes");
	assert_lacks(page.body, "Video, audio and subtitle tracks (streams)");
	assert_lacks(page.body, "<h2>Format and length (container)</h2>");
	axis_test_reply_clear(&page);
}

static void
test_file_page_of_a_file_not_probed_yet(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "new.mkv");
	scan_now(fx);
	path = file_path(id_of(fx, "new.mkv"));

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Waiting");
	assert_lacks(page.body, "Read at");
	axis_test_reply_clear(&page);
}

/* Spec (design): everything from a file that reaches a page is escaped. */
static void
test_markup_from_a_file_is_escaped(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *source = axis_test_media_path("multi.mkv");
	g_autofree gchar *out = NULL;
	g_autofree gchar *command = NULL;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *standard_error = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply page;
	g_autofree gchar *id_text = NULL;
	gint status = 0;
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	out = g_build_filename(fx->films, "<img src=x onerror=alert(1)>.mkv", NULL);
	command = g_strdup_printf("ffmpeg -v error -y -i %s -c copy -metadata title='<script>alert(1)</script>'"
		" -metadata:s:a:0 title='<b>loud</b> \"quoted\" &amp;' -metadata:s:a:1 language='en<i>' '%s'",
		source, out);
	g_assert_true(g_shell_parse_argv(command, NULL, &argv, &error));
	g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
		NULL, &standard_error, &status, &error));
	g_assert_true(g_spawn_check_wait_status(status, NULL));
	scan_now(fx);
	probe_all(fx);
	id_text = axis_test_db_query(fx->db, "SELECT id FROM library_file");
	id = g_ascii_strtoll(id_text, NULL, 10);

	{
		g_autofree gchar *path = file_path(id);

		page = get_as(fx, fx->admin, path);
	}
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "&lt;img src=x onerror=alert(1)&gt;.mkv");
	assert_contains(page.body, "&lt;b&gt;loud&lt;/b&gt; &quot;quoted&quot; &amp;amp;");
	assert_contains(page.body, "&lt;script&gt;alert(1)&lt;/script&gt;");
	assert_lacks(page.body, "<img src=x");
	assert_lacks(page.body, "<b>loud");
	assert_lacks(page.body, "<script>");
	assert_lacks(page.body, "<i>");
	axis_test_reply_clear(&page);

	/* The root's page lists it, escaped too. */
	{
		g_autofree gchar *root = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", fx->root);

		page = get_as(fx, fx->viewer, root);
	}
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "&lt;img src=x onerror=alert(1)&gt;.mkv");
	assert_lacks(page.body, "<img src=x");
	axis_test_reply_clear(&page);
}

static void
test_missing_files(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->viewer, "/library/file/424242");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/library/file/abc");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/storage/roots/424242/probe");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	/* Not logged in: sent to the login page, nothing shown. */
	page = get_as(fx, NULL, "/library/file/1");
	g_assert_cmpuint(page.status, !=, 200);
	axis_test_reply_clear(&page);
}

/* ── A root's page ──────────────────────────────────────────────── */

static void
test_root_probe_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", fx->root);
	g_autofree gchar *link = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "good.mkv");
	axis_test_write_named(fx->films, "broken.mkv", "this is not a video\n");
	scan_now(fx);
	probe_all(fx);
	install(fx, "short.mp4", "later.mp4");
	scan_now(fx);
	link = g_strdup_printf("href=\"/library/file/%" G_GINT64_FORMAT "\"", id_of(fx, "broken.mkv"));

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Files read in Films");
	assert_contains(page.body, "Video and audio files");
	assert_contains(page.body, "Files Axis could not read");
	assert_contains(page.body, "Files read most recently");
	assert_contains(page.body, link);
	assert_contains(page.body, "Invalid data found");
	assert_contains(page.body, "Waiting to be read");
	assert_lacks(page.body, "Read the failed files again");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, path);
	assert_contains(page.body, "Read the failed files again");
	axis_test_reply_clear(&page);
}

/* ── Actions ────────────────────────────────────────────────────── */

/* Spec (browser): actions only create jobs; administrators only. */
static void
test_probe_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *placed = NULL;
	AxisTestReply reply;
	gint64 id;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	scan_now(fx);
	probe_all(fx);
	id = id_of(fx, "a.mkv");
	path = file_path(id);
	action = g_strdup_printf("%s/probe", path);
	location = g_strdup_printf("%s?done=probe-requested", path);
	placed = g_strconcat(location, "&at=file-probe#file-probe", NULL);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file'", "0");

	/* Not for a viewer. */
	reply = post_as(fx, fx->viewer, action, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file'", "0");

	/* Twice by an administrator: one job. */
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, placed);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file' AND priority = 10", "1");
	assert_query(fx, "SELECT changed::text FROM library_file", "true");

	reply = get_as(fx, fx->admin, location);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis will read this file again");
	assert_contains(reply.body, "Waiting");
	axis_test_reply_clear(&reply);

	/* Without the CSRF token, nothing. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", action, fx->admin, NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* No such file. */
	reply = post_as(fx, fx->admin, "/library/file/424242/probe", "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

static void
test_probe_failed_files_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", fx->root);
	AxisTestReply reply;

	SKIP_WITHOUT_MEDIA(fx);
	axis_test_write_named(fx->films, "broken1.mkv", "no\n");
	axis_test_write_named(fx->films, "broken2.mkv", "nope\n");
	install(fx, "sdr.mkv", "good.mkv");
	scan_now(fx);
	probe_all(fx);

	reply = post_as(fx, fx->viewer, path, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file'", "0");

	reply = post_as(fx, fx->admin, path, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file'", "2");
	assert_query(fx, "SELECT count(*) FROM library_file WHERE changed", "2");
}

/* A form's answer is a redirect to the element the person acted on, and the
 * page there draws its notice inside that element (the element comes before
 * the first notice in the page). */
static void
assert_back_to_place(
	Fixture       *fx,
	AxisTestReply *reply,
	const gchar   *place
){
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *path = NULL;
	const gchar *location = axis_test_reply_header(reply, "Location");
	const gchar *element;
	const gchar *notice;
	AxisTestReply page;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strlen(location) - strlen(tail));
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, id);
	notice = strstr(page.body, "notice-here");
	if (element == NULL || notice == NULL || element > notice)
		g_error("expected the notice inside %s in:\n%s", place, page.body);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(reply);
}

/* Spec: a form comes back to the card or row it was posted from */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *root = g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT "/probe", fx->root);
	g_autofree gchar *file = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_MEDIA(fx);
	axis_test_write_named(fx->films, "broken1.mkv", "no\n");
	install(fx, "sdr.mkv", "good.mkv");
	scan_now(fx);
	probe_all(fx);

	/* The failed files again: the card that lists them. */
	reply = post_as(fx, fx->admin, root, "");
	assert_back_to_place(fx, &reply, "probe-failed");

	/* One file again: its Probe card. */
	file = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/probe", id_of(fx, "good.mkv"));
	reply = post_as(fx, fx->admin, file, "");
	assert_back_to_place(fx, &reply, "file-probe");
}

/* ── The module on the real queue ───────────────────────────────── */

static gboolean
all_probed(gpointer data)
{
	Fixture *fx = data;
	g_autofree gchar *value = axis_test_db_query(fx->db,
		"SELECT count(*) FROM job WHERE kind = 'probe.file' AND state = 'succeeded'");

	return g_strcmp0(value, "3") == 0;
}

/* Tasks 2.5 and 2.6: new files are enqueued and probed by the queue the
 * daemon runs, with the module's own handlers, queue and schedules. */
static void
test_jobs_on_the_queue(Fixture *fx, gconstpointer data)
{
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	guint enqueued = 0;

	SKIP_WITHOUT_MEDIA(fx);
	install(fx, "sdr.mkv", "a.mkv");
	install(fx, "multi.mkv", "b.mkv");
	install(fx, "tone1.flac", "c.flac");
	axis_test_write_named(fx->films, "notes.txt", "not media\n");
	scan_now(fx);

	g_assert_true(axis_job_registry_add_modules(registry, fx->registry, &error));
	g_assert_no_error(error);
	queue = axis_job_queue_new(fx->pool, registry);
	axis_job_queue_set_concurrency(queue, AXIS_PROBE_QUEUE, 2);
	axis_job_queue_set_poll_interval(queue, 50);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);

	/* Its schedules exist; its jobs run on the "probe" queue. */
	assert_query(fx, "SELECT count(*) FROM job_schedule WHERE name IN ('probe.due', 'probe.stale')", "2");
	g_assert_true(axis_probe_enqueue_due(fx->probe, FALSE, &enqueued, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(enqueued, ==, 3);
	assert_query(fx, "SELECT count(*) FROM job WHERE kind = 'probe.file' AND queue = 'probe' AND max_attempts = 8",
	             "3");

	g_assert_true(axis_test_pump_until(all_probed, fx, AXIS_TEST_WAIT_MS * 3));
	assert_query(fx, "SELECT count(*) FROM media_info WHERE status = 'ok'", "3");
	assert_query(fx, "SELECT count(*) FROM library_file WHERE changed AND kind IN ('video', 'audio')", "0");

	/* Nothing more to do: the schedule's job enqueues nothing. */
	g_assert_true(axis_probe_enqueue_due(fx->probe, FALSE, &enqueued, &error));
	g_assert_cmpuint(enqueued, ==, 0);
	g_assert_true(axis_job_queue_stop(queue, 5000));
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/web/probe/file-page-video", test_file_page_of_a_video);
	ADD("/web/probe/file-page-music", test_file_page_of_a_music_file);
	ADD("/web/probe/file-page-uncertain-hdr", test_file_page_of_an_uncertain_hdr_file);
	ADD("/web/probe/file-page-failed", test_file_page_of_a_failed_probe);
	ADD("/web/probe/file-page-not-probed", test_file_page_of_a_file_not_probed_yet);
	ADD("/web/probe/markup-is-escaped", test_markup_from_a_file_is_escaped);
	ADD("/web/probe/missing-files", test_missing_files);
	ADD("/web/probe/root-page", test_root_probe_page);
	ADD("/web/probe/probe-again", test_probe_again);
	ADD("/web/probe/probe-failed-files-again", test_probe_failed_files_again);
	ADD("/web/probe/back-to-place", test_back_to_place);
	ADD("/web/probe/jobs-on-the-queue", test_jobs_on_the_queue);
#undef ADD

	return g_test_run();
}
