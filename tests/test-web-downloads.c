/*
 * test-web-downloads.c - the Download clients pages, the Downloads page,
 * its live progress and the dashboard's "Coming in", with the real shell on
 * an ephemeral loopback port, the fixture kind and its fake client, a real
 * database and volume; and the tracking job on the real job queue
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios: (downloaders) Administrators only; Test; Live progress;
 * Client secrets stay with their client (in no page, log line or job row);
 * (web-shell) No client; Active download.  Task 5.7.
 *
 * With AXIS_TEST_DOWNLOAD_PAGES set to a directory, the pages are also
 * saved there, for looking at in a browser.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-qbittorrent.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "download/axis-download-private.h"
#include "download/axis-download-qbittorrent.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-settings.h"

#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD
#define MAGNET "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=Some.Film.2020"

typedef struct
{
	AxisTestLibrary      *t;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads;
	AxisWebDownloads     *ui;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisWebHub           *hub;
	AxisModuleRegistry   *registry;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;
	gchar                *viewer;
	gchar                *disk;
	gint64                volume_id;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisDownloadTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(8);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->fake = axis_test_download_fake_new(PASSWORD);
	fx->settings = axis_settings_new();
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_download_qbittorrent_kind(), &error));
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(fx->downloads, &tuning);
	fx->hub = axis_web_hub_new(NULL);
	fx->ui = axis_web_downloads_new(fx->downloads, fx->t->storage, fx->hub);
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
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
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_stop(fx->web);
	axis_web_downloads_stop(fx->ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_hub_free(fx->hub);
	axis_web_free(fx->web);
	axis_web_downloads_free(fx->ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_downloads_unref(fx->downloads);
	axis_settings_unref(fx->settings);
	axis_test_download_fake_free(fx->fake);
	g_free(fx->disk);
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
	AxisTestReply reply;

	axis_test_reply_clear(&opened);
	reply = axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
	return reply;
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
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	assert_lacks(body, "javascript:");
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_DOWNLOAD_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gint64
add_client(
	Fixture     *fx,
	const gchar *name,
	const gchar *address,
	const gchar *secret
){
	AxisDownloadClientSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = "fixture";
	spec.address = address;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	spec.enabled = TRUE;
	spec.secret = secret;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
add_magnet(
	Fixture     *fx,
	gint64       client,
	const gchar *magnet,
	const gchar *name,
	const gchar *title
){
	AxisDownloadRequest request;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&request, 0, sizeof(request));
	request.client_id = client;
	request.protocol = AXIS_DOWNLOAD_TORRENT;
	request.magnet = magnet;
	request.release_name = name;
	request.title_label = title;
	request.module = "movies";
	g_assert_true(axis_downloads_add(fx->downloads, &request, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
poll(
	Fixture *fx,
	gint64   client
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, client, NULL, NULL, &error));
	g_assert_no_error(error);
}

static gchar *
ref_of(
	Fixture *fx,
	gint64   id
){
	g_autofree gchar *sql = g_strdup_printf("SELECT client_ref FROM download WHERE id = %" G_GINT64_FORMAT, id);

	return axis_test_db_query(fx->t->db, sql);
}

/* A client with a download in progress at 40 %. */
static gint64
downloading(
	Fixture  *fx,
	gint64   *client_out
){
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 client = add_client(fx, "Torrents", base, PASSWORD);
	gint64 id = add_magnet(fx, client, MAGNET, "Some.Film.2020.1080p.BluRay.x264", "Some Film (2020)");
	g_autofree gchar *ref = ref_of(fx, id);

	axis_test_download_fake_set(fx->fake, ref, "downloading", 1600000000, NULL, NULL);
	poll(fx, client);
	if (client_out != NULL)
		*client_out = client;
	return id;
}

/* ── Download clients ───────────────────────────────────────────── */

/* Spec: Administrators only */
static void
test_clients_page_is_for_administrators(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 client = add_client(fx, "Torrents", base, PASSWORD);
	g_autofree gchar *one = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client);
	AxisTestReply viewer = get_as(fx, fx->viewer, "/downloads/clients");
	AxisTestReply viewer_one = get_as(fx, fx->viewer, one);
	AxisTestReply admin = get_as(fx, fx->admin, "/downloads/clients");
	AxisTestReply home = get_as(fx, fx->viewer, "/downloads");
	AxisTestReply posted = axis_test_web_request(fx->session, fx->port, "POST", "/downloads/clients", fx->viewer,
		NULL, "name=x");

	g_assert_cmpuint(viewer.status, ==, 403);
	g_assert_cmpuint(viewer_one.status, ==, 403);
	g_assert_cmpuint(posted.status, ==, 403);
	g_assert_cmpuint(admin.status, ==, 200);
	g_assert_cmpuint(home.status, ==, 200);
	/* In System for administrators; Downloads in Activity for everyone. */
	assert_contains(admin.body, "href=\"/downloads/clients\"");
	assert_lacks(home.body, "href=\"/downloads/clients\"");
	assert_contains(home.body, "href=\"/downloads\"");
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&viewer_one);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&home);
	axis_test_reply_clear(&posted);
}

static void
test_clients_page_empty(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/downloads/clients");
	AxisTestReply home = get_as(fx, fx->admin, "/");
	AxisTestReply downloads = get_as(fx, fx->admin, "/downloads");
	AxisTestReply viewer_downloads = get_as(fx, fx->viewer, "/downloads");

	/* Spec (web-shell, tidy-labels): An empty state says what to do. */
	g_assert_cmpuint(downloads.status, ==, 200);
	assert_contains(downloads.body, "No download client is set up yet.");
	assert_contains(downloads.body, "href=\"/downloads/clients\">Add a download client</a>");
	assert_contains(viewer_downloads.body, "Ask an administrator to add one.");
	assert_lacks(viewer_downloads.body, "Add a download client");
	axis_test_reply_clear(&downloads);
	axis_test_reply_clear(&viewer_downloads);

	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "No download client has been added yet.");
	assert_contains(page.body, "Add a download client");
	assert_contains(page.body, "<option value=\"fixture\">Fixture</option>");
	assert_contains(page.body, "type=\"password\"");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 0);
	save_page("clients-empty.html", page.body);
	/* Spec (web-shell): No client - no downloads section. */
	g_assert_cmpuint(home.status, ==, 200);
	assert_lacks(home.body, "dash-coming-in");
	assert_lacks(home.body, "Coming into the library");
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&home);
}

