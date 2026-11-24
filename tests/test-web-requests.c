/*
 * test-web-requests.c - the requests' pages: Request for an ordinary user,
 * My requests, the Requests page of an administrator, who may read and
 * change what, and the notices
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * (add-request-portal, slice P1; the `requests` and `attention`
 * specifications.)  The daemon's pages run on the test's own main loop, on a
 * port the system gave; the database, the scratch directory and the fixture
 * provider are the test's own.  Three accounts: root (an administrator),
 * robin and sam (ordinary users), each with a session.
 */

#define _GNU_SOURCE

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-plex.h"
#include "axis-test-requests.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "web/axis-password.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

/* A library folder whose name no page of a person who asks may show. */
#define FOLDER_MARK "zq-folder-mark"

typedef struct
{
	AxisTestRequests   *r;
	AxisWebSettings    *web_settings;
	AxisWebMeta        *meta_ui;
	AxisWebIndexers    *indexers_ui;
	AxisWebDownloads   *downloads_ui;
	AxisWebQuality     *quality_ui;
	AxisModuleRegistry *registry;
	AxisWebHub         *hub;
	AxisWebJobs        *jobs;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	gchar              *admin;
	gchar              *robin;
	gchar              *sam;
	gint64              marked_root;
	gboolean            real;
} Fixture;

/* The fixture with accounts in the database and a real login (the others
 * have sessions made for the test and no database behind the accounts'
 * pages). */
#define REAL_ACCOUNTS  1
#define ADMIN_PASSWORD "a made-up password of the tests"

/* The cookies a reply sets, as a Cookie header's value, added to @have
 * (nullable); a cookie that is being cleared is left out. */
static gchar *
cookies_after(
	AxisTestReply *reply,
	const gchar   *have
){
	GString *cookies = g_string_new(have != NULL ? have : "");
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;

	soup_message_headers_iter_init(&iter, soup_message_get_response_headers(reply->message));
	while (soup_message_headers_iter_next(&iter, &name, &value)) {
		g_autofree gchar *pair = NULL;

		if (g_ascii_strcasecmp(name, "Set-Cookie") != 0)
			continue;
		pair = g_strndup(value, strcspn(value, ";"));
		if (g_str_has_suffix(pair, "=") || strstr(value, "Max-Age=0") != NULL)
			continue;
		g_string_append_printf(cookies, "%s%s", cookies->len > 0 ? "; " : "", pair);
	}
	return g_string_free(cookies, FALSE);
}

/* Logs in through the login page; the session's cookies. */
static gchar *
log_in(
	Fixture     *fx,
	const gchar *username,
	const gchar *password
){
	g_autoptr(GRegex) field = g_regex_new("name=\"_csrf\"[^>]*value=\"([^\"]+)\"|value=\"([^\"]+)\"[^>]*name=\"_csrf\"", 0,
		0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *secret = g_uri_escape_string(password, NULL, TRUE);
	g_autofree gchar *form = NULL;
	AxisTestReply page = axis_test_web_request(fx->session, fx->port, "GET", "/login", NULL, NULL, NULL);
	AxisTestReply reply;
	gchar *cookies;

	g_assert_cmpuint(page.status, ==, 200);
	before = cookies_after(&page, NULL);
	g_assert_true(g_regex_match(field, page.body, 0, &match));
	token = g_match_info_fetch(match, 1);
	if (token == NULL || token[0] == '\0') {
		g_free(token);
		token = g_match_info_fetch(match, 2);
	}
	axis_test_reply_clear(&page);
	escaped = g_uri_escape_string(token, NULL, TRUE);
	form = g_strdup_printf("_csrf=%s&username=%s&password=%s", escaped, username, secret);
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/login", before, NULL, form);
	if (reply.status != 303)
		g_error("login as %s: expected 303, got %u:\n%s", username, reply.status, reply.body);
	cookies = cookies_after(&reply, NULL);
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(cookies, !=, "");
	return cookies;
}

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->r->fa->api, path, 200, "application/json", body, NULL);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestRequests *r;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->r = r = axis_test_requests_new();
	if (r == NULL)
		return;
	fx->web_settings = axis_web_settings_new(r->settings, r->t->pool);
	fx->meta_ui = axis_web_meta_new(r->meta);
	fx->indexers_ui = axis_web_indexers_new(r->indexers);
	fx->downloads_ui = axis_web_downloads_new(r->downloads, r->t->storage, NULL);
	fx->quality_ui = axis_web_quality_new(r->quality);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), r->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), r->tv, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_requests_get_module(), r->requests, &error));
	fx->hub = axis_web_hub_new(NULL);
	fx->jobs = axis_web_jobs_new(r->t->pool, fx->hub);
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_jobs_get_module(), fx->jobs, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(r->settings, fx->registry, &error));
	g_assert_no_error(error);
	axis_test_requests_ready(r);
	g_assert_true(axis_module_state_load(fx->registry, r->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);

	/* With real accounts: the administrator has a password to log in with,
	 * and the Users page works on the database. */
	fx->real = GPOINTER_TO_UINT(data) == REAL_ACCOUNTS;
	if (fx->real) {
		g_autofree gchar *hash = axis_password_hash(ADMIN_PASSWORD, &error);

		g_assert_no_error(error);
		g_assert_null(strchr(hash, '\''));
		axis_test_requests_n(r, "UPDATE app_user SET password_hash = '%s' WHERE username = 'root' RETURNING id", hash);
	}

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	fx->auth = axis_web_auth_new(fx->real ? r->t->pool : NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	axis_web_hub_install(fx->hub, fx->web, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	/* The sessions' accounts are the database's, by their ids: the first
	 * three accounts of each, made in the same order. */
	g_assert_cmpint(r->admin, ==, 1);
	g_assert_cmpint(r->robin, ==, 2);
	g_assert_cmpint(r->sam, ==, 3);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);
	if (fx->real) {
		fx->admin = log_in(fx, "root", ADMIN_PASSWORD);
	} else {
		fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
		fx->robin = axis_web_auth_open_test_session(fx->auth, "robin", AXIS_USER_ROLE_USER);
		fx->sam = axis_web_auth_open_test_session(fx->auth, "sam", AXIS_USER_ROLE_USER);
	}
	fx->marked_root = axis_test_library_add_root(r->t, r->volume_id, r->disk, FOLDER_MARK, AXIS_MEDIA_MOVIES);

	/* A search for films finds five of the made-up ones. */
	set_route(fx, "/search/film", "{\"results\":["
		"{\"id\":\"" AXIS_TEST_REQUESTS_ORCHARD "\",\"kind\":\"film\",\"title\":\"The Lantern Orchard\",\"year\":2019},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_LEDGER "\",\"kind\":\"film\",\"title\":\"Saltmarsh Ledger\",\"year\":2015},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_KILN "\",\"kind\":\"film\",\"title\":\"Paper Kiln\",\"year\":2011},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_BELLS "\",\"kind\":\"film\",\"title\":\"Quarry Bells\",\"year\":2008},"
		"{\"id\":\"" AXIS_TEST_REQUESTS_COMPASS "\",\"kind\":\"film\",\"title\":\"Tin Compass\",\"year\":2021}]}");
	set_route(fx, "/search/series", "{\"results\":[]}");
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	if (fx->r == NULL) {
		axis_test_log_reset();
		return;
	}
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	if (fx->real)
		axis_web_auth_stop(fx->auth);
	axis_web_stop(fx->web);
	axis_web_jobs_stop(fx->jobs);
	axis_requests_stop(fx->r->requests);
	axis_movies_stop(fx->r->movies);
	axis_tv_stop(fx->r->tv);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_jobs_free(fx->jobs);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_quality_free(fx->quality_ui);
	axis_web_downloads_free(fx->downloads_ui);
	axis_web_indexers_free(fx->indexers_ui);
	axis_web_meta_free(fx->meta_ui);
	axis_web_settings_free(fx->web_settings);
	g_free(fx->admin);
	g_free(fx->robin);
	g_free(fx->sam);
	axis_test_requests_free(fx->r);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->r != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

#define Q(...) axis_test_requests_q(fx->r, __VA_ARGS__)
#define N(...) axis_test_requests_n(fx->r, __VA_ARGS__)

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

/* A form posted from a page the user has open: its CSRF token from there
 * (@token_from NULL: no token). */
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
		g_error("expected to find '%s' in:\n%s\n(expected to find '%s')", needle, haystack != NULL ? haystack : "(null)",
			needle);
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain != NULL && strstr(plain, needle) != NULL)
		g_error("did not expect '%s' in:\n%s\n(did not expect '%s')", needle, haystack, needle);
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);
	g_autoptr(GRegex) handler = g_regex_new("<[a-zA-Z][^>]*\\son[a-z]+=", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	g_assert_false(g_regex_match(handler, body, 0, NULL));
	assert_lacks(body, "javascript:");
}

