/*
 * test-web-youtube.c - the YouTube downloader end to end (add-youtube-downloader,
 * slice Y1): the pages on the real shell, the built-in download client in
 * the real downloads layer, the fetch job on the real job queue, a real
 * database and volume, and a stand-in program in yt-dlp's place
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing here reaches the network or runs a real yt-dlp.  The stand-in is a
 * shell script this test writes: it answers `--version`, prints a video's
 * record for the look, and for a fetch prints progress lines and writes
 * "video.mkv" into its working directory.  The video id in the address it is
 * given chooses how it behaves (it fails, hangs, floods, is refused by
 * YouTube, is a live stream).  It records every argument it was given, and
 * its process id.  Every title, channel and video id is made up.
 *
 * Spec scenarios (youtube): Before the switch; Someone who is not an
 * administrator; A video beside a list; A download to completion; Progress;
 * Stopped by a person; A failure; A hang; A flood; A file of that name is
 * there; Read-only storage; The same video again; A bot check.  (downloaders)
 * Sent and followed; Not a configured client; An address for another
 * protocol.  (storage-volumes) Adding the root.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "core/axis-outbound.h"
#include "download/axis-download.h"
#include "download/axis-download-sabnzbd.h"
#include "import/axis-import.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-youtube.h"
#include "youtube/axis-youtube.h"

#define CHANNEL_DIR "Moon Kettle Studio [UCmoonkettle0000000000001]"
#define SEASON_DIR  "Season 2021"
#define WATCH       "https://www.youtube.com/watch?v="

static const gchar standin[] =
	"#!/bin/sh\n"
	"# A stand-in for yt-dlp (tests/test-web-youtube.c).\n"
	"log='@LOG@'\n"
	"last=\n"
	"look=0\n"
	"version=0\n"
	"for a in \"$@\"; do\n"
	"	printf '%s\\n' \"$a\" >> \"$log\"\n"
	"	[ \"$a\" = '--dump-single-json' ] && look=1\n"
	"	[ \"$a\" = '--version' ] && version=1\n"
	"	last=$a\n"
	"done\n"
	"printf '%s\\n' '---' >> \"$log\"\n"
	"{\n"
	"	printf 'HOME=%s\\n' \"${HOME-unset}\"\n"
	"	printf 'XDG_CONFIG_HOME=%s\\n' \"${XDG_CONFIG_HOME-unset}\"\n"
	"	printf 'XDG_CACHE_HOME=%s\\n' \"${XDG_CACHE_HOME-unset}\"\n"
	"	printf 'ALL_PROXY=%s\\n' \"${ALL_PROXY-unset}\"\n"
	"	printf 'HTTPS_PROXY=%s\\n' \"${HTTPS_PROXY-unset}\"\n"
	"	printf 'http_proxy=%s\\n' \"${http_proxy-unset}\"\n"
	"} > '@ENV@'\n"
	"if [ \"$version\" = 1 ]; then echo '2025.01.15'; exit 0; fi\n"
	"id=${last##*v=}\n"
	"if [ \"$id\" = 'botvideo001' ]; then\n"
	"	echo \"ERROR: [youtube] $id: Sign in to confirm you're not a bot\" >&2\n"
	"	exit 1\n"
	"fi\n"
	"if [ \"$look\" = 1 ]; then\n"
	"	[ \"$id\" = 'looksleep01' ] && sleep 60\n"
	"	if [ \"$id\" = 'nullvideo01' ]; then\n"
	"		echo 'null'\n"
	"		echo \"ERROR: [youtube] $id: Unable to download API page: [Errno 111] Connection refused\" >&2\n"
	"		exit 1\n"
	"	fi\n"
	"	live='not_live'\n"
	"	[ \"$id\" = 'livevideo01' ] && live='is_live'\n"
	"	shown=$id\n"
	"	[ \"$id\" = 'otherid0001' ] && shown='zzzzzzzzzzz'\n"
	"	printf '{\"id\":\"%s\",\"_type\":\"video\",\"title\":\"The Lost Spoon: Part 1/2\",' \"$shown\"\n"
	"	printf '\"channel\":\"Moon Kettle Studio\",\"channel_id\":\"UCmoonkettle0000000000001\",'\n"
	"	printf '\"upload_date\":\"20210315\",\"duration\":1260.5,\"live_status\":\"%s\",' \"$live\"\n"
	"	printf '\"availability\":\"public\",\"requested_formats\":['\n"
	"	printf '{\"height\":1080,\"vcodec\":\"avc1.640028\",\"acodec\":\"none\",\"filesize\":4000},'\n"
	"	printf '{\"vcodec\":\"none\",\"acodec\":\"mp4a.40.2\",\"filesize_approx\":1000}],'\n"
	"	printf '\"description\":\"made up\"}\\n'\n"
	"	exit 0\n"
	"fi\n"
	"echo $$ > '@PID@'\n"
	"case \"$id\" in\n"
	"failvideo01)\n"
	"	printf 'partial' > video.f137.mp4.part\n"
	"	echo 'WARNING: something small' >&2\n"
	"	echo 'ERROR: unable to download video data: HTTP Error 403: Forbidden' >&2\n"
	"	exit 1;;\n"
	"hangvideo01)\n"
	"	printf 'partial' > video.f137.mp4.part\n"
	"	sleep 60\n"
	"	exit 0;;\n"
	"floodvideo1)\n"
	"	printf 'partial' > video.f137.mp4.part\n"
	"	exec yes 'junk junk junk junk junk junk junk junk junk junk';;\n"
	"nofilevid01)\n"
	"	exit 0;;\n"
	"mp4video001)\n"
	"	echo 'AXIS finished 4000 4000 NA NA NA'\n"
	"	printf 'a made-up video file, served whole' > video.mp4\n"
	"	exit 0;;\n"
	"twofilevid1)\n"
	"	printf 'one' > video.mp4\n"
	"	printf 'two' > video.webm\n"
	"	exit 0;;\n"
	"longvideo01)\n"
	"	printf 'partial' > video.f137.mp4.part\n"
	"	i=0\n"
	"	while [ $i -lt 300 ]; do\n"
	"		echo \"AXIS downloading $((1000 + i)) 4000 NA 100 9\"\n"
	"		sleep 0.1\n"
	"		i=$((i + 1))\n"
	"	done\n"
	"	exit 1;;\n"
	"esac\n"
	"echo 'AXIS downloading 1000 4000 NA 2000.5 2'\n"
	"printf 'video' > video.f137.mp4.part\n"
	"if [ \"$id\" = 'slowvideo01' ]; then sleep 30; fi\n"
	"echo 'AXIS downloading 4000 4000 NA 2000 0'\n"
	"echo 'AXIS finished 4000 4000 NA NA NA'\n"
	"echo 'AXIS downloading 500 NA 1000 100 5'\n"
	"echo 'AXIS finished 1000 1000 NA NA NA'\n"
	"rm -f video.f137.mp4.part\n"
	"printf 'a made-up video file' > video.mkv\n"
	"exit 0\n";

typedef struct
{
	AxisTestLibrary    *t;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisDownloads      *downloads;
	AxisWebDownloads   *downloads_ui;
	AxisYoutube        *youtube;
	AxisWebYoutube     *ui;
	AxisWebHub         *hub;
	AxisModuleRegistry *registry;
	AxisJobRegistry    *job_registry;
	AxisJobQueue       *queue;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
	gchar              *disk;
	gchar              *root_dir;
	gchar              *script;
	gchar              *log;
	gchar              *pid_file;
	gchar              *env_file;	/* what the stand-in's last run had in its environment */
	gchar              *state_dir;	/* yt-dlp's home, settings folder and cache are in it */
	gint64              volume_id;
	gint64              root_id;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_auto(GStrv) parts = NULL;
	const AxisModuleInfo *module;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(10);
	if (fx->t == NULL)
		return;
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	/* Spec (storage-volumes): Adding the root */
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "YouTube", AXIS_MEDIA_YOUTUBE);
	fx->root_dir = g_build_filename(fx->disk, "YouTube", NULL);

	/* The stand-in, with where it records its arguments and its process. */
	fx->script = g_build_filename(fx->t->dir, "yt-dlp-standin", NULL);
	fx->log = g_build_filename(fx->t->dir, "standin-arguments", NULL);
	fx->pid_file = g_build_filename(fx->t->dir, "standin-pid", NULL);
	fx->env_file = g_build_filename(fx->t->dir, "standin-environment", NULL);
	fx->state_dir = g_build_filename(fx->t->dir, "youtube-state", NULL);
	parts = g_strsplit(standin, "@LOG@", 2);
	{
		g_autofree gchar *joined = g_strconcat(parts[0], fx->log, parts[1], NULL);
		g_auto(GStrv) again = g_strsplit(joined, "@PID@", 2);
		g_autofree gchar *with_pid = g_strconcat(again[0], fx->pid_file, again[1], NULL);
		g_auto(GStrv) third = g_strsplit(with_pid, "@ENV@", 2);

		text = g_strconcat(third[0], fx->env_file, third[1], NULL);
	}
	g_assert_true(g_file_set_contents(fx->script, text, -1, &error));
	g_assert_cmpint(g_chmod(fx->script, 0755), ==, 0);

	fx->settings = axis_settings_new();
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->youtube = axis_youtube_new(fx->t->pool, fx->settings, fx->t->storage, fx->downloads, fx->t->library,
		fx->state_dir, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fx->youtube);
	fx->hub = axis_web_hub_new(NULL);
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, fx->hub);
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->ui = axis_web_youtube_new(fx->youtube, fx->t->storage);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_youtube_get_module(), fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	/* The free space imports keep is honoured by a fetch too. */
	g_assert_true(axis_settings_add_table(fx->settings, AXIS_IMPORT_MODULE_ID, axis_import_get_setting_table(), &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_YOUTUBE_SETTING_PROGRAM, fx->script, &error));
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
	fx->session = soup_session_new_with_options("timeout", 60, "idle-timeout", 60, NULL);

	/* The fetch job, on a real queue, for the whole test. */
	module = axis_web_youtube_get_module();
	g_assert_cmpstr(module->job_handlers[0].kind, ==, AXIS_YOUTUBE_FETCH_KIND);
	g_assert_cmpstr(module->job_handlers[0].queue, ==, AXIS_YOUTUBE_QUEUE);
	fx->job_registry = axis_job_registry_new();
	g_assert_true(axis_job_registry_add(fx->job_registry, &module->job_handlers[0], fx->ui, &error));
	fx->queue = axis_job_queue_new(fx->t->pool, fx->job_registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_stop(fx->web);
	axis_web_youtube_stop(fx->ui);
	axis_web_downloads_stop(fx->downloads_ui);
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_clear_object(&fx->queue);
	axis_job_registry_unref(fx->job_registry);
	axis_web_hub_free(fx->hub);
	axis_web_free(fx->web);
	axis_web_youtube_free(fx->ui);
	axis_web_downloads_free(fx->downloads_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_youtube_free(fx->youtube);
	axis_downloads_unref(fx->downloads);
	axis_settings_unref(fx->settings);
	g_free(fx->disk);
	g_free(fx->root_dir);
	g_free(fx->script);
	g_free(fx->log);
	g_free(fx->pid_file);
	g_free(fx->env_file);
	g_free(fx->state_dir);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_library_free(fx->t);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->t != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

/* A form posted from a page the user has open: its CSRF token from there. */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *page,
	const gchar *path,
	const gchar *form
){
	AxisTestReply opened = get_as(fx, cookie, page);
	g_autofree gchar *token = axis_test_web_csrf_token(opened.body);
	g_autofree gchar *escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	g_autofree gchar *body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");

	axis_test_reply_clear(&opened);
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
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
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain != NULL && strstr(plain, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
switch_on(Fixture *fx)
{
	AxisTestReply reply = post_as(fx, fx->admin, "/admin/youtube", "/admin/youtube/switch", "on=yes&understood=yes");

	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
}

/* Posts @address to be looked up; the reply. */
static AxisTestReply
look(
	Fixture     *fx,
	const gchar *address
){
	g_autofree gchar *escaped = g_uri_escape_string(address, NULL, FALSE);
	g_autofree gchar *form = g_strconcat("address=", escaped, NULL);

	return post_as(fx, fx->admin, "/youtube", "/youtube/look", form);
}

static AxisTestReply
download(
	Fixture     *fx,
	const gchar *video_id
){
	g_autofree gchar *form = g_strdup_printf("video=%s&root=%" G_GINT64_FORMAT, video_id, fx->root_id);

	return post_as(fx, fx->admin, "/youtube", "/youtube/download", form);
}

/* Looks @video_id up and asks for its download; both must be taken. */
static void
look_and_download(
	Fixture     *fx,
	const gchar *video_id
){
	g_autofree gchar *address = g_strconcat(WATCH, video_id, NULL);
	AxisTestReply looked = look(fx, address);
	AxisTestReply asked;

	if (looked.status != 303)
		g_error("the look was not taken (%u):\n%s", looked.status, looked.body);
	axis_test_reply_clear(&looked);
	asked = download(fx, video_id);
	if (asked.status != 303)
		g_error("the download was not taken (%u):\n%s", asked.status, asked.body);
	g_assert_cmpstr(axis_test_reply_header(&asked, "Location"), ==, "/youtube?done=queued");
	axis_test_reply_clear(&asked);
}

static gboolean
wait_video(
	Fixture     *fx,
	const gchar *video_id,
	const gchar *state
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM youtube_video WHERE video_id = '%s'", video_id);

	return axis_test_wait_query(fx->t->db, sql, state, 2 * AXIS_TEST_WAIT_MS);
}

static gboolean
wait_download(
	Fixture     *fx,
	const gchar *video_id,
	const gchar *state
){
	g_autofree gchar *sql = g_strdup_printf("SELECT state FROM download WHERE client_ref = '%s' AND protocol = 'youtube'"
		" ORDER BY id DESC LIMIT 1", video_id);

	return axis_test_wait_query(fx->t->db, sql, state, 2 * AXIS_TEST_WAIT_MS);
}

/* The first column of the first row of @sql is @expected. */
static void
assert_query(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *got = axis_test_db_query(fx->t->db, sql);

	g_assert_cmpstr(got, ==, expected);
}

static gchar *
video_field(
	Fixture     *fx,
	const gchar *video_id,
	const gchar *column
){
	return axis_test_db_queryf(fx->t->db, "SELECT %s::text FROM youtube_video WHERE video_id = '%s'", column, video_id);
}

static gchar *
final_path(
	Fixture     *fx,
	const gchar *video_id
){
	g_autofree gchar *name = g_strdup_printf("Moon Kettle Studio - 2021-03-15 - The Lost Spoon - Part 1-2 [%s].mkv",
		video_id);

	return g_build_filename(fx->root_dir, CHANNEL_DIR, SEASON_DIR, name, NULL);
}

/* How many names are in the folder @path (0 when it is not there). */
static guint
dir_entries(const gchar *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	guint n = 0;

	if (dir == NULL)
		return 0;
	while (g_dir_read_name(dir) != NULL)
		n++;
	g_dir_close(dir);
	return n;
}

/* How many names are in the folder yt-dlp works in (0 when it is not there). */
static guint
staging_entries(Fixture *fx)
{
	g_autofree gchar *path = g_build_filename(fx->root_dir, AXIS_YOUTUBE_STAGING_DIR, NULL);

	return dir_entries(path);
}

static gboolean
staging_is_empty(gpointer data)
{
	return staging_entries(data) == 0;
}

/* Nothing of a fetch is left in the library folder: no folder of yt-dlp's,
 * no temporary name, no channel folder. */
static void
assert_nothing_left(Fixture *fx)
{
	g_autofree gchar *channel = g_build_filename(fx->root_dir, CHANNEL_DIR, NULL);

	g_assert_true(axis_test_pump_until(staging_is_empty, fx, AXIS_TEST_WAIT_MS));
	g_assert_false(g_file_test(channel, G_FILE_TEST_EXISTS));
}

/* How many times the stand-in was started for a video (a look or a fetch:
 * every such command has "--" before the address; asking its version, which
 * a page does once YouTube is switched on, is not counted). */
static guint
standin_runs(Fixture *fx)
{
	g_autofree gchar *text = NULL;
	g_auto(GStrv) lines = NULL;
	guint n = 0;
	guint i;

	if (!g_file_get_contents(fx->log, &text, NULL, NULL))
		return 0;
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i] != NULL; i++) {
		if (strcmp(lines[i], "--") == 0)
			n++;
	}
	return n;
}

static gchar *
standin_arguments(Fixture *fx)
{
	gchar *text = NULL;

	g_file_get_contents(fx->log, &text, NULL, NULL);
	return text != NULL ? text : g_strdup("");
}

static GPid
standin_pid(Fixture *fx)
{
	g_autofree gchar *text = NULL;

	if (!g_file_get_contents(fx->pid_file, &text, NULL, NULL))
		return 0;
	return (GPid)g_ascii_strtoll(text, NULL, 10);
}

static gboolean
standin_started(gpointer data)
{
	return standin_pid(data) > 0;
}

static gboolean
standin_gone(gpointer data)
{
	GPid pid = standin_pid(data);

	return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH;
}

/* The downloads layer looks at the built-in client now, as its tracking
 * job does every few seconds. */
static void
poll_builtin(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 client = 0;

	g_assert_true(axis_downloads_builtin_client(fx->downloads, AXIS_YOUTUBE_KIND_NAME, &client, &error));
	g_assert_true(axis_downloads_poll_client(fx->downloads, client, NULL, NULL, &error));
	g_assert_no_error(error);
}

/* The job queue again, after a test stopped it to change something between
 * a download being queued and its fetch.  (A send also queues the downloads
 * layer's tracking job, whose kind this test's queue does not have.) */
static void
start_queue_again(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'download.track' AND state = 'queued'");
	g_clear_object(&fx->queue);
	fx->queue = axis_job_queue_new(fx->t->pool, fx->job_registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

/* ── The switch, and who may ────────────────────────────────────── */

/* Spec: Before the switch */
static void
test_off_until_switched_on(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/youtube");
	AxisTestReply admin = get_as(fx, fx->admin, "/admin/youtube");
	AxisTestReply looked = look(fx, WATCH "okvideo0001");
	AxisTestReply unticked = post_as(fx, fx->admin, "/admin/youtube", "/admin/youtube/switch", "on=yes");
	AxisTestReply after;

	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "YouTube is switched off");
	assert_lacks(page.body, "name=\"address\"");
	g_assert_cmpuint(admin.status, ==, 200);
	assert_contains(admin.body, "may be against YouTube&#39;s terms of service");
	assert_contains(admin.body, "Switch YouTube on");
	assert_contains(admin.body, "Not asked while YouTube is switched off");
	g_assert_cmpuint(looked.status, ==, 409);
	assert_contains(looked.body, "YouTube is switched off");
	g_assert_cmpuint(unticked.status, ==, 400);
	/* yt-dlp was not run, for a video or for its version. */
	g_assert_cmpuint(standin_runs(fx), ==, 0);
	g_assert_cmpuint(axis_youtube_get_runs(fx->youtube), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM youtube_video"), ==, 0);

	switch_on(fx);
	after = get_as(fx, fx->admin, "/admin/youtube");
	assert_contains(after.body, "YouTube is switched on");
	assert_contains(after.body, "Switched on by root");
	assert_contains(after.body, "2025.01.15 (as the program says)");
	assert_contains(after.body, fx->script);
	axis_test_reply_clear(&after);
	after = get_as(fx, fx->admin, "/youtube");
	assert_contains(after.body, "name=\"address\"");
	axis_test_reply_clear(&after);
	axis_test_web_assert_tables(page.body);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&unticked);
}

/* Spec: Someone who is not an administrator */
static void
test_administrators_only(Fixture *fx, gconstpointer data)
{
	AxisTestReply looked;
	AxisTestReply asked;
	AxisTestReply switched;
	AxisTestReply admin_page;
	AxisTestReply page;

	switch_on(fx);
	looked = axis_test_web_request(fx->session, fx->port, "POST", "/youtube/look", fx->viewer, NULL,
		"address=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3Dokvideo0001");
	asked = axis_test_web_request(fx->session, fx->port, "POST", "/youtube/download", fx->viewer, NULL,
		"video=okvideo0001");
	switched = axis_test_web_request(fx->session, fx->port, "POST", "/admin/youtube/switch", fx->viewer, NULL, "on=no");
	admin_page = get_as(fx, fx->viewer, "/admin/youtube");
	page = get_as(fx, fx->viewer, "/youtube?video=okvideo0001");

	g_assert_cmpuint(looked.status, ==, 403);
	g_assert_cmpuint(asked.status, ==, 403);
	g_assert_cmpuint(switched.status, ==, 403);
	g_assert_cmpuint(admin_page.status, ==, 403);
	/* The list is for everyone; the box is not. */
	g_assert_cmpuint(page.status, ==, 200);
	assert_lacks(page.body, "name=\"address\"");
	assert_lacks(page.body, "/youtube/download");
	assert_lacks(page.body, "href=\"/admin/youtube\"");
	/* A post without the page's token is refused whoever sends it. */
	axis_test_reply_clear(&looked);
	looked = axis_test_web_request(fx->session, fx->port, "POST", "/youtube/look", fx->admin, NULL,
		"address=https%3A%2F%2Fwww.youtube.com%2Fwatch%3Fv%3Dokvideo0001");
	g_assert_cmpuint(looked.status, ==, 403);
	g_assert_cmpuint(standin_runs(fx), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM youtube_video"), ==, 0);
	assert_query(fx, "SELECT enabled::text FROM youtube_state", "true");

	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&asked);
	axis_test_reply_clear(&switched);
	axis_test_reply_clear(&admin_page);
	axis_test_reply_clear(&page);
}

/* Spec: Text that looks like an option; A playlist or a channel */
static void
test_addresses_refused_on_the_page(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *address;
		const gchar *words;
	} cases[] = {
		{ "--exec=touch /tmp/axis-made-up", "no spaces" },
		{ "--exec=id", "must begin with https://" },
		{ "http://www.youtube.com/watch?v=okvideo0001", "must begin with https://" },
		{ "https://video.example/watch?v=okvideo0001", "Only addresses at youtube.com and youtu.be" },
		{ "https://www.youtube.com/watch?v=okvideo0001 --exec id", "no spaces" },
		{ "https://www.youtube.com/playlist?list=PLmadeup0000000000000000000000001", "That is a playlist" },
		{ "https://www.youtube.com/@MoonKettleStudio", "That is a channel" },
		{ "https://www.youtube.com/watch?v=short", "no video in it" }
	};
	guint i;

	switch_on(fx);
	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		AxisTestReply reply = look(fx, cases[i].address);

		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, cases[i].words);
		axis_test_reply_clear(&reply);
	}
	/* Only the version was ever asked of yt-dlp (by the page). */
	{
		g_autofree gchar *arguments = standin_arguments(fx);

		g_assert_null(strstr(arguments, "exec"));
		g_assert_null(strstr(arguments, "watch"));
	}
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM youtube_video"), ==, 0);
}

