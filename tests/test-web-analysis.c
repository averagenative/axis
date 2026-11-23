/*
 * test-web-analysis.c - the Analysis pages: the overview and the reports'
 * pages, the coverage they state, charts without inline style, figures that
 * link to lists of the same length, names that are escaped, and the
 * administrator's refresh
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell, storage, library, probe, browser and analysis modules on
 * an ephemeral loopback port, with sessions opened in an auth without a
 * database, as in test-web-browser.c, and the library of
 * tests/axis-test-analysis.h.
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
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "library/axis-analysis.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
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
	AxisAnalysis       *analysis;
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
	AxisWebProbe       *probe_ui;
	AxisWebBrowser     *browser_ui;
	AxisWebAnalysis    *analysis_ui;
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
	AxisTestLibrary     view;	/* the pieces above, as the scenario wants them */
	AxisTestScenario    s;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

/* GLib's sizes put a no-break space between the number and a unit such as
 * KiB (but not before "bytes"). */
#define NBSP "\xc2\xa0"

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
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);
	fx->browser_ui = axis_web_browser_new(fx->library, fx->probe, fx->storage, fx->hub);
	fx->analysis_ui = axis_web_analysis_new(fx->analysis);

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

	/* The library, built with the test support's pieces. */
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
	axis_test_scenario_build(&fx->view, &fx->s);
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
	axis_web_analysis_stop(fx->analysis_ui);
	axis_web_browser_stop(fx->browser_ui);
	/* The modules' stop hooks use their user data: before it is freed. */
	axis_module_registry_stop_all(fx->registry);
	axis_web_probe_stop(fx->probe_ui);
	axis_web_library_stop(fx->library_ui);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_analysis_free(fx->analysis_ui);
	axis_web_browser_free(fx->browser_ui);
	axis_web_probe_free(fx->probe_ui);
	axis_web_library_free(fx->library_ui);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_analysis_unref(fx->analysis);
	axis_probe_unref(fx->probe);
	axis_library_unref(fx->library);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_scenario_clear(&fx->s);
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
	AxisTestReply page = get_as(fx, cookie, "/analysis");
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

/* Every link on the page is a plain path made of safe characters: nothing
 * from a file name is in one. */
static void
assert_links_safe(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("href=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) allowed = g_regex_new("^[/#A-Za-z0-9_.?=&;%:+-]*$", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;

	g_assert_nonnull(regex);
	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *href = g_match_info_fetch(info, 1);

		if (!g_regex_match(allowed, href, 0, NULL))
			g_error("a link with something other than a path in it: %s", href);
		g_match_info_next(info, NULL);
	}
}

/* No inline style or script, in the markup or in an SVG. */
static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "style='");
	assert_lacks(body, "<style");
	/* The page's own scripts are files; none is written into it. */
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	assert_lacks(body, "onclick");
	assert_lacks(body, "onerror=\"");
	assert_lacks(body, "javascript:");
}

/* Every page a snapshot shows. */
static const gchar *const report_pages[] = {
	"/analysis", "/analysis/space", "/analysis/composition", "/analysis/largest", "/analysis/duplicates",
	"/analysis/split", "/analysis/leftovers", NULL
};

