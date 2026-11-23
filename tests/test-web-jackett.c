/*
 * test-web-jackett.c - "Fetch definitions from Jackett": the list an
 * administrator asks for, the fetch of ticked files at the listed commit,
 * what is refused and why, updating, putting an earlier version back; with
 * the real shell on an ephemeral loopback port, a real database, and an
 * in-process stand-in for GitHub's API and its file host
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing here asks the network: the stand-in listens on loopback, and the
 * layer is pointed at it by its tests' allowance.  The definitions are of
 * made-up sites (tests/fixtures/definitions), under made-up file names and
 * made-up commits.
 *
 * add-definition-fetch, tasks 6.1 to 6.3.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-plex.h"
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-cardigann.h"
#include "indexer/axis-indexer-jackett-parse.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-settings.h"

#define FIXTURE_FILE "tests/fixtures/definitions/example-html.yml"

/* Two made-up commits. */
#define COMMIT_A "aaaa1111bbbb2222cccc3333dddd4444eeee5555"
#define COMMIT_B "1111aaaa2222bbbb3333cccc4444dddd5555eeee"

#define API_COMMIT  "/repos/Jackett/Jackett/commits/master"
#define API_LISTING "/repos/Jackett/Jackett/contents/src/Jackett.Common/Definitions"
#define RAW_PREFIX  "/Jackett/Jackett/"
#define RAW_FOLDER  "/src/Jackett.Common/Definitions/"

#define PAGE  "/indexers/definitions/jackett"
#define LIST  PAGE "/list"
#define FETCH PAGE "/fetch"

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisIndexers       *indexers;
	AxisWebIndexers    *ui;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	AxisTestPlex       *github;
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	/* The stand-in is there before anything of Axis starts, so that a
	 * request made at start would be seen. */
	fx->github = axis_test_plex_new();
	base = axis_test_plex_base(fx->github);
	axis_indexers_jackett_set_testing(base, base);
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->settings = axis_settings_new();
	fx->indexers = axis_indexers_new(fx->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_cardigann_kind(), &error));
	fx->ui = axis_web_indexers_new(fx->indexers);
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->ui, &error));
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
	axis_db_pool_free(fx->pool);
	axis_indexers_jackett_set_testing(NULL, NULL);
	axis_test_plex_free(fx->github);
	axis_test_db_free(fx->db);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── The stand-in's answers ─────────────────────────────────────── */

/* The definition of a made-up site with the id @id: the hand-written
 * fixture under that id, with @extra at its end (a comment makes another
 * version of the same definition). */
static gchar *
definition(
	const gchar *id,
	const gchar *extra
){
	g_autofree gchar *text = NULL;
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *renamed = NULL;

	g_assert_true(g_file_get_contents(FIXTURE_FILE, &text, NULL, NULL));
	parts = g_strsplit(text, "example-html", -1);
	renamed = g_strjoinv(id, parts);
	return g_strconcat(renamed, extra != NULL ? extra : "", NULL);
}

typedef struct
{
	const gchar *name;
	const gchar *body;
} Listed;

/* GitHub's answer for the folder: each file as its contents API gives it,
 * and three entries Axis must leave out. */
static gchar *
listing_json(
	const Listed *files,
	guint         n
){
	GString *json = g_string_new("[");
	guint i;

	g_string_append(json, "{\"name\":\"more\",\"path\":\"src/Jackett.Common/Definitions/more\",\"sha\":\""
		COMMIT_B "\",\"size\":0,\"type\":\"dir\"},");
	g_string_append(json, "{\"name\":\"Bad_Name.yml\",\"sha\":\"" COMMIT_B "\",\"size\":10,\"type\":\"file\"},");
	g_string_append(json, "{\"name\":\"../upward.yml\",\"sha\":\"" COMMIT_B "\",\"size\":10,\"type\":\"file\"}");
	for (i = 0; i < n; i++) {
		g_autofree gchar *blob = axis_jackett_blob_id(files[i].body, strlen(files[i].body));

		g_string_append_printf(json, ",{\"name\":\"%s\",\"path\":\"src/Jackett.Common/Definitions/%s\",\"sha\":\"%s\","
			"\"size\":%u,\"type\":\"file\",\"_links\":{\"self\":\"x\"}}", files[i].name, files[i].name, blob,
			(guint)strlen(files[i].body));
	}
	g_string_append(json, "]");
	return g_string_free(json, FALSE);
}

/* The branch is at @commit, with these files in the folder. */
static void
github_head(
	Fixture      *fx,
	const gchar  *commit,
	const Listed *files,
	guint         n
){
	g_autofree gchar *json = listing_json(files, n);
	g_autoptr(GBytes) commit_body = g_bytes_new(commit, strlen(commit));
	g_autoptr(GBytes) listing_body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->github, API_COMMIT, 200, "application/vnd.github.sha", commit_body, NULL);
	axis_test_plex_route_full(fx->github, API_LISTING, 200, "application/json", listing_body, NULL);
}

static gchar *
raw_path(
	const gchar *commit,
	const gchar *name
){
	return g_strconcat(RAW_PREFIX, commit, RAW_FOLDER, name, NULL);
}

/* The file host has @body for @name at @commit. */
static void
github_file(
	Fixture     *fx,
	const gchar *commit,
	const gchar *name,
	const gchar *body
){
	g_autofree gchar *path = raw_path(commit, name);
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

	axis_test_plex_route_full(fx->github, path, 200, "text/plain", bytes, NULL);
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
	AxisTestReply opened = get_as(fx, cookie, "/");
	g_autofree gchar *token = axis_test_web_csrf_token(opened.body);
	g_autofree gchar *escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	g_autofree gchar *body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");

	axis_test_reply_clear(&opened);
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain == NULL || strstr(plain, needle) == NULL)
		g_error("expected the page to hold \"%s\"", needle);
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? axis_test_web_unbroken(haystack) : NULL;

	if (plain != NULL && strstr(plain, needle) != NULL)
		g_error("expected the page not to hold \"%s\"", needle);
}