static gint64
film(
	Fixture     *fx,
	const gchar *id
){
	return axis_test_requests_title(fx->r, AXIS_META_KIND_FILM, id);
}

static gint64
series(
	Fixture     *fx,
	const gchar *id
){
	return axis_test_requests_title(fx->r, AXIS_META_KIND_SERIES, id);
}

static gchar *
title_path(gint64 title)
{
	return g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
}

/* Asks for a movie through the model; the request's public identifier. */
static gchar *
ask_movie(
	Fixture *fx,
	gint64   user,
	gint64   title
){
	g_autoptr(GError) error = NULL;
	AxisRequestMade made;
	AxisRequestAsk ask;
	gchar *id;

	memset(&ask, 0, sizeof(ask));
	ask.user_id = user;
	ask.title_id = title;
	ask.kind = AXIS_REQUEST_MOVIE;
	g_assert_true(axis_requests_ask(fx->r->requests, &ask, &made, &error));
	g_assert_no_error(error);
	g_assert_cmpint(made.outcome, ==, AXIS_REQUEST_MADE);
	id = g_strdup(made.public_id);
	axis_request_made_clear(&made);
	return id;
}

/* The state a title's card on a list of results reads: the name in the class
 * of the first request mark after the link to the title. */
static gchar *
card_state(
	const gchar *page,
	gint64       title
){
	g_autofree gchar *link = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
	const gchar *at = strstr(page, link);
	const gchar *mark;
	const gchar *end;

	g_assert_nonnull(at);
	mark = strstr(at, "request-mark-");
	g_assert_nonnull(mark);
	mark += strlen("request-mark-");
	end = mark;
	while (g_ascii_isalpha(*end))
		end++;
	return g_strndup(mark, end - mark);
}

/* ── Asking ─────────────────────────────────────────────────────── */

/*
 * Scenarios: An ordinary user asks (from a search result's card and from the
 * title's page; a Discover card draws the same piece); An ordinary user still
 * cannot add; A forged request post; A request cannot choose; No episodes and
 * nothing typed (the form's part).
 */
