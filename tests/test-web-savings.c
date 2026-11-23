/*
 * test-web-savings.c - the Savings pages: the overview comparing targets,
 * a target's page, the calibration page, figures that link to lists of the
 * same length, the uncalibrated notice, names that are escaped, the
 * administrator's controls with their CSRF token, the file page's "If
 * converted" card, and the module switched off
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell, storage, library, probe, browser, analysis and estimate
 * modules on an ephemeral loopback port, as in test-web-analysis.c, with
 * the library of tests/axis-test-savings.h.
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
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
	AxisWebProbe       *probe_ui;
	AxisWebBrowser     *browser_ui;
	AxisWebAnalysis    *analysis_ui;
	AxisWebSavings     *savings_ui;
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
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);
	fx->browser_ui = axis_web_browser_new(fx->library, fx->probe, fx->storage, fx->hub);
	fx->analysis_ui = axis_web_analysis_new(fx->analysis);
	fx->savings_ui = axis_web_savings_new(fx->estimator);
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
	axis_estimator_unref(fx->estimator);
	axis_analysis_unref(fx->analysis);
	axis_probe_unref(fx->probe);
	axis_library_unref(fx->library);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
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
get_htmx(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	const gchar *extra[3];

	extra[0] = "HX-Request";
	extra[1] = "true";
	extra[2] = NULL;
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, extra, NULL);
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

static void
refresh(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_estimator_refresh(fx->estimator, "direct", 0, NULL, NULL, &error));
	g_assert_no_error(error);
}

static gint64
stated_count(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new(">([0-9]+) files?\\.<", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	g_autofree gchar *number = NULL;

	g_regex_match(regex, body, 0, &info);
	if (!g_match_info_matches(info))
		return -1;
	number = g_match_info_fetch(info, 1);
	return g_ascii_strtoll(number, NULL, 10);
}

/* Every figure of a page that links to the file list: the list it opens
 * states the number the figure shows. */
static guint
follow_figures(
	Fixture     *fx,
	const gchar *body
){
	g_autoptr(GRegex) regex = g_regex_new("<a href=\"(/library/files[^\"]*)\">([^<]*)</a>", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint followed = 0;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *raw = g_match_info_fetch(info, 1);
		g_autofree gchar *text = g_match_info_fetch(info, 2);

		if (g_ascii_isdigit(text[0])) {
			g_autoptr(GString) plain = g_string_new(raw);
			gint64 shown = g_ascii_strtoll(text, NULL, 10);
			AxisTestReply list;
			gint64 stated;

			g_string_replace(plain, "&amp;", "&", 0);
			list = get_as(fx, fx->viewer, plain->str);
			if (list.status != 200)
				g_error("%s: status %u", plain->str, list.status);
			stated = stated_count(list.body);
			if (stated != shown)
				g_error("%s: the figure says %s, the list says %" G_GINT64_FORMAT, plain->str, text, stated);
			axis_test_reply_clear(&list);
			followed++;
		}
		g_match_info_next(info, NULL);
	}
	return followed;
}

static const gchar *const pages[] = {
	"/savings", "/savings/target/space_saver", "/savings/target/balanced", "/savings/target/max_compat",
	"/savings/target/keep_lossless", "/savings/target/music_space_saver", "/savings/target/music_compat",
	"/savings/target/audiobooks", "/savings/calibration", NULL
};

/* ── Before any estimate ────────────────────────────────────────── */

static void
test_no_estimate_yet(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; pages[i] != NULL; i++) {
		page = get_as(fx, fx->viewer, pages[i]);
		g_assert_cmpuint(page.status, ==, 200);
		assert_contains(page.body, "No estimate yet");
		assert_lacks(page.body, "action=\"/savings/refresh\"");
		axis_test_reply_clear(&page);
	}
	page = get_as(fx, fx->admin, "/savings");
	assert_contains(page.body, "action=\"/savings/refresh\"");
	axis_test_reply_clear(&page);
}

/* ── The pages ──────────────────────────────────────────────────── */

/* Spec: each built-in target side by side with its saving and calibration
 * state; uncalibrated is stated; figures link to lists of their length. */