static void
assert_q(
	Fixture     *fx,
	const gchar *expected,
	const gchar *sql
){
	g_autofree gchar *value = axis_test_db_query(fx->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static gchar *
sha256_of(const gchar *text)
{
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}

/* "See what Jackett has", as an administrator; the list is then kept. */
static void
ask_for_the_list(Fixture *fx)
{
	AxisTestReply reply = post_as(fx, fx->admin, LIST, "");

	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(reply.message), "Location"), ==,
		PAGE "?done=listed");
	axis_test_reply_clear(&reply);
}

/* A fetch of the form's ticks; the page that says what happened. */
static AxisTestReply
fetch(
	Fixture     *fx,
	const gchar *form
){
	return post_as(fx, fx->admin, FETCH, form);
}

/* ── Tests ──────────────────────────────────────────────────────── */

/* Nothing is asked of GitHub at start or by opening a page, and nothing in
 * the daemon can point the fetch anywhere else. */
static void
scan_for_the_allowance(const gchar *dir)
{
	g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
	const gchar *name;

	g_assert_nonnull(handle);
	while ((name = g_dir_read_name(handle)) != NULL) {
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		g_autofree gchar *text = NULL;

		if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
			scan_for_the_allowance(path);
			continue;
		}
		if (!g_str_has_suffix(name, ".c") && !g_str_has_suffix(name, ".h"))
			continue;
		g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
		if (strstr(text, "axis_indexers_jackett_set_testing") != NULL &&
		    strcmp(path, "src/indexer/axis-indexer.h") != 0 && strcmp(path, "src/indexer/axis-indexer-jackett.c") != 0)
			g_error("%s names the tests' allowance for the stand-in for GitHub; nothing in the daemon may", path);
	}
}

static void
test_nothing_at_start(Fixture *fx, gconstpointer data)
{
	static const Listed files[] = { { "madeupsite.yml", "id: madeupsite\n" } };
	AxisTestReply reply;

	github_head(fx, COMMIT_A, files, 1);
	reply = get_as(fx, fx->admin, "/indexers");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Fetch definitions from Jackett");
	assert_contains(reply.body, "See what Jackett has");
	/* The licence paragraph stays, with what is now true and the link. */
	assert_contains(reply.body, "That repository names no licence in its files; most of its definitions come from "
		"Jackett, which is published under the GNU GPL version 2. Install only definitions you have the right to "
		"use.");
	assert_lacks(reply.body, "Axis does not fetch them for you in this version");
	assert_contains(reply.body, "GNU General Public License, version 2 (GPL-2.0)");
	assert_contains(reply.body, "https://www.gnu.org/licenses/old-licenses/gpl-2.0.html");
	assert_lacks(reply.body, "Check Jackett for newer versions");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, PAGE);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis has not asked Jackett&#39;s repository yet.");
	assert_contains(reply.body, "GNU General Public License, version 2 (GPL-2.0)");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 0);
	assert_q(fx, "0", "SELECT count(*) FROM definition_listing");
	scan_for_the_allowance("src");
}

/* The list: two requests, the commit and the folder at that commit; kept
 * with the commit; searchable; names of another form left out. */
static void
test_listing(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = definition("madeupsite", NULL);
	g_autofree gchar *two = definition("othersite", NULL);
	g_autofree gchar *transcript = NULL;
	Listed files[2];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = one;
	files[1].name = "othersite.yml";
	files[1].body = two;
	github_head(fx, COMMIT_A, files, 2);
	ask_for_the_list(fx);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 2);
	g_assert_cmpuint(axis_test_plex_count(fx->github, API_COMMIT), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->github, API_LISTING), ==, 1);
	g_assert_false(axis_test_plex_saw_non_get(fx->github));
	transcript = axis_test_plex_transcript(fx->github);
	g_assert_nonnull(strstr(transcript, API_LISTING "?ref=" COMMIT_A));
	g_assert_null(strstr(transcript, "Cookie"));
	g_assert_null(strstr(transcript, "Authorization"));
	assert_q(fx, COMMIT_A, "SELECT commit_id FROM definition_listing WHERE origin = 'jackett'");
	assert_q(fx, "2", "SELECT count(*) FROM definition_listing_file");
	assert_q(fx, "3", "SELECT n_left_out FROM definition_listing");
	assert_q(fx, "t", "SELECT listed_at IS NOT NULL FROM definition_listing");

	reply = get_as(fx, fx->admin, PAGE "?done=listed");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis has read the list of Jackett&#39;s definition files. No file was fetched.");
	assert_contains(reply.body, "2 definition files, as Jackett&#39;s repository had them at commit aaaa1111bb,");
	assert_contains(reply.body, "madeupsite.yml");
	assert_contains(reply.body, "othersite.yml");
	assert_contains(reply.body, "name=\"pick-madeupsite.yml\"");
	assert_contains(reply.body, "Not installed");
	assert_contains(reply.body, "3 entries of the folder are not listed");
	assert_lacks(reply.body, "Bad_Name");
	assert_lacks(reply.body, "upward");
	assert_contains(reply.body, "Fetch the ticked definitions");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, PAGE "?q=Other");
	assert_contains(reply.body, "othersite.yml");
	assert_lacks(reply.body, "pick-madeupsite.yml");
	assert_contains(reply.body, "1 of 2 files have that in their name.");
	axis_test_reply_clear(&reply);
	/* Reading the page asked nothing more, and nothing was installed. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 2);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_definition WHERE origin IS NOT NULL");
}

/* GitHub refuses for now: said plainly, and the list kept is the one
 * before; a refusal while files are fetched ends the fetch. */