static void
test_user_asks(Fixture *fx, gconstpointer data)
{
	static const gchar *const extras[] = {
		"root=1", "profile=1", "quality_profile=1", "monitor=all", "monitored=yes", "episode=3", "note=please",
		"season-1x=yes", "user=1", "requested_by=1", "availability=announced", "search=yes"
	};
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *page_path = title_path(orchard);
	g_autofree gchar *form = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=movie", orchard);
	g_autofree gchar *add = g_strdup_printf("title=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&root=%"
		G_GINT64_FORMAT "&availability=released&monitored=yes&search=yes", orchard, fx->r->profile_id,
		fx->r->films_root);
	AxisTestReply reply;
	guint i;

	/* The title's page: its state, and Request where an administrator has
	 * Add. */
	reply = get_as(fx, fx->robin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Can be requested");
	assert_contains(reply.body, "action=\"/requests\"");
	assert_contains(reply.body, ">Request<");
	assert_lacks(reply.body, "Add to Movies");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page_path);
	assert_contains(reply.body, "Can be requested");
	assert_lacks(reply.body, "action=\"/requests\"");
	axis_test_reply_clear(&reply);
	/* A card of the search results. */
	reply = get_as(fx, fx->robin, "/search?q=lantern");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The Lantern Orchard");
	assert_contains(reply.body, "action=\"/requests\"");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);

	/* An ordinary user still cannot add. */
	reply = post_as(fx, fx->robin, page_path, "/films", add);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);

	/* Without the session's CSRF token. */
	reply = post_as(fx, fx->robin, NULL, "/requests", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Without a session at all. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/requests", NULL, NULL, form);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, <, 500);
	axis_test_reply_clear(&reply);
	/* With a field that is not a title, a kind or seasons: refused whole. */
	for (i = 0; i < G_N_ELEMENTS(extras); i++) {
		g_autofree gchar *with = g_strdup_printf("%s&%s", form, extras[i]);

		reply = post_as(fx, fx->robin, page_path, "/requests", with);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	/* Seasons for a movie, no kind, no title, a title that is no number. */
	reply = post_as(fx, fx->robin, page_path, "/requests", "title=1&kind=movie&seasons=all");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, page_path, "/requests", "title=1");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, page_path, "/requests", "kind=movie&title=1%20OR%201%3D1");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);

	/* Asked: a waiting request in their name, and no movie was added. */
	reply = post_as(fx, fx->robin, page_path, "/requests", form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=requested");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE requested_by = %" G_GINT64_FORMAT " AND title_id = %"
		G_GINT64_FORMAT " AND kind = 'movie' AND decision = 'waiting'", fx->r->robin, orchard), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM job"), ==, 0);
	reply = get_as(fx, fx->robin, page_path);
	assert_contains(reply.body, "Requested");
	assert_contains(reply.body, "You asked for it");
	assert_lacks(reply.body, "action=\"/requests\"");
	axis_test_reply_clear(&reply);
	/* Asked twice: one request. */
	reply = post_as(fx, fx->robin, page_path, "/requests", form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=request_unchanged");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 1);
}

/* Scenarios: What is left is shown; My requests; An ordinary user; Nothing of
 * the engine's inner workings; Another person's request by its identifier. */
static void
test_mine(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	g_autofree gchar *r1 = ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *r2 = ask_movie(fx, fx->r->robin, ledger);
	g_autofree gchar *r3 = ask_movie(fx, fx->r->robin, kiln);
	g_autofree gchar *s1 = ask_movie(fx, fx->r->sam, bells);
	g_autofree gchar *s2 = ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_COMPASS));
	g_autofree gchar *profile_name = Q("SELECT name FROM quality_profile WHERE id = %" G_GINT64_FORMAT,
		fx->r->profile_id);
	g_autofree gchar *own = g_strdup_printf("/requests/%s", r1);
	g_autofree gchar *others = g_strdup_printf("/requests/%s", s2);
	g_autofree gchar *withdraw_others = g_strdup_printf("/requests/%s/withdraw", s2);
	g_autofree gchar *unfollow_others = g_strdup_printf("/requests/%s/unfollow", s2);
	g_autofree gchar *follow = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=movie", bells);
	g_autofree gchar *unknown_body = NULL;
	g_autoptr(GError) error = NULL;
	gboolean added = FALSE;
	AxisTestReply reply;
	gint64 movie;
	gint64 grab;

	/* robin follows one of sam's. */
	reply = post_as(fx, fx->robin, "/requests", "/requests", follow);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=request_followed");
	axis_test_reply_clear(&reply);
	/* One of robin's is approved into a folder and with a profile whose
	 * names no page of robin's may show, downloads, fails and downloads. */
	g_assert_true(axis_requests_approve(fx->r->requests, r1, "root", fx->marked_root, fx->r->profile_id, &added, &error));
	g_assert_no_error(error);
	g_assert_true(added);
	movie = N("SELECT id FROM movie WHERE title_id = %" G_GINT64_FORMAT, orchard);
	axis_test_requests_movie_grab(fx->r, movie, "failed");
	grab = axis_test_requests_movie_grab(fx->r, movie, NULL);
	g_assert_cmpint(grab, >, 0);
	g_assert_true(axis_requests_decline(fx->r->requests, r2, "root", "Too <b>long</b> & \"loud\"", FALSE, &error));
	g_assert_no_error(error);

	reply = get_as(fx, fx->robin, "/requests");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "My requests");
	/* Three asked of five. */
	assert_contains(reply.body, "Movies: 2 of 5 left this week.");
	assert_contains(reply.body, "Seasons of series: 5 of 5 left this week.");
	/* Those three and the one followed; none of sam's others. */
	assert_contains(reply.body, "The Lantern Orchard");
	assert_contains(reply.body, "Saltmarsh Ledger");
	assert_contains(reply.body, "Paper Kiln");
	assert_contains(reply.body, "Quarry Bells");
	assert_contains(reply.body, "You follow it.");
	assert_lacks(reply.body, "Tin Compass");
	assert_lacks(reply.body, s2);
	assert_contains(reply.body, "Downloading");
	/* The reason, as text. */
	assert_contains(reply.body, "Reason: Too &lt;b&gt;long&lt;/b&gt; &amp; ");
	assert_lacks(reply.body, "<b>long</b>");
	/* No control to approve or decline, and nothing of the inner workings. */
	assert_lacks(reply.body, "/approve");
	assert_lacks(reply.body, "/decline");
	assert_lacks(reply.body, "/retry");
	assert_lacks(reply.body, "/requests/people");
	assert_lacks(reply.body, "/requests/defaults");
	assert_lacks(reply.body, FOLDER_MARK);
	assert_lacks(reply.body, profile_name);
	assert_lacks(reply.body, "Made.Up.Release");
	assert_lacks(reply.body, "MADEUP");
	assert_lacks(reply.body, ".mkv");
	assert_lacks(reply.body, "usenet");
	assert_contains(reply.body, "/withdraw");
	assert_contains(reply.body, "/unfollow");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	/* The download failed for good and nothing is in flight. */
	N("UPDATE movie_grab SET closed_at = now(), close_reason = 'failed' WHERE id = %" G_GINT64_FORMAT " RETURNING id",
		grab);
	reply = get_as(fx, fx->robin, own);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Failed: A download failed. Axis is looking again.");
	assert_lacks(reply.body, FOLDER_MARK);
	assert_lacks(reply.body, profile_name);
	assert_lacks(reply.body, "Made.Up.Release");
	assert_lacks(reply.body, "/approve");
	axis_test_reply_clear(&reply);

	/* Another person's request by its identifier: the answer of one that was
	 * never issued, to read and to change. */
	reply = get_as(fx, fx->robin, "/requests/AAAAAAAAAAAAAAAAAAAAAA");
	g_assert_cmpuint(reply.status, ==, 404);
	unknown_body = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, others);
	g_assert_cmpuint(reply.status, ==, 404);
	g_assert_cmpstr(reply.body, ==, unknown_body);
	assert_lacks(reply.body, "Tin Compass");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, "/requests", withdraw_others, "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, "/requests", unfollow_others, "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'waiting'", s2), ==, 1);
	/* An administrator reads any. */
	reply = get_as(fx, fx->admin, others);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Tin Compass");
	axis_test_reply_clear(&reply);

	/* Withdrawing their own, with the token; never without. */
	{
		g_autofree gchar *withdraw = g_strdup_printf("/requests/%s/withdraw", r3);

		reply = post_as(fx, fx->robin, NULL, withdraw, "");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'waiting'", r3), ==, 1);
		reply = post_as(fx, fx->robin, "/requests", withdraw, "");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'withdrawn'", r3), ==, 1);
		/* Not twice, and not one that was approved. */
		reply = post_as(fx, fx->robin, "/requests", withdraw, "");
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	(void)s1;
}

