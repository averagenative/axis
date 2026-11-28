/*
 * test-web-discover.c - the Discover pages with the real shell on a loopback
 * port, the movies and TV modules, a real database and volume, and a fake
 * TMDb serving hand-written fixtures with made-up titles
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (add-discover, discover): Viewing a page; The first view;
 * A wanted movie; Hiding what you have; Paging; Hiding a title; Showing it
 * again; Not for everyone; No key; The section.  (movies) Adding from
 * Discover; Nowhere to put it; Not a local path.  (tv) Adding from Discover.
 * Task 3.4.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-discover.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "discover/axis-discover.h"
#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-discover.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                film_root;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisTestDiscoverTmdb *tmdb;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisMovies           *movies;
	AxisTv               *tv;
	AxisDiscover         *discover;
	AxisWebDiscover      *discover_ui;
	AxisModuleRegistry   *registry;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;
	gchar                *viewer;
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
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->film_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->tmdb = axis_test_discover_tmdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->tmdb->info, &error));
	memset(&tuning, 0, sizeof(tuning));
	tuning.request_timeout_ms = 3000;
	tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	fx->discover = axis_discover_new(fx->t->pool, fx->settings, fx->meta);
	fx->discover_ui = axis_web_discover_new(fx->discover);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), fx->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), fx->tv, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_discover_get_module(), fx->discover_ui,
		&error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, AXIS_TEST_DISCOVER_TOKEN, &error));
	g_assert_no_error(error);

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
	if (fx->t == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_stop(fx->web);
	axis_web_discover_stop(fx->discover_ui);
	axis_movies_stop(fx->movies);
	axis_tv_stop(fx->tv);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_discover_free(fx->discover_ui);
	axis_discover_unref(fx->discover);
	axis_tv_unref(fx->tv);
	axis_movies_unref(fx->movies);
	axis_web_quality_free(fx->quality_ui);
	axis_quality_store_unref(fx->quality);
	axis_web_downloads_free(fx->downloads_ui);
	axis_downloads_unref(fx->downloads);
	axis_web_indexers_free(fx->indexers_ui);
	axis_indexers_unref(fx->indexers);
	axis_web_meta_free(fx->meta_ui);
	axis_meta_unref(fx->meta);
	axis_web_settings_free(fx->web_settings);
	axis_test_discover_tmdb_free(fx->tmdb);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
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

/* A form posted with the CSRF token of the page @token_from (NULL: none). */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *token_from,
	const gchar *path,
	const gchar *form
){
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;

	if (token_from != NULL) {
		AxisTestReply opened = get_as(fx, cookie, token_from);

		token = axis_test_web_csrf_token(opened.body);
		axis_test_reply_clear(&opened);
	}
	escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");
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

static guint
count_of(
	const gchar *haystack,
	const gchar *needle
){
	const gchar *p = haystack;
	guint n = 0;

	while (p != NULL && (p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);
	g_autoptr(GRegex) handler = g_regex_new("<[a-zA-Z][^>]*\\son[a-z]+=", 0, 0, NULL);

	g_assert_null(strstr(body, "style=\""));
	g_assert_null(strstr(body, "<style"));
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	g_assert_false(g_regex_match(handler, body, 0, NULL));
	g_assert_null(strstr(body, "javascript:"));
}

static void
query_is(
	Fixture     *fx,
	const gchar *sql,
	const gchar *expected
){
	g_autofree gchar *value = axis_test_db_query(fx->t->db, sql);

	g_assert_cmpstr(value, ==, expected);
}

static void
refresh(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_discover_run_refresh(fx->discover, NULL, FALSE, NULL, &error));
	g_assert_no_error(error);
	axis_test_plex_reset_requests(fx->tmdb->api);
}

static gint64
title_id(
	Fixture     *fx,
	const gchar *kind,
	const gchar *tmdb
){
	g_autofree gchar *text = axis_test_db_queryf(fx->t->db, "SELECT title_id FROM meta_external_id WHERE kind = '%s'"
		" AND scheme = 'tmdb' AND value = '%s'", kind, tmdb);

	g_assert_nonnull(text);
	return g_ascii_strtoll(text, NULL, 10);
}

/* Markup's text, its five entities undone. */
static gchar *
unescape(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = text; *p != '\0'; p++) {
		if (g_str_has_prefix(p, "&amp;")) {
			g_string_append_c(out, '&');
			p += 4;
		} else if (g_str_has_prefix(p, "&quot;")) {
			g_string_append_c(out, '"');
			p += 5;
		} else if (g_str_has_prefix(p, "&#39;")) {
			g_string_append_c(out, '\'');
			p += 4;
		} else if (g_str_has_prefix(p, "&lt;")) {
			g_string_append_c(out, '<');
			p += 3;
		} else if (g_str_has_prefix(p, "&gt;")) {
			g_string_append_c(out, '>');
			p += 3;
		} else {
			g_string_append_c(out, *p);
		}
	}
	return g_string_free(out, FALSE);
}

/*
 * The body a browser would post for the first form of @html that posts to
 * @action and holds @needle: every input of it, name and value, as the page
 * wrote them.  %NULL when there is no such form.
 */
static gchar *
form_body(
	const gchar *html,
	const gchar *action,
	const gchar *needle
){
	g_autofree gchar *want = g_strdup_printf("action=\"%s\"", action);
	g_autoptr(GRegex) input = g_regex_new("<input[^>]*>", 0, 0, NULL);
	g_autoptr(GRegex) name = g_regex_new("name=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) value = g_regex_new("value=\"([^\"]*)\"", 0, 0, NULL);
	const gchar *p = html;

	while ((p = strstr(p, "<form")) != NULL) {
		const gchar *end = strstr(p, "</form>");
		g_autofree gchar *form = NULL;
		g_autoptr(GMatchInfo) inputs = NULL;
		GString *body;

		if (end == NULL)
			return NULL;
		form = g_strndup(p, (gsize)(end - p));
		p = end;
		if (strstr(form, want) == NULL || strstr(form, needle) == NULL)
			continue;
		body = g_string_new(NULL);
		g_regex_match(input, form, 0, &inputs);
		while (g_match_info_matches(inputs)) {
			g_autofree gchar *tag = g_match_info_fetch(inputs, 0);
			g_autoptr(GMatchInfo) n = NULL;
			g_autoptr(GMatchInfo) v = NULL;

			if (g_regex_match(name, tag, 0, &n) && g_regex_match(value, tag, 0, &v)) {
				g_autofree gchar *key = g_match_info_fetch(n, 1);
				g_autofree gchar *raw = g_match_info_fetch(v, 1);
				g_autofree gchar *plain = unescape(raw);
				g_autofree gchar *encoded = g_uri_escape_string(plain, NULL, TRUE);

				g_string_append_printf(body, "%s%s=%s", body->len > 0 ? "&" : "", key, encoded);
			}
			g_match_info_next(inputs, NULL);
		}
		return g_string_free(body, FALSE);
	}
	return NULL;
}

/* ── Without a key, and the first view ──────────────────────────── */

static void
test_no_key(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply admin;
	AxisTestReply viewer;

	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TMDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	admin = get_as(fx, fx->admin, "/discover/movies");
	g_assert_cmpuint(admin.status, ==, 200);
	assert_contains(admin.body, "Discover needs a TMDb token");
	assert_contains(admin.body, "Add a TMDb token in Settings");
	assert_contains(admin.body, "href=\"/admin/settings#module-metadata\"");
	assert_lacks(admin.body, "Fetch the lists now");
	viewer = get_as(fx, fx->viewer, "/discover/tv");
	g_assert_cmpuint(viewer.status, ==, 200);
	assert_contains(viewer.body, "An administrator can add one in Settings.");
	/* Nothing asked, and nothing queued. */
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);
	query_is(fx, "SELECT count(*) FROM job WHERE kind = 'discover.refresh'", "0");
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&viewer);
}

static void
test_first_view_queues(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply = get_as(fx, fx->viewer, "/discover/movies");
	AxisTestReply again;

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis is fetching the lists from TMDb for the first time");
	query_is(fx, "SELECT count(*) FROM job WHERE kind = 'discover.refresh' AND state = 'queued'", "1");
	query_is(fx, "SELECT payload ->> 'all' FROM job WHERE kind = 'discover.refresh'", NULL);
	again = get_as(fx, fx->viewer, "/discover/movies");
	query_is(fx, "SELECT count(*) FROM job WHERE kind = 'discover.refresh'", "1");
	/* The page asked TMDb nothing: the job will. */
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&again);
}

/* ── The pages ──────────────────────────────────────────────────── */

