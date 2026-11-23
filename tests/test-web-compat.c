/*
 * test-web-compat.c - the Compatibility pages and their verdicts on the
 * Savings pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell, storage, library, probe, browser, analysis, estimate and
 * compat modules on an ephemeral loopback port, as in test-web-savings.c,
 * with the library of tests/axis-test-savings.h and the fake media server of
 * tests/axis-test-plex.h.
 *
 * Spec scenarios: Non-administrator refused; Names off by default; Token
 * stays secret (pages and log); Connection test; Coverage shown; Mapped
 * through the path map / Unmapped library; Largest safe saving; Nothing
 * safe; the verdict on the Savings pages names no device and no account.
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

	fx->plex = axis_test_plex_new();
	g_assert_nonnull(fx->plex);
	axis_test_plex_standard(fx->plex);
	{
		g_autofree gchar *base = axis_test_plex_base(fx->plex);

		g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_SETTING_ADDRESS, base, &error));
		g_assert_true(axis_settings_set(fx->settings, AXIS_COMPAT_SETTING_TOKEN, AXIS_TEST_COMPAT_TOKEN, &error));
		g_assert_no_error(error);
	}
	/* The support functions of axis-test-compat.h work on this fixture too. */
	fx->tc.lib = &fx->view;
	fx->tc.s = fx->s;
	fx->tc.settings = fx->settings;
	fx->tc.estimator = fx->estimator;
	fx->tc.compat = fx->compat;
	fx->tc.plex = fx->plex;
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

/* A redirect to @location: the address and word, then (as the pages come
 * back to a place) the place. */
static void
assert_redirect_to(
	AxisTestReply *reply,
	const gchar   *location
){
	const gchar *got = axis_test_reply_header(reply, "Location");

	g_assert_nonnull(got);
	g_assert_true(g_str_has_prefix(got, location));
	g_assert_true(got[strlen(location)] == '\0' || g_str_has_prefix(got + strlen(location), "&at="));
}

/* The answer of a form that stays on its page: it names the place in "at"
 * and ends "#place", and that page shows the notice inside the element. */
static void
assert_place(
	Fixture       *fx,
	AxisTestReply *reply,
	const gchar   *place
){
	const gchar *location = axis_test_reply_header(reply, "Location");
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *path = NULL;
	const gchar *element;
	const gchar *notice;
	AxisTestReply page;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strrchr(location, '#') - location);
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, id);
	notice = strstr(page.body, "notice-here");
	g_assert_nonnull(element);
	g_assert_nonnull(notice);
	g_assert_true(element < notice);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(reply);
}

/* With AXIS_TEST_COMPAT_PAGES set to a directory, a page is saved there for
 * looking at in a browser. */
static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_COMPAT_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

/*
 * Spec (web-shell): Media management pages say what they are.  Its row in
 * order, the first of the second run set apart by a divider, and the open
 * page's heading its label.
 */
static void
assert_management_row(
	const gchar *body,
	const gchar *heading
){
	static const gchar *const order[] = {
		">Files</a>", ">Disks</a>", ">Disk usage</a>", ">Space savings</a>", ">Player compatibility</a>", NULL
	};
	const gchar *row = strstr(body, "aria-label=\"Library pages\"");
	const gchar *end = row != NULL ? strstr(row, "</nav>") : NULL;
	const gchar *divider = row != NULL ? strstr(row, "nav-run-start") : NULL;
	const gchar *last = row;
	g_autofree gchar *h1 = g_strdup_printf("<h1>%s</h1><p class=\"page-intro\">", heading);
	guint i;

	g_assert_nonnull(row);
	g_assert_nonnull(end);
	for (i = 0; order[i] != NULL; i++) {
		const gchar *at = strstr(last, order[i]);

		if (at == NULL || at > end)
			g_error("the Library row lacks %s in order:\n%.*s", order[i], (gint)(end - row), row);
		last = at;
	}
	/* One divider, before Disk usage and after Disks. */
	g_assert_nonnull(divider);
	g_assert_true(divider < end);
	g_assert_true(divider > strstr(row, ">Disks</a>"));
	g_assert_true(divider < strstr(row, ">Disk usage</a>"));
	g_assert_null(g_strstr_len(divider + 1, end - divider - 1, "nav-run-start"));
	assert_contains(body, h1);
}