static gint64
refresh(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	g_assert_true(axis_analysis_refresh(fx->analysis, "direct", 0, NULL, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
execf(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static void
execf(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list args;

	va_start(args, format);
	sql = g_strdup_vprintf(format, args);
	va_end(args);
	axis_test_db_exec(fx->db, sql);
}

/* The count a list page states: "17 files." */
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

/*
 * Follows every figure of a page that links to the file list: the list it
 * opens states the number the figure shows.  Returns how many it followed.
 */
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
		g_autofree gchar *href = g_strdup(raw);

		if (g_ascii_isdigit(text[0])) {
			gint64 shown = g_ascii_strtoll(text, NULL, 10);
			AxisTestReply list;
			gint64 stated;

			/* HTML wrote the ampersands of the query as entities. */
			{
				g_autoptr(GString) plain = g_string_new(href);

				g_string_replace(plain, "&amp;", "&", 0);
				g_free(href);
				href = g_string_free(g_steal_pointer(&plain), FALSE);
			}
			list = get_as(fx, fx->viewer, href);
			if (list.status != 200)
				g_error("%s: status %u", href, list.status);
			stated = stated_count(list.body);
			if (stated != shown)
				g_error("%s: the figure says %s, the list says %" G_GINT64_FORMAT, href, text, stated);
			axis_test_reply_clear(&list);
			followed++;
		}
		g_match_info_next(info, NULL);
	}
	return followed;
}

/* The links of the charts' rows open lists too. */
static guint
follow_chart_rows(
	Fixture     *fx,
	const gchar *body
){
	g_autoptr(GRegex) regex = g_regex_new("<a class=\"chart-row\" href=\"(/library/files[^\"]*)\">", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint followed = 0;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *raw = g_match_info_fetch(info, 1);
		g_autoptr(GString) plain = g_string_new(raw);
		AxisTestReply list;

		g_string_replace(plain, "&amp;", "&", 0);
		list = get_as(fx, fx->viewer, plain->str);
		g_assert_cmpuint(list.status, ==, 200);
		g_assert_cmpint(stated_count(list.body), >=, 0);
		axis_test_reply_clear(&list);
		followed++;
		g_match_info_next(info, NULL);
	}
	return followed;
}

/* ── Before any report ──────────────────────────────────────────── */

static void
test_no_report_yet(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint i;

	SKIP_WITHOUT_DB(fx);

	for (i = 0; report_pages[i] != NULL; i++) {
		page = get_as(fx, fx->viewer, report_pages[i]);
		g_assert_cmpuint(page.status, ==, 200);
		assert_contains(page.body, "No report yet");
		assert_contains(page.body, "<nav aria-label=\"Disk usage reports\">");
		/* A viewer cannot ask for one. */
		assert_lacks(page.body, "Refresh the reports now");
		assert_csp_clean(page.body);
		axis_test_reply_clear(&page);
	}
	page = get_as(fx, fx->admin, "/analysis");
	assert_contains(page.body, "Refresh the reports now");
	axis_test_reply_clear(&page);
}

/* ── The reports ────────────────────────────────────────────────── */

/* Spec: Snapshot time shown; Following a figure; no inline style. */
static void
test_pages(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint i;
	guint followed = 0;
	guint rows = 0;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	for (i = 0; report_pages[i] != NULL; i++) {
		page = get_as(fx, fx->viewer, report_pages[i]);
		g_assert_cmpuint(page.status, ==, 200);
		/* When the figures were computed. */
		assert_contains(page.body, "Figures as of ");
		assert_contains(page.body, " UTC");
		assert_lacks(page.body, "No report yet");
		/* The navigation has Disk usage, and the page the reports. */
		assert_contains(page.body, "href=\"/analysis\">Disk usage</a></li>");
		assert_contains(page.body, "<nav aria-label=\"Disk usage reports\">");
		assert_csp_clean(page.body);
		assert_links_safe(page.body);
		/* A viewer sees no button to refresh. */
		assert_lacks(page.body, "Refresh the reports now");
		followed += follow_figures(fx, page.body);
		rows += follow_chart_rows(fx, page.body);
		axis_test_reply_clear(&page);
	}
	/* The figures of the pages were followed, not an empty set of them. */
	g_assert_cmpuint(followed, >, 60);
	g_assert_cmpuint(rows, >, 20);
}

/* The overview: totals, space by volume and category, growth, coverage. */
static void
test_overview(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	execf(fx, "INSERT INTO analysis_snapshot (taken_at, trigger, took_ms, paths, bytes, volumes_total, volumes_used,"
		" coverage, config) SELECT now() - make_interval(days => d), 'schedule', 0, 10 * d, 1000 * d, 100000, 1000 * d,"
		" '{}', '{}' FROM generate_series(1, 3) d");

	page = get_as(fx, fx->viewer, "/analysis");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>Disk usage</h1><p class=\"page-intro\">What is taking the space");
	assert_contains(page.body, "<dd><a href=\"/library/files\">32</a></dd>");
	assert_contains(page.body, "Disk A");
	assert_contains(page.body, "Disk B");
	assert_contains(page.body, "Online");
	assert_contains(page.body, "/library/files?volume=");
	assert_contains(page.body, "/library/files?type=movies");
	/* Bars are SVG: geometry in attributes, colours in classes, a link per row. */
	assert_contains(page.body, "<svg class=\"chart\" role=\"img\"");
	assert_contains(page.body, "viewBox=\"0 0 760 ");
	assert_contains(page.body, "<progress max=\"100\" value=\"");
	assert_contains(page.body, "<rect class=\"chart-bar\"");
	assert_contains(page.body, "<a class=\"chart-row\" href=\"/library/files?volume=");
	assert_contains(page.body, "<title>Library size by disk</title>");
	/* The tables are beside the charts; every one declares its kind and
	 * its columns' roles. */
	assert_contains(page.body, "<table class=\"data-table kind-analysis-volumes ");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), >=, 3);
	/* GLib writes a no-break space between a size and its unit. */
	assert_contains(page.body, "11.1" NBSP "KiB");
	/* Growth: a line and the dates. */
	assert_contains(page.body, "<polyline class=\"chart-line\"");
	assert_contains(page.body, "points=\"");
	assert_contains(page.body, "Library size over time");
	assert_contains(page.body, "<h2>Library size over time</h2>");
	/* Hard links are explained where they make the numbers differ. */
	assert_contains(page.body, "hard links of files that are counted already");
	/* What could not be seen. */
	assert_contains(page.body, "What these figures could not see");
	axis_test_reply_clear(&page);
}

