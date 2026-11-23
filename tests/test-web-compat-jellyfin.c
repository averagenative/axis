/*
 * test-web-compat-jellyfin.c - the Compatibility pages with a Plex and a
 * Jellyfin server connected at once: which server everything came from,
 * the stated reasons, the connection test per server, the path map per
 * server, and the key in no page
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The same shell as test-web-compat.c, with the fake Plex of
 * tests/axis-test-plex.h and a second fake server that serves the answers a
 * real Jellyfin gave (tests/fixtures/compat/jellyfin/recorded).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-analysis.h"
#include "axis-test-compat.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-savings.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "library/axis-analysis.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "transcode/axis-estimator.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-compat.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-library.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-savings.h"
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
	AxisAnalysis       *analysis;
	AxisEstimator      *estimator;
	AxisCompat         *compat;
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
	AxisWebProbe       *probe_ui;
	AxisWebBrowser     *browser_ui;
	AxisWebAnalysis    *analysis_ui;
	AxisWebSavings     *savings_ui;
	AxisWebCompat      *compat_ui;
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
	AxisTestLibrary     view;
	AxisTestSavings     s;
	AxisTestPlex       *plex;
	AxisTestCompat      tc;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisLibraryTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*does not match what is at*");
	axis_session_store_set_idle_seconds(30 * 24 * 3600);

	fx->dir = axis_test_storage_dir_new();
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
	fx->analysis = axis_analysis_new(fx->pool, fx->settings);
	fx->estimator = axis_estimator_new(fx->pool, fx->storage, fx->settings);
	fx->compat = axis_compat_new(fx->pool, fx->storage, fx->settings, fx->estimator);
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);
	fx->browser_ui = axis_web_browser_new(fx->library, fx->probe, fx->storage, fx->hub);
	fx->analysis_ui = axis_web_analysis_new(fx->analysis);
	fx->savings_ui = axis_web_savings_new(fx->estimator);
	fx->compat_ui = axis_web_compat_new(fx->compat);
	axis_web_savings_set_compat(fx->savings_ui, fx->compat);
	axis_web_probe_set_estimator(fx->probe_ui, fx->estimator);
	axis_web_browser_set_estimator(fx->browser_ui, fx->estimator);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(),
		fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(),
		fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_library_get_module(),
		fx->library_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_probe_get_module(),
		fx->probe_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_browser_get_module(),
		fx->browser_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_analysis_get_module(),
		fx->analysis_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_savings_get_module(),
		fx->savings_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_compat_get_module(),
		fx->compat_ui, &error));
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
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);

	fx->view.db = fx->db;
	fx->view.pool = fx->pool;
	fx->view.settings = fx->settings;
	fx->view.storage = fx->storage;
	fx->view.library = fx->library;
	fx->view.dir = fx->dir;
	fx->view.media = g_build_filename(fx->dir, "media", NULL);
	fx->view.stash = g_build_filename(fx->dir, "stash", NULL);
	axis_test_storage_mkdir(fx->view.media);
	axis_test_storage_mkdir(fx->view.stash);
	axis_test_savings_build(&fx->view, &fx->s);

	/* The support functions of axis-test-compat.h work on this fixture too. */
	fx->tc.lib = &fx->view;
	fx->tc.s = fx->s;
	fx->tc.settings = fx->settings;
	fx->tc.estimator = fx->estimator;
	fx->tc.compat = fx->compat;
	fx->plex = axis_test_plex_new();
	g_assert_nonnull(fx->plex);
	axis_test_plex_standard(fx->plex);
	fx->tc.plex = fx->plex;
	axis_test_compat_connect(&fx->tc);
	axis_test_compat_enable_jellyfin(&fx->tc);
	axis_test_compat_connect_jellyfin(&fx->tc);
	axis_test_compat_map_standard(&fx->tc);
	axis_test_compat_map_jellyfin_standard(&fx->tc);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	if (fx->db == NULL) {
		axis_test_storage_dir_free(fx->dir);
		axis_test_log_reset();
		return;
	}

	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_compat_stop(fx->compat_ui);
	axis_web_savings_stop(fx->savings_ui);
	axis_web_analysis_stop(fx->analysis_ui);
	axis_web_browser_stop(fx->browser_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_probe_stop(fx->probe_ui);
	axis_web_library_stop(fx->library_ui);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_compat_free(fx->compat_ui);
	axis_web_savings_free(fx->savings_ui);
	axis_web_analysis_free(fx->analysis_ui);
	axis_web_browser_free(fx->browser_ui);
	axis_web_probe_free(fx->probe_ui);
	axis_web_library_free(fx->library_ui);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_compat_unref(fx->compat);
	axis_estimator_unref(fx->estimator);
	axis_analysis_unref(fx->analysis);
	axis_probe_unref(fx->probe);
	axis_library_unref(fx->library);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_plex_free(fx->tc.jellyfin);
	axis_test_plex_free(fx->plex);
	axis_test_db_free(fx->db);
	axis_test_savings_clear(&fx->s);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->view.media);
	g_free(fx->view.stash);
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
	AxisTestReply page = get_as(fx, cookie, "/savings");
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
	if (haystack == NULL || strstr(haystack, needle) == NULL)
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