static void
assert_links_safe(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("href=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) allowed = g_regex_new("^[/#A-Za-z0-9_.?=&;%:+-]*$", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *href = g_match_info_fetch(info, 1);

		if (!g_regex_match(allowed, href, 0, NULL))
			g_error("a link with something other than a path in it: %s", href);
		g_match_info_next(info, NULL);
	}
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	assert_lacks(body, "onclick");
	assert_lacks(body, "javascript:");
}

/* The token is in no recorded log message, whatever its level. */
static void
assert_token_not_logged(void)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO,
		G_LOG_LEVEL_DEBUG
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		g_assert_cmpuint(axis_test_log_count(levels[i], "*" AXIS_TEST_COMPAT_TOKEN "*"), ==, 0);
}

/* Every page the module has, as an administrator: none shows the token. */
static void
assert_token_in_no_page(Fixture *fx)
{
	static const gchar *const paths[] = {
		"/compat", "/compat/devices", "/compat/verdicts", "/compat/failures", "/compat/mapping", "/compat/rescans", "/savings",
		"/savings/target/space_saver", "/admin/settings", "/admin/modules", "/", NULL
	};
	guint i;

	for (i = 0; paths[i] != NULL; i++) {
		AxisTestReply page = get_as(fx, fx->admin, paths[i]);

		assert_lacks(page.body, AXIS_TEST_COMPAT_TOKEN);
		axis_test_reply_clear(&page);
	}
	assert_token_not_logged();
}

static void
refresh_estimates(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_estimator_refresh(fx->estimator, "direct", 0, NULL, NULL, &error));
	g_assert_no_error(error);
}

/* The observed session, of the film "Big (2001)" of the Films root. */
static void
serve_big(
	Fixture     *fx,
	const gchar *session_id
){
	g_autofree gchar *text = axis_test_compat_fixture("session-converted-audio.json", "Invented Film (2001)", "Big (2001)");
	GString *s = g_string_new(text);
	g_autofree gchar *out = NULL;

	g_string_replace(s, "sess-converted-audio-1", session_id, 0);
	out = g_string_free(s, FALSE);
	axis_test_plex_route(fx->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, out, 0);
}

/* One look at the sessions (which also fetches the libraries). */
static void
poll_only(Fixture *fx)
{
	AxisCompatPollSummary poll;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_compat_poll(fx->compat, TRUE, NULL, &poll, &error));
	g_assert_no_error(error);
}

/* A look at the sessions and an import of the history. */
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

/* ── Access ─────────────────────────────────────────────────────── */

/* Spec: Non-administrator refused */
static void
test_only_administrators_see_the_pages_and_every_post_needs_the_token(Fixture *fx, gconstpointer data)
{
	static const gchar *const gets[] = {
		"/compat", "/compat/devices", "/compat/devices/client-tizen-1", "/compat/verdicts", "/compat/failures",
		"/compat/mapping", "/compat/rescans", NULL
	};
	static const gchar *const posts[] = {
		"/compat/test", "/compat/poll", "/compat/history", "/compat/mapping/add", "/compat/mapping/1/remove",
		"/compat/devices/client-tizen-1/pin", "/compat/rescans/1/again", NULL
	};
	AxisTestReply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_device(&fx->tc, "client-tizen-1", "Living room TV");
	for (i = 0; gets[i] != NULL; i++) {
		reply = get_as(fx, NULL, gets[i]);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "/login");
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->viewer, gets[i]);
		g_assert_cmpuint(reply.status, ==, 403);
		assert_lacks(reply.body, "Living room TV");
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, gets[i]);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_csp_clean(reply.body);
		assert_links_safe(reply.body);
		axis_test_reply_clear(&reply);
	}
	for (i = 0; posts[i] != NULL; i++) {
		/* A user who is not an administrator, with a valid token. */
		reply = post_as(fx, fx->viewer, posts[i], "server_path=/a&axis_path=/b&pinned=yes");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		/* An administrator without the token. */
		reply = axis_test_web_request(fx->session, fx->port, "POST", posts[i], fx->admin, NULL,
			"server_path=/a&axis_path=/b&pinned=yes");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "job", "kind LIKE 'compat.%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_path_map", NULL), ==, 0);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_device", "pinned"), ==, 0);
}

/* ── The pages ──────────────────────────────────────────────────── */

