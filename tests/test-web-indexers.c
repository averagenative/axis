/*
 * test-web-indexers.c - the Indexers pages with the real shell on an
 * ephemeral loopback port, the Prowlarr kind and a fake Prowlarr, and a
 * real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (indexers): Non-administrator refused; Markup in a name;
 * Key in a fetch address (no fetch address is shown); Some indexers fail;
 * Repeat; Redirecting indexer (against the recorded answers of a real
 * Prowlarr).  Task 3.1, 4.3, 4.6; fix-prowlarr-live 2.1, 2.3.
 *
 * With AXIS_TEST_INDEXER_PAGES set to a directory, the pages are also saved
 * there, for looking at in a browser.
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
#include "axis-test-jobs.h"
#include "axis-test-newznab.h"
#include "axis-test-prowlarr.h"
#include "axis-test-sites.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "indexer/axis-indexer-sites.h"
#include "jobs/axis-job-builtin.h"
#include "jobs/axis-job-registry.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-settings.h"

#define KEY "prowlarr-api-key-7f3a9c21d84e5b06"
#define NZB_KEY "nzb-api-key-5c1e9a07b3d24f68"

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisTestProwlarr   *fake;
	AxisTestNewznab    *nzb;
	AxisTestNewznab    *other;
	AxisIndexers       *indexers;
	AxisWebIndexers    *ui;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisIndexerTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	/* 1: the recorded answers of a real Prowlarr. */
	fx->fake = GPOINTER_TO_UINT(data) == 1 ? axis_test_prowlarr_new_recorded(KEY, "") : axis_test_prowlarr_new(KEY, "");
	fx->nzb = axis_test_newznab_new(NZB_KEY, FALSE, "");
	fx->other = axis_test_newznab_new(NZB_KEY, FALSE, "");
	fx->settings = axis_settings_new();
	fx->indexers = axis_indexers_new(fx->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_newznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_torznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_yts_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_eztv_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_piratebay_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	fx->ui = axis_web_indexers_new(fx->indexers);
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->pool, &error));
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
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_stop(fx->web);
	axis_web_indexers_stop(fx->ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_indexers_stop(fx->indexers);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_indexers_free(fx->ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_indexers_unref(fx->indexers);
	axis_settings_unref(fx->settings);
	axis_test_prowlarr_free(fx->fake);
	axis_test_newznab_free(fx->nzb);
	axis_test_newznab_free(fx->other);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
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

/* What no page may hold: the key, an address that holds it, a link that
 * may hold a passkey. */
static void
assert_no_secret(const gchar *body)
{
	assert_lacks(body, KEY);
	assert_lacks(body, "apikey");
	assert_lacks(body, "/download?");
	assert_lacks(body, "pk-secret");
	assert_lacks(body, "magnet:");
	assert_lacks(body, "downloadUrl");
	assert_lacks(body, "link=");
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_INDEXER_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gchar *
db_text(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->db, sql);
}

/* A source on the fake, made through the layer. */
static gint64
add_source(
	Fixture     *fx,
	const gchar *name,
	const gchar *key
){
	AxisIndexerSourceSpec spec;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = AXIS_INDEXER_PROWLARR_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.secret = key;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

#define FILM_FORM "kind=film&title=Hearth&year=1995&imdb=tt0600001&tmdb=6001&tvdb=&season=&episode="

/* ── Pages ──────────────────────────────────────────────────────── */

/* Spec: Non-administrator refused */
static void
test_the_page_is_for_administrators(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *search = g_strdup_printf("%s/search", one);
	AxisTestReply viewer = get_as(fx, fx->viewer, "/indexers");
	AxisTestReply viewer_one = get_as(fx, fx->viewer, one);
	AxisTestReply admin = get_as(fx, fx->admin, "/indexers");
	AxisTestReply system = get_as(fx, fx->admin, "/admin/settings");
	g_autoptr(GArray) admin_nav = axis_module_registry_collect_nav(fx->registry, AXIS_NAV_GROUP_SYSTEM, TRUE);
	g_autoptr(GArray) viewer_nav = axis_module_registry_collect_nav(fx->registry, AXIS_NAV_GROUP_SYSTEM, FALSE);
	AxisTestReply posted = axis_test_web_request(fx->session, fx->port, "POST", "/indexers", fx->viewer, NULL, "name=x");
	AxisTestReply searched = axis_test_web_request(fx->session, fx->port, "POST", search, fx->viewer, NULL,
		FILM_FORM);

	g_assert_cmpuint(viewer.status, ==, 403);
	g_assert_cmpuint(viewer_one.status, ==, 403);
	g_assert_cmpuint(posted.status, ==, 403);
	g_assert_cmpuint(searched.status, ==, 403);
	g_assert_cmpuint(admin.status, ==, 200);
	g_assert_cmpuint(system.status, ==, 200);
	/* In System, for administrators only. */
	assert_contains(system.body, "href=\"/indexers\"");
	{
		gboolean in_admin = FALSE;
		gboolean in_viewer = FALSE;
		guint i;

		for (i = 0; i < admin_nav->len; i++)
			in_admin = in_admin || g_strcmp0(g_array_index(admin_nav, AxisNavEntry, i).path, "/indexers") == 0;
		for (i = 0; i < viewer_nav->len; i++)
			in_viewer = in_viewer || g_strcmp0(g_array_index(viewer_nav, AxisNavEntry, i).path, "/indexers") == 0;
		g_assert_true(in_admin);
		g_assert_false(in_viewer);
	}
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 0);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&viewer_one);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&system);
	axis_test_reply_clear(&posted);
	axis_test_reply_clear(&searched);
}

/* A POST without the token is refused, and a GET never contacts a source. */
static void
test_nothing_is_sent_without_a_token_or_by_a_get(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *search = g_strdup_printf("%s/search", one);
	AxisTestReply no_token = axis_test_web_request(fx->session, fx->port, "POST", search, fx->admin, NULL, FILM_FORM);
	AxisTestReply get_search = get_as(fx, fx->admin, search);
	AxisTestReply page = get_as(fx, fx->admin, one);

	g_assert_cmpuint(no_token.status, ==, 403);
	g_assert_cmpuint(get_search.status, ==, 405);
	g_assert_cmpuint(page.status, ==, 200);
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 0);
	axis_test_reply_clear(&no_token);
	axis_test_reply_clear(&get_search);
	axis_test_reply_clear(&page);
}