/* ── A download to completion ───────────────────────────────────── */

/* Spec: A video beside a list; A download to completion; Sent and followed;
 * Not a configured client; The same video again */
static void
test_download_to_completion(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = final_path(fx, "okvideo0001");
	g_autofree gchar *content = NULL;
	g_autofree gchar *arguments = NULL;
	g_autofree gchar *rel = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply looked;
	AxisTestReply shown;
	AxisTestReply asked;
	AxisTestReply page;
	AxisTestReply downloads;
	AxisTestReply clients;
	guint runs;
	guint n_clients = 99;

	switch_on(fx);
	/* As a person pastes it: with a list beside the video, and a space. */
	looked = look(fx, " https://www.youtube.com/watch?v=okvideo0001&list=PLmadeup0000000000000000000000001&index=2 ");
	g_assert_cmpuint(looked.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&looked, "Location"), ==, "/youtube?video=okvideo0001");
	shown = get_as(fx, fx->admin, "/youtube?video=okvideo0001");
	g_assert_cmpuint(shown.status, ==, 200);
	assert_contains(shown.body, "The Lost Spoon: Part 1/2");
	assert_contains(shown.body, "Moon Kettle Studio");
	assert_contains(shown.body, "2021-03-15");
	assert_contains(shown.body, "21 min 00 s");
	assert_contains(shown.body, "1080p, avc1 with mp4a");
	assert_contains(shown.body, "/youtube/download");
	/* Looked up, and nothing downloaded yet. */
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 0);

	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 303);
	g_assert_true(wait_video(fx, "okvideo0001", "fetched"));
	g_assert_true(wait_download(fx, "okvideo0001", "imported"));

	/* The file, under its channel and year, named from what yt-dlp said. */
	g_assert_true(g_file_get_contents(path, &content, NULL, &error));
	g_assert_cmpstr(content, ==, "a made-up video file");
	g_assert_true(axis_test_pump_until(staging_is_empty, fx, AXIS_TEST_WAIT_MS));
	rel = video_field(fx, "okvideo0001", "rel_path");
	g_assert_cmpstr(rel, ==, "YouTube/" CHANNEL_DIR "/" SEASON_DIR
		"/Moon Kettle Studio - 2021-03-15 - The Lost Spoon - Part 1-2 [okvideo0001].mkv");

	/* Recorded as a download of the built-in client, like any other. */
	assert_query(fx, "SELECT protocol || '/' || source || '/' || client_name || '/' || release_name FROM download",
		"youtube/address/YouTube (yt-dlp)/Moon Kettle Studio - The Lost Spoon: Part 1/2 [okvideo0001]");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 1);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download WHERE volume_id = %" G_GINT64_FORMAT
		" AND rel_path IS NOT NULL", fx->volume_id), ==, 1);

	/* What yt-dlp was given: Axis's own address, last, after "--"; none of
	 * what was pasted beside the video. */
	arguments = standin_arguments(fx);
	g_assert_nonnull(strstr(arguments, "--ignore-config\n"));
	g_assert_nonnull(strstr(arguments, "--no-cookies\n"));
	g_assert_nonnull(strstr(arguments, "\n--\nhttps://www.youtube.com/watch?v=okvideo0001\n---\n"));
	g_assert_null(strstr(arguments, "PLmadeup"));
	g_assert_null(strstr(arguments, "index=2"));

	/* What it ran in: a home and a settings folder of Axis's own that hold
	 * nothing and that only Axis can write to (yt-dlp loads plugins from
	 * under both), its cache beside them, and no proxy. */
	{
		g_autofree gchar *home = g_build_filename(fx->state_dir, "home", NULL);
		g_autofree gchar *config = g_build_filename(fx->state_dir, "config", NULL);
		g_autofree gchar *cache = g_build_filename(fx->state_dir, "cache", NULL);
		g_autofree gchar *expected = g_strdup_printf("HOME=%s\nXDG_CONFIG_HOME=%s\nXDG_CACHE_HOME=%s\nALL_PROXY=unset\n"
			"HTTPS_PROXY=unset\nhttp_proxy=unset\n", home, config, cache);
		g_autofree gchar *cache_option = g_strdup_printf("--cache-dir\n%s\n", cache);
		g_autofree gchar *environment = NULL;
		struct stat st;

		g_assert_true(g_file_get_contents(fx->env_file, &environment, NULL, &error));
		g_assert_cmpstr(environment, ==, expected);
		g_assert_nonnull(strstr(arguments, cache_option));
		g_assert_cmpint(g_lstat(home, &st), ==, 0);
		g_assert_true(S_ISDIR(st.st_mode));
		g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
		g_assert_cmpuint(dir_entries(home), ==, 0);
		g_assert_cmpint(g_lstat(config, &st), ==, 0);
		g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
		g_assert_cmpuint(dir_entries(config), ==, 0);
	}

	/* On the pages. */
	page = get_as(fx, fx->admin, "/youtube");
	assert_contains(page.body, "In the library");
	assert_contains(page.body, "Disk one: YouTube/" CHANNEL_DIR);
	axis_test_web_assert_tables(page.body);
	downloads = get_as(fx, fx->admin, "/downloads");
	g_assert_cmpuint(downloads.status, ==, 200);
	assert_contains(downloads.body, "Moon Kettle Studio - The Lost Spoon: Part 1/2 [okvideo0001]");
	assert_contains(downloads.body, "YouTube (yt-dlp)");
	/* No client an administrator set up, and still a download to show: the
	 * page does not say that there is no client at all, and the dashboard
	 * has its card. */
	assert_contains(downloads.body, "No download client for Usenet or torrents is set up yet");
	assert_contains(downloads.body, "YouTube videos, which Axis downloads itself");
	assert_lacks(downloads.body, "No download client is set up yet");
	{
		AxisTestReply board = get_as(fx, fx->admin, "/");

		g_assert_cmpuint(board.status, ==, 200);
		assert_contains(board.body, "Coming into the library");
		assert_contains(board.body, "The Lost Spoon");
		axis_test_reply_clear(&board);
	}

	/* Its file is in the library's index after the folder's scan. */
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM library_file WHERE root_id = %" G_GINT64_FORMAT,
		fx->root_id), ==, 1);

	/* Spec (downloaders): Not a configured client */
	g_assert_true(axis_downloads_count_clients(fx->downloads, &n_clients, &error));
	g_assert_cmpuint(n_clients, ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download_client WHERE address = ''"), ==, 1);
	clients = get_as(fx, fx->admin, "/downloads/clients");
	assert_lacks(clients.body, "YouTube (yt-dlp)");

	/* Spec: The same video again */
	runs = standin_runs(fx);
	axis_test_reply_clear(&looked);
	looked = look(fx, "https://youtu.be/okvideo0001");
	g_assert_cmpuint(looked.status, ==, 303);
	axis_test_reply_clear(&shown);
	shown = get_as(fx, fx->admin, "/youtube?video=okvideo0001");
	assert_contains(shown.body, "It is in the library already");
	axis_test_reply_clear(&asked);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 409);
	assert_contains(asked.body, "This video is in the library already");
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 1);

	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&shown);
	axis_test_reply_clear(&asked);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&downloads);
	axis_test_reply_clear(&clients);
}

