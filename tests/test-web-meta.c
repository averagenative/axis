/*
 * test-web-meta.c - the Movie and TV details page and the artwork route, with the real
 * shell on an ephemeral loopback port, the fixture provider and its fake
 * servers, a real database and a data directory of the test's own
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (metadata): Non-administrator refused; Notice shown;
 * Search while the provider is down (a notice, not an error page); Served
 * locally (every image address is the daemon's own); Session required; Key
 * stays put (in no page).
 *
 * With AXIS_TEST_META_PAGES set to a directory, the pages are also saved
 * there (metadata.html, metadata-down.html, metadata-empty.html), for
 * looking at in a browser.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "axis-test-plex.h"
#include "axis-test-tvdb.h"
#include "core/axis-module-state.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-settings.h"

#define KEY AXIS_TEST_META_KEY

#define TVDB_KEY AXIS_TEST_TVDB_KEY

/* A case whose fixture also registers TheTVDB, pointed at its stand-in
 * (tests/axis-test-tvdb.h). */
static const gchar with_tvdb[] = "with TheTVDB";

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
	/* With TheTVDB only. */
	AxisTestTvdb        *tvdb_stand;
	AxisTestPlex        *tvdb;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning tuning;

	memset(fx, 0, sizeof(*fx));
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
	if (data == with_tvdb) {
		/* TheTVDB as the daemon registers it, after the provider of titles,
		 * but for its addresses. */
		fx->tvdb_stand = axis_test_tvdb_new();
		fx->tvdb = axis_test_tvdb_api(fx->tvdb_stand);
		g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb_stand), &error));
	}
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

	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", KEY, &error));
	g_assert_no_error(error);
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
	axis_test_tvdb_free(fx->tvdb_stand);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
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
	const gchar *dir = g_getenv("AXIS_TEST_META_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gboolean
posters_kept(gpointer data)
{
	Fixture *fx = data;
	g_autofree gchar *n = axis_test_db_query(fx->db, "SELECT count(*) FROM meta_artwork WHERE sha256 IS NOT NULL");

	return g_strcmp0(n, "2") == 0;
}

/* Spec: Non-administrator refused */
static void
test_page_for_administrators(Fixture *fx, gconstpointer data)
{
	AxisTestReply viewer = get_as(fx, fx->viewer, "/metadata");
	AxisTestReply admin = get_as(fx, fx->admin, "/metadata");
	AxisTestReply home = get_as(fx, fx->viewer, "/");

	g_assert_cmpuint(viewer.status, ==, 403);
	g_assert_cmpuint(admin.status, ==, 200);
	g_assert_cmpuint(axis_test_web_assert_tables(admin.body), ==, 1);
	assert_csp_clean(admin.body);
	assert_contains(admin.body, "kind-meta-providers");
	assert_contains(admin.body, "Fixture");
	assert_contains(admin.body, "movies, series");
	assert_contains(admin.body, ">Set<");
	assert_contains(admin.body, ">Ready<");
	assert_contains(admin.body, "50 requests a second");
	assert_contains(admin.body, "Requests since Axis started");
	/* Spec: Notice shown (on the page about the provider too) */
	assert_contains(admin.body, "Test data from the fixture provider");
	assert_contains(admin.body, "Details and artwork kept by Axis");
	assert_contains(admin.body, "Update out-of-date details now");
	assert_contains(admin.body, "Check a key with a lookup");
	/* Spec: Key stays put (in no page) */
	assert_lacks(admin.body, KEY);
	/* In the System tab, for administrators only. */
	assert_contains(admin.body, "href=\"/metadata\"");
	assert_lacks(home.body, "href=\"/metadata\"");
	save_page("metadata-empty.html", admin.body);

	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&home);
}

/* The Settings page shows the provider's key as a secret: set or not,
 * never its value. */
static void
test_key_on_the_settings_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/admin/settings");

	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "metadata.fixture_key");
	assert_contains(page.body, "Fixture token");
	assert_contains(page.body, "Requests a second (Fixture)");
	assert_lacks(page.body, KEY);
	axis_test_reply_clear(&page);
}