static void
test_add_edit_and_test_a_client(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("name=Torrents&kind=fixture&address=%s&secret=%s&category=axis"
		"&priority=high&enabled=yes", escaped, PASSWORD);
	g_autofree gchar *bad = g_strdup_printf("name=Second&kind=fixture&address=ftp%%3A%%2F%%2Fnot-an-origin"
		"&secret=second-secret-value-91&category=axis&priority=normal&enabled=yes");
	AxisTestReply added = post_as(fx, fx->admin, "/downloads/clients", "/downloads/clients", form);
	AxisTestReply refused = post_as(fx, fx->admin, "/downloads/clients", "/downloads/clients", bad);
	AxisTestReply page;
	AxisTestReply saved;
	AxisTestReply tested;
	AxisTestReply list;
	g_autofree gchar *count = NULL;

	g_assert_cmpuint(added.status, ==, 303);
	assert_contains(axis_test_reply_header(&added, "Location"), "/downloads/clients/1?done=added");
	count = axis_test_db_query(fx->t->db, "SELECT count(*) FROM download_client_secret WHERE secret = '" PASSWORD "'");
	g_assert_cmpstr(count, ==, "1");

	/* Refused: the form again, with what was typed but never the secret,
	 * and the reason naming the field. */
	g_assert_cmpuint(refused.status, ==, 400);
	assert_contains(refused.body, "The address must start with http:// or https://");
	assert_contains(refused.body, "value=\"Second\"");
	assert_lacks(refused.body, "second-secret-value-91");
	assert_csp_clean(refused.body);

	page = get_as(fx, fx->admin, "/downloads/clients/1?done=added");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "The download client has been added.");
	/* Spec (web-shell, tidy-labels): A button names what it acts on; a card names its contents. */
	assert_contains(page.body, "Test this download client now");
	assert_contains(page.body, "Remove this download client");
	assert_contains(page.body, ">State of this download client<");
	assert_contains(page.body, ">Settings for this download client<");
	assert_lacks(page.body, "<h2>State</h2>");
	assert_lacks(page.body, "<h2>Settings</h2>");
	assert_contains(page.body, "A value is stored.");
	assert_contains(page.body, "Not checked yet");
	assert_contains(page.body, "plain http");
	assert_lacks(page.body, PASSWORD);
	assert_csp_clean(page.body);
	/* The secret's field never has a value. */
	{
		g_autoptr(GRegex) secret_value = g_regex_new("<input[^>]*type=\"password\"[^>]*value=", 0, 0, NULL);

		g_assert_false(g_regex_match(secret_value, page.body, 0, NULL));
	}

	/* An edit with the secret left empty keeps it. */
	saved = post_as(fx, fx->admin, "/downloads/clients/1", "/downloads/clients/1",
		"name=Torrents+box&address=http%3A%2F%2F127.0.0.1%3A1&secret=&category=axis-films&priority=low&enabled=yes");
	g_assert_cmpuint(saved.status, ==, 303);
	g_clear_pointer(&count, g_free);
	count = axis_test_db_query(fx->t->db, "SELECT count(*) FROM download_client_secret WHERE secret = '" PASSWORD "'");
	g_assert_cmpstr(count, ==, "1");
	axis_test_reply_clear(&saved);
	saved = post_as(fx, fx->admin, "/downloads/clients/1", "/downloads/clients/1", form);
	g_assert_cmpuint(saved.status, ==, 303);

	/* Spec: Test */
	tested = post_as(fx, fx->admin, "/downloads/clients/1", "/downloads/clients/1/test", NULL);
	g_assert_cmpuint(tested.status, ==, 303);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/downloads/clients/1?done=tested");
	assert_contains(page.body, "Working");
	assert_contains(page.body, "Fixture 1.0");
	assert_lacks(page.body, PASSWORD);
	save_page("client-edit.html", page.body);

	list = get_as(fx, fx->admin, "/downloads/clients");
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	assert_contains(list.body, "kind-download-clients");
	assert_contains(list.body, "Not encrypted");
	assert_lacks(list.body, PASSWORD);

	/* The base path (spec: A client's address may carry a base path): the
	 * form has the field; an unsafe one is refused with the reason, keeping
	 * what was typed; a good one is stored without its trailing slash and
	 * shown after the address. */
	{
		g_autofree gchar *unsafe = g_strdup_printf("name=Third&kind=fixture&address=%s&base_path=%%2Fa%%2F..%%2Fb"
			"&secret=third-secret-value-42&category=axis&priority=normal&enabled=yes", escaped);
		g_autofree gchar *good = g_strdup_printf("name=Under+a+path&kind=fixture&address=%s&base_path=%%2Fsab%%2F"
			"&secret=%s&category=axis&priority=normal&enabled=yes", escaped, PASSWORD);
		AxisTestReply no_way = post_as(fx, fx->admin, "/downloads/clients", "/downloads/clients", unsafe);
		AxisTestReply under = post_as(fx, fx->admin, "/downloads/clients", "/downloads/clients", good);
		AxisTestReply form_page;
		AxisTestReply under_list;

		g_assert_cmpuint(no_way.status, ==, 400);
		assert_contains(no_way.body, "The base path");
		assert_contains(no_way.body, "Base path (optional)");
		assert_contains(no_way.body, "value=\"Third\"");
		assert_lacks(no_way.body, "third-secret-value-42");
		assert_csp_clean(no_way.body);
		g_assert_cmpuint(under.status, ==, 303);
		g_clear_pointer(&count, g_free);
		count = axis_test_db_query(fx->t->db, "SELECT base_path FROM download_client WHERE name = 'Under a path'");
		g_assert_cmpstr(count, ==, "/sab");
		g_clear_pointer(&count, g_free);
		count = axis_test_db_query(fx->t->db, "SELECT count(*) FROM download_client");
		g_assert_cmpstr(count, ==, "2");
		form_page = get_as(fx, fx->admin, "/downloads/clients/2");
		g_assert_cmpuint(form_page.status, ==, 200);
		assert_contains(form_page.body, "Base path (optional)");
		assert_contains(form_page.body, "name=\"base_path\"");
		assert_contains(form_page.body, "value=\"/sab\"");
		assert_csp_clean(form_page.body);
		save_page("client-base-path.html", form_page.body);
		under_list = get_as(fx, fx->admin, "/downloads/clients");
		assert_contains(under_list.body, "/sab</code>");
		assert_lacks(under_list.body, PASSWORD);
		axis_test_reply_clear(&no_way);
		axis_test_reply_clear(&under);
		axis_test_reply_clear(&form_page);
		axis_test_reply_clear(&under_list);
	}

	axis_test_reply_clear(&added);
	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&saved);
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&list);
}

/* Spec (qBittorrent): a client may have a user name; the pages offer the kind, show the
 * user name (never the password), and say which kinds use one */
