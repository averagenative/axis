/*
 * test-web-library.c - a root's page: its last scan, what it found (names
 * shown escaped), the index's counts, and the administrators' actions
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell, storage and library modules on an ephemeral loopback
 * port, with sessions opened in an auth without a database, as in
 * test-web-storage.c.  Scans are run directly; "Scan now" is checked by
 * the job it enqueues.
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
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "library/axis-library.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-library.h"
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
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
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
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(),
		fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(),
		fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_library_get_module(),
		fx->library_ui, &error));
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
	axis_web_library_stop(fx->library_ui);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_library_free(fx->library_ui);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
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

static gchar *
csrf_for(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply page = get_as(fx, cookie, "/storage");
	gchar *token = axis_test_web_csrf_token(page.body);

	g_assert_nonnull(token);
	axis_test_reply_clear(&page);
	return token;
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	g_autofree gchar *csrf = csrf_for(fx, cookie);
	const gchar *extra[3];

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

static gchar *
root_path(Fixture *fx)
{
	return g_strdup_printf("/storage/roots/%" G_GINT64_FORMAT, fx->root);
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

/* ── Tests ──────────────────────────────────────────────────────── */

/* Spec: Hostile-looking name (displayed escaped); task 3.1 */
static void
test_root_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = root_path(fx);
	g_autofree gchar *locked = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_write_named(fx->films, "Film (2001).mkv", "frames");
	axis_test_write_named(fx->films, "<script>alert(1)<script>.mkv", "frames");
	axis_test_write_named(fx->films, "poster.jpg", "jpg");
	axis_test_write_named(fx->films, "caf\xe9 <&>.mkv", "Latin-1");
	/* Partial downloads are listed by name: markup, and bytes that are
	 * not UTF-8, and a line break. */
	axis_test_write_named(fx->films, "<b>bold<b> \"q\".mkv.part", "partial");
	axis_test_write_named(fx->films, "odd\xff\n<i>.part", "partial");
	axis_test_write_named(fx->films, "\xe2\x80\xae" "vkm.part", "partial");
	if (geteuid() != 0) {
		locked = g_build_filename(fx->films, "Locked & \"quoted\"", NULL);
		axis_test_storage_mkdir(locked);
		g_assert_cmpint(chmod(locked, 0), ==, 0);
	}
	scan_now(fx);

	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>Films</h1>");
	assert_contains(page.body, "Complete");
	assert_contains(page.body, "Last scan");
	assert_contains(page.body, "Video");
	assert_contains(page.body, "Images");
	assert_contains(page.body, "Partial downloads");
	assert_contains(page.body, "&lt;b&gt;bold&lt;b&gt; &quot;q&quot;.mkv.part");
	assert_lacks(page.body, "<b>bold");
	assert_lacks(page.body, "<i>");
	assert_lacks(page.body, "\xe2\x80\xae");
	assert_contains(page.body, "\\u{202E}vkm.part");
	assert_contains(page.body, "odd\xef\xbf\xbd\\n&lt;i&gt;.part");
	assert_contains(page.body, "not valid UTF-8");
	assert_contains(page.body, "<bdi>");
	/* An indexed name that is not UTF-8: as shown, and its exact bytes. */
	assert_contains(page.body, "File names with characters that cannot be shown");
	assert_contains(page.body, "<bdi>caf\xef\xbf\xbd &lt;&amp;&gt;.mkv</bdi>");
	assert_contains(page.body, "<bdi>caf\\xE9 &lt;&amp;&gt;.mkv</bdi>");
	if (locked != NULL) {
		assert_contains(page.body, "Could not be read");
		assert_contains(page.body, "Locked &amp; &quot;quoted&quot;");
	}
	/* Present, as of the scan. */
	assert_contains(page.body, "Present");
	/* No actions for someone who may not take them; no inline code. */
	assert_lacks(page.body, "Scan this library folder now");
	assert_lacks(page.body, "style=\"");
	assert_lacks(page.body, "<script>");
	assert_lacks(page.body, "onclick");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Scan this library folder now");
	assert_contains(page.body, "Names to leave out");
	axis_test_reply_clear(&page);

	/* The list of roots links to the page. */
	page = get_as(fx, fx->viewer, "/storage/roots");
	assert_contains(page.body, path);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->viewer, "/storage/roots/999999");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);

	if (locked != NULL)
		g_assert_cmpint(chmod(locked, 0755), ==, 0);
}