/* The text between @before and the next "<" after it in @html. */
static gchar *
text_after(
	const gchar *html,
	const gchar *before
){
	const gchar *start = strstr(html, before);
	const gchar *end;

	g_assert_nonnull(start);
	start += strlen(before);
	end = strchr(start, '<');
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* Spec (web-shell): Figures agree.  The dashboard's collection is the
 * Analysis overview's totals. */
static void
test_dashboard_collection(Fixture *fx, gconstpointer data)
{
	AxisTestReply board;
	AxisTestReply page;
	g_autofree gchar *files = NULL;
	g_autofree gchar *size = NULL;
	g_autofree gchar *files_figure = NULL;
	g_autofree gchar *size_figure = NULL;

	SKIP_WITHOUT_DB(fx);

	/* No report yet: the card says so rather than show nothing. */
	board = get_as(fx, fx->viewer, "/");
	assert_contains(board.body, "The library has not been measured yet");
	axis_test_reply_clear(&board);

	refresh(fx);
	page = get_as(fx, fx->viewer, "/analysis");
	files = text_after(page.body, "<dt>Files</dt><dd><a href=\"/library/files\">");
	size = text_after(page.body, "<dt>Size</dt><dd>");
	axis_test_reply_clear(&page);

	board = get_as(fx, fx->viewer, "/");
	files_figure = g_strdup_printf("<span class=\"figure-value\">%s</span><span class=\"figure-label\">files</span>",
		files);
	size_figure = g_strdup_printf("<span class=\"figure-value\">%s</span>"
		"<span class=\"figure-label\">in the library</span>", size);
	assert_contains(board.body, files_figure);
	assert_contains(board.body, size_figure);
	assert_contains(board.body, "Library as of ");
	assert_contains(board.body, "<a href=\"/analysis\">Disk usage</a>");
	axis_test_reply_clear(&board);
}

/* Spec: Partial data is labelled (3.4: unprobed, failed, never scanned). */
static void
test_coverage_banner(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	axis_test_library_add_root(&fx->view, fx->s.vol_b, fx->s.disk_b, "Never", AXIS_MEDIA_OTHER);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis");
	assert_contains(page.body, "What these figures could not see");
	assert_contains(page.body, "Not read yet (video and audio): </span><a href=\"/library/files?probe=none\">5 files</a>");
	assert_contains(page.body, "Could not be read: </span><a href=\"/library/files?probe=failed\">1 file</a>");
	assert_contains(page.body, "HDR class uncertain: </span><a href=\"/library/files?hdr=uncertain\">1 file</a>");
	assert_contains(page.body, "Never completely scanned: </span><a href=\"/library\">1 library folder</a>");
	assert_contains(page.body, "(Never): what it holds may be missing from every figure");
	axis_test_reply_clear(&page);

	/* The reports that depend on the probe say so; the space report does
	 * not claim it. */
	page = get_as(fx, fx->viewer, "/analysis/composition");
	assert_contains(page.body, "left out of every figure that needs what is inside a file");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis/space");
	assert_contains(page.body, "Never completely scanned: </span>");
	assert_lacks(page.body, "Not read yet");
	axis_test_reply_clear(&page);

	/* Nothing to report: every probed. */
	execf(fx, "DELETE FROM library_file WHERE id NOT IN (SELECT file_id FROM media_info)"
		" AND kind IN ('video', 'audio')");
	execf(fx, "DELETE FROM library_file WHERE id IN (SELECT file_id FROM media_info WHERE status <> 'ok')");
	execf(fx, "UPDATE media_info SET hdr_uncertain = false");
	execf(fx, "DELETE FROM library_root WHERE name = 'Never'");
	refresh(fx);
	page = get_as(fx, fx->viewer, "/analysis/composition");
	assert_contains(page.body, "Nothing is left out of the figures on this page");
	assert_lacks(page.body, "Not read yet");
	axis_test_reply_clear(&page);
}

/* Spec: Offline volume (the pages mark it, the totals do not move). */
static void
test_offline_volume(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);
	axis_test_library_unplug(&fx->view, fx->s.disk_b, "away-b");
	g_assert_true(axis_storage_check_volume(fx->storage, fx->s.vol_b, NULL, &error));
	g_assert_no_error(error);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<dd><a href=\"/library/files\">32</a></dd>");
	assert_contains(page.body, "Offline: last known");
	assert_contains(page.body, "Disk offline: </span><a href=\"/library/files?volume=2\">8 files</a>");
	assert_contains(page.body, "(5.5" NBSP "KiB) on disk Disk B, which is Offline. They are counted as Axis last saw them");
	assert_contains(page.body, "totals do not change while the disk is offline");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis/space");
	assert_contains(page.body, "Disk offline: last known");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis/duplicates");
	assert_contains(page.body, "Disk offline: last known");
	axis_test_reply_clear(&page);
}