static void
test_qbittorrent_client_on_the_pages(Fixture *fx, gconstpointer data)
{
	AxisTestQbit *qb = axis_test_qbit_new(AXIS_TEST_QBIT_V5, "axisuser", PASSWORD);
	g_autofree gchar *base = axis_test_qbit_base(qb);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *fixture_base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *form = g_strdup_printf("name=qBittorrent&kind=qbittorrent&address=%s&username=axisuser&secret=%s"
		"&category=axis&priority=normal&enabled=yes", escaped, PASSWORD);
	g_autofree gchar *bad = g_strdup_printf("name=Bad+user&kind=qbittorrent&address=%s&username=bad%%0Auser&secret=%s"
		"&category=axis&priority=normal&enabled=yes", escaped, PASSWORD);
	g_autofree gchar *name = NULL;
	AxisTestReply empty = get_as(fx, fx->admin, "/downloads/clients");
	AxisTestReply refused;
	AxisTestReply added;
	AxisTestReply tested;
	AxisTestReply page;
	AxisTestReply list;
	AxisTestReply other;
	gint64 plain;

	/* The form that adds a client offers the kind, and the user name with the kinds that use it. */
	g_assert_cmpuint(empty.status, ==, 200);
	assert_contains(empty.body, "<option value=\"qbittorrent\">qBittorrent</option>");
	assert_contains(empty.body, "name=\"username\"");
	assert_contains(empty.body, "User name (qBittorrent only)");
	assert_contains(empty.body, "The kinds that log in with a user name: qBittorrent.");
	assert_contains(empty.body, "Deluge or qBittorrent for torrents");
	assert_csp_clean(empty.body);
	save_page("qbittorrent-add.html", empty.body);

	refused = post_as(fx, fx->admin, "/downloads/clients", "/downloads/clients", bad);
	g_assert_cmpuint(refused.status, ==, 400);
	assert_contains(refused.body, "The user name is at most 128 characters");
	assert_contains(refused.body, "value=\"Bad user\"");
	assert_lacks(refused.body, PASSWORD);
	assert_csp_clean(refused.body);

	added = post_as(fx, fx->admin, "/downloads/clients", "/downloads/clients", form);
	g_assert_cmpuint(added.status, ==, 303);
	name = axis_test_db_query(fx->t->db, "SELECT username FROM download_client WHERE name = 'qBittorrent'");
	g_assert_cmpstr(name, ==, "axisuser");
	tested = post_as(fx, fx->admin, "/downloads/clients/1", "/downloads/clients/1/test", NULL);
	g_assert_cmpuint(tested.status, ==, 303);

	/* Its page: the user name shown, the password not, the field filled in, the version found. */
	page = get_as(fx, fx->admin, "/downloads/clients/1?done=tested");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "User name");
	assert_contains(page.body, "axisuser");
	assert_contains(page.body, "name=\"username\"");
	assert_contains(page.body, "value=\"axisuser\"");
	assert_contains(page.body, "v5.0.1 (Web API 2.11.2)");
	assert_contains(page.body, "Working");
	assert_lacks(page.body, PASSWORD);
	assert_csp_clean(page.body);
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 0);
	save_page("qbittorrent-client.html", page.body);

	list = get_as(fx, fx->admin, "/downloads/clients");
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	assert_contains(list.body, "qBittorrent");
	assert_lacks(list.body, PASSWORD);
	save_page("qbittorrent-list.html", list.body);

	/* A client of a kind without a user name has no such field, and its edit leaves the stored one alone. */
	plain = add_client(fx, "Fixture box", fixture_base, PASSWORD);
	g_assert_cmpint(plain, ==, 2);
	other = get_as(fx, fx->admin, "/downloads/clients/2");
	g_assert_cmpuint(other.status, ==, 200);
	assert_lacks(other.body, "name=\"username\"");
	assert_lacks(other.body, "User name");
	axis_test_reply_clear(&other);

	axis_test_reply_clear(&empty);
	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&added);
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&list);
	axis_test_qbit_free(qb);
}

/* Spec: Test (a wrong secret) - on the page */
static void
test_wrong_secret_on_the_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 client = add_client(fx, "Wrong", base, "not-the-password-0d8f");
	g_autofree gchar *path = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client);
	g_autofree gchar *test = g_strdup_printf("%s/test", path);
	AxisTestReply tested = post_as(fx, fx->admin, path, test, NULL);
	AxisTestReply page = get_as(fx, fx->admin, path);

	g_assert_cmpuint(tested.status, ==, 303);
	assert_contains(page.body, "Access refused");
	assert_contains(page.body, "The API key or password was refused");
	assert_lacks(page.body, "not-the-password-0d8f");
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&page);
}

/* The pages of the screenshots: clients working, unreachable, with an
 * unmapped path; and the mapping form. */
static void
test_clients_with_problems(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	AxisTestPlex *closed = axis_test_plex_new();
	g_autofree gchar *closed_base = axis_test_plex_base(closed);
	gint64 good = add_client(fx, "Torrents", base, PASSWORD);
	gint64 down;
	gint64 id;
	g_autofree gchar *ref = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply list;
	AxisTestReply one;
	g_autofree gchar *path = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, good);

	axis_test_plex_free(closed);
	down = add_client(fx, "Old box", closed_base, PASSWORD);
	g_assert_true(axis_downloads_test_client(fx->downloads, good, NULL, &error));
	g_assert_true(axis_downloads_test_client(fx->downloads, down, NULL, &error));
	g_assert_true(axis_downloads_add_path_map(fx->downloads, good, "/downloads/complete", fx->volume_id,
		"downloads/complete", &error));
	id = add_magnet(fx, good, MAGNET, "Some.Film.2020.1080p.BluRay.x264", "Some Film (2020)");
	ref = ref_of(fx, id);
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/incomplete/Some.Film.2020", NULL);
	poll(fx, good);

	list = get_as(fx, fx->admin, "/downloads/clients");
	g_assert_cmpuint(list.status, ==, 200);
	assert_contains(list.body, "Not answering");
	assert_contains(list.body, "Working");
	assert_contains(list.body, "1 finished download cannot be imported yet");
	assert_csp_clean(list.body);
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	save_page("clients.html", list.body);

	one = get_as(fx, fx->admin, path);
	assert_contains(one.body, "kind-download-maps");
	assert_contains(one.body, "/downloads/complete");
	assert_contains(one.body, "Disk one");
	assert_contains(one.body, "Add the mapping");
	g_assert_cmpuint(axis_test_web_assert_tables(one.body), ==, 1);
	assert_csp_clean(one.body);
	save_page("client-maps.html", one.body);
	axis_test_reply_clear(&list);
	axis_test_reply_clear(&one);
}