/* Spec: Notice shown; Served locally */
static void
test_lookup_shows_results(Fixture *fx, gconstpointer data)
{
	AxisTestReply first = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=mazework&year=");
	AxisTestReply again;

	g_assert_cmpuint(first.status, ==, 200);
	assert_contains(first.body, ">The Mazework</a>");
	assert_contains(first.body, "Movie, 1999");
	assert_contains(first.body, ">The Mazework Reloaded</a>");
	assert_contains(first.body, "Movie, 2003");
	assert_contains(first.body, "Answered");
	assert_contains(first.body, "Fixture: Test data from the fixture provider");
	assert_contains(first.body, "IMDb tt0600003");
	/* Posters are fetched in the background the first time: until they are
	 * kept a card holds a placeholder with its title in it. */
	assert_contains(first.body, "poster-none");
	assert_contains(first.body, "<span class=\"poster-title\">The Mazework</span>");
	assert_images_local(first.body);
	assert_csp_clean(first.body);
	assert_lacks(first.body, KEY);
	g_assert_true(axis_test_pump_until(posters_kept, fx, AXIS_TEST_WAIT_MS));

	again = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=mazework");
	g_assert_cmpuint(again.status, ==, 200);
	g_assert_cmpuint(assert_images_local(again.body), ==, 2);
	g_assert_cmpuint(axis_test_web_assert_tables(again.body), ==, 1);
	save_page("metadata.html", again.body);
	/* Asked once: the second page came from the store. */
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/search/film"), ==, 1);

	axis_test_reply_clear(&first);
	axis_test_reply_clear(&again);
}

/* Spec: Search while the provider is down */
static void
test_provider_down_is_a_notice(Fixture *fx, gconstpointer data)
{
	AxisTestReply first = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=mazework");
	AxisTestReply down;
	AxisTestReply nothing;
	AxisTestReply bad;

	g_assert_cmpuint(first.status, ==, 200);
	axis_test_db_exec(fx->db, "UPDATE meta_search SET fetched_at = now() - interval '3 days'");
	axis_test_plex_route(fx->fa->api, "/search/film", -1, AXIS_TEST_PLEX_STATUS, 503, NULL, 503);
	down = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=mazework");
	g_assert_cmpuint(down.status, ==, 200);
	assert_contains(down.body, "notice-warning");
	assert_contains(down.body, "Showing the saved results from");
	assert_contains(down.body, "503");
	/* The providers' table says so too, after the lookup. */
	assert_contains(down.body, ">Failing<");
	assert_contains(down.body, "Fixture answered 503 (");
	assert_contains(down.body, ">The Mazework</a>");
	assert_contains(down.body, "Movie, 1999");
	assert_contains(down.body, "Test data from the fixture provider");
	save_page("metadata-down.html", down.body);

	nothing = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=nothing+saved");
	g_assert_cmpuint(nothing.status, ==, 200);
	assert_contains(nothing.body, "Nothing was saved for this lookup");

	/* A question that cannot be asked is said so, on the page. */
	bad = get_as(fx, fx->admin, "/metadata?provider=fixture&kind=film&q=mazework&year=19x9");
	g_assert_cmpuint(bad.status, ==, 200);
	assert_contains(bad.body, "four digits");

	axis_test_reply_clear(&first);
	axis_test_reply_clear(&down);
	axis_test_reply_clear(&nothing);
	axis_test_reply_clear(&bad);
}