/* Spec: Likely duplicates; Hardlinks are not duplicates; labelled likely. */
static void
test_duplicates_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis/duplicates");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Likely the same content");
	assert_contains(page.body, "Likely the same title");
	assert_contains(page.body, "badge badge-note\">Likely</span>");
	assert_contains(page.body, "Nothing is changed");
	/* The evidence. */
	assert_contains(page.body, "of 777 bytes each, 133:20 min long, streams video:h264:1920:1080");
	assert_contains(page.body, "The same movie: &quot;dunmore&quot;, 2021");
	assert_contains(page.body, "The same episode: &quot;show x&quot;, 2010, season 1, episode 1");
	assert_contains(page.body, "The same track: &quot;track&quot; by artist one on album one");
	/* The copy that looks best is marked, the others' size is stated. */
	assert_contains(page.body, "Looks best");
	assert_contains(page.body, "The ones that do not look best occupy 500 bytes");
	/* Every copy shown was probed, with its real size. */
	assert_lacks(page.body, "<td class=\"col-wide\" data-label=\"What is in it\">Not probed</td>");
	assert_lacks(page.body, "<td class=\"num col-size\" data-label=\"Size\">0 bytes</td>");
	assert_contains(page.body, "<td class=\"col-wide\" data-label=\"What is in it\">");
	axis_test_web_assert_tables(page.body);
	assert_contains(page.body, "flac, lossless");
	assert_contains(page.body, "mp3, lossy");
	/* Near misses are not in it. */
	assert_lacks(page.body, "The same movie: &quot;hearth&quot;");
	assert_lacks(page.body, "total rewind");
	assert_lacks(page.body, "&quot;the mazework&quot;");
	assert_lacks(page.body, "unknown film");
	/* The hard-linked Hearth is nowhere in the lists of copies. */
	assert_lacks(page.body, "Hearth (1995) copy.mkv");
	assert_lacks(page.body, "Hearth.1995.1080p.mkv");
	axis_test_reply_clear(&page);
}