static void
test_path_mappings_from_the_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 client = add_client(fx, "Torrents", base, PASSWORD);
	g_autofree gchar *path = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client);
	g_autofree gchar *maps = g_strdup_printf("%s/maps", path);
	g_autofree gchar *remove = g_strdup_printf("%s/maps/remove", path);
	g_autofree gchar *form = g_strdup_printf("prefix=%%2Fdata%%2Fdownloads&volume=%" G_GINT64_FORMAT
		"&rel_path=downloads", fx->volume_id);
	AxisTestReply added = post_as(fx, fx->admin, path, maps, form);
	AxisTestReply dotdot = post_as(fx, fx->admin, path, maps, "prefix=%2Fdata%2F..%2Fx&volume=1&rel_path=");
	AxisTestReply removed;
	g_autofree gchar *n = NULL;

	g_assert_cmpuint(added.status, ==, 303);
	n = axis_test_db_query(fx->t->db, "SELECT client_prefix || ' ' || rel_path FROM download_path_map");
	g_assert_cmpstr(n, ==, "/data/downloads downloads");
	g_assert_cmpuint(dotdot.status, ==, 400);
	assert_contains(dotdot.body, "The mapping cannot be used");
	removed = post_as(fx, fx->admin, path, remove, "prefix=%2Fdata%2Fdownloads");
	g_assert_cmpuint(removed.status, ==, 303);
	g_clear_pointer(&n, g_free);
	n = axis_test_db_query(fx->t->db, "SELECT count(*) FROM download_path_map");
	g_assert_cmpstr(n, ==, "0");
	axis_test_reply_clear(&added);
	axis_test_reply_clear(&dotdot);
	axis_test_reply_clear(&removed);
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
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	gint64 client = add_client(fx, "Torrents", base, PASSWORD);
	g_autofree gchar *path = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client);
	g_autofree gchar *test = g_strdup_printf("%s/test", path);
	g_autofree gchar *maps = g_strdup_printf("%s/maps", path);
	g_autofree gchar *remove = g_strdup_printf("%s/maps/remove", path);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *save = g_strdup_printf("name=Torrents+box&address=%s&secret=&category=axis&priority=low"
		"&enabled=yes", escaped);
	g_autofree gchar *map = g_strdup_printf("prefix=%%2Fdata%%2Fdownloads&volume=%" G_GINT64_FORMAT
		"&rel_path=downloads", fx->volume_id);
	AxisTestReply reply;

	/* Save: the Settings card; test: the State card. */
	reply = post_as(fx, fx->admin, path, path, save);
	assert_back_to_place(fx, &reply, "client-settings");
	reply = post_as(fx, fx->admin, path, test, NULL);
	assert_back_to_place(fx, &reply, "client-state");

	/* Add and remove a path mapping: the mappings card. */
	reply = post_as(fx, fx->admin, path, maps, map);
	assert_back_to_place(fx, &reply, "client-maps");
	reply = post_as(fx, fx->admin, path, remove, "prefix=%2Fdata%2Fdownloads");
	assert_back_to_place(fx, &reply, "client-maps");
}

/* ── Downloads ──────────────────────────────────────────────────── */

static void
test_downloads_page(Fixture *fx, gconstpointer data)
{
	gint64 client = 0;
	gint64 active = downloading(fx, &client);
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "Another.Film.2019", NULL);
	gint64 done;
	gint64 failed;
	g_autofree gchar *ref = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply viewer;
	AxisTestReply admin;

	axis_test_storage_mkdir(place);
	g_assert_true(axis_downloads_add_path_map(fx->downloads, client, "/data", fx->volume_id, "", &error));
	done = add_magnet(fx, client, "magnet:?xt=urn:btih:1111111111111111111111111111111111111111",
		"Another.Film.2019.2160p", "Another Film (2019)");
	ref = ref_of(fx, done);
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/Another.Film.2019", NULL);
	failed = add_magnet(fx, client, "magnet:?xt=urn:btih:2222222222222222222222222222222222222222",
		"Third.Film.2018.720p", NULL);
	g_clear_pointer(&ref, g_free);
	ref = ref_of(fx, failed);
	axis_test_download_fake_set(fx->fake, ref, "failed", 0, NULL, "Tracker says: torrent not registered");
	poll(fx, client);
	(void)active;

	viewer = get_as(fx, fx->viewer, "/downloads");
	admin = get_as(fx, fx->admin, "/downloads");
	g_assert_cmpuint(viewer.status, ==, 200);
	g_assert_cmpuint(admin.status, ==, 200);
	assert_csp_clean(admin.body);
	g_assert_cmpuint(axis_test_web_assert_tables(admin.body), ==, 2);
	g_assert_cmpuint(axis_test_web_assert_tables(viewer.body), ==, 2);
	assert_contains(viewer.body, "Some Film (2020)");
	assert_contains(viewer.body, "Some.Film.2020.1080p.BluRay.x264");
	assert_contains(viewer.body, "40%");
	assert_contains(viewer.body, "Downloading");
	assert_contains(viewer.body, "id=\"download-1\"");
	assert_contains(viewer.body, "Another Film (2019)");
	assert_contains(viewer.body, "Disk one: ");
	assert_contains(viewer.body, "downloads/Another.Film.2019");
	assert_contains(viewer.body, "Tracker says: torrent not registered");
	assert_contains(viewer.body, "kind-downloads-active min-");
	assert_lacks(viewer.body, "kind-downloads-active-admin");
	assert_contains(admin.body, "kind-downloads-active-admin");
	assert_contains(admin.body, "/downloads/remove/1");
	assert_lacks(viewer.body, "/downloads/remove/");
	/* Live: each follows its own topic. */
	assert_contains(viewer.body, "topic=downloads\"");
	assert_contains(admin.body, "topic=downloads:admin");
	assert_lacks(admin.body, PASSWORD);
	save_page("downloads.html", admin.body);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&admin);
}

static gboolean
stream_has_progress(gpointer data)
{
	AxisTestStream *stream = data;

	return strstr(axis_test_stream_get_text(stream), "75%") != NULL;
}

/* Spec: Live progress */
static void
test_progress_is_live(Fixture *fx, gconstpointer data)
{
	gint64 client = 0;
	gint64 id = downloading(fx, &client);
	g_autofree gchar *ref = ref_of(fx, id);
	g_autoptr(AxisTestStream) viewer = axis_test_stream_open(fx->port, "/events?topic=downloads", fx->viewer, 0);
	g_autoptr(AxisTestStream) admin = axis_test_stream_open(fx->port, "/events?topic=downloads:admin", fx->admin, 0);
	g_autoptr(AxisTestStream) refused = axis_test_stream_open(fx->port, "/events?topic=downloads:admin",
		fx->viewer, 0);

	g_assert_true(axis_test_stream_wait_for(viewer, "\r\n\r\n", 5000));
	g_assert_true(axis_test_stream_wait_for(admin, "\r\n\r\n", 5000));
	axis_test_stream_wait_for(refused, "\r\n\r\n", 5000);
	g_assert_cmpuint(axis_test_stream_get_status(refused), ==, 403);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 3000000000, NULL, NULL);
	poll(fx, client);
	g_assert_true(axis_test_pump_until(stream_has_progress, viewer, AXIS_TEST_WAIT_MS));
	g_assert_true(axis_test_pump_until(stream_has_progress, admin, AXIS_TEST_WAIT_MS));
	g_assert_nonnull(strstr(axis_test_stream_get_text(viewer), "id=\"download-1\""));
	g_assert_nonnull(strstr(axis_test_stream_get_text(viewer), "hx-swap-oob"));
	g_assert_null(strstr(axis_test_stream_get_text(viewer), "/downloads/remove/"));
	g_assert_nonnull(strstr(axis_test_stream_get_text(admin), "/downloads/remove/1"));

	/* Completed: the lists are told to fetch themselves again. */
	{
		g_autofree gchar *place = g_build_filename(fx->disk, "dl", "Some.Film", NULL);
		g_autoptr(GError) error = NULL;

		axis_test_storage_mkdir(place);
		g_assert_true(axis_downloads_add_path_map(fx->downloads, client, "/data", fx->volume_id, "dl", &error));
		axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/Some.Film", NULL);
		poll(fx, client);
		g_assert_true(axis_test_stream_wait_for(viewer, AXIS_DOWNLOADS_EVENT_CHANGED, AXIS_TEST_WAIT_MS));
	}
}