/* Task 3.1: no source */
static void
test_the_page_with_no_source(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/indexers");

	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "No indexer has been added yet.");
	assert_contains(page.body, "Add an indexer");
	assert_lacks(page.body, "or a source");
	assert_lacks(page.body, "indexer source");
	/* What the choices of Axis's own say about where an API is (task 2.1). */
	assert_contains(page.body, "An API at /api/v1/api is the base path /api/v1.");
	assert_contains(page.body, "/api/v2.0/indexers/NAME/results/torznab");
	/* One form for each kind, the Usenet indexer's open. */
	assert_contains(page.body, "<details class=\"guide-more\" open=\"open\"><summary>A Usenet indexer (Newznab)</summary>");
	assert_contains(page.body, "A torrent indexer, through a Torznab feed");
	assert_contains(page.body, "Prowlarr, for every indexer in it");
	assert_contains(page.body, "<input type=\"hidden\" name=\"kind\" value=\"newznab\" />");
	assert_contains(page.body, "<input type=\"hidden\" name=\"kind\" value=\"torznab\" />");
	assert_contains(page.body, "<input type=\"hidden\" name=\"kind\" value=\"prowlarr\" />");
	assert_contains(page.body, "in your profile or on its API page");
	assert_contains(page.body, "Newznab categories for movies");
	assert_contains(page.body, "Files fetched from (optional)");
	assert_contains(page.body, "type=\"password\"");
	assert_contains(page.body, "Base path (optional)");
	/* The page says both ways are possible now. */
	assert_contains(page.body, "which Axis asks itself with your own keys, public torrent sites (YTS, EZTV, The Pirate Bay, "
		"TorrentProject, LimeTorrents, TheRARBG, Torrents-CSV, and others through site definitions you install), which "
		"need none, and Prowlarr if you use it");
	assert_lacks(page.body, "never the indexers");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 0);
	save_page("indexers-none.html", page.body);
	axis_test_reply_clear(&page);
}

/* Spec: Indexers page; tasks 2.1 and 3.1 (add, save, test, state, indexers) */
static void
test_add_save_and_test_a_source(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("name=Prowlarr&kind=prowlarr&address=%s&secret=%s&enabled=yes", escaped, KEY);
	const gchar *bad = "name=Second&kind=prowlarr&address=ftp%3A%2F%2Fnot-an-origin&secret=second-key-value-91&enabled=yes";
	g_autofree gchar *unsafe = g_strdup_printf("name=Third&kind=prowlarr&address=%s&base_path=%%2Fa%%2F..%%2Fb"
		"&secret=third-key-value-42&enabled=yes", escaped);
	AxisTestReply added = post_as(fx, fx->admin, "/indexers", "/indexers", form);
	AxisTestReply refused = post_as(fx, fx->admin, "/indexers", "/indexers", bad);
	AxisTestReply no_way = post_as(fx, fx->admin, "/indexers", "/indexers", unsafe);
	AxisTestReply page;
	AxisTestReply saved;
	AxisTestReply tested;
	AxisTestReply list;
	g_autofree gchar *count = NULL;

	g_assert_cmpuint(added.status, ==, 303);
	assert_contains(axis_test_reply_header(&added, "Location"), "/indexers/1?done=added");
	count = db_text(fx, "SELECT count(*) FROM indexer_source_secret WHERE secret = '" KEY "'");
	g_assert_cmpstr(count, ==, "1");

	/* Refused: the form again with what was typed, never the key, and the
	 * reason naming the field. */
	g_assert_cmpuint(refused.status, ==, 400);
	assert_contains(refused.body, "The address");
	assert_contains(refused.body, "value=\"Second\"");
	assert_lacks(refused.body, "second-key-value-91");
	assert_csp_clean(refused.body);
	g_assert_cmpuint(no_way.status, ==, 400);
	assert_contains(no_way.body, "The base path");
	assert_contains(no_way.body, "value=\"Third\"");
	assert_lacks(no_way.body, "third-key-value-42");
	g_clear_pointer(&count, g_free);
	count = db_text(fx, "SELECT count(*) FROM indexer_source");
	g_assert_cmpstr(count, ==, "1");

	page = get_as(fx, fx->admin, "/indexers/1?done=added");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "The indexer has been added.");
	/* Spec (web-shell, tidy-labels): A card names its contents. */
	assert_contains(page.body, ">State of this indexer<");
	assert_contains(page.body, ">Settings for this indexer<");
	assert_lacks(page.body, "<h2>State</h2>");
	assert_lacks(page.body, "<h2>Settings</h2>");
	assert_contains(page.body, "A key is stored.");
	assert_contains(page.body, "Not checked yet");
	assert_contains(page.body, "plain http");
	assert_contains(page.body, "None read yet.");
	assert_lacks(page.body, KEY);
	assert_csp_clean(page.body);
	{
		g_autoptr(GRegex) secret_value = g_regex_new("<input[^>]*type=\"password\"[^>]*value=", 0, 0, NULL);

		g_assert_false(g_regex_match(secret_value, page.body, 0, NULL));
	}

	/* An edit with the key left empty keeps it, and stores the base path
	 * without its trailing slash. */
	saved = post_as(fx, fx->admin, "/indexers/1", "/indexers/1",
		"name=Prowlarr+box&address=http%3A%2F%2F127.0.0.1%3A1&base_path=%2Fprowlarr%2F&secret=&enabled=yes");
	g_assert_cmpuint(saved.status, ==, 303);
	g_clear_pointer(&count, g_free);
	count = db_text(fx, "SELECT count(*) FROM indexer_source_secret WHERE secret = '" KEY "'");
	g_assert_cmpstr(count, ==, "1");
	g_clear_pointer(&count, g_free);
	count = db_text(fx, "SELECT base_path || '|' || name FROM indexer_source");
	g_assert_cmpstr(count, ==, "/prowlarr|Prowlarr box");
	axis_test_reply_clear(&saved);
	saved = post_as(fx, fx->admin, "/indexers/1", "/indexers/1", form);
	g_assert_cmpuint(saved.status, ==, 303);

	/* Spec: Test */
	tested = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/test", NULL);
	g_assert_cmpuint(tested.status, ==, 303);
	assert_contains(axis_test_reply_header(&tested, "Location"), "/indexers/1?done=tested");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/indexers/1?done=tested");
	assert_contains(page.body, "Working");
	assert_contains(page.body, "1.21.2.4649");
	assert_contains(page.body, "NewsOwl");
	assert_contains(page.body, "Tracker One");
	/* Names from the source are text: escaped, not markup. */
	assert_contains(page.body, "Slow &lt;b&gt;Tracker&lt;/b&gt; &amp; Co");
	assert_lacks(page.body, "<b>Tracker</b>");
	assert_contains(page.body, "3 of 4 indexers are on, and 1 of those is failing now");
	assert_contains(page.body, "Failing");
	assert_contains(page.body, "IMDb, TMDb");
	assert_no_secret(page.body);
	assert_csp_clean(page.body);
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 1);

	list = get_as(fx, fx->admin, "/indexers");
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	assert_contains(list.body, "kind-indexer-sources");
	assert_contains(list.body, "Not encrypted");
	assert_contains(list.body, "3 indexers on, 1 failing");
	assert_no_secret(list.body);
	save_page("indexers-list.html", list.body);

	axis_test_reply_clear(&added);
	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&no_way);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&saved);
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&list);
}