/* Spec: Progress; Stopped by a person */
static void
test_progress_and_stop(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = final_path(fx, "slowvideo01");
	g_autofree gchar *download_id = NULL;
	g_autofree gchar *remove = NULL;
	g_autofree gchar *message = NULL;
	AxisTestReply page;
	AxisTestReply downloads;
	AxisTestReply confirm;
	AxisTestReply removed;
	GPid pid;

	switch_on(fx);
	look_and_download(fx, "slowvideo01");
	g_assert_true(wait_video(fx, "slowvideo01", "fetching"));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT done_bytes::text FROM youtube_video WHERE video_id ="
		" 'slowvideo01'", "1000", AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(standin_started, fx, AXIS_TEST_WAIT_MS));
	pid = standin_pid(fx);
	g_assert_cmpint(kill(pid, 0), ==, 0);
	g_assert_cmpuint(staging_entries(fx), ==, 1);

	/* Spec: Progress.  Tracking carries it into the download's record. */
	poll_builtin(fx);
	assert_query(fx, "SELECT state || ' ' || done_bytes || ' of ' || size_bytes FROM download",
		"downloading 1000 of 5000");
	downloads = get_as(fx, fx->admin, "/downloads");
	assert_contains(downloads.body, "Moon Kettle Studio - The Lost Spoon: Part 1/2 [slowvideo01]");
	assert_contains(downloads.body, "20%");
	page = get_as(fx, fx->admin, "/youtube");
	assert_contains(page.body, "20% downloaded");

	/* Spec: Stopped by a person, from the Downloads page. */
	download_id = axis_test_db_query(fx->t->db, "SELECT id::text FROM download");
	remove = g_strdup_printf("/downloads/remove/%s", download_id);
	confirm = get_as(fx, fx->admin, remove);
	g_assert_cmpuint(confirm.status, ==, 200);
	assert_contains(confirm.body, "stops it and deletes what was downloaded so far");
	removed = post_as(fx, fx->admin, remove, remove, "confirm=yes");
	g_assert_cmpuint(removed.status, ==, 303);
	g_assert_true(axis_test_pump_until(standin_gone, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT state FROM job WHERE kind = 'youtube.fetch'", "cancelled",
		AXIS_TEST_WAIT_MS));
	assert_nothing_left(fx);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	assert_query(fx, "SELECT state FROM download", "removed");
	assert_query(fx, "SELECT state FROM youtube_video", "cancelled");
	message = video_field(fx, "slowvideo01", "message");
	g_assert_cmpstr(message, ==, "Stopped by a person.");

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&downloads);
	axis_test_reply_clear(&confirm);
	axis_test_reply_clear(&removed);
}