static void
test_movies_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	AxisTestReply viewer;
	AxisTestReply tv;

	refresh(fx);
	reply = get_as(fx, fx->admin, "/discover/movies");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<h1>Discover movies</h1>");
	assert_contains(reply.body, "Find a movie to add when you do not know its name");
	assert_contains(reply.body, ">Popular<");
	assert_contains(reply.body, ">Trending this week<");
	assert_contains(reply.body, ">In cinemas<");
	assert_contains(reply.body, ">Coming soon<");
	assert_contains(reply.body, ">Top rated<");
	assert_contains(reply.body, ">Recommended for you<");
	assert_contains(reply.body, "The Brass Orchard");
	assert_contains(reply.body, "2024, rated 7.4 of 10");
	assert_contains(reply.body, "2027, no rating yet");
	assert_contains(reply.body, "Showing 1 to 7 of 7 movies.");
	assert_contains(reply.body, "Fetched from TMDb on");
	assert_contains(reply.body, "once it is 12 hours old");
	assert_contains(reply.body, "TMDb: ");
	assert_contains(reply.body, "Fetch the lists now");
	/* Every card has its one-step add, Options and Not interested. */
	g_assert_cmpuint(count_of(reply.body, "action=\"/films\""), ==, 7);
	g_assert_cmpuint(count_of(reply.body, "action=\"/discover/dismiss\""), ==, 7);
	assert_contains(reply.body, ">Add with options<");
	assert_contains(reply.body, "name=\"back\" value=\"/discover/movies\"");
	assert_lacks(reply.body, "Velvet Ledger");
	assert_csp_clean(reply.body);
	/* The section of Media, beside Movies and TV. */
	assert_contains(reply.body, "href=\"/discover/tv\"");
	assert_contains(reply.body, "href=\"/discover/not-interested\"");
	assert_contains(reply.body, ">Discover<");

	viewer = get_as(fx, fx->viewer, "/discover/movies");
	g_assert_cmpuint(viewer.status, ==, 200);
	assert_contains(viewer.body, "The Brass Orchard");
	assert_lacks(viewer.body, "action=\"/films\"");
	assert_lacks(viewer.body, "action=\"/discover/dismiss\"");
	assert_lacks(viewer.body, "Fetch the lists now");

	tv = get_as(fx, fx->viewer, "/discover/tv?list=now");
	g_assert_cmpuint(tv.status, ==, 200);
	assert_contains(tv.body, "<h1>Discover TV</h1>");
	assert_contains(tv.body, ">On the air<");
	assert_lacks(tv.body, ">Coming soon<");
	assert_contains(tv.body, "Northbound Signals");
	assert_lacks(tv.body, "The Tidewater Archive");

	/* No page view asked TMDb's API anything. */
	g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&tv);
}

static void
test_filters_and_paging(Fixture *fx, gconstpointer data)
{
	g_autoptr(GString) json = g_string_new("{\"page\": 1, \"total_pages\": 1, \"results\": [");
	AxisTestReply reply;
	guint i;

	for (i = 0; i < 25; i++)
		g_string_append_printf(json, "%s{\"id\": %u, \"title\": \"Generated Feature %u\", \"release_date\":"
			" \"2020-01-01\", \"vote_average\": 6.0, \"vote_count\": 5, \"genre_ids\": [18]}", i > 0 ? "," : "", 8100 + i,
			i + 1);
	g_string_append(json, "]}");
	axis_test_discover_tmdb_set_body(fx->tmdb, "/3/movie/top_rated", 0, json->str);
	refresh(fx);

	reply = get_as(fx, fx->viewer, "/discover/movies?list=popular&genre=18");
	assert_contains(reply.body, "The Brass Orchard");
	assert_lacks(reply.body, "Paper Tide");
	assert_contains(reply.body, "Showing 1 to 3 of 3 movies.");
	assert_contains(reply.body, "Clear the filters");
	assert_contains(reply.body, "Apply the filters");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/discover/movies?list=popular&from=2030");
	assert_contains(reply.body, "No movie matches these filters among the 7 movies of this list that Axis keeps. Change the filters, or clear them.");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/discover/movies?list=top_rated");
	assert_contains(reply.body, "Showing 1 to 20 of 25 movies.");
	assert_contains(reply.body, "Generated Feature 20<");
	assert_lacks(reply.body, "Generated Feature 21<");
	assert_contains(reply.body, "href=\"/discover/movies?list=top_rated&amp;page=2\"");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/discover/movies?list=top_rated&page=2");
	assert_contains(reply.body, "Showing 21 to 25 of 25 movies.");
	assert_contains(reply.body, "Generated Feature 25<");
	assert_contains(reply.body, ">Previous page<");
	axis_test_reply_clear(&reply);

	/* Beyond the end. */
	reply = get_as(fx, fx->viewer, "/discover/movies?list=popular&page=5");
	assert_contains(reply.body, "There is nothing on this page: the list holds 7 movies that match.");
	assert_contains(reply.body, ">Go to the first page<");
	axis_test_reply_clear(&reply);

	/* What is not a value of a filter is no filter. */
	reply = get_as(fx, fx->viewer, "/discover/movies?list=nonsense&genre=x&from=12&rating=99&page=-1");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Showing 1 to 7 of 7 movies.");
	axis_test_reply_clear(&reply);
}

/* ── Hide unreleased (add-discover-hide-unreleased) ────────────── */

/* Spec: Hide unreleased: a movie is left out by the Movies module's own
 * rule (a date ahead, or none and not released), a series that has not begun
 * airing too; the rest stays. */
static void
test_hide_unreleased(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	refresh(fx);
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = DATE '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	/* Ahead; undated and not out; undated and out. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = current_date + 30 WHERE title = 'The Brass Orchard'");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = NULL, status = 'upcoming'"
		" WHERE title = 'Nine Lanterns South'");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = NULL WHERE title = 'Paper Tide'");

	reply = get_as(fx, fx->viewer, "/discover/movies?list=popular");
	assert_contains(reply.body, "The Brass Orchard");
	assert_contains(reply.body, "Showing 1 to 7 of 7 movies.");
	assert_contains(reply.body, "Hide unreleased");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/discover/movies?list=popular&unreleased=yes");
	assert_lacks(reply.body, "The Brass Orchard");
	assert_lacks(reply.body, "Nine Lanterns South");
	assert_contains(reply.body, "Paper Tide");
	assert_contains(reply.body, "Showing 1 to 5 of 5 movies.");
	assert_contains(reply.body, "id=\"discover-unreleased\" checked=\"checked\"");
	assert_contains(reply.body, "Clear the filters");
	/* The tabs and the pages keep it. */
	assert_contains(reply.body, "href=\"/discover/movies?list=top_rated&amp;unreleased=yes\"");
	axis_test_reply_clear(&reply);

	/* Coming soon is of titles not out: its empty state says why. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = current_date + 30 WHERE kind = 'film'");
	reply = get_as(fx, fx->viewer, "/discover/movies?list=upcoming&unreleased=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Hide unreleased hides them");
	axis_test_reply_clear(&reply);

	/* Series: a first air date ahead; none and not begun; none but airing. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET first_air_date = DATE '2001-01-01', status = 'continuing'"
		" WHERE kind = 'series'");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET first_air_date = current_date + 30 WHERE title = 'The Tidewater Archive'");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET first_air_date = NULL, status = 'upcoming'"
		" WHERE title = 'Harbour Lights Precinct'");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET first_air_date = NULL WHERE title = 'The Orrery Club'");
	reply = get_as(fx, fx->viewer, "/discover/tv?list=popular&unreleased=yes");
	assert_lacks(reply.body, "The Tidewater Archive");
	assert_lacks(reply.body, "Harbour Lights Precinct");
	assert_contains(reply.body, "The Orrery Club");
	assert_contains(reply.body, "Showing 1 to 1 of 1 series.");
	axis_test_reply_clear(&reply);
}

/* Spec: a page stays full, and the way back after an Add keeps the box. */
static void
test_hide_unreleased_paging_and_back(Fixture *fx, gconstpointer data)
{
	g_autoptr(GString) json = g_string_new("{\"page\": 1, \"total_pages\": 1, \"results\": [");
	g_autofree gchar *needle = NULL;
	g_autofree gchar *body = NULL;
	AxisTestReply reply;
	AxisTestReply page;
	AxisTestReply posted;
	gint64 orchard;
	guint i;

	for (i = 0; i < 25; i++)
		g_string_append_printf(json, "%s{\"id\": %u, \"title\": \"Generated Feature %u\", \"release_date\":"
			" \"2020-01-01\", \"vote_average\": 6.0, \"vote_count\": 5, \"genre_ids\": [18]}", i > 0 ? "," : "", 8100 + i,
			i + 1);
	g_string_append(json, "]}");
	axis_test_discover_tmdb_set_body(fx->tmdb, "/3/movie/top_rated", 0, json->str);
	refresh(fx);
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = DATE '2000-01-01', status = 'released'"
		" WHERE kind = 'film'");
	/* Five of the first page's are not out. */
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET release_date = current_date + 30"
		" WHERE title IN ('Generated Feature 1', 'Generated Feature 2', 'Generated Feature 3',"
		" 'Generated Feature 4', 'Generated Feature 5')");

	reply = get_as(fx, fx->viewer, "/discover/movies?list=top_rated&unreleased=yes");
	assert_contains(reply.body, "Showing 1 to 20 of 20 movies.");
	assert_lacks(reply.body, "Generated Feature 5<");
	assert_contains(reply.body, "Generated Feature 25<");
	assert_lacks(reply.body, ">Next page<");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/discover/movies?list=top_rated");
	assert_contains(reply.body, "Showing 1 to 20 of 25 movies.");
	axis_test_reply_clear(&reply);

	/* Add, and back to the same place with the box still ticked. */
	orchard = title_id(fx, "film", "9001");
	page = get_as(fx, fx->admin, "/discover/movies?list=popular&unreleased=yes");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", orchard);
	body = form_body(page.body, "/films", needle);
	g_assert_nonnull(body);
	assert_contains(body, "back=%2Fdiscover%2Fmovies%3Flist%3Dpopular%26unreleased%3Dyes");
	posted = axis_test_web_request(fx->session, fx->port, "POST", "/films", fx->admin, NULL, body);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/discover/movies?list=popular&unreleased=yes&done=added");
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&posted);
}