/* Spec: Coverage shown / Evidence is per stream / Mapped through the path map */
static void
test_the_pages_show_what_was_collected(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	g_autofree gchar *big = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_map_standard(&fx->tc);
	serve_big(fx, "sess-page-1");
	collect(fx);

	page = get_as(fx, fx->admin, "/compat");
	g_assert_cmpuint(page.status, ==, 200);
	assert_management_row(page.body, "Player compatibility");
	save_page("compat.html", page.body);
	/* The first run's pages: the same row, their own heading and intro. */
	{
		AxisTestReply files = get_as(fx, fx->admin, "/library");
		AxisTestReply storage = get_as(fx, fx->admin, "/storage");

		g_assert_cmpuint(files.status, ==, 200);
		g_assert_cmpuint(storage.status, ==, 200);
		assert_management_row(files.body, "Files");
		assert_management_row(storage.body, "Disks and library folders");
		save_page("files.html", files.body);
		save_page("storage.html", storage.body);
		axis_test_reply_clear(&files);
		axis_test_reply_clear(&storage);
	}
	assert_contains(page.body, "Invented Server, version 1.43.5.0-invented");
	assert_contains(page.body, "1 session observed from");
	assert_contains(page.body, "0 on the local network");
	assert_contains(page.body, "1 remote");
	assert_contains(page.body, "1 matched to a file in the index");
	assert_contains(page.body, "4 past plays imported");
	assert_contains(page.body, "unknown is never safe");
	assert_contains(page.body, "Test the Plex connection");
	assert_contains(page.body, "Import the play history now");
	/* The address is shown, the token never. */
	assert_contains(page.body, "http://127.0.0.1:");
	assert_lacks(page.body, AXIS_TEST_COMPAT_TOKEN);
	assert_csp_clean(page.body);
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/devices");
	assert_contains(page.body, "Pixel");
	assert_contains(page.body, "/compat/devices/client-android-1");
	assert_contains(page.body, "Always consider");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/devices/client-android-1");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "client-android-1");
	/* Per stream: the video passed through, the audio converted. */
	assert_contains(page.body, "passed through");
	assert_contains(page.body, "converted: the device did not take the codec (to mp3)");
	assert_contains(page.body, "h264 high 8-bit 1080p");
	assert_contains(page.body, "eac3 6 ch");
	assert_contains(page.body, "Always consider this device");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/devices/no-such-device");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/compat/devices/bad%20id");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/failures");
	assert_contains(page.body, "Pixel");
	assert_contains(page.body, "eac3");
	assert_contains(page.body, "mp3");
	big = g_strdup_printf("/library/file/%" G_GINT64_FORMAT, axis_test_compat_file_id(&fx->tc, fx->s.films_a,
		"Big (2001)/Big (2001).mkv"));
	assert_contains(page.body, big);
	assert_contains(page.body, "Big (2001).mkv");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/mapping");
	assert_contains(page.body, "Films");
	assert_contains(page.body, "Mapped");
	assert_contains(page.body, "Partly mapped");
	assert_contains(page.body, "/data/tv-extra");
	assert_contains(page.body, "Not mapped");
	assert_contains(page.body, "/data/movies");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->admin, "/compat/verdicts");
	assert_contains(page.body, "Films");
	/* Nobody has been seen playing the Films: every target is unknown. */
	assert_contains(page.body, "badge-warn");
	assert_lacks(page.body, "Recommended for this library");
	axis_test_reply_clear(&page);

	/* Rescan requests: what each root reaches, and what was sent. */
	page = get_as(fx, fx->admin, "/compat/rescans");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Folders the servers are told about");
	assert_contains(page.body, "Nothing has been sent yet.");
	assert_lacks(page.body, "Not mapped to a Plex library");
	axis_test_reply_clear(&page);
	axis_test_db_exec(fx->tc.lib->db, "DELETE FROM compat_path_map WHERE server_path = '/data/tv'");
	{
		g_autofree gchar *shows = g_build_filename(fx->s.disk_a, "Shows", NULL);

		axis_test_compat_map(&fx->tc, "/nolib/tv", shows);
	}
	axis_test_db_exec(fx->tc.lib->db,
		"INSERT INTO compat_notify (server_kind, folder, change, state, attempts, done_at, sent_folder, note)"
		" VALUES ('plex', '/media/a/Films/Hearth (1995)', 'created', 'sent', 1, now(), '/data/movies/Hearth (1995)', NULL),"
		" ('plex', '/media/a/Shows/Odd <b>Show</b>', 'modified', 'dropped', 6, now(), NULL,"
		"  'Given up after 6 tries: the media server answered 503'),"
		" ('plex', '/media/a/Films/Alder (1979)', 'deleted', 'unmapped', 0, now(), NULL, 'Plex has no library'),"
		" ('plex', '/media/a/Films/Rokan (1998)', 'created', 'pending', 0, NULL, NULL, NULL)");
	page = get_as(fx, fx->admin, "/compat/rescans");
	g_assert_cmpuint(page.status, ==, 200);
	save_page("compat-rescans.html", page.body);
	assert_contains(page.body, "Not mapped to a Plex library");
	assert_contains(page.body, "/nolib/tv");
	assert_contains(page.body, "Dropped");
	assert_contains(page.body, "Given up after 6 tries");
	assert_contains(page.body, "Not sent");
	assert_contains(page.body, "Waiting");
	assert_contains(page.body, "Odd &lt;b&gt;Show&lt;/b&gt;");
	assert_lacks(page.body, "<b>Show</b>");
	axis_test_reply_clear(&page);
	assert_token_in_no_page(fx);
}