/* ── Failures ───────────────────────────────────────────────────── */

/* One fetch that must fail: its reason on the video and on its download,
 * and nothing left in the library folder. */
static void
assert_fails(
	Fixture     *fx,
	const gchar *video_id,
	const gchar *words
){
	g_autofree gchar *path = final_path(fx, video_id);
	g_autofree gchar *message = NULL;
	g_autofree gchar *said = NULL;
	AxisTestReply page;

	look_and_download(fx, video_id);
	g_assert_true(wait_video(fx, video_id, "failed"));
	g_assert_true(wait_download(fx, video_id, "failed"));
	message = video_field(fx, video_id, "message");
	if (message == NULL || strstr(message, words) == NULL)
		g_error("expected the reason to say '%s', but it says: %s", words, message != NULL ? message : "(nothing)");
	said = axis_test_db_queryf(fx->t->db, "SELECT message FROM download WHERE client_ref = '%s'", video_id);
	g_assert_cmpstr(said, ==, message);
	assert_nothing_left(fx);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT state FROM job WHERE kind = 'youtube.fetch'"
		" ORDER BY id DESC LIMIT 1", "failed", AXIS_TEST_WAIT_MS));
	page = get_as(fx, fx->admin, "/youtube");
	assert_contains(page.body, "Failed");
	assert_contains(page.body, words);
	axis_test_reply_clear(&page);
}

