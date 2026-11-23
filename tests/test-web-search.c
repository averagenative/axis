/*
 * test-web-search.c - the title search and the title pages, with the real
 * shell on an ephemeral loopback port, the fixture provider and its fake
 * servers (never TMDb), a real database and a data directory of the test's
 * own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (title-search): Search; Without script; Nothing found; Slow
 * image host; Provider down; No provider; Series; Markup in a title; No
 * module; Too many.  Also: only the results region is replaced for htmx,
 * no primary tab is marked on the results or a title's page (and the tabs
 * come along when the box replaces the content), images come only from the
 * daemon's own address, external links carry
 * noreferrer noopener, the number in /title/<id> is checked strictly, every
 * page needs a session, and what was typed is not logged.
 *
 * With AXIS_TEST_SEARCH_PAGES set to a directory, the pages are also saved
 * there (results-*.html, film.html, series.html), for looking at in a
 * browser.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-version.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "meta/axis-meta.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-settings.h"

#define KEY AXIS_TEST_META_KEY

#define WITH_ACTIONS 1
#define WITHOUT_KEY  2

typedef struct
{
	AxisTestDb          *db;
	AxisDbPool          *pool;
	AxisSettings        *settings;
	AxisWebSettings     *web_settings;
	gchar               *dir;
	AxisDataDir         *data;
	AxisTestMetaFixture *fa;
	AxisMeta            *meta;
	AxisWebMeta         *meta_ui;
	AxisModuleRegistry  *registry;
	HtmxServer          *server;
	AxisWebAuth         *auth;
	AxisWeb             *web;
	SoupSession         *session;
	guint16              port;
	gchar               *admin;
	gchar               *viewer;
} Fixture;

/* ── A module that offers actions, for the action slot ──────────── */

static struct
{
	guint    collected;
	gboolean collect_on_main_loop;
} actions_seen;

static gpointer
actions_collect(
	const GPtrArray    *titles,
	AxisWebActionPlace  place,
	gpointer            user_data
){
	actions_seen.collected += titles->len;
	actions_seen.collect_on_main_loop = g_main_context_is_owner(g_main_context_default());
	return g_strdup_printf("%u titles", titles->len);
}

static HtmxNode *
actions_render(
	HtmxRequest         *request,
	AxisWebActionPlace   place,
	const AxisMetaTitle *title,
	gpointer             figures,
	gpointer             user_data
){
	HtmxSpan *piece;
	g_autofree gchar *text = NULL;

	/* Nothing below the header and nothing above it: this module has no
	 * cards or notices of its own, and nothing to add from a list. */
	if (place == AXIS_WEB_ACTION_DETAIL || place == AXIS_WEB_ACTION_NOTICE || place == AXIS_WEB_ACTION_BAR)
		return NULL;
	piece = htmx_span_new();
	text = g_strdup_printf("%s-action of title %" G_GINT64_FORMAT " (%s)",
		place == AXIS_WEB_ACTION_PAGE ? "page" : "card", title->id, (const gchar *)figures);
	htmx_element_add_class(HTMX_ELEMENT(piece), "test-action");
	htmx_node_set_text_content(HTMX_NODE(piece), text);
	return HTMX_NODE(piece);
}

static void
actions_routes(
	AxisWeb  *web,
	gpointer  user_data
){
	axis_web_add_title_actions(web, actions_collect, actions_render, g_free, NULL);
}