/* Spec: Session required, and how artwork is served */
static void
test_artwork_route(Fixture *fx, gconstpointer data)
{
	g_autoptr(GBytes) png = axis_test_meta_png();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *upper = NULL;
	g_autofree gchar *upper_path = NULL;
	AxisTestReply admin;
	AxisTestReply viewer;
	AxisTestReply anonymous;
	const gchar *const missing[] = {
		"/art/0000000000000000000000000000000000000000000000000000000000000000",
		"/art/abc",
		"/art/000000000000000000000000000000000000000000000000000000000000000g",
		"/art/00000000000000000000000000000000000000000000000000000000000000000",
		"/art/%2e%2e%2fsecret",
		NULL
	};
	guint i;

	g_assert_true(axis_meta_artwork_fetch(fx->meta, "fixture", "/p101.png", AXIS_META_ART_POSTER_SMALL, &hash, &error));
	g_assert_no_error(error);
	path = g_strdup_printf("/art/%s", hash);

	admin = get_as(fx, fx->admin, path);
	g_assert_cmpuint(admin.status, ==, 200);
	g_assert_cmpstr(axis_test_reply_header(&admin, "Content-Type"), ==, "image/png");
	g_assert_cmpstr(axis_test_reply_header(&admin, "X-Content-Type-Options"), ==, "nosniff");
	g_assert_cmpstr(axis_test_reply_header(&admin, "Cache-Control"), ==, "private, max-age=31536000, immutable");
	g_assert_nonnull(axis_test_reply_header(&admin, "Content-Security-Policy"));
	g_assert_cmpuint(admin.length, ==, g_bytes_get_size(png));
	g_assert_cmpmem(admin.body, admin.length, g_bytes_get_data(png, NULL), admin.length);

	/* Any logged-in user: every page may show artwork. */
	viewer = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(viewer.status, ==, 200);

	/* Spec: Session required */
	anonymous = get_as(fx, NULL, path);
	g_assert_cmpuint(anonymous.status, !=, 200);
	g_assert_null(g_strstr_len(anonymous.body, (gssize)anonymous.length, "IHDR"));

	for (i = 0; missing[i] != NULL; i++) {
		AxisTestReply reply = get_as(fx, fx->admin, missing[i]);

		g_assert_cmpuint(reply.status, >=, 400);
		g_assert_cmpuint(reply.status, <, 500);
		axis_test_reply_clear(&reply);
	}
	upper = g_ascii_strup(hash, -1);
	upper_path = g_strdup_printf("/art/%s", upper);
	{
		AxisTestReply reply = get_as(fx, fx->admin, upper_path);

		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
	}
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&anonymous);
}

static void
test_refresh_now(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/metadata");
	g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
	const gchar *extra[3];
	AxisTestReply first;
	AxisTestReply second;
	AxisTestReply viewer;
	AxisTestReply done;

	g_assert_nonnull(csrf);
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf;
	extra[2] = NULL;
	first = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/refresh", fx->admin, extra, "");
	g_assert_cmpuint(first.status, ==, 303);
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&first, "Location"), "/metadata?done=refresh-queued&at="));
	second = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/refresh", fx->admin, extra, "");
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&second, "Location"), "/metadata?done=refresh-exists&at="));
	{
		g_autofree gchar *n = axis_test_db_query(fx->db, "SELECT count(*) FROM job WHERE kind = '"
			AXIS_META_REFRESH_KIND "' AND queue = '" AXIS_META_QUEUE "'");

		g_assert_cmpstr(n, ==, "1");
	}
	viewer = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/refresh", fx->viewer, NULL, "");
	g_assert_cmpuint(viewer.status, >=, 400);
	done = get_as(fx, fx->admin, "/metadata?done=refresh-queued");
	assert_contains(done.body, "An update of out-of-date details is queued");
	assert_contains(done.body, "queued");

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&first);
	axis_test_reply_clear(&second);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&done);
}

/* The refresh comes back to its card, with the notice in it. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/metadata");
	g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
	const gchar *extra[3];
	AxisTestReply reply;
	const gchar *location;
	g_autofree gchar *path = NULL;
	const gchar *element;
	const gchar *notice;

	g_assert_nonnull(csrf);
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf;
	extra[2] = NULL;
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/refresh", fx->admin, extra, "");
	g_assert_cmpuint(reply.status, ==, 303);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_nonnull(strstr(location, "at=refresh"));
	g_assert_true(g_str_has_suffix(location, "#refresh"));
	path = g_strndup(location, strrchr(location, '#') - location);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, "id=\"refresh\"");
	notice = strstr(page.body, "notice-here");
	g_assert_nonnull(element);
	g_assert_nonnull(notice);
	g_assert_true(element < notice);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&reply);
}

/*
 * The latest refresh is the one going on, or else the one that ended last:
 * a refresh a person retried from the Jobs page runs as itself, under its
 * older id, after newer ones ended.
 */