/* Spec: A failure */
static void
test_failure(Fixture *fx, gconstpointer data)
{
	AxisTestReply again;

	switch_on(fx);
	assert_fails(fx, "failvideo01", "yt-dlp said: unable to download video data: HTTP Error 403: Forbidden");
	/* It can be asked for again, by a person. */
	again = look(fx, WATCH "failvideo01");
	g_assert_cmpuint(again.status, ==, 303);
	assert_query(fx, "SELECT state FROM youtube_video", "listed");
	axis_test_reply_clear(&again);
	/* One that ends well and leaves no file is a failure too. */
	assert_fails(fx, "nofilevid01", "finished without a video file");
	/* Two video files are not one video: neither is taken. */
	assert_fails(fx, "twofilevid1", "more than one video file");
}

/* Spec: A hang */
static void
test_hang(Fixture *fx, gconstpointer data)
{
	AxisYoutubeTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.fetch_timeout_ms = 1500;
	axis_youtube_set_tuning(fx->youtube, &tuning);
	switch_on(fx);
	assert_fails(fx, "hangvideo01", "did not finish within");
	/* Its process, and what it started, are gone. */
	g_assert_true(axis_test_pump_until(standin_gone, fx, AXIS_TEST_WAIT_MS));
}

/* Spec: A flood */
static void
test_flood(Fixture *fx, gconstpointer data)
{
	AxisYoutubeTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.fetch_output_max = 65536;
	axis_youtube_set_tuning(fx->youtube, &tuning);
	switch_on(fx);
	assert_fails(fx, "floodvideo1", "printed more than Axis accepts");
	g_assert_true(axis_test_pump_until(standin_gone, fx, AXIS_TEST_WAIT_MS));
}

/* ── What is never written ──────────────────────────────────────── */

/* Spec: A file of that name is there */
static void
test_existing_file_is_kept(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = final_path(fx, "okvideo0001");
	g_autofree gchar *folder = g_path_get_dirname(path);
	g_autofree gchar *content = NULL;
	AxisTestReply looked;
	AxisTestReply asked;
	guint runs;

	switch_on(fx);
	g_assert_cmpint(g_mkdir_with_parents(folder, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, "a file that was here first", -1, NULL));
	looked = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(looked.status, ==, 303);
	runs = standin_runs(fx);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 409);
	assert_contains(asked.body, "already in the library folder");
	assert_contains(asked.body, "Nothing is replaced");
	/* Nothing queued, nothing run, the file as it was. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE kind = 'youtube.fetch'"), ==, 0);
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "a file that was here first");
	g_assert_cmpuint(staging_entries(fx), ==, 0);

	/* And when it appears after the download was queued: the fetch job
	 * finds it, and replaces nothing. */
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	axis_test_reply_clear(&asked);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 303);
	g_assert_true(g_file_set_contents(path, "a file that came meanwhile", -1, NULL));
	start_queue_again(fx);
	g_assert_true(wait_video(fx, "okvideo0001", "failed"));
	g_clear_pointer(&content, g_free);
	g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "a file that came meanwhile");
	g_clear_pointer(&content, g_free);
	content = video_field(fx, "okvideo0001", "message");
	g_assert_nonnull(strstr(content, "Nothing was replaced"));
	g_assert_true(axis_test_pump_until(staging_is_empty, fx, AXIS_TEST_WAIT_MS));

	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&asked);
}