static void
test_rate_limited(Fixture *fx, gconstpointer data)
{
	static const gchar *const wait[] = { "Retry-After", "600", NULL };
	g_autofree gchar *one = definition("madeupsite", NULL);
	g_autofree gchar *two = definition("othersite", NULL);
	g_autofree gchar *path = raw_path(COMMIT_A, "madeupsite.yml");
	g_autoptr(GBytes) refusal = g_bytes_new_static("{\"message\":\"API rate limit exceeded\"}", 37);
	Listed files[2];
	AxisTestReply reply;

	axis_test_plex_route_full(fx->github, API_COMMIT, 403, "application/json", refusal, wait);
	reply = post_as(fx, fx->admin, LIST, "");
	g_assert_cmpuint(reply.status, ==, 502);
	assert_contains(reply.body, "GitHub refused to give the list of Jackett&#39;s definitions for now (status 403).");
	assert_contains(reply.body, "about 60 requests an hour");
	assert_contains(reply.body, "Try again in about 10 minutes.");
	assert_lacks(reply.body, "rate limit exceeded");
	axis_test_reply_clear(&reply);
	assert_q(fx, "0", "SELECT count(*) FROM definition_listing");
	/* From the Indexers page: the same words, on that page. */
	reply = post_as(fx, fx->admin, LIST, "from=installed");
	g_assert_cmpuint(reply.status, ==, 502);
	assert_contains(reply.body, "GitHub refused to give the list");
	assert_contains(reply.body, "Other torrent sites, through site definitions");
	axis_test_reply_clear(&reply);

	/* A list, then the file host refuses the first file: the second is not
	 * asked for. */
	axis_test_plex_clear_routes(fx->github);
	files[0].name = "madeupsite.yml";
	files[0].body = one;
	files[1].name = "othersite.yml";
	files[1].body = two;
	github_head(fx, COMMIT_A, files, 2);
	ask_for_the_list(fx);
	axis_test_plex_route_full(fx->github, path, 429, "text/plain", NULL, NULL);
	github_file(fx, COMMIT_A, "othersite.yml", two);
	axis_test_plex_reset_requests(fx->github);
	reply = fetch(fx, "pick-madeupsite.yml=yes&pick-othersite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "GitHub refused to give this file for now (status 429).");
	assert_contains(reply.body, "It was not asked for, because GitHub refused the file before it.");
	assert_contains(reply.body, "0 installed or updated, 2 not installed, of 2 asked for.");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 1);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_definition WHERE origin IS NOT NULL");
}

/* GitHub's ordinary refusal has no Retry-After: the time its allowance
 * returns (x-ratelimit-reset) says when to try again. */
static void
test_rate_limit_reset(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *reset = g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / G_USEC_PER_SEC + 1800);
	g_autofree gchar *far = g_strdup_printf("%" G_GINT64_FORMAT, g_get_real_time() / G_USEC_PER_SEC + 900000000);
	const gchar *const out[] = { "x-ratelimit-limit", "60", "x-ratelimit-remaining", "0", "x-ratelimit-reset", reset,
		NULL };
	const gchar *const left[] = { "x-ratelimit-remaining", "7", "x-ratelimit-reset", reset, NULL };
	const gchar *const distant[] = { "x-ratelimit-remaining", "0", "x-ratelimit-reset", far, NULL };
	g_autoptr(GBytes) refusal = g_bytes_new_static("{\"message\":\"API rate limit exceeded\"}", 37);
	AxisTestReply reply;

	axis_test_plex_route_full(fx->github, API_COMMIT, 403, "application/json", refusal, out);
	reply = post_as(fx, fx->admin, LIST, "");
	g_assert_cmpuint(reply.status, ==, 502);
	assert_contains(reply.body, "Try again in about 30 minutes.");
	axis_test_reply_clear(&reply);
	/* Allowance left: the reset time is not what this refusal is about. */
	axis_test_plex_clear_routes(fx->github);
	axis_test_plex_route_full(fx->github, API_COMMIT, 403, "application/json", refusal, left);
	reply = post_as(fx, fx->admin, LIST, "");
	assert_contains(reply.body, "Try again in an hour.");
	axis_test_reply_clear(&reply);
	/* A time far away is bounded to a day. */
	axis_test_plex_clear_routes(fx->github);
	axis_test_plex_route_full(fx->github, API_COMMIT, 403, "application/json", refusal, distant);
	reply = post_as(fx, fx->admin, LIST, "");
	assert_contains(reply.body, "Try again in about 24 hours.");
	axis_test_reply_clear(&reply);
}

/* A file is fetched at the commit that was listed, whatever the branch is
 * at by then; bytes that are not the listed file's are refused; nothing is
 * switched on by installing. */