static void
test_names_from_the_server_are_escaped_and_never_part_of_a_link(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_map_standard(&fx->tc);
	axis_test_compat_device(&fx->tc, "evil-1", "<script>alert(1)</script> \"quoted\" & more");
	axis_test_compat_play(&fx->tc, "evil-1", "1", 1);
	axis_test_compat_evidence(&fx->tc, "evil-1", "1", 1, "h264", "high", 8, "1080p", 0, "mkv", "direct", FALSE, 1);
	poll_only(fx);
	page = get_as(fx, fx->admin, "/compat/devices");
	assert_lacks(page.body, "<script>alert(1)</script>");
	assert_contains(page.body, "&lt;script&gt;alert(1)&lt;/script&gt;");
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/compat/verdicts");
	assert_lacks(page.body, "<script>alert(1)</script>");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/compat/devices/evil-1");
	assert_lacks(page.body, "<script>alert(1)</script>");
	assert_contains(page.body, "&lt;script&gt;");
	axis_test_reply_clear(&page);
}

/* Spec: Safe / Unknown is never safe, with the devices behind each verdict. */
static void
test_the_verdicts_page_names_the_devices_behind_each_verdict(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_map_standard(&fx->tc);
	poll_only(fx);
	axis_test_compat_device(&fx->tc, "tv", "Living room TV");
	axis_test_compat_play(&fx->tc, "tv", "1", 1);
	axis_test_compat_evidence(&fx->tc, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 0, "mkv", "direct", FALSE, 1);
	axis_test_compat_evidence(&fx->tc, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 1, "mkv", "direct", FALSE, 1);
	axis_test_compat_device(&fx->tc, "box", "Bedroom box");
	axis_test_compat_play(&fx->tc, "box", "1", 1);
	refresh_estimates(fx);

	page = get_as(fx, fx->admin, "/compat/verdicts");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Living room TV");
	assert_contains(page.body, "Bedroom box");
	assert_contains(page.body, "Seen playing directly hevc main 10 10-bit 2160p");
	assert_contains(page.body, "never seen with this codec");
	assert_contains(page.body, "badge-succeeded");
	assert_contains(page.body, "badge-warn");
	/* The box has not been seen playing anything: no target is safe. */
	assert_contains(page.body, "No conversion profile is safe for this media server library");
	assert_contains(page.body, "Bedroom box (unknown)");
	assert_lacks(page.body, "Recommended for this library");
	axis_test_reply_clear(&page);

	/* The box is seen playing what the Space saver makes: it is recommended. */
	axis_test_compat_evidence(&fx->tc, "box", "1", 1, "hevc", "main 10", 10, "2160p", 0, "mkv", "direct", FALSE, 1);
	axis_test_compat_evidence(&fx->tc, "box", "1", 1, "hevc", "main 10", 10, "2160p", 1, "mkv", "direct", FALSE, 1);
	page = get_as(fx, fx->admin, "/compat/verdicts");
	assert_contains(page.body, "Recommended for this library: Video: Space saver");
	axis_test_reply_clear(&page);
}

