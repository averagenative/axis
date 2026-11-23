/*
 * test-web-definitions.c - the site definitions on the Indexers pages: the
 * list with its licence note, installing a file (a multipart upload, its
 * CSRF field read from it), a definition's page and the form that adds an
 * indexer running it, removing one; with the real shell on an ephemeral
 * loopback port and a real database
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The definitions are the hand-written fixtures of tests/fixtures/
 * definitions.  No site is asked: adding an indexer asks nothing.
 *
 * add-indexer-definitions, task 1.6; spec scenario (indexers): Unsupported
 * feature (shown, naming the feature).
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
#include "indexer/axis-indexer-shipped.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-settings.h"

#define FIXTURES "tests/fixtures/definitions/"
#define BOUNDARY "axis-test-boundary-4f1c"

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
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
} Fixture;

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
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
	axis_test_db_free(fx->db);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

AXIS_TEST_DB_CASES(Fixture, fx->db != NULL)

/* ── Helpers ────────────────────────────────────────────────────── */

static gchar *
fixture(const gchar *name)
{
	g_autofree gchar *path = g_strconcat(FIXTURES, name, NULL);
	gchar *data = NULL;

	g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
	return data;
}

static AxisTestReply
get_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path
){
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

static gchar *
token_of(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *page
){
	AxisTestReply opened = get_as(fx, cookie, page);
	gchar *token = axis_test_web_csrf_token(opened.body);

	axis_test_reply_clear(&opened);
	return token;
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *page,
	const gchar *path,
	const gchar *form
){
	g_autofree gchar *token = token_of(fx, cookie, page);
	g_autofree gchar *escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	g_autofree gchar *body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");

	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} Pending;

static void
on_sent(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Pending *pending = user_data;

	pending->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &pending->error);
	g_main_loop_quit(pending->loop);
}

/* A file uploaded as a page's form does it: multipart, the CSRF field
 * (@token, or none) beside it. */