static void
test_overview(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	const AxisEstimateTarget *targets = axis_estimate_builtin_targets();
	guint i;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	page = get_as(fx, fx->viewer, "/savings");
	g_assert_cmpuint(page.status, ==, 200);
	for (i = 0; i < AXIS_ESTIMATE_N_TARGETS; i++) {
		g_autofree gchar *href = g_strdup_printf("href=\"/savings/target/%s\"", targets[i].id);

		assert_contains(page.body, href);
	}
	assert_contains(page.body, "Informed guesses only: these estimates come from how the encoders usually behave");
	assert_contains(page.body, ">Informed guess<");
	assert_contains(page.body, "Conversion profiles side by side");
	assert_contains(page.body, "about ");
	assert_contains(page.body, "<svg");
	assert_contains(page.body, "What these figures could not see");
	assert_lacks(page.body, "action=\"/savings/refresh\"");
	assert_csp_clean(page.body);
	assert_links_safe(page.body);
	g_assert_cmpuint(follow_figures(fx, page.body), >=, 2 * AXIS_ESTIMATE_N_TARGETS);
	axis_test_reply_clear(&page);
}

/* Spec (web-shell): Figures agree.  The dashboard's Savings card gives the
 * saving of the profile that would save most, as the overview lists it,
 * with its calibration and when the estimates were made. */
static void
test_dashboard_savings(Fixture *fx, gconstpointer data)
{
	AxisTestReply board;
	AxisTestReply page;
	const gchar *card;
	const gchar *start;
	const gchar *end;
	const gchar *open;
	const gchar *close;
	g_autofree gchar *saving = NULL;
	g_autofree gchar *head = NULL;
	g_autofree gchar *range = NULL;
	g_autofree gchar *cell = NULL;

	SKIP_WITHOUT_DB(fx);

	/* Before any estimate: the card says so. */
	board = get_as(fx, fx->viewer, "/");
	assert_contains(board.body, "id=\"dash-savings\"");
	assert_contains(board.body, "Nothing has been estimated yet");
	axis_test_reply_clear(&board);

	refresh(fx);
	board = get_as(fx, fx->viewer, "/");
	card = strstr(board.body, "id=\"dash-savings\"");
	g_assert_nonnull(card);
	assert_contains(card, "<a href=\"/savings\">Space savings</a>");
	assert_contains(card, "Estimates as of ");
	assert_contains(card, ">Informed guess<");
	assert_contains(card, "href=\"/savings/target/");
	start = strstr(card, "<span class=\"figure-value\">");
	g_assert_nonnull(start);
	start += strlen("<span class=\"figure-value\">");
	end = strstr(start, "</span>");
	g_assert_nonnull(end);
	saving = g_strndup(start, end - start);
	g_assert_true(g_str_has_prefix(saving, "about "));

	/* The overview lists the same saving: the estimate and its share, its
	 * range under it. */
	open = strstr(saving, " (");
	g_assert_nonnull(open);
	close = strchr(open, ')');
	g_assert_nonnull(close);
	head = g_strndup(saving, open - saving);
	range = g_strndup(open + 1, close - open);
	cell = g_strdup_printf("%s%s<span class=\"range-detail\">%s</span>", head, close + 1, range);
	page = get_as(fx, fx->viewer, "/savings");
	assert_contains(page.body, cell);
	axis_test_web_assert_tables(page.body);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&board);
}

static void
test_target_pages(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint followed = 0;
	guint i;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	for (i = 1; pages[i] != NULL; i++) {
		page = get_as(fx, fx->viewer, pages[i]);
		g_assert_cmpuint(page.status, ==, 200);
		assert_csp_clean(page.body);
		assert_links_safe(page.body);
		followed += follow_figures(fx, page.body);
		axis_test_reply_clear(&page);
	}
	g_assert_cmpuint(followed, >, 40);

	/* Spec: skip reasons with counts and sizes; projected free space per
	 * volume with a range; the files with the largest saving. */
	page = get_as(fx, fx->viewer, "/savings/target/space_saver");
	assert_contains(page.body, "Projected free space per disk");
	assert_contains(page.body, "Disk A");
	assert_contains(page.body, "Disk B");
	assert_contains(page.body, "Why files would be skipped");
	assert_contains(page.body, "Pinned");
	assert_contains(page.body, "Dolby Vision");
	assert_contains(page.body, "HDR uncertain");
	assert_contains(page.body, "Would not shrink enough");
	assert_contains(page.body, "outcome=pinned");
	assert_contains(page.body, "The files with the largest estimated saving");
	assert_contains(page.body, "Remux (2002).mkv");
	assert_contains(page.body, "Saving by resolution class");
	assert_contains(page.body, "Informed guess");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->viewer, "/savings/target/music_space_saver");
	assert_contains(page.body, "Album not eligible as a whole");
	assert_contains(page.body, "Lossy source");
	assert_contains(page.body, "Saving by current audio codec");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->viewer, "/savings/target/nosuch");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
}