/* Spec: Search; Some indexers fail; Markup in a name; Key in a fetch
 * address; tasks 3.1, 4.3, 4.6 */
static void
test_a_test_search_shows_raw_names_as_text(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *search = g_strdup_printf("%s/search", one);
	g_autofree gchar *requests = NULL;
	AxisTestReply tested = post_as(fx, fx->admin, one, "/indexers/1/test", NULL);
	AxisTestReply page = post_as(fx, fx->admin, one, search, FILM_FORM);

	g_assert_cmpuint(tested.status, ==, 303);
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Releases found");
	assert_contains(page.body, "8 releases found, searched by the IMDb identifier");
	assert_contains(page.body, "1 of 3 indexers did not answer: Tracker One.");
	assert_contains(page.body, "2 results were left out because they named a download address that is not this "
		"indexer&#39;s own");
	assert_contains(page.body, "1 result left out because there is no way to fetch it.");
	assert_contains(page.body, "Results that could not be read: 3.");

	/* The names as the indexers gave them, as text. */
	assert_contains(page.body, "Hearth.1995.1080p.BluRay.x264-GRP");
	assert_contains(page.body, "Hearth.1995.2160p.UHD.BluRay.x265-TRK");
	assert_contains(page.body, "NewsOwl");
	assert_contains(page.body, "Tracker One");
	assert_contains(page.body, "&lt;script&gt;alert(");
	assert_lacks(page.body, "<script>alert");
	assert_contains(page.body, "&lt;b&gt;bold&lt;/b&gt;");
	assert_lacks(page.body, "<b>bold</b>");
	assert_contains(page.body, "freeleech, internal");
	/* GLib puts a no-break space between a size and its unit. */
	assert_contains(page.body, "8.3\xc2\xa0GiB");
	assert_contains(page.body, "85 / 4");
	assert_contains(page.body, "2024-03-02 10:11:12 UTC");
	/* Nothing to fetch with is in the page. */
	assert_no_secret(page.body);
	assert_lacks(page.body, "0123456789abcdef0123456789abcdef01234567");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 2);
	assert_contains(page.body, "kind-indexer-releases");
	save_page("indexers-source.html", page.body);

	/* The page's search is not in a log line (any level) and not in the
	 * database as words. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "*Hearth*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*Hearth*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG, "*Hearth*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG, "*" KEY "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "*" KEY "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*" KEY "*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "*tt0600001*"), ==, 0);
	requests = db_text(fx, "SELECT count(*) FROM indexer_search WHERE request_key ~ '^[0-9a-f]{64}$'");
	g_assert_cmpstr(requests, ==, "1");

	/* Spec: Repeat - the same search again says so, and asks nothing. */
	axis_test_reply_clear(&page);
	{
		guint before = axis_test_prowlarr_n_requests(fx->fake);

		page = post_as(fx, fx->admin, one, search, "kind=film&title=+HEARTH+&year=1995&imdb=tt0600001&tmdb=6001");
		g_assert_cmpuint(page.status, ==, 200);
		assert_contains(page.body, "an earlier search, kept until");
		assert_contains(page.body, "the indexer was not asked again");
		g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, before);
	}
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&page);
}

/* A search that cannot be made is the form's to fix: 400, what was typed, the reason. */
static void
test_a_test_search_that_cannot_be_made(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *search = g_strdup_printf("%s/search", one);
	AxisTestReply empty = post_as(fx, fx->admin, one, search, "kind=film&title=&year=&imdb=&tmdb=&tvdb=&season=&episode=");
	AxisTestReply episode = post_as(fx, fx->admin, one, search, "kind=episode&title=The+Show&season=1&episode=");
	AxisTestReply words = post_as(fx, fx->admin, one, search, "kind=film&title=Zoolander&year=nineteen");

	g_assert_cmpuint(empty.status, ==, 400);
	assert_contains(empty.body, "The search did not give a result: ");
	assert_contains(empty.body, "needs a title or an identifier");
	g_assert_cmpuint(episode.status, ==, 400);
	assert_contains(episode.body, "value=\"The Show\"");
	g_assert_cmpuint(words.status, ==, 400);
	assert_contains(words.body, "whole numbers");
	assert_contains(words.body, "value=\"Zoolander\"");
	g_assert_cmpuint(axis_test_prowlarr_n_requests(fx->fake), ==, 0);
	axis_test_reply_clear(&empty);
	axis_test_reply_clear(&episode);
	axis_test_reply_clear(&words);
}