static guint
upload(
	Fixture      *fx,
	const gchar  *cookie,
	const gchar  *token,
	const gchar  *content,
	gchar       **page
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/indexers/definitions/install", (guint)fx->port);
	g_autoptr(SoupMessage) message = soup_message_new("POST", url);
	GString *body = g_string_new(NULL);
	g_autoptr(GBytes) bytes = NULL;
	Pending pending;
	guint status;

	if (token != NULL)
		g_string_append_printf(body, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"_csrf\"\r\n\r\n%s\r\n",
			token);
	g_string_append(body, "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"definition\"; "
		"filename=\"site.yml\"\r\nContent-Type: application/x-yaml\r\n\r\n");
	g_string_append(body, content);
	g_string_append(body, "\r\n--" BOUNDARY "--\r\n");
	bytes = g_bytes_new(body->str, body->len);
	g_string_free(body, TRUE);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	soup_message_set_request_body_from_bytes(message, "multipart/form-data; boundary=" BOUNDARY, bytes);
	memset(&pending, 0, sizeof(pending));
	pending.loop = g_main_loop_new(NULL, FALSE);
	soup_session_send_and_read_async(fx->session, message, G_PRIORITY_DEFAULT, NULL, on_sent, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);
	g_assert_no_error(pending.error);
	status = soup_message_get_status(message);
	if (page != NULL) {
		const gchar *location = soup_message_headers_get_one(soup_message_get_response_headers(message), "Location");

		*page = status == 303 && location != NULL ? g_strdup(location) :
			g_strndup(g_bytes_get_data(pending.bytes, NULL), g_bytes_get_size(pending.bytes));
	}
	g_bytes_unref(pending.bytes);
	return status;
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

/* ── Tests ──────────────────────────────────────────────────────── */

/* Task 1.6: the list, installing, a definition's page, adding an indexer,
 * removing. */
static void
test_definitions_on_the_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *token = token_of(fx, fx->admin, "/indexers");
	g_autofree gchar *location = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply list = get_as(fx, fx->admin, "/indexers");
	AxisTestReply page;
	AxisTestReply added;
	AxisTestReply after;
	gchar *id_text;
	gint64 id;

	g_assert_cmpuint(list.status, ==, 200);
	assert_contains(list.body, "Other torrent sites, through site definitions");
	/* Only the built-in ones (add-builtin-torrent-indexers). */
	assert_lacks(list.body, "No site definition is installed yet.");
	assert_contains(list.body, "axis-limetorrents, https://www.limetorrents.fun/; Built in, version 1");
	assert_lacks(list.body, "Example HTML");
	assert_contains(list.body, "Prowlarr/Indexers");
	assert_contains(list.body, "enctype=\"multipart/form-data\"");
	assert_contains(list.body, "action=\"/indexers/definitions/install\"");
	axis_test_web_assert_tables(list.body);
	axis_test_reply_clear(&list);

	/* Installed: the page of the definition, usable, with the form. */
	g_assert_cmpuint(upload(fx, fx->admin, token, html, &location), ==, 303);
	g_assert_true(g_str_has_prefix(location, "/indexers/definitions/"));
	g_assert_nonnull(strstr(location, "done=installed"));
	page = get_as(fx, fx->admin, location);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "<h1>Example HTML</h1>");
	assert_contains(page.body, "One of the site definitions installed on this Axis");
	assert_contains(page.body, "The definition has been installed.");
	assert_contains(page.body, "Usable");
	assert_contains(page.body, "https://example-html.invalid/");
	assert_contains(page.body, "https://old.example-html.invalid/");
	assert_contains(page.body, "name=\"definition\"");
	assert_contains(page.body, "Add the indexer");
	axis_test_reply_clear(&page);
	id_text = strrchr(location, '/') + 1;
	id = g_ascii_strtoll(id_text, NULL, 10);
	g_assert_cmpint(id, >, 0);

	/* An indexer from it. */
	form = g_strdup_printf("kind=cardigann&definition=%" G_GINT64_FORMAT "&name=&address=https%%3A%%2F%%2Fexample-"
		"html.invalid%%2F&address_other=&priority=&enabled=yes", id);
	added = post_as(fx, fx->admin, "/indexers", "/indexers", form);
	g_assert_cmpuint(added.status, ==, 303);
	/* Its own page says what it runs, and where. */
	{
		const gchar *where = soup_message_headers_get_one(soup_message_get_response_headers(added.message),
			"Location");
		AxisTestReply source;

		g_assert_nonnull(where);
		source = get_as(fx, fx->admin, where);
		g_assert_cmpuint(source.status, ==, 200);
		assert_contains(source.body, "Site definition");
		assert_contains(source.body, "Example HTML");
		assert_contains(source.body, "https://example-html.invalid:443");
		axis_test_web_assert_tables(source.body);
		axis_test_reply_clear(&source);
	}
	axis_test_reply_clear(&added);
	after = get_as(fx, fx->admin, "/indexers");
	assert_contains(after.body, "Site definition (torrents)");
	assert_contains(after.body, "definition-");
	axis_test_web_assert_tables(after.body);
	axis_test_reply_clear(&after);

	/* What is not a definition is refused, with why; without the CSRF
	 * field, refused before anything. */
	g_assert_cmpuint(upload(fx, fx->admin, token, "a: &x 1\n", &refused), ==, 400);
	assert_contains(refused, "That file was not installed.");
	assert_contains(refused, "anchors are not supported");
	g_clear_pointer(&refused, g_free);
	g_assert_cmpuint(upload(fx, fx->admin, NULL, html, NULL), ==, 403);

	/* Not usable: listed, saying what it needs, and no form to add. */
	{
		g_autofree gchar *login = g_strconcat(html, "login:\n  method: form\n  path: /login\n", NULL);
		g_autofree gchar *where = NULL;
		AxisTestReply one;
		AxisTestReply all;

		g_assert_cmpuint(upload(fx, fx->admin, token, login, &where), ==, 303);
		g_assert_nonnull(strstr(where, "done=replaced"));
		one = get_as(fx, fx->admin, where);
		assert_contains(one.body, "Not usable yet");
		assert_contains(one.body, "Needs a login (stage 2).");
		assert_lacks(one.body, "Add the indexer");
		axis_test_reply_clear(&one);
		all = get_as(fx, fx->admin, "/indexers");
		assert_contains(all.body, "Needs a login (stage 2).");
		axis_test_reply_clear(&all);
	}

	/* Removing while an indexer runs it is refused. */
	{
		g_autofree gchar *path = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT "/remove", id);
		AxisTestReply removed = post_as(fx, fx->admin, "/indexers", path, "confirm=yes");

		g_assert_cmpuint(removed.status, ==, 400);
		assert_contains(removed.body, "remove the indexer first");
		axis_test_reply_clear(&removed);
	}
}