static const AxisModuleInfo actions_module = {
	.id = "titletest",
	.name = "Title test",
	.register_routes = actions_routes,
	.description = "Offers an action for every title, for the tests of the action slot."
};

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	guint flags = GPOINTER_TO_UINT(data);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
	memset(&actions_seen, 0, sizeof(actions_seen));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	path = g_build_filename(fx->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	if ((flags & WITH_ACTIONS) != 0)
		g_assert_true(axis_module_registry_register(fx->registry, &actions_module, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);

	if ((flags & WITHOUT_KEY) == 0) {
		g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", KEY, &error));
		g_assert_no_error(error);
	}
	axis_test_meta_fixture_standard(fx->fa);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_storage_dir_free(fx->dir);
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_meta_stop(fx->meta_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_meta_free(fx->meta_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_meta_unref(fx->meta);
	axis_test_meta_fixture_free(fx->fa);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

/* As htmx sends it: with HX-Request, and the element it replaces. */
static AxisTestReply
get_htmx(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *target
){
	const gchar *extra[5];

	extra[0] = "HX-Request";
	extra[1] = "true";
	extra[2] = "HX-Target";
	extra[3] = target;
	extra[4] = NULL;
	if (target == NULL)
		extra[2] = NULL;
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, extra, NULL);
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

static guint
count_of(
	const gchar *haystack,
	const gchar *needle
){
	guint n = 0;
	const gchar *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/* Every image the page shows is the daemon's own. */
static guint
assert_images_local(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("<img [^>]*src=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) local = g_regex_new("^/art/[0-9a-f]{64}$", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint n = 0;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *src = g_match_info_fetch(info, 1);

		if (!g_regex_match(local, src, 0, NULL))
			g_error("an image that is not the daemon's own: %s", src);
		n++;
		g_match_info_next(info, NULL);
	}
	return n;
}

/* Every link that leaves for another site opens elsewhere and sends nothing
 * along, and its address is one built from a valid identifier. */
static guint
assert_external_links(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("<a [^>]*href=\"(https?://[^\"]*)\"[^>]*>", 0, 0, NULL);
	g_autoptr(GRegex) known = g_regex_new("^https://(www\\.imdb\\.com/title/tt[0-9]{7,10}/|"
		"www\\.themoviedb\\.org/(movie|tv)/[1-9][0-9]{0,11}|thetvdb\\.com/dereferrer/(series|movie)/[1-9][0-9]{0,11})$",
		0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint n = 0;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *tag = g_match_info_fetch(info, 0);
		g_autofree gchar *href = g_match_info_fetch(info, 1);

		assert_contains(tag, "rel=\"noreferrer noopener\"");
		assert_contains(tag, "target=\"_blank\"");
		/* The footer's link to the source is the build's own constant, on
		 * every page; it is not one of the title's links and is not counted. */
		if (strcmp(href, AXIS_SOURCE_URL) == 0) {
			g_match_info_next(info, NULL);
			continue;
		}
		if (!g_regex_match(known, href, 0, NULL))
			g_error("an external link that is not built from an identifier: %s", href);
		n++;
		g_match_info_next(info, NULL);
	}
	return n;
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);
	g_autoptr(GRegex) script_url = g_regex_new("(href|src|action)=\"\\s*javascript:", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GRegex) handler = g_regex_new("<[a-zA-Z][^>]*\\son[a-z]+=", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	g_assert_false(g_regex_match(script_url, body, 0, NULL));
	/* No element carries an event handler. */
	g_assert_false(g_regex_match(handler, body, 0, NULL));
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_SEARCH_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->fa->api, path, 200, "application/json", body, NULL);
}

static gint64
title_id(
	Fixture     *fx,
	const gchar *provider_id
){
	g_autofree gchar *sql = g_strdup_printf("SELECT id FROM meta_title WHERE provider = 'fixture' AND "
		"provider_id = '%s'", provider_id);
	g_autofree gchar *id = axis_test_db_query(fx->db, sql);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

typedef struct
{
	Fixture *fx;
	guint    n;
} Kept;

static gboolean
artwork_kept(gpointer data)
{
	const Kept *kept = data;
	g_autofree gchar *n = axis_test_db_query(kept->fx->db, "SELECT count(*) FROM meta_artwork WHERE sha256 IS NOT NULL");
	g_autofree gchar *want = g_strdup_printf("%u", kept->n);

	return g_strcmp0(n, want) == 0;
}

/* Waits until @n images are kept (the background fetches have finished). */
static gboolean
wait_for_artwork(
	Fixture *fx,
	guint    n
){
	Kept kept = { fx, n };

	return axis_test_pump_until(artwork_kept, &kept, AXIS_TEST_WAIT_MS);
}

/* ── Search ─────────────────────────────────────────────────────── */

/* Spec: Search; Without script */
static void
test_search_without_script(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->viewer, "/search?q=mazework");
	g_autoptr(GRegex) counts = g_regex_new("tab-count\">([0-9]+)<", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	g_autofree gchar *all = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *series = NULL;
	gint64 id;
	g_autofree gchar *link = NULL;

	/* A plain request: the whole document, from any logged-in user. */
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<!DOCTYPE html>");
	assert_contains(page.body, "<h1>Search movies and TV</h1>");
	assert_contains(page.body, "The Mazework");
	assert_contains(page.body, "The Mazework Reloaded");
	assert_contains(page.body, "The Mazework Revolutions");
	assert_contains(page.body, "Fixture Street");
	assert_contains(page.body, "A cartographer learns that the maze she maps redraws itself");
	assert_contains(page.body, "Movie, 1999");
	assert_contains(page.body, "TV series, 2021");
	assert_lacks(page.body, "A series among movies is left out");
	/* Films 3, series 1: All is their sum. */
	g_regex_match(counts, page.body, 0, &info);
	g_assert_true(g_match_info_matches(info));
	all = g_match_info_fetch(info, 1);
	g_match_info_next(info, NULL);
	films = g_match_info_fetch(info, 1);
	g_match_info_next(info, NULL);
	series = g_match_info_fetch(info, 1);
	g_assert_cmpstr(all, ==, "4");
	g_assert_cmpstr(films, ==, "3");
	g_assert_cmpstr(series, ==, "1");
	assert_contains(page.body, "aria-current=\"page\"");
	/* The search box is a plain form, with the query in it. */
	assert_contains(page.body, "role=\"search\"");
	assert_contains(page.body, "action=\"/search\"");
	assert_contains(page.body, "method=\"get\"");
	assert_contains(page.body, "name=\"q\"");
	assert_contains(page.body, "value=\"mazework\"");
	/* Each card links to its title's page, by the store's number, with the
	 * results to come back to (add-activity-feedback). */
	id = title_id(fx, "101");
	link = g_strdup_printf("href=\"/title/%" G_GINT64_FORMAT "?from=%%2Fsearch%%3Fq%%3D", id);
	assert_contains(page.body, link);
	assert_contains(page.body, "Fixture: Test data from the fixture provider");
	assert_csp_clean(page.body);
	assert_lacks(page.body, KEY);
	/* Nothing registered an action: no area for one. */
	assert_lacks(page.body, "card-actions");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 0);
	axis_test_reply_clear(&page);
}

/* Spec: only the results region is replaced (by htmx), and the box is
 * enhanced to ask for it. */
static void
test_htmx_replaces_only_the_region(Fixture *fx, gconstpointer data)
{
	AxisTestReply plain = get_as(fx, fx->viewer, "/");
	AxisTestReply region = get_htmx(fx, fx->viewer, "/search?q=mazework&kind=film", "search-results");
	AxisTestReply content = get_htmx(fx, fx->viewer, "/search?q=mazework", "content");
	AxisTestReply other = get_as(fx, fx->viewer, "/search?q=mazework&kind=film");

	/* The box is on every page, and htmx enhances it. */
	assert_contains(plain.body, "role=\"search\"");
	assert_contains(plain.body, "hx-get=\"/search\"");
	assert_contains(plain.body, "delay:300ms");
	assert_contains(plain.body, "hx-target=\"#content\"");
	assert_contains(plain.body, "hx-push-url=\"true\"");
	assert_contains(plain.body, "<form class=\"search-box\"");

	/* A tab or "more": the region and a title, nothing of the page around. */
	g_assert_cmpuint(region.status, ==, 200);
	assert_lacks(region.body, "<!DOCTYPE");
	assert_lacks(region.body, "<header");
	assert_lacks(region.body, "<h1>");
	assert_contains(region.body, "<title>");
	assert_contains(region.body, "id=\"search-results\"");
	assert_contains(region.body, "The Mazework Reloaded");
	assert_lacks(region.body, "Fixture Street");
	assert_contains(axis_test_reply_header(&region, "Vary"), "HX-Target");
	/* The tabs and "more" replace the region and only it. */
	assert_contains(region.body, "hx-target=\"#search-results\"");
	assert_contains(region.body, "hx-swap=\"outerHTML\"");

	/* The box (target: the content): the heading too, but no header. */
	g_assert_cmpuint(content.status, ==, 200);
	assert_lacks(content.body, "<!DOCTYPE");
	assert_lacks(content.body, "<header");
	assert_contains(content.body, "<h1>Search movies and TV</h1>");
	assert_contains(content.body, "id=\"search-results\"");

	/* The same address without htmx is the whole page. */
	assert_contains(other.body, "<!DOCTYPE html>");
	assert_contains(other.body, "id=\"search-results\"");
	g_assert_cmpstr(axis_test_reply_header(&other, "Vary"), !=, NULL);

	axis_test_reply_clear(&plain);
	axis_test_reply_clear(&region);
	axis_test_reply_clear(&content);
	axis_test_reply_clear(&other);
}

/*
 * The labels of the marked tabs in @body's primary tabs (nav#primary-nav:
 * with @oob the one sent out of band, otherwise the header's), "|"-joined;
 * "" when none is marked.  A tab is marked by its class or its aria-current,
 * and either counts.
 */
static gchar *
marked_tabs(
	const gchar *body,
	gboolean     oob
){
	g_autoptr(GRegex) navs = g_regex_new("<nav([^>]*)>(.*?)</nav>", G_REGEX_DOTALL, 0, NULL);
	g_autoptr(GRegex) links = g_regex_new("<a([^>]*)>([^<]*)</a>", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	g_autoptr(GMatchInfo) each = NULL;
	g_autofree gchar *tabs = NULL;
	GString *marked;

	g_regex_match(navs, body, 0, &info);
	while (tabs == NULL && g_match_info_matches(info)) {
		g_autofree gchar *attributes = g_match_info_fetch(info, 1);

		if (strstr(attributes, "id=\"primary-nav\"") != NULL &&
		    (strstr(attributes, "hx-swap-oob=\"true\"") != NULL) == oob)
			tabs = g_match_info_fetch(info, 2);
		g_match_info_next(info, NULL);
	}
	if (tabs == NULL)
		g_error("no %stabs in:\n%s", oob ? "out-of-band " : "", body);

	marked = g_string_new(NULL);
	g_regex_match(links, tabs, 0, &each);
	while (g_match_info_matches(each)) {
		g_autofree gchar *attributes = g_match_info_fetch(each, 1);
		g_autofree gchar *label = g_match_info_fetch(each, 2);

		if (strstr(attributes, "current") != NULL)
			g_string_append_printf(marked, "%s%s", marked->len > 0 ? "|" : "", label);
		g_match_info_next(each, NULL);
	}
	return g_string_free(marked, FALSE);
}

/* The aria-current of the first link to @href in @body ("" when it has
 * none); there must be such a link. */
static gchar *
link_current(
	const gchar *body,
	const gchar *href
){
	g_autofree gchar *quoted = g_regex_escape_string(href, -1);
	g_autofree gchar *pattern = g_strdup_printf("<a\\s[^>]*href=\"%s\"[^>]*>", quoted);
	g_autoptr(GRegex) link = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GRegex) current = g_regex_new("aria-current=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	g_autoptr(GMatchInfo) found = NULL;
	g_autofree gchar *tag = NULL;

	if (!g_regex_match(link, body, 0, &info))
		g_error("no link to %s in:\n%s", href, body);
	tag = g_match_info_fetch(info, 0);
	if (!g_regex_match(current, tag, 0, &found))
		return g_strdup("");
	return g_match_info_fetch(found, 1);
}

/* Whether the rows below the tabs in @body (div#nav-rows) hold nothing; the
 * one in @body must be out of band exactly when @oob is set. */
static gboolean
rows_are_empty(
	const gchar *body,
	gboolean     oob
){
	const gchar *at = strstr(body, "id=\"nav-rows\"");
	const gchar *start;
	const gchar *end;
	g_autofree gchar *tag = NULL;

	if (at == NULL)
		g_error("no rows below the tabs in:\n%s", body);
	g_assert_null(strstr(at + 1, "id=\"nav-rows\""));
	for (start = at; start > body && start[0] != '<'; start--)
		;
	end = strchr(at, '>');
	g_assert_nonnull(end);
	tag = g_strndup(start, end - start + 1);
	g_assert_true(g_str_has_prefix(tag, "<div"));
	g_assert_cmpint(strstr(tag, "hx-swap-oob=\"true\"") != NULL, ==, oob);
	return g_str_has_prefix(end + 1, "</div>");
}

/*
 * The tabs follow the page that is open.  The Dashboard tab is marked on the
 * dashboard only: the results and a title's page are in no tab, and mark
 * none, as a whole page.  Typed in the box on any page, the search replaces
 * the content and the address but not the header; so the tabs and the rows
 * below come along out of band, marked for the results (none), and the box
 * does not (it keeps its focus).  A tab or "more" replaces the region only:
 * no tabs with it.  A content swap to a page in a tab marks that tab.
 */
static void
test_tabs_follow_the_page(Fixture *fx, gconstpointer data)
{
	const gchar *who[2];
	g_autofree gchar *title = NULL;
	AxisTestReply reply;
	guint i;

	who[0] = fx->admin;
	who[1] = fx->viewer;

	/* The dashboard marks itself, and has no rows below. */
	for (i = 0; i < G_N_ELEMENTS(who); i++) {
		g_autofree gchar *marked = NULL;
		g_autofree gchar *dashboard = NULL;

		reply = get_as(fx, who[i], "/");
		g_assert_cmpuint(reply.status, ==, 200);
		marked = marked_tabs(reply.body, FALSE);
		g_assert_cmpstr(marked, ==, "Dashboard");
		/* The brand links to "/" first, unmarked; the tab is the second. */
		dashboard = link_current(strstr(reply.body, "id=\"primary-nav\""), "/");
		g_assert_cmpstr(dashboard, ==, "page");
		g_assert_true(rows_are_empty(reply.body, FALSE));
		axis_test_reply_clear(&reply);
	}

	/* The results and a title's page, as whole pages: no tab. */
	reply = get_as(fx, fx->viewer, "/search?q=mazework");
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	title = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id(fx, "101"));
	for (i = 0; i < G_N_ELEMENTS(who); i++) {
		const gchar *paths[3];
		guint p;

		paths[0] = "/search?q=mazework";
		paths[1] = "/search?q=mazework&kind=series";
		paths[2] = title;
		for (p = 0; p < G_N_ELEMENTS(paths); p++) {
			g_autofree gchar *marked = NULL;

			reply = get_as(fx, who[i], paths[p]);
			g_assert_cmpuint(reply.status, ==, 200);
			marked = marked_tabs(reply.body, FALSE);
			if (strcmp(marked, "") != 0)
				g_error("%s marks the tab \"%s\"", paths[p], marked);
			g_assert_true(rows_are_empty(reply.body, FALSE));
			axis_test_reply_clear(&reply);
		}
	}

	/* Typed in the box (the content replaced): the tabs out of band, none
	 * marked, and empty rows; not the box, nor the rest of the header. */
	for (i = 0; i < G_N_ELEMENTS(who); i++) {
		g_autofree gchar *marked = NULL;

		reply = get_htmx(fx, who[i], "/search?q=mazework", "content");
		g_assert_cmpuint(reply.status, ==, 200);
		assert_lacks(reply.body, "<!DOCTYPE");
		assert_contains(reply.body, "<h1>Search movies and TV</h1>");
		marked = marked_tabs(reply.body, TRUE);
		g_assert_cmpstr(marked, ==, "");
		g_assert_true(rows_are_empty(reply.body, TRUE));
		assert_lacks(reply.body, "<header");
		assert_lacks(reply.body, "id=\"search-q\"");
		assert_lacks(reply.body, "user-menu");
		/* Which element is replaced decides the shape, and the cache knows. */
		assert_contains(axis_test_reply_header(&reply, "Vary"), "HX-Request");
		assert_contains(axis_test_reply_header(&reply, "Vary"), "HX-Target");
		axis_test_reply_clear(&reply);
	}
	reply = get_htmx(fx, fx->viewer, title, "content");
	g_assert_cmpuint(reply.status, ==, 200);
	{
		g_autofree gchar *marked = marked_tabs(reply.body, TRUE);

		g_assert_cmpstr(marked, ==, "");
	}
	axis_test_reply_clear(&reply);

	/* A tab or "more" (the region replaced): nothing of the header. */
	reply = get_htmx(fx, fx->viewer, "/search?q=mazework&kind=film", "search-results");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "primary-nav");
	assert_lacks(reply.body, "nav-rows");
	assert_contains(axis_test_reply_header(&reply, "Vary"), "HX-Target");
	axis_test_reply_clear(&reply);

	/* Plain htmx with no target named: the content alone, as before. */
	reply = get_htmx(fx, fx->viewer, "/search?q=mazework", NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "primary-nav");
	axis_test_reply_clear(&reply);

	/* A content swap to a page in a tab marks the tab, with its pages. */
	reply = get_htmx(fx, fx->admin, "/admin/settings", "content");
	g_assert_cmpuint(reply.status, ==, 200);
	{
		g_autofree gchar *marked = marked_tabs(reply.body, TRUE);

		g_assert_cmpstr(marked, ==, "System");
	}
	g_assert_false(rows_are_empty(reply.body, TRUE));
	assert_contains(reply.body, "aria-label=\"System pages\"");
	{
		const gchar *rows = strstr(reply.body, "id=\"nav-rows\"");
		g_autofree gchar *settings = NULL;

		g_assert_nonnull(rows);
		settings = link_current(rows, "/admin/settings");
		g_assert_cmpstr(settings, ==, "page");
	}
	axis_test_reply_clear(&reply);
}

/*
 * The kind select of the box (select#search-kind) in @body: how many there
 * are, its options' values in order (joined by ","), the one chosen, and
 * whether it is swapped in out of band.
 */
static guint
kind_select_of(
	const gchar  *body,
	gchar       **values,
	gchar       **chosen,
	gboolean     *oob
){
	g_autoptr(GRegex) selects = g_regex_new("<select([^>]*)>(.*?)</select>", G_REGEX_DOTALL, 0, NULL);
	g_autoptr(GRegex) options = g_regex_new("<option([^>]*)>", 0, 0, NULL);
	g_autoptr(GRegex) value = g_regex_new("value=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint n = 0;

	*values = NULL;
	*chosen = NULL;
	*oob = FALSE;
	g_regex_match(selects, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *attributes = g_match_info_fetch(info, 1);
		g_autofree gchar *inside = g_match_info_fetch(info, 2);
		g_autoptr(GMatchInfo) each = NULL;
		g_autoptr(GString) list = NULL;

		if (strstr(attributes, "id=\"search-kind\"") == NULL) {
			g_match_info_next(info, NULL);
			continue;
		}
		n++;
		assert_contains(attributes, "name=\"kind\"");
		*oob = strstr(attributes, "hx-swap-oob=\"true\"") != NULL;
		list = g_string_new(NULL);
		g_regex_match(options, inside, 0, &each);
		while (g_match_info_matches(each)) {
			g_autofree gchar *option = g_match_info_fetch(each, 1);
			g_autoptr(GMatchInfo) found = NULL;

			if (g_regex_match(value, option, 0, &found)) {
				g_autofree gchar *v = g_match_info_fetch(found, 1);

				g_string_append_printf(list, "%s%s", list->len > 0 ? "," : "", v);
				if (strstr(option, "selected=\"selected\"") != NULL) {
					g_assert_null(*chosen);
					*chosen = g_strdup(v);
				}
			}
			g_match_info_next(each, NULL);
		}
		g_free(*values);
		*values = g_string_free(g_steal_pointer(&list), FALSE);
		g_match_info_next(info, NULL);
	}
	return n;
}

/* The one kind select in @body, with @want chosen. */
static void
assert_kind_chosen(
	const gchar *body,
	const gchar *want,
	gboolean     want_oob
){
	g_autofree gchar *values = NULL;
	g_autofree gchar *chosen = NULL;
	gboolean oob = FALSE;

	g_assert_cmpuint(kind_select_of(body, &values, &chosen, &oob), ==, 1);
	g_assert_cmpstr(values, ==, "all,film,series");
	g_assert_cmpstr(chosen, ==, want);
	g_assert_cmpint(oob, ==, want_oob);
}

/* As htmx sends a request that @trigger (an element's id) started. */
static AxisTestReply
get_triggered(
	Fixture     *fx,
	const gchar *path,
	const gchar *target,
	const gchar *trigger
){
	const gchar *extra[7];

	extra[0] = "HX-Request";
	extra[1] = "true";
	extra[2] = "HX-Target";
	extra[3] = target;
	extra[4] = trigger != NULL ? "HX-Trigger" : NULL;
	extra[5] = trigger;
	extra[6] = NULL;
	return axis_test_web_request(fx->session, fx->port, "GET", path, fx->viewer, extra, NULL);
}

/*
 * Spec (web-shell, task 2.1): the kind beside the box.  On every page, All
 * chosen, in the plain form with the box; sent without script, the results
 * page keeps it (its tab current, the select showing it); htmx sends it with
 * what is typed; a tab chosen on the results makes the select follow; a
 * change with nothing typed asks nothing, one with something typed searches
 * that kind.
 */
static void
test_kind_select(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	g_autoptr(GRegex) form = g_regex_new("<form class=\"search-box\"[^>]*>(.*?)</form>", G_REGEX_DOTALL, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;

	page = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(page.status, ==, 200);
	assert_kind_chosen(page.body, "all", FALSE);
	assert_contains(page.body, "for=\"search-kind\">Search in</label>");
	/* Both in the one form, the kind first; each sends the other. */
	g_assert_true(g_regex_match(form, page.body, 0, &info));
	{
		g_autofree gchar *inside = g_match_info_fetch(info, 1);
		const gchar *select = strstr(inside, "<select");
		const gchar *input = strstr(inside, "<input");

		g_assert_nonnull(select);
		g_assert_nonnull(input);
		g_assert_true(select < input);
	}
	assert_contains(page.body, "hx-include=\"#search-kind\"");
	assert_contains(page.body, "hx-include=\"#search-q\"");
	assert_csp_clean(page.body);
	axis_test_reply_clear(&page);

	/* Without script: the form's kind is the results' tab, and the select
	 * on the results page shows it. */
	page = get_as(fx, fx->viewer, "/search?q=mazework&kind=series");
	g_assert_cmpuint(page.status, ==, 200);
	assert_kind_chosen(page.body, "series", FALSE);
	assert_contains(page.body, "Fixture Street");
	assert_lacks(page.body, "The Mazework Reloaded");
	save_page("search-kind-series.html", page.body);
	axis_test_reply_clear(&page);
	/* A kind there is not is All. */
	page = get_as(fx, fx->viewer, "/search?q=mazework&kind=books");
	g_assert_cmpuint(page.status, ==, 200);
	assert_kind_chosen(page.body, "all", FALSE);
	assert_contains(page.body, "Fixture Street");
	assert_contains(page.body, "The Mazework Reloaded");
	axis_test_reply_clear(&page);

	/* Typed, with htmx: the content, of the kind sent with it. */
	page = get_triggered(fx, "/search?kind=film&q=mazework", "content", "search-q");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "The Mazework Reloaded");
	assert_lacks(page.body, "Fixture Street");
	axis_test_reply_clear(&page);

	/* A tab on the results: the region, and the select swapped to follow. */
	page = get_triggered(fx, "/search?q=mazework&kind=film", "search-results", NULL);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "id=\"search-results\"");
	assert_kind_chosen(page.body, "film", TRUE);
	axis_test_reply_clear(&page);

	/* The select changed with nothing typed: nothing asked, nothing swapped. */
	page = get_triggered(fx, "/search?kind=film&q=", "content", "search-kind");
	g_assert_cmpuint(page.status, ==, 204);
	axis_test_reply_clear(&page);
	page = get_triggered(fx, "/search?kind=series", "content", "search-kind");
	g_assert_cmpuint(page.status, ==, 204);
	axis_test_reply_clear(&page);
	/* With something typed: that kind's results. */
	page = get_triggered(fx, "/search?kind=series&q=mazework", "content", "search-kind");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Fixture Street");
	assert_lacks(page.body, "The Mazework Reloaded");
	axis_test_reply_clear(&page);
}

/* Spec: Nothing found; and what is asked of a search that is not one */
static void
test_nothing_found_and_not_a_search(Fixture *fx, gconstpointer data)
{
	AxisTestReply empty;
	AxisTestReply none = get_as(fx, fx->viewer, "/search");
	AxisTestReply one = get_as(fx, fx->viewer, "/search?q=m");
	AxisTestReply spaces = get_as(fx, fx->viewer, "/search?q=%20%20%20");
	g_autofree gchar *too_long = g_strnfill(500, 'a');
	g_autofree gchar *long_path = g_strdup_printf("/search?q=%s", too_long);
	AxisTestReply huge = get_as(fx, fx->viewer, long_path);
	AxisTestReply bad = get_as(fx, fx->viewer, "/search?q=%ff%fe");

	set_route(fx, "/search/film", "{\"results\":[]}");
	set_route(fx, "/search/series", "{\"results\":[]}");
	empty = get_as(fx, fx->viewer, "/search?q=zzzz+qqqq");
	g_assert_cmpuint(empty.status, ==, 200);
	assert_contains(empty.body, "Nothing matches &quot;zzzz qqqq&quot;. Check the spelling");
	assert_contains(empty.body, "tab-count\">0<");
	assert_lacks(empty.body, "title-card");
	save_page("results-none.html", empty.body);

	g_assert_cmpuint(none.status, ==, 200);
	assert_contains(none.body, "Type the name of a movie or a series");
	g_assert_cmpuint(one.status, ==, 200);
	assert_contains(one.body, "at least two characters");
	g_assert_cmpuint(spaces.status, ==, 200);
	assert_contains(spaces.body, "Type the name of a movie or a series");
	g_assert_cmpuint(huge.status, ==, 200);
	assert_contains(huge.body, "too long");
	g_assert_cmpuint(bad.status, ==, 200);
	assert_contains(bad.body, "too long");
	/* None of them asked the provider anything. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);

	axis_test_reply_clear(&empty);
	axis_test_reply_clear(&none);
	axis_test_reply_clear(&one);
	axis_test_reply_clear(&spaces);
	axis_test_reply_clear(&huge);
	axis_test_reply_clear(&bad);
}

/* Spec: Markup in a title (results and the title's page) */
static void
test_markup_is_text(Fixture *fx, gconstpointer data)
{
	AxisTestReply results;
	AxisTestReply page;
	g_autofree gchar *path = NULL;
	gint64 id;

	set_route(fx, "/search/film",
		"{\"results\":[{\"id\":\"901\",\"kind\":\"film\",\"title\":\"<script>alert(1)</script> & <b>Bold</b>\","
		"\"year\":2001,\"overview\":\"<img src=x onerror=alert(2)> \\\"quoted\\\" & more\","
		"\"poster\":\"/p901.png\",\"ids\":{\"imdb\":\"tt0000901\"}}]}");
	set_route(fx, "/title/film/901",
		"{\"id\":\"901\",\"kind\":\"film\",\"title\":\"<script>alert(1)</script> & <b>Bold</b>\","
		"\"original_title\":\"<i>Original</i>\",\"year\":2001,"
		"\"overview\":\"<img src=x onerror=alert(2)> \\\"quoted\\\" & more\",\"runtime\":90,\"status\":\"released\","
		"\"genres\":[\"<u>Drama</u>\"],\"language\":\"en\",\"release_date\":\"2001-02-03\",\"poster\":\"/p901.png\","
		"\"ids\":{\"imdb\":\"tt0000901\",\"tmdb\":\"901\"},"
		"\"alt_titles\":[{\"title\":\"<a href=javascript:evil()>x</a>\",\"country\":\"DE\"}]}");
	results = get_as(fx, fx->viewer, "/search?q=script&kind=film");
	g_assert_cmpuint(results.status, ==, 200);
	assert_contains(results.body, "&lt;script&gt;alert(1)&lt;/script&gt; &amp; &lt;b&gt;Bold&lt;/b&gt;");
	assert_contains(results.body, "&lt;img src=x onerror=alert(2)&gt;");
	assert_lacks(results.body, "<script>alert");
	assert_lacks(results.body, "<b>Bold");
	assert_lacks(results.body, "<img src=x");
	assert_csp_clean(results.body);

	id = title_id(fx, "901");
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, id);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "&lt;script&gt;alert(1)&lt;/script&gt;");
	assert_contains(page.body, "&lt;img src=x onerror=alert(2)&gt;");
	assert_contains(page.body, "&lt;i&gt;Original&lt;/i&gt;");
	assert_contains(page.body, "&lt;u&gt;Drama&lt;/u&gt;");
	assert_contains(page.body, "&lt;a href=javascript:evil()&gt;x&lt;/a&gt;");
	assert_lacks(page.body, "<script>alert");
	assert_lacks(page.body, "<i>Original");
	assert_lacks(page.body, "<u>Drama");
	assert_lacks(page.body, "<a href=javascript");
	assert_csp_clean(page.body);
	/* The tab's title is text too. */
	assert_contains(page.body, "<title>&lt;script&gt;");

	axis_test_reply_clear(&results);
	axis_test_reply_clear(&page);
}

/* Spec: Provider down; "a repeated search is made while the provider is
 * unreachable": saved results with the time they were fetched. */
static void
test_provider_down(Fixture *fx, gconstpointer data)
{
	AxisTestReply first = get_as(fx, fx->viewer, "/search?q=mazework");
	AxisTestReply down;
	AxisTestReply nothing;

	g_assert_cmpuint(first.status, ==, 200);
	assert_lacks(first.body, "did not answer");
	axis_test_db_exec(fx->db, "UPDATE meta_search SET fetched_at = now() - interval '3 days'");
	axis_test_plex_route(fx->fa->api, "/search/film", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	axis_test_plex_route(fx->fa->api, "/search/series", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	down = get_as(fx, fx->viewer, "/search?q=mazework");
	g_assert_cmpuint(down.status, ==, 200);
	assert_contains(down.body, "notice-warning");
	/* One notice for both kinds, in the provider's words, with the age. */
	assert_contains(down.body, "Fixture answered 503.");
	assert_contains(down.body, "The movies shown are the results saved on");
	assert_contains(down.body, "The series are from that time too.");
	assert_contains(down.body, "3 days ago");
	g_assert_cmpuint(count_of(down.body, "notice-warning"), ==, 1);
	assert_contains(down.body, "The Mazework Reloaded");
	assert_contains(down.body, "Fixture Street");
	assert_contains(down.body, "Test data from the fixture provider");
	save_page("results-down.html", down.body);

	nothing = get_as(fx, fx->viewer, "/search?q=never+saved");
	g_assert_cmpuint(nothing.status, ==, 200);
	assert_contains(nothing.body, "Nothing was saved for this search");
	assert_lacks(nothing.body, "title-card");
	/* Not "nothing matches": nothing could be asked. */
	assert_lacks(nothing.body, "Nothing matches");

	axis_test_reply_clear(&first);
	axis_test_reply_clear(&down);
	axis_test_reply_clear(&nothing);
}

/* Spec: No provider (and the settings link for an administrator only) */
static void
test_no_provider(Fixture *fx, gconstpointer data)
{
	AxisTestReply viewer = get_as(fx, fx->viewer, "/search?q=mazework");
	AxisTestReply admin = get_as(fx, fx->admin, "/search?q=mazework");
	AxisTestReply region = get_htmx(fx, fx->viewer, "/search?q=mazework", "search-results");

	g_assert_cmpuint(viewer.status, ==, 200);
	assert_contains(viewer.body, "Axis has no TMDb token");
	/* Spec (web-shell, tidy-labels): One word for the TMDb credential. */
	assert_lacks(viewer.body, "metadata key");
	assert_contains(viewer.body, "An administrator can add one in Settings");
	assert_lacks(viewer.body, "/admin/settings");
	assert_contains(viewer.body, "role=\"search\"");
	assert_lacks(viewer.body, "title-card");

	assert_contains(admin.body, "Axis has no TMDb token");
	assert_contains(admin.body, "Add a TMDb token in Settings");
	assert_contains(admin.body, "href=\"/admin/settings#module-metadata\"");
	assert_lacks(admin.body, "An administrator can add one");
	save_page("results-noprovider.html", admin.body);

	assert_contains(region.body, "Axis has no TMDb token");
	assert_lacks(region.body, "/admin/settings");
	/* Nothing was asked of the provider. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 0);

	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&region);
}

/* Spec: Slow image host: the list is shown without waiting for artwork, and
 * with placeholders. */
static void
test_slow_image_host(Fixture *fx, gconstpointer data)
{
	AxisTestReply first;
	gint64 started;

	/* A poster host that never answers. */
	axis_test_plex_route(fx->fa->images, "/img/w185/p101.png", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	axis_test_plex_route(fx->fa->images, "/img/w185/p102.png", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	axis_test_plex_route(fx->fa->images, "/img/w185/p201.png", -1, AXIS_TEST_PLEX_HANG, 0, NULL, 0);
	started = g_get_monotonic_time();
	first = get_as(fx, fx->viewer, "/search?q=mazework");
	g_assert_cmpuint(first.status, ==, 200);
	g_assert_cmpint((g_get_monotonic_time() - started) / 1000, <, 2500);
	assert_contains(first.body, "The Mazework Reloaded");
	/* Placeholders with the title set in them, loaded later one by one. */
	g_assert_cmpuint(count_of(first.body, "poster-none"), ==, 4);
	assert_contains(first.body, "<span class=\"poster-title\">The Mazework</span>");
	g_assert_cmpuint(count_of(first.body, "hx-get=\"/search/poster?provider=fixture&amp;ref=%2Fp"), ==, 3);
	assert_contains(first.body, "hx-trigger=\"revealed\"");
	assert_lacks(first.body, "<img ");
	/* The one without a poster is a placeholder that stays. */
	assert_contains(first.body, "<span class=\"poster-title\">The Mazework Revolutions</span>");
	g_assert_cmpuint(count_of(first.body, "hx-get=\"/search/poster?"), ==, 3);
	save_page("results-slow.html", first.body);
	axis_test_reply_clear(&first);
}

/* Every image is the daemon's own, and how a poster arrives later. */
static void
test_posters_arrive_after_the_text(Fixture *fx, gconstpointer data)
{
	AxisTestReply first = get_as(fx, fx->viewer, "/search?q=mazework");
	AxisTestReply second;
	AxisTestReply late;
	AxisTestReply kept;
	AxisTestReply not_yet;
	AxisTestReply last;

	g_assert_cmpuint(first.status, ==, 200);
	assert_lacks(first.body, "<img ");
	g_assert_cmpuint(count_of(first.body, "hx-get=\"/search/poster?"), ==, 3);
	save_page("results-placeholders.html", first.body);

	/* They are kept in time, and then the list has the images. */
	g_assert_true(wait_for_artwork(fx, 3));
	second = get_as(fx, fx->viewer, "/search?q=mazework");
	g_assert_cmpuint(second.status, ==, 200);
	g_assert_cmpuint(assert_images_local(second.body), ==, 3);
	assert_lacks(second.body, "hx-get=\"/search/poster?");
	assert_contains(second.body, "<span class=\"poster-title\">The Mazework Revolutions</span>");
	assert_csp_clean(second.body);
	save_page("results.html", second.body);

	/* The lazy load: the image once it is kept... */
	kept = get_htmx(fx, fx->viewer, "/search/poster?provider=fixture&ref=%2Fp101.png&size=card&try=0&t=The+Mazework", NULL);
	g_assert_cmpuint(kept.status, ==, 200);
	assert_contains(kept.body, "<img ");
	assert_lacks(kept.body, "poster-none");
	g_assert_cmpuint(assert_images_local(kept.body), ==, 1);
	assert_lacks(kept.body, "<!DOCTYPE");
	/* ...the placeholder again, to ask once more, while it is on its way... */
	not_yet = get_htmx(fx, fx->viewer, "/search/poster?provider=fixture&ref=%2Fp999.png&size=card&try=0&t=Not+yet", NULL);
	g_assert_cmpuint(not_yet.status, ==, 200);
	assert_contains(not_yet.body, "poster-none");
	assert_contains(not_yet.body, "try=1");
	assert_contains(not_yet.body, "load delay:2s");
	assert_contains(not_yet.body, "<span class=\"poster-title\">Not yet</span>");
	/* ...and the placeholder that stays when it has asked enough. */
	last = get_htmx(fx, fx->viewer, "/search/poster?provider=fixture&ref=%2Fp999.png&size=card&try=5&t=Not+yet", NULL);
	g_assert_cmpuint(last.status, ==, 200);
	assert_contains(last.body, "poster-none");
	assert_lacks(last.body, "hx-get");
	/* Asking for what is not a poster is answered with nothing to swap. */
	late = get_htmx(fx, fx->viewer, "/search/poster?provider=fixture&ref=..%2Fsecret&size=card&try=0&t=x", NULL);
	g_assert_cmpuint(late.status, ==, 204);
	axis_test_reply_clear(&late);
	late = get_htmx(fx, fx->viewer, "/search/poster?provider=nosuch&ref=%2Fp101.png&size=card&try=0&t=x", NULL);
	g_assert_cmpuint(late.status, ==, 204);
	axis_test_reply_clear(&late);
	late = get_htmx(fx, fx->viewer, "/search/poster?provider=fixture&ref=%2Fp101.png&size=huge&try=0&t=x", NULL);
	g_assert_cmpuint(late.status, ==, 204);
	axis_test_reply_clear(&late);
	late = get_htmx(fx, fx->viewer, "/search/poster", NULL);
	g_assert_cmpuint(late.status, ==, 204);

	axis_test_reply_clear(&first);
	axis_test_reply_clear(&second);
	axis_test_reply_clear(&late);
	axis_test_reply_clear(&kept);
	axis_test_reply_clear(&not_yet);
	axis_test_reply_clear(&last);
}

/* Twenty of each kind and "more" */
static void
test_twenty_and_more(Fixture *fx, gconstpointer data)
{
	g_autoptr(GString) films = g_string_new("{\"results\":[");
	g_autoptr(GString) series = g_string_new("{\"results\":[");
	AxisTestReply all;
	AxisTestReply tab;
	AxisTestReply more;
	AxisTestReply third;
	guint i;

	for (i = 1; i <= 24; i++) {
		g_string_append_printf(films, "%s{\"id\":\"%u\",\"kind\":\"film\",\"title\":\"%s%u\",\"year\":%u,"
			"\"overview\":\"%s\",\"poster\":\"%s\"}", i > 1 ? "," : "", 1000 + i,
			i % 7 == 0 ? "A film with a very long name that goes on and on, number " : "Film ", i, 1990 + i,
			i % 5 == 0 ? "" : "An overview of a film, which is long enough to take more than the two lines a card "
				"shows of it, so that the rest is cut and the cards keep one height.",
			i % 3 == 0 ? "" : "/p101.png");
	}
	g_string_append(films, "]}");
	for (i = 1; i <= 6; i++) {
		g_string_append_printf(series, "%s{\"id\":\"%u\",\"kind\":\"series\",\"title\":\"Series %u\",\"year\":%u,"
			"\"overview\":\"A series with an overview.\",\"poster\":\"/p201.png\"}", i > 1 ? "," : "", 2000 + i, i,
			2010 + i);
	}
	g_string_append(series, "]}");
	set_route(fx, "/search/film", films->str);
	set_route(fx, "/search/series", series->str);

	all = get_as(fx, fx->viewer, "/search?q=many");
	g_assert_cmpuint(all.status, ==, 200);
	/* 20 films and the 6 series; the other four films are behind a link. */
	g_assert_cmpuint(count_of(all.body, "class=\"title-card\""), ==, 26);
	assert_contains(all.body, "Show all 24 movies");
	assert_lacks(all.body, "Show all 6 series");
	assert_lacks(all.body, "Film 24");
	save_page("results-many.html", all.body);

	tab = get_as(fx, fx->viewer, "/search?q=many&kind=film");
	g_assert_cmpuint(count_of(tab.body, "class=\"title-card\""), ==, 20);
	assert_contains(tab.body, "Show more movies (20 of 24 shown)");
	assert_contains(tab.body, "page=2");
	assert_lacks(tab.body, "Series 1");

	more = get_as(fx, fx->viewer, "/search?q=many&kind=film&page=2");
	g_assert_cmpuint(count_of(more.body, "class=\"title-card\""), ==, 24);
	assert_lacks(more.body, "Show more");
	assert_contains(more.body, "Film 24");
	/* A page number that is not one is the first. */
	third = get_as(fx, fx->viewer, "/search?q=many&kind=film&page=99");
	g_assert_cmpuint(count_of(third.body, "class=\"title-card\""), ==, 20);

	axis_test_reply_clear(&all);
	axis_test_reply_clear(&tab);
	axis_test_reply_clear(&more);
	axis_test_reply_clear(&third);
}

/* Spec: Searches require a session and are limited per user */
static void
test_rate_limit_per_user(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	AxisTestReply other;
	AxisTestReply title;
	guint ok = 0;
	guint i;

	for (i = 0; i < 40; i++) {
		reply = get_as(fx, fx->viewer, "/search?q=mazework");
		if (reply.status == 429)
			break;
		g_assert_cmpuint(reply.status, ==, 200);
		ok++;
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(reply.status, ==, 429);
	/* A burst, not a trickle: a person typing is never stopped by it. */
	g_assert_cmpuint(ok, >=, 10);
	g_assert_cmpuint(ok, <=, 16);
	assert_contains(reply.body, "Too many searches");
	g_assert_nonnull(axis_test_reply_header(&reply, "Retry-After"));
	g_assert_cmpuint(g_ascii_strtoull(axis_test_reply_header(&reply, "Retry-After"), NULL, 10), >=, 1);
	/* The provider was asked once for what was asked many times. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);

	/* It is that user's limit: another's is untouched, and so is every page
	 * that is not a search. */
	other = get_as(fx, fx->admin, "/search?q=mazework");
	g_assert_cmpuint(other.status, ==, 200);
	title = get_as(fx, fx->viewer, "/");
	g_assert_cmpuint(title.status, ==, 200);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&other);
	axis_test_reply_clear(&title);
	/* What is not a search (and so asks nothing) is not counted. */
	reply = get_as(fx, fx->viewer, "/search");
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
}

/* Spec: a session is required for every page */
static void
test_session_required(Fixture *fx, gconstpointer data)
{
	const gchar *const paths[] = {
		"/search?q=mazework", "/search", "/search/poster?provider=fixture&ref=%2Fp101.png&size=card&try=0&t=x",
		"/title/1", "/title/1/season/1", NULL
	};
	guint i;

	for (i = 0; paths[i] != NULL; i++) {
		AxisTestReply reply = get_as(fx, NULL, paths[i]);

		g_assert_cmpuint(reply.status, !=, 200);
		g_assert_cmpuint(reply.status, <, 400);
		assert_lacks(reply.body, "The Mazework");
		assert_lacks(reply.body, "role=\"search\"");
		axis_test_reply_clear(&reply);
	}
}

/* Search text is not logged at the default level, and nothing but the
 * provider gets it. */
static void
test_search_text_is_not_logged(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply = get_as(fx, fx->viewer, "/search?q=Zorgon+Wibble");
	const GLogLevelFlags levels[] = { G_LOG_LEVEL_ERROR, G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING,
		G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO, G_LOG_LEVEL_DEBUG };
	guint i;

	g_assert_cmpuint(reply.status, ==, 200);
	for (i = 0; i < G_N_ELEMENTS(levels); i++) {
		g_assert_cmpuint(axis_test_log_count(levels[i], "*Zorgon*"), ==, 0);
		g_assert_cmpuint(axis_test_log_count(levels[i], "*Wibble*"), ==, 0);
	}
	axis_test_reply_clear(&reply);
}

/* The module's switch takes the box and the pages with it. */
static void
test_module_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply off;
	AxisTestReply search_off;
	AxisTestReply on;

	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_META_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	off = get_as(fx, fx->viewer, "/");
	assert_lacks(off.body, "role=\"search\"");
	assert_contains(off.body, "search-slot");
	search_off = get_as(fx, fx->viewer, "/search?q=mazework");
	g_assert_cmpuint(search_off.status, ==, 404);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_META_MODULE_ID, TRUE, &error));
	on = get_as(fx, fx->viewer, "/");
	assert_contains(on.body, "role=\"search\"");
	axis_test_reply_clear(&off);
	axis_test_reply_clear(&search_off);
	axis_test_reply_clear(&on);
}

/* ── Title pages ────────────────────────────────────────────────── */

/* Spec: Title page (a film), identifiers, strict number */
static void
test_film_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply search = get_as(fx, fx->viewer, "/search?q=mazework");
	g_autofree gchar *path = NULL;
	g_autofree gchar *viewed = NULL;
	AxisTestReply first;
	AxisTestReply page;
	AxisTestReply htmx_page;
	gint64 id;
	guint i;
	const gchar *const missing[] = {
		"/title/0", "/title/00", "/title/012", "/title/abc", "/title/-1", "/title/1.5", "/title/1e3",
		"/title/99999999999999999999", "/title/999999999999999999", "/title/1%20or%201", "/title/999999",
		"/title/0x10", "/title/1;2", "/title/+1", NULL
	};

	g_assert_cmpuint(search.status, ==, 200);
	id = title_id(fx, "101");
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, id);
	/* The first time the page's poster and backdrop are not kept: a
	 * placeholder that loads the poster, and no backdrop. */
	first = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(first.status, ==, 200);
	assert_contains(first.body, "poster poster-large poster-none");
	assert_contains(first.body, "size=page");
	assert_lacks(first.body, "title-backdrop");
	g_assert_true(wait_for_artwork(fx, 5));
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>The Mazework</h1>");
	assert_contains(page.body, "<title>The Mazework - Axis</title>");
	assert_contains(page.body, "<li>Movie</li>");
	assert_contains(page.body, "<li>1999</li>");
	assert_contains(page.body, "<li>136 min</li>");
	assert_contains(page.body, "<li>Released</li>");
	assert_contains(page.body, "A cartographer maps a hedge maze");
	assert_contains(page.body, "1999-03-31");
	assert_contains(page.body, "Action, Science Fiction");
	assert_contains(page.body, "Mazework (DE)");
	assert_contains(page.body, "<title>");
	/* Identifiers, as links that open elsewhere. */
	assert_contains(page.body, "href=\"https://www.imdb.com/title/tt0600003/\"");
	g_assert_cmpuint(assert_external_links(page.body), ==, 1);
	assert_contains(page.body, "Test data from the fixture provider");
	/* The backdrop band and the poster. */
	g_assert_cmpuint(assert_images_local(page.body), ==, 2);
	assert_contains(page.body, "title-backdrop");
	assert_csp_clean(page.body);
	assert_lacks(page.body, KEY);
	/* No season list for a film; no action area with no module. */
	assert_lacks(page.body, "season-name");
	assert_lacks(page.body, "title-actions");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 0);
	/* Viewing it counts, for the refresh. */
	viewed = g_strdup_printf("SELECT viewed_at IS NOT NULL FROM meta_title WHERE id = %" G_GINT64_FORMAT, id);
	{
		g_autofree gchar *answer = axis_test_db_query(fx->db, viewed);

		g_assert_cmpstr(answer, ==, "t");
	}
	save_page("film.html", page.body);

	htmx_page = get_htmx(fx, fx->viewer, path, "content");
	assert_lacks(htmx_page.body, "<!DOCTYPE");
	assert_contains(htmx_page.body, "<h1>The Mazework</h1>");

	/* Whatever is not a store number is no title. */
	for (i = 0; missing[i] != NULL; i++) {
		AxisTestReply reply = get_as(fx, fx->viewer, missing[i]);

		if (reply.status != 404)
			g_error("%s was answered %u", missing[i], reply.status);
		assert_contains(reply.body, "Not found");
		axis_test_reply_clear(&reply);
	}
	axis_test_reply_clear(&search);
	axis_test_reply_clear(&first);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&htmx_page);
}