/* Spec: Read-only storage */
static void
test_read_only_storage(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *staging = g_build_filename(fx->root_dir, AXIS_YOUTUBE_STAGING_DIR, NULL);
	g_autoptr(GError) error = NULL;
	AxisTestReply looked;
	AxisTestReply asked;
	guint runs;

	switch_on(fx);
	looked = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(looked.status, ==, 303);
	runs = standin_runs(fx);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 409);
	assert_contains(asked.body, "Storage is read-only");
	assert_contains(asked.body, "storage.read_only");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE kind = 'youtube.fetch'"), ==, 0);
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	g_assert_false(g_file_test(staging, G_FILE_TEST_EXISTS));

	/* Switched on between the question and the fetch: the job writes
	 * nothing either. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	axis_test_reply_clear(&asked);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 303);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	start_queue_again(fx);
	g_assert_true(wait_video(fx, "okvideo0001", "failed"));
	g_assert_true(wait_download(fx, "okvideo0001", "failed"));
	{
		g_autofree gchar *message = video_field(fx, "okvideo0001", "message");

		g_assert_nonnull(strstr(message, "Storage is read-only"));
	}
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	g_assert_false(g_file_test(staging, G_FILE_TEST_EXISTS));

	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&asked);
}

/* The free space imports keep is kept by a fetch too. */
static void
test_free_space_floor(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *staging = g_build_filename(fx->root_dir, AXIS_YOUTUBE_STAGING_DIR, NULL);
	g_autoptr(GError) error = NULL;
	guint runs;

	AxisYoutubeTuning tuning;
	AxisTestReply looked;
	AxisTestReply asked;

	switch_on(fx);
	looked = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(looked.status, ==, 303);
	runs = standin_runs(fx);

	/* Asked for while the disk has too little: nothing is queued. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "8000000GiB", &error));
	g_assert_no_error(error);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 409);
	assert_contains(asked.body, "this download needs");
	assert_contains(asked.body, "import.free_space_floor");
	assert_contains(asked.body, "Nothing is downloaded");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE kind = 'youtube.fetch'"), ==, 0);

	/* Too little by the time its turn comes: the fetch job writes nothing. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "1MiB", &error));
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	axis_test_reply_clear(&asked);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 303);
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "8000000GiB", &error));
	start_queue_again(fx);
	g_assert_true(wait_video(fx, "okvideo0001", "failed"));
	{
		g_autofree gchar *message = video_field(fx, "okvideo0001", "message");

		g_assert_nonnull(strstr(message, "free"));
		g_assert_nonnull(strstr(message, "Nothing was written"));
	}
	/* yt-dlp was not started for the fetch, and no folder was made. */
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	g_assert_false(g_file_test(staging, G_FILE_TEST_EXISTS));

	/* And while a video comes in: the disk is looked at again as yt-dlp
	 * reports (once a minute; here ten times a second), and a disk below the
	 * floor stops it, with nothing of it left. */
	memset(&tuning, 0, sizeof(tuning));
	tuning.space_check_ms = 100;
	axis_youtube_set_tuning(fx->youtube, &tuning);
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "1MiB", &error));
	look_and_download(fx, "longvideo01");
	g_assert_true(wait_video(fx, "longvideo01", "fetching"));
	g_assert_true(axis_test_pump_until(standin_started, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_FREE_FLOOR, "8000000GiB", &error));
	g_assert_no_error(error);
	g_assert_true(wait_video(fx, "longvideo01", "failed"));
	{
		g_autofree gchar *message = video_field(fx, "longvideo01", "message");

		g_assert_nonnull(strstr(message, "less than the"));
		g_assert_nonnull(strstr(message, "the download was stopped"));
	}
	g_assert_true(axis_test_pump_until(standin_gone, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(staging_is_empty, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(wait_download(fx, "longvideo01", "failed"));

	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&asked);
}

/* ── What a run cut short leaves, and what stops one ────────────── */

#define STOPPED_WORDS "Axis stopped while it was downloading; download it again."

/* A daemon killed during a fetch: the video does not stay "downloading" for
 * ever, it can be asked for again, and the folder yt-dlp worked in goes. */
static void
test_killed_daemon(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *staging = g_build_filename(fx->root_dir, AXIS_YOUTUBE_STAGING_DIR, NULL);
	g_autofree gchar *outside = g_build_filename(fx->t->dir, "outside", NULL);
	g_autofree gchar *outside_file = g_build_filename(outside, "kept.txt", NULL);
	g_autofree gchar *job = NULL;
	g_autofree gchar *left = NULL;
	g_autofree gchar *left_file = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *other_file = NULL;
	g_autofree gchar *not_ours = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *again_left = NULL;
	g_autofree gchar *path = final_path(fx, "okvideo0001");
	g_autofree gchar *message = NULL;
	AxisTestReply page;
	AxisTestReply asked;

	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "youtube: * video download(s) were cut short*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "youtube: removed * folder(s)*");
	switch_on(fx);
	/* As the kill leaves things: the record says it is being fetched, its
	 * job is no longer queued or running (the job queue of the next start
	 * took it back, and it had its one attempt), and yt-dlp's folder is on
	 * the disk with what it had written. */
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	look_and_download(fx, "okvideo0001");
	job = axis_test_db_query(fx->t->db, "SELECT job_id::text FROM youtube_video");
	g_assert_nonnull(job);
	axis_test_db_exec(fx->t->db, "UPDATE youtube_video SET state = 'fetching', started_at = now(), done_bytes = 1000");
	axis_test_db_exec(fx->t->db, "UPDATE job SET state = 'failed', attempts = 1, finished_at = now(),"
		" last_error = 'taken back' WHERE kind = 'youtube.fetch'");
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'downloading' WHERE protocol = 'youtube'");
	left = g_strdup_printf("%s/okvideo0001-%s", staging, job);
	left_file = g_build_filename(left, "video.f137.mp4.part", NULL);
	other = g_build_filename(staging, "slowvideo01-999999", NULL);
	other_file = g_build_filename(other, "deep", "video.f251.webm.part", NULL);
	not_ours = g_build_filename(staging, "something-else", NULL);
	link = g_build_filename(staging, "hangvideo01-999998", NULL);
	g_assert_cmpint(g_mkdir_with_parents(left, 0700), ==, 0);
	g_assert_true(g_file_set_contents(left_file, "partial", -1, NULL));
	{
		g_autofree gchar *deep = g_path_get_dirname(other_file);

		g_assert_cmpint(g_mkdir_with_parents(deep, 0700), ==, 0);
	}
	g_assert_true(g_file_set_contents(other_file, "partial", -1, NULL));
	g_assert_cmpint(g_mkdir_with_parents(not_ours, 0700), ==, 0);
	/* A link under a fetch's name, to a folder outside: never followed. */
	g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);
	g_assert_true(g_file_set_contents(outside_file, "kept", -1, NULL));
	g_assert_cmpint(symlink(outside, link), ==, 0);

	/* The page said "Downloading"; the next look of the downloads layer
	 * (its tracking, every few seconds) ends that, with a plain reason on
	 * the video and on its download. */
	page = get_as(fx, fx->admin, "/youtube");
	assert_contains(page.body, "Downloading");
	poll_builtin(fx);
	assert_query(fx, "SELECT state FROM youtube_video", "failed");
	message = video_field(fx, "okvideo0001", "message");
	g_assert_cmpstr(message, ==, STOPPED_WORDS);
	assert_query(fx, "SELECT state || ': ' || message FROM download", "failed: " STOPPED_WORDS);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/youtube");
	assert_contains(page.body, "Axis stopped while it was downloading");

	/* The start of the daemon: the folders of fetches that are no longer in
	 * hand go, with what is in them; a name that is not a fetch's stays, and
	 * so does a link and what it leads to. */
	axis_youtube_recover(fx->youtube);
	g_assert_false(g_file_test(left, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(other, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(not_ours, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
	g_assert_true(g_file_test(outside_file, G_FILE_TEST_EXISTS));

	/* It can be asked for again; what another killed run left under this
	 * video's name goes before yt-dlp starts, and the video arrives. */
	again_left = g_build_filename(staging, "okvideo0001-777777", NULL);
	g_assert_cmpint(g_mkdir_with_parents(again_left, 0700), ==, 0);
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 303);
	start_queue_again(fx);
	g_assert_true(wait_video(fx, "okvideo0001", "fetched"));
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(again_left, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(not_ours, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(outside_file, G_FILE_TEST_EXISTS));
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download WHERE state = 'failed'"), ==, 1);
	g_assert_true(wait_download(fx, "okvideo0001", "imported"));

	/* add-attention-notices, slice N2: nothing failed, so no notice; a video
	 * whose download failed is one, and the notice names no video. */
	{
		AxisTestReply notices;

		axis_test_web_read_attention(fx->web);
		notices = get_as(fx, fx->admin, "/attention");
		g_assert_cmpuint(notices.status, ==, 200);
		assert_lacks(notices.body, "Axis could not read");
		assert_lacks(notices.body, "YouTube video");
		axis_test_reply_clear(&notices);
		axis_test_db_exec(fx->t->db, "UPDATE youtube_video SET state = 'failed', finished_at = now(),"
			" message = 'As the test made it.' WHERE video_id = 'okvideo0001'");
		axis_test_web_read_attention(fx->web);
		notices = get_as(fx, fx->admin, "/attention");
		assert_contains(notices.body, "The download of 1 YouTube video failed. Axis does not try it again by itself.");
		assert_contains(notices.body, "<a href=\"/youtube\">Open YouTube</a>");
		assert_lacks(notices.body, "okvideo0001");
		axis_test_reply_clear(&notices);
	}

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&asked);
}

/* The same when Axis was killed and the video is asked for again before
 * anything has looked: it is not "being downloaded already". */
static void
test_killed_then_asked_again(Fixture *fx, gconstpointer data)
{
	AxisTestReply asked;

	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "youtube: * video download(s) were cut short*");
	switch_on(fx);
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	look_and_download(fx, "okvideo0001");
	axis_test_db_exec(fx->t->db, "UPDATE youtube_video SET state = 'fetching', started_at = now()");
	axis_test_db_exec(fx->t->db, "UPDATE job SET state = 'failed', attempts = 1, finished_at = now(),"
		" last_error = 'taken back' WHERE kind = 'youtube.fetch'");
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'downloading' WHERE protocol = 'youtube'");
	asked = download(fx, "okvideo0001");
	g_assert_cmpuint(asked.status, ==, 303);
	/* The download cut short has ended, and the new one is its own record. */
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 2);
	assert_query(fx, "SELECT state || ': ' || message FROM download ORDER BY id LIMIT 1", "failed: " STOPPED_WORDS);
	start_queue_again(fx);
	g_assert_true(wait_video(fx, "okvideo0001", "fetched"));
	g_assert_true(wait_download(fx, "okvideo0001", "imported"));
	axis_test_reply_clear(&asked);
}

typedef struct
{
	AxisYoutube *youtube;
	GError      *error;
	gboolean     ok;
	gint         done;	/* atomic */
} LookCall;

static gpointer
look_thread(gpointer data)
{
	LookCall *call = data;
	AxisYoutubeVideo *video = NULL;

	call->ok = axis_youtube_look(call->youtube, WATCH "looksleep01", "root", &video, &call->error);
	g_clear_pointer(&video, axis_youtube_video_free);
	g_atomic_int_set(&call->done, 1);
	return NULL;
}

static gboolean
three_runs(gpointer data)
{
	return standin_runs(data) >= 3;
}

/* Axis stopping does not wait out a look's two minutes or a fetch's hours:
 * both are cancelled, and the video of the fetch says that Axis stopped. */
static void
test_stopping(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *message = NULL;
	GThread *thread;
	LookCall call;
	gint64 began;
	GPid pid;

	switch_on(fx);
	look_and_download(fx, "slowvideo01");
	g_assert_true(wait_video(fx, "slowvideo01", "fetching"));
	g_assert_true(axis_test_pump_until(standin_started, fx, AXIS_TEST_WAIT_MS));
	pid = standin_pid(fx);
	g_assert_cmpint(kill(pid, 0), ==, 0);
	/* A look beside it, of a video yt-dlp takes a minute over. */
	memset(&call, 0, sizeof(call));
	call.youtube = fx->youtube;
	thread = g_thread_new("look", look_thread, &call);
	g_assert_true(axis_test_pump_until(three_runs, fx, AXIS_TEST_WAIT_MS));
	g_assert_cmpint(g_atomic_int_get(&call.done), ==, 0);

	began = g_get_monotonic_time();
	axis_web_youtube_stop(fx->ui);
	g_thread_join(thread);
	g_assert_cmpint(g_get_monotonic_time() - began, <, 10 * G_USEC_PER_SEC);
	g_assert_false(call.ok);
	g_assert_error(call.error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_FAILED);
	g_assert_nonnull(strstr(call.error->message, "Axis is stopping"));
	g_clear_error(&call.error);

	/* The fetch: yt-dlp is gone, its folder too, and the video and its
	 * download say why, so that nothing shows it downloading for ever. */
	g_assert_true(wait_video(fx, "slowvideo01", "failed"));
	g_assert_cmpint(g_get_monotonic_time() - began, <, 15 * G_USEC_PER_SEC);
	message = video_field(fx, "slowvideo01", "message");
	g_assert_cmpstr(message, ==, STOPPED_WORDS);
	g_assert_true(axis_test_pump_until(standin_gone, fx, AXIS_TEST_WAIT_MS));
	assert_nothing_left(fx);
	g_assert_true(wait_download(fx, "slowvideo01", "failed"));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT state FROM job WHERE kind = 'youtube.fetch'", "failed",
		AXIS_TEST_WAIT_MS));
	/* Nothing starts after. */
	{
		g_autoptr(AxisYoutubeVideo) video = NULL;
		g_autoptr(GError) error = NULL;
		guint runs = standin_runs(fx);

		g_assert_false(axis_youtube_look(fx->youtube, WATCH "okvideo0001", "root", &video, &error));
		g_assert_error(error, AXIS_YOUTUBE_ERROR, AXIS_YOUTUBE_ERROR_OFF);
		g_assert_cmpuint(standin_runs(fx), ==, runs);
	}
}