/* Removing an unused definition comes back to the list at the definitions'
 * card, with the notice in it (its row is gone). */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *html = fixture("example-html.yml");
	g_autofree gchar *token = token_of(fx, fx->admin, "/indexers");
	g_autofree gchar *location = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *page_path = NULL;
	const gchar *where;
	const gchar *element;
	const gchar *notice;
	AxisTestReply removed;
	AxisTestReply page;

	g_assert_cmpuint(upload(fx, fx->admin, token, html, &location), ==, 303);
	{
		const gchar *query = strchr(location, '?');

		page_path = g_strndup(location, query != NULL ? (gsize)(query - location) : strlen(location));
	}
	path = g_strdup_printf("%s/remove", page_path);
	removed = post_as(fx, fx->admin, "/indexers", path, "confirm=yes");
	g_assert_cmpuint(removed.status, ==, 303);
	where = soup_message_headers_get_one(soup_message_get_response_headers(removed.message), "Location");
	g_assert_nonnull(where);
	g_assert_nonnull(strstr(where, "at=definitions"));
	g_assert_true(g_str_has_suffix(where, "#definitions"));
	{
		g_autofree gchar *fetch = g_strndup(where, strrchr(where, '#') - where);

		page = get_as(fx, fx->admin, fetch);
	}
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, "id=\"definitions\"");
	notice = strstr(page.body, "notice-here");
	g_assert_nonnull(element);
	g_assert_nonnull(notice);
	g_assert_true(element < notice);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&removed);
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

/* The stored id of the built-in @def_id. */
static gint64
builtin_id(
	Fixture     *fx,
	const gchar *def_id
){
	g_autofree gchar *sql = g_strdup_printf("SELECT id FROM indexer_definition WHERE def_id = '%s'", def_id);
	g_autofree gchar *text = axis_test_db_query(fx->db, sql);

	g_assert_nonnull(text);
	return g_ascii_strtoll(text, NULL, 10);
}

/* The built-in definition's file with its version and @from made @to. */
static gchar *
pasted(
	const gchar *def_id,
	gint         version,
	const gchar *from,
	const gchar *to
){
	const AxisIndexerShipped *shipped = axis_indexer_shipped_find(def_id);
	g_autofree gchar *was = g_strdup_printf("\nversion: %d\n", shipped->version);
	g_autofree gchar *now = g_strdup_printf("\nversion: %d\n", version);
	g_auto(GStrv) parts = g_strsplit(shipped->body, was, -1);
	g_autofree gchar *versioned = g_strjoinv(now, parts);
	g_auto(GStrv) more = g_strsplit(versioned, from, -1);
	g_autofree gchar *text = g_strjoinv(to, more);

	return g_uri_escape_string(text, NULL, TRUE);
}

/* Spec: Offered; A newer override; Not newer: the built-in sites offered,
 * each in one sentence, nothing asked of them; a built-in definition's page
 * with the file in use, its override pasted, refused, removed */