static void
test_latest_refresh_retried(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	g_autofree gchar *older = NULL;
	g_autofree gchar *newer = NULL;

	older = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, "
		"earlier_runs) VALUES ('" AXIS_META_REFRESH_KIND "', '" AXIS_META_QUEUE "', 1, 'queued', 0, "
		"'[{\"state\": \"failed\", \"attempts\": 1}]') RETURNING id");
	newer = axis_test_db_query(fx->db, "INSERT INTO job (kind, queue, max_attempts, state, attempts, started_at, "
		"finished_at) VALUES ('" AXIS_META_REFRESH_KIND "', '" AXIS_META_QUEUE "', 1, 'succeeded', 1, now(), "
		"now()) RETURNING id");
	g_assert_cmpint(g_ascii_strtoll(older, NULL, 10), <, g_ascii_strtoll(newer, NULL, 10));

	page = get_as(fx, fx->admin, "/metadata");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Latest update of details");
	assert_contains(page.body, "<dd>Queued</dd>");
	g_assert_null(strstr(page.body, "<dd>Succeeded"));
	axis_test_reply_clear(&page);
}

/* ── TheTVDB on the page (add-metadata-tvdb, V1) ────────────────── */

/* Spec: No key: TheTVDB is offered nowhere, its notice is not shown, and
 * nothing is sent to it. */
static void
test_tvdb_without_key(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->admin, "/metadata");

	g_assert_cmpuint(page.status, ==, 200);
	assert_lacks(page.body, "Test TheTVDB key");
	assert_lacks(page.body, "Metadata provided by TheTVDB");
	assert_lacks(page.body, "value=\"tvdb\"");
	/* Its row says it has no key, which is where an administrator sets it. */
	assert_contains(page.body, "TheTVDB");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tvdb), ==, 0);
}

static AxisTestReply
post_test_key(Fixture *fx)
{
	AxisTestReply page = get_as(fx, fx->admin, "/metadata");
	g_autofree gchar *csrf = axis_test_web_csrf_token(page.body);
	const gchar *extra[3];
	AxisTestReply reply;

	g_assert_nonnull(csrf);
	extra[0] = "X-CSRF-Token";
	extra[1] = csrf;
	extra[2] = NULL;
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/test-key", fx->admin, extra,
		"provider=tvdb");
	axis_test_reply_clear(&page);
	return reply;
}

/* Spec: Testing the key; TheTVDB's notice on the Metadata page, with its
 * link. */