/* The verdicts and the recommendation on the Savings pages: counts only. */
static void
test_the_savings_pages_show_verdicts_without_devices_or_accounts(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_map_standard(&fx->tc);
	serve_big(fx, "sess-savings-1");
	poll_only(fx);
	refresh_estimates(fx);

	/* Before any play is seen: unknown, no recommendation. */
	page = get_as(fx, fx->viewer, "/savings");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Plays on your devices");
	assert_contains(page.body, "Recommended conversion profile per media server library");
	assert_contains(page.body, "badge-warn");
	assert_contains(page.body, "No profile is safe yet");
	assert_lacks(page.body, "Safe for all");
	axis_test_reply_clear(&page);

	/* The household's TV has been seen playing it all. */
	axis_test_compat_device(&fx->tc, "tv", "Living room TV");
	axis_test_compat_play(&fx->tc, "tv", "1", 1);
	axis_test_compat_evidence(&fx->tc, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 0, "mkv", "direct", FALSE, 1);
	axis_test_compat_evidence(&fx->tc, "tv", "1", 1, "hevc", "main 10", 10, "2160p", 1, "mkv", "direct", FALSE, 1);
	axis_test_db_exec(fx->db, "DELETE FROM compat_session WHERE device_id = 'client-android-1'");
	axis_test_db_exec(fx->db, "DELETE FROM compat_play WHERE device_id <> 'tv'");
	axis_test_db_exec(fx->db, "DELETE FROM compat_device WHERE client_id <> 'tv'");

	page = get_as(fx, fx->viewer, "/savings");
	assert_contains(page.body, "Safe for the one device that plays it");
	assert_contains(page.body, "Space saver");
	assert_contains(page.body, "badge-succeeded");
	/* No device, no account, no identifier, no link to the details. */
	assert_lacks(page.body, "Living room TV");
	assert_lacks(page.body, "Pixel");
	assert_lacks(page.body, "Bob Example");
	assert_lacks(page.body, "client-");
	assert_lacks(page.body, "/compat/");
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/savings/target/space_saver");
	assert_contains(page.body, "Plays on your devices, per media server library");
	assert_contains(page.body, "Films");
	assert_contains(page.body, "badge-succeeded");
	assert_lacks(page.body, "Living room TV");
	assert_lacks(page.body, "/compat/");
	axis_test_reply_clear(&page);

	/* An administrator is also pointed to the details. */
	page = get_as(fx, fx->admin, "/savings");
	assert_management_row(page.body, "Space savings");
	save_page("savings.html", page.body);
	assert_contains(page.body, "/compat/verdicts");
	assert_lacks(page.body, "Living room TV");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/savings/target/space_saver");
	assert_contains(page.body, "/compat/verdicts");
	axis_test_reply_clear(&page);
}

/* The verdicts go with the module. */
static void
test_the_module_off_removes_the_pages_and_the_verdicts(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_map_standard(&fx->tc);
	collect(fx);
	refresh_estimates(fx);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_COMPAT_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/compat");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/savings");
	g_assert_cmpuint(page.status, ==, 200);
	assert_lacks(page.body, "Plays on your devices");
	assert_lacks(page.body, "Recommended conversion profile per media server library");
	axis_test_reply_clear(&page);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_COMPAT_MODULE_ID, TRUE, &error));
	page = get_as(fx, fx->viewer, "/savings");
	assert_contains(page.body, "Plays on your devices");
	axis_test_reply_clear(&page);
}

/* ── Actions ────────────────────────────────────────────────────── */

/* Spec: Connection test */
static void
test_the_connection_test_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = post_as(fx, fx->admin, "/compat/test", "");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Connected to Invented Server, version 1.43.5.0-invented.");
	assert_lacks(reply.body, AXIS_TEST_COMPAT_TOKEN);
	axis_test_reply_clear(&reply);

	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_STATUS, 401, NULL, 401);
	reply = post_as(fx, fx->admin, "/compat/test", "");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The connection failed:");
	assert_contains(reply.body, "token was refused");
	assert_lacks(reply.body, AXIS_TEST_COMPAT_TOKEN);
	axis_test_reply_clear(&reply);

	/* A server that redirects to somewhere else: the page says it was refused. */
	axis_test_plex_route(fx->plex, "/identity", -1, AXIS_TEST_PLEX_REDIRECT, 302, "http://203.0.113.9:32400/identity", 0);
	reply = post_as(fx, fx->admin, "/compat/test", "");
	assert_contains(reply.body, "redirected to a different address");
	assert_lacks(reply.body, AXIS_TEST_COMPAT_TOKEN);
	axis_test_reply_clear(&reply);

	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit:*compat connection test: succeeded*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit:*compat connection test: failed*"), ==, 2);
	assert_token_not_logged();
}