static void
test_never_scanned_and_offline(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = root_path(fx);
	g_autofree gchar *away = g_build_filename(fx->dir, "away", NULL);
	g_autoptr(GError) error = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "has not been scanned yet");
	axis_test_reply_clear(&page);

	/* The disk goes away: the page says so, and the scan reports it. */
	g_assert_cmpint(g_rename(fx->disk, away), ==, 0);
	axis_test_storage_mkdir(fx->disk);
	g_assert_true(axis_storage_check_volume(fx->storage, fx->volume, NULL, &error));
	{
		g_autoptr(AxisScanResult) result = NULL;

		g_assert_true(axis_library_scan_root(fx->library, fx->root, NULL, &result, &error));
		g_assert_cmpint(result->status, ==, AXIS_SCAN_UNAVAILABLE);
	}
	page = get_as(fx, fx->viewer, path);
	assert_contains(page.body, "is not online");
	assert_contains(page.body, "Disk offline");
	assert_contains(page.body, "Nothing was concluded");
	axis_test_reply_clear(&page);

	g_assert_cmpint(g_rmdir(fx->disk), ==, 0);
	g_assert_cmpint(g_rename(away, fx->disk), ==, 0);
}

/* "Scan now" makes a job, for administrators only. */
static void
test_scan_now(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = root_path(fx);
	g_autofree gchar *action = g_strconcat(path, "/scan", NULL);
	g_autofree gchar *location = g_strconcat(path, "?done=scan-requested", NULL);
	g_autofree gchar *placed = g_strconcat(location, "&at=root-actions#root-actions", NULL);
	g_autofree gchar *dedupe = g_strdup_printf("library.scan:volume:%" G_GINT64_FORMAT, fx->volume);
	g_autofree gchar *jobs = NULL;
	g_autofree gchar *none = NULL;
	g_autofree gchar *expected = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = post_as(fx, fx->viewer, action, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	none = axis_test_db_query(fx->db, "SELECT count(*) FROM job");
	g_assert_cmpstr(none, ==, "0");

	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, placed);
	axis_test_reply_clear(&reply);
	/* Twice is one job. */
	reply = post_as(fx, fx->admin, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	jobs = axis_test_db_queryf(fx->db, "SELECT count(*) || ':' || min(dedupe_key) || ':' || min(priority)"
		" FROM job WHERE kind = 'library.scan' AND payload->>'volume_id' = '%" G_GINT64_FORMAT "'",
		fx->volume);
	expected = g_strdup_printf("1:%s:10", dedupe);
	g_assert_cmpstr(jobs, ==, expected);
	g_assert_true(axis_test_wait_query(fx->db, "SELECT count(*) FROM library_root_state"
		" WHERE scan_requested_at IS NOT NULL", "1", AXIS_TEST_WAIT_MS));

	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "waiting for its turn");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, location);
	assert_contains(reply.body, "A scan has been requested");
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/storage/roots/999999/scan", "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

static void
test_settings(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = root_path(fx);
	g_autofree gchar *action = g_strconcat(path, "/settings", NULL);
	g_autofree gchar *stored = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = post_as(fx, fx->viewer, action, "interval_hours=12&patterns=x");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, action, "interval_hours=12&patterns=*.nfo%0D%0ASample%0A%0A++%0A*.nfo");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	stored = axis_test_db_queryf(fx->db, "SELECT scan_interval_seconds || ':' || array_to_string(ignore_patterns,"
		" '|') FROM library_root_state WHERE root_id = %" G_GINT64_FORMAT, fx->root);
	g_assert_cmpstr(stored, ==, "43200:*.nfo|Sample");

	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Every 12 hours");
	assert_contains(reply.body, "*.nfo  Sample");
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, action, "interval_hours=0&patterns=");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "interval_hours=1000&patterns=");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, action, "interval_hours=6&patterns=a%2Fb");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "cannot contain");
	axis_test_reply_clear(&reply);
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
	g_autofree gchar *path = root_path(fx);
	g_autofree gchar *scan = g_strconcat(path, "/scan", NULL);
	g_autofree gchar *settings = g_strconcat(path, "/settings", NULL);
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = post_as(fx, fx->admin, scan, "");
	assert_back_to_place(fx, &reply, "root-actions");
	reply = post_as(fx, fx->admin, settings, "interval_hours=12&patterns=Sample");
	assert_back_to_place(fx, &reply, "root-actions");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD_TEST(path, func) \
	g_test_add(path, Fixture, NULL, fixture_setup, func, fixture_teardown)

	ADD_TEST("/web/library/root-page", test_root_page);
	ADD_TEST("/web/library/never-scanned-and-offline", test_never_scanned_and_offline);
	ADD_TEST("/web/library/scan-now", test_scan_now);
	ADD_TEST("/web/library/settings", test_settings);
	ADD_TEST("/web/library/back-to-place", test_back_to_place);

	return g_test_run();
}