static void
test_fetch_at_listed_commit(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *v1 = definition("madeupsite", NULL);
	g_autofree gchar *v2 = definition("madeupsite", "# a later version\n");
	g_autofree gchar *other = definition("othersite", NULL);
	g_autofree gchar *other_changed = definition("othersite", "# changed where it was\n");
	g_autofree gchar *sha_v1 = sha256_of(v1);
	g_autofree gchar *blob_v1 = axis_jackett_blob_id(v1, strlen(v1));
	g_autofree gchar *path_a = raw_path(COMMIT_A, "madeupsite.yml");
	g_autofree gchar *path_b = raw_path(COMMIT_B, "madeupsite.yml");
	g_autofree gchar *sql = NULL;
	Listed files[2];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = v1;
	files[1].name = "othersite.yml";
	files[1].body = other;
	github_head(fx, COMMIT_A, files, 2);
	ask_for_the_list(fx);

	/* The branch moves on, and one file is changed where it was listed. */
	axis_test_plex_clear_routes(fx->github);
	files[0].body = v2;
	github_head(fx, COMMIT_B, files, 2);
	github_file(fx, COMMIT_A, "madeupsite.yml", v1);
	github_file(fx, COMMIT_B, "madeupsite.yml", v2);
	github_file(fx, COMMIT_A, "othersite.yml", other_changed);
	axis_test_plex_reset_requests(fx->github);

	reply = fetch(fx, "pick-madeupsite.yml=yes&pick-othersite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "What the fetch did");
	assert_contains(reply.body, "Installed. Nothing searches it yet: add an indexer from its page.");
	assert_contains(reply.body, "What GitHub sent is not the file that was listed");
	assert_contains(reply.body, "1 installed or updated, 1 not installed, of 2 asked for.");
	assert_contains(reply.body, "Installed: the same file");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	/* Only the file host was asked, at the listed commit. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 2);
	g_assert_cmpuint(axis_test_plex_count(fx->github, path_a), ==, 1);
	g_assert_cmpuint(axis_test_plex_count(fx->github, path_b), ==, 0);
	g_assert_cmpuint(axis_test_plex_count(fx->github, API_COMMIT), ==, 0);
	g_assert_false(axis_test_plex_saw_non_get(fx->github));

	assert_q(fx, sha_v1, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, "jackett", "SELECT origin FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, "madeupsite.yml", "SELECT origin_file FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, COMMIT_A, "SELECT origin_commit FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, blob_v1, "SELECT origin_blob FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, sha_v1, "SELECT origin_sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, "t", "SELECT origin_at IS NOT NULL AND previous_body IS NULL FROM indexer_definition"
		" WHERE def_id = 'madeupsite'");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_definition WHERE def_id = 'othersite'");
	/* Installed, not switched on: no indexer, nothing searched. */
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source");

	/* Its page says where it came from; the form that adds an indexer is the
	 * separate step it was. */
	sql = g_strdup("SELECT '/indexers/definitions/' || id FROM indexer_definition WHERE def_id = 'madeupsite'");
	{
		g_autofree gchar *where = axis_test_db_query(fx->db, sql);

		reply = get_as(fx, fx->admin, where);
	}
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Fetched from Jackett (commit aaaa1111bb)");
	assert_contains(reply.body, "madeupsite.yml at commit " COMMIT_A);
	assert_contains(reply.body, "Add the indexer");
	assert_lacks(reply.body, "Put the earlier version back");
	axis_test_reply_clear(&reply);

	/* The same file again: nothing changes. */
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	assert_contains(reply.body, "Already installed: the same file. Nothing changed.");
	axis_test_reply_clear(&reply);
	assert_q(fx, "t", "SELECT previous_body IS NULL FROM indexer_definition WHERE def_id = 'madeupsite'");
}

/* A name is outside input: only one form is taken, never a path, and only
 * a name of the list kept. */
static void
test_bad_names(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = definition("madeupsite", NULL);
	Listed files[1];
	AxisTestReply reply;
	GString *many = g_string_new(NULL);
	guint i;

	/* Before any list: refused whole. */
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Axis has no list of Jackett&#39;s definitions yet");
	axis_test_reply_clear(&reply);

	files[0].name = "madeupsite.yml";
	files[0].body = one;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", one);
	ask_for_the_list(fx);
	axis_test_plex_reset_requests(fx->github);

	reply = fetch(fx, "pick-..%2Fmadeupsite.yml=yes&pick-Madeupsite.yml=yes&pick-madeupsite.yml%2F..%2Fx.yml=yes"
		"&pick-madeupsite.yaml=yes&pick-.yml=yes&pick--dash.yml=yes&pick-made%20up.yml=yes"
		"&pick-%2Fetc%2Fpasswd=yes&pick-madeupsite.yml%3Fref%3Dx=yes&pick-ghostsite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "(a name Axis does not take)");
	assert_contains(reply.body, "It is not in the list read from Jackett.");
	assert_contains(reply.body, "0 installed or updated, 10 not installed, of 10 asked for.");
	assert_lacks(reply.body, "passwd");
	assert_lacks(reply.body, "Madeupsite.yml");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 0);

	/* Nothing ticked, and too many. */
	reply = fetch(fx, "");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Tick at least one definition to fetch.");
	axis_test_reply_clear(&reply);
	for (i = 0; i < AXIS_INDEXER_JACKETT_MAX_PER_FETCH + 1; i++)
		g_string_append_printf(many, "%spick-site%u.yml=yes", i > 0 ? "&" : "", i);
	reply = fetch(fx, many->str);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "At most 50 definitions are fetched at once");
	axis_test_reply_clear(&reply);
	g_string_free(many, TRUE);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 0);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_definition WHERE origin IS NOT NULL");
}

/* A fetched file is judged as one installed by hand: what is not a
 * definition is not installed, with the loader's reason. */
static void
test_invalid_definition(Fixture *fx, gconstpointer data)
{
	static const gchar broken[] = "a: &x 1\n";
	g_autofree gchar *wrong_id = definition("madeupsite", NULL);
	g_autofree gchar *reserved = definition("axis-limetorrents", NULL);
	g_autofree gchar *login = definition("needslogin", "login:\n  method: form\n  path: /login\n");
	g_autofree gchar *large = g_strnfill(300 * 1024, '#');
	Listed files[5];
	AxisTestReply reply;

	files[0].name = "brokensite.yml";
	files[0].body = broken;
	files[1].name = "othername.yml";
	files[1].body = wrong_id;
	files[2].name = "axis-limetorrents.yml";
	files[2].body = reserved;
	files[3].name = "needslogin.yml";
	files[3].body = login;
	files[4].name = "largesite.yml";
	files[4].body = large;
	github_head(fx, COMMIT_A, files, 5);
	github_file(fx, COMMIT_A, "brokensite.yml", broken);
	github_file(fx, COMMIT_A, "othername.yml", wrong_id);
	github_file(fx, COMMIT_A, "axis-limetorrents.yml", reserved);
	github_file(fx, COMMIT_A, "needslogin.yml", login);
	github_file(fx, COMMIT_A, "largesite.yml", large);
	ask_for_the_list(fx);
	axis_test_plex_reset_requests(fx->github);

	reply = fetch(fx, "pick-brokensite.yml=yes&pick-othername.yml=yes&pick-axis-limetorrents.yml=yes"
		"&pick-needslogin.yml=yes&pick-largesite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Not installed. That file was not installed.");
	assert_contains(reply.body, "anchors are not supported");
	assert_contains(reply.body, "the definition in it has the id madeupsite, not its file&#39;s name");
	assert_contains(reply.body, "are Axis&#39;s own built-in definitions");
	assert_contains(reply.body, "It is larger than a definition file may be (256 KiB), and was not asked for.");
	assert_contains(reply.body, "1 installed or updated, 4 not installed, of 5 asked for.");
	axis_test_reply_clear(&reply);
	/* The one too large was not asked for. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 4);
	assert_q(fx, "1", "SELECT count(*) FROM indexer_definition WHERE origin IS NOT NULL");
	assert_q(fx, "0", "SELECT count(*) FROM indexer_definition WHERE def_id IN ('brokensite', 'othername', "
		"'madeupsite', 'largesite') OR def_id LIKE '%override'");
	/* A definition Axis cannot run yet is installed, and says what it needs:
	 * the same as for a file installed by hand. */
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, "Needs a login (stage 2).");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
}