/* The other reports' contents. */
static void
test_report_contents(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis/space");
	assert_contains(page.body, "<h2>Space by disk</h2>");
	assert_contains(page.body, "<h2>Space by library folder</h2>");
	assert_contains(page.body, "<h2>Space by media type</h2>");
	assert_contains(page.body, "<h2>Space by kind of file</h2>");
	assert_contains(page.body, "Films");
	assert_contains(page.body, "Movies");
	assert_contains(page.body, "<th class=\"col-date\" scope=\"col\">Last complete scan</th>");
	axis_test_web_assert_tables(page.body);
	assert_lacks(page.body, "Never completely scanned");
	/* A file with several hard links on one volume is explained. */
	assert_contains(page.body, "counts its size once, in the library folder of the path Axis found first");
	axis_test_reply_clear(&page);
}

static void
test_composition_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis/composition");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Size by video codec");
	/* Counts carry the count role: right-aligned in figures of one width. */
	assert_contains(page.body, "<td class=\"num col-count\" data-label=\"Files\"><a href=\"/library/files?vcodec=h264\">"
		"17</a></td>");
	assert_contains(page.body, "<td class=\"num col-count\" data-label=\"Files\"><a href=\"/library/files?vcodec=hevc\">"
		"2</a></td>");
	axis_test_web_assert_tables(page.body);
	assert_contains(page.body, "href=\"/library/files?res=1080p\"");
	assert_contains(page.body, "href=\"/library/files?hdr=hdr10\"");
	assert_contains(page.body, "href=\"/library/files?container=matroska%2Cwebm\"");
	assert_contains(page.body, "href=\"/library/files?acodec=eac3\"");
	assert_contains(page.body, "Bit rate by resolution class");
	assert_contains(page.body, "Thresholds used for this report: h264: 0.300; hevc: 0.150");
	assert_contains(page.body, "The largest files that are large for their class");
	/* The large files link to the same files the figure counts. */
	assert_contains(page.body, "href=\"/library/files?vcodec=h264&amp;res=1080p&amp;min_bpp=300\"");
	assert_contains(page.body, "href=\"/library/files?vcodec=hevc&amp;res=2160p&amp;min_bpp=150\"");
	/* The histograms. */
	assert_contains(page.body, "1080p h264: files by bits per pixel");
	/* The longest bar of a chart is full length, the others in proportion. */
	assert_contains(page.body, "<rect class=\"chart-bar\" width=\"320\"");
	assert_contains(page.body, "0.050 to below 0.100");
	axis_test_reply_clear(&page);
}

static void
test_other_pages(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	g_autofree gchar *folder = NULL;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis/largest");
	assert_contains(page.body, "Largest files");
	assert_contains(page.body, "Largest folders");
	assert_contains(page.body, "Alder (1979) 2160p.mkv");
	assert_contains(page.body, "Hard-linked: counted once");
	assert_contains(page.body, "href=\"/library/files?sort=size\"");
	assert_contains(page.body, "/library/folder/");
	assert_contains(page.body, "/library/file/");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->viewer, "/analysis/split");
	assert_contains(page.body, "Show X (2010)");
	assert_contains(page.body, "on 2 disks, 1.4" NBSP "KiB in all; 610 bytes are not on the disk that holds the most");
	assert_contains(page.body, "Artist One");
	assert_contains(page.body, "/library/files?under=");
	assert_lacks(page.body, "Show Y");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->viewer, "/analysis/leftovers");
	assert_contains(page.body, "Samples");
	assert_contains(page.body, "Subtitles without a video");
	assert_contains(page.body, "alder-sample.mkv");
	assert_contains(page.body, "Other Movie.srt");
	assert_contains(page.body, "Thumbs.db");
	assert_contains(page.body, "href=\"/library/files?leftover=any&amp;sort=size\"");
	assert_contains(page.body, "Empty folders cannot be listed");
	/* The subtitle beside its video is not a leftover. */
	assert_lacks(page.body, "2160p.en.srt");
	axis_test_reply_clear(&page);
}

/* ── Names are text ─────────────────────────────────────────────── */