static void
test_client_down_on_the_page(Fixture *fx, gconstpointer data)
{
	gint64 client = 0;
	AxisTestReply page;

	downloading(fx, &client);
	axis_test_db_exec(fx->t->db, "UPDATE download SET seen_at = now() - interval '7 minutes'");
	axis_test_db_exec(fx->t->db, "UPDATE download_client SET last_contact_at = now() - interval '7 minutes'");
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	poll(fx, client);
	page = get_as(fx, fx->admin, "/downloads");
	assert_contains(page.body, "last answered properly 7 minutes ago");
	assert_contains(page.body, "last seen 7 minutes ago");
	assert_contains(page.body, "Downloading");
	save_page("downloads-down.html", page.body);
	axis_test_reply_clear(&page);
}

/* Review of the fixes, item 7: a client whose list of Axis's downloads has not
 * been whole for long is said on the Downloads page too (only its own page
 * said it): what it does not show is kept as it was, so a download gone there
 * is not noticed meanwhile. */
static void
test_partial_for_long_on_the_page(Fixture *fx, gconstpointer data)
{
	AxisDownloadSlot *slot;
	gint64 client = 0;
	AxisTestReply page;

	downloading(fx, &client);
	/* Whole: nothing said. */
	poll(fx, client);
	page = get_as(fx, fx->admin, "/downloads");
	g_assert_null(strstr(page.body, "not been able to read the whole download list"));
	axis_test_reply_clear(&page);
	/* Not whole since two hours ago (as tracking records it). */
	axis_download_note_partial(fx->downloads, client, TRUE);
	slot = axis_download_slot_get(fx->downloads, client);
	g_mutex_lock(&fx->downloads->lock);
	slot->partial_since_us = g_get_real_time() - G_GINT64_CONSTANT(7200) * G_USEC_PER_SEC;
	g_mutex_unlock(&fx->downloads->lock);
	axis_download_slot_unref(slot);
	page = get_as(fx, fx->admin, "/downloads");
	assert_contains(page.body, "download list of Torrents for 2 hours");
	assert_contains(page.body, "Torrents");
	assert_csp_clean(page.body);
	axis_test_reply_clear(&page);
	/* Whole again: no longer said. */
	poll(fx, client);
	page = get_as(fx, fx->admin, "/downloads");
	g_assert_null(strstr(page.body, "not been able to read the whole download list"));
	axis_test_reply_clear(&page);
}

static void
test_remove_with_confirmation(Fixture *fx, gconstpointer data)
{
	gint64 id = downloading(fx, NULL);
	g_autofree gchar *ref = ref_of(fx, id);
	AxisTestReply confirm = get_as(fx, fx->admin, "/downloads/remove/1");
	AxisTestReply unconfirmed = post_as(fx, fx->admin, "/downloads/remove/1", "/downloads/remove/1", "delete_data=yes");
	AxisTestReply viewer = axis_test_web_request(fx->session, fx->port, "POST", "/downloads/remove/1", fx->viewer,
		NULL, "confirm=yes");
	AxisTestReply removed;
	g_autofree gchar *state = NULL;

	g_assert_cmpuint(confirm.status, ==, 200);
	assert_contains(confirm.body, "Also delete its files at the client");
	assert_contains(confirm.body, "Remove this download from Torrents");
	assert_csp_clean(confirm.body);
	save_page("download-remove.html", confirm.body);
	g_assert_cmpuint(unconfirmed.status, ==, 400);
	g_assert_cmpuint(viewer.status, ==, 403);
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	removed = post_as(fx, fx->admin, "/downloads/remove/1", "/downloads/remove/1", "confirm=yes&delete_data=yes");
	g_assert_cmpuint(removed.status, ==, 303);
	assert_contains(axis_test_reply_header(&removed, "Location"), "done=removed-data");
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	state = axis_test_db_query(fx->t->db, "SELECT state FROM download WHERE id = 1");
	g_assert_cmpstr(state, ==, "removed");
	axis_test_reply_clear(&confirm);
	axis_test_reply_clear(&unconfirmed);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&removed);
}

/* Spec (downloaders): a download its client no longer has, removed from the
 * page: recorded gone, and the page says that nothing was removed or deleted
 * there. */
static void
test_remove_a_download_already_gone(Fixture *fx, gconstpointer data)
{
	gint64 id = downloading(fx, NULL);
	g_autofree gchar *ref = ref_of(fx, id);
	g_autofree gchar *state = NULL;
	AxisTestReply removed;
	AxisTestReply page;

	axis_test_download_fake_drop(fx->fake, ref);
	axis_test_download_fake_say_gone(fx->fake, TRUE);
	removed = post_as(fx, fx->admin, "/downloads/remove/1", "/downloads/remove/1", "confirm=yes&delete_data=yes");
	g_assert_cmpuint(removed.status, ==, 303);
	assert_contains(axis_test_reply_header(&removed, "Location"), "done=gone");
	state = axis_test_db_query(fx->t->db, "SELECT state FROM download WHERE id = 1");
	g_assert_cmpstr(state, ==, "gone");
	page = get_as(fx, fx->admin, "/downloads?done=gone");
	assert_contains(page.body, "The client no longer had the download (something else removed it), so nothing was "
		"removed or deleted there; it is recorded as gone.");
	axis_test_reply_clear(&removed);
	axis_test_reply_clear(&page);
}

#define STATION "Quillmere.Station.2012.1080p.WEB-DL.x264"

/* A client ("Torrents") with a torrent of STATION in progress, which the
 * client says is at @client_path and which is on the disk in @folder.  The
 * client's /data/films and /data/downloads are the disk's films (a library
 * root) and downloads; nothing else of the client's is mapped.  *@file: a
 * file of the torrent, there. */