/* The path of a definition's page, by its id. */
static gchar *
page_of(
	Fixture     *fx,
	const gchar *def_id
){
	g_autofree gchar *sql = g_strdup_printf("SELECT '/indexers/definitions/' || id FROM indexer_definition"
		" WHERE def_id = '%s'", def_id);
	gchar *path = axis_test_db_query(fx->db, sql);

	g_assert_nonnull(path);
	return path;
}

/* Installed and differs; "Check Jackett for newer versions"; Update; "Put
 * the earlier version back". */
static void
test_update_and_put_back(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *v1 = definition("madeupsite", NULL);
	g_autofree gchar *v2 = definition("madeupsite", "# a later version\n");
	g_autofree gchar *sha_v1 = sha256_of(v1);
	g_autofree gchar *sha_v2 = sha256_of(v2);
	g_autofree gchar *where = NULL;
	g_autofree gchar *back = NULL;
	Listed files[1];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = v1;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", v1);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	where = page_of(fx, "madeupsite");
	back = g_strconcat(where, "/put-back", NULL);

	/* Nothing newer yet: the list of installed ones offers the check. */
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, "Fetched from Jackett (commit aaaa1111bb)");
	assert_contains(reply.body, "Check Jackett for newer versions");
	assert_contains(reply.body, "Update the ticked definitions");
	assert_contains(reply.body, "form=\"jackett-update\"");
	assert_lacks(reply.body, "Newer at Jackett");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);

	/* Jackett's file changes; nothing here follows by itself. */
	axis_test_plex_clear_routes(fx->github);
	files[0].body = v2;
	github_head(fx, COMMIT_B, files, 1);
	github_file(fx, COMMIT_B, "madeupsite.yml", v2);
	axis_test_plex_reset_requests(fx->github);
	reply = get_as(fx, fx->admin, "/indexers");
	assert_lacks(reply.body, "Newer at Jackett");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 0);
	assert_q(fx, sha_v1, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");

	/* The check: the same asking, back at the installed list. */
	reply = post_as(fx, fx->admin, LIST, "from=installed");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(soup_message_headers_get_one(soup_message_get_response_headers(reply.message), "Location"), ==,
		"/indexers?done=jackett-checked&at=definitions#definitions");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 2);
	reply = get_as(fx, fx->admin, "/indexers?done=jackett-checked&at=definitions");
	assert_contains(reply.body, "A definition whose file changed there is marked Newer at Jackett; nothing was "
		"updated.");
	assert_contains(reply.body, "badge-warn\">Newer at Jackett");
	assert_contains(reply.body, "name=\"only\"");
	assert_contains(reply.body, "value=\"madeupsite.yml\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, PAGE);
	assert_contains(reply.body, "Installed, and differs: Jackett&#39;s file has changed since yours was fetched");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v1, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");

	/* Update: the row's button. */
	reply = post_as(fx, fx->admin, FETCH, "from=installed&only=madeupsite.yml");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Updated. The version it replaced is kept: its page can put it back.");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v2, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, COMMIT_B, "SELECT origin_commit FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, sha_v1, "SELECT encode(sha256(convert_to(previous_body, 'UTF8')), 'hex') FROM indexer_definition"
		" WHERE def_id = 'madeupsite'");
	assert_q(fx, COMMIT_A, "SELECT previous_commit FROM indexer_definition WHERE def_id = 'madeupsite'");
	reply = get_as(fx, fx->admin, "/indexers");
	assert_lacks(reply.body, "Newer at Jackett");
	axis_test_reply_clear(&reply);

	/* Put the earlier version back: once. */
	reply = get_as(fx, fx->admin, where);
	assert_contains(reply.body, "Put the earlier version back");
	assert_contains(reply.body, "The version fetched from Jackett on ");
	assert_contains(reply.body, " is kept.");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, back, "");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v2, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	reply = post_as(fx, fx->admin, back, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v1, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, COMMIT_A, "SELECT origin_commit FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, sha_v1, "SELECT origin_sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, "t", "SELECT previous_body IS NULL AND previous_commit IS NULL FROM indexer_definition"
		" WHERE def_id = 'madeupsite'");
	{
		g_autofree gchar *done = g_strconcat(where, "?done=put-back", NULL);

		reply = get_as(fx, fx->admin, done);
	}
	assert_contains(reply.body, "The earlier version is in use again");
	assert_lacks(reply.body, "Put the earlier version back");
	assert_contains(reply.body, "Jackett&#39;s file for this definition has changed since it was fetched.");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, back, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "No earlier version of that definition is kept.");
	axis_test_reply_clear(&reply);
}

/* A definition changed by hand since it was fetched, and one installed by
 * hand, are not replaced without the tick that says so. */