static void
test_the_actions_queue_jobs_change_the_map_and_pin_devices(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	g_autofree gchar *id = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_device(&fx->tc, "client-tizen-1", "Living room TV");

	reply = post_as(fx, fx->admin, "/compat/poll", "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/compat?done=poll-requested");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/history", "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "job", "kind = 'compat.poll' AND queue = 'compat'"), ==, 1);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "job", "kind = 'compat.history' AND queue = 'compat'"), ==, 1);
	reply = get_as(fx, fx->admin, "/compat?done=poll-requested");
	assert_contains(reply.body, "will be looked at now");
	axis_test_reply_clear(&reply);

	/* add-attention-notices, slice N2: a media server that answers has no
	 * notice; one unreached for two hours, on a third failed check, has. */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Axis could not read");
	assert_lacks(reply.body, "has not reached the media server");
	axis_test_reply_clear(&reply);
	{
		g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_state", "true"), >, 0);
		axis_test_db_exec(fx->db, "UPDATE compat_state SET failures = 3, last_error = 'As the test made it.',"
			" last_poll_ok_at = now() - interval '2 hours'");
		axis_test_web_read_attention(fx->web);
		reply = get_as(fx, fx->admin, "/attention");
		assert_contains(reply.body, "<span>Axis has not reached the media server </span>");
		assert_contains(reply.body, "(3 failed checks in a row). Test the connection.");
		assert_contains(reply.body, "<a href=\"/compat\">Open Player compatibility</a>");
		axis_test_reply_clear(&reply);
		axis_test_db_exec(fx->db, "UPDATE compat_state SET failures = 0, last_error = NULL, last_poll_ok_at = now()");
	}

	/* The path map: add, refuse, remove. */
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server_path=%2Fdata%2Fmovies&axis_path=%2Fmedia%2Fdisk3%2Fmovies");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/compat/mapping?done=mapping-added");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_path_map", "server_path = '/data/movies'"), ==, 1);
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server_path=data%2Fmovies&axis_path=%2Fmedia");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "must be an absolute path");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server_path=%2Fdata%2Fmovies&axis_path=%2Fmedia%2Fother");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "already mapped");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server_path=%2Fa%2F..%2Fb&axis_path=%2Fmedia");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_path_map", NULL), ==, 1);
	reply = get_as(fx, fx->admin, "/compat/mapping");
	assert_contains(reply.body, "/media/disk3/movies");
	axis_test_reply_clear(&reply);
	id = axis_test_db_query(fx->db, "SELECT id FROM compat_path_map");
	{
		g_autofree gchar *path = g_strdup_printf("/compat/mapping/%s/remove", id);

		reply = post_as(fx, fx->admin, path, "");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_path_map", NULL), ==, 0);
	reply = post_as(fx, fx->admin, "/compat/mapping/abc/remove", "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* Always consider a device, and stop. */
	reply = post_as(fx, fx->admin, "/compat/devices/client-tizen-1/pin", "pinned=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/compat/devices/client-tizen-1?done=pinned");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_device", "pinned"), ==, 1);
	reply = get_as(fx, fx->admin, "/compat/devices/client-tizen-1?done=pinned");
	assert_contains(reply.body, "Stop always considering this device");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/devices/client-tizen-1/pin", "pinned=no");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_device", "pinned"), ==, 0);
	reply = post_as(fx, fx->admin, "/compat/devices/client-tizen-1/pin", "pinned=maybe");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/devices/nobody/pin", "pinned=yes");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit:*compat path map entry added*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit:*compat device pinned*"), ==, 1);
	assert_token_not_logged();
}

/* Spec (client-compatibility): a rescan request that was not sent can be
 * asked again from the Rescan requests page, by an administrator, with the
 * token; one that was sent or is waiting cannot; and a mapping added on the
 * Libraries and paths page sends the recent ones it reaches by itself. */