/* ── Adding in one step ─────────────────────────────────────────── */

static void
test_add_movie(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *needle = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *expected_root = NULL;
	g_autofree gchar *outside = NULL;
	g_autofree gchar *title_page = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	AxisTestReply after;
	gint64 orchard;

	refresh(fx);
	orchard = title_id(fx, "film", "9001");
	page = get_as(fx, fx->admin, "/discover/movies?list=popular&genre=18");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", orchard);
	body = form_body(page.body, "/films", needle);
	g_assert_nonnull(body);
	/* The add form's defaults, and the way back. */
	assert_contains(body, "availability=released");
	assert_contains(body, "monitored=yes");
	assert_contains(body, "search=yes");
	assert_contains(body, "back=%2Fdiscover%2Fmovies%3Flist%3Dpopular%26genre%3D18");
	posted = axis_test_web_request(fx->session, fx->port, "POST", "/films", fx->admin, NULL, body);
	g_assert_cmpuint(posted.status, ==, 303);
	/* Back at its card (add-activity-feedback). */
	{
		g_autofree gchar *back = g_strdup_printf("/discover/movies?list=popular&genre=18&done=added&at=title-%"
			G_GINT64_FORMAT "#title-%" G_GINT64_FORMAT, orchard, orchard);

		g_assert_cmpstr(axis_test_reply_header(&posted, "Location"), ==, back);
	}
	sql = g_strdup_printf("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT " AND monitored", orchard);
	query_is(fx, sql, "1");
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("SELECT root_id FROM movie WHERE title_id = %" G_GINT64_FORMAT, orchard);
	expected_root = g_strdup_printf("%" G_GINT64_FORMAT, fx->film_root);
	query_is(fx, sql, expected_root);
	/* Its search was queued, as an add from the title page queues it. */
	query_is(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'", "1");

	{
		g_autofree gchar *shown = g_strdup_printf("/discover/movies?list=popular&genre=18&done=added&at=title-%"
			G_GINT64_FORMAT, orchard);
		g_autofree gchar *card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", orchard);
		const gchar *in_card;
		const gchar *notice;

		after = get_as(fx, fx->admin, shown);
		in_card = strstr(after.body, card);
		notice = strstr(after.body, "Added The Brass Orchard");
		g_assert_nonnull(in_card);
		g_assert_nonnull(notice);
		/* The notice is inside the card, not at the top. */
		g_assert_true(notice > in_card);
		assert_contains(after.body, "notice-here");
	}
	/* Its card now shows its state, not an Add. */
	g_assert_cmpuint(count_of(after.body, "action=\"/films\""), ==, 2);
	assert_contains(after.body, "film-mark");
	/* "Hide what I have or want" leaves it out. */
	axis_test_reply_clear(&after);
	after = get_as(fx, fx->admin, "/discover/movies?list=popular&hide=yes");
	assert_lacks(after.body, "The Brass Orchard");
	assert_contains(after.body, "Nine Lanterns South");

	/* A way back that is not a path of this site is not followed. */
	axis_test_reply_clear(&posted);
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&needle, g_free);
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", title_id(fx, "film", "9002"));
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/discover/movies?list=popular");
	body = form_body(page.body, "/films", needle);
	g_assert_nonnull(body);
	outside = g_strconcat(body, "&back=https%3A%2F%2Felsewhere.example%2F", NULL);
	/* The page's own back comes first in the body; drop it for this one. */
	{
		g_autoptr(GRegex) own = g_regex_new("&back=%2Fdiscover[^&]*", 0, 0, NULL);
		g_autofree gchar *only = g_regex_replace_literal(own, outside, -1, 0, "", 0, NULL);

		posted = axis_test_web_request(fx->session, fx->port, "POST", "/films", fx->admin, NULL, only);
	}
	g_assert_cmpuint(posted.status, ==, 303);
	title_page = g_strdup_printf("/title/%" G_GINT64_FORMAT "?done=added", title_id(fx, "film", "9002"));
	/* (The title page is the place the movies' own handler picks.) */
	assert_contains(axis_test_reply_header(&posted, "Location"), title_page);

	axis_test_reply_clear(&page);
	axis_test_reply_clear(&posted);
	axis_test_reply_clear(&after);
}

static void
test_add_series(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *needle = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *sql = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	gint64 archive;

	/* No root for TV yet: only the way to the title page. */
	refresh(fx);
	page = get_as(fx, fx->admin, "/discover/tv");
	g_assert_cmpuint(page.status, ==, 200);
	assert_lacks(page.body, "action=\"/series\"");
	assert_contains(page.body, ">Add with options<");
	axis_test_reply_clear(&page);

	axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	archive = title_id(fx, "series", "9601");
	page = get_as(fx, fx->admin, "/discover/tv");
	g_assert_cmpuint(count_of(page.body, "action=\"/series\""), ==, 3);
	/* The series' bar: folder, profile and what to monitor. */
	assert_contains(page.body, "id=\"add-defaults-series\"");
	assert_contains(page.body, "New series go to");
	assert_contains(page.body, ">monitoring<");
	assert_contains(page.body, "add-default-monitor-series");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", archive);
	body = form_body(page.body, "/series", needle);
	g_assert_nonnull(body);
	assert_contains(body, "monitor=");
	assert_contains(body, "back=%2Fdiscover%2Ftv");
	posted = axis_test_web_request(fx->session, fx->port, "POST", "/series", fx->admin, NULL, body);
	g_assert_cmpuint(posted.status, ==, 303);
	{
		g_autofree gchar *back = g_strdup_printf("/discover/tv?done=added&at=title-%" G_GINT64_FORMAT "#title-%"
			G_GINT64_FORMAT, archive, archive);

		g_assert_cmpstr(axis_test_reply_header(&posted, "Location"), ==, back);
	}
	sql = g_strdup_printf("SELECT count(*) FROM series WHERE title_id = %" G_GINT64_FORMAT, archive);
	query_is(fx, sql, "1");
	/* Its record was fetched from TMDb for the add, as from its page. */
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/tv/9601"), ==, 1);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&posted);

	page = get_as(fx, fx->admin, "/discover/tv");
	g_assert_cmpuint(count_of(page.body, "action=\"/series\""), ==, 2);

	/* Another one, added from its own page opened from its card: the page
	 * knows the list, its back link and its Add come back to the card
	 * (add-activity-feedback). */
	{
		const gchar *form = strstr(page.body, "action=\"/series\"");
		const gchar *value = form != NULL ? strstr(form, "name=\"title\" value=\"") : NULL;
		g_autoptr(GRegex) pick = g_regex_new("(?:^|&)((?:profile|root|monitor)=[^&]*)", 0, 0, NULL);
		g_autoptr(GMatchInfo) picked = NULL;
		g_autoptr(GString) full = NULL;
		g_autofree gchar *mine = NULL;
		g_autofree gchar *quick = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *link = NULL;
		g_autofree gchar *back = NULL;
		g_autofree gchar *own = NULL;
		g_autofree gchar *at = NULL;
		g_autofree gchar *location = NULL;
		g_autofree gchar *shown = NULL;
		AxisTestReply title_page;
		AxisTestReply added;
		gint64 other;

		g_assert_nonnull(value);
		other = g_ascii_strtoll(value + strlen("name=\"title\" value=\""), NULL, 10);
		mine = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", other);
		quick = form_body(page.body, "/series", mine);
		g_assert_nonnull(quick);
		path = g_strdup_printf("/title/%" G_GINT64_FORMAT "?from=%%2Fdiscover%%2Ftv", other);
		link = g_strdup_printf("href=\"%s\"", path);
		assert_contains(page.body, link);
		title_page = get_as(fx, fx->admin, path);
		assert_contains(title_page.body, ">Back to Discover<");
		back = g_strdup_printf("href=\"/discover/tv#title-%" G_GINT64_FORMAT "\"", other);
		assert_contains(title_page.body, back);
		own = form_body(title_page.body, "/series", mine);
		g_assert_nonnull(own);
		at = g_strdup_printf("at=title-%" G_GINT64_FORMAT, other);
		assert_contains(own, "back=%2Fdiscover%2Ftv");
		assert_contains(own, at);
		/* (Its selects, as the card's Add chose them.) */
		full = g_string_new(own);
		g_regex_match(pick, quick, 0, &picked);
		while (g_match_info_matches(picked)) {
			g_autofree gchar *pair = g_match_info_fetch(picked, 1);

			g_string_append_printf(full, "&%s", pair);
			g_match_info_next(picked, NULL);
		}
		added = axis_test_web_request(fx->session, fx->port, "POST", "/series", fx->admin, NULL, full->str);
		g_assert_cmpuint(added.status, ==, 303);
		location = g_strdup_printf("/discover/tv?done=added&at=title-%" G_GINT64_FORMAT "#title-%" G_GINT64_FORMAT,
			other, other);
		g_assert_cmpstr(axis_test_reply_header(&added, "Location"), ==, location);
		axis_test_reply_clear(&added);
		/* The notice in its card names it; the card shows its new state. */
		shown = g_strndup(location, strrchr(location, '#') - location);
		added = get_as(fx, fx->admin, shown);
		{
			g_autofree gchar *card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", other);
			const gchar *in_card = strstr(added.body, card);
			const gchar *notice = strstr(added.body, "notice-here");

			g_assert_nonnull(in_card);
			g_assert_nonnull(notice);
			g_assert_true(notice > in_card);
			assert_contains(notice, "Added ");
			assert_contains(notice, " to TV. Its card shows where it stands now");
			assert_contains(in_card, "film-mark");
		}
		axis_test_reply_clear(&added);
		axis_test_reply_clear(&title_page);
	}

	/* A card drawn before its disk was mounted read-only: its Add is refused,
	 * naming the folder and why, and nothing is added. */
	{
		const gchar *first = strstr(page.body, "action=\"/series\"");
		const gchar *next = first != NULL ? strstr(first + 1, "action=\"/series\"") : NULL;
		const gchar *value = next != NULL ? strstr(next, "name=\"title\" value=\"") : NULL;
		g_autofree gchar *mine = NULL;
		g_autofree gchar *stale = NULL;
		g_autofree gchar *count = NULL;
		gint64 third;

		g_assert_nonnull(value);
		third = g_ascii_strtoll(value + strlen("name=\"title\" value=\""), NULL, 10);
		mine = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", third);
		stale = form_body(page.body, "/series", mine);
		g_assert_nonnull(stale);
		axis_storage_force_volume_state(fx->t->storage, fx->volume_id, AXIS_VOLUME_READ_ONLY);
		posted = axis_test_web_request(fx->session, fx->port, "POST", "/series", fx->admin, NULL, stale);
		g_assert_cmpuint(posted.status, ==, 400);
		assert_contains(posted.body, "cannot be written to now: its disk is mounted read-only");
		axis_test_reply_clear(&posted);
		count = g_strdup_printf("SELECT count(*) FROM series WHERE title_id = %" G_GINT64_FORMAT, third);
		query_is(fx, count, "0");
		axis_storage_force_volume_state(fx->t->storage, fx->volume_id, AXIS_VOLUME_ONLINE);
	}
	axis_test_reply_clear(&page);
}

static void assert_place(Fixture *fx, AxisTestReply *reply, const gchar *place);

/* The number after "@name=" in a form's body. */
static gint64
form_number(
	const gchar *body,
	const gchar *name
){
	g_autofree gchar *key = g_strdup_printf("%s=", name);
	const gchar *at = body != NULL ? strstr(body, key) : NULL;

	g_assert_nonnull(at);
	return g_ascii_strtoll(at + strlen(key), NULL, 10);
}

/*
 * Spec (web-shell): Where a person's Adds from a list go.  The bar above the
 * list chooses the folder and the profile for every Add of the page; a
 * folder on a disk that cannot be written to is shown and cannot be chosen;
 * the choice is the person's and survives a new session; a title's own
 * Options win for it; a viewer has no bar; with htmx the bar and the cards
 * follow out of band.
 */
static void
test_add_defaults(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *needle = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *disk2 = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *expected = NULL;
	AxisTestReply page;
	AxisTestReply reply;
	gint64 orchard;
	gint64 lanterns;
	gint64 second;
	gint64 away;
	gint64 volume2;
	gint64 standard;
	gint64 other;

	refresh(fx);
	/* (The test sessions' accounts, as the database has them.) */
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (id, username, password_hash, role) OVERRIDING SYSTEM VALUE"
		" VALUES (1, 'root', '$argon2id$made-up', 'admin'), (2, 'viewer', '$argon2id$made-up', 'user')");
	orchard = title_id(fx, "film", "9001");
	lanterns = title_id(fx, "film", "9002");
	second = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "more films", AXIS_MEDIA_MOVIES);
	disk2 = axis_test_library_disk(fx->t, "disk2");
	volume2 = axis_test_library_add_volume_beside(fx->t, disk2, "Disk two");
	away = axis_test_library_add_root(fx->t, volume2, disk2, "away films", AXIS_MEDIA_MOVIES);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_library_unplug(fx->t, disk2, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, volume2, NULL, &error));
	g_assert_no_error(error);

	/* The bar, for an administrator; the folder away shown, not choosable. */
	page = get_as(fx, fx->admin, "/discover/movies?list=popular");
	assert_contains(page.body, "id=\"add-defaults-movies\"");
	assert_contains(page.body, "New movies go to");
	assert_contains(page.body, "hx-post=\"/films/defaults\"");
	assert_contains(page.body, ": cannot be written to now");
	assert_contains(page.body, ">Save as my defaults<");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", orchard);
	body = form_body(page.body, "/films", needle);
	standard = form_number(body, "profile");
	g_assert_cmpint(form_number(body, "root"), ==, fx->film_root);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/discover/movies?list=popular");
	assert_lacks(page.body, "add-defaults-movies");
	axis_test_reply_clear(&page);
	sql = g_strdup_printf("SELECT min(id) FROM quality_profile WHERE built_in AND id <> %" G_GINT64_FORMAT, standard);
	{
		g_autofree gchar *text = axis_test_db_query(fx->t->db, sql);

		other = g_ascii_strtoll(text, NULL, 10);
	}

	/* A folder that cannot be written to cannot be chosen. */
	form = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&back=%%2Fdiscover%%2Fmovies%%3F"
		"list%%3Dpopular&at=add-defaults-movies", away, other);
	reply = post_as(fx, fx->admin, "/discover/movies", "/films/defaults", form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	query_is(fx, "SELECT add_defaults::text FROM app_user WHERE username = 'root'", "{}");

	/* Chosen: back at the bar, said there. */
	g_free(form);
	form = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&back=%%2Fdiscover%%2Fmovies%%3F"
		"list%%3Dpopular&at=add-defaults-movies", second, other);
	reply = post_as(fx, fx->admin, "/discover/movies", "/films/defaults", form);
	assert_place(fx, &reply, "add-defaults-movies");

	/* The choice is the person's: a new session has it, and the cards' Adds
	 * carry it, their Options saying so. */
	again = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	page = get_as(fx, again, "/discover/movies?list=popular");
	g_free(body);
	body = form_body(page.body, "/films", needle);
	g_assert_cmpint(form_number(body, "root"), ==, second);
	g_assert_cmpint(form_number(body, "profile"), ==, other);
	assert_contains(page.body, "add-default-chosen-movies");
	axis_test_reply_clear(&page);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/films", again, NULL, body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(sql);
	sql = g_strdup_printf("SELECT root_id || '/' || quality_profile_id FROM movie WHERE title_id = %" G_GINT64_FORMAT,
		orchard);
	expected = g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, second, other);
	query_is(fx, sql, expected);

	/* A title's own Options win for it: its page's form, another folder. */
	{
		g_autofree gchar *path = g_strdup_printf("/title/%" G_GINT64_FORMAT "?from=%%2Fdiscover%%2Fmovies", lanterns);
		g_autofree gchar *mine = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", lanterns);
		g_autofree gchar *own = NULL;
		g_autofree gchar *full = NULL;
		g_autofree gchar *chose = NULL;

		page = get_as(fx, again, path);
		/* (It starts from the person's choice.) */
		chose = g_strdup_printf("<option selected=\"selected\" value=\"%" G_GINT64_FORMAT "\"", second);
		assert_contains(page.body, chose);
		own = form_body(page.body, "/films", mine);
		g_assert_nonnull(own);
		full = g_strdup_printf("%s&root=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&availability=released", own,
			fx->film_root, standard);
		axis_test_reply_clear(&page);
		reply = axis_test_web_request(fx->session, fx->port, "POST", "/films", again, NULL, full);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_free(sql);
		sql = g_strdup_printf("SELECT root_id || '/' || quality_profile_id FROM movie WHERE title_id = %"
			G_GINT64_FORMAT, lanterns);
		g_free(expected);
		expected = g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, fx->film_root, standard);
		query_is(fx, sql, expected);
	}

	/* With htmx: stored, and the answer swaps the bar and every card's Add. */
	{
		g_autofree gchar *token = NULL;
		g_autofree gchar *escaped = NULL;
		g_autofree gchar *posted = NULL;
		const gchar *headers[3];

		page = get_as(fx, fx->admin, "/discover/movies?list=popular");
		token = axis_test_web_csrf_token(page.body);
		axis_test_reply_clear(&page);
		escaped = g_uri_escape_string(token, NULL, TRUE);
		posted = g_strdup_printf("_csrf=%s&root=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&back=%%2Fdiscover%%2F"
			"movies&at=add-defaults-movies", escaped, fx->film_root, standard);
		headers[0] = "HX-Request";
		headers[1] = "true";
		headers[2] = NULL;
		reply = axis_test_web_request(fx->session, fx->port, "POST", "/films/defaults", fx->admin, headers, posted);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "id=\"add-defaults-movies\"");
		assert_contains(reply.body, "hx-swap-oob=\"outerHTML:.add-default-root-movies\"");
		assert_contains(reply.body, "hx-swap-oob=\"outerHTML:.add-default-profile-movies\"");
		assert_contains(reply.body, "hx-swap-oob=\"outerHTML:.add-default-chosen-movies\"");
		axis_test_reply_clear(&reply);
	}

	/* A disk mounted read-only: its folder is listed, with why, and cannot
	 * be chosen; the person's choice of it, made before, is no choice now
	 * (the bar says so); a card drawn before, or a hand-made post, naming
	 * it is refused on Add, with the folder and why. */
	{
		g_autofree gchar *disk3 = axis_test_library_disk(fx->t, "disk3");
		gint64 volume3 = axis_test_library_add_volume_beside(fx->t, disk3, "Disk three");
		gint64 third = axis_test_library_add_root(fx->t, volume3, disk3, "third films", AXIS_MEDIA_MOVIES);
		g_autofree gchar *stale = NULL;
		g_autofree gchar *chose = NULL;
		gint64 dusk = title_id(fx, "film", "9003");
		g_autofree gchar *dusk_needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", dusk);

		g_free(form);
		form = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&back=%%2Fdiscover%%2Fmovies"
			"&at=add-defaults-movies", third, other);
		reply = post_as(fx, fx->admin, "/discover/movies", "/films/defaults", form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		page = get_as(fx, fx->admin, "/discover/movies?list=popular");
		stale = form_body(page.body, "/films", dusk_needle);
		g_assert_cmpint(form_number(stale, "root"), ==, third);
		axis_test_reply_clear(&page);

		axis_storage_force_volume_state(fx->t->storage, volume3, AXIS_VOLUME_READ_ONLY);
		page = get_as(fx, fx->admin, "/discover/movies?list=popular");
		assert_contains(page.body, "third films on Disk three");
		assert_contains(page.body, ": cannot be written to now, its disk is mounted read-only");
		assert_contains(page.body, "add-defaults-lapsed");
		g_free(body);
		body = form_body(page.body, "/films", dusk_needle);
		g_assert_cmpint(form_number(body, "root"), ==, fx->film_root);
		chose = g_strdup_printf("<option selected=\"selected\" value=\"%" G_GINT64_FORMAT "\"", third);
		assert_lacks(page.body, chose);
		axis_test_reply_clear(&page);
		reply = post_as(fx, fx->admin, "/discover/movies", "/films/defaults", form);
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
		reply = axis_test_web_request(fx->session, fx->port, "POST", "/films", fx->admin, NULL, stale);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "third films cannot be written to now: its disk is mounted read-only");
		axis_test_reply_clear(&reply);
		g_free(sql);
		sql = g_strdup_printf("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, dusk);
		query_is(fx, sql, "0");
		/* Its title page's own Options list it, not to be chosen. */
		{
			g_autofree gchar *path = g_strdup_printf("/title/%" G_GINT64_FORMAT, dusk);
			g_autofree gchar *disabled = g_strdup_printf("<option disabled=\"disabled\" value=\"%" G_GINT64_FORMAT "\"",
				third);

			page = get_as(fx, fx->admin, path);
			assert_contains(page.body, disabled);
			axis_test_reply_clear(&page);
		}
		axis_storage_force_volume_state(fx->t->storage, volume3, AXIS_VOLUME_ONLINE);
	}
}