/* ── Deciding ───────────────────────────────────────────────────── */

/* Scenarios: Not an administrator; No defaults yet; The waiting list; A
 * decline has a reason (through the page). */
static void
test_decide(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *third = NULL;
	g_autofree gchar *decided = NULL;
	g_autofree gchar *approve = NULL;
	g_autofree gchar *decline = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *placed = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT, fx->r->films_root,
		fx->r->profile_id);
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	/* Three wait, made a minute apart; one was decided. */
	decided = ask_movie(fx, fx->r->robin, bells);
	g_assert_true(axis_requests_decline(fx->r->requests, decided, "root", "Not for this library", FALSE, &error));
	first = ask_movie(fx, fx->r->robin, orchard);
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + G_TIME_SPAN_MINUTE);
	second = ask_movie(fx, fx->r->sam, ledger);
	axis_requests_set_clock(fx->r->requests, AXIS_TEST_REQUESTS_NOW_US + 2 * G_TIME_SPAN_MINUTE);
	third = ask_movie(fx, fx->r->robin, kiln);
	approve = g_strdup_printf("/requests/%s/approve", first);
	decline = g_strdup_printf("/requests/%s/decline", second);
	one = g_strdup_printf("/requests/%s", first);

	/* Not an administrator: 403, and the request still waits. */
	reply = post_as(fx, fx->robin, "/requests", approve, placed);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, "/requests", decline, "reason=No");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, "/requests", "/requests/defaults", "movies_root=1");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, "/requests/people");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE decision = 'waiting'"), ==, 3);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);

	/* The Requests page of an administrator: the waiting ones first, the
	 * oldest at the top, each saying who asked and what an approval would
	 * look for. */
	reply = get_as(fx, fx->admin, "/requests");
	g_assert_cmpuint(reply.status, ==, 200);
	{
		g_autofree gchar *row1 = g_strdup_printf("id=\"request-%s\"", first);
		g_autofree gchar *row2 = g_strdup_printf("id=\"request-%s\"", second);
		g_autofree gchar *row3 = g_strdup_printf("id=\"request-%s\"", third);
		g_autofree gchar *row4 = g_strdup_printf("id=\"request-%s\"", decided);
		const gchar *p1 = strstr(reply.body, row1);
		const gchar *p2 = strstr(reply.body, row2);
		const gchar *p3 = strstr(reply.body, row3);
		const gchar *p4 = strstr(reply.body, row4);

		g_assert_nonnull(p1);
		g_assert_nonnull(p2);
		g_assert_nonnull(p3);
		g_assert_nonnull(p4);
		g_assert_true(p1 < p2 && p2 < p3 && p3 < p4);
	}
	assert_contains(reply.body, "Waiting for you");
	assert_contains(reply.body, "robin, ");
	assert_contains(reply.body, "sam, ");
	assert_contains(reply.body, "An approval looks for 1 movie.");
	assert_contains(reply.body, "Not for this library");
	/* No defaults yet: the approval asks for the folder and the profile. */
	assert_contains(reply.body, "name=\"root\"");
	assert_contains(reply.body, "name=\"profile\"");
	assert_contains(reply.body, "Choose a folder");
	assert_contains(reply.body, "Choose a profile");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, one);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Choose a folder");
	assert_contains(reply.body, "The Lantern Orchard");
	axis_test_reply_clear(&reply);
	/* Nothing is approved until both are chosen. */
	reply = post_as(fx, fx->admin, "/requests", approve, "");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Choose a library folder and a quality profile");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *only_root = g_strdup_printf("root=%" G_GINT64_FORMAT, fx->r->films_root);

		reply = post_as(fx, fx->admin, "/requests", approve, only_root);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE decision = 'waiting'"), ==, 3);
	/* Without the CSRF token: refused. */
	reply = post_as(fx, fx->admin, NULL, approve, placed);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);

	/* Approved with both. */
	reply = post_as(fx, fx->admin, "/requests", approve, placed);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=approved");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT " AND root_id = %" G_GINT64_FORMAT,
		orchard, fx->r->films_root), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'approved'"
		" AND decided_by = 'root'", first), ==, 1);
	/* Decided once. */
	reply = post_as(fx, fx->admin, "/requests", approve, placed);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "already decided");
	axis_test_reply_clear(&reply);

	/* Declined: a reason is needed, at most 300 characters, and it may close
	 * the title. */
	reply = post_as(fx, fx->admin, "/requests", decline, "reason=");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *fill = g_strnfill(301, 'x');
		g_autofree gchar *too_long = g_strconcat("reason=", fill, NULL);

		reply = post_as(fx, fx->admin, "/requests", decline, too_long);
		g_assert_cmpuint(reply.status, ==, 400);
		/* What was typed is not sent back. */
		assert_lacks(reply.body, "xxxxxxxxxx");
		axis_test_reply_clear(&reply);
	}
	reply = post_as(fx, fx->admin, "/requests", decline, "reason=Not%20for%20this%20library&close=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'declined'"
		" AND decline_reason = 'Not for this library'", second), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request_closed_title WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 1);
	/* Closed: the title's page says so, and a request is refused. */
	{
		g_autofree gchar *page_path = title_path(ledger);
		g_autofree gchar *form = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=movie", ledger);
		g_autofree gchar *open = g_strdup_printf("/requests/titles/%" G_GINT64_FORMAT "/open", ledger);

		reply = get_as(fx, fx->robin, page_path);
		assert_contains(reply.body, "Closed to requests");
		assert_lacks(reply.body, "action=\"/requests\"");
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->robin, page_path, "/requests", form);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "closed to requests");
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->robin, page_path, open, "");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, "/requests");
		assert_contains(reply.body, "Closed to requests");
		assert_contains(reply.body, open);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, "/requests", open, "");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_assert_cmpint(N("SELECT count(*) FROM request_closed_title"), ==, 0);
		reply = post_as(fx, fx->robin, page_path, "/requests", form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
}