/* A film without a poster or a backdrop, and one whose provider is down. */
static void
test_film_page_from_the_store(Fixture *fx, gconstpointer data)
{
	AxisTestReply search = get_as(fx, fx->viewer, "/search?q=mazework");
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	g_assert_cmpuint(search.status, ==, 200);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id(fx, "103"));
	/* Never fetched in full: only what the search said, and the provider does not answer. */
	axis_test_plex_route(fx->fa->api, "/title/film/103", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>The Mazework Revolutions</h1>");
	assert_contains(page.body, "Fixture answered 503.");
	assert_contains(page.body, "short summary was saved");
	assert_contains(page.body, "The maze and the town come to terms.");
	assert_contains(page.body, "poster-none");
	axis_test_reply_clear(&search);
	axis_test_reply_clear(&page);
}

/* Spec: Series (its seasons are listed and each shows its episodes) */
static void
test_series_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply search = get_as(fx, fx->viewer, "/search?q=fixture&kind=series");
	g_autofree gchar *path = NULL;
	g_autofree gchar *with_season = NULL;
	AxisTestReply page;
	AxisTestReply again;
	AxisTestReply opened;
	gint64 id;

	set_route(fx, "/title/series/201/season/2",
		"{\"number\":2,\"name\":\"Season 2\",\"air_date\":\"2026-09-04\",\"episodes\":["
		"{\"number\":1,\"name\":\"Back Again\",\"air_date\":\"2026-09-04\"},"
		"{\"number\":2,\"name\":\"Far Away <i>Soon</i>\",\"air_date\":\"2099-01-01\"},"
		"{\"number\":3,\"name\":\"Date Not Set\"}]}");
	g_assert_cmpuint(search.status, ==, 200);
	id = title_id(fx, "201");
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, id);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>Fixture Street</h1>");
	assert_contains(page.body, "<li>TV series</li>");
	assert_contains(page.body, "<li>45 min an episode</li>");
	assert_contains(page.body, "<li>Continuing</li>");
	assert_contains(page.body, "First aired");
	assert_contains(page.body, "2021-09-01");
	assert_contains(page.body, "Last aired");
	/* Seasons as <details>, each with its episodes. */
	g_assert_cmpuint(count_of(page.body, "<details "), ==, 2);
	assert_contains(page.body, "id=\"season-1\"");
	assert_contains(page.body, "id=\"season-2\"");
	assert_contains(page.body, "3 episodes, 2021");
	assert_contains(page.body, "Number One");
	assert_contains(page.body, "Number Three");
	assert_contains(page.body, "2021-09-15");
	assert_contains(page.body, "Back Again");
	/* The latest season is open; a future episode is marked, a past one is not. */
	g_assert_cmpuint(count_of(page.body, "<details "), ==, 2);
	assert_contains(page.body, "Far Away &lt;i&gt;Soon&lt;/i&gt;");
	assert_lacks(page.body, "<i>Soon");
	g_assert_cmpuint(count_of(page.body, "episode-future"), ==, 1);
	g_assert_cmpuint(count_of(page.body, ">Not aired<"), ==, 1);
	assert_contains(page.body, "Not announced");
	assert_contains(page.body, "href=\"https://thetvdb.com/dereferrer/series/350001\"");
	assert_contains(page.body, "href=\"https://www.imdb.com/title/tt1234567/\"");
	g_assert_cmpuint(assert_external_links(page.body), ==, 2);
	assert_csp_clean(page.body);
	/* The seasons were fetched for this page, once each. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201/season/1"), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201/season/2"), ==, 1);
	save_page("series.html", page.body);

	again = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(again.status, ==, 200);
	assert_contains(again.body, "Number Two");
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/title/series/201/season/1"), ==, 1);

	with_season = g_strdup_printf("%s?season=1", path);
	opened = get_as(fx, fx->viewer, with_season);
	g_assert_cmpuint(opened.status, ==, 200);
	assert_contains(opened.body, "Number One");

	axis_test_reply_clear(&search);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&again);
	axis_test_reply_clear(&opened);
}

/* A season that was not ready when the page was made opens with htmx, or on
 * a page of its own. */