/* A source that refuses the key or does not answer is shown as it is. */
static void
test_a_failing_source_is_shown_as_it_is(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *search = g_strdup_printf("%s/search", one);
	AxisTestReply refused;
	AxisTestReply page;
	AxisTestReply list;

	axis_test_prowlarr_set_key(fx->fake, "some-other-key-18f0c2d94a");
	refused = post_as(fx, fx->admin, one, search, FILM_FORM);
	g_assert_cmpuint(refused.status, ==, 200);
	assert_contains(refused.body, "The search did not give a result: ");
	assert_contains(refused.body, "refused");
	assert_contains(refused.body, "Access refused");
	assert_no_secret(refused.body);
	assert_lacks(refused.body, "some-other-key-18f0c2d94a");
	assert_csp_clean(refused.body);
	save_page("indexers-failing.html", refused.body);

	page = get_as(fx, fx->admin, one);
	assert_contains(page.body, "Access refused");
	assert_contains(page.body, "What the indexer said");
	assert_no_secret(page.body);
	list = get_as(fx, fx->admin, "/indexers");
	assert_contains(list.body, "Access refused");
	assert_contains(list.body, "The API key was refused");
	assert_no_secret(list.body);
	save_page("indexers-failing-list.html", list.body);

	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&list);
}

static void
test_removing_a_source(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *delete = g_strdup_printf("%s/delete", one);
	AxisTestReply unconfirmed = post_as(fx, fx->admin, one, delete, NULL);
	AxisTestReply gone;
	AxisTestReply after;
	AxisTestReply missing;
	g_autofree gchar *count = NULL;

	g_assert_cmpuint(unconfirmed.status, ==, 400);
	count = db_text(fx, "SELECT count(*) FROM indexer_source");
	g_assert_cmpstr(count, ==, "1");
	gone = post_as(fx, fx->admin, one, delete, "confirm=yes");
	g_assert_cmpuint(gone.status, ==, 303);
	assert_contains(axis_test_reply_header(&gone, "Location"), "/indexers?done=deleted");
	g_clear_pointer(&count, g_free);
	count = db_text(fx, "SELECT count(*) FROM indexer_source");
	g_assert_cmpstr(count, ==, "0");
	after = get_as(fx, fx->admin, "/indexers?done=deleted");
	assert_contains(after.body, "The indexer has been removed");
	missing = get_as(fx, fx->admin, one);
	g_assert_cmpuint(missing.status, ==, 404);
	axis_test_reply_clear(&unconfirmed);
	axis_test_reply_clear(&gone);
	axis_test_reply_clear(&after);
	axis_test_reply_clear(&missing);
}

/*
 * Spec: Indexers that redirect are shown (Redirecting indexer); tasks 2.1,
 * 2.3, against the recorded answers of a real Prowlarr (NewsOwl and
 * DozyNewt have Redirect on).  Following is on by default: the indexers
 * say where their downloads come from, the test search marks the releases
 * fetched from an indexer's site, and keeps every torrent known by its link
 * address.  Switched off: the source's State says which indexers cannot be
 * fetched from and the two ways out, and the list says the source needs
 * attention.
 */
static void
test_indexers_that_redirect_are_shown(Fixture *fx, gconstpointer data)
{
	gint64 id = add_source(fx, "Prowlarr", KEY);
	g_autofree gchar *one = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);
	g_autofree gchar *search = g_strdup_printf("%s/search", one);
	g_autofree gchar *base = axis_test_prowlarr_base(fx->fake);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *off = g_strdup_printf("name=Prowlarr&address=%s&base_path=&secret=&enabled=yes", escaped);
	g_autofree gchar *on = g_strdup_printf("%s&follow_redirects=yes", off);
	g_autofree gchar *follow = NULL;
	AxisTestReply tested = post_as(fx, fx->admin, one, "/indexers/1/test", NULL);
	AxisTestReply page = get_as(fx, fx->admin, one);
	AxisTestReply found;
	AxisTestReply saved;
	AxisTestReply list;
	AxisTestReply fresh = get_as(fx, fx->admin, "/indexers");

	/* A new source follows them unless it is told not to. */
	follow = db_text(fx, "SELECT follow_redirects FROM indexer_source");
	g_assert_cmpstr(follow, ==, "t");
	assert_contains(fresh.body, "Follow download redirects to the indexers");
	{
		g_autoptr(GRegex) input = g_regex_new("<input[^>]*follow_redirects[^>]*>", 0, 0, NULL);
		g_autoptr(GMatchInfo) info = NULL;
		g_autofree gchar *tag = NULL;

		g_assert_true(g_regex_match(input, fresh.body, 0, &info));
		tag = g_match_info_fetch(info, 0);
		if (strstr(tag, "checked") == NULL)
			g_error("the box is not ticked: %s", tag);
	}

	g_assert_cmpuint(tested.status, ==, 303);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "3.0.1.5752");
	assert_contains(page.body, ">Downloads</th>");
	assert_contains(page.body, "redirects to the indexer");
	assert_contains(page.body, "through Prowlarr");
	assert_contains(page.body, "followed to the indexers");
	assert_contains(page.body, "DozyNewt and NewsOwl redirect their downloads to the indexer&#39;s own site. Axis "
		"fetches those releases from there");
	assert_lacks(page.body, "Not fetched");
	assert_no_secret(page.body);
	assert_csp_clean(page.body);

	/* The test search: by the identifier and, for the indexers that take
	 * none, by the title; the torrents known by their link address kept,
	 * and what an indexer's site serves marked. */
	found = post_as(fx, fx->admin, one, search, FILM_FORM);
	g_assert_cmpuint(found.status, ==, 200);
	/* A test search of one source shows what it found as it found it (the
	 * merging of several indexers' answers is a search of them all's). */
	assert_contains(found.body, "10 releases found, searched by the IMDb identifier, and by the title for the "
		"indexers that take no identifier.");
	assert_lacks(found.body, "no way to fetch");
	assert_contains(found.body, "Hearth (1995) 1080p BrRip x264 -YIFY");
	assert_contains(found.body, "From the indexer&#39;s site");
	assert_no_secret(found.body);
	assert_lacks(found.body, AXIS_TEST_PROWLARR_INDEXER_KEY);
	assert_csp_clean(found.body);
	save_page("indexers-redirects-followed.html", found.body);

	/* Switched off. */
	saved = post_as(fx, fx->admin, one, one, off);
	g_assert_cmpuint(saved.status, ==, 303);
	g_clear_pointer(&follow, g_free);
	follow = db_text(fx, "SELECT follow_redirects FROM indexer_source");
	g_assert_cmpstr(follow, ==, "f");
	axis_test_reply_clear(&page);
	page = post_as(fx, fx->admin, one, search, FILM_FORM);
	assert_contains(page.body, "not followed");
	assert_contains(page.body, "DozyNewt and NewsOwl redirect their downloads to the indexer&#39;s own site "
		"(Prowlarr&#39;s Redirect option), and following those redirects is switched off for this indexer, so their "
		"releases cannot be fetched. Switch on &quot;Follow download redirects to the indexers&quot; under Settings "
		"for this indexer below, or turn off Redirect for those indexers in Prowlarr.");
	assert_contains(page.body, "Not fetched");
	assert_contains(page.body, "Cannot be fetched");
	assert_no_secret(page.body);
	assert_csp_clean(page.body);
	save_page("indexers-redirects-off.html", page.body);
	list = get_as(fx, fx->admin, "/indexers");
	assert_contains(list.body, "Some of its indexers redirect their downloads, and following redirects is switched off");
	save_page("indexers-redirects-off-list.html", list.body);

	/* On again. */
	axis_test_reply_clear(&saved);
	saved = post_as(fx, fx->admin, one, one, on);
	g_assert_cmpuint(saved.status, ==, 303);
	g_clear_pointer(&follow, g_free);
	follow = db_text(fx, "SELECT follow_redirects FROM indexer_source");
	g_assert_cmpstr(follow, ==, "t");

	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&found);
	axis_test_reply_clear(&saved);
	axis_test_reply_clear(&list);
	axis_test_reply_clear(&fresh);
}