/* Scenarios: The folder is gone; Tried again (through the pages). */
static void
test_try_again(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *approve = g_strdup_printf("/requests/%s/approve", id);
	g_autofree gchar *retry = g_strdup_printf("/requests/%s/retry", id);
	g_autofree gchar *own = g_strdup_printf("/requests/%s", id);
	g_autofree gchar *gone = g_strdup_printf("root=987654&profile=%" G_GINT64_FORMAT, fx->r->profile_id);
	g_autofree gchar *good = g_strdup_printf("root=%" G_GINT64_FORMAT, fx->r->films_root);
	AxisTestReply reply;

	reply = post_as(fx, fx->admin, "/requests", approve, gone);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=approved_not_added");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM movie"), ==, 0);
	reply = get_as(fx, fx->admin, "/requests");
	assert_contains(reply.body, "The library folder chosen for it no longer exists.");
	assert_contains(reply.body, "Try again");
	assert_contains(reply.body, retry);
	axis_test_reply_clear(&reply);
	/* The person who asked sees that it could not be added, and not why. */
	reply = get_as(fx, fx->robin, own);
	assert_contains(reply.body, "Failed: The title could not be added.");
	assert_lacks(reply.body, "library folder");
	assert_lacks(reply.body, retry);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, "/requests", retry, good);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/requests", retry, good);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=retried");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, orchard), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 1);
}

/* ── States and who is told what ────────────────────────────────── */

/* Scenario: The states of a search, for an administrator and two users
 * alike. */
static void
test_states_of_a_search(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);	/* in the library */
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);	/* being downloaded */
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);	/* waits for approval */
	gint64 bells = film(fx, AXIS_TEST_REQUESTS_BELLS);	/* declined */
	gint64 compass = film(fx, AXIS_TEST_REQUESTS_COMPASS);	/* nobody asked */
	const gchar *const viewers[] = { fx->admin, fx->robin, fx->sam };
	g_autofree gchar *waiting = NULL;
	g_autofree gchar *declined = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	axis_test_requests_movie_file(fx->r, axis_test_requests_add_movie(fx->r, orchard));
	axis_test_requests_movie_grab(fx->r, axis_test_requests_add_movie(fx->r, ledger), NULL);
	waiting = ask_movie(fx, fx->r->robin, kiln);
	declined = ask_movie(fx, fx->r->robin, bells);
	g_assert_true(axis_requests_decline(fx->r->requests, declined, "root", "Not for this library", FALSE, &error));

	for (i = 0; i < G_N_ELEMENTS(viewers); i++) {
		AxisTestReply reply = get_as(fx, viewers[i], "/search?q=made&kind=film");
		g_autofree gchar *s1 = NULL;
		g_autofree gchar *s2 = NULL;
		g_autofree gchar *s3 = NULL;
		g_autofree gchar *s4 = NULL;
		g_autofree gchar *s5 = NULL;

		g_assert_cmpuint(reply.status, ==, 200);
		s1 = card_state(reply.body, orchard);
		s2 = card_state(reply.body, ledger);
		s3 = card_state(reply.body, kiln);
		s4 = card_state(reply.body, bells);
		s5 = card_state(reply.body, compass);
		g_assert_cmpstr(s1, ==, "available");
		g_assert_cmpstr(s2, ==, "downloading");
		g_assert_cmpstr(s3, ==, "requested");
		g_assert_cmpstr(s4, ==, "declined");
		g_assert_cmpstr(s5, ==, "can");
		assert_contains(reply.body, "Available");
		assert_contains(reply.body, "Downloading");
		assert_contains(reply.body, "Requested");
		assert_contains(reply.body, "Declined");
		assert_contains(reply.body, "Can be requested");
		assert_csp_clean(reply.body);
		axis_test_reply_clear(&reply);
	}
}

/* Scenarios: Who asked, shown; Who asked, hidden; A reason is not for
 * everyone. */