/* A volume that is away is marked, its last known figures kept. */
static void
test_offline_marked(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_library_unplug(&fx->view, fx->s.disk_b, "away-b");
	axis_storage_check_volume(fx->storage, fx->s.vol_b, NULL, NULL);
	refresh(fx);
	page = get_as(fx, fx->viewer, "/savings/target/space_saver");
	assert_contains(page.body, "Offline: last known");
	assert_contains(page.body, "On a disk that is offline");
	assert_contains(page.body, "on disks that are offline, counted as Axis last saw them");
	g_assert_cmpuint(follow_figures(fx, page.body), >, 5);
	axis_test_reply_clear(&page);
	axis_test_library_plug(&fx->view, fx->s.disk_b, "away-b");
}

static void
test_names_are_escaped(Fixture *fx, gconstpointer data)
{
	static const AxisTestProbeStream big[] = {
		{ "video", "h264", 8000000, 1920, 1080, 24.0, "sdr", FALSE, 0, 0, 0, NULL, "progressive", NULL },
		{ "audio", "aac", 128000, 0, 0, 0, NULL, FALSE, 2, 48000, 0, "false", NULL, NULL },
		{ NULL, NULL, 0, 0, 0, 0, NULL, FALSE, 0, 0, 0, NULL, NULL, NULL }
	};
	const gchar *rel = "<img src=x onerror=alert(1)>/<b>Big (2000).mkv";
	AxisTestReply page;
	g_autofree gchar *sql = NULL;

	SKIP_WITHOUT_DB(fx);
	axis_test_scenario_put(fx->s.disk_a, "Films", rel, axis_test_savings_size(big, 1.0));
	axis_scan_result_free(axis_test_library_scan_expect(&fx->view, fx->s.films_a, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
	axis_test_savings_probe(&fx->view, axis_test_scenario_file(&fx->view, fx->s.films_a, rel), "matroska,webm", 1.0,
		big, "false");
	sql = g_strdup_printf("UPDATE library_root SET name = '<i>Films</i> & more' WHERE id = %" G_GINT64_FORMAT,
		fx->s.films_a);
	axis_test_db_exec(fx->db, sql);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/savings/target/space_saver");
	g_assert_cmpuint(page.status, ==, 200);
	assert_lacks(page.body, "<img src=x");
	assert_lacks(page.body, "<b>Big");
	assert_lacks(page.body, "<i>Films</i>");
	assert_contains(page.body, "&lt;b&gt;Big (2000).mkv");
	assert_contains(page.body, "&lt;i&gt;Films&lt;/i&gt; &amp; more");
	assert_links_safe(page.body);
	assert_csp_clean(page.body);
	axis_test_reply_clear(&page);
}

/* ── The file page ──────────────────────────────────────────────── */

/* Spec: a probed video file's page lists each video target with its
 * decision and the estimated size after. */
static void
test_file_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *big = NULL;
	g_autofree gchar *mixed = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	big = g_strdup_printf("/library/file/%" G_GINT64_FORMAT,
		axis_test_scenario_file(&fx->view, fx->s.films_a, "Big (2001)/Big (2001).mkv"));
	page = get_as(fx, fx->viewer, big);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Space saved if converted");
	assert_contains(page.body, "Video: Space saver");
	assert_contains(page.body, "Video: Balanced");
	assert_contains(page.body, "Video: Maximum compatibility");
	assert_lacks(page.body, "Music: Space saver");
	assert_contains(page.body, "Would be converted");
	assert_contains(page.body, "about ");
	assert_contains(page.body, "Informed guess");
	assert_csp_clean(page.body);
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);
	/* Big is converted by every video profile now (Maximum compatibility's
	 * own threshold); Small, at 0.03 bits per pixel, is already efficient. */
	g_free(big);
	big = g_strdup_printf("/library/file/%" G_GINT64_FORMAT,
		axis_test_scenario_file(&fx->view, fx->s.films_a, "Small (2003)/Small (2003).mkv"));
	page = get_as(fx, fx->viewer, big);
	assert_contains(page.body, "Skipped: Already efficient");
	axis_test_reply_clear(&page);

	mixed = g_strdup_printf("/library/file/%" G_GINT64_FORMAT,
		axis_test_scenario_file(&fx->view, fx->s.music_a, "Band/Mixed/01 A.flac"));
	page = get_as(fx, fx->viewer, mixed);
	assert_contains(page.body, "Music: Space saver");
	assert_contains(page.body, ">Audiobooks</a>");
	assert_contains(page.body, "another file of its album: Lossy source");
	axis_test_reply_clear(&page);
}