static void
test_changed_by_hand(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *v1 = definition("madeupsite", NULL);
	g_autofree gchar *v2 = definition("madeupsite", "# a later version\n");
	g_autofree gchar *mine = definition("madeupsite", "# my own change\n");
	g_autofree gchar *by_hand = definition("othersite", "# installed by hand\n");
	g_autofree gchar *other = definition("othersite", NULL);
	g_autofree gchar *sha_v2 = sha256_of(v2);
	g_autofree gchar *sha_mine = sha256_of(mine);
	g_autofree gchar *sha_by_hand = sha256_of(by_hand);
	g_autofree gchar *where = NULL;
	g_autofree gchar *back = NULL;
	g_autoptr(GError) error = NULL;
	AxisIndexerDefinitionInstalled installed;
	Listed files[2];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = v1;
	files[1].name = "othersite.yml";
	files[1].body = other;
	github_head(fx, COMMIT_A, files, 2);
	github_file(fx, COMMIT_A, "madeupsite.yml", v1);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	axis_test_reply_clear(&reply);
	/* A file installed over the fetched one, and one never fetched. */
	g_assert_true(axis_indexers_install_definition(fx->indexers, mine, strlen(mine), &installed, &error));
	g_assert_true(axis_indexers_install_definition(fx->indexers, by_hand, strlen(by_hand), &installed, &error));
	g_assert_no_error(error);
	where = page_of(fx, "madeupsite");
	back = g_strconcat(where, "/put-back", NULL);
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, "Fetched from Jackett (commit aaaa1111bb), changed by hand since");
	assert_contains(reply.body, "badge-warn\">Changed by hand");
	assert_contains(reply.body, "Installed by hand");
	axis_test_reply_clear(&reply);

	axis_test_plex_clear_routes(fx->github);
	files[0].body = v2;
	github_head(fx, COMMIT_B, files, 2);
	github_file(fx, COMMIT_B, "madeupsite.yml", v2);
	github_file(fx, COMMIT_B, "othersite.yml", other);
	ask_for_the_list(fx);
	reply = get_as(fx, fx->admin, PAGE);
	assert_contains(reply.body, "Installed, and differs: yours was installed or changed by hand");
	axis_test_reply_clear(&reply);

	/* Without the tick: both left as they are, saying why. */
	reply = fetch(fx, "pick-madeupsite.yml=yes&pick-othersite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The definition installed was installed or changed by hand on this Axis, and was "
		"left as it is.");
	assert_contains(reply.body, "0 installed or updated, 2 not installed, of 2 asked for.");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_mine, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, sha_by_hand, "SELECT sha256 FROM indexer_definition WHERE def_id = 'othersite'");
	assert_q(fx, "t", "SELECT origin IS NULL FROM indexer_definition WHERE def_id = 'othersite'");

	/* With it: replaced, and what was there is kept to put back. */
	reply = fetch(fx, "pick-madeupsite.yml=yes&pick-othersite.yml=yes&replace=yes");
	assert_contains(reply.body, "2 installed or updated, 0 not installed, of 2 asked for.");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v2, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, "jackett", "SELECT origin FROM indexer_definition WHERE def_id = 'othersite'");
	/* The earlier version kept is the one changed by hand, and says so. */
	reply = get_as(fx, fx->admin, where);
	assert_contains(reply.body, "The version installed or changed by hand before is kept.");
	assert_lacks(reply.body, "The version this one replaced is kept");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, back, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	/* Back: the file changed by hand, known as that again. */
	assert_q(fx, sha_mine, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	assert_q(fx, "t", "SELECT origin = 'jackett' AND sha256 <> origin_sha256 AND origin_commit = '" COMMIT_A "'"
		" FROM indexer_definition WHERE def_id = 'madeupsite'");
	{
		g_autofree gchar *other_page = page_of(fx, "othersite");
		g_autofree gchar *other_back = g_strconcat(other_page, "/put-back", NULL);

		reply = post_as(fx, fx->admin, other_back, "confirm=yes");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
	/* One installed by hand is that again: no origin at all. */
	assert_q(fx, sha_by_hand, "SELECT sha256 FROM indexer_definition WHERE def_id = 'othersite'");
	assert_q(fx, "t", "SELECT origin IS NULL AND origin_file IS NULL AND origin_commit IS NULL"
		" FROM indexer_definition WHERE def_id = 'othersite'");
}

/* A database error on one file does not lose what the files before it did:
 * the page and the outcomes say what was installed, and the files after the
 * one that failed are not asked for. */
static void
test_database_error_midway(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = definition("madeupsite", NULL);
	g_autofree gchar *two = definition("othersite", NULL);
	g_autofree gchar *three = definition("thirdsite", NULL);
	Listed files[3];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = one;
	files[1].name = "othersite.yml";
	files[1].body = two;
	files[2].name = "thirdsite.yml";
	files[2].body = three;
	github_head(fx, COMMIT_A, files, 3);
	github_file(fx, COMMIT_A, "madeupsite.yml", one);
	github_file(fx, COMMIT_A, "othersite.yml", two);
	github_file(fx, COMMIT_A, "thirdsite.yml", three);
	ask_for_the_list(fx);
	axis_test_db_exec(fx->db, "CREATE FUNCTION refuse_made_up() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN"
		" RAISE EXCEPTION 'made-up failure'; END $$");
	axis_test_db_exec(fx->db, "CREATE TRIGGER refuse_made_up BEFORE INSERT ON indexer_definition FOR EACH ROW"
		" WHEN (NEW.def_id = 'othersite') EXECUTE FUNCTION refuse_made_up()");
	axis_test_plex_reset_requests(fx->github);

	reply = fetch(fx, "pick-madeupsite.yml=yes&pick-othersite.yml=yes&pick-thirdsite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Installed. Nothing searches it yet");
	assert_contains(reply.body, "Axis could not store it: the database gave an error.");
	assert_contains(reply.body, "It was not asked for, because the database gave an error on the file before it.");
	assert_contains(reply.body, "1 installed or updated, 2 not installed, of 3 asked for.");
	assert_lacks(reply.body, "made-up failure");
	axis_test_reply_clear(&reply);
	/* The first is installed; the failed one and the one after are not asked
	 * for or stored. */
	assert_q(fx, "madeupsite", "SELECT def_id FROM indexer_definition WHERE origin = 'jackett'");
	assert_q(fx, "1", "SELECT count(*) FROM indexer_definition WHERE origin = 'jackett'");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 2);
}

/* A row whose origin is 'jackett' always says which file, commit and
 * content: a NULL is not let through by the constraints. */