static void
test_who_asked(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	g_autofree gchar *waiting = ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *declined = ask_movie(fx, fx->r->robin, ledger);
	g_autofree gchar *waiting_page = title_path(orchard);
	g_autofree gchar *declined_page = title_path(ledger);
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	g_assert_true(axis_requests_decline(fx->r->requests, declined, "root", "Kept for the winter list", FALSE, &error));
	/* Shown (the default). */
	reply = get_as(fx, fx->sam, waiting_page);
	assert_contains(reply.body, "Requested: robin asked for it");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->sam, "/search?q=made&kind=film");
	assert_contains(reply.body, "Requested: robin asked for it");
	axis_test_reply_clear(&reply);
	/* A reason is not for everyone: sam sees declined, and not why. */
	reply = get_as(fx, fx->sam, declined_page);
	assert_contains(reply.body, "Declined");
	assert_lacks(reply.body, "Kept for the winter list");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->sam, "/search?q=made&kind=film");
	assert_lacks(reply.body, "Kept for the winter list");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, declined_page);
	assert_contains(reply.body, "Declined: Kept for the winter list");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, declined_page);
	assert_contains(reply.body, "Declined: Kept for the winter list");
	axis_test_reply_clear(&reply);

	/* Hidden: nothing sam is sent names robin. */
	g_assert_true(axis_settings_set(fx->r->settings, AXIS_REQUESTS_SETTING_SHOW_WHO, "false", &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->sam, waiting_page);
	assert_contains(reply.body, "Requested");
	assert_lacks(reply.body, "robin");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->sam, declined_page);
	assert_contains(reply.body, "Declined");
	assert_lacks(reply.body, "robin");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->sam, "/search?q=made&kind=film");
	assert_contains(reply.body, "Requested");
	assert_lacks(reply.body, "robin");
	axis_test_reply_clear(&reply);
	/* A follower's own list then says "someone else". */
	{
		g_autofree gchar *form = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=movie", orchard);

		reply = post_as(fx, fx->sam, waiting_page, "/requests", form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->sam, "/requests");
		assert_contains(reply.body, "The Lantern Orchard");
		assert_contains(reply.body, "Someone else");
		assert_lacks(reply.body, "robin");
		axis_test_reply_clear(&reply);
	}
	/* An administrator is shown the name always; robin their own. */
	reply = get_as(fx, fx->admin, waiting_page);
	assert_contains(reply.body, "Requested: robin asked for it");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, waiting_page);
	assert_contains(reply.body, "Requested: robin asked for it");
	axis_test_reply_clear(&reply);
	(void)waiting;
}

/* Scenarios: Seasons of a series (its page); picked seasons through the
 * form; A limit of none (no series request is offered, and one posted anyway
 * is refused). */
static void
test_series_page(Fixture *fx, gconstpointer data)
{
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 series_id = axis_test_requests_add_series(fx->r, harbour);
	g_autofree gchar *page_path = title_path(harbour);
	g_autofree gchar *picked = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=series&seasons=picked&season-3=yes",
		harbour);
	g_autofree gchar *whole = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=series&seasons=all", harbour);
	g_autofree gchar *listed = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestPerson person;
	AxisTestReply reply;

	/* Season 1 with every aired episode on disk, season 2 with half. */
	axis_test_requests_episode_files(fx->r, series_id, 1, 2);
	axis_test_requests_episode_files(fx->r, series_id, 2, 1);
	reply = post_as(fx, fx->robin, page_path, "/requests", picked);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	listed = Q("SELECT string_agg(season::text, ',') FROM request_season");
	g_assert_cmpstr(listed, ==, "3");

	reply = get_as(fx, fx->sam, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Partly available");
	assert_contains(reply.body, "Season 1");
	assert_contains(reply.body, "Partly available, 1 of 2 episodes");
	assert_contains(reply.body, "Requested");
	assert_contains(reply.body, "Specials");
	/* What can still be asked for can be ticked: the specials and season 2. */
	assert_contains(reply.body, "name=\"season-0\"");
	assert_contains(reply.body, "name=\"season-2\"");
	assert_lacks(reply.body, "name=\"season-1\"");
	assert_lacks(reply.body, "name=\"season-3\"");
	axis_test_web_assert_tables(reply.body);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);

	/* A limit of none for series: no request is offered, and one posted
	 * anyway is refused. */
	memset(&person, 0, sizeof(person));
	person.id = fx->r->sam;
	person.movies_limit = -1;
	person.seasons_limit = 0;
	g_assert_true(axis_requests_set_person(fx->r->requests, &person, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->sam, page_path);
	assert_contains(reply.body, "Partly available");
	assert_lacks(reply.body, "action=\"/requests\"");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->sam, page_path, "/requests", whole);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "cannot ask for series");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE requested_by = %" G_GINT64_FORMAT, fx->r->sam), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request_follower"), ==, 0);
	reply = get_as(fx, fx->sam, "/requests");
	assert_contains(reply.body, "None: you cannot ask for series");
	axis_test_reply_clear(&reply);
}