static void
test_rescan_requests_can_be_asked_again(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *films = NULL;
	g_autofree gchar *folder = NULL;
	g_autofree gchar *unmapped = NULL;
	g_autofree gchar *sent = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *again_sent = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *state = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	films = g_build_filename(fx->s.disk_a, "Films", NULL);
	folder = g_build_filename(films, "Marrowfield Lanterns (2011) {tmdb-990120}", NULL);
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO compat_notify (server_kind, folder, change, state, done_at,"
			" note) VALUES ('plex', '%s', 'created', 'unmapped', now(), 'No path mapping reaches this folder, so the"
			" server was not asked.') RETURNING id", folder);
		g_autofree gchar *sql_sent = g_strdup_printf("INSERT INTO compat_notify (server_kind, folder, change, state,"
			" done_at, sent_folder) VALUES ('plex', '%s/Ostrander Peaks (2018)', 'created', 'sent', now(),"
			" '/data/movies/Ostrander Peaks (2018)') RETURNING id", films);

		unmapped = axis_test_db_query(fx->db, sql);
		sent = axis_test_db_query(fx->db, sql_sent);
	}
	again = g_strdup_printf("/compat/rescans/%s/again", unmapped);
	again_sent = g_strdup_printf("/compat/rescans/%s/again", sent);

	/* The page: a button for the one not sent, none for the one sent. */
	reply = get_as(fx, fx->admin, "/compat/rescans");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, again);
	assert_contains(reply.body, ">Send this request again</button>");
	assert_contains(reply.body, "name=\"_csrf\"");
	assert_lacks(reply.body, again_sent);
	assert_csp_clean(reply.body);
	save_page("compat-rescans.html", reply.body);
	axis_test_reply_clear(&reply);

	/* Asked again: it waits, and the page says so. */
	reply = post_as(fx, fx->admin, again, "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/compat/rescans?done=asked-again");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT state || '|' || attempts FROM compat_notify WHERE id = %s",
			unmapped);

		state = axis_test_db_query(fx->db, sql);
	}
	g_assert_cmpstr(state, ==, "pending|0");
	reply = get_as(fx, fx->admin, "/compat/rescans?done=asked-again");
	assert_contains(reply.body, "waits again");
	axis_test_reply_clear(&reply);

	/* Waiting already, or sent: nothing to ask again.  No such request. */
	reply = post_as(fx, fx->admin, again, "");
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, again_sent, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "was sent");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/rescans/abc/again", "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/compat/rescans/987654321/again", "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit:*compat rescan request * asked again*"), ==, 1);

	/* Not sent again (no mapping reached it): a mapping added on the page
	 * that reaches it sends it by itself. */
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE compat_notify SET state = 'unmapped', done_at = now(),"
			" note = 'No path mapping reaches this folder.' WHERE id = %s", unmapped);

		axis_test_db_exec(fx->db, sql);
	}
	{
		g_autofree gchar *escaped = g_uri_escape_string(films, NULL, TRUE);

		form = g_strdup_printf("server_path=%%2Fdata%%2Fmovies&axis_path=%s", escaped);
	}
	reply = post_as(fx, fx->admin, "/compat/mapping/add", form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_redirect_to(&reply, "/compat/mapping?done=mapping-added");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *sql = g_strdup_printf("SELECT state FROM compat_notify WHERE id = %s", unmapped);

		g_free(state);
		state = axis_test_db_query(fx->db, sql);
	}
	g_assert_cmpstr(state, ==, "pending");
	reply = get_as(fx, fx->admin, "/compat/mapping?done=mapping-added");
	assert_contains(reply.body, "are sent again");
	axis_test_reply_clear(&reply);
	assert_token_not_logged();
}