/* Switching YouTube off stops the download that runs, and says so. */
static void
test_switch_off_stops(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *message = NULL;
	AxisTestReply before;
	AxisTestReply switched;
	AxisTestReply after;

	switch_on(fx);
	before = get_as(fx, fx->admin, "/admin/youtube");
	assert_contains(before.body, "Switching YouTube off stops the download that is running");
	look_and_download(fx, "slowvideo01");
	g_assert_true(wait_video(fx, "slowvideo01", "fetching"));
	g_assert_true(axis_test_pump_until(standin_started, fx, AXIS_TEST_WAIT_MS));

	switched = post_as(fx, fx->admin, "/admin/youtube", "/admin/youtube/switch", "on=no");
	g_assert_cmpuint(switched.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&switched, "Location"), ==, "/admin/youtube?stopped=1");
	after = get_as(fx, fx->admin, "/admin/youtube?stopped=1");
	assert_contains(after.body, "YouTube is switched off. The download that was running was stopped");
	g_assert_true(axis_test_pump_until(standin_gone, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT state FROM job WHERE kind = 'youtube.fetch'", "cancelled",
		AXIS_TEST_WAIT_MS));
	assert_nothing_left(fx);
	assert_query(fx, "SELECT state FROM youtube_video", "cancelled");
	message = video_field(fx, "slowvideo01", "message");
	g_assert_cmpstr(message, ==, "Stopped: YouTube was switched off.");
	g_assert_true(wait_download(fx, "slowvideo01", "failed"));
	assert_query(fx, "SELECT message FROM download", "Stopped: YouTube was switched off.");

	axis_test_reply_clear(&before);
	axis_test_reply_clear(&switched);
	axis_test_reply_clear(&after);
}

/* A video YouTube serves as one file is not merged: it comes as mp4 (or
 * webm), and is taken with the extension it has. */
static void
test_one_file_video(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fx->root_dir, CHANNEL_DIR, SEASON_DIR,
		"Moon Kettle Studio - 2021-03-15 - The Lost Spoon - Part 1-2 [mp4video001].mp4", NULL);
	g_autofree gchar *content = NULL;
	g_autofree gchar *rel = NULL;

	switch_on(fx);
	look_and_download(fx, "mp4video001");
	g_assert_true(wait_video(fx, "mp4video001", "fetched"));
	g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "a made-up video file, served whole");
	rel = video_field(fx, "mp4video001", "rel_path");
	g_assert_true(g_str_has_suffix(rel, " [mp4video001].mp4"));
	g_assert_true(axis_test_pump_until(staging_is_empty, fx, AXIS_TEST_WAIT_MS));
	g_assert_true(wait_download(fx, "mp4video001", "imported"));
}

/* A finished video's download that the fetch could not tell the downloads
 * layer about at once (its own look failed) is found finished by the next
 * ordinary look: it is imported then, never left as one waiting to be. */
static void
test_finished_seen_later(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *file = g_build_filename(fx->root_dir, "seen-later.mkv", NULL);

	switch_on(fx);
	g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	look_and_download(fx, "okvideo0001");
	g_assert_true(g_file_set_contents(file, "a made-up video file", -1, NULL));
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE youtube_video SET state = 'fetched', volume_id = %"
			G_GINT64_FORMAT ", rel_path = 'YouTube/seen-later.mkv', size_bytes = 20, finished_at = now()",
			fx->volume_id);

		axis_test_db_exec(fx->t->db, sql);
	}
	poll_builtin(fx);
	assert_query(fx, "SELECT state || ' ' || rel_path FROM download", "imported YouTube/seen-later.mkv");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download WHERE state = 'completed'"), ==, 0);
	start_queue_again(fx);
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT state FROM job WHERE kind = 'youtube.fetch'", "succeeded",
		AXIS_TEST_WAIT_MS));
	assert_query(fx, "SELECT state FROM youtube_video", "fetched");
}

/* The page everyone signed in can open starts no program. */
static void
test_viewer_starts_nothing(Fixture *fx, gconstpointer data)
{
	AxisTestReply viewer;
	AxisTestReply admin;
	AxisTestReply admin_page;

	/* Switched on, and yt-dlp never asked its version yet. */
	axis_test_db_exec(fx->t->db, "UPDATE youtube_state SET enabled = true, enabled_by = 'root', enabled_at = now()");
	viewer = get_as(fx, fx->viewer, "/youtube");
	g_assert_cmpuint(viewer.status, ==, 200);
	admin = get_as(fx, fx->admin, "/youtube");
	g_assert_cmpuint(admin.status, ==, 200);
	assert_contains(admin.body, "name=\"address\"");
	g_assert_cmpuint(axis_youtube_get_runs(fx->youtube), ==, 0);
	g_assert_false(g_file_test(fx->log, G_FILE_TEST_EXISTS));
	/* The administrator's page asks, once. */
	admin_page = get_as(fx, fx->admin, "/admin/youtube");
	assert_contains(admin_page.body, "2025.01.15 (as the program says)");
	g_assert_cmpuint(axis_youtube_get_runs(fx->youtube), ==, 1);

	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&admin_page);
}

/* yt-dlp loads plugins from under its home: Axis gives it one that holds
 * nothing, and does not run it when something is there. */
static void
test_home_kept_empty(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *home = g_build_filename(fx->state_dir, "home", NULL);
	g_autofree gchar *plugins = g_build_filename(home, "yt-dlp-plugins", NULL);
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;
	AxisTestReply looked;
	AxisTestReply refused;
	AxisTestReply page;
	guint runs;

	switch_on(fx);
	looked = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(looked.status, ==, 303);
	runs = standin_runs(fx);
	g_assert_cmpint(g_mkdir_with_parents(plugins, 0700), ==, 0);
	refused = look(fx, WATCH "failvideo01");
	g_assert_cmpuint(refused.status, ==, 409);
	assert_contains(refused.body, "is not empty");
	assert_contains(refused.body, "loads no plugin");
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	page = get_as(fx, fx->admin, "/admin/youtube");
	assert_contains(page.body, "is not empty");
	/* Emptied, it runs again; and a home others could write to is made
	 * Axis's alone before it does. */
	g_assert_cmpint(g_rmdir(plugins), ==, 0);
	g_assert_cmpint(g_chmod(home, 0777), ==, 0);
	axis_test_reply_clear(&refused);
	refused = look(fx, WATCH "failvideo01");
	g_assert_cmpuint(refused.status, ==, 303);
	{
		struct stat st;

		g_assert_cmpint(g_lstat(home, &st), ==, 0);
		g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	}

	/* The built-in client's name is not one to give a client of one's own
	 * (it would keep Axis from making its own). */
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_download_sabnzbd_kind(), &error));
	g_assert_no_error(error);
	memset(&spec, 0, sizeof(spec));
	spec.name = "youtube (YT-DLP)";
	spec.kind = axis_download_sabnzbd_kind()->name;
	spec.address = "http://sabnzbd.example:8080";
	spec.enabled = TRUE;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	g_assert_false(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_assert_nonnull(strstr(error->message, "the one Axis gives its own built-in download client"));
	g_clear_error(&error);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download_client WHERE address <> ''"), ==, 0);
	/* Any other name is taken as ever. */
	spec.name = "A made-up client";
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &spec, NULL, &error));
	g_assert_no_error(error);

	axis_test_reply_clear(&looked);
	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&page);
}