/* Scenario: A module that is off. */
static void
test_module_off(Fixture *fx, gconstpointer data)
{
	gint64 harbour = series(fx, AXIS_TEST_REQUESTS_HARBOUR);
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *page_path = title_path(harbour);
	g_autofree gchar *film_path = title_path(orchard);
	g_autofree gchar *whole = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=series&seasons=all", harbour);
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	reply = get_as(fx, fx->robin, page_path);
	assert_contains(reply.body, "action=\"/requests\"");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_TV_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->robin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "action=\"/requests\"");
	assert_lacks(reply.body, "request-mark");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, film_path, "/requests", whole);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Series cannot be requested here.");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM request"), ==, 0);
	/* Movies still can. */
	reply = get_as(fx, fx->robin, film_path);
	assert_contains(reply.body, "action=\"/requests\"");
	axis_test_reply_clear(&reply);
	/* With the requests' own module off, its pages are gone. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_REQUESTS_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->robin, "/requests");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, film_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "action=\"/requests\"");
	axis_test_reply_clear(&reply);
}

/* Scenario: The switches and limits of a person; and the defaults. */
static void
test_people(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = g_strdup_printf("/requests/people/%" G_GINT64_FORMAT, fx->r->robin);
	g_autofree gchar *defaults = g_strdup_printf("movies_root=%" G_GINT64_FORMAT "&movies_profile=%" G_GINT64_FORMAT
		"&series_root=%" G_GINT64_FORMAT "&series_profile=%" G_GINT64_FORMAT, fx->r->films_root, fx->r->profile_id,
		fx->r->series_root, fx->r->profile_id);
	g_autofree gchar *wrong = g_strdup_printf("movies_root=%" G_GINT64_FORMAT "&movies_profile=%" G_GINT64_FORMAT,
		fx->r->series_root, fx->r->profile_id);
	g_autofree gchar *stored = NULL;
	AxisTestReply reply;

	reply = get_as(fx, fx->admin, "/requests/people");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "robin");
	assert_contains(reply.body, "Approve their movie requests automatically");
	assert_lacks(reply.body, "checked");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);

	/* A change without the CSRF token, and by someone else, is refused. */
	reply = post_as(fx, fx->admin, NULL, path, "auto_movies=yes&seasons_limit=2");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, "/requests", path, "auto_movies=yes&seasons_limit=2");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/requests/people", path, "auto_movies=yes&seasons_limit=2000");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/requests/people", path, "auto_movies=yes&seasons_limit=two");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	stored = Q("SELECT request_auto_movies || ' ' || request_auto_series || ' ' || coalesce(request_movies_limit::text,"
		" '-') || ' ' || coalesce(request_seasons_limit::text, '-') FROM app_user WHERE username = 'robin'");
	g_assert_cmpstr(stored, ==, "false false - -");
	g_clear_pointer(&stored, g_free);

	/* The movies switch on and the series limit 2: stored and shown. */
	reply = post_as(fx, fx->admin, "/requests/people", path, "auto_movies=yes&movies_limit=&seasons_limit=2");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	stored = Q("SELECT request_auto_movies || ' ' || request_auto_series || ' ' || coalesce(request_movies_limit::text,"
		" '-') || ' ' || coalesce(request_seasons_limit::text, '-') FROM app_user WHERE username = 'robin'");
	g_assert_cmpstr(stored, ==, "true false - 2");
	reply = get_as(fx, fx->admin, "/requests/people");
	assert_contains(reply.body, "checked");
	assert_contains(reply.body, "value=\"2\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, "/requests");
	assert_contains(reply.body, "Seasons of series: 2 of 2 left this week.");
	axis_test_reply_clear(&reply);

	/* The defaults: a folder of the other kind is no folder for movies. */
	reply = post_as(fx, fx->admin, "/requests", "/requests/defaults", wrong);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, "/requests/defaults", defaults);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/requests", "/requests/defaults", defaults);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(axis_settings_get_integer(fx->r->settings, AXIS_REQUESTS_SETTING_MOVIES_ROOT), ==,
		fx->r->films_root);
	g_assert_cmpint(axis_settings_get_integer(fx->r->settings, AXIS_REQUESTS_SETTING_SERIES_PROFILE), ==,
		fx->r->profile_id);
	/* With them, robin's movie request is approved when it is made. */
	{
		gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
		g_autofree gchar *page_path = title_path(orchard);
		g_autofree gchar *form = g_strdup_printf("title=%" G_GINT64_FORMAT "&kind=movie", orchard);

		reply = post_as(fx, fx->robin, page_path, "/requests", form);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "done=request_approved");
		axis_test_reply_clear(&reply);
		g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, orchard), ==, 1);
		reply = get_as(fx, fx->admin, "/requests");
		assert_contains(reply.body, "Approved automatically");
		axis_test_reply_clear(&reply);
	}
}

/* ── The notices ────────────────────────────────────────────────── */

/* Scenarios (attention): Requests wait; One request; Decided; Approved
 * automatically; Could not be added; Not an administrator. */
static void
test_attention(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	gint64 kiln = film(fx, AXIS_TEST_REQUESTS_KILN);
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *third = NULL;
	g_autoptr(GError) error = NULL;
	AxisRequestPerson person;
	AxisTestReply reply;
	gboolean added = TRUE;

	/* The readings judge by the real clock: so do these requests. */
	axis_requests_set_clock(fx->r->requests, 0);
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "approval");
	axis_test_reply_clear(&reply);

	/* One request waits. */
	first = ask_movie(fx, fx->r->robin, orchard);
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	assert_contains(reply.body, "1 request waits for approval");
	assert_contains(reply.body, "href=\"/requests\"");
	assert_contains(reply.body, "Waiting for you");
	axis_test_reply_clear(&reply);

	/* Three wait, and the oldest was made two hours ago. */
	second = ask_movie(fx, fx->r->robin, ledger);
	third = ask_movie(fx, fx->r->sam, kiln);
	N("UPDATE request SET created_at = now() - interval '2 hours 5 minutes' WHERE public_id = '%s' RETURNING id", first);
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	assert_contains(reply.body, "3 requests wait for approval, the oldest for 2 hours.");
	assert_contains(reply.body, "href=\"/requests\"");
	/* It names no one and no title. */
	assert_lacks(reply.body, "The Lantern Orchard");
	axis_test_reply_clear(&reply);

	/* Not an administrator: no attention control and no count, on any page. */
	reply = get_as(fx, fx->robin, "/requests");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "/attention");
	assert_lacks(reply.body, "wait for approval");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, "/");
	assert_lacks(reply.body, "/attention");
	assert_lacks(reply.body, "wait for approval");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->robin, "/attention");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* Decided, each of them: gone without anyone touching it.  One approval
	 * names a folder that is gone: that one could not be added. */
	g_assert_true(axis_requests_decline(fx->r->requests, first, "root", "Not for this library", FALSE, &error));
	g_assert_true(axis_requests_approve(fx->r->requests, second, "root", fx->r->films_root, fx->r->profile_id, &added,
		&error));
	g_assert_true(added);
	g_assert_true(axis_requests_approve(fx->r->requests, third, "root", 987654, fx->r->profile_id, &added, &error));
	g_assert_no_error(error);
	g_assert_false(added);
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	assert_lacks(reply.body, "for approval");
	assert_contains(reply.body, "1 approved request could not be added.");
	assert_contains(reply.body, "href=\"/requests\"");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_requests_try_again(fx->r->requests, third, "root", fx->r->films_root, 0, &added, &error));
	g_assert_true(added);

	/* Approved automatically: it was never part of the notice. */
	axis_test_requests_set_defaults(fx->r);
	memset(&person, 0, sizeof(person));
	person.id = fx->r->sam;
	person.auto_movies = TRUE;
	person.movies_limit = -1;
	person.seasons_limit = -1;
	g_assert_true(axis_requests_set_person(fx->r->requests, &person, &error));
	{
		g_autofree gchar *made = ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_BELLS));

		g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'approved'"
			" AND approved_how = 'auto'", made), ==, 1);
	}
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/attention");
	assert_lacks(reply.body, "for approval");
	assert_lacks(reply.body, "could not be added");
	axis_test_reply_clear(&reply);
}