/* The forms of the pages come back to their card or row, with the notice. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *folder = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *row = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *removal = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_device(&fx->tc, "client-tizen-1", "Living room TV");

	/* Look now and import: the connection's card. */
	reply = post_as(fx, fx->admin, "/compat/poll", "");
	assert_place(fx, &reply, "compat-connection");
	reply = post_as(fx, fx->admin, "/compat/history", "");
	assert_place(fx, &reply, "compat-connection");

	/* The path map: its card, for an entry added and one removed. */
	reply = post_as(fx, fx->admin, "/compat/mapping/add", "server_path=%2Fdata%2Fmovies&axis_path=%2Fmedia%2Fdisk3%2Fmovies");
	assert_place(fx, &reply, "path-map");
	id = axis_test_db_query(fx->db, "SELECT id FROM compat_path_map");
	removal = g_strdup_printf("/compat/mapping/%s/remove", id);
	reply = post_as(fx, fx->admin, removal, "");
	assert_place(fx, &reply, "path-map");

	/* A device's pin: the device's card. */
	reply = post_as(fx, fx->admin, "/compat/devices/client-tizen-1/pin", "pinned=yes");
	assert_place(fx, &reply, "device");

	/* Ask again: the request's row. */
	{
		g_autofree gchar *sql = NULL;

		folder = g_build_filename(fx->s.disk_a, "Films", "Marrowfield Lanterns (2011) {tmdb-990120}", NULL);
		sql = g_strdup_printf("INSERT INTO compat_notify (server_kind, folder, change, state, done_at, note) VALUES"
			" ('plex', '%s', 'created', 'unmapped', now(), 'No path mapping reaches this folder.') RETURNING id", folder);
		g_free(id);
		id = axis_test_db_query(fx->db, sql);
	}
	again = g_strdup_printf("/compat/rescans/%s/again", id);
	row = g_strdup_printf("rescan-%s", id);
	reply = post_as(fx, fx->admin, again, "");
	assert_place(fx, &reply, row);
}

/* Spec: Token stays secret: a full run with failures, every page, the log, the job table. */
static void
test_the_token_is_nowhere_after_a_full_run_including_failures(Fixture *fx, gconstpointer data)
{
	AxisCompatPollSummary summary;
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	axis_test_compat_map_standard(&fx->tc);
	serve_big(fx, "sess-secret-1");
	collect(fx);
	refresh_estimates(fx);
	reply = post_as(fx, fx->admin, "/compat/test", "");
	axis_test_reply_clear(&reply);
	/* The server fails in the ways that put text in messages. */
	axis_test_plex_route(fx->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 500, "{\"X-Plex-Token\":\"" AXIS_TEST_COMPAT_TOKEN "\"}", 0);
	g_assert_false(axis_compat_poll(fx->compat, TRUE, NULL, &summary, &error));
	g_clear_error(&error);
	axis_test_plex_route(fx->plex, "/status/sessions", -1, AXIS_TEST_PLEX_REDIRECT, 302, "http://203.0.113.9:32400/status/sessions", 0);
	g_assert_false(axis_compat_poll(fx->compat, TRUE, NULL, &summary, &error));
	g_clear_error(&error);
	axis_test_plex_route(fx->plex, "/status/sessions", -1, AXIS_TEST_PLEX_NORMAL, 200, "{\"MediaContainer\":\"" AXIS_TEST_COMPAT_TOKEN "\"}", 0);
	g_assert_false(axis_compat_poll(fx->compat, TRUE, NULL, &summary, &error));
	g_clear_error(&error);

	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_state s", "to_jsonb(s)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_session s", "to_jsonb(s)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "compat_device d", "to_jsonb(d)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
	g_assert_cmpint(axis_test_compat_count(&fx->tc, "job j", "to_jsonb(j)::text LIKE '%" AXIS_TEST_COMPAT_TOKEN "%'"), ==, 0);
	/* Pages that show the failure. */
	reply = get_as(fx, fx->admin, "/compat");
	assert_contains(reply.body, "The last look failed (3 in a row)");
	axis_test_reply_clear(&reply);
	assert_token_in_no_page(fx);
	/* The token is only ever set, never shown, on the Settings page. */
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(reply.body, "Media server token");
	assert_lacks(reply.body, AXIS_TEST_COMPAT_TOKEN);
	axis_test_reply_clear(&reply);
}

#define ADD(name, func) g_test_add("/web/compat/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("access", test_only_administrators_see_the_pages_and_every_post_needs_the_token);
	ADD("pages", test_the_pages_show_what_was_collected);
	ADD("escaping", test_names_from_the_server_are_escaped_and_never_part_of_a_link);
	ADD("verdicts", test_the_verdicts_page_names_the_devices_behind_each_verdict);
	ADD("savings", test_the_savings_pages_show_verdicts_without_devices_or_accounts);
	ADD("module-off", test_the_module_off_removes_the_pages_and_the_verdicts);
	ADD("connection-test", test_the_connection_test_page);
	ADD("actions", test_the_actions_queue_jobs_change_the_map_and_pin_devices);
	ADD("rescans-again", test_rescan_requests_can_be_asked_again);
	ADD("back-to-place", test_back_to_place);
	ADD("token", test_the_token_is_nowhere_after_a_full_run_including_failures);
	return g_test_run();
}