/* Every link is a path made of characters that cannot break out of it. */
static void
assert_links_safe(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("href=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) allowed = g_regex_new("^[/#A-Za-z0-9_.?=&;%:+~-]*$", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *href = g_match_info_fetch(info, 1);

		if (!g_regex_match(allowed, href, 0, NULL))
			g_error("a link with something other than a path in it: %s", href);
		g_match_info_next(info, NULL);
	}
}

/* Every page the module has, as an administrator: none shows either secret. */
static void
assert_secrets_in_no_page(Fixture *fx)
{
	static const gchar *const paths[] = {
		"/compat", "/compat/devices", "/compat/devices/jellyfin~axis-test-phone", "/compat/verdicts",
		"/compat/failures", "/compat/mapping", "/savings", "/savings/target/space_saver", "/admin/settings",
		"/admin/modules", "/", NULL
	};
	guint i;

	for (i = 0; paths[i] != NULL; i++) {
		AxisTestReply page = get_as(fx, fx->admin, paths[i]);

		assert_lacks(page.body, AXIS_TEST_COMPAT_JELLYFIN_KEY);
		assert_lacks(page.body, AXIS_TEST_COMPAT_TOKEN);
		axis_test_reply_clear(&page);
	}
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*" AXIS_TEST_COMPAT_JELLYFIN_KEY "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*" AXIS_TEST_COMPAT_JELLYFIN_KEY "*"), ==, 0);
}

static void
collect(Fixture *fx)
{
	AxisCompatPollSummary poll;
	AxisCompatHistorySummary history;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_poll(fx->compat, TRUE, NULL, &poll, &error));
	g_assert_true(axis_compat_import_history(fx->compat, TRUE, NULL, &history, &error));
	g_assert_no_error(error);
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Spec: More than one server at once: each page says which server. */
static void
test_the_pages_say_which_server_everything_came_from(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	collect(fx);

	page = get_as(fx, fx->admin, "/compat");
	g_assert_cmpuint(page.status, ==, 200);
	/* A card for each server, named; each with its own state. */
	assert_contains(page.body, "<h2>Connection to Plex</h2>");
	assert_contains(page.body, "<h2>Connection to Jellyfin</h2>");
	assert_contains(page.body, "Invented Server, version 1.43.5.0-invented");
	assert_contains(page.body, "83b9dc12ff49, version 10.11.8");
	assert_contains(page.body, "Test the Jellyfin connection");
	assert_contains(page.body, "Import the play history now");
	/* Jellyfin keeps no history: said so, once. */
	assert_contains(page.body, "not available from this server");
	assert_contains(page.body, "Jellyfin keeps no play history per device");
	assert_contains(page.body, "http://127.0.0.1:");
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/devices");
	assert_contains(page.body, "Pixel Phone");
	assert_contains(page.body, "/compat/devices/jellyfin~axis-test-phone");
	assert_contains(page.body, ">Jellyfin<");
	assert_contains(page.body, ">Plex<");
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/devices/jellyfin~axis-test-phone");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Pixel Phone");
	assert_contains(page.body, "Reason the server gave");
	assert_contains(page.body, "VideoCodecNotSupported");
	assert_contains(page.body, "ContainerNotSupported");
	assert_contains(page.body, "converted: the device did not take the container");
	assert_contains(page.body, "/compat/devices/jellyfin~axis-test-phone/pin");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/devices/jellyfin~axis-test-tablet");
	assert_contains(page.body, "converted: the device did not take the bit depth");
	assert_contains(page.body, "VideoBitDepthNotSupported");
	axis_test_reply_clear(&page);

	/* The Plex device's address is what it always was. */
	page = get_as(fx, fx->admin, "/compat/devices/client-android-1");
	g_assert_cmpuint(page.status, ==, 200);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/compat/devices/nonsense~axis-test-phone");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/compat/devices/jellyfin~no-such-device");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/compat/devices/jellyfin~axis-test-phone");
	g_assert_cmpuint(page.status, ==, 403);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/failures");
	assert_contains(page.body, "the device did not take the bit depth: VideoBitDepthNotSupported");
	assert_contains(page.body, "ContainerBitrateExceedsLimit");
	assert_contains(page.body, "/compat/devices/jellyfin~axis-test-hotel");
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/mapping");
	assert_contains(page.body, "Films");
	assert_contains(page.body, "/media/films");
	assert_contains(page.body, "Mapped");
	assert_contains(page.body, "name=\"server\"");
	assert_contains(page.body, "value=\"jellyfin\"");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/verdicts");
	assert_contains(page.body, "Films (Jellyfin library)");
	assert_contains(page.body, "(Plex library)");
	axis_test_reply_clear(&page);
	assert_secrets_in_no_page(fx);
}