static void
test_season_on_demand(Fixture *fx, gconstpointer data)
{
	AxisTestReply search = get_as(fx, fx->viewer, "/search?q=fixture&kind=series");
	g_autofree gchar *path = NULL;
	g_autofree gchar *season_path = NULL;
	g_autofree gchar *page_path = NULL;
	AxisTestReply page;
	AxisTestReply fragment;
	AxisTestReply plain;
	AxisTestReply bad;
	gint64 id;

	g_assert_cmpuint(search.status, ==, 200);
	id = title_id(fx, "201");
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, id);
	season_path = g_strdup_printf("/title/%" G_GINT64_FORMAT "/season/2", id);
	page_path = g_strdup_printf("/title/%" G_GINT64_FORMAT "?season=2#season-2", id);
	axis_test_plex_route(fx->fa->api, "/title/series/201/season/2", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Number One");
	assert_lacks(page.body, "Back Again");
	/* Season 2 holds a way to open it, and no episodes. */
	assert_contains(page.body, "hx-trigger=\"toggle once\"");
	assert_contains(page.body, season_path);
	assert_contains(page.body, "Show the episodes of season 2");
	save_page("series-late-season.html", page.body);

	/* The service is back: opening the season loads it. */
	axis_test_plex_route_file(fx->fa->api, "/title/series/201/season/2", -1, "tests/fixtures/meta/season-201-2.json");
	fragment = get_htmx(fx, fx->viewer, season_path, NULL);
	g_assert_cmpuint(fragment.status, ==, 200);
	assert_lacks(fragment.body, "<!DOCTYPE");
	assert_contains(fragment.body, "Back Again");
	assert_contains(fragment.body, "Next Door");
	assert_contains(fragment.body, "class=\"season-body\"");
	/* Without script, the same season is on a page of its own. */
	plain = get_as(fx, fx->viewer, season_path);
	g_assert_cmpuint(plain.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&plain, "Location"), ==, page_path);

	bad = get_htmx(fx, fx->viewer, "/title/0/season/1", NULL);
	g_assert_cmpuint(bad.status, ==, 404);
	axis_test_reply_clear(&bad);
	bad = get_htmx(fx, fx->viewer, "/title/1/season/x", NULL);
	g_assert_cmpuint(bad.status, ==, 404);
	axis_test_reply_clear(&bad);
	bad = get_htmx(fx, fx->viewer, "/title/1/season/99999", NULL);
	g_assert_cmpuint(bad.status, ==, 404);

	axis_test_reply_clear(&search);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&fragment);
	axis_test_reply_clear(&plain);
	axis_test_reply_clear(&bad);
}