/* ── Native indexers (add-indexer-newznab) ──────────────────────── */

/* A native indexer on a stand-in, made through the layer. */
static gint64
add_native(
	Fixture         *fx,
	AxisTestNewznab *stand_in,
	const gchar     *name,
	gint             priority
){
	AxisIndexerSourceSpec spec;
	g_autofree gchar *base = axis_test_newznab_base(stand_in);
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = name;
	spec.kind = AXIS_INDEXER_NEWZNAB_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.secret = NZB_KEY;
	spec.priority = priority;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
assert_no_native_secret(const gchar *body)
{
	assert_no_secret(body);
	assert_lacks(body, NZB_KEY);
	assert_lacks(body, "t=get");
	assert_lacks(body, "getnzb");
}

/* Task 3.1; spec: Usenet indexer: added from its form, tested, its page
 * says what it can search by, its allowances, and where its files come
 * from, to accept */
static void
test_a_usenet_indexer_on_its_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_newznab_base(fx->nzb);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("kind=newznab&name=NewsOwl&address=%s&base_path=&secret=%s"
		"&movie_categories=2000%%2C+2040&tv_categories=&priority=&enabled=yes", escaped, NZB_KEY);
	g_autofree gchar *bad = g_strdup_printf("kind=newznab&name=Second&address=%s&secret=second-key-value-91"
		"&movie_categories=movies&enabled=yes", escaped);
	AxisTestReply added = post_as(fx, fx->admin, "/indexers", "/indexers", form);
	AxisTestReply refused = post_as(fx, fx->admin, "/indexers", "/indexers", bad);
	AxisTestReply tested;
	AxisTestReply page;
	AxisTestReply found;
	AxisTestReply accepted;
	g_autofree gchar *stored = NULL;

	g_assert_cmpuint(added.status, ==, 303);
	assert_contains(axis_test_reply_header(&added, "Location"), "/indexers/1?done=added");
	stored = db_text(fx, "SELECT kind || '|' || protocol || '|' || movie_categories::text FROM indexer_source");
	g_assert_cmpstr(stored, ==, "newznab|usenet|{2000,2040}");
	/* Refused: its own form again, open, with what was typed, never the key. */
	g_assert_cmpuint(refused.status, ==, 400);
	assert_contains(refused.body, "The categories are Newznab category numbers");
	assert_contains(refused.body, "<details class=\"guide-more\" open=\"open\"><summary>A Usenet indexer (Newznab)");
	assert_contains(refused.body, "value=\"Second\"");
	assert_lacks(refused.body, "second-key-value-91");
	save_page("indexers-add.html", refused.body);

	tested = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/test", NULL);
	g_assert_cmpuint(tested.status, ==, 303);
	page = get_as(fx, fx->admin, "/indexers/1?done=tested");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Newznab (Usenet)");
	assert_contains(page.body, "Working");
	assert_contains(page.body, "Stand-in Newznab 1.0");
	assert_contains(page.body, "What this indexer can search by");
	assert_contains(page.body, "IMDb, words");
	assert_contains(page.body, "TVDB, words; with a season and an episode");
	assert_contains(page.body, "2040 HD");
	/* Counts are written with a comma between thousands (task 3.1). */
	assert_contains(page.body, "12 of 5,000 calls, 3 of 100 files");
	assert_contains(page.body, "at 4,500 calls");
	assert_contains(page.body, "This indexer serves its files from https://files.elsewhere.test:443, not from its own "
		"address.");
	assert_contains(page.body, "Accept files from https://files.elsewhere.test:443");
	assert_contains(page.body, "/indexers/1/accept-files");
	assert_lacks(page.body, "Indexers in this Prowlarr");
	assert_lacks(page.body, "Follow download redirects");
	assert_no_native_secret(page.body);
	assert_csp_clean(page.body);
	save_page("indexers-native.html", page.body);

	/* Its own test search names the indexer of each release. */
	found = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/search", FILM_FORM);
	g_assert_cmpuint(found.status, ==, 200);
	assert_contains(found.body, "6 releases found, searched by the IMDb identifier.");
	assert_contains(found.body, "Hearth.1995.&lt;b&gt;bold&lt;/b&gt;.1080p-MARK");
	assert_contains(found.body, "passworded");
	assert_contains(found.body, "1 result was left out because it named a download address that is not this "
		"indexer&#39;s own");
	assert_no_native_secret(found.body);
	assert_csp_clean(found.body);
	g_assert_cmpuint(axis_test_web_assert_tables(found.body), ==, 1);

	/* Accepted: said so, and shown as its files' address. */
	accepted = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/accept-files", NULL);
	g_assert_cmpuint(accepted.status, ==, 303);
	assert_contains(axis_test_reply_header(&accepted, "Location"), "/indexers/1?done=accepted");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/indexers/1?done=accepted");
	assert_contains(page.body, "The address of its files has been accepted");
	assert_lacks(page.body, "Accept files from https://");
	g_clear_pointer(&stored, g_free);
	stored = db_text(fx, "SELECT files_origin FROM indexer_source");
	g_assert_cmpstr(stored, ==, "https://files.elsewhere.test:443");
	/* The key is not in its settings form either. */
	{
		g_autoptr(GRegex) secret_value = g_regex_new("<input[^>]*type=\"password\"[^>]*value=", 0, 0, NULL);

		g_assert_false(g_regex_match(secret_value, page.body, 0, NULL));
	}
	g_assert_cmpuint(axis_test_newznab_n_bad(fx->nzb), ==, 0);

	axis_test_reply_clear(&added);
	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&found);
	axis_test_reply_clear(&accepted);
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