static void
test_the_connection_test_is_per_server(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = post_as(fx, fx->admin, "/compat/test", "server=jellyfin");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Connected to 83b9dc12ff49, version 10.11.8");
	axis_test_reply_clear(&reply);
	/* No server named: Plex's, as it always was. */
	reply = post_as(fx, fx->admin, "/compat/test", "");
	assert_contains(reply.body, "Connected to Invented Server, version 1.43.5.0-invented");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/test", "server=nonsense");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* A refused key is reported without the key. */
	axis_test_plex_route(fx->tc.jellyfin, "/System/Info", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	reply = post_as(fx, fx->admin, "/compat/test", "server=jellyfin");
	assert_contains(reply.body, "The connection failed");
	assert_contains(reply.body, "token was refused");
	assert_lacks(reply.body, AXIS_TEST_COMPAT_JELLYFIN_KEY);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit:*compat connection test: failed*"), ==, 1);
	assert_secrets_in_no_page(fx);
}

static void
test_actions_name_their_server(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	collect(fx);
	/* A path map entry for Jellyfin, beside Plex's own. */
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server=jellyfin&server_path=/media/shows&axis_path=/mnt/shows");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_path_map", "server_kind = 'jellyfin' AND server_path = '/media/shows'"), ==, 1);
	/* The same path may be mapped for the other server too. */
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server_path=/media/shows&axis_path=/mnt/other");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_path_map", "server_path = '/media/shows'"), ==, 2);
	/* But not twice for one. */
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server=jellyfin&server_path=/media/shows&axis_path=/mnt/again");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "already mapped");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server=nonsense&server_path=/a&axis_path=/b");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* Pinning a Jellyfin device. */
	reply = post_as(fx, fx->admin, "/compat/devices/jellyfin~axis-test-setup/pin", "pinned=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "/compat/devices/jellyfin~axis-test-setup?done=pinned");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_device", "pinned AND server_kind = 'jellyfin'"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_device", "pinned AND server_kind = 'plex'"), ==, 0);
	reply = post_as(fx, fx->admin, "/compat/devices/jellyfin~no-such/pin", "pinned=yes");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/devices/bad~~id/pin", "pinned=yes");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

static void
test_the_settings_page_labels_each_server_and_shows_no_key(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(reply.body, "Media server address (Plex)");
	assert_contains(reply.body, "Media server token (Plex)");
	assert_contains(reply.body, "Media server address (Jellyfin)");
	assert_contains(reply.body, "Media server API key (Jellyfin)");
	assert_lacks(reply.body, AXIS_TEST_COMPAT_JELLYFIN_KEY);
	assert_lacks(reply.body, AXIS_TEST_COMPAT_TOKEN);
	axis_test_reply_clear(&reply);
}

static void
test_only_jellyfin_connected_the_overview_has_one_card_named_for_it(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	g_assert_true(axis_settings_clear(fx->settings, AXIS_COMPAT_SETTING_TOKEN, NULL));
	collect(fx);
	page = get_as(fx, fx->admin, "/compat");
	assert_contains(page.body, "83b9dc12ff49, version 10.11.8");
	assert_lacks(page.body, "Invented Server");
	assert_lacks(page.body, "The media server connection is not set up");
	assert_contains(page.body, "Test the Jellyfin connection");
	axis_test_reply_clear(&page);
}

#define ADD(name, func) g_test_add("/web/compat-jellyfin/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("pages", test_the_pages_say_which_server_everything_came_from);
	ADD("connection-test", test_the_connection_test_is_per_server);
	ADD("actions", test_actions_name_their_server);
	ADD("settings", test_the_settings_page_labels_each_server_and_shows_no_key);
	ADD("only-jellyfin", test_only_jellyfin_connected_the_overview_has_one_card_named_for_it);
	return g_test_run();
}