static void
test_builtin_on_the_page(Fixture *fx, gconstpointer data)
{
	gint64 id = builtin_id(fx, "axis-limetorrents");
	g_autofree gchar *page_path = g_strdup_printf("/indexers/definitions/%" G_GINT64_FORMAT, id);
	g_autofree gchar *override_path = g_strdup_printf("%s/override", page_path);
	g_autofree gchar *remove_path = g_strdup_printf("%s/override/remove", page_path);
	g_autofree gchar *newer = pasted("axis-limetorrents", 2, "searchrss/", "searchfeed/");
	g_autofree gchar *same = pasted("axis-limetorrents", 1, "searchrss/", "searchfeed/");
	g_autofree gchar *other = pasted("axis-torrentscsv", 2, "service/", "api/");
	g_autofree gchar *form = NULL;
	AxisTestReply list = get_as(fx, fx->admin, "/indexers");
	AxisTestReply page;
	AxisTestReply posted;

	/* Offered, each in its own sentence, added with no key. */
	g_assert_cmpuint(list.status, ==, 200);
	assert_contains(list.body, "TorrentProject, for movies and TV");
	assert_contains(list.body, "LimeTorrents, for movies and TV");
	assert_contains(list.body, "TheRARBG, for movies and TV");
	assert_contains(list.body, "Torrents-CSV, for movies and TV");
	assert_contains(list.body, "LimeTorrents is a public torrent index of every kind of file; Axis reads its search "
		"feed by words");
	assert_contains(list.body, "Its mirror rarbg.torrentsbay.org answers programs with a browser challenge, which "
		"Axis cannot pass.");
	assert_contains(list.body, "It needs no account and no key, and nothing is asked of it until you add it here.");
	assert_contains(list.body, "id=\"new-axis-therarbg-address\"");
	assert_contains(list.body, "<option value=\"https://rarbg.torrentsbay.org/\">");
	assert_contains(list.body, "TorrentProject, LimeTorrents, TheRARBG, Torrents-CSV");
	axis_test_web_assert_tables(list.body);
	axis_test_reply_clear(&list);
	assert_q(fx, "0", "SELECT count(*) FROM indexer_source");

	/* Its page: built in, the file in use, the way to repair it. */
	page = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Where it comes from: ");
	assert_contains(page.body, "Built in, version 1");
	assert_contains(page.body, "Repairing it: your own version");
	assert_contains(page.body, "name=\"text\"");
	assert_contains(page.body, "The file in use, to start a repair from");
	assert_contains(page.body, "id: axis-limetorrents");
	assert_contains(page.body, "definitions/axis-limetorrents.yml");
	assert_contains(page.body, "In use: the built-in version 1.");
	assert_lacks(page.body, "Remove your version");
	axis_test_reply_clear(&page);

	/* Not newer, or another's: refused on the page, saying why. */
	form = g_strdup_printf("text=%s", same);
	posted = post_as(fx, fx->admin, page_path, override_path, form);
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "an override must say a later version");
	axis_test_reply_clear(&posted);
	g_free(form);
	form = g_strdup_printf("text=%s", other);
	posted = post_as(fx, fx->admin, page_path, override_path, form);
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "That definition&#39;s id is axis-torrentscsv: a version of this one keeps the id "
		"axis-limetorrents.");
	axis_test_reply_clear(&posted);
	posted = post_as(fx, fx->admin, page_path, override_path, "text=");
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "Paste the definition to use.");
	axis_test_reply_clear(&posted);

	/* Newer: in use, said so, removable. */
	g_free(form);
	form = g_strdup_printf("text=%s", newer);
	posted = post_as(fx, fx->admin, page_path, override_path, form);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, page_path);
	assert_contains(page.body, "Built in, overridden by your version 2 (built in: 1)");
	assert_contains(page.body, "In use: your version 2, pasted or uploaded on this page.");
	assert_contains(page.body, "Remove your version");
	assert_contains(page.body, "searchfeed/");
	axis_test_reply_clear(&page);
	list = get_as(fx, fx->admin, "/indexers");
	assert_contains(list.body, "Built in, overridden by your version 2 (built in: 1)");
	axis_test_reply_clear(&list);
	posted = post_as(fx, fx->admin, page_path, remove_path, "confirm=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, page_path);
	assert_contains(page.body, "Built in, version 1");
	assert_lacks(page.body, "Remove your version");
	axis_test_reply_clear(&page);
	/* A built-in one is not removed. */
	{
		g_autofree gchar *path = g_strdup_printf("%s/remove", page_path);

		posted = post_as(fx, fx->admin, "/indexers", path, "confirm=yes");
		g_assert_cmpuint(posted.status, ==, 400);
		assert_contains(posted.body, "built into Axis");
		axis_test_reply_clear(&posted);
	}
}

/* Spec: A field that matches nothing; Markup in an answer; Changed layout
 * (the dashboard): "Check this indexer", step by step */