/* Save, test and accept the files' address come back to the card of the
 * indexer's page they were on. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_newznab_base(fx->nzb);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("kind=newznab&name=NewsOwl&address=%s&base_path=&secret=%s"
		"&movie_categories=2000%%2C+2040&tv_categories=&priority=&enabled=yes", escaped, NZB_KEY);
	AxisTestReply reply = post_as(fx, fx->admin, "/indexers", "/indexers", form);

	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/indexers/1", "/indexers/1", form);
	assert_place(fx, &reply, "settings");
	reply = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/test", NULL);
	assert_place(fx, &reply, "state");
	reply = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/accept-files", NULL);
	assert_place(fx, &reply, "state");
}

/* Task 3.1; spec: One indexer down, Allowance: native indexers and a
 * Prowlarr source in one list, one of them failing and one near its
 * allowance; a search of them all says what each did and which indexer each
 * release came from */
static void
test_indexers_and_sources_together(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *near = NULL;
	g_autofree gchar *items = NULL;
	gint64 prowlarr = add_source(fx, "Prowlarr", KEY);
	gint64 news;
	gint64 slug;
	g_autoptr(GError) error = NULL;
	AxisTestReply list;
	AxisTestReply all;
	AxisIndexerTuning tuning;

	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 2000;
	axis_indexers_set_tuning(fx->indexers, &tuning);
	news = add_native(fx, fx->nzb, "NewsOwl", 10);
	slug = add_native(fx, fx->other, "DozyNewt", 20);
	g_assert_true(axis_indexers_test_source(fx->indexers, prowlarr, NULL, &error));
	g_assert_true(axis_indexers_test_source(fx->indexers, news, NULL, &error));
	/* NewsOwl has used most of its day's calls. */
	items = axis_test_newznab_fixture(fx->nzb, "search-movie.xml");
	near = g_strdup(items);
	{
		gchar **parts = g_strsplit(near, "apicurrent=\"12\"", -1);

		g_free(near);
		near = g_strjoinv("apicurrent=\"4612\"", parts);
		g_strfreev(parts);
	}
	axis_test_newznab_set_search(fx->nzb, NULL, near);
	/* DozyNewt does not answer. */
	axis_test_newznab_set_mode(fx->other, AXIS_TEST_NEWZNAB_DOWN);
	g_assert_true(axis_indexers_test_source(fx->indexers, slug, NULL, &error));
	g_assert_no_error(error);

	all = post_as(fx, fx->admin, "/indexers", "/indexers/search", FILM_FORM);
	g_assert_cmpuint(all.status, ==, 200);
	assert_contains(all.body, "Search every indexer");
	assert_contains(all.body, "Releases found");
	assert_contains(all.body, "kind-indexer-parts");
	assert_lacks(all.body, "Indexer or source");
	/* The test search's form: the title or words and what to look for, and
	 * the identifiers and episode in a group that is open when something was
	 * typed in it (task 6.1). */
	assert_contains(all.body, "<details class=\"guide-more\" open=\"open\"><summary>Identifiers and episode (optional)"
		"</summary>");
	{
		AxisTestReply plain = get_as(fx, fx->admin, "/indexers");

		assert_contains(plain.body, "<details class=\"guide-more\"><summary>Identifiers and episode (optional)</summary>");
		assert_contains(plain.body, "id=\"search-imdb\"");
		assert_lacks(plain.body, "open=\"open\"><summary>Identifiers and episode");
		axis_test_reply_clear(&plain);
	}
	assert_contains(all.body, "No answer: ");
	assert_contains(all.body, "Answered.");
	/* Releases from the native indexer and from Prowlarr's indexers, each
	 * named, Prowlarr's said to come through it; the one both have, once. */
	assert_contains(all.body, "Hearth.1995.2160p.UHD.BluRay.x265-TRK");
	assert_contains(all.body, "Tracker One");
	assert_contains(all.body, "through Prowlarr");
	/* NewsOwl is a native indexer and Prowlarr lists one of that name (with no
	 * address, as a real Prowlarr gives for a private indexer): it is asked
	 * directly only, and the page says so (fix-first-contact 3). */
	assert_contains(all.body, "NewsOwl is asked directly, not through Prowlarr.");
	assert_lacks(all.body, "also at NewsOwl through Prowlarr");
	assert_no_native_secret(all.body);
	assert_csp_clean(all.body);
	g_assert_cmpuint(axis_test_web_assert_tables(all.body), ==, 3);
	save_page("indexers-search-all.html", all.body);

	list = get_as(fx, fx->admin, "/indexers");
	assert_contains(list.body, "Newznab (Usenet)");
	assert_contains(list.body, "Prowlarr");
	assert_contains(list.body, "3 indexers on, 1 failing");
	assert_contains(list.body, "Movies by IMDb, TV by TVDB");
	assert_contains(list.body, "4,612 of 5,000 calls");
	assert_contains(list.body, "Near the limit");
	assert_contains(list.body, "Near its daily allowance: automatic searches pass it over");
	assert_contains(list.body, "Not answering");
	assert_contains(list.body, "Allowance today");
	assert_no_native_secret(list.body);
	assert_csp_clean(list.body);
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	save_page("indexers-list-mixed.html", list.body);

	axis_test_reply_clear(&list);
	axis_test_reply_clear(&all);
}