static void
test_origin_constraints(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *one = definition("madeupsite", NULL);
	Listed files[1];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = one;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", one);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	axis_test_reply_clear(&reply);
	assert_q(fx, "1", "SELECT count(*) FROM indexer_definition WHERE origin = 'jackett'");
	/* Each column in turn: the row is refused. */
	{
		static const gchar *const columns[] = { "origin_file", "origin_commit", "origin_blob", "origin_sha256", NULL };
		guint i;

		for (i = 0; columns[i] != NULL; i++) {
			g_autofree gchar *sql = g_strdup_printf("DO $$ BEGIN UPDATE indexer_definition SET %s = NULL WHERE"
				" def_id = 'madeupsite'; RAISE EXCEPTION 'the check let a NULL through'; EXCEPTION WHEN check_violation"
				" THEN NULL; END $$", columns[i]);

			axis_test_db_exec(fx->db, sql);
		}
	}
	/* And the kept earlier file: a NULL there is refused too. */
	axis_test_db_exec(fx->db, "DO $$ BEGIN UPDATE indexer_definition SET previous_body = 'x', previous_name = 'x',"
		" previous_origin = 'jackett', previous_commit = NULL, previous_blob = '" COMMIT_A "', previous_sha256 = '"
		"0000000000000000000000000000000000000000000000000000000000000000', previous_at = now() WHERE def_id ="
		" 'madeupsite'; RAISE EXCEPTION 'the check let a NULL through'; EXCEPTION WHEN check_violation THEN NULL;"
		" END $$");
	axis_test_db_exec(fx->db, "DO $$ BEGIN UPDATE indexer_definition SET previous_body = NULL, previous_name = 'x',"
		" previous_origin = NULL, previous_commit = NULL, previous_blob = NULL, previous_sha256 = NULL,"
		" previous_at = NULL WHERE def_id = 'madeupsite'; RAISE EXCEPTION 'the check let a NULL through'; EXCEPTION"
		" WHEN check_violation THEN NULL; END $$");
}

/* A definition that indexers use: the listing and the outcome say how many,
 * and a file Axis cannot run does not replace it without the tick. */
static void
test_update_of_a_definition_in_use(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *v1 = definition("madeupsite", NULL);
	g_autofree gchar *v2 = definition("madeupsite", "# a later version\n");
	g_autofree gchar *v3 = definition("madeupsite", "login:\n  method: form\n  path: /login\n");
	g_autofree gchar *sha_v2 = sha256_of(v2);
	g_autofree gchar *sha_v3 = sha256_of(v3);
	Listed files[1];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = v1;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", v1);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->db, "INSERT INTO indexer_source (name, kind, address, protocol, definition_id)"
		" VALUES ('my copy', 'cardigann', 'http://127.0.0.1:1', 'torrent',"
		" (SELECT id FROM indexer_definition WHERE def_id = 'madeupsite'))");
	reply = get_as(fx, fx->admin, PAGE);
	assert_contains(reply.body, "1 indexer uses it.");
	axis_test_reply_clear(&reply);

	/* A file Axis can run: updated, and said to be used by one indexer. */
	axis_test_plex_clear_routes(fx->github);
	files[0].body = v2;
	github_head(fx, COMMIT_B, files, 1);
	github_file(fx, COMMIT_B, "madeupsite.yml", v2);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	assert_contains(reply.body, "Updated. The version it replaced is kept");
	assert_contains(reply.body, "1 indexer uses it. It runs this version from its next search.");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v2, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");

	/* A file Axis cannot run: refused, saying what happens, until the tick. */
	axis_test_plex_clear_routes(fx->github);
	files[0].body = v3;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", v3);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	assert_contains(reply.body, "1 indexer uses this definition, and Axis cannot run Jackett&#39;s file for it");
	assert_contains(reply.body, "would not search until the earlier version is put back");
	assert_contains(reply.body, "0 installed or updated, 1 not installed, of 1 asked for.");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v2, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
	reply = fetch(fx, "pick-madeupsite.yml=yes&replace=yes");
	assert_contains(reply.body, "1 indexer uses it. Axis cannot run this version, so that indexer will not search "
		"until the earlier version is put back.");
	axis_test_reply_clear(&reply);
	assert_q(fx, sha_v3, "SELECT sha256 FROM indexer_definition WHERE def_id = 'madeupsite'");
}

/* A file installed by hand with Jackett's content is adopted as fetched
 * without the tick, and the outcome says so. */
static void
test_the_same_as_by_hand(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *v1 = definition("madeupsite", NULL);
	g_autoptr(GError) error = NULL;
	AxisIndexerDefinitionInstalled installed;
	Listed files[1];
	AxisTestReply reply;

	g_assert_true(axis_indexers_install_definition(fx->indexers, v1, strlen(v1), &installed, &error));
	g_assert_no_error(error);
	files[0].name = "madeupsite.yml";
	files[0].body = v1;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", v1);
	ask_for_the_list(fx);
	reply = get_as(fx, fx->admin, PAGE);
	assert_contains(reply.body, "Installed: the same file");
	axis_test_reply_clear(&reply);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	assert_contains(reply.body, "The same as the one you installed by hand; now tracked as fetched from Jackett.");
	axis_test_reply_clear(&reply);
	assert_q(fx, "jackett", "SELECT origin FROM indexer_definition WHERE def_id = 'madeupsite'");
	/* A second time it is the plain "already installed". */
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	assert_contains(reply.body, "Already installed: the same file. Nothing changed.");
	axis_test_reply_clear(&reply);
}

/* A file removed at Jackett is shown as that, once a list that lacks it was
 * read. */
static void
test_no_longer_at_jackett(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *v1 = definition("madeupsite", NULL);
	g_autofree gchar *other = definition("othersite", NULL);
	Listed files[1];
	AxisTestReply reply;

	files[0].name = "madeupsite.yml";
	files[0].body = v1;
	github_head(fx, COMMIT_A, files, 1);
	github_file(fx, COMMIT_A, "madeupsite.yml", v1);
	ask_for_the_list(fx);
	reply = fetch(fx, "pick-madeupsite.yml=yes");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/indexers");
	assert_lacks(reply.body, "No longer at Jackett");
	axis_test_reply_clear(&reply);

	axis_test_plex_clear_routes(fx->github);
	files[0].name = "othersite.yml";
	files[0].body = other;
	github_head(fx, COMMIT_B, files, 1);
	ask_for_the_list(fx);
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, "badge-warn\">No longer at Jackett");
	assert_lacks(reply.body, "Newer at Jackett");
	axis_test_reply_clear(&reply);
}

/* Administrators only, and a post without the form's token is refused:
 * nothing is asked of GitHub either way. */