/* ── Calibration and the measurement ────────────────────────────── */

static void
test_calibration_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	page = get_as(fx, fx->viewer, "/savings/calibration");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Accuracy of each conversion profile&#39;s estimates");
	assert_contains(page.body, "No measurement has been made yet.");
	assert_contains(page.body, "can take many minutes");
	assert_lacks(page.body, "action=\"/savings/measure\"");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/savings/calibration");
	assert_contains(page.body, "action=\"/savings/measure\"");
	assert_contains(page.body, "<option value=\"space_saver\"");
	axis_test_reply_clear(&page);

	/* One sample: Space saver is partly calibrated and says so, the music
	 * targets are still uncalibrated, and the overview does not call the
	 * whole of it uncalibrated. */
	{
		g_autoptr(GError) error = NULL;
		AxisEstimateSample sample;

		memset(&sample, 0, sizeof(sample));
		sample.target = "space_saver";
		sample.codec = AXIS_TCODEC_HEVC;
		sample.family = AXIS_ENCODER_SOFTWARE;
		sample.quality = 4;
		sample.part = "video";
		sample.source_codec = "h264";
		sample.width = 1920;
		sample.height = 1080;
		sample.frame_rate = 24.0;
		sample.source_bitrate = 8000000;
		sample.seconds = 60.0;
		sample.output_bitrate = 1000000;
		g_assert_true(axis_estimator_record_sample(fx->estimator, NULL, &sample, "measured", 0,
			axis_test_scenario_file(&fx->view, fx->s.films_a, "Big (2001)/Big (2001).mkv"), &error));
		g_assert_no_error(error);
	}
	refresh(fx);
	page = get_as(fx, fx->viewer, "/savings/target/space_saver");
	assert_contains(page.body, "Partly based on measurements: some of these estimates are still informed guesses");
	assert_contains(page.body, "Partly based on measurements (1 sample");
	assert_lacks(page.body, "Informed guesses only: these estimates");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/savings/target/music_space_saver");
	assert_contains(page.body, "Informed guesses only: these estimates come from how the encoders usually behave");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/savings");
	assert_contains(page.body, "Partly based on measurements: some of these estimates");
	assert_lacks(page.body, "Informed guesses only: these estimates");
	axis_test_reply_clear(&page);
}

/* Spec: the controls are an administrator's, each POST needs the token. */
static void
test_admin_controls(Fixture *fx, gconstpointer data)
{
	static const gchar *const posts[] = { "/savings/refresh", "/savings/measure", "/savings/measure/cancel", NULL };
	AxisTestReply reply;
	g_autofree gchar *count = NULL;
	guint i;

	SKIP_WITHOUT_DB(fx);
	for (i = 0; posts[i] != NULL; i++) {
		reply = post_as(fx, fx->viewer, posts[i], "target=space_saver");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = axis_test_web_request(fx->session, fx->port, "POST", posts[i], fx->admin, NULL, "target=space_saver");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind LIKE 'estimate.%'");
	g_assert_cmpstr(count, ==, "0");

	reply = post_as(fx, fx->admin, "/savings/refresh", "");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/savings?done=refresh-requested&at="));
	axis_test_reply_clear(&reply);
	g_clear_pointer(&count, g_free);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind = 'estimate.refresh'"
		" AND queue = 'maintenance'");
	g_assert_cmpstr(count, ==, "1");
	reply = get_as(fx, fx->admin, "/savings?done=refresh-requested");
	assert_contains(reply.body, "A refresh has been requested");
	assert_contains(reply.body, "hx-get=\"/savings/status\"");
	axis_test_reply_clear(&reply);
	reply = get_htmx(fx, fx->viewer, "/savings/status");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "<html");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/savings/status");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/savings/measure", "target=nosuch");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/savings/measure", "target=space_saver");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"),
		"/savings/calibration?done=measure-requested&at="));
	axis_test_reply_clear(&reply);
	g_clear_pointer(&count, g_free);
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind = 'estimate.sample' AND queue = 'sample'"
		" AND payload->>'target' = 'space_saver' AND payload->>'requested_by' = 'root'");
	g_assert_cmpstr(count, ==, "1");
	reply = get_as(fx, fx->admin, "/savings/calibration?done=measure-requested");
	assert_contains(reply.body, "A measurement is waiting for its turn");
	assert_contains(reply.body, "action=\"/savings/measure/cancel\"");
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/savings/measure/cancel", "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&count, g_free);
	count = axis_test_db_query(fx->db, "SELECT state FROM job WHERE kind = 'estimate.sample'");
	g_assert_cmpstr(count, ==, "cancelled");
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