static void
test_add_needs_admin(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	refresh(fx);
	form = g_strdup_printf("title=%" G_GINT64_FORMAT "&profile=1&root=%" G_GINT64_FORMAT "&availability=released"
		"&monitored=yes&back=%%2Fdiscover%%2Fmovies", title_id(fx, "film", "9001"), fx->film_root);
	reply = post_as(fx, fx->viewer, "/discover/movies", "/films", form);
	g_assert_cmpuint(reply.status, ==, 403);
	query_is(fx, "SELECT count(*) FROM movie", "0");
	axis_test_reply_clear(&reply);
}

/* ── Not interested ─────────────────────────────────────────────── */

static void
test_not_interested(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *form = NULL;
	g_autofree gchar *unknown = NULL;
	AxisTestReply reply;
	gint64 lanterns;

	refresh(fx);
	lanterns = title_id(fx, "film", "9002");
	form = g_strdup_printf("title=%" G_GINT64_FORMAT "&back=%%2Fdiscover%%2Fmovies%%3Flist%%3Dpopular", lanterns);

	/* Not for viewers, and not without the token. */
	reply = post_as(fx, fx->viewer, "/discover/movies", "/discover/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, "/discover/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	query_is(fx, "SELECT count(*) FROM discover_dismissed", "0");

	reply = post_as(fx, fx->admin, "/discover/movies", "/discover/dismiss", form);
	g_assert_cmpuint(reply.status, ==, 303);
	/* The list is told which title went, and its notice names it: the
	 * notice stands at another title's card, and must not read as that
	 * card's. */
	{
		g_autofree gchar *named = g_strdup_printf("/discover/movies?list=popular&added=%" G_GINT64_FORMAT "&done=hidden",
			lanterns);

		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, named);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, named);
	}
	assert_contains(reply.body, "Hidden from Discover: Nine Lanterns South");
	assert_contains(reply.body, "The Not interested page lists it and can show it again.");
	assert_lacks(reply.body, "alt=\"Nine Lanterns South");
	axis_test_reply_clear(&reply);
	/* Without the title (an old address), the plain notice says whose it is. */
	reply = get_as(fx, fx->admin, "/discover/movies?list=popular&done=hidden");
	assert_contains(reply.body, "Hidden: the title that stood here before this one is gone from Discover.");
	assert_contains(reply.body, "Showing 1 to 6 of 6 movies.");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->viewer, "/discover/not-interested");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<h1>Hidden from Discover</h1>");
	assert_contains(reply.body, "Nine Lanterns South");
	assert_contains(reply.body, "Hidden by root on");
	assert_lacks(reply.body, "Show in Discover again");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/discover/not-interested");
	assert_contains(reply.body, "Show in Discover again");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);

	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("title=%" G_GINT64_FORMAT "&back=%%2Fdiscover%%2Fnot-interested", lanterns);
	reply = post_as(fx, fx->admin, "/discover/not-interested", "/discover/undismiss", form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/discover/not-interested?done=shown");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/discover/movies?list=popular");
	assert_contains(reply.body, "Nine Lanterns South");
	axis_test_reply_clear(&reply);

	/* A title that is not stored. */
	unknown = g_strdup("title=987654321");
	reply = post_as(fx, fx->admin, "/discover/movies", "/discover/dismiss", unknown);
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/discover/movies", "/discover/dismiss", "title=x");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
}