static void
test_check_page(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *site = axis_test_plex_new();
	g_autofree gchar *base = axis_test_plex_base(site);
	g_autofree gchar *form = NULL;
	g_autofree gchar *check_path = NULL;
	g_autofree gchar *source_path = NULL;
	g_autoptr(GBytes) page_body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisIndexerSearchResult) result = NULL;
	AxisIndexerSourceSpec spec;
	AxisIndexerRequest request;
	AxisTestReply reply;
	gint64 id = 0;
	static const gchar page[] =
		"<html><body><div id=\"similarfiles\"><div><span><a href='/t3-1/Made-Up-Film-torrent.html'>"
		"Made Up &lt;script&gt;alert(1)&lt;/script&gt; Film</a></span> <span>3</span><span>1</span><span>6 months ago"
		"</span><span>1 GB</span></div><div><span><a href='/t3-2/Made-Up-Film-2-torrent.html'>Made Up Film 2</a>"
		"</span> <span>4</span></div></div></body></html>";

	/* Fast, as the other tests of public sites are. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_INDEXER_SETTING_SITE_RATE, "20", &error));
	memset(&spec, 0, sizeof(spec));
	spec.kind = AXIS_INDEXER_CARDIGANN_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.definition_id = builtin_id(fx, "axis-torrentproject");
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));
	g_assert_no_error(error);
	check_path = g_strdup_printf("/indexers/%" G_GINT64_FORMAT "/check", id);
	source_path = g_strdup_printf("/indexers/%" G_GINT64_FORMAT, id);

	/* Linked from its row and its page; the form asks nothing. */
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, check_path);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, source_path);
	assert_contains(reply.body, "Check this indexer");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, check_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "name=\"words\"");
	assert_lacks(reply.body, "Answer 1");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(site), ==, 0);
	reply = get_as(fx, fx->viewer, check_path);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* A check: every step, the markup a name holds as text, the field that
	 * matched nothing. */
	page_body = g_bytes_new(page, strlen(page));
	axis_test_plex_route_full(site, "/", 200, "text/html", page_body, NULL);
	reply = post_as(fx, fx->admin, check_path, check_path, "words=made+up+film&excerpts=yes");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Answer 1");
	assert_contains(reply.body, "/?t=made+up+film&amp;orderby=seeders");
	assert_contains(reply.body, "#similarfiles: found");
	assert_contains(reply.body, "matched 2");
	assert_contains(reply.body, "Row 1: kept");
	assert_contains(reply.body, "Row 2: left out: the field leechers matched nothing, and it must be there");
	assert_contains(reply.body, "Made Up &lt;script&gt;alert(1)&lt;/script&gt; Film");
	assert_lacks(reply.body, "<script>alert(1)");
	assert_contains(reply.body, "check-excerpt");
	axis_test_web_assert_tables(reply.body);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_plex_n_requests(site), ==, 1);

	/* Its pages change: the dashboard says so the day it happens. */
	axis_test_plex_clear_routes(site);
	g_clear_pointer(&page_body, g_bytes_unref);
	page_body = g_bytes_new_static("<html><body><h1>New look</h1></body></html>", 43);
	axis_test_plex_route_full(site, "/", 200, "text/html", page_body, NULL);
	axis_indexer_request_init(&request, AXIS_INDEXER_QUERY_TEXT);
	request.title = "made up film";
	g_assert_false(axis_indexers_search(fx->indexers, id, &request, NULL, &result, &error));
	g_assert_error(error, AXIS_INDEXER_ERROR, AXIS_INDEXER_ERROR_UNREADABLE);
	g_clear_error(&error);
	/* (The dashboard's card is the head of the list of "Needs attention":
	 * add-attention-notices, slice N2.  An indexer Axis holds for this has
	 * its notice at once, with no wait and no second contact.) */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	assert_contains(reply.body, "<span>Axis does not ask the indexer </span><bdi>TorrentProject</bdi><span>: its pages "
		"no longer look as its site definition expects.");
	assert_lacks(reply.body, "has been failing");
	assert_lacks(reply.body, "has not answered");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, "pages no longer look as expected");
	assert_contains(reply.body, "Passed over");
	axis_test_reply_clear(&reply);
	axis_test_plex_free(site);
}

static void
test_only_administrators(Fixture *fx, gconstpointer data)
{
	AxisTestReply page = get_as(fx, fx->viewer, "/indexers/definitions/1");
	AxisTestReply removed = post_as(fx, fx->viewer, "/indexers", "/indexers/definitions/1/remove", "confirm=yes");

	g_assert_cmpuint(page.status, ==, 403);
	g_assert_cmpuint(removed.status, ==, 403);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&removed);
	{
		g_autofree gchar *token = token_of(fx, fx->viewer, "/");

		g_assert_cmpuint(upload(fx, fx->viewer, token, "id: x\n", NULL), ==, 403);
	}
	{
		AxisTestReply missing = get_as(fx, fx->admin, "/indexers/definitions/999");

		g_assert_cmpuint(missing.status, ==, 404);
		axis_test_reply_clear(&missing);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web/definitions/" path, Fixture, NULL, func)
	ADD("on-the-page", test_definitions_on_the_page);
	ADD("back-to-place", test_back_to_place);
	ADD("builtin-on-the-page", test_builtin_on_the_page);
	ADD("check-page", test_check_page);
	ADD("only-administrators", test_only_administrators);
#undef ADD
	return g_test_run();
}