static gchar *
torrent_at(
	Fixture      *fx,
	const gchar  *folder,
	const gchar  *client_path,
	gchar       **file
){
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *where = g_build_filename(fx->disk, folder, NULL);
	g_autofree gchar *dir = g_build_filename(fx->disk, folder, STATION, NULL);
	g_autoptr(GError) error = NULL;
	gint64 client = add_client(fx, "Torrents", base, PASSWORD);
	gint64 id = add_magnet(fx, client, MAGNET, STATION, "Quillmere Station (2012)");
	gchar *ref = ref_of(fx, id);

	g_assert_cmpint(id, ==, 1);
	axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	g_assert_true(axis_downloads_add_path_map(fx->downloads, client, "/data/films", fx->volume_id, "films", &error));
	g_assert_true(axis_downloads_add_path_map(fx->downloads, client, "/data/downloads", fx->volume_id, "downloads",
		&error));
	g_assert_no_error(error);
	axis_test_storage_mkdir(dir);
	*file = g_build_filename(dir, "quillmere-station.mkv", NULL);
	axis_test_storage_write(*file, "a film");
	axis_test_download_fake_set_data_dir(fx->fake, where);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000, client_path, NULL);
	poll(fx, client);
	return ref;
}

/* Remove, for a torrent its client says is inside the films' library folder
 * (a person moved it there in the client): the confirmation says its files
 * are left and offers no deletion; the torrent is removed from the client
 * without its data; nothing in the library is deleted; the download and the
 * page say that its files were left and why.  With @data "forged": the form
 * asks for the files to be deleted all the same (a page opened before the
 * move, or a form made by hand), and the answer is the same. */
static void
test_remove_in_a_library_folder(Fixture *fx, gconstpointer data)
{
	gboolean forged = g_strcmp0(data, "forged") == 0;
	g_autofree gchar *file = NULL;
	g_autofree gchar *ref = torrent_at(fx, "films", "/data/films/" STATION, &file);
	AxisTestReply confirm = get_as(fx, fx->admin, "/downloads/remove/1");
	AxisTestReply removed;
	AxisTestReply page;
	g_autofree gchar *record = NULL;
	g_autofree gchar *text = NULL;

	g_assert_cmpuint(confirm.status, ==, 200);
	assert_contains(confirm.body, "Its files will be left where they are: its client says the torrent");
	assert_contains(confirm.body, "files are at /data/films/" STATION ", which is inside a library folder.");
	assert_contains(confirm.body, "does not ask the client to delete its files");
	assert_lacks(confirm.body, "Also delete its files at the client");
	assert_lacks(confirm.body, "deletes them at the client");
	assert_csp_clean(confirm.body);
	removed = post_as(fx, fx->admin, "/downloads/remove/1", "/downloads/remove/1", forged ?
		"confirm=yes&delete_data=yes" : "confirm=yes&leave=yes&delete_data=yes");
	g_assert_cmpuint(removed.status, ==, 303);
	assert_contains(axis_test_reply_header(&removed, "Location"), "done=removed-left");
	/* (First: the file in the library is still there.) */
	g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 0);
	record = axis_test_db_query(fx->t->db, "SELECT state || '|' || folder_removal FROM download WHERE id = 1");
	g_assert_cmpstr(record, ==, "removed|left");
	text = axis_test_db_query(fx->t->db, "SELECT folder_removal_text FROM download WHERE id = 1");
	g_assert_cmpstr(text, ==, "Removed from its client without its data. Its files were left where they are: its client "
		"says the torrent's files are at /data/films/" STATION ", which is inside a library folder. It was probably "
		"moved in the client.");
	page = get_as(fx, fx->admin, "/downloads?done=removed-left");
	assert_contains(page.body, "The download was removed from its download client without its files.");
	assert_contains(page.body, "Files left at the download client: Removed from its client without its data. Its files "
		"were left where they are: its client says the torrent");
	assert_contains(page.body, "files are at /data/films/" STATION ", which is inside a library folder.");
	assert_csp_clean(page.body);
	axis_test_reply_clear(&confirm);
	axis_test_reply_clear(&removed);
	axis_test_reply_clear(&page);
}

/* Remove, for a torrent in the client's own download folder, and for one at
 * a place no path mapping reaches (@data "unmapped"): the confirmation offers
 * the deletion, and the client is asked to remove it with its data, as it
 * always was. */
static void
test_remove_outside_the_library(Fixture *fx, gconstpointer data)
{
	gboolean unmapped = g_strcmp0(data, "unmapped") == 0;
	g_autofree gchar *file = NULL;
	g_autofree gchar *ref = torrent_at(fx, unmapped ? "elsewhere" : "downloads", unmapped ? "/elsewhere/" STATION :
		"/data/downloads/" STATION, &file);
	AxisTestReply confirm;
	AxisTestReply removed;
	g_autofree gchar *record = NULL;

	/* "registered": the downloads folder is a registered root of type
	 * downloads, on the volume of the films root: inside it is still the
	 * client's own place. */
	if (g_strcmp0(data, "registered") == 0)
		axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "downloads", AXIS_MEDIA_DOWNLOADS);
	confirm = get_as(fx, fx->admin, "/downloads/remove/1");

	g_assert_cmpuint(confirm.status, ==, 200);
	assert_contains(confirm.body, "Also delete its files at the client");
	assert_lacks(confirm.body, "will be left where they are");
	removed = post_as(fx, fx->admin, "/downloads/remove/1", "/downloads/remove/1", "confirm=yes&delete_data=yes");
	g_assert_cmpuint(removed.status, ==, 303);
	assert_contains(axis_test_reply_header(&removed, "Location"), "done=removed-data");
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove-data", NULL), ==, 1);
	g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS));
	record = axis_test_db_query(fx->t->db, "SELECT state || '|' || coalesce(folder_removal, 'none') FROM download"
		" WHERE id = 1");
	g_assert_cmpstr(record, ==, "removed|none");
	axis_test_reply_clear(&confirm);
	axis_test_reply_clear(&removed);
}

/* Spec (import): a download removed with its data outside an import: its line
 * says what became of the files its client may have kept. */
static void
test_removed_download_files_on_the_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	downloading(fx, NULL);
	axis_test_db_exec(fx->t->db, "UPDATE download SET state = 'removed', finished_at = now(),"
		" folder_removal = 'pending' WHERE id = 1");
	page = get_as(fx, fx->admin, "/downloads");
	assert_contains(page.body, "Files left at the download client: What the client kept of its files is looked at by the hourly "
		"cleanup, and deleted where the rules allow it.");
	axis_test_reply_clear(&page);
	axis_test_db_exec(fx->t->db, "UPDATE download SET folder_removal = 'left', folder_removal_text = 'Its folder was"
		" left where it is: it is at /usenet/complete/axis/Quillmere.Station.2012 at the client and can be deleted"
		" there by hand.' WHERE id = 1");
	page = get_as(fx, fx->admin, "/downloads");
	assert_contains(page.body, "Files left at the download client: Its folder was left where it is: it is at");
	assert_csp_clean(page.body);
	axis_test_reply_clear(&page);
}