/* ── Recommended, and fetching now ──────────────────────────────── */

static void
test_recommended(Fixture *fx, gconstpointer data)
{
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;
	gint64 movie = 0;

	refresh(fx);
	reply = get_as(fx, fx->viewer, "/discover/movies?list=recommended");
	assert_contains(reply.body, "Recommended for you comes from the movies you have and want");
	assert_lacks(reply.body, "Hide what I have or want");
	axis_test_reply_clear(&reply);

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_id(fx, "film", "9001");
	spec.root_id = fx->film_root;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &movie, &error));
	g_assert_no_error(error);
	refresh(fx);
	reply = get_as(fx, fx->viewer, "/discover/movies?list=recommended");
	assert_contains(reply.body, "Seven Bridges of Elm");
	assert_contains(reply.body, "Because you have The Brass Orchard");
	/* Recommended for the one you have; the one you have is not a card. */
	assert_contains(reply.body, "Nine Lanterns South");
	assert_contains(reply.body, "Showing 1 to 4 of 4 movies.");
	g_assert_cmpuint(count_of(reply.body, ">The Brass Orchard</a>"), ==, 0);
	assert_contains(reply.body, "The latest recommendations were fetched from TMDb on");
	axis_test_reply_clear(&reply);

	/* Turned off: the list is not offered. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_DISCOVER_SETTING_RECOMMEND, "0", &error));
	reply = get_as(fx, fx->viewer, "/discover/movies?list=recommended");
	assert_lacks(reply.body, ">Recommended for you<");
	assert_contains(reply.body, "The Brass Orchard");
	axis_test_reply_clear(&reply);
}

static void
test_refresh_now(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	refresh(fx);
	reply = post_as(fx, fx->viewer, "/discover/movies", "/discover/refresh", "back=%2Fdiscover%2Ftv");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/discover/movies", "/discover/refresh", "back=%2Fdiscover%2Ftv");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/discover/tv?done=refreshing");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/discover/movies", "/discover/refresh", NULL);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/discover/movies?done=already_refreshing");
	axis_test_reply_clear(&reply);
	query_is(fx, "SELECT count(*) FROM job WHERE kind = 'discover.refresh'", "1");
	/* Asked by a person: every list, whatever its age. */
	query_is(fx, "SELECT payload ->> 'all' FROM job WHERE kind = 'discover.refresh'", "true");
	reply = get_as(fx, fx->admin, "/discover/tv?done=refreshing");
	assert_contains(reply.body, "Axis is fetching the lists from TMDb now.");
	axis_test_reply_clear(&reply);
}