static void
test_tvdb_test_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	AxisTestReply page;
	AxisTestReply reply;
	AxisTestReply viewer;
	const gchar *location;

	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, TVDB_KEY, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/metadata");
	assert_contains(page.body, "Test TheTVDB key");
	assert_contains(page.body, "Metadata provided by TheTVDB. Please consider adding missing information or "
		"subscribing.");
	/* A space keeps the link from running into the sentence. */
	assert_contains(page.body, "subscribing. </span>");
	assert_contains(page.body, "href=\"https://thetvdb.com/subscribe\"");
	assert_lacks(page.body, TVDB_KEY);
	axis_test_reply_clear(&page);
	/* Opening the page sends nothing. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tvdb), ==, 0);

	/* Review of V1/V2, blocker 1: TheTVDB is never searched by name, even
	 * when the address asks for it, and makes no title. */
	page = get_as(fx, fx->admin, "/metadata");
	assert_lacks(page.body, "<option value=\"tvdb\"");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/metadata?provider=tvdb&kind=series&q=Wobbly+Lighthouse");
	g_assert_cmpuint(page.status, ==, 200);
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_count(fx->tvdb, "/v4/search"), ==, 0);
	{
		g_autofree gchar *n = axis_test_db_query(fx->db, "SELECT count(*) FROM meta_title WHERE provider = 'tvdb'");

		g_assert_cmpstr(n, ==, "0");
	}
	axis_test_plex_reset_requests(fx->tvdb);

	reply = post_test_key(fx);
	g_assert_cmpuint(reply.status, ==, 303);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_true(g_str_has_prefix(location, "/metadata?done=key-tested"));
	g_assert_true(g_str_has_suffix(location, "#providers"));
	page = get_as(fx, fx->admin, "/metadata?done=key-tested&at=providers");
	assert_contains(page.body, "The key was tested.");
	assert_contains(page.body, "TheTVDB accepted the key and lists 4 episode orders: Aired Order, DVD Order, "
		"Absolute Order, Alternate Order.");
	assert_lacks(page.body, TVDB_KEY);
	assert_lacks(page.body, AXIS_TEST_TVDB_TOKEN_PREFIX);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&reply);

	/* A login and the list of orders: nothing about any title. */
	requests = axis_test_plex_requests(fx->tvdb);
	g_assert_cmpuint(requests->len, ==, 2);
	g_assert_true(g_str_has_prefix(g_ptr_array_index(requests, 0), "POST /v4/login\n"));
	g_assert_true(g_str_has_prefix(g_ptr_array_index(requests, 1), "GET /v4/seasons/types\n"));

	/* Administrators only. */
	viewer = axis_test_web_request(fx->session, fx->port, "POST", "/metadata/test-key", fx->viewer, NULL,
		"provider=tvdb");
	g_assert_cmpuint(viewer.status, >=, 400);
	axis_test_reply_clear(&viewer);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tvdb), ==, 2);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO | G_LOG_LEVEL_MESSAGE |
		G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL, "*" TVDB_KEY "*"), ==, 0);
}

/* Spec: Key refused: the page says so, offers no test of the same key, and
 * nothing more is sent. */
static void
test_tvdb_key_refused(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply page;
	AxisTestReply reply;

	axis_test_tvdb_refuse(fx->tvdb_stand, TRUE);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, TVDB_KEY, &error));
	reply = post_test_key(fx);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	page = get_as(fx, fx->admin, "/metadata");
	assert_contains(page.body, "Key refused");
	assert_contains(page.body, "TheTVDB refused this key, so nothing is sent to it.");
	assert_lacks(page.body, "Test TheTVDB key");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tvdb), ==, 1);

	/* A second test, sent by hand, sends nothing either. */
	reply = post_test_key(fx);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->tvdb), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web-meta/page/administrators", Fixture, NULL, test_page_for_administrators);
	AXIS_TEST_ADD_DB_CASE("/web-meta/page/settings", Fixture, NULL, test_key_on_the_settings_page);
	AXIS_TEST_ADD_DB_CASE("/web-meta/lookup/results", Fixture, NULL, test_lookup_shows_results);
	AXIS_TEST_ADD_DB_CASE("/web-meta/lookup/provider-down", Fixture, NULL, test_provider_down_is_a_notice);
	AXIS_TEST_ADD_DB_CASE("/web-meta/art", Fixture, NULL, test_artwork_route);
	AXIS_TEST_ADD_DB_CASE("/web-meta/refresh-now", Fixture, NULL, test_refresh_now);
	AXIS_TEST_ADD_DB_CASE("/web/meta/back-to-place", Fixture, NULL, test_back_to_place);
	AXIS_TEST_ADD_DB_CASE("/web-meta/latest-refresh-retried", Fixture, NULL, test_latest_refresh_retried);
	AXIS_TEST_ADD_DB_CASE("/web-meta/tvdb/without-key", Fixture, with_tvdb, test_tvdb_without_key);
	AXIS_TEST_ADD_DB_CASE("/web-meta/tvdb/test-key", Fixture, with_tvdb, test_tvdb_test_key);
	AXIS_TEST_ADD_DB_CASE("/web-meta/tvdb/key-refused", Fixture, with_tvdb, test_tvdb_key_refused);
	return g_test_run();
}