static guint
count_of(
	const gchar *text,
	const gchar *needle
){
	guint n = 0;
	const gchar *at = text;

	while ((at = strstr(at, needle)) != NULL) {
		n++;
		at += strlen(needle);
	}
	return n;
}

/* add-indexer-sites, task 1.4; spec: Site moved: the add form offers the
 * three public sites with their address filled in and no key; a site is added
 * from it, tested, searched, and its page says its address, its default and
 * when it last answered; one that has moved is shown as failing */
static void
test_a_public_site_on_its_pages(Fixture *fx, gconstpointer data)
{
	AxisTestSites *yts = axis_test_sites_new(AXIS_TEST_SITE_YTS);
	g_autofree gchar *base = axis_test_sites_base(yts);
	g_autofree gchar *escaped = g_uri_escape_string(base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("kind=yts&name=YTS&address=%s&secret=sneaky-key-1234567890&base_path=%%2Fx"
		"&priority=&enabled=yes", escaped);
	g_autofree gchar *stored = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply none;
	AxisTestReply refused;
	AxisTestReply added;
	AxisTestReply tested;
	AxisTestReply page;
	AxisTestReply found;
	AxisTestReply list;
	AxisTestReply moved;

	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_SITE_RATE, "20", &error));
	/* The add blocks: the address of each, ready, and no key beside it. */
	none = get_as(fx, fx->admin, "/indexers");
	g_assert_cmpuint(none.status, ==, 200);
	assert_contains(none.body, "public torrent sites (YTS, EZTV, The Pirate Bay, TorrentProject, LimeTorrents, TheRARBG, "
		"Torrents-CSV, and others through site definitions you install), which need none");
	assert_contains(none.body, "YTS, for movies");
	assert_contains(none.body, "EZTV, for TV");
	assert_contains(none.body, "The Pirate Bay, for movies and TV");
	assert_contains(none.body, "name=\"address\" value=\"" AXIS_INDEXER_YTS_ADDRESS "\"");
	assert_contains(none.body, "name=\"address\" value=\"" AXIS_INDEXER_EZTV_ADDRESS "\"");
	assert_contains(none.body, "name=\"address\" value=\"" AXIS_INDEXER_PIRATEBAY_ADDRESS "\"");
	assert_contains(none.body, "The address it is known at is " AXIS_INDEXER_YTS_ADDRESS ".");
	assert_contains(none.body, "It needs no account and no key.");
	/* The key fields are the three kinds that have a key's (Newznab, Torznab,
	 * Prowlarr), and the sites have no base path, categories or files field. */
	g_assert_cmpuint(count_of(none.body, "type=\"password\""), ==, 3);
	g_assert_cmpuint(count_of(none.body, "id=\"new-yts-"), ==, 5);
	g_assert_null(strstr(none.body, "id=\"new-yts-secret\""));
	g_assert_null(strstr(none.body, "id=\"new-eztv-base-path\""));
	g_assert_null(strstr(none.body, "id=\"new-piratebay-movie-categories\""));
	assert_csp_clean(none.body);

	/* A refused form is shown again, open, with the address as it was. */
	refused = post_as(fx, fx->admin, "/indexers", "/indexers", "kind=yts&name="
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&address=" "https%3A%2F%2Fyts.mx&enabled=yes");
	g_assert_cmpuint(refused.status, ==, 400);
	assert_contains(refused.body, "An indexer&#39;s name is 1 to 64 characters.");
	assert_contains(refused.body, "<details class=\"guide-more\" open=\"open\"><summary>YTS, for movies");
	assert_contains(refused.body, "name=\"address\" value=\"https://yts.mx\"");
	assert_csp_clean(refused.body);
	save_page("indexers-add-site.html", refused.body);

	/* Added: no key is kept, whatever was sent. */
	added = post_as(fx, fx->admin, "/indexers", "/indexers", form);
	g_assert_cmpuint(added.status, ==, 303);
	assert_contains(axis_test_reply_header(&added, "Location"), "/indexers/1?done=added");
	stored = db_text(fx, "SELECT kind || '|' || protocol || '|' || (SELECT count(*) FROM indexer_source_secret) || '|' || "
		"base_path FROM indexer_source");
	g_assert_cmpstr(stored, ==, "yts|torrent|0|");
	tested = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/test", NULL);
	g_assert_cmpuint(tested.status, ==, 303);
	found = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/search", FILM_FORM);
	g_assert_cmpuint(found.status, ==, 200);
	assert_contains(found.body, "4 releases found, searched by the IMDb identifier.");
	assert_contains(found.body, "Hearth (1995) [1080p] [BluRay] [x264] [YTS]");
	assert_no_secret(found.body);
	assert_csp_clean(found.body);
	g_assert_cmpuint(axis_test_web_assert_tables(found.body), ==, 1);

	/* A search of every indexer for an episode: YTS has no TV, nothing is asked
	 * of it, and the table says that, not "Answered" (fix-first-contact 2); a
	 * movie search where it was asked and found nothing still says Answered. */
	{
		AxisTestReply tv = post_as(fx, fx->admin, "/indexers", "/indexers/search",
			"kind=episode&title=Some+Show&year=&imdb=tt0000001&tmdb=&tvdb=&season=1&episode=2");

		g_assert_cmpuint(tv.status, ==, 200);
		assert_contains(tv.body, "Not asked: it has no TV.");
		assert_lacks(tv.body, "Answered.");
		g_assert_cmpuint(axis_test_web_assert_tables(tv.body), >=, 1);
		axis_test_reply_clear(&tv);
	}

	page = get_as(fx, fx->admin, "/indexers/1?done=tested");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "YTS (torrents)");
	assert_contains(page.body, "Working");
	assert_contains(page.body, "Answer read as");
	assert_contains(page.body, "YTS&#39;s list of movies");
	assert_contains(page.body, "Last answered");
	assert_contains(page.body, "Address");
	assert_contains(page.body, "Default address");
	assert_contains(page.body, AXIS_INDEXER_YTS_ADDRESS " (changed here)");
	assert_contains(page.body, "Movies, by IMDb identifier or by title");
	assert_contains(page.body, "What this indexer can search by");
	assert_contains(page.body, "name=\"address\" value=\"");
	assert_lacks(page.body, "API key");
	assert_lacks(page.body, "type=\"password\"");
	assert_lacks(page.body, "Base path");
	assert_lacks(page.body, "Files fetched from");
	assert_lacks(page.body, "Allowance today");
	assert_lacks(page.body, "Follow download redirects");
	assert_lacks(page.body, "Newznab categories for");
	assert_lacks(page.body, "its key");
	assert_no_secret(page.body);
	assert_csp_clean(page.body);
	save_page("indexers-site.html", page.body);

	/* In the list: its address, what it covers, no complaint about a key. */
	list = get_as(fx, fx->admin, "/indexers");
	assert_contains(list.body, "YTS (torrents)");
	assert_contains(list.body, "Movies, by IMDb identifier or by title");
	assert_lacks(list.body, "No API key is set");
	g_assert_cmpuint(axis_test_web_assert_tables(list.body), ==, 1);
	save_page("indexers-list-site.html", list.body);

	/* The site moves to an address nothing answers at: the page says so. */
	axis_test_sites_set_mode(yts, AXIS_TEST_SITES_GONE);
	axis_test_reply_clear(&tested);
	tested = post_as(fx, fx->admin, "/indexers/1", "/indexers/1/test", NULL);
	g_assert_cmpuint(tested.status, ==, 303);
	moved = get_as(fx, fx->admin, "/indexers/1?done=tested");
	assert_contains(moved.body, "Failing");
	assert_contains(moved.body, "change its address here");
	assert_csp_clean(moved.body);
	g_assert_cmpuint(axis_test_sites_n_bad(yts), ==, 0);

	axis_test_reply_clear(&none);
	axis_test_reply_clear(&refused);
	axis_test_reply_clear(&added);
	axis_test_reply_clear(&tested);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&found);
	axis_test_reply_clear(&list);
	axis_test_reply_clear(&moved);
	axis_test_sites_free(yts);
}