static void
test_names_are_escaped(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint i;

	SKIP_WITHOUT_DB(fx);
	axis_test_scenario_put(fx->s.disk_a, "Films", "<img src=x onerror=alert(1)>/<b>Big (2000).mkv", 9000);
	axis_test_scenario_put(fx->s.disk_a, "Films", "<img src=x onerror=alert(1)>/Big (2000).en.srt", 5);
	axis_scan_result_free(axis_test_library_scan_expect(&fx->view, fx->s.films_a, AXIS_SCAN_COMPLETE,
		AXIS_SCAN_REASON_NONE));
	execf(fx, "UPDATE library_root SET name = '<i>Films</i> & more' WHERE id = %" G_GINT64_FORMAT, fx->s.films_a);
	refresh(fx);

	for (i = 0; report_pages[i] != NULL; i++) {
		page = get_as(fx, fx->viewer, report_pages[i]);
		g_assert_cmpuint(page.status, ==, 200);
		assert_lacks(page.body, "<img src=x");
		assert_lacks(page.body, "<b>Big");
		assert_lacks(page.body, "<i>Films</i>");
		assert_csp_clean(page.body);
		assert_links_safe(page.body);
		axis_test_reply_clear(&page);
	}
	page = get_as(fx, fx->viewer, "/analysis/largest");
	assert_contains(page.body, "&lt;b&gt;Big (2000).mkv");
	assert_contains(page.body, "&lt;img src=x onerror=alert(1)&gt;");
	assert_contains(page.body, "&lt;i&gt;Films&lt;/i&gt; &amp; more");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis/leftovers");
	assert_contains(page.body, "&lt;img src=x onerror=alert(1)&gt;");
	axis_test_reply_clear(&page);
}

/* ── Queries and access ─────────────────────────────────────────── */

static void
test_bad_requests(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/analysis?x=1");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis/space?done=refresh-requested");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis?done=other");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis?done=refresh-requested");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "A refresh has been requested");
	axis_test_reply_clear(&page);
	page = get_htmx(fx, fx->viewer, "/analysis/status?x=1");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/analysis/nothing");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
}

static void
test_login_required(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint i;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	for (i = 0; report_pages[i] != NULL; i++) {
		page = get_as(fx, NULL, report_pages[i]);
		g_assert_cmpuint(page.status, ==, 303);
		assert_contains(axis_test_reply_header(&page, "Location"), "/login");
		axis_test_reply_clear(&page);
	}
}

/* The refresh comes back to the header it was made from, with the notice in
 * it. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	AxisTestReply page;
	const gchar *location;
	g_autofree gchar *path = NULL;
	const gchar *element;
	const gchar *notice;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	reply = post_as(fx, fx->admin, "/analysis/refresh", "");
	g_assert_cmpuint(reply.status, ==, 303);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_nonnull(strstr(location, "at=analysis-refresh"));
	g_assert_true(g_str_has_suffix(location, "#analysis-refresh"));
	path = g_strndup(location, strrchr(location, '#') - location);
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, "id=\"analysis-refresh\"");
	notice = strstr(page.body, "notice-here");
	g_assert_nonnull(element);
	g_assert_nonnull(notice);
	g_assert_true(element < notice);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&reply);
}

/* Spec: refresh now is an administrator's, with the CSRF token; the page
 * shows the refreshing state. */