/* ── The action slot ────────────────────────────────────────────── */

/* Spec: No module (the area is absent) */
static void
test_no_actions_without_a_module(Fixture *fx, gconstpointer data)
{
	AxisTestReply search = get_as(fx, fx->viewer, "/search?q=mazework");
	g_autofree gchar *path = NULL;
	AxisTestReply page;

	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id(fx, "101"));
	page = get_as(fx, fx->viewer, path);
	assert_lacks(search.body, "card-actions");
	assert_lacks(search.body, "test-action");
	assert_lacks(page.body, "title-actions");
	assert_lacks(page.body, "test-action");
	axis_test_reply_clear(&search);
	axis_test_reply_clear(&page);
}

/* A module registers: its pieces are drawn on every card and on the page,
 * after its figures were read on a worker thread. */
static void
test_actions_when_a_module_registers(Fixture *fx, gconstpointer data)
{
	AxisTestReply search = get_as(fx, fx->viewer, "/search?q=mazework");
	g_autofree gchar *path = NULL;
	g_autofree gchar *card = NULL;
	g_autofree gchar *on_page = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply page;
	AxisTestReply off;
	gint64 id = title_id(fx, "101");

	g_assert_cmpuint(search.status, ==, 200);
	g_assert_cmpuint(count_of(search.body, "<div class=\"card-actions\">"), ==, 4);
	g_assert_cmpuint(count_of(search.body, "class=\"test-action\""), ==, 4);
	card = g_strdup_printf("card-action of title %" G_GINT64_FORMAT " (4 titles)", id);
	assert_contains(search.body, card);
	assert_lacks(search.body, "title-actions");
	/* The figures were read once for the page's titles, off the main loop. */
	g_assert_cmpuint(actions_seen.collected, ==, 4);
	g_assert_false(actions_seen.collect_on_main_loop);

	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, id);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<div class=\"title-actions\">");
	on_page = g_strdup_printf("page-action of title %" G_GINT64_FORMAT " (1 titles)", id);
	assert_contains(page.body, on_page);
	assert_lacks(page.body, "card-actions");
	save_page("results-actions.html", search.body);

	/* The module is switched off: nothing of it is drawn. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "titletest", FALSE, &error));
	g_assert_no_error(error);
	off = get_as(fx, fx->viewer, "/search?q=mazework");
	assert_lacks(off.body, "card-actions");
	assert_lacks(off.body, "test-action");

	axis_test_reply_clear(&search);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&off);
}

/* The Metadata page's own lookup draws the same cards. */
static void
test_admin_lookup_uses_the_cards(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=mazework&year=");

	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "class=\"title-card\"");
	assert_contains(page.body, "IMDb tt0600003");
	assert_contains(page.body, "Saved ");
	axis_test_reply_clear(&page);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web-search/search/without-script", Fixture, NULL, test_search_without_script);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/htmx-region", Fixture, NULL, test_htmx_replaces_only_the_region);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/tabs", Fixture, NULL, test_tabs_follow_the_page);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/kind-select", Fixture, NULL, test_kind_select);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/nothing-found", Fixture, NULL, test_nothing_found_and_not_a_search);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/markup", Fixture, NULL, test_markup_is_text);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/provider-down", Fixture, NULL, test_provider_down);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/no-provider", Fixture, GUINT_TO_POINTER(WITHOUT_KEY), test_no_provider);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/slow-images", Fixture, NULL, test_slow_image_host);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/posters", Fixture, NULL, test_posters_arrive_after_the_text);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/more", Fixture, NULL, test_twenty_and_more);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/rate-limit", Fixture, NULL, test_rate_limit_per_user);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/session", Fixture, NULL, test_session_required);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/log", Fixture, NULL, test_search_text_is_not_logged);
	AXIS_TEST_ADD_DB_CASE("/web-search/search/module-off", Fixture, NULL, test_module_off);
	AXIS_TEST_ADD_DB_CASE("/web-search/title/film", Fixture, NULL, test_film_page);
	AXIS_TEST_ADD_DB_CASE("/web-search/title/film-from-store", Fixture, NULL, test_film_page_from_the_store);
	AXIS_TEST_ADD_DB_CASE("/web-search/title/series", Fixture, NULL, test_series_page);
	AXIS_TEST_ADD_DB_CASE("/web-search/title/season-on-demand", Fixture, NULL, test_season_on_demand);
	AXIS_TEST_ADD_DB_CASE("/web-search/actions/none", Fixture, NULL, test_no_actions_without_a_module);
	AXIS_TEST_ADD_DB_CASE("/web-search/actions/module", Fixture, GUINT_TO_POINTER(WITH_ACTIONS),
		test_actions_when_a_module_registers);
	AXIS_TEST_ADD_DB_CASE("/web-search/admin-lookup", Fixture, NULL, test_admin_lookup_uses_the_cards);
	return g_test_run();
}