/*
 * Scenario: A disabled requester (an ordinary user in this slice), through
 * the Users page with accounts in the database: disabling an account
 * withdraws what it still waits for and leaves what was approved; deleting
 * one does the same and the request still says who asked.
 */
static void
test_account_goes(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	gint64 ledger = film(fx, AXIS_TEST_REQUESTS_LEDGER);
	g_autofree gchar *waiting = ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *approved = ask_movie(fx, fx->r->robin, ledger);
	g_autofree gchar *sams = ask_movie(fx, fx->r->sam, film(fx, AXIS_TEST_REQUESTS_KILN));
	g_autofree gchar *disable = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/disable", fx->r->robin);
	g_autofree gchar *remove = g_strdup_printf("/admin/users/%" G_GINT64_FORMAT "/delete", fx->r->sam);
	g_autoptr(GError) error = NULL;
	gboolean added = FALSE;
	AxisTestReply reply;

	g_assert_true(axis_requests_approve(fx->r->requests, approved, "root", fx->r->films_root, fx->r->profile_id, &added,
		&error));
	g_assert_no_error(error);
	g_assert_true(added);
	/* The Users page leads to the people's switches and limits. */
	reply = get_as(fx, fx->admin, "/admin/users");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "href=\"/requests/people\"");
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/admin/users", disable, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'robin' AND disabled"), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'withdrawn'", waiting), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'approved'", approved), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, ledger), ==, 1);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'waiting'", sams), ==, 1);

	reply = post_as(fx, fx->admin, "/admin/users", remove, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(N("SELECT count(*) FROM app_user WHERE username = 'sam'"), ==, 0);
	g_assert_cmpint(N("SELECT count(*) FROM request WHERE public_id = '%s' AND decision = 'withdrawn'"
		" AND requested_by IS NULL AND requested_name = 'sam'", sams), ==, 1);
}

/* What a person typed is not written to the log above the debug level. */
static void
test_nothing_typed_is_logged(Fixture *fx, gconstpointer data)
{
	gint64 orchard = film(fx, AXIS_TEST_REQUESTS_ORCHARD);
	g_autofree gchar *id = ask_movie(fx, fx->r->robin, orchard);
	g_autofree gchar *decline = g_strdup_printf("/requests/%s/decline", id);
	g_autofree gchar *page_path = title_path(orchard);
	AxisTestReply reply;

	reply = post_as(fx, fx->robin, page_path, "/requests", "title=1&kind=movie&note=zq-typed-mark");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_lacks(reply.body, "zq-typed-mark");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->robin, page_path, "/requests", "title=1&kind=zq-typed-mark");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_lacks(reply.body, "zq-typed-mark");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/requests", decline, "reason=zq-typed-mark");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*zq-typed-mark*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_INFO, "*zq-typed-mark*"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*zq-typed-mark*"), ==, 0);
	/* The decision itself is in the audit trail, without the reason. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*audit: request * declined by=root*"), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	AXIS_TEST_ADD_DB_CASE("/web/requests/user-asks", Fixture, NULL, test_user_asks);
	AXIS_TEST_ADD_DB_CASE("/web/requests/mine", Fixture, NULL, test_mine);
	AXIS_TEST_ADD_DB_CASE("/web/requests/decide", Fixture, NULL, test_decide);
	AXIS_TEST_ADD_DB_CASE("/web/requests/try-again", Fixture, NULL, test_try_again);
	AXIS_TEST_ADD_DB_CASE("/web/requests/states-of-a-search", Fixture, NULL, test_states_of_a_search);
	AXIS_TEST_ADD_DB_CASE("/web/requests/who-asked", Fixture, NULL, test_who_asked);
	AXIS_TEST_ADD_DB_CASE("/web/requests/series-page", Fixture, NULL, test_series_page);
	AXIS_TEST_ADD_DB_CASE("/web/requests/module-off", Fixture, NULL, test_module_off);
	AXIS_TEST_ADD_DB_CASE("/web/requests/switches-and-limits", Fixture, NULL, test_people);
	AXIS_TEST_ADD_DB_CASE("/web/attention/requests", Fixture, NULL, test_attention);
	AXIS_TEST_ADD_DB_CASE("/web/requests/nothing-typed-is-logged", Fixture, NULL, test_nothing_typed_is_logged);
	AXIS_TEST_ADD_DB_CASE("/web/requests/account-goes", Fixture, GUINT_TO_POINTER(REAL_ACCOUNTS), test_account_goes);

	return g_test_run();
}