/* Task 1.3: the expiry job is on an existing queue (no queue of its own) */
static void
test_the_expiry_job_is_on_the_maintenance_queue(Fixture *fx, gconstpointer data)
{
	const AxisModuleInfo *module = axis_web_indexers_get_module();
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;

	g_assert_cmpstr(module->job_handlers[0].kind, ==, AXIS_INDEXER_EXPIRE_KIND);
	g_assert_cmpstr(module->job_handlers[0].queue, ==, AXIS_JOB_MAINTENANCE_QUEUE);
	/* add-indexer-newznab: the capabilities' job, on the same queue, with a
	 * label and a class. */
	g_assert_cmpstr(module->job_handlers[1].kind, ==, AXIS_INDEXER_CAPS_KIND);
	g_assert_cmpstr(module->job_handlers[1].queue, ==, AXIS_JOB_MAINTENANCE_QUEUE);
	g_assert_nonnull(module->job_handlers[1].label);
	g_assert_cmpint(module->job_handlers[1].job_class, ==, AXIS_JOB_CLASS_UPKEEP);
	g_assert_null(module->job_handlers[2].kind);
	g_assert_cmpstr(module->job_schedules[0].kind, ==, AXIS_INDEXER_EXPIRE_KIND);
	g_assert_cmpstr(module->job_schedules[1].kind, ==, AXIS_INDEXER_CAPS_KIND);
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[0], fx->ui, &error));
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[1], fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_job_registry_contains(registry, AXIS_INDEXER_EXPIRE_KIND));
	g_assert_true(axis_job_registry_contains(registry, AXIS_INDEXER_CAPS_KIND));

	/* The handler forgets what is past its life. */
	axis_test_db_exec(fx->db, "INSERT INTO indexer_source (name, kind, address) VALUES ('s', 'prowlarr', 'http://127.0.0.1:1')");
	axis_test_db_exec(fx->db, "INSERT INTO indexer_search (source_id, request_key, mode, n_releases, expires_at)"
		" SELECT id, repeat('a', 64), 'text', 0, now() - interval '1 minute' FROM indexer_source");
	{
		g_autoptr(GError) failure = NULL;
		guint n = 0;

		g_assert_true(axis_indexers_expire(fx->indexers, &n, &failure));
		g_assert_cmpuint(n, ==, 1);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web/indexers/" path, Fixture, NULL, func)
	ADD("administrators-only", test_the_page_is_for_administrators);
	ADD("no-token-no-get", test_nothing_is_sent_without_a_token_or_by_a_get);
	ADD("none", test_the_page_with_no_source);
	ADD("add-save-test", test_add_save_and_test_a_source);
	ADD("search", test_a_test_search_shows_raw_names_as_text);
	ADD("search-invalid", test_a_test_search_that_cannot_be_made);
	ADD("failing", test_a_failing_source_is_shown_as_it_is);
	ADD("remove", test_removing_a_source);
	ADD("native/page", test_a_usenet_indexer_on_its_page);
	ADD("back-to-place", test_back_to_place);
	ADD("native/together", test_indexers_and_sources_together);
	ADD("sites/pages", test_a_public_site_on_its_pages);
	ADD("expiry-job", test_the_expiry_job_is_on_the_maintenance_queue);
	AXIS_TEST_ADD_DB_CASE("/web/indexers/redirects", Fixture, GUINT_TO_POINTER(1), test_indexers_that_redirect_are_shown);
#undef ADD
	return g_test_run();
}