/* A failed attempt: said, with the list shown from before. */
static void
test_problem_shown(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	refresh(fx);
	axis_test_db_exec(fx->t->db, "UPDATE discover_list SET fetched_at = now() - interval '13 hours'"
		" WHERE kind = 'film' AND list = 'popular'");
	axis_test_discover_tmdb_set_status(fx->tmdb, "/3/movie/popular", 2, 404);
	refresh(fx);
	reply = get_as(fx, fx->viewer, "/discover/movies");
	assert_contains(reply.body, "Axis could not fetch this list again on");
	assert_contains(reply.body, "TMDb did not have page 2 of this list.");
	assert_contains(reply.body, "The list shown is the one fetched on");
	assert_contains(reply.body, "The Brass Orchard");
	axis_test_reply_clear(&reply);
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

/* Not interested, shown again and fetching now come back to the cards or
 * the fetch button; the cards have ids for the forms that add. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *needle = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *card = NULL;
	AxisTestReply page;
	AxisTestReply reply;
	gint64 lanterns;

	refresh(fx);
	lanterns = title_id(fx, "film", "9002");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", lanterns);
	card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", lanterns);

	page = get_as(fx, fx->admin, "/discover/movies?list=popular");
	assert_contains(page.body, card);
	body = form_body(page.body, "/discover/dismiss", needle);
	g_assert_nonnull(body);
	/* Its card goes: back at the next one (add-activity-feedback). */
	{
		const gchar *mine = strstr(page.body, card);
		const gchar *after = mine != NULL ? strstr(mine + strlen(card), "id=\"title-") : NULL;
		g_autofree gchar *next = NULL;
		g_autofree gchar *at = NULL;

		g_assert_nonnull(after);
		next = g_strndup(after + 4, strchr(after + 4, '"') - (after + 4));
		at = g_strdup_printf("at=%s", next);
		assert_contains(body, at);
		reply = axis_test_web_request(fx->session, fx->port, "POST", "/discover/dismiss", fx->admin, NULL, body);
		assert_place(fx, &reply, next);
	}
	g_clear_pointer(&body, g_free);

	/* The fetch button: its own place. */
	body = form_body(page.body, "/discover/refresh", "action=\"/discover/refresh\"");
	g_assert_nonnull(body);
	assert_contains(body, "at=discover-refresh");
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/discover/refresh", fx->admin, NULL, body);
	assert_place(fx, &reply, "discover-refresh");
	axis_test_reply_clear(&page);
	g_clear_pointer(&body, g_free);

	/* Shown again, from the list of the hidden. */
	page = get_as(fx, fx->admin, "/discover/not-interested");
	body = form_body(page.body, "/discover/undismiss", needle);
	g_assert_nonnull(body);
	assert_contains(body, "at=discover-cards");
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/discover/undismiss", fx->admin, NULL, body);
	assert_place(fx, &reply, "discover-cards");
	axis_test_reply_clear(&page);
}

/* "Hide what I have or want" on: an Add takes its card away, so it comes back
 * at the next card (the block of the cards for the last), and the notice
 * there names the title.  With it off the Add comes back to its own card. */
static void
test_add_hidden_card(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *orchard_needle = NULL;
	g_autofree gchar *last_needle = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *last_body = NULL;
	g_autofree gchar *card = NULL;
	AxisTestReply page;
	AxisTestReply reply;
	AxisTestReply shown;
	gint64 orchard;

	refresh(fx);
	orchard = title_id(fx, "film", "9001");
	orchard_needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", orchard);
	card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", orchard);

	/* Off: its own card, as before. */
	page = get_as(fx, fx->admin, "/discover/movies?list=popular");
	body = form_body(page.body, "/films", orchard_needle);
	g_assert_nonnull(body);
	{
		g_autofree gchar *own = g_strdup_printf("at=title-%" G_GINT64_FORMAT, orchard);

		assert_contains(body, own);
		assert_lacks(body, "added%3D");
		assert_lacks(page.body, "return_at");
	}
	axis_test_reply_clear(&page);
	g_clear_pointer(&body, g_free);

	/* On: the next card, and the title named there. */
	page = get_as(fx, fx->admin, "/discover/movies?list=popular&hide=yes");
	body = form_body(page.body, "/films", orchard_needle);
	g_assert_nonnull(body);
	{
		const gchar *mine = strstr(page.body, card);
		const gchar *after = mine != NULL ? strstr(mine + strlen(card), "id=\"title-") : NULL;
		g_autofree gchar *next = NULL;
		g_autofree gchar *at = NULL;
		g_autofree gchar *added = g_strdup_printf("added%%3D%" G_GINT64_FORMAT, orchard);
		const gchar *last = NULL;
		const gchar *scan = page.body;

		g_assert_nonnull(after);
		next = g_strndup(after + 4, strchr(after + 4, '"') - (after + 4));
		at = g_strdup_printf("at=%s", next);
		assert_contains(body, at);
		assert_contains(body, added);

		/* "Add with options": the title page carries the next card's place, and
		 * its form comes back there with the title named. */
		{
			g_autofree gchar *link = g_strdup_printf("/title/%" G_GINT64_FORMAT "\\?from=[^\"]*return_at=[^\"]*",
				orchard);
			g_autoptr(GRegex) find = g_regex_new(link, 0, 0, NULL);
			g_autoptr(GMatchInfo) found = NULL;
			g_autofree gchar *raw = NULL;
			g_autofree gchar *path = NULL;
			g_autofree gchar *form = NULL;
			g_autofree gchar *wants_at = g_strdup_printf("return_at=%s", next);
			AxisTestReply title_page;

			g_assert_true(g_regex_match(find, page.body, 0, &found));
			raw = g_match_info_fetch(found, 0);
			path = unescape(raw);
			assert_contains(path, wants_at);
			title_page = get_as(fx, fx->admin, path);
			form = form_body(title_page.body, "/films", orchard_needle);
			g_assert_nonnull(form);
			assert_contains(form, at);
			assert_contains(form, added);
			axis_test_reply_clear(&title_page);
		}
		reply = axis_test_web_request(fx->session, fx->port, "POST", "/films", fx->admin, NULL, body);
		{
			const gchar *location = axis_test_reply_header(&reply, "Location");
			g_autofree gchar *path = g_strndup(location, strrchr(location, '#') - location);

			/* (Posting once: the notice must be there, inside the next card.) */
			assert_place(fx, &reply, next);
			shown = get_as(fx, fx->admin, path);
		}
		assert_contains(shown.body, "Added The Brass Orchard");
		assert_lacks(shown.body, "Its card below");
		assert_lacks(shown.body, card);

		/* The last card: back at the block of the cards. */
		while ((scan = strstr(scan, "id=\"title-")) != NULL) {
			last = scan;
			scan += 4;
		}
		g_assert_nonnull(last);
		{
			g_autofree gchar *last_id = g_strndup(last + 10, strchr(last + 10, '"') - (last + 10));

			last_needle = g_strdup_printf("name=\"title\" value=\"%s\"", last_id);
		}
		last_body = form_body(page.body, "/films", last_needle);
		g_assert_nonnull(last_body);
		assert_contains(last_body, "at=discover-cards");
	}
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&shown);
}

/* ── More like this (add-more-like-this) ────────────────────────── */

/* The card's own request: what htmx sends when the card loads itself. */
static AxisTestReply
get_card(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	static const gchar *const htmx[] = { "HX-Request", "true", NULL };

	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, htmx, NULL);
}

#define ORCHARD_LIKE "/3/movie/9001/recommendations"
#define ARCHIVE_LIKE "/3/tv/9601/recommendations"

/* Not fetched yet: the page is drawn without asking, the card asks once and
 * shows the titles, one you have carries its mark, and an Add comes back to
 * the card.  Stored: drawn with the page, nothing asked.  Older than a week:
 * asked again, with "Recommended for you" off too. */