/* Spec (downloaders): switched off, a client is sent no new downloads and
 * the ones it has are followed to the end, as its pages say. */
static void
test_switched_off_on_the_pages(Fixture *fx, gconstpointer data)
{
	gint64 client = 0;
	g_autofree gchar *path = NULL;
	AxisTestReply form;
	AxisTestReply list;

	downloading(fx, &client);
	path = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client);
	form = get_as(fx, fx->admin, path);
	g_assert_cmpuint(form.status, ==, 200);
	assert_contains(form.body, "Switched off, it is sent no new downloads; the downloads it has already are still "
		"followed to the end and imported.");
	axis_test_db_exec(fx->t->db, "UPDATE download_client SET enabled = false");
	list = get_as(fx, fx->admin, "/downloads/clients");
	g_assert_cmpuint(list.status, ==, 200);
	assert_contains(list.body, "Off: no new downloads");
	axis_test_reply_clear(&form);
	axis_test_reply_clear(&list);
}

/* ── The dashboard ──────────────────────────────────────────────── */

/* Spec (web-shell): Active download */
static void
test_dashboard_shows_what_is_coming_in(Fixture *fx, gconstpointer data)
{
	gint64 client = 0;
	g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "Another.Film.2019", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	AxisTestReply home;
	gint64 done;

	downloading(fx, &client);
	axis_test_storage_mkdir(place);
	g_assert_true(axis_downloads_add_path_map(fx->downloads, client, "/data", fx->volume_id, "", &error));
	done = add_magnet(fx, client, "magnet:?xt=urn:btih:1111111111111111111111111111111111111111",
		"Another.Film.2019.2160p", "Another Film (2019)");
	ref = ref_of(fx, done);
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/Another.Film.2019", NULL);
	poll(fx, client);
	home = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(home.status, ==, 200);
	assert_contains(home.body, "id=\"dash-coming-in\"");
	assert_contains(home.body, "Coming into the library");
	assert_contains(home.body, "href=\"/downloads\"");
	assert_contains(home.body, "Some Film (2020)");
	/* (g_format_size() puts a no-break space before the unit.) */
	assert_contains(home.body, "40% of 3.7\302\240GiB, Torrents");
	assert_contains(home.body, "Arrived lately");
	assert_contains(home.body, "Another Film (2019)");
	assert_contains(home.body, "space-bar");
	assert_csp_clean(home.body);
	save_page("dashboard.html", home.body);
	axis_test_reply_clear(&home);
}

/* ── The job, and secrets (5.5) ─────────────────────────────────── */

/* Runs the tracking job through a real queue (for @run_ms, or its own full
 * run for 0) and waits up to @wait_ms for it to succeed; *@job is its id. */
static gboolean
run_tracking_job(
	Fixture *fx,
	guint    run_ms,
	guint    wait_ms,
	gint64  *job
){
	const AxisModuleInfo *module = axis_web_downloads_get_module();
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(JsonObject) payload = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	gboolean ok;

	/* A send queues a tracking job of its own (full run); this test runs its own. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'download.track' AND state = 'queued'");
	g_assert_cmpstr(module->job_handlers[0].queue, ==, AXIS_DOWNLOAD_QUEUE);
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[0], fx->ui, &error));
	queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_poll_interval(queue, 50);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	if (run_ms > 0)
		json_object_set_int_member(payload, "run_ms", run_ms);
	g_assert_true(axis_job_queue_enqueue(queue, AXIS_DOWNLOAD_TRACK_KIND, payload, NULL, job, NULL, &error));
	g_assert_no_error(error);
	axis_job_queue_wake(queue);
	sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, *job);
	ok = axis_test_wait_query(fx->t->db, sql, "succeeded", wait_ms);
	g_assert_true(axis_job_queue_stop(queue, 5000));
	return ok;
}

static gboolean
run_job_and_wait(Fixture *fx)
{
	gint64 job = 0;

	return run_tracking_job(fx, 1200, AXIS_TEST_WAIT_MS, &job);
}

/* Spec (web-shell): Only work a person would watch counts as running.  With
 * no download client set up (or none switched on) the tracking job, which
 * otherwise follows the clients for most of a minute, ends at once: it
 * holds no place on the downloads queue and shows as running nowhere. */
static void
test_tracking_without_a_client_ends_at_once(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autoptr(GError) error = NULL;
	AxisDownloadClientSpec spec;
	g_autofree gchar *quick = NULL;
	g_autofree gchar *quick_off = NULL;
	gint64 job = 0;
	gint64 off;
	gint64 start;

	/* None at all: its whole run (55 s) is not waited for. */
	start = g_get_monotonic_time();
	g_assert_true(run_tracking_job(fx, 0, 5000, &job));
	g_test_message("with no client the run took %" G_GINT64_FORMAT " ms", (g_get_monotonic_time() - start) / 1000);
	quick = axis_test_db_queryf(fx->t->db, "SELECT finished_at - started_at < interval '2 seconds' FROM job "
		"WHERE id = %" G_GINT64_FORMAT, job);
	g_assert_cmpstr(quick, ==, "t");

	/* One, switched off: the same. */
	off = add_client(fx, "Off", base, PASSWORD);
	memset(&spec, 0, sizeof(spec));
	spec.name = "Off";
	spec.address = base;
	spec.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	spec.enabled = FALSE;
	g_assert_true(axis_downloads_save_client(fx->downloads, off, &spec, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(run_tracking_job(fx, 0, 5000, &job));
	quick_off = axis_test_db_queryf(fx->t->db, "SELECT finished_at - started_at < interval '2 seconds' FROM job "
		"WHERE id = %" G_GINT64_FORMAT, job);
	g_assert_cmpstr(quick_off, ==, "t");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "list", NULL), ==, 0);
}

/* A running queue with the tracking handler, as axisd has it. */
static AxisJobQueue *
track_queue_start(Fixture *fx)
{
	const AxisModuleInfo *module = axis_web_downloads_get_module();
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;
	AxisJobQueue *queue;

	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[0], fx->ui, &error));
	queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_poll_interval(queue, 50);
	g_assert_true(axis_job_queue_start(queue, &error));
	g_assert_no_error(error);
	return queue;
}

/* Sends a magnet to @client and has the fake client report it at 40 %.
 * Returns the download. */
static gint64
send_and_read_by_client(
	Fixture *fx,
	gint64   client
){
	gint64 id = add_magnet(fx, client, MAGNET, "Some.Film.2020.1080p.BluRay.x264", "Some Film (2020)");
	g_autofree gchar *ref = ref_of(fx, id);

	axis_test_download_fake_set(fx->fake, ref, "downloading", 1600000000, NULL, NULL);
	return id;
}