/* ── What Axis does not fetch, and what holds it back ───────────── */

/* Spec: A bot check */
static void
test_refusal_pauses(Fixture *fx, gconstpointer data)
{
	AxisTestReply refused;
	AxisTestReply paused;
	AxisTestReply page;
	guint runs;

	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "youtube: YouTube refused this server*");
	switch_on(fx);
	refused = look(fx, WATCH "botvideo001");
	g_assert_cmpuint(refused.status, ==, 409);
	assert_contains(refused.body, "YouTube refused this server for now");
	runs = standin_runs(fx);
	/* The next one is not tried: YouTube work is paused. */
	paused = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(paused.status, ==, 409);
	assert_contains(paused.body, "YouTube work is paused until");
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	assert_query(fx, "SELECT (paused_until > now() + interval '50 minutes' AND"
		" paused_until < now() + interval '70 minutes' AND pause_step = 1)::text FROM youtube_state", "true");
	page = get_as(fx, fx->admin, "/admin/youtube");
	assert_contains(page.body, "Paused until");
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM youtube_video"), ==, 0);

	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&paused);
	axis_test_reply_clear(&page);
}

/* A live stream is not fetched; an answer about another video is not used. */
static void
test_not_fetched(Fixture *fx, gconstpointer data)
{
	AxisTestReply live;
	AxisTestReply other;
	AxisTestReply unknown;
	AxisTestReply unreached;

	switch_on(fx);
	/* A look that could not reach YouTube (a proxy that is not there), as
	 * yt-dlp ends it: "null" printed, a status that is not 0, and why on its
	 * standard error.  The reason is yt-dlp's, not a fault of its record. */
	unreached = look(fx, WATCH "nullvideo01");
	g_assert_cmpuint(unreached.status, ==, 409);
	assert_contains(unreached.body, "yt-dlp said: [youtube] nullvideo01: Unable to download API page");
	axis_test_reply_clear(&unreached);
	live = look(fx, WATCH "livevideo01");
	g_assert_cmpuint(live.status, ==, 409);
	assert_contains(live.body, "live stream that is still running");
	other = look(fx, WATCH "otherid0001");
	g_assert_cmpuint(other.status, ==, 409);
	assert_contains(other.body, "answered about another video");
	/* A video that was never looked up cannot be downloaded. */
	unknown = download(fx, "neverlooked");
	g_assert_cmpuint(unknown.status, ==, 404);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM youtube_video"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 0);

	axis_test_reply_clear(&live);
	axis_test_reply_clear(&other);
	axis_test_reply_clear(&unknown);
}

/* Spec (scope): YouTube traffic fails closed under the outbound binding */
static void
test_outbound_binding_fails_closed(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisOutboundTarget target;
	AxisTestReply refused;
	AxisTestReply page;
	AxisTestReply taken;
	guint runs;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "outbound connections: *");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "outbound connections: *");
	switch_on(fx);
	runs = standin_runs(fx);
	g_assert_true(axis_outbound_target_parse("axis-absent0", &target, NULL));
	axis_outbound_configure(&target, AXIS_OUTBOUND_SCOPE_INTERNET);
	refused = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(refused.status, ==, 409);
	assert_contains(refused.body, "outbound.bind");
	assert_contains(refused.body, "Set a proxy for YouTube");
	g_assert_cmpuint(standin_runs(fx), ==, runs);
	page = get_as(fx, fx->admin, "/youtube");
	assert_contains(page.body, "outbound.bind");
	assert_lacks(page.body, "name=\"address\"");

	/* A proxy for YouTube, or the administrator's word that the whole
	 * network is bound, lets it run; the proxy is passed on, and is in no
	 * page. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_YOUTUBE_SETTING_PROXY, "socks5://vpn:1080", &error));
	g_assert_no_error(error);
	taken = look(fx, WATCH "okvideo0001");
	g_assert_cmpuint(taken.status, ==, 303);
	{
		g_autofree gchar *arguments = standin_arguments(fx);
		g_autofree gchar *environment = NULL;

		/* In its environment, in every spelling yt-dlp reads; never in its
		 * command, which the process list shows to everyone. */
		g_assert_null(strstr(arguments, "--proxy"));
		g_assert_null(strstr(arguments, "vpn:1080"));
		g_assert_true(g_file_get_contents(fx->env_file, &environment, NULL, NULL));
		g_assert_nonnull(strstr(environment, "\nALL_PROXY=socks5://vpn:1080\n"));
		g_assert_nonnull(strstr(environment, "\nHTTPS_PROXY=socks5://vpn:1080\n"));
		g_assert_nonnull(strstr(environment, "\nhttp_proxy=socks5://vpn:1080\n"));
	}
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/admin/youtube");
	assert_lacks(page.body, "socks5://vpn:1080");
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);

	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&taken);
}

/* Spec (downloaders): An address for another protocol */
static void
test_address_and_protocol(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) file = g_bytes_new_static("x", 1);
	AxisDownloadRequest request;
	gint64 id = 0;

	switch_on(fx);
	memset(&request, 0, sizeof(request));
	request.release_name = "A Made Up Release";
	request.protocol = AXIS_DOWNLOAD_USENET;
	request.address = WATCH "okvideo0001";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	request.protocol = AXIS_DOWNLOAD_YOUTUBE;
	request.file = file;
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	request.file = NULL;
	request.address = "http://www.youtube.com/watch?v=okvideo0001";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download"), ==, 0);

	/* An https address the kind does not take, and a video that was never
	 * looked up: recorded as a download that could not be sent, never
	 * fetched. */
	request.address = "https://video.example/watch?v=okvideo0001";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_INVALID);
	g_clear_error(&error);
	request.address = WATCH "okvideo0001";
	g_assert_false(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_error(error, AXIS_DOWNLOAD_ERROR, AXIS_DOWNLOAD_ERROR_REFUSED);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM download WHERE state <> 'failed'"), ==, 0);
	g_assert_cmpint(axis_test_library_n(fx->t, "SELECT count(*) FROM job WHERE kind = 'youtube.fetch'"), ==, 0);
	g_assert_cmpuint(standin_runs(fx), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web-youtube/" path, Fixture, NULL, func)
	ADD("off-until-switched-on", test_off_until_switched_on);
	ADD("administrators-only", test_administrators_only);
	ADD("addresses-refused", test_addresses_refused_on_the_page);
	ADD("download-to-completion", test_download_to_completion);
	ADD("progress-and-stop", test_progress_and_stop);
	ADD("failure", test_failure);
	ADD("hang", test_hang);
	ADD("flood", test_flood);
	ADD("existing-file-is-kept", test_existing_file_is_kept);
	ADD("read-only-storage", test_read_only_storage);
	ADD("free-space-floor", test_free_space_floor);
	ADD("killed-daemon", test_killed_daemon);
	ADD("killed-then-asked-again", test_killed_then_asked_again);
	ADD("stopping", test_stopping);
	ADD("switch-off-stops", test_switch_off_stops);
	ADD("one-file-video", test_one_file_video);
	ADD("finished-seen-later", test_finished_seen_later);
	ADD("viewer-starts-nothing", test_viewer_starts_nothing);
	ADD("home-kept-empty", test_home_kept_empty);
	ADD("refusal-pauses", test_refusal_pauses);
	ADD("not-fetched", test_not_fetched);
	ADD("outbound-binding", test_outbound_binding_fails_closed);
	ADD("address-and-protocol", test_address_and_protocol);
#undef ADD
	return g_test_run();
}