static void
test_like_movie(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page_path = NULL;
	g_autofree gchar *card_path = NULL;
	g_autofree gchar *loads = NULL;
	g_autofree gchar *needle = NULL;
	g_autofree gchar *have = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *at = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *old = NULL;
	AxisMovieSpec spec;
	AxisTestReply reply;
	gint64 orchard;
	gint64 lanterns;
	gint64 garden;
	gint64 movie = 0;

	refresh(fx);
	orchard = title_id(fx, "film", "9001");
	lanterns = title_id(fx, "film", "9002");
	memset(&spec, 0, sizeof(spec));
	spec.title_id = lanterns;
	spec.root_id = fx->film_root;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &movie, &error));
	g_assert_no_error(error);
	page_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, orchard);
	card_path = g_strdup_printf("/discover/like/%" G_GINT64_FORMAT, orchard);
	loads = g_strdup_printf("hx-get=\"%s\"", card_path);

	/* The page: at once, with a card that loads itself. */
	reply = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "id=\"more-like-this\"");
	assert_contains(reply.body, ">More like this<");
	assert_contains(reply.body, loads);
	assert_contains(reply.body, "hx-trigger=\"load\"");
	assert_contains(reply.body, "Axis is asking TMDb what is like this movie.");
	assert_lacks(reply.body, "The Tin Garden");
	assert_csp_clean(reply.body);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 0);
	axis_test_reply_clear(&reply);

	/* The card: one request, then the titles as Discover's cards. */
	reply = get_card(fx, fx->admin, card_path);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 1);
	assert_lacks(reply.body, "<html");
	assert_lacks(reply.body, "hx-get=\"/discover/like");
	assert_contains(reply.body, "id=\"more-like-this\"");
	assert_contains(reply.body, "discover-page");
	assert_contains(reply.body, "The Tin Garden");
	assert_contains(reply.body, "Seven Bridges of Elm");
	assert_contains(reply.body, "Hollow Bell Road");
	assert_contains(reply.body, "What TMDb recommends for this movie, fetched on");
	g_assert_cmpuint(count_of(reply.body, "class=\"title-card\""), ==, 4);
	/* A list to add from, not one to tidy. */
	assert_lacks(reply.body, "Hide from Discover");
	/* The one you have: its mark and its page, no Add. */
	{
		g_autofree gchar *card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", lanterns);
		g_autofree gchar *href = g_strdup_printf("href=\"/title/%" G_GINT64_FORMAT, lanterns);
		const gchar *mine = strstr(reply.body, card);

		g_assert_nonnull(mine);
		assert_contains(mine, "Nine Lanterns South");
		assert_contains(reply.body, href);
		have = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", lanterns);
		assert_lacks(reply.body, have);
		assert_contains(reply.body, "film-mark");
	}
	/* The others: the one-step Add, which comes back to the title's page at
	 * the card. */
	g_assert_cmpuint(count_of(reply.body, "action=\"/films\""), ==, 3);
	assert_contains(reply.body, ">Add with options<");
	garden = title_id(fx, "film", "9021");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", garden);
	body = form_body(reply.body, "/films", needle);
	g_assert_nonnull(body);
	back = g_strdup_printf("back=%%2Ftitle%%2F%" G_GINT64_FORMAT, orchard);
	at = g_strdup_printf("at=title-%" G_GINT64_FORMAT, garden);
	assert_contains(body, back);
	assert_contains(body, at);
	assert_lacks(body, "discover%2Flike");
	axis_test_reply_clear(&reply);

	/* Stored now: drawn with the page, and nothing asked, by the page or by
	 * the card. */
	reply = get_as(fx, fx->viewer, page_path);
	assert_contains(reply.body, "The Tin Garden");
	assert_lacks(reply.body, "hx-get=\"/discover/like");
	/* (Someone who cannot add sees the cards and the marks.) */
	assert_lacks(reply.body, "action=\"/films\"");
	assert_contains(reply.body, "film-mark");
	axis_test_reply_clear(&reply);
	reply = get_card(fx, fx->viewer, card_path);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 1);

	/* Without script the card's link comes back to the page at the card. */
	reply = get_as(fx, fx->viewer, card_path);
	g_assert_cmpuint(reply.status, ==, 303);
	location = g_strdup_printf("/title/%" G_GINT64_FORMAT "?at=more-like-this#more-like-this", orchard);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, location);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);

	/* Add: back to the page at the card, the notice there and of that title;
	 * the page's own movie says nothing of it. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/films", fx->admin, NULL, body);
	g_assert_cmpuint(reply.status, ==, 303);
	location = g_strdup_printf("/title/%" G_GINT64_FORMAT "?done=added&at=title-%" G_GINT64_FORMAT "#title-%"
		G_GINT64_FORMAT, orchard, garden, garden);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, location);
	axis_test_reply_clear(&reply);
	shown = g_strndup(location, strrchr(location, '#') - location);
	reply = get_as(fx, fx->admin, shown);
	{
		g_autofree gchar *card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", garden);
		const gchar *in_card = strstr(reply.body, card);
		const gchar *notice = strstr(reply.body, "Added The Tin Garden");

		g_assert_nonnull(in_card);
		g_assert_nonnull(notice);
		g_assert_true(notice > in_card);
		assert_contains(reply.body, "notice-here");
		assert_lacks(reply.body, "The movie was added.");
		/* Its card shows its state now: two cards still add, and the page's
		 * own form for its own movie. */
		g_assert_cmpuint(count_of(reply.body, "action=\"/films\""), ==, 3);
		assert_lacks(reply.body, needle);
	}
	axis_test_reply_clear(&reply);

	/* Older than its week: the card asks again, and "Recommended for you"
	 * turned off does not stop it. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_DISCOVER_SETTING_RECOMMEND, "0", &error));
	g_assert_no_error(error);
	old = g_strdup_printf("UPDATE discover_list SET fetched_at = now() - interval '8 days', tried_at = now() -"
		" interval '8 days' WHERE list = 'recommended' AND seed_title_id = %" G_GINT64_FORMAT, orchard);
	axis_test_db_exec(fx->t->db, old);
	reply = get_as(fx, fx->admin, page_path);
	assert_contains(reply.body, loads);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 1);
	reply = get_card(fx, fx->admin, card_path);
	assert_contains(reply.body, "Seven Bridges of Elm");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 2);
}

/* Nothing recommended says so; an attempt that failed says why, and is not
 * repeated within the hour; a title with no id of TMDb's has no card; a
 * title that is not stored is no title. */
static void
test_like_empty_and_none(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page_path = NULL;
	g_autofree gchar *card_path = NULL;
	g_autofree gchar *other_card = NULL;
	g_autofree gchar *sql = NULL;
	AxisTestReply reply;
	gint64 orchard;
	gint64 lanterns;

	refresh(fx);
	orchard = title_id(fx, "film", "9001");
	lanterns = title_id(fx, "film", "9002");
	page_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, orchard);
	card_path = g_strdup_printf("/discover/like/%" G_GINT64_FORMAT, orchard);
	other_card = g_strdup_printf("/discover/like/%" G_GINT64_FORMAT, lanterns);

	/* Nothing recommended. */
	axis_test_discover_tmdb_set_body(fx->tmdb, ORCHARD_LIKE, 0,
		"{\"page\":1,\"results\":[],\"total_pages\":1,\"total_results\":0}");
	reply = get_card(fx, fx->viewer, card_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "TMDb recommends nothing for this movie yet.");
	assert_lacks(reply.body, "class=\"title-card\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page_path);
	assert_contains(reply.body, "TMDb recommends nothing for this movie yet.");
	assert_lacks(reply.body, "hx-get=\"/discover/like");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 1);

	/* A failed attempt: said, and not repeated at once. */
	axis_test_discover_tmdb_set_status(fx->tmdb, "/3/movie/9002/recommendations", 0, 500);
	reply = get_card(fx, fx->viewer, other_card);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis could not ask TMDb what is like this movie on");
	assert_contains(reply.body, "It asks again when this page is opened an hour or more after that.");
	axis_test_reply_clear(&reply);
	{
		guint asked = axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9002/recommendations");

		g_assert_cmpuint(asked, >=, 1);
		reply = get_card(fx, fx->viewer, other_card);
		assert_contains(reply.body, "Axis could not ask TMDb what is like this movie on");
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9002/recommendations"), ==, asked);
	}

	/* Without a token: what is stored is still shown; nothing stored says
	 * what is needed, and nothing is asked. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *third = g_strdup_printf("/discover/like/%" G_GINT64_FORMAT, title_id(fx, "film", "9003"));

		g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TMDB_SETTING_KEY, &error));
		g_assert_no_error(error);
		axis_test_plex_reset_requests(fx->tmdb->api);
		reply = get_card(fx, fx->viewer, third);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Axis needs a TMDb token to ask what is like this movie.");
		assert_contains(reply.body, "An administrator can add one in Settings.");
		axis_test_reply_clear(&reply);
		reply = get_card(fx, fx->viewer, card_path);
		assert_contains(reply.body, "TMDb recommends nothing for this movie yet.");
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(axis_test_discover_tmdb_requests(fx->tmdb), ==, 0);
		g_assert_true(axis_settings_set(fx->settings, AXIS_META_TMDB_SETTING_KEY, AXIS_TEST_DISCOVER_TOKEN, &error));
		g_assert_no_error(error);
	}

	/* Another provider's record that keeps TMDb's id (a series known by
	 * TheTVDB): TMDb is asked with that id. */
	{
		gint64 fourth = title_id(fx, "film", "9004");
		g_autofree gchar *kept = g_strdup_printf("/discover/like/%" G_GINT64_FORMAT, fourth);
		g_autofree gchar *moved = g_strdup_printf("UPDATE meta_title SET provider = 'elsewhere', provider_id = 'e-4'"
			" WHERE id = %" G_GINT64_FORMAT, fourth);

		axis_test_db_exec(fx->t->db, moved);
		reply = get_card(fx, fx->viewer, kept);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "id=\"more-like-this\"");
		axis_test_reply_clear(&reply);
		g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, "/3/movie/9004/recommendations"), ==, 1);
	}

	/* No id of TMDb's: no card, and nothing asked. */
	sql = g_strdup_printf("DELETE FROM meta_external_id WHERE scheme = 'tmdb' AND title_id = %" G_GINT64_FORMAT,
		orchard);
	axis_test_db_exec(fx->t->db, sql);
	g_clear_pointer(&sql, g_free);
	sql = g_strdup_printf("UPDATE meta_title SET provider = 'elsewhere' WHERE id = %" G_GINT64_FORMAT, orchard);
	axis_test_db_exec(fx->t->db, sql);
	axis_test_plex_reset_requests(fx->tmdb->api);
	reply = get_card(fx, fx->viewer, card_path);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(reply.body, ==, "");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ORCHARD_LIKE), ==, 0);

	/* Not a title. */
	reply = get_card(fx, fx->viewer, "/discover/like/987654321");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_card(fx, fx->viewer, "/discover/like/0x1");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/* A series: the same card on its title page, and on its episodes page, which
 * loads it; an Add there comes back to the episodes page at the card. */