/* Spec (web-shell): a download sent just after the tracking job ended with
 * nothing to follow is read within a few seconds, not at the next scheduled
 * run (a minute on). */
static void
test_tracking_starts_when_a_download_is_sent(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	gint64 job = 0;
	gint64 client;
	gint64 id;

	queue = track_queue_start(fx);
	g_assert_true(axis_job_queue_enqueue(queue, AXIS_DOWNLOAD_TRACK_KIND, NULL, NULL, &job, NULL, &error));
	g_assert_no_error(error);
	axis_job_queue_wake(queue);
	sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, job);
	g_assert_true(axis_test_wait_query(fx->t->db, sql, "succeeded", 5000));

	/* Nothing was followed; now a client is set up and a download sent. */
	client = add_client(fx, "Torrents", base, PASSWORD);
	id = send_and_read_by_client(fx, client);
	g_free(sql);
	sql = g_strdup_printf("SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(axis_test_wait_query(fx->t->db, sql, "downloading", 4000));
	g_assert_true(axis_job_queue_stop(queue, 5000));
}

/* Spec (web-shell): a tracking run that is idle for another client's sake
 * reads a newly sent download on its next pass. */
static void
test_tracking_run_reads_a_new_download_at_once(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_download_fake_base(fx->fake);
	g_autoptr(AxisJobQueue) queue = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sql = NULL;
	gint64 job = 0;
	gint64 first;
	gint64 second;
	gint64 id;

	first = add_client(fx, "Idle", base, PASSWORD);
	second = add_client(fx, "Torrents", base, PASSWORD);
	g_assert_cmpint(first, !=, second);
	queue = track_queue_start(fx);
	g_assert_true(axis_job_queue_enqueue(queue, AXIS_DOWNLOAD_TRACK_KIND, NULL, NULL, &job, NULL, &error));
	g_assert_no_error(error);
	axis_job_queue_wake(queue);
	sql = g_strdup_printf("SELECT state FROM job WHERE id = %" G_GINT64_FORMAT, job);
	g_assert_true(axis_test_wait_query(fx->t->db, sql, "running", 5000));
	/* Both clients read once; now both wait out the idle interval. */
	g_usleep(1500 * 1000);

	id = send_and_read_by_client(fx, second);
	g_free(sql);
	sql = g_strdup_printf("SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, id);
	g_assert_true(axis_test_wait_query(fx->t->db, sql, "downloading", 4000));
	g_assert_true(axis_job_queue_stop(queue, 5000));
}

/* Spec: Client secrets stay with their client */
static void
test_secrets_in_no_page_log_or_job(Fixture *fx, gconstpointer data)
{
	gint64 client = 0;
	g_autofree gchar *session = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *jobs = NULL;
	const gchar *pages[5];
	guint lists_before;
	guint i;

	downloading(fx, &client);
	path = g_strdup_printf("/downloads/clients/%" G_GINT64_FORMAT, client);
	lists_before = axis_test_download_fake_count(fx->fake, "list", NULL);
	g_assert_true(run_job_and_wait(fx));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "list", NULL), >, lists_before);
	session = axis_test_download_fake_session(fx->fake);
	g_assert_nonnull(session);
	pages[0] = "/";
	pages[1] = "/downloads";
	pages[2] = "/downloads/clients";
	pages[3] = path;
	pages[4] = NULL;
	for (i = 0; pages[i] != NULL; i++) {
		AxisTestReply page = get_as(fx, fx->admin, pages[i]);

		g_assert_cmpuint(page.status, ==, 200);
		assert_lacks(page.body, PASSWORD);
		assert_lacks(page.body, session);
		axis_test_reply_clear(&page);
	}
	jobs = axis_test_db_queryf(fx->t->db, "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%%%s%%'"
		" OR to_jsonb(j)::text LIKE '%%%s%%'", PASSWORD, session);
	g_assert_cmpstr(jobs, ==, "0");
	{
		static const GLogLevelFlags levels[] = {
			G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO, G_LOG_LEVEL_DEBUG
		};
		g_autofree gchar *pattern = g_strdup_printf("*%s*", session);
		guint l;

		for (l = 0; l < G_N_ELEMENTS(levels); l++) {
			g_assert_cmpuint(axis_test_log_count(levels[l], "*" PASSWORD "*"), ==, 0);
			g_assert_cmpuint(axis_test_log_count(levels[l], pattern), ==, 0);
		}
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web-downloads/" path, Fixture, NULL, func)
	ADD("clients/administrators-only", test_clients_page_is_for_administrators);
	ADD("clients/empty", test_clients_page_empty);
	ADD("clients/add-edit-test", test_add_edit_and_test_a_client);
	ADD("clients/qbittorrent", test_qbittorrent_client_on_the_pages);
	ADD("clients/wrong-secret", test_wrong_secret_on_the_page);
	ADD("clients/problems", test_clients_with_problems);
	ADD("clients/path-mappings", test_path_mappings_from_the_page);
	ADD("clients/back-to-place", test_back_to_place);
	ADD("downloads/page", test_downloads_page);
	ADD("downloads/live", test_progress_is_live);
	ADD("downloads/client-down", test_client_down_on_the_page);
	ADD("downloads/partial-for-long", test_partial_for_long_on_the_page);
	ADD("downloads/remove", test_remove_with_confirmation);
	ADD("downloads/remove-gone", test_remove_a_download_already_gone);
	AXIS_TEST_ADD_DB_CASE("/web-downloads/downloads/remove-in-library", Fixture, NULL, test_remove_in_a_library_folder);
	AXIS_TEST_ADD_DB_CASE("/web-downloads/downloads/remove-in-library-forged", Fixture, "forged",
		test_remove_in_a_library_folder);
	AXIS_TEST_ADD_DB_CASE("/web-downloads/downloads/remove-own-folder", Fixture, NULL, test_remove_outside_the_library);
	AXIS_TEST_ADD_DB_CASE("/web-downloads/downloads/remove-in-downloads-root", Fixture, "registered",
		test_remove_outside_the_library);
	AXIS_TEST_ADD_DB_CASE("/web-downloads/downloads/remove-unmapped", Fixture, "unmapped",
		test_remove_outside_the_library);
	ADD("downloads/removed-files", test_removed_download_files_on_the_page);
	ADD("clients/switched-off", test_switched_off_on_the_pages);
	ADD("dashboard/coming-in", test_dashboard_shows_what_is_coming_in);
	ADD("secrets/nowhere", test_secrets_in_no_page_log_or_job);
	ADD("track/no-client-ends-at-once", test_tracking_without_a_client_ends_at_once);
	ADD("track/starts-when-sent", test_tracking_starts_when_a_download_is_sent);
	ADD("track/running-reads-new", test_tracking_run_reads_a_new_download_at_once);
#undef ADD
	return g_test_run();
}