static void
test_refresh_now(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	/* Not for a viewer, and not without the token. */
	reply = post_as(fx, fx->viewer, "/analysis/refresh", "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/analysis/refresh", fx->admin, NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *none = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind = 'analysis.refresh'");

		g_assert_cmpstr(none, ==, "0");
	}

	/* An administrator, twice: one job. */
	reply = post_as(fx, fx->admin, "/analysis/refresh", "");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/analysis?done=refresh-requested&at="));
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/analysis/refresh", "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *count = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind = 'analysis.refresh'");

		g_assert_cmpstr(count, ==, "1");
	}

	/* The page shows that it is refreshing, and asks again until it is done. */
	reply = get_as(fx, fx->admin, "/analysis");
	assert_contains(reply.body, "id=\"analysis-status\"");
	assert_contains(reply.body, "hx-get=\"/analysis/status\"");
	assert_contains(reply.body, "hx-trigger=\"every 2s\"");
	assert_contains(reply.body, "A refresh is waiting for its turn");
	axis_test_reply_clear(&reply);

	/* The status alone is for htmx. */
	reply = get_as(fx, fx->admin, "/analysis/status");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	reply = get_htmx(fx, fx->viewer, "/analysis/status");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "hx-trigger=\"every 2s\"");
	assert_lacks(reply.body, "<html");
	g_assert_null(axis_test_reply_header(&reply, "HX-Refresh"));
	axis_test_reply_clear(&reply);

	/* When the job is done the answer reloads the page. */
	axis_test_db_exec(fx->db, "DELETE FROM job");
	reply = get_htmx(fx, fx->viewer, "/analysis/status");
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(axis_test_reply_header(&reply, "HX-Refresh"), ==, "true");
	assert_lacks(reply.body, "hx-trigger");
	axis_test_reply_clear(&reply);
}

/* A list opened from a figure says what narrows it beyond its form, and
 * keeps that when the form is sent. */
static void
test_list_says_why(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	refresh(fx);

	page = get_as(fx, fx->viewer, "/library/files?volume=1&type=movies&min_bpp=300&ids=5%2C13&leftover=junk&changed=no");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Also narrowed by: the disk with number 1; library folders that hold movies; 2 chosen files; "
		"video at least 0.300 bits per pixel; the leftover rule junk; not changed since Axis last read them.");
	assert_contains(page.body, "<input type=\"hidden\" name=\"volume\" value=\"1\"");
	assert_contains(page.body, "name=\"type\" value=\"movies\"");
	assert_contains(page.body, "name=\"min_bpp\" value=\"300\"");
	assert_contains(page.body, "name=\"ids\" value=\"5,13\"");
	assert_contains(page.body, "name=\"leftover\" value=\"junk\"");
	assert_contains(page.body, "name=\"changed\" value=\"no\"");
	assert_csp_clean(page.body);
	axis_test_reply_clear(&page);

	/* A plain list says nothing of the kind. */
	page = get_as(fx, fx->viewer, "/library/files?kind=video");
	assert_lacks(page.body, "Also narrowed by");
	axis_test_reply_clear(&page);

	/* A folder's files, as the split report links to them. */
	{
		g_autofree gchar *dir = axis_test_db_query(fx->db,
			"SELECT id FROM library_dir WHERE path = 'Show X (2010)' ORDER BY id LIMIT 1");
		g_autofree gchar *path = NULL;

		g_assert_nonnull(dir);
		path = g_strdup_printf("/library/files?under=%s", dir);
		page = get_as(fx, fx->viewer, path);
		g_assert_cmpuint(page.status, ==, 200);
		assert_contains(page.body, "the folder with number ");
		axis_test_reply_clear(&page);
	}
}

/* Settings are the administrators' Settings pages. */
static void
test_settings_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->admin, "/admin/settings");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "analysis.large_bpp_h264");
	assert_contains(page.body, "Large for its class: H.264");
	assert_contains(page.body, "Keep the last report of each day for");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/admin/settings");
	g_assert_cmpuint(page.status, ==, 403);
	axis_test_reply_clear(&page);
}

#define ADD(name, func) g_test_add("/web/analysis/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("no-report-yet", test_no_report_yet);
	ADD("pages", test_pages);
	ADD("overview", test_overview);
	ADD("dashboard-collection", test_dashboard_collection);
	ADD("coverage-banner", test_coverage_banner);
	ADD("offline-volume", test_offline_volume);
	ADD("duplicates", test_duplicates_page);
	ADD("space", test_report_contents);
	ADD("composition", test_composition_page);
	ADD("other-pages", test_other_pages);
	ADD("names-are-escaped", test_names_are_escaped);
	ADD("bad-requests", test_bad_requests);
	ADD("login-required", test_login_required);
	ADD("refresh-now", test_refresh_now);
	ADD("back-to-place", test_back_to_place);
	ADD("list-says-why", test_list_says_why);
	ADD("settings", test_settings_page);

	return g_test_run();
}