static void
test_like_series(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *needle = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *page_path = NULL;
	g_autofree gchar *card_path = NULL;
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *series_card = NULL;
	g_autofree gchar *loads = NULL;
	g_autofree gchar *sid = NULL;
	g_autofree gchar *salt_needle = NULL;
	g_autofree gchar *add = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *after_card = NULL;
	AxisTestReply reply;
	gint64 archive;
	gint64 harbour;
	gint64 salt;

	refresh(fx);
	axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	archive = title_id(fx, "series", "9601");
	harbour = title_id(fx, "series", "9501");
	/* The series whose page it is, and one of those like it, are yours. */
	reply = get_as(fx, fx->admin, "/discover/tv");
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", archive);
	body = form_body(reply.body, "/series", needle);
	g_assert_nonnull(body);
	g_clear_pointer(&needle, g_free);
	needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", harbour);
	add = form_body(reply.body, "/series", needle);
	g_assert_nonnull(add);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/series", fx->admin, NULL, body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/series", fx->admin, NULL, add);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&add, g_free);
	sid = axis_test_db_queryf(fx->t->db, "SELECT id FROM series WHERE title_id = %" G_GINT64_FORMAT, archive);
	g_assert_nonnull(sid);

	axis_test_discover_tmdb_set_body(fx->tmdb, ARCHIVE_LIKE, 0, "{\"page\":1,\"results\":["
		"{\"adult\":false,\"genre_ids\":[18],\"id\":9611,\"origin_country\":[\"GB\"],\"original_language\":\"en\","
		"\"original_name\":\"The Salt Ledger\",\"overview\":\"A tide clerk keeps two sets of books.\","
		"\"popularity\":90.5,\"poster_path\":\"/s9611.jpg\",\"first_air_date\":\"2022-03-04\","
		"\"name\":\"The Salt Ledger\",\"vote_average\":7.9,\"vote_count\":120},"
		"{\"adult\":false,\"genre_ids\":[80,18],\"id\":9501,\"origin_country\":[\"US\"],\"original_language\":\"en\","
		"\"original_name\":\"Harbour Lights Precinct\",\"overview\":\"The night desk of a harbour police station.\","
		"\"popularity\":401.2,\"poster_path\":\"/s9501.jpg\",\"first_air_date\":\"2021-04-05\","
		"\"name\":\"Harbour Lights Precinct\",\"vote_average\":8.1,\"vote_count\":900}],"
		"\"total_pages\":1,\"total_results\":2}");
	axis_test_plex_reset_requests(fx->tmdb->api);
	page_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, archive);
	card_path = g_strdup_printf("/discover/like/%" G_GINT64_FORMAT, archive);
	loads = g_strdup_printf("hx-get=\"%s\"", card_path);

	/* Its title page: the card that loads itself, then the series. */
	reply = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, loads);
	assert_contains(reply.body, "Axis is asking TMDb what is like this series.");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ARCHIVE_LIKE), ==, 0);
	reply = get_card(fx, fx->admin, card_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The Salt Ledger");
	assert_contains(reply.body, "Harbour Lights Precinct");
	assert_contains(reply.body, "What TMDb recommends for this series, fetched on");
	/* The one you have: its mark; the other: the one-step Add. */
	assert_contains(reply.body, "film-mark");
	g_assert_cmpuint(count_of(reply.body, "action=\"/series\""), ==, 1);
	salt = title_id(fx, "series", "9611");
	salt_needle = g_strdup_printf("name=\"title\" value=\"%" G_GINT64_FORMAT "\"", salt);
	add = form_body(reply.body, "/series", salt_needle);
	g_assert_nonnull(add);
	back = g_strdup_printf("back=%%2Ftitle%%2F%" G_GINT64_FORMAT, archive);
	assert_contains(add, back);
	g_clear_pointer(&add, g_free);
	g_clear_pointer(&back, g_free);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page_path);
	assert_contains(reply.body, "The Salt Ledger");
	assert_lacks(reply.body, "hx-get=\"/discover/like");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ARCHIVE_LIKE), ==, 1);

	/* Its episodes page: the card loads itself, told the page it is on. */
	series_path = g_strdup_printf("/series/%s", sid);
	series_card = g_strdup_printf("%s?on=%%2Fseries%%2F%s", card_path, sid);
	reply = get_as(fx, fx->admin, series_path);
	g_assert_cmpuint(reply.status, ==, 200);
	{
		g_autofree gchar *attr = g_strdup_printf("hx-get=\"%s\"", series_card);

		assert_contains(reply.body, "id=\"more-like-this\"");
		assert_contains(reply.body, attr);
		assert_contains(reply.body, "Axis is looking up what is like this series.");
	}
	axis_test_reply_clear(&reply);
	reply = get_card(fx, fx->admin, series_card);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The Salt Ledger");
	add = form_body(reply.body, "/series", salt_needle);
	g_assert_nonnull(add);
	back = g_strdup_printf("back=%%2Fseries%%2F%s&", sid);
	assert_contains(add, back);
	axis_test_reply_clear(&reply);
	/* Stored: the card asked nothing more. */
	g_assert_cmpuint(axis_test_discover_tmdb_count(fx->tmdb, ARCHIVE_LIKE), ==, 1);

	/* Add: back to the episodes page at the card; the page passes what was
	 * done on to the card, which says it there, and says nothing itself. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/series", fx->admin, NULL, add);
	g_assert_cmpuint(reply.status, ==, 303);
	location = g_strdup_printf("/series/%s?done=added&at=title-%" G_GINT64_FORMAT "#title-%" G_GINT64_FORMAT, sid, salt,
		salt);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, location);
	axis_test_reply_clear(&reply);
	shown = g_strndup(location, strrchr(location, '#') - location);
	after = g_strdup_printf("%s&amp;done=added&amp;at=title-%" G_GINT64_FORMAT "\"", series_card, salt);
	reply = get_as(fx, fx->admin, shown);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, after);
	assert_lacks(reply.body, "The series was added");
	axis_test_reply_clear(&reply);
	after_card = g_strdup_printf("%s&done=added&at=title-%" G_GINT64_FORMAT, series_card, salt);
	reply = get_card(fx, fx->admin, after_card);
	{
		g_autofree gchar *card = g_strdup_printf("id=\"title-%" G_GINT64_FORMAT "\"", salt);
		const gchar *in_card = strstr(reply.body, card);
		const gchar *notice = strstr(reply.body, "Added The Salt Ledger");

		g_assert_nonnull(in_card);
		g_assert_nonnull(notice);
		g_assert_true(notice > in_card);
		assert_contains(reply.body, " to TV. Its card shows where it stands now");
		assert_lacks(reply.body, "action=\"/series\"");
	}
	axis_test_reply_clear(&reply);

	/* A way back that is not a page of this site is not taken. */
	{
		g_autofree gchar *outside = g_strdup_printf("%s?on=https%%3A%%2F%%2Felsewhere.example%%2F", card_path);

		reply = get_card(fx, fx->admin, outside);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_lacks(reply.body, "elsewhere.example");
		axis_test_reply_clear(&reply);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web/discover/no-key", Fixture, NULL, test_no_key);
	AXIS_TEST_ADD_DB_CASE("/web/discover/first-view-queues", Fixture, NULL, test_first_view_queues);
	AXIS_TEST_ADD_DB_CASE("/web/discover/movies-page", Fixture, NULL, test_movies_page);
	AXIS_TEST_ADD_DB_CASE("/web/discover/filters-and-paging", Fixture, NULL, test_filters_and_paging);
	AXIS_TEST_ADD_DB_CASE("/web/discover/hide-unreleased", Fixture, NULL, test_hide_unreleased);
	AXIS_TEST_ADD_DB_CASE("/web/discover/hide-unreleased-paging-and-back", Fixture, NULL,
		test_hide_unreleased_paging_and_back);
	AXIS_TEST_ADD_DB_CASE("/web/discover/add-movie", Fixture, NULL, test_add_movie);
	AXIS_TEST_ADD_DB_CASE("/web/discover/add-series", Fixture, NULL, test_add_series);
	AXIS_TEST_ADD_DB_CASE("/web/discover/add-needs-admin", Fixture, NULL, test_add_needs_admin);
	AXIS_TEST_ADD_DB_CASE("/web/discover/not-interested", Fixture, NULL, test_not_interested);
	AXIS_TEST_ADD_DB_CASE("/web/discover/recommended", Fixture, NULL, test_recommended);
	AXIS_TEST_ADD_DB_CASE("/web/discover/refresh-now", Fixture, NULL, test_refresh_now);
	AXIS_TEST_ADD_DB_CASE("/web/discover/problem-shown", Fixture, NULL, test_problem_shown);
	AXIS_TEST_ADD_DB_CASE("/web/discover/back-to-place", Fixture, NULL, test_back_to_place);
	AXIS_TEST_ADD_DB_CASE("/web/discover/add-defaults", Fixture, NULL, test_add_defaults);
	AXIS_TEST_ADD_DB_CASE("/web/discover/add-hidden-card", Fixture, NULL, test_add_hidden_card);
	AXIS_TEST_ADD_DB_CASE("/web/discover/like-movie", Fixture, NULL, test_like_movie);
	AXIS_TEST_ADD_DB_CASE("/web/discover/like-empty-and-none", Fixture, NULL, test_like_empty_and_none);
	AXIS_TEST_ADD_DB_CASE("/web/discover/like-series", Fixture, NULL, test_like_series);
	return g_test_run();
}