static void
test_only_administrators(Fixture *fx, gconstpointer data)
{
	static const Listed files[] = { { "madeupsite.yml", "id: madeupsite\n" } };
	AxisTestReply reply;

	github_head(fx, COMMIT_A, files, 1);
	reply = get_as(fx, fx->viewer, PAGE);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, LIST, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, FETCH, "pick-madeupsite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, "/indexers/definitions/1/put-back", "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* An administrator's post without the token. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", LIST, fx->admin, NULL, "from=installed");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = axis_test_web_request(fx->session, fx->port, "POST", FETCH, fx->admin, NULL, "pick-madeupsite.yml=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Not logged in at all. */
	reply = axis_test_web_request(fx->session, fx->port, "GET", PAGE, NULL, NULL, NULL);
	g_assert_cmpuint(reply.status, !=, 200);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(fx->github), ==, 0);
	assert_q(fx, "0", "SELECT count(*) FROM definition_listing");
}

/* ── Reading GitHub's answers (no database) ─────────────────────── */

static void
test_names(void)
{
	static const gchar *const good[] = { "madeupsite.yml", "0day.yml", "a.yml", "made-up-site-2.yml", NULL };
	static const gchar *const bad[] = {
		"", ".yml", "-x.yml", "Made.yml", "made_up.yml", "made.up.yml", "made up.yml", "madeupsite.yaml",
		"madeupsite.yml ", "../madeupsite.yml", "a/b.yml", "a\\b.yml", "madeupsite.yml/..", "madeupsite", "x.YML",
		"mad\xc3\xa9.yml", "a%2f.yml", NULL
	};
	g_autofree gchar *longest = g_strnfill(AXIS_JACKETT_MAX_STEM_BYTES, 'a');
	g_autofree gchar *fits = g_strconcat(longest, ".yml", NULL);
	g_autofree gchar *too_long = g_strconcat(longest, "a.yml", NULL);
	guint i;

	for (i = 0; good[i] != NULL; i++)
		g_assert_true(axis_jackett_name_valid(good[i]));
	for (i = 0; bad[i] != NULL; i++)
		g_assert_false(axis_jackett_name_valid(bad[i]));
	g_assert_false(axis_jackett_name_valid(NULL));
	g_assert_true(axis_jackett_name_valid(fits));
	g_assert_false(axis_jackett_name_valid(too_long));
}

static void
test_answers(void)
{
	static const gchar listing[] =
		"[{\"name\":\"b.yml\",\"sha\":\"" COMMIT_A "\",\"size\":12,\"type\":\"file\"},"
		"{\"name\":\"a.yml\",\"sha\":\"" COMMIT_B "\",\"size\":7,\"type\":\"file\"},"
		"{\"name\":\"a.yml\",\"sha\":\"" COMMIT_B "\",\"size\":7,\"type\":\"file\"},"
		"{\"name\":\"c.yml\",\"sha\":\"short\",\"size\":7,\"type\":\"file\"},"
		"{\"name\":\"d.yml\",\"sha\":\"" COMMIT_B "\",\"size\":\"7\",\"type\":\"file\"},"
		"{\"name\":\"e.yml\",\"sha\":\"" COMMIT_B "\",\"size\":7,\"type\":\"symlink\"},"
		"7, null]";
	g_autoptr(GPtrArray) files = NULL;
	g_autofree gchar *problem = NULL;
	g_autofree gchar *commit = axis_jackett_parse_commit(COMMIT_A "\n", 41);
	g_autofree gchar *blob = axis_jackett_blob_id("made up\n", 8);
	g_autofree gchar *expected = NULL;
	GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA1);
	const AxisJackettFile *first;
	guint left_out = 0;
	gboolean cut = TRUE;

	g_assert_cmpstr(commit, ==, COMMIT_A);
	g_assert_null(axis_jackett_parse_commit(COMMIT_A "0", 41));
	g_assert_null(axis_jackett_parse_commit("AAAA1111BBBB2222CCCC3333DDDD4444EEEE5555", 40));
	g_assert_null(axis_jackett_parse_commit("{\"sha\":\"" COMMIT_A "\"}", 50));
	g_assert_null(axis_jackett_parse_commit(NULL, 0));

	g_assert_true(axis_jackett_parse_listing(listing, strlen(listing), &files, &left_out, &cut, &problem));
	g_assert_null(problem);
	g_assert_cmpuint(files->len, ==, 2);
	g_assert_cmpuint(left_out, ==, 6);
	g_assert_false(cut);
	first = g_ptr_array_index(files, 0);
	g_assert_cmpstr(first->name, ==, "a.yml");
	g_assert_cmpstr(first->blob, ==, COMMIT_B);
	g_assert_cmpint(first->size_bytes, ==, 7);
	g_clear_pointer(&files, g_ptr_array_unref);
	g_assert_false(axis_jackett_parse_listing("{\"message\":\"Not Found\"}", 23, &files, &left_out, &cut, &problem));
	g_assert_nonnull(problem);
	g_assert_null(files);

	/* The git blob id, made here the long way. */
	g_checksum_update(sum, (const guchar *)"blob 8\0made up\n", 15);
	expected = g_strdup(g_checksum_get_string(sum));
	g_checksum_free(sum);
	g_assert_cmpstr(blob, ==, expected);
	g_assert_true(axis_jackett_id_valid(blob));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web/jackett/names", test_names);
	g_test_add_func("/web/jackett/answers", test_answers);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web/jackett/" path, Fixture, NULL, func)
	ADD("nothing-at-start", test_nothing_at_start);
	ADD("listing", test_listing);
	ADD("rate-limited", test_rate_limited);
	ADD("fetch-at-listed-commit", test_fetch_at_listed_commit);
	ADD("bad-names", test_bad_names);
	ADD("invalid-definition", test_invalid_definition);
	ADD("update-and-put-back", test_update_and_put_back);
	ADD("changed-by-hand", test_changed_by_hand);
	ADD("rate-limit-reset", test_rate_limit_reset);
	ADD("database-error-midway", test_database_error_midway);
	ADD("origin-constraints", test_origin_constraints);
	ADD("update-of-a-definition-in-use", test_update_of_a_definition_in_use);
	ADD("the-same-as-by-hand", test_the_same_as_by_hand);
	ADD("no-longer-at-jackett", test_no_longer_at_jackett);
	ADD("only-administrators", test_only_administrators);
#undef ADD
	return g_test_run();
}