/* Refresh, a profile's refresh, measure and cancel come back to where they
 * were made, with the notice there. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = post_as(fx, fx->admin, "/savings/refresh", "");
	assert_place(fx, &reply, "savings-refresh");
	reply = post_as(fx, fx->admin, "/savings/target/space_saver/refresh", "");
	assert_place(fx, &reply, "savings-refresh");
	reply = post_as(fx, fx->admin, "/savings/measure", "target=space_saver");
	assert_place(fx, &reply, "measure-samples");
	reply = post_as(fx, fx->admin, "/savings/measure/cancel", "");
	assert_place(fx, &reply, "measure-samples");
}

static void
test_bad_requests(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->viewer, "/savings?x=1");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	/* A profile's page shows the notice of its own refresh, no other. */
	page = get_as(fx, fx->viewer, "/savings/target/space_saver?done=measure-requested");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/savings/target/nosuch");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/savings/calibration?done=refresh-requested");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/library/files?outcome=eligible");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/library/files?target=nosuch");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, NULL, "/savings");
	g_assert_cmpuint(page.status, ==, 303);
	axis_test_reply_clear(&page);
}

/* The list says what the estimator decided, and keeps it in its form. */
static void
test_list_says_why(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->viewer, "/library/files?target=space_saver&outcome=pinned");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "decides with the conversion profile Space saver (Video): Pinned, as the files are now");
	assert_contains(page.body, "name=\"target\" value=\"space_saver\"");
	assert_contains(page.body, "name=\"outcome\" value=\"pinned\"");
	g_assert_cmpint(stated_count(page.body), ==, 2);
	axis_test_reply_clear(&page);
}

/* Switched off: no pages, no lists by estimate, no card. */
static void
test_module_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *big = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_ESTIMATE_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->viewer, "/savings");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/library/files?target=space_saver");
	g_assert_cmpuint(page.status, ==, 404);
	assert_contains(page.body, "needs the Space savings module");
	axis_test_reply_clear(&page);
	big = g_strdup_printf("/library/file/%" G_GINT64_FORMAT,
		axis_test_scenario_file(&fx->view, fx->s.films_a, "Big (2001)/Big (2001).mkv"));
	page = get_as(fx, fx->viewer, big);
	g_assert_cmpuint(page.status, ==, 200);
	assert_lacks(page.body, "Space saved if converted");
	axis_test_reply_clear(&page);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_ESTIMATE_MODULE_ID, TRUE, &error));
	page = get_as(fx, fx->viewer, big);
	assert_contains(page.body, "Space saved if converted");
	axis_test_reply_clear(&page);
}

#define ADD(name, func) g_test_add("/web/savings/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("no-estimate-yet", test_no_estimate_yet);
	ADD("overview", test_overview);
	ADD("dashboard", test_dashboard_savings);
	ADD("target-pages", test_target_pages);
	ADD("offline-marked", test_offline_marked);
	ADD("names-are-escaped", test_names_are_escaped);
	ADD("file-page", test_file_page);
	ADD("calibration-page", test_calibration_page);
	ADD("admin-controls", test_admin_controls);
	ADD("back-to-place", test_back_to_place);
	ADD("bad-requests", test_bad_requests);
	ADD("list-says-why", test_list_says_why);
	ADD("module-off", test_module_off);

	return g_test_run();
}
