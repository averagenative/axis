/*
 * test-web.c - the web shell: layout, escaping, navigation, static
 * assets, security headers, error pages and the dashboard
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test runs a real HtmxServer on an ephemeral loopback port with the
 * shell installed, and talks to it with libsoup.  No database is needed.
 *
 * The shell always runs with authentication: there is no way to install
 * it without (that would fail open).  These tests use an auth that has no
 * database, open a session in it directly, and send that session's
 * cookie with every request unless a test asks for an anonymous one.  So
 * the shell is tested behind exactly the middleware the daemon uses; the
 * tests of logging in itself, which need the database, are in
 * test-web-auth.c.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-web.h"
#include "axis-version.h"
#include "core/axis-module.h"
#include "web/axis-web.h"
#include "web/axis-web-attention.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-path.h"

#define SCRIPT_TEXT "<script>alert(\"x\")</script> 'q' & \"d\""

enum
{
	WITH_MOVIES = 1 << 0,
	WITH_SERIES = 1 << 1,
	AS_USER     = 1 << 2,	/* the session is not an administrator's */
	WITH_ALL    = 1 << 3,	/* a module with a page in every tab */
	WITH_EXTRA  = 1 << 4,	/* one more management page */
	WITH_BOARD  = 1 << 5	/* a dashboard part */
};

typedef struct
{
	HtmxServer         *server;
	AxisModuleRegistry *registry;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	guint16             port;
	gchar              *cookie;	/* "axis_session=..." */
	gchar              *other_cookie;	/* a second user's session */
} Fixture;

/* How often the movies module's POST handler ran. */
static guint pokes;

/* A completed request. */
typedef struct
{
	guint        status;
	SoupMessage *message;
	gchar       *body;
} Reply;

/* ── Test modules ───────────────────────────────────────────────── */

static HtmxResponse *
page_movies(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "Movie list goes here.");

	return axis_web_page_respond(request, "Movies", HTMX_NODE(text));
}

static HtmxResponse *
page_escape(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), SCRIPT_TEXT);

	return axis_web_page_respond(request, SCRIPT_TEXT, HTMX_NODE(text));
}

static HtmxResponse *
page_boom(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(GError) error = NULL;

	g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
		"secret-detail: password=hunter2");

	return axis_web_internal_error(request, error);
}

static HtmxResponse *
page_poke(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	pokes++;
	htmx_node_set_text_content(HTMX_NODE(text), "Poked.");

	return axis_web_page_respond(request, "Poke", HTMX_NODE(text));
}

static HtmxResponse *
page_admin(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "Movie settings.");

	return axis_web_page_respond(request, "Movie settings", HTMX_NODE(text));
}

/*
 * A module that registers its routes and does nothing about
 * authentication (apart from the one route it marks as for
 * administrators).
 */
static HtmxResponse *page_tables(HtmxRequest *request, GHashTable *params, gpointer user_data);

static void
movies_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_get(web, "/tables", page_tables, NULL);
	axis_web_get(web, "/movies", page_movies, NULL);
	axis_web_get(web, "/escape", page_escape, NULL);
	axis_web_get(web, "/boom", page_boom, NULL);
	axis_web_post(web, "/movies/poke", page_poke, NULL);
	axis_web_route_full(web, HTMX_METHOD_GET, "/movies/settings", AXIS_WEB_ROUTE_ADMIN,
		page_admin, NULL);
}

/* Media -> Movies: its list, and a page for administrators. */
static const AxisNavEntry movies_nav[] = {
	{ "All", "/movies", 20, AXIS_NAV_GROUP_MEDIA, FALSE, "Movies", 0 },
	{ "Movie settings", "/movies/settings", 30, AXIS_NAV_GROUP_MEDIA, TRUE, "Movies", 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

/* Media -> TV, after Movies (its orders are higher). */
static const AxisNavEntry series_nav[] = {
	{ "Series", "/series", 110, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ "Anime", "/anime", 110, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

/* A page in every tab and of every media type; the system one is for
 * administrators, and the one without a group lands in System.  Tidy
 * starts a second run of Media management's pages. */
static const AxisNavEntry all_nav[] = {
	{ "Wanted films", "/wanted-films", 10, AXIS_NAV_GROUP_MEDIA, FALSE, "Movies", 0 },
	{ "Wanted series", "/wanted-series", 110, AXIS_NAV_GROUP_MEDIA, FALSE, "TV", 0 },
	{ "Albums", "/albums", 210, AXIS_NAV_GROUP_MEDIA, FALSE, "Music", 0 },
	{ "Shelf", "/shelf", 310, AXIS_NAV_GROUP_MEDIA, FALSE, "Books", 0 },
	{ "Downloads", "/downloads", 10, AXIS_NAV_GROUP_ACTIVITY, FALSE, NULL, 0 },
	{ "Disks", "/disks", 10, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 0 },
	{ "Tidy", "/tidy", 20, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 1 },
	{ "Machine", "/machine", 10, AXIS_NAV_GROUP_SYSTEM, TRUE, NULL, 0 },
	/* States no group (zero): it is a System page. */
	{ "Ungrouped", "/ungrouped", 50, 0, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static const AxisNavEntry extra_nav[] = {
	{ "Later", "/later", 90, AXIS_NAV_GROUP_MANAGEMENT, FALSE, NULL, 1 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};

static void
all_routes(AxisWeb *web, gpointer user_data)
{
	static const gchar *const paths[] = {
		"/wanted-films", "/wanted-series", "/albums", "/shelf", "/downloads", "/disks", "/tidy", "/ungrouped", NULL
	};
	guint i;

	for (i = 0; paths[i] != NULL; i++)
		axis_web_get(web, paths[i], page_movies, NULL);
	axis_web_route_full(web, HTMX_METHOD_GET, "/machine", AXIS_WEB_ROUTE_ADMIN, page_admin, NULL);
}

/* Adds @child to @parent and drops the caller's reference. */
static void
own(
	gpointer parent,
	gpointer child
){
	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(child));
	g_object_unref(child);
}

/* ── A dashboard part ───────────────────────────────────────────── */

/* How often the part was read, what it gives, and whether a reading of it
 * waits (the started board's test holds one on its thread). */
static gint board_collects;
static gint board_figure = 42;
static gint board_held;

static gpointer
board_collect(gpointer user_data)
{
	g_atomic_int_inc(&board_collects);
	while (g_atomic_int_get(&board_held))
		g_usleep(1000);
	/* On the worker thread: the figure this part shows. */
	return g_strdup(g_atomic_int_get(&board_figure) == 42 ? "forty-two" : "forty-three");
}

static void
board_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          figures,
	gpointer          user_data
){
	g_autoptr(HtmxP) piece = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(piece), figures);
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SPACE, 0, "/movies", HTMX_NODE(g_steal_pointer(&piece)));
}

/* What the module says needs attention: the dashboard's card is drawn from
 * these, through the board, and from nothing a part gives. */
static gboolean
board_attention(
	const AxisAttentionReading  *reading,
	GPtrArray                   *items,
	gpointer                     user_data,
	GError                     **error
){
	AxisAttentionItem *wrong = axis_attention_item_new("webtest.wrong", "", AXIS_ATTENTION_WAITING, 3, 0);
	AxisAttentionItem *away = axis_attention_item_new("webtest.away", "1", AXIS_ATTENTION_BROKEN, 1, 0);

	axis_attention_item_say(wrong, "3 things are wrong.", NULL, NULL);
	axis_attention_item_link(wrong, "/movies", "Open Movies");
	g_ptr_array_add(items, wrong);
	axis_attention_item_say(away, "The volume ", "Disk <one>", " is away.");
	axis_attention_item_link(away, "/movies", "Open Movies");
	g_ptr_array_add(items, away);
	return TRUE;
}

/* How often the administrators' part was read. */
static gint admin_collects;

static gpointer
admin_collect(gpointer user_data)
{
	g_atomic_int_inc(&admin_collects);
	return NULL;
}

static void
admin_render(
	AxisWebDashboard *board,
	HtmxRequest      *request,
	gpointer          figures,
	gpointer          user_data
){
	g_autoptr(HtmxP) piece = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(piece), "an administrators' matter");
	axis_web_dashboard_add(board, AXIS_WEB_DASHBOARD_SPACE, 5, "/movies", HTMX_NODE(g_steal_pointer(&piece)));
}

static void
board_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_add_dashboard_part(web, board_collect, board_render, g_free, NULL);
	axis_web_set_dashboard_part_card(web, AXIS_WEB_DASHBOARD_SPACE);
	axis_web_add_attention_source(web, "the test's things", board_attention, NULL);
	axis_web_add_dashboard_part_for_admins(web, admin_collect, admin_render, NULL, NULL);
}

/* Two tables of one kind with different contents, and one of another. */
static HtmxResponse *
page_tables(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	static const AxisWebColumn columns[] = {
		{ "Name", AXIS_WEB_COL_NAME },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "When", AXIS_WEB_COL_DATE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	static const AxisWebColumn other[] = {
		{ "What", AXIS_WEB_COL_WIDE },
		{ "Share", AXIS_WEB_COL_NUMBER },
		{ NULL, 0 }
	};
	static const gchar *const names[] = { "a", "a much longer name that has to wrap somewhere", NULL };
	g_autoptr(HtmxDiv) content = htmx_div_new();
	guint t;

	for (t = 0; names[t] != NULL; t++) {
		HtmxNode *table = axis_web_table_new("things", columns);
		HtmxTbody *body = htmx_tbody_new();
		HtmxTr *row = htmx_tr_new();
		g_autofree gchar *count = g_strdup_printf("%u", 7 + t * 1000);

		own(row, htmx_td_new_with_text(names[t]));
		own(row, htmx_td_new_with_text(count));
		own(row, htmx_td_new_with_text("1.0 GiB"));
		own(row, htmx_td_new_with_text("2026-11-13 10:00:00 UTC"));
		own(row, htmx_td_new_with_text(""));
		own(body, row);
		own(table, body);
		own(content, table);
	}
	own(content, axis_web_table_new("others", other));

	return axis_web_page_respond(request, "Tables", HTMX_NODE(content));
}

static void
register_modules(AxisModuleRegistry *registry, guint flags)
{
	AxisModuleInfo info;
	GError *error = NULL;

	if (flags & WITH_SERIES) {
		memset(&info, 0, sizeof(info));
		info.id = "series";
		info.name = "Series library";
		info.nav_entries = series_nav;
		g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_no_error(error);
	}
	if (flags & WITH_MOVIES) {
		memset(&info, 0, sizeof(info));
		info.id = "movies";
		info.name = "Movie library";
		info.nav_entries = movies_nav;
		info.register_routes = movies_routes;
		g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_no_error(error);
	}
	if (flags & WITH_ALL) {
		memset(&info, 0, sizeof(info));
		info.id = "everything";
		info.name = "Everything";
		info.nav_entries = all_nav;
		info.register_routes = all_routes;
		g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_no_error(error);
	}
	if (flags & WITH_EXTRA) {
		memset(&info, 0, sizeof(info));
		info.id = "extra";
		info.name = "Extra";
		info.nav_entries = extra_nav;
		g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_no_error(error);
	}
	if (flags & WITH_BOARD) {
		memset(&info, 0, sizeof(info));
		info.id = "board";
		info.name = "Board";
		info.register_routes = board_routes;
		g_assert_true(axis_module_registry_register(registry, &info, NULL, &error));
		g_assert_no_error(error);
	}
}

/* ── Fixture ────────────────────────────────────────────────────── */

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	GError *error = NULL;
	GSList *uris;
	const gchar *colon;

	memset(fx, 0, sizeof(*fx));
	fx->registry = axis_module_registry_new();
	register_modules(fx->registry, GPOINTER_TO_UINT(data));

	fx->server = htmx_server_new();
	g_assert_true(htmx_server_listen_on(fx->server, "127.0.0.1", 0, &error));
	g_assert_no_error(error);
	fx->auth = axis_web_auth_new(NULL, "", AXIS_SECURE_COOKIES_AUTO, &error);
	g_assert_no_error(error);
	fx->web = axis_web_new(fx->server, fx->registry, fx->auth);
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_no_error(error);
	fx->cookie = axis_web_auth_open_test_session(fx->auth, "tester",
		(GPOINTER_TO_UINT(data) & AS_USER) ? AXIS_USER_ROLE_USER : AXIS_USER_ROLE_ADMIN);
	fx->other_cookie = axis_web_auth_open_test_session(fx->auth, "other", AXIS_USER_ROLE_USER);
	pokes = 0;

	uris = htmx_server_get_listen_uris(fx->server);
	g_assert_nonnull(uris);
	colon = strrchr(uris->data, ':');
	g_assert_nonnull(colon);
	fx->port = (guint16)g_ascii_strtoull(colon + 1, NULL, 10);
	g_assert_cmpuint(fx->port, !=, 0);
	g_slist_free_full(uris, g_free);

	fx->session = soup_session_new_with_options("timeout", 10, "idle-timeout", 10, NULL);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_free(fx->web);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	g_free(fx->cookie);
	g_free(fx->other_cookie);
}

/* ── Requests ───────────────────────────────────────────────────── */

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} Pending;

static void
on_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Pending *pending = user_data;

	pending->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &pending->error);
	g_main_loop_quit(pending->loop);
}

/*
 * Sends @method to @path (not normalised or decoded: it goes out as
 * given) with @cookie (nullable), @extra headers (name, value pairs
 * ended by NULL) and @form (nullable: an urlencoded body).  Redirects are
 * not followed.
 */
static Reply
request_full(
	Fixture     *fx,
	const gchar *method,
	const gchar *path,
	const gchar *cookie,
	const gchar *const *extra,
	const gchar *form
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", (guint)fx->port, path);
	GError *error = NULL;
	GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED | G_URI_FLAGS_PARSE_RELAXED, &error);
	SoupMessage *message;
	SoupMessageHeaders *headers;
	Pending pending;
	Reply reply;
	gsize size;
	gconstpointer data;
	guint i;

	g_assert_no_error(error);
	message = soup_message_new_from_uri(method, uri);
	g_uri_unref(uri);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	headers = soup_message_get_request_headers(message);
	if (cookie != NULL)
		soup_message_headers_append(headers, "Cookie", cookie);
	for (i = 0; extra != NULL && extra[i] != NULL; i += 2)
		soup_message_headers_append(headers, extra[i], extra[i + 1]);
	if (form != NULL) {
		g_autoptr(GBytes) body = g_bytes_new(form, strlen(form));

		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", body);
	}

	memset(&pending, 0, sizeof(pending));
	pending.loop = g_main_loop_new(NULL, FALSE);
	soup_session_send_and_read_async(fx->session, message, G_PRIORITY_DEFAULT, NULL, on_done, &pending);
	g_main_loop_run(pending.loop);
	g_main_loop_unref(pending.loop);

	g_assert_no_error(pending.error);
	g_assert_nonnull(pending.bytes);

	data = g_bytes_get_data(pending.bytes, &size);
	reply.message = message;
	reply.status = soup_message_get_status(message);
	reply.body = g_strndup(data != NULL ? data : "", size);
	g_bytes_unref(pending.bytes);

	return reply;
}

static Reply
request_with(
	Fixture     *fx,
	const gchar *method,
	const gchar *path,
	const gchar *cookie,
	const gchar *const *extra
){
	return request_full(fx, method, path, cookie, extra, NULL);
}

/* As the logged-in test user. */
static Reply
request(
	Fixture     *fx,
	const gchar *method,
	const gchar *path,
	const gchar *const *extra
){
	return request_with(fx, method, path, fx->cookie, extra);
}

static Reply
get(Fixture *fx, const gchar *path)
{
	return request(fx, "GET", path, NULL);
}

static Reply
get_anonymous(Fixture *fx, const gchar *path)
{
	return request_with(fx, "GET", path, NULL, NULL);
}

static void
reply_clear(Reply *reply)
{
	g_object_unref(reply->message);
	g_free(reply->body);
}

static const gchar *
header(const Reply *reply, const gchar *name)
{
	return soup_message_headers_get_one(soup_message_get_response_headers(reply->message), name);
}

/* ── Assertions ─────────────────────────────────────────────────── */

static void
assert_contains(const gchar *haystack, const gchar *needle)
{
	if (strstr(haystack, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack);
}

static void
assert_lacks(const gchar *haystack, const gchar *needle)
{
	if (strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static guint
count_of(
	const gchar *haystack,
	const gchar *needle
){
	guint count = 0;
	const gchar *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		count++;
		p += strlen(needle);
	}
	return count;
}

static void
assert_security_headers(const Reply *reply)
{
	const gchar *csp = header(reply, "Content-Security-Policy");

	g_assert_nonnull(csp);
	g_assert_true(g_str_has_prefix(csp, "default-src 'self'"));
	assert_contains(csp, "frame-ancestors 'none'");
	assert_lacks(csp, "unsafe-inline");
	assert_lacks(csp, "unsafe-eval");
	g_assert_cmpstr(header(reply, "X-Content-Type-Options"), ==, "nosniff");
	g_assert_cmpstr(header(reply, "Referrer-Policy"), ==, "same-origin");
	g_assert_cmpstr(header(reply, "X-Frame-Options"), ==, "DENY");
}

static void
assert_no_store(const Reply *reply)
{
	g_assert_cmpstr(header(reply, "Cache-Control"), ==, "no-store");
}

/* The page has no inline script, no style element and no style attribute. */
static void
assert_no_inline_code(const gchar *html)
{
	const gchar *p = html;

	assert_lacks(html, "<style");
	assert_lacks(html, " style=");
	assert_lacks(html, "\tstyle=");
	assert_lacks(html, "\nstyle=");

	while ((p = strstr(p, "<script")) != NULL) {
		const gchar *end = strchr(p, '>');
		g_autofree gchar *tag = NULL;

		g_assert_nonnull(end);
		tag = g_strndup(p, end - p + 1);
		assert_contains(tag, " src=");
		/* ... and it has no body. */
		g_assert_true(g_str_has_prefix(end + 1, "</script>"));
		p = end;
	}
}

/* The part of @html from the element whose opening tag holds @marker to the
 * end of the first "</nav>" after it, or NULL. */
static gchar *
nav_part(
	const gchar *html,
	const gchar *marker
){
	const gchar *start = strstr(html, marker);
	const gchar *end;

	if (start == NULL)
		return NULL;
	end = strstr(start, "</nav>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* The primary tabs, and the second row (NULL when there is none). */
static gchar *
tab_row(const gchar *html)
{
	gchar *row = nav_part(html, "class=\"primary-nav\"");

	g_assert_nonnull(row);
	return row;
}

static gchar *
page_row(const gchar *html)
{
	return nav_part(html, "class=\"page-nav\"");
}

/* Inside Media: the second row (the media types) and the third (the open
 * type's pages), NULL when there is none. */
static gchar *
section_row(const gchar *html)
{
	return nav_part(html, "class=\"page-nav section-nav\"");
}

static gchar *
type_row(const gchar *html)
{
	return nav_part(html, "class=\"page-nav type-nav\"");
}

/* The anchor in @part that links to @href, from "<a" to ">"; NULL if none. */
static gchar *
anchor_in(const gchar *part, const gchar *href)
{
	g_autofree gchar *needle = g_strdup_printf("href=\"%s\"", href);
	const gchar *at;
	const gchar *start;
	const gchar *end;

	if (part == NULL)
		return NULL;
	at = strstr(part, needle);
	if (at == NULL)
		return NULL;
	for (start = at; start > part && !(start[0] == '<' && start[1] == 'a'); start--)
		;
	end = strchr(at, '>');
	g_assert_nonnull(end);

	return g_strndup(start, end - start + 1);
}

/* The labels of the tabs, in order, "|"-separated. */
static gchar *
tab_labels(const gchar *html)
{
	g_autofree gchar *row = tab_row(html);
	GString *labels = g_string_new(NULL);
	const gchar *p = row;

	while ((p = strstr(p, "<a ")) != NULL) {
		const gchar *open = strchr(p, '>');
		const gchar *close;

		g_assert_nonnull(open);
		close = strstr(open, "</a>");
		g_assert_nonnull(close);
		g_string_append_printf(labels, "%s%.*s", labels->len > 0 ? "|" : "", (gint)(close - open - 1), open + 1);
		p = close;
	}
	return g_string_free(labels, FALSE);
}

/* ── Layout ─────────────────────────────────────────────────────── */

static void
test_layout_full_page(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/movies");

	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(header(&reply, "Content-Type"), "text/html"));
	g_assert_true(g_str_has_prefix(reply.body, "<!DOCTYPE html>"));
	assert_contains(reply.body, "<html");
	assert_contains(reply.body, "<title>Movies - Axis</title>");
	assert_contains(reply.body, "<nav");
	assert_contains(reply.body, "Movie list goes here.");
	assert_contains(reply.body, "Axis");
	reply_clear(&reply);
}

static void
test_layout_htmx_fragment(Fixture *fx, gconstpointer data)
{
	const gchar *const extra[] = { "HX-Request", "true", NULL };
	Reply reply = request(fx, "GET", "/movies", extra);

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Movie list goes here.");
	assert_contains(reply.body, "<title>Movies - Axis</title>");
	assert_lacks(reply.body, "<!DOCTYPE");
	assert_lacks(reply.body, "<html");
	assert_lacks(reply.body, "<body");
	assert_lacks(reply.body, "<nav");
	assert_lacks(reply.body, "<header");
	reply_clear(&reply);
}

static void
test_layout_history_restore(Fixture *fx, gconstpointer data)
{
	const gchar *const extra[] = {
		"HX-Request", "true",
		"HX-History-Restore-Request", "true",
		NULL
	};
	Reply reply = request(fx, "GET", "/movies", extra);

	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<!DOCTYPE html>"));
	assert_contains(reply.body, "<nav");
	assert_contains(reply.body, "Movie list goes here.");
	reply_clear(&reply);
}

/* ── Escaping and inline code ───────────────────────────────────── */

static void
test_escaping(Fixture *fx, gconstpointer data)
{
	const gchar *const htmx[] = { "HX-Request", "true", NULL };
	Reply full = get(fx, "/escape");
	Reply fragment = request(fx, "GET", "/escape", htmx);

	g_assert_cmpuint(full.status, ==, 200);
	assert_contains(full.body, "&lt;script&gt;alert(");
	assert_lacks(full.body, "<script>");
	assert_lacks(full.body, "alert(\"x\")</script>");
	assert_no_inline_code(full.body);

	g_assert_cmpuint(fragment.status, ==, 200);
	assert_contains(fragment.body, "&lt;script&gt;alert(");
	assert_lacks(fragment.body, "<script>");

	reply_clear(&full);
	reply_clear(&fragment);
}

static void
test_no_inline_code(Fixture *fx, gconstpointer data)
{
	const gchar *const paths[] = { "/", "/movies", "/escape", "/missing", NULL };
	guint i;

	for (i = 0; paths[i] != NULL; i++) {
		Reply reply = get(fx, paths[i]);

		assert_no_inline_code(reply.body);
		reply_clear(&reply);
	}
}

/* ── Navigation ─────────────────────────────────────────────────── */

static void
test_nav_entry_present(Fixture *fx, gconstpointer data)
{
	Reply home = get(fx, "/");
	Reply other = get(fx, "/missing");
	Reply movies = get(fx, "/movies");
	g_autofree gchar *tabs = tab_row(home.body);
	g_autofree gchar *types = section_row(movies.body);
	g_autofree gchar *pages = type_row(movies.body);
	g_autofree gchar *anchor = anchor_in(tabs, "/movies");

	/* The Media tab links to its first type's first page; the type is in
	 * the second row, its page in the third. */
	g_assert_nonnull(anchor);
	assert_contains(tabs, ">Media</a>");
	assert_contains(other.body, "href=\"/movies\"");
	g_assert_nonnull(types);
	assert_contains(types, ">Movies</a>");
	g_assert_nonnull(pages);
	assert_contains(pages, ">All</a>");
	/* No second row of the plain kind inside Media. */
	g_assert_null(page_row(movies.body));
	/* Three labelled navigation landmarks. */
	assert_contains(movies.body, "aria-label=\"Main\"");
	assert_contains(movies.body, "aria-label=\"Media sections\"");
	assert_contains(movies.body, "aria-label=\"Movies pages\"");

	reply_clear(&home);
	reply_clear(&other);
	reply_clear(&movies);
}

/* Spec: Module switched off (and a module not there at all). */
static void
test_nav_absent_module(Fixture *fx, gconstpointer data)
{
	Reply home = get(fx, "/");
	Reply movies = get(fx, "/movies");
	g_autofree gchar *labels = tab_labels(home.body);
	GError *error = NULL;

	/* The series module is registered, the movies module is not: Media
	 * leads to TV's first page. */
	assert_contains(home.body, "href=\"/anime\"");
	assert_lacks(home.body, "href=\"/movies\"");
	assert_lacks(home.body, ">Movies</a>");
	g_assert_cmpuint(movies.status, ==, 404);
	assert_lacks(movies.body, "href=\"/movies\"");
	g_assert_cmpstr(labels, ==, "Dashboard|Media|System");
	reply_clear(&home);
	reply_clear(&movies);

	/* Switched off: its pages, and with no media type left the Media tab,
	 * are gone. */
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "series", FALSE, &error));
	g_assert_no_error(error);
	home = get(fx, "/");
	assert_lacks(home.body, "href=\"/anime\"");
	assert_lacks(home.body, "href=\"/series\"");
	assert_lacks(home.body, ">Media</a>");
	assert_lacks(home.body, ">TV</a>");
	reply_clear(&home);
}

/* Within a tab: by order, then by label. */
static void
test_nav_order(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/series");
	g_autofree gchar *types = section_row(reply.body);
	g_autofree gchar *pages = type_row(reply.body);
	g_autofree gchar *labels = tab_labels(reply.body);
	const gchar *anime;
	const gchar *series;
	const gchar *movies;
	const gchar *tv;

	g_assert_nonnull(pages);
	anime = strstr(pages, "href=\"/anime\"");
	series = strstr(pages, "href=\"/series\"");
	g_assert_nonnull(anime);
	g_assert_nonnull(series);
	/* Equal orders (110, 110) by label. */
	g_assert_true(anime < series);
	/* Only the open type's pages. */
	assert_lacks(pages, "href=\"/movies\"");
	/* The media types in the order of their first pages: Movies, then TV;
	 * each links to its first page. */
	g_assert_nonnull(types);
	movies = strstr(types, ">Movies</a>");
	tv = strstr(types, ">TV</a>");
	g_assert_nonnull(movies);
	g_assert_nonnull(tv);
	g_assert_true(movies < tv);
	assert_contains(types, "href=\"/anime\"");
	/* One Media tab after the dashboard, whatever the number of types. */
	g_assert_cmpstr(labels, ==, "Dashboard|Media|System");

	reply_clear(&reply);
}

/* Spec: Second row (the current tab and page marked). */
static void
test_nav_current_page(Fixture *fx, gconstpointer data)
{
	Reply movies = get(fx, "/movies");
	Reply settings = get(fx, "/movies/settings");
	Reply home = get(fx, "/");
	g_autofree gchar *tabs = tab_row(movies.body);
	g_autofree gchar *types = section_row(movies.body);
	g_autofree gchar *pages = type_row(movies.body);
	g_autofree gchar *settings_pages = type_row(settings.body);
	g_autofree gchar *home_tabs = tab_row(home.body);
	g_autofree gchar *home_pages = page_row(home.body);
	g_autofree gchar *home_types = section_row(home.body);
	g_autofree gchar *movies_on_movies = anchor_in(pages, "/movies");
	g_autofree gchar *settings_on_movies = anchor_in(pages, "/movies/settings");
	g_autofree gchar *media_tab = anchor_in(tabs, "/movies");
	g_autofree gchar *movies_type = anchor_in(types, "/movies");
	g_autofree gchar *tv_type = anchor_in(types, "/anime");
	g_autofree gchar *dashboard_on_movies = anchor_in(tabs, "/");
	g_autofree gchar *movies_on_settings = anchor_in(settings_pages, "/movies");
	g_autofree gchar *settings_on_settings = anchor_in(settings_pages, "/movies/settings");
	g_autofree gchar *dashboard_on_home = anchor_in(home_tabs, "/");
	g_autofree gchar *media_on_home = anchor_in(home_tabs, "/movies");

	/* The page that is open, its media type and its tab. */
	assert_contains(movies_on_movies, "aria-current=\"page\"");
	assert_contains(movies_on_movies, "current");
	assert_lacks(settings_on_movies, "aria-current");
	assert_contains(media_tab, "aria-current=\"true\"");
	assert_contains(media_tab, "current");
	assert_contains(movies_type, "aria-current=\"true\"");
	assert_lacks(tv_type, "aria-current");
	assert_lacks(dashboard_on_movies, "aria-current");

	/* A page below another is the one marked. */
	assert_contains(settings_on_settings, "aria-current=\"page\"");
	assert_lacks(movies_on_settings, "aria-current");

	/* The dashboard has no second row. */
	assert_contains(dashboard_on_home, "aria-current=\"page\"");
	assert_lacks(media_on_home, "aria-current");
	g_assert_null(home_pages);
	g_assert_null(home_types);

	reply_clear(&movies);
	reply_clear(&settings);
	reply_clear(&home);
}

/* Spec: Media types and their pages.  Media's second row lists the types
 * whose module is enabled, and only those: a module switched off takes its
 * type (and its pages) with it, the others stay. */
static void
test_nav_media_types(Fixture *fx, gconstpointer data)
{
	GError *error = NULL;
	Reply reply = get(fx, "/movies");
	g_autofree gchar *types = section_row(reply.body);
	g_autofree gchar *pages = type_row(reply.body);
	g_autofree gchar *after = NULL;
	g_autofree gchar *after_pages = NULL;

	g_assert_nonnull(types);
	assert_contains(types, ">Movies</a>");
	assert_contains(types, ">TV</a>");
	/* No type without a module: no placeholders. */
	assert_lacks(types, ">Music</a>");
	assert_lacks(types, ">Books</a>");
	g_assert_nonnull(pages);
	assert_lacks(pages, "href=\"/anime\"");
	reply_clear(&reply);

	g_assert_true(axis_module_registry_set_enabled(fx->registry, "series", FALSE, &error));
	g_assert_no_error(error);
	reply = get(fx, "/movies");
	after = section_row(reply.body);
	after_pages = type_row(reply.body);
	g_assert_nonnull(after);
	assert_contains(after, ">Movies</a>");
	assert_lacks(after, ">TV</a>");
	assert_lacks(reply.body, "href=\"/anime\"");
	g_assert_nonnull(after_pages);
	assert_contains(after_pages, "href=\"/movies\"");
	reply_clear(&reply);
}

/*
 * Spec (web-shell, add-phone-layout): A phone's header; The tabs at the
 * foot; Safe areas.  What the phone's layout of the header is drawn from,
 * without script: the viewport reaches the screen's edges; the tabs are the
 * one list of the main navigation (the bar at the foot); the search box is
 * in the top bar; the rows of pages are one element after the top bar, the
 * media types before the open type's pages, every link a plain link.
 */
static void
test_nav_phone_markup(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/movies");
	const gchar *top = strstr(reply.body, "class=\"bar top-bar\"");
	const gchar *tabs = strstr(reply.body, "<nav id=\"primary-nav\" class=\"primary-nav\" aria-label=\"Main\">");
	const gchar *search = strstr(reply.body, "class=\"search-slot\"");
	const gchar *rows = strstr(reply.body, "<div id=\"nav-rows\">");
	const gchar *types = strstr(reply.body, "class=\"page-nav section-nav\"");
	const gchar *pages = strstr(reply.body, "class=\"page-nav type-nav\"");
	const gchar *main_start = strstr(reply.body, "<main");

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, "
		"viewport-fit=cover\" />");
	g_assert_nonnull(top);
	g_assert_nonnull(tabs);
	g_assert_nonnull(search);
	g_assert_nonnull(rows);
	g_assert_nonnull(types);
	g_assert_nonnull(pages);
	g_assert_nonnull(main_start);
	/* In the top bar: the tabs, then the search box; then the rows. */
	g_assert_true(top < tabs);
	g_assert_true(tabs < search);
	g_assert_true(search < rows);
	g_assert_true(rows < types);
	g_assert_true(types < pages);
	g_assert_true(pages < main_start);
	g_assert_cmpuint(count_of(reply.body, "class=\"nav-list tab-list\""), ==, 1);
	/* Nothing opens or closes them: no disclosure, no script. */
	g_assert_null(g_strstr_len(reply.body, main_start - reply.body, "<details"));
	g_assert_null(g_strstr_len(reply.body, main_start - reply.body, " onclick="));
	reply_clear(&reply);
}

/* Spec: Order of tabs; a page that states no group is under System. */
static void
test_nav_tab_order(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/");
	g_autofree gchar *labels = tab_labels(reply.body);
	Reply system = get(fx, "/ungrouped");
	g_autofree gchar *pages = page_row(system.body);
	Reply albums = get(fx, "/albums");
	g_autofree gchar *types = section_row(albums.body);
	g_autofree gchar *album_pages = type_row(albums.body);
	const gchar *order[] = { ">Movies</a>", ">TV</a>", ">Music</a>", ">Books</a>", NULL };
	const gchar *last = NULL;
	guint i;

	g_assert_cmpstr(labels, ==, "Dashboard|Media|Activity|Library|System");
	g_assert_nonnull(pages);
	assert_contains(pages, "href=\"/ungrouped\"");
	assert_contains(pages, "href=\"/machine\"");
	assert_contains(pages, "href=\"/admin/users\"");

	/* Every media type is in Media's second row, in the order of its first
	 * page; a type of one page has no third row and is that page. */
	g_assert_nonnull(types);
	for (i = 0; order[i] != NULL; i++) {
		const gchar *at = strstr(types, order[i]);

		g_assert_nonnull(at);
		g_assert_true(last == NULL || at > last);
		last = at;
	}
	g_assert_null(album_pages);
	{
		g_autofree gchar *music = anchor_in(types, "/albums");

		assert_contains(music, "aria-current=\"page\"");
	}

	reply_clear(&reply);
	reply_clear(&system);
	reply_clear(&albums);
}

/* Spec: Empty tab. */
static void
test_nav_empty_tab(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/");
	g_autofree gchar *labels = tab_labels(reply.body);

	/* Only the movies module: Media, and no Activity or Media management
	 * tab. */
	g_assert_cmpstr(labels, ==, "Dashboard|Media|System");
	reply_clear(&reply);
}

/* A tab with one page has no second row; the tab is that page. */
static void
test_nav_single_page(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/downloads");
	g_autofree gchar *tabs = tab_row(reply.body);
	g_autofree gchar *pages = page_row(reply.body);
	g_autofree gchar *activity = anchor_in(tabs, "/downloads");

	g_assert_null(pages);
	g_assert_nonnull(activity);
	assert_contains(activity, "aria-current=\"page\"");
	assert_contains(tabs, ">Activity</a>");
	reply_clear(&reply);

	/* A tab with two has one; Tidy starts a second run, set apart. */
	reply = get(fx, "/tidy");
	g_free(pages);
	pages = page_row(reply.body);
	g_assert_nonnull(pages);
	assert_contains(pages, ">Disks</a>");
	assert_contains(pages, ">Tidy</a>");
	{
		const gchar *run = strstr(pages, "nav-run-start");
		const gchar *disks = strstr(pages, "href=\"/disks\"");

		g_assert_nonnull(run);
		g_assert_nonnull(disks);
		g_assert_true(disks < run);
		g_assert_true(g_str_has_prefix(strstr(run, "href="), "href=\"/tidy\""));
		g_assert_null(strstr(run + 1, "nav-run-start"));
	}
	reply_clear(&reply);
}

/* Spec: Non-administrator. */
static void
test_nav_not_admin(Fixture *fx, gconstpointer data)
{
	Reply home = get(fx, "/");
	Reply movies = get(fx, "/movies");
	Reply settings = get(fx, "/movies/settings");
	g_autofree gchar *labels = tab_labels(home.body);
	g_autofree gchar *pages = type_row(movies.body);
	g_autofree gchar *types = section_row(movies.body);

	/* The System tab has only administrators' pages (Users): it is not
	 * shown. */
	g_assert_cmpstr(labels, ==, "Dashboard|Media");
	assert_lacks(home.body, "href=\"/admin/users\"");
	assert_lacks(movies.body, "href=\"/movies/settings\"");
	/* Movies has one page left for this user: the types' row, no third. */
	g_assert_nonnull(types);
	g_assert_null(pages);
	/* Hiding is not the protection: the route still refuses. */
	g_assert_cmpuint(settings.status, ==, 403);

	reply_clear(&home);
	reply_clear(&movies);
	reply_clear(&settings);
}

/* The administrator's view of the same: all of it. */
static void
test_nav_admin(Fixture *fx, gconstpointer data)
{
	Reply movies = get(fx, "/movies");
	g_autofree gchar *labels = tab_labels(movies.body);
	g_autofree gchar *pages = type_row(movies.body);

	g_assert_cmpstr(labels, ==, "Dashboard|Media|System");
	g_assert_nonnull(pages);
	assert_contains(pages, "href=\"/movies/settings\"");
	reply_clear(&movies);
}

/* Spec: More pages.  A page added to a tab that has one already does not
 * change the top row. */
static void
test_nav_more_pages(Fixture *fx, gconstpointer data)
{
	GError *error = NULL;
	Reply before;
	Reply after;
	g_autofree gchar *row_before = NULL;
	g_autofree gchar *row_after = NULL;
	g_autofree gchar *pages = NULL;

	g_assert_true(axis_module_registry_set_enabled(fx->registry, "extra", FALSE, &error));
	g_assert_no_error(error);
	before = get(fx, "/");
	g_assert_true(axis_module_registry_set_enabled(fx->registry, "extra", TRUE, &error));
	g_assert_no_error(error);
	after = get(fx, "/");
	row_before = tab_row(before.body);
	row_after = tab_row(after.body);

	g_assert_cmpstr(row_before, ==, row_after);
	reply_clear(&before);
	reply_clear(&after);

	/* ... while the tab's own row has it. */
	after = get(fx, "/disks");
	pages = page_row(after.body);
	assert_contains(pages, ">Later</a>");
	reply_clear(&after);
}

/* ── Static assets ──────────────────────────────────────────────── */

static void
test_static_asset(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *url = axis_web_asset_url("vendor/htmx.min.js");
	g_autofree gchar *css_url = axis_web_asset_url("axis.css");
	Reply reply = get(fx, url);
	Reply css = get(fx, css_url);

	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(header(&reply, "Content-Type"), "text/javascript"));
	g_assert_nonnull(header(&reply, "ETag"));
	g_assert_cmpstr(header(&reply, "Cache-Control"), ==,
		"public, max-age=31536000, immutable");
	g_assert_cmpuint(strlen(reply.body), >, 1000);
	assert_contains(reply.body, "htmx");
	assert_security_headers(&reply);

	g_assert_cmpuint(css.status, ==, 200);
	g_assert_true(g_str_has_prefix(header(&css, "Content-Type"), "text/css"));
	assert_contains(css.body, "--bg");

	reply_clear(&reply);
	reply_clear(&css);
}

static void
test_static_url_matches_pages(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css_url = axis_web_asset_url("axis.css");
	g_autofree gchar *href = g_strdup_printf("href=\"%s\"", css_url);
	g_autofree gchar *js_url = axis_web_asset_url("vendor/htmx.min.js");
	g_autofree gchar *src = g_strdup_printf("src=\"%s\"", js_url);
	Reply reply = get(fx, "/");

	g_assert_cmpuint(strlen(axis_web_asset_version()), ==, AXIS_WEB_VERSION_LEN);
	assert_contains(reply.body, href);
	assert_contains(reply.body, src);

	reply_clear(&reply);
}

static void
test_static_conditional(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *url = axis_web_asset_url("vendor/htmx.min.js");
	Reply first = get(fx, url);
	const gchar *etag = header(&first, "ETag");
	const gchar *const match[] = { "If-None-Match", etag, NULL };
	const gchar *const other[] = { "If-None-Match", "\"not-this-one\"", NULL };
	Reply hit;
	Reply miss;

	g_assert_nonnull(etag);
	hit = request(fx, "GET", url, match);
	g_assert_cmpuint(hit.status, ==, 304);
	g_assert_cmpstr(hit.body, ==, "");
	g_assert_cmpstr(header(&hit, "ETag"), ==, etag);
	assert_security_headers(&hit);

	miss = request(fx, "GET", url, other);
	g_assert_cmpuint(miss.status, ==, 200);
	g_assert_cmpuint(strlen(miss.body), >, 1000);

	reply_clear(&first);
	reply_clear(&hit);
	reply_clear(&miss);
}

static void
test_static_stale_version(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *current = axis_web_asset_url("vendor/htmx.min.js");
	const gchar *stale_version = "000000000000";
	g_autofree gchar *stale = NULL;
	Reply reply;

	g_assert_cmpstr(axis_web_asset_version(), !=, stale_version);
	stale = g_strdup_printf("/static/%s/vendor/htmx.min.js", stale_version);
	reply = get(fx, stale);

	g_assert_true(reply.status == 301 || reply.status == 302 || reply.status == 307 ||
	              reply.status == 308);
	g_assert_cmpstr(header(&reply, "Location"), ==, current);
	assert_security_headers(&reply);
	reply_clear(&reply);

	/* A stale version of an asset that does not exist is not redirected. */
	g_free(stale);
	stale = g_strdup_printf("/static/%s/nothing.js", stale_version);
	reply = get(fx, stale);
	g_assert_cmpuint(reply.status, ==, 404);
	reply_clear(&reply);
}

static void
test_static_unknown_and_traversal(Fixture *fx, gconstpointer data)
{
	const gchar *version = axis_web_asset_version();
	const gchar *const tails[] = {
		"nothing.js",
		"vendor/nothing.js",
		"vendor/",
		"vendor",
		"",
		"../../../../etc/passwd",
		"vendor/../../../../../etc/passwd",
		"..%2f..%2f..%2fetc%2fpasswd",
		"%2e%2e/%2e%2e/%2e%2e/etc/passwd",
		"%2e%2e%2f%2e%2e%2fetc%2fpasswd",
		"%252e%252e%252f%252e%252e%252fetc%252fpasswd",
		"%252e%252e/%252e%252e/etc/passwd",
		"%25252e%25252e%25252fetc%25252fpasswd",
		"vendor/..%5c..%5cetc%5cpasswd",
		"..\\..\\etc\\passwd",
		".%2e/.%2e/etc/passwd",
		"%c0%ae%c0%ae/%c0%ae%c0%ae/etc/passwd",
		"vendor//htmx.min.js",
		".hidden",
		"axis.css/",
		"axis.css%00.js",
		NULL
	};
	guint i;

	for (i = 0; tails[i] != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/static/%s/%s", version, tails[i]);
		Reply reply = get(fx, path);

		/*
		 * libsoup itself answers 400 to an encoded '/' or NUL in a path,
		 * before the shell sees the request (so those responses carry no
		 * shell headers).  Everything else reaches the shell and is a 404.
		 */
		if (strstr(tails[i], "%2f") != NULL || strstr(tails[i], "%00") != NULL) {
			if (reply.status != 400 && reply.status != 404)
				g_error("%s: expected 400 or 404, got %u", path, reply.status);
		} else {
			if (reply.status != 404)
				g_error("%s: expected 404, got %u", path, reply.status);
			assert_security_headers(&reply);
		}
		assert_lacks(reply.body, "root:");
		reply_clear(&reply);
	}

	/* The same attempts under no valid version at all. */
	{
		Reply reply = get(fx, "/static/../../etc/passwd");

		g_assert_cmpuint(reply.status, ==, 404);
		assert_lacks(reply.body, "root:");
		reply_clear(&reply);
	}
	{
		Reply reply = get(fx, "/static/");

		g_assert_cmpuint(reply.status, ==, 404);
		reply_clear(&reply);
	}
}

/* ── Security headers ───────────────────────────────────────────── */

static void
test_security_headers_pages(Fixture *fx, gconstpointer data)
{
	const gchar *const htmx[] = { "HX-Request", "true", NULL };
	Reply page = get(fx, "/movies");
	Reply fragment = request(fx, "GET", "/movies", htmx);
	Reply home = get(fx, "/");

	assert_security_headers(&page);
	assert_no_store(&page);
	assert_security_headers(&fragment);
	assert_no_store(&fragment);
	assert_security_headers(&home);
	assert_no_store(&home);

	reply_clear(&page);
	reply_clear(&fragment);
	reply_clear(&home);
}

static void
test_security_headers_errors_and_assets(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *url = axis_web_asset_url("axis.css");
	Reply missing = get(fx, "/missing");
	Reply wrong_method = request(fx, "POST", "/", NULL);
	Reply asset = get(fx, url);

	g_assert_cmpuint(missing.status, ==, 404);
	assert_security_headers(&missing);
	assert_no_store(&missing);

	g_assert_cmpuint(wrong_method.status, ==, 405);
	assert_security_headers(&wrong_method);
	assert_no_store(&wrong_method);

	g_assert_cmpuint(asset.status, ==, 200);
	assert_security_headers(&asset);
	/* The asset keeps its own cache policy. */
	g_assert_cmpstr(header(&asset, "Cache-Control"), !=, "no-store");
	assert_contains(header(&asset, "Cache-Control"), "immutable");

	reply_clear(&missing);
	reply_clear(&wrong_method);
	reply_clear(&asset);
}

/*
 * Spec (web-shell, add-phone-layout): every tap target is 44 px each way.
 * The demo screenshots measure it on real pages; this holds the rules that
 * do it, in the phones section of the stylesheet.
 */
static void
test_phone_tap_target_rules(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *url = axis_web_asset_url("axis.css");
	Reply asset = get(fx, url);
	const gchar *phones = strstr(asset.body, "Phones (add-phone-layout)");

	g_assert_cmpuint(asset.status, ==, 200);
	g_assert_nonnull(phones);
	/* A box beside its label, a link in a sentence or a cell, a bar chart's thin rows. */
	assert_contains(phones, ".field-check > input[type=\"checkbox\"]");
	assert_contains(phones, "a:where(:not(.button, .mark-link, .quick-add-options))");
	assert_contains(phones, ".chart:not(.chart-growth)");
	reply_clear(&asset);
}

/* ── Error pages ────────────────────────────────────────────────── */

static void
test_error_unknown_path(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/no/such/page");

	g_assert_cmpuint(reply.status, ==, 404);
	g_assert_true(g_str_has_prefix(reply.body, "<!DOCTYPE html>"));
	assert_contains(reply.body, "<nav");
	assert_contains(reply.body, "Not found");
	reply_clear(&reply);
}

static void
test_error_wrong_method(Fixture *fx, gconstpointer data)
{
	Reply reply = request(fx, "POST", "/", NULL);
	const gchar *allow;

	g_assert_cmpuint(reply.status, ==, 405);
	allow = header(&reply, "Allow");
	g_assert_nonnull(allow);
	assert_contains(allow, "GET");
	assert_lacks(allow, "POST");
	g_assert_true(g_str_has_prefix(reply.body, "<!DOCTYPE html>"));
	assert_contains(reply.body, "Not a page to open");
	reply_clear(&reply);

	/* A DELETE on a module page says the same. */
	reply = request(fx, "DELETE", "/movies", NULL);
	g_assert_cmpuint(reply.status, ==, 405);
	assert_contains(header(&reply, "Allow"), "GET");
	reply_clear(&reply);
}

static void
test_error_htmx_fragment(Fixture *fx, gconstpointer data)
{
	const gchar *const htmx[] = { "HX-Request", "true", NULL };
	Reply missing = request(fx, "GET", "/missing", htmx);
	Reply wrong = request(fx, "POST", "/", htmx);

	g_assert_cmpuint(missing.status, ==, 404);
	assert_lacks(missing.body, "<html");
	assert_contains(missing.body, "Not found");
	g_assert_cmpuint(wrong.status, ==, 405);
	assert_lacks(wrong.body, "<html");

	reply_clear(&missing);
	reply_clear(&wrong);
}

static void
test_error_internal_hides_details(Fixture *fx, gconstpointer data)
{
	Reply reply;

	g_test_expect_message("axis", G_LOG_LEVEL_WARNING, "*secret-detail: password=hunter2*");
	reply = get(fx, "/boom");
	g_test_assert_expected_messages();

	g_assert_cmpuint(reply.status, ==, 500);
	g_assert_true(g_str_has_prefix(reply.body, "<!DOCTYPE html>"));
	assert_contains(reply.body, "Something went wrong");
	assert_lacks(reply.body, "secret-detail");
	assert_lacks(reply.body, "hunter2");
	assert_security_headers(&reply);
	assert_no_store(&reply);
	reply_clear(&reply);
}

/* ── Dashboard ──────────────────────────────────────────────────── */

/* The footer of @body holds one link named "Source" to AXIS_SOURCE_URL (the
 * order of an element's attributes is not fixed). */
static void
assert_source_link(const gchar *body)
{
	const gchar *footer = strstr(body, "<footer class=\"site-footer\">");
	const gchar *mark;
	const gchar *open;
	const gchar *close;
	g_autofree gchar *tag = NULL;

	g_assert_nonnull(footer);
	mark = strstr(footer, "class=\"footer-source\"");
	g_assert_nonnull(mark);
	g_assert_null(strstr(mark + 1, "class=\"footer-source\""));
	open = g_strrstr_len(footer, mark - footer, "<a ");
	g_assert_nonnull(open);
	close = strchr(mark, '>');
	g_assert_nonnull(close);
	tag = g_strndup(open, close - open + 1);
	g_assert_null(strchr(tag + 1, '<'));
	assert_contains(tag, " href=\"" AXIS_SOURCE_URL "\"");
	assert_contains(tag, " rel=\"noreferrer noopener\"");
	g_assert_true(g_str_has_prefix(close, ">Source</a>"));
}

/* The daemon's internals are not on the dashboard: its version and build
 * are in the footer, its modules on the Modules page. */
static void
test_dashboard(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/");
	g_autofree gchar *footer = g_strdup_printf("Axis %s, build %s", AXIS_VERSION, AXIS_GIT_SHA);
	Reply login = get_anonymous(fx, "/login");

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<title>Dashboard - Axis</title>");
	assert_contains(reply.body, footer);
	assert_lacks(reply.body, "Movie library");
	assert_lacks(reply.body, "Series library");
	assert_lacks(reply.body, "<code>movies</code>");
	assert_lacks(reply.body, ">Modules</h2>");
	/* Someone not logged in is not told the build. */
	assert_lacks(login.body, ", build ");
	/* Everyone is offered the source, logged in or not: one link, to the
	 * address the build was given (SOURCE_URL), in the footer. */
	g_assert_true(g_str_has_prefix(AXIS_SOURCE_URL, "https://") || g_str_has_prefix(AXIS_SOURCE_URL, "http://"));
	assert_source_link(reply.body);
	assert_source_link(login.body);
	assert_no_inline_code(reply.body);
	reply_clear(&reply);
	reply_clear(&login);
}

/* Spec: Acquisition not present; Nothing needs attention. */
static void
test_dashboard_without_modules(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/");

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "id=\"dash-attention\"");
	assert_contains(reply.body, "Nothing needs attention.");
	assert_lacks(reply.body, "attention-list");
	/* No card for what does not exist: not even an empty one. */
	assert_lacks(reply.body, "Coming into the library");
	assert_lacks(reply.body, "dash-coming-in");
	assert_lacks(reply.body, "dash-space");
	assert_lacks(reply.body, "dash-now");
	assert_lacks(reply.body, "dash-savings");
	reply_clear(&reply);
}

/* Spec: Something needs attention.  A part's pieces and items, only the
 * items that are not zero, in their order; none once its module is off. */
static void
test_dashboard_parts(Fixture *fx, gconstpointer data)
{
	gint before = g_atomic_int_get(&admin_collects);
	Reply reply;
	const gchar *away;
	const gchar *wrong;
	GError *error = NULL;

	/* The card of what needs attention is the board's: nothing is on it
	 * until the board has been read. */
	axis_web_attention_start(fx->web, NULL);
	while (axis_web_attention_get_readings(fx->web) == 0)
		g_main_context_iteration(NULL, TRUE);
	reply = get(fx, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	/* What the part read on its thread; an administrators' part, read. */
	assert_contains(reply.body, "<p>forty-two</p>");
	assert_contains(reply.body, "an administrators&#39; matter");
	g_assert_cmpint(g_atomic_int_get(&admin_collects), ==, before + 1);
	assert_contains(reply.body, "id=\"dash-space\"");
	assert_contains(reply.body, "<a href=\"/movies\">Disk space</a>");
	assert_lacks(reply.body, "dash-coming-in");
	assert_contains(reply.body, "id=\"dash-attention\"");
	assert_contains(reply.body, "3 things are wrong.");
	assert_lacks(reply.body, "Nothing needs attention.");
	/* Names are text inside <bdi>, escaped. */
	assert_contains(reply.body, "<bdi>Disk &lt;one&gt;</bdi>");
	/* The most serious first, each with its level in words, linking to
	 * where it is dealt with; and the page of the same name behind it. */
	away = strstr(reply.body, "The volume ");
	wrong = strstr(reply.body, "3 things are wrong.");
	g_assert_true(away != NULL && wrong != NULL && away < wrong);
	assert_contains(reply.body, "attention-level\">Not working</span><a href=\"/movies\">");
	assert_contains(reply.body, "attention-level\">Waiting for you</span>");
	assert_contains(reply.body, "<a href=\"/attention\">Needs attention</a>");
	assert_contains(reply.body, "<a href=\"/attention\">Open the Needs attention page</a>");
	assert_no_inline_code(reply.body);
	reply_clear(&reply);

	g_assert_true(axis_module_registry_set_enabled(fx->registry, "board", FALSE, &error));
	g_assert_no_error(error);
	reply = get(fx, "/");
	assert_lacks(reply.body, "forty-two");
	assert_lacks(reply.body, "dash-space");
	assert_contains(reply.body, "Nothing needs attention.");
	reply_clear(&reply);
}

/* add-builtin-torrent-indexers, review item 9: a part only administrators
 * are shown is not even read for anyone else's dashboard. */
static void
test_dashboard_parts_for_admins(Fixture *fx, gconstpointer data)
{
	gint before = g_atomic_int_get(&admin_collects);
	Reply reply = get(fx, "/");

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<p>forty-two</p>");
	assert_lacks(reply.body, "an administrators");
	g_assert_cmpint(g_atomic_int_get(&admin_collects), ==, before);
	reply_clear(&reply);
}

/* Runs the main loop until the dashboard's figures have been read more than
 * @before times. */
static void
pump_until_dashboard_read(
	Fixture *fx,
	guint    before
){
	gint64 until = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

	while (axis_web_dashboard_get_readings(fx->web) <= before) {
		g_assert_cmpint(g_get_monotonic_time(), <, until);
		g_main_context_iteration(NULL, FALSE);
		g_usleep(500);
	}
}

/* Runs the main loop until the test's part has been read once more than
 * @before times (its reading may be held on its thread). */
static void
pump_until_collected(gint before)
{
	gint64 until = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;

	while (g_atomic_int_get(&board_collects) == before) {
		g_assert_cmpint(g_get_monotonic_time(), <, until);
		g_main_context_iteration(NULL, FALSE);
		g_usleep(500);
	}
}

/*
 * speed-up-lists-and-dashboard: a started dashboard is drawn from memory.  A
 * view never runs a part's collect function and never waits for one: a part
 * not read yet says "Counting..." in its card; what a part last gave is shown
 * until it is read again; the figures are read again when the shell is told
 * that something happened and when a dashboard is opened, and never by the
 * cards an open dashboard asks for; an administrators' part is read for the
 * board and shown to them only; a stop with a reading on its thread ends.
 */
static void
test_dashboard_started(Fixture *fx, gconstpointer data)
{
	Reply reply;
	guint readings;
	gint collects;
	gint64 began;

	g_atomic_int_set(&board_figure, 42);
	/* The first reading is held on its thread. */
	g_atomic_int_set(&board_held, 1);
	collects = g_atomic_int_get(&board_collects);
	axis_web_dashboard_set_tuning(fx->web, 20, 3600 * 1000);
	axis_web_dashboard_start(fx->web);
	pump_until_collected(collects);

	/* Answered all the same, at once, saying what it does not know yet. */
	began = g_get_monotonic_time();
	reply = get(fx, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpint((g_get_monotonic_time() - began) / 1000, <, 5000);
	assert_contains(reply.body, "<title>Dashboard - Axis</title>");
	assert_contains(reply.body, "id=\"dashboard-cards\"");
	assert_contains(reply.body, "id=\"dash-space\"");
	assert_contains(reply.body, "<p class=\"help dash-counting\">Counting...</p>");
	assert_lacks(reply.body, "forty-two");
	assert_lacks(reply.body, "an administrators");
	assert_no_inline_code(reply.body);
	reply_clear(&reply);
	g_assert_cmpuint(axis_web_dashboard_get_readings(fx->web), ==, 0);

	/* Read: what the parts gave, and no view reads them.  (The view above
	 * asked for one more reading, as a view does: it is waited for first.) */
	g_atomic_int_set(&board_held, 0);
	pump_until_dashboard_read(fx, 0);
	pump_until_dashboard_read(fx, 1);
	collects = g_atomic_int_get(&board_collects);
	readings = axis_web_dashboard_get_readings(fx->web);
	reply = get(fx, "/?part=cards");
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<div id=\"dashboard-cards\""));
	assert_contains(reply.body, "<p>forty-two</p>");
	assert_contains(reply.body, "an administrators&#39; matter");
	assert_lacks(reply.body, "Counting...");
	assert_lacks(reply.body, "<title>");
	reply_clear(&reply);
	reply = get(fx, "/?part=cards");
	reply_clear(&reply);
	/* The cards asked for again read nothing and ask for no reading. */
	g_usleep(100 * 1000);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_cmpint(g_atomic_int_get(&board_collects), ==, collects);
	g_assert_cmpuint(axis_web_dashboard_get_readings(fx->web), ==, readings);

	/* Another user's dashboard: the same figures, nothing of the
	 * administrators' part. */
	reply = request_with(fx, "GET", "/?part=cards", fx->other_cookie, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<p>forty-two</p>");
	assert_lacks(reply.body, "an administrators");
	reply_clear(&reply);

	/* Something happened: read again in the background, and a view shows
	 * what was last read whatever is being read. */
	g_atomic_int_set(&board_figure, 43);
	g_atomic_int_set(&board_held, 1);
	collects = g_atomic_int_get(&board_collects);
	axis_web_attention_changed(fx->web);
	pump_until_collected(collects);
	reply = get(fx, "/?part=cards");
	assert_contains(reply.body, "<p>forty-two</p>");
	reply_clear(&reply);
	g_atomic_int_set(&board_held, 0);
	pump_until_dashboard_read(fx, readings);
	reply = get(fx, "/?part=cards");
	assert_contains(reply.body, "<p>forty-three</p>");
	assert_lacks(reply.body, "forty-two");
	reply_clear(&reply);

	/* Opening the dashboard asks for a reading; the page is what is known. */
	readings = axis_web_dashboard_get_readings(fx->web);
	g_atomic_int_set(&board_figure, 42);
	reply = get(fx, "/");
	assert_contains(reply.body, "<p>forty-three</p>");
	reply_clear(&reply);
	pump_until_dashboard_read(fx, readings);
	reply = get(fx, "/?part=cards");
	assert_contains(reply.body, "<p>forty-two</p>");
	reply_clear(&reply);

	/* A stop with a reading on its thread ends once the part returns, and
	 * what that reading gave is thrown away (the teardown frees the rest). */
	g_atomic_int_set(&board_held, 1);
	collects = g_atomic_int_get(&board_collects);
	readings = axis_web_dashboard_get_readings(fx->web);
	axis_web_attention_changed(fx->web);
	pump_until_collected(collects);
	g_atomic_int_set(&board_held, 0);
	axis_web_stop(fx->web);
	g_assert_cmpuint(axis_web_dashboard_get_readings(fx->web), ==, readings);
}

/* ── Tables ─────────────────────────────────────────────────────── */

/* The <colgroup> of the @n-th table of @html (from 0), or NULL. */
static gchar *
nth_colgroup(
	const gchar *html,
	guint        n
){
	const gchar *p = html;
	const gchar *start;
	const gchar *end;
	guint i;

	for (i = 0; i <= n; i++) {
		p = strstr(p, "<colgroup>");
		if (p == NULL)
			return NULL;
		if (i < n)
			p++;
	}
	start = p;
	end = strstr(start, "</colgroup>");
	g_assert_nonnull(end);
	return g_strndup(start, end - start);
}

/* Spec: Two tables of one kind; Numbers; Every table has a kind. */
static void
test_tables_line_up(Fixture *fx, gconstpointer data)
{
	Reply reply = get(fx, "/tables");
	g_autofree gchar *first = nth_colgroup(reply.body, 0);
	g_autofree gchar *second = nth_colgroup(reply.body, 1);
	g_autofree gchar *third = nth_colgroup(reply.body, 2);

	g_assert_cmpuint(reply.status, ==, 200);
	/* The same columns, by role, whatever the tables hold. */
	g_assert_nonnull(first);
	g_assert_nonnull(third);
	g_assert_cmpstr(first, ==, second);
	g_assert_cmpstr(first, ==, "<colgroup><col class=\"col-name\" /><col class=\"col-count\" />"
		"<col class=\"col-size\" /><col class=\"col-date\" /><col class=\"col-actions\" />");
	g_assert_cmpstr(first, !=, third);
	g_assert_cmpuint(count_of(reply.body, "class=\"data-table kind-things min-"), ==, 2);
	g_assert_cmpuint(count_of(reply.body, "class=\"data-table kind-others min-"), ==, 1);
	/* Every cell and heading has its column's role: numbers and their
	 * headings are right-aligned by it. */
	assert_contains(reply.body, "<th class=\"col-count\" scope=\"col\">Files</th>");
	assert_contains(reply.body, "<th class=\"col-size\" scope=\"col\">Size</th>");
	assert_contains(reply.body, "<td class=\"col-count\" data-label=\"Files\">7</td>");
	assert_contains(reply.body, "<td class=\"col-count\" data-label=\"Files\">1007</td>");
	assert_contains(reply.body, "<td class=\"col-size\" data-label=\"Size\">1.0 GiB</td>");
	assert_contains(reply.body, "<td class=\"col-name\" data-label=\"Name\">a much longer name that has to wrap "
		"somewhere</td>");
	axis_test_web_assert_tables(reply.body);
	reply_clear(&reply);
}

/*
 * Spec (web-shell, add-phone-layout): A table's rows stacked on a phone.
 * Every body cell of a column with a heading carries the heading as its
 * label, once; a column whose heading is empty, and a cell across columns,
 * carry none; a heading cell gets none; a lone row (a live update) gets its
 * labels from its columns.
 */
static void
test_table_cells_carry_labels(void)
{
	static const AxisWebColumn columns[] = {
		{ "Download", AXIS_WEB_COL_NAME },
		{ "Progress", AXIS_WEB_COL_WIDE },
		{ "State", AXIS_WEB_COL_STATE },
		{ "", AXIS_WEB_COL_ACTIONS },
		{ NULL, 0 }
	};
	g_autoptr(HtmxNode) table = axis_web_table_new("labels", columns);
	g_autoptr(HtmxTbody) body = htmx_tbody_new();
	g_autoptr(HtmxTr) row = htmx_tr_new();
	g_autoptr(HtmxTd) name = htmx_td_new_with_text("A made-up film");
	g_autoptr(HtmxTd) progress = htmx_td_new_with_text("37%");
	g_autoptr(HtmxTd) state = htmx_td_new_with_text("Downloading");
	g_autoptr(HtmxTd) actions = htmx_td_new_with_text("Remove");
	g_autoptr(HtmxTr) wide = htmx_tr_new();
	g_autoptr(HtmxTd) span = htmx_td_new_with_text("Nothing more");
	g_autoptr(HtmxTr) lone = htmx_tr_new();
	g_autoptr(HtmxTd) lone_name = htmx_td_new_with_text("Another made-up film");
	g_autoptr(HtmxTd) lone_progress = htmx_td_new_with_text("82%");
	g_autofree gchar *markup = NULL;
	g_autofree gchar *lone_markup = NULL;

	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(name));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(progress));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(state));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
	htmx_element_set_attribute(HTMX_ELEMENT(span), "colspan", "4");
	htmx_node_add_child(HTMX_NODE(wide), HTMX_NODE(span));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(wide));
	htmx_node_add_child(table, HTMX_NODE(body));

	axis_web_table_apply_roles(table);
	axis_web_table_apply_roles(table);
	markup = htmx_element_render(HTMX_ELEMENT(table));
	assert_contains(markup, "<td class=\"col-name\" data-label=\"Download\">A made-up film</td>");
	assert_contains(markup, "<td class=\"col-wide\" data-label=\"Progress\">37%</td>");
	assert_contains(markup, "<td class=\"col-state\" data-label=\"State\">Downloading</td>");
	assert_contains(markup, "<td class=\"col-actions\">Remove</td>");
	assert_contains(markup, "<td colspan=\"4\">Nothing more</td>");
	assert_contains(markup, "<th class=\"col-wide\" scope=\"col\">Progress</th>");
	/* An empty heading still has a name for a screen reader (tidy-labels). */
	assert_contains(markup, "aria-label=\"Actions\"");
	g_assert_cmpuint(count_of(markup, "data-label="), ==, 3);

	htmx_node_add_child(HTMX_NODE(lone), HTMX_NODE(lone_name));
	htmx_node_add_child(HTMX_NODE(lone), HTMX_NODE(lone_progress));
	axis_web_table_row_roles(HTMX_NODE(lone), columns);
	lone_markup = htmx_element_render(HTMX_ELEMENT(lone));
	g_assert_cmpstr(lone_markup, ==, "<tr><td class=\"col-name\" data-label=\"Download\">Another made-up film</td>"
		"<td class=\"col-wide\" data-label=\"Progress\">82%</td></tr>");
}

/* The roles are given once, however often the page is rendered. */
static void
test_table_roles_once(void)
{
	static const AxisWebColumn columns[] = {
		{ "A", AXIS_WEB_COL_NAME },
		{ "B", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	g_autoptr(HtmxNode) table = axis_web_table_new("once", columns);
	g_autoptr(HtmxTbody) body = htmx_tbody_new();
	g_autoptr(HtmxTr) row = htmx_tr_new();
	g_autoptr(HtmxTd) a = htmx_td_new_with_text("x");
	g_autoptr(HtmxTd) b = htmx_td_new_with_text("1");
	g_autoptr(HtmxTd) span = htmx_td_new_with_text("both");
	g_autoptr(HtmxTr) wide = htmx_tr_new();
	g_autofree gchar *markup = NULL;

	/* The parent takes its own reference; g_autoptr drops ours. */
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(a));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(b));
	htmx_element_set_attribute(HTMX_ELEMENT(span), "colspan", "2");
	htmx_node_add_child(HTMX_NODE(wide), HTMX_NODE(span));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(wide));
	htmx_node_add_child(table, HTMX_NODE(body));

	axis_web_table_apply_roles(table);
	axis_web_table_apply_roles(table);
	markup = htmx_element_render(HTMX_ELEMENT(table));
	assert_contains(markup, "<td class=\"col-name\" data-label=\"A\">x</td><td class=\"col-count\" data-label=\"B\">1</td>");
	assert_lacks(markup, "data-label=\"A\" data-label");
	/* A cell across columns has no one role. */
	assert_contains(markup, "<td colspan=\"2\">both</td>");
	assert_lacks(markup, "col-name col-name");
}

/* Spec (web-shell): A cell's content never overlaps the next column.  A
 * figure long enough to be cut in a wide face carries itself whole in its
 * title; a short one, which always fits, does not; a title already given is
 * kept. */
static void
test_table_cut_values_have_titles(void)
{
	static const AxisWebColumn columns[] = {
		{ "Name", AXIS_WEB_COL_NAME },
		{ "When", AXIS_WEB_COL_DATE },
		{ "Size", AXIS_WEB_COL_SIZE },
		{ "Files", AXIS_WEB_COL_COUNT },
		{ NULL, 0 }
	};
	g_autoptr(HtmxNode) table = axis_web_table_new("cut", columns);
	g_autoptr(HtmxTbody) body = htmx_tbody_new();
	g_autoptr(HtmxTr) row = htmx_tr_new();
	g_autoptr(HtmxTr) other = htmx_tr_new();
	g_autoptr(HtmxTd) name = htmx_td_new_with_text("a film");
	g_autoptr(HtmxTd) when = htmx_td_new();
	g_autoptr(HtmxTd) size = htmx_td_new_with_text("1023.9\302\240GiB");
	g_autoptr(HtmxTd) files = htmx_td_new_with_text("1234567");
	g_autoptr(HtmxTd) name2 = htmx_td_new_with_text("another");
	g_autoptr(HtmxTd) when2 = htmx_td_new_with_text("2026-11-15 00:10:10 UTC");
	g_autoptr(HtmxTd) size2 = htmx_td_new_with_text("26 bytes");
	g_autoptr(HtmxTd) files2 = htmx_td_new_with_text("5");
	g_autoptr(HtmxNode) time = HTMX_NODE(htmx_time_new_with_datetime("2026-11-15 00:10:10 UTC",
		"2026-11-15T00:10:10Z"));
	g_autofree gchar *markup = NULL;

	htmx_node_add_child(HTMX_NODE(when), time);
	htmx_element_set_attribute(HTMX_ELEMENT(size2), "title", "26 bytes exactly");
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(name));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(when));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(size));
	htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(files));
	htmx_node_add_child(HTMX_NODE(other), HTMX_NODE(name2));
	htmx_node_add_child(HTMX_NODE(other), HTMX_NODE(when2));
	htmx_node_add_child(HTMX_NODE(other), HTMX_NODE(size2));
	htmx_node_add_child(HTMX_NODE(other), HTMX_NODE(files2));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(other));
	htmx_node_add_child(table, HTMX_NODE(body));

	axis_web_table_apply_roles(table);
	markup = htmx_element_render(HTMX_ELEMENT(table));
	/* A date, as text or in its <time>; a long size. */
	assert_contains(markup, "<td class=\"col-date\" title=\"2026-11-15 00:10:10 UTC\" data-label=\"When\"><time");
	assert_contains(markup, "<td class=\"col-date\" title=\"2026-11-15 00:10:10 UTC\" data-label=\"When\">2026-11-15");
	assert_contains(markup, "<td class=\"col-size\" title=\"1023.9\302\240GiB\" data-label=\"Size\">");
	/* Short ones fit in any face; a title given is kept. */
	assert_contains(markup, "<td class=\"col-count\" data-label=\"Files\">1234567</td>");
	assert_contains(markup, "<td class=\"col-count\" data-label=\"Files\">5</td>");
	assert_contains(markup, "<td class=\"col-size\" title=\"26 bytes exactly\" data-label=\"Size\">26 bytes</td>");
	assert_contains(markup, "<td class=\"col-name\" data-label=\"Name\">a film</td>");
}

/* ── Asset version and paths ────────────────────────────────────── */

static void
test_asset_version_shape(void)
{
	const gchar *version = axis_web_asset_version();
	g_autofree gchar *url = axis_web_asset_url("axis.css");
	g_autofree gchar *parsed_version = NULL;
	g_autofree gchar *parsed_asset = NULL;
	guint i;

	g_assert_cmpuint(strlen(version), ==, AXIS_WEB_VERSION_LEN);
	for (i = 0; version[i] != '\0'; i++)
		g_assert_true(g_ascii_isxdigit(version[i]) && !g_ascii_isupper(version[i]));
	g_assert_true(axis_web_static_path_parse(url, &parsed_version, &parsed_asset));
	g_assert_cmpstr(parsed_version, ==, version);
	g_assert_cmpstr(parsed_asset, ==, "axis.css");
}

static void
test_path_parser(void)
{
	g_autofree gchar *version = NULL;
	g_autofree gchar *asset = NULL;

	g_assert_true(axis_web_static_path_parse("/static/0123456789ab/a/b.c-d_e.js", &version, &asset));
	g_assert_cmpstr(version, ==, "0123456789ab");
	g_assert_cmpstr(asset, ==, "a/b.c-d_e.js");

	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789AB/a.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789a/a.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789abc/a.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/../a.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/./a.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/a//b.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/a/", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/a%2eb", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/a\\b", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/a\tb", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/static/0123456789ab/a/b/c/d/e/f/g.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("/other/0123456789ab/a.js", NULL, NULL));
	g_assert_false(axis_web_static_path_parse("static/0123456789ab/a.js", NULL, NULL));
}

/* ── Default deny, roles and CSRF ───────────────────────────────── */

/* The CSRF token a page hands htmx: from the hx-headers of its body. */
static gchar *
page_csrf_token(const gchar *html)
{
	const gchar *marker = "X-CSRF-Token&quot;:&quot;";
	const gchar *start = strstr(html, marker);
	const gchar *end;

	if (start == NULL)
		return NULL;
	start += strlen(marker);
	end = strstr(start, "&quot;");
	g_assert_nonnull(end);

	return g_strndup(start, end - start);
}

static gchar *
csrf_for(Fixture *fx, const gchar *cookie)
{
	Reply page = request_with(fx, "GET", "/", cookie, NULL);
	gchar *token = page_csrf_token(page.body);

	g_assert_cmpuint(page.status, ==, 200);
	g_assert_nonnull(token);
	reply_clear(&page);

	return token;
}

/* Spec (web-auth): Unauthenticated page request */
static void
test_deny_page(Fixture *fx, gconstpointer data)
{
	static const struct
	{
		const gchar *path;
		const gchar *location;
	} cases[] = {
		{ "/movies", "/login?next=%2Fmovies" },
		{ "/movies?sort=size&dir=a", "/login?next=%2Fmovies%3Fsort%3Dsize%26dir%3Da" },
		{ "/", "/login" },
		/* An unknown path is not told apart from a known one. */
		{ "/no/such/page", "/login?next=%2Fno%2Fsuch%2Fpage" },
		{ "/account", "/login?next=%2Faccount" },
		{ "/admin/users", "/login?next=%2Fadmin%2Fusers" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		Reply reply = get_anonymous(fx, cases[i].path);

		if (reply.status != 303)
			g_error("%s: expected 303, got %u", cases[i].path, reply.status);
		g_assert_cmpstr(header(&reply, "Location"), ==, cases[i].location);
		assert_lacks(reply.body, "Movie list");
		assert_security_headers(&reply);
		assert_no_store(&reply);
		reply_clear(&reply);
	}

	/* A cookie that is not a session is no session. */
	{
		Reply reply = request_with(fx, "GET", "/movies",
			"axis_session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", NULL);

		g_assert_cmpuint(reply.status, ==, 303);
		reply_clear(&reply);
	}
}

/* Spec (web-auth): Unauthenticated htmx request */
static void
test_deny_htmx(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *current = g_strdup_printf("http://127.0.0.1:%u/movies?x=1", (guint)fx->port);
	const gchar *const extra[] = { "HX-Request", "true", "HX-Current-URL", current, NULL };
	const gchar *const hostile[] = { "HX-Request", "true", "HX-Current-URL", "https://evil.example//evil.example/", NULL };
	Reply reply = request_with(fx, "GET", "/movies/fragment", NULL, extra);
	Reply other = request_with(fx, "POST", "/movies/poke", NULL, hostile);

	g_assert_cmpuint(reply.status, ==, 401);
	g_assert_cmpstr(header(&reply, "HX-Redirect"), ==, "/login?next=%2Fmovies%3Fx%3D1");
	assert_lacks(reply.body, "<nav");
	assert_lacks(reply.body, "<html");
	assert_security_headers(&reply);

	/* The page htmx reports is only taken as a local path. */
	g_assert_cmpuint(other.status, ==, 401);
	g_assert_cmpstr(header(&other, "HX-Redirect"), ==, "/login");
	g_assert_cmpuint(pokes, ==, 0);

	reply_clear(&reply);
	reply_clear(&other);
}

static HtmxResponse *
page_raw(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	pokes++;
	return htmx_response_new_with_content("raw");
}

/* Spec (web-auth): New routes are protected without opting in */
static void
test_deny_routes_without_opting_in(Fixture *fx, gconstpointer data)
{
	Reply post = request_with(fx, "POST", "/movies/poke", NULL, NULL);
	Reply escape = get_anonymous(fx, "/escape");
	Reply raw;
	Reply raw_post;

	/* The movies module registered these and nothing else. */
	g_assert_cmpuint(post.status, ==, 303);
	g_assert_cmpstr(header(&post, "Location"), ==, "/login");
	g_assert_cmpuint(escape.status, ==, 303);
	g_assert_cmpuint(pokes, ==, 0);

	/* Even a route added to the router behind the shell's back. */
	htmx_router_add_route(htmx_server_get_router(fx->server), HTMX_METHOD_GET, "/raw", page_raw, NULL);
	htmx_router_add_route(htmx_server_get_router(fx->server), HTMX_METHOD_POST, "/raw", page_raw, NULL);
	raw = get_anonymous(fx, "/raw");
	g_assert_cmpuint(raw.status, ==, 303);
	g_assert_cmpuint(pokes, ==, 0);
	reply_clear(&raw);
	raw = get(fx, "/raw");
	g_assert_cmpuint(raw.status, ==, 200);
	g_assert_cmpuint(pokes, ==, 1);
	/* ... and its POST still needs the CSRF token. */
	raw_post = request(fx, "POST", "/raw", NULL);
	g_assert_cmpuint(raw_post.status, ==, 403);
	g_assert_cmpuint(pokes, ==, 1);

	reply_clear(&post);
	reply_clear(&escape);
	reply_clear(&raw);
	reply_clear(&raw_post);
}

static void
test_public_paths(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *css = axis_web_asset_url("axis.css");
	Reply asset = get_anonymous(fx, css);
	Reply login = get_anonymous(fx, "/login");

	g_assert_cmpuint(asset.status, ==, 200);
	g_assert_cmpuint(login.status, ==, 200);
	assert_contains(login.body, "action=\"/login\"");
	/* Nothing of the inside is shown to someone not logged in. */
	assert_lacks(login.body, "<nav");
	assert_lacks(login.body, "href=\"/movies\"");
	assert_lacks(login.body, "hx-headers");
	assert_no_inline_code(login.body);

	reply_clear(&asset);
	reply_clear(&login);
}

static void
test_public_route_refused(Fixture *fx, gconstpointer data)
{
	Reply anonymous;
	Reply logged_in;

	g_test_expect_message("axis", G_LOG_LEVEL_CRITICAL,
		"*refusing to register '/movies/open' as a public route*");
	axis_web_route_full(fx->web, HTMX_METHOD_GET, "/movies/open", AXIS_WEB_ROUTE_PUBLIC,
		page_movies, NULL);
	g_test_assert_expected_messages();

	anonymous = get_anonymous(fx, "/movies/open");
	logged_in = get(fx, "/movies/open");
	g_assert_cmpuint(anonymous.status, ==, 303);
	g_assert_cmpuint(logged_in.status, ==, 404);

	reply_clear(&anonymous);
	reply_clear(&logged_in);
}

/* Spec (web-auth): Non-admin refused (route level) */
static void
test_admin_route_refused(Fixture *fx, gconstpointer data)
{
	Reply settings = get(fx, "/movies/settings");
	Reply users = get(fx, "/admin/users");
	Reply home = get(fx, "/");

	g_assert_cmpuint(settings.status, ==, 403);
	assert_lacks(settings.body, "Movie settings.");
	g_assert_cmpuint(users.status, ==, 403);
	assert_lacks(home.body, "href=\"/admin/users\"");

	reply_clear(&settings);
	reply_clear(&users);
	reply_clear(&home);
}

static void
test_admin_route_allowed(Fixture *fx, gconstpointer data)
{
	Reply settings = get(fx, "/movies/settings");
	Reply home = get(fx, "/");

	g_assert_cmpuint(settings.status, ==, 200);
	assert_contains(settings.body, "Movie settings.");
	assert_contains(home.body, "href=\"/admin/users\"");

	reply_clear(&settings);
	reply_clear(&home);
}

/* Spec (web-auth): Missing token, Token from another session, Cross-site
 * request, htmx requests carry the token */
static void
test_csrf(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = csrf_for(fx, fx->cookie);
	g_autofree gchar *other = csrf_for(fx, fx->other_cookie);
	g_autofree gchar *origin = g_strdup_printf("http://127.0.0.1:%u", (guint)fx->port);
	g_autofree gchar *field = g_strdup_printf("_csrf=%s", token);
	const gchar *const none[] = { "HX-Request", "true", NULL };
	const gchar *const wrong[] = { "HX-Request", "true", "X-CSRF-Token", other, NULL };
	const gchar *const cross_origin[] = { "X-CSRF-Token", token, "Origin", "http://evil.example", NULL };
	const gchar *const null_origin[] = { "X-CSRF-Token", token, "Origin", "null", NULL };
	const gchar *const cross_site[] = { "X-CSRF-Token", token, "Sec-Fetch-Site", "cross-site", NULL };
	const gchar *const same_site[] = { "X-CSRF-Token", token, "Sec-Fetch-Site", "same-site", NULL };
	const gchar *const htmx[] = { "HX-Request", "true", "X-CSRF-Token", token, NULL };
	const gchar *const browser[] = {
		"X-CSRF-Token", token, "Origin", origin, "Sec-Fetch-Site", "same-origin", NULL
	};
	Reply reply;

	g_assert_cmpstr(token, !=, other);

	reply = request(fx, "POST", "/movies/poke", none);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	reply = request(fx, "POST", "/movies/poke", wrong);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	reply = request(fx, "POST", "/movies/poke", cross_origin);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	reply = request(fx, "POST", "/movies/poke", null_origin);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	reply = request(fx, "POST", "/movies/poke", cross_site);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	reply = request(fx, "POST", "/movies/poke", same_site);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	reply = request_full(fx, "POST", "/movies/poke", fx->cookie, NULL, "_csrf=wrong");
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	g_assert_cmpuint(pokes, ==, 0);

	/* The token on the page is what htmx sends, and it is accepted. */
	reply = request(fx, "POST", "/movies/poke", htmx);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Poked.");
	reply_clear(&reply);
	g_assert_cmpuint(pokes, ==, 1);

	reply = request(fx, "POST", "/movies/poke", browser);
	g_assert_cmpuint(reply.status, ==, 200);
	reply_clear(&reply);
	reply = request_full(fx, "POST", "/movies/poke", fx->cookie, NULL, field);
	g_assert_cmpuint(reply.status, ==, 200);
	reply_clear(&reply);
	g_assert_cmpuint(pokes, ==, 3);

	/* Another session's own token works for that session only. */
	{
		const gchar *const theirs[] = { "X-CSRF-Token", other, NULL };

		reply = request_with(fx, "POST", "/movies/poke", fx->other_cookie, theirs);
		g_assert_cmpuint(reply.status, ==, 200);
		reply_clear(&reply);
	}
	g_assert_cmpuint(pokes, ==, 4);
}

/* A raw HTTP exchange on a thread, so the server can run on this one. */
typedef struct
{
	guint16    port;
	gchar     *request;
	gchar     *response;
	GMainLoop *loop;
} RawExchange;

static gboolean
raw_done(gpointer data)
{
	g_main_loop_quit(data);
	return G_SOURCE_REMOVE;
}

static gpointer
raw_thread(gpointer data)
{
	RawExchange *raw = data;
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GError) error = NULL;
	GString *response = g_string_new(NULL);
	gchar buffer[4096];
	gssize n;

	g_socket_client_set_timeout(client, 10);
	connection = g_socket_client_connect_to_host(client, "127.0.0.1", raw->port, NULL, &error);
	g_assert_no_error(error);
	g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),
		raw->request, strlen(raw->request), NULL, NULL, &error);
	g_assert_no_error(error);
	while ((n = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
	                                buffer, sizeof(buffer), NULL, NULL)) > 0) {
		g_string_append_len(response, buffer, n);
		if (strstr(response->str, "\r\n\r\n") != NULL)
			break;
	}

	raw->response = g_string_free(response, FALSE);
	g_idle_add(raw_done, raw->loop);
	return NULL;
}

static gchar *
raw_exchange(Fixture *fx, const gchar *request)
{
	RawExchange raw;
	GThread *thread;

	raw.port = fx->port;
	raw.request = (gchar *)request;
	raw.response = NULL;
	raw.loop = g_main_loop_new(NULL, FALSE);
	thread = g_thread_new("raw-client", raw_thread, &raw);
	g_main_loop_run(raw.loop);
	g_thread_join(thread);
	g_main_loop_unref(raw.loop);

	return raw.response;
}

static void
test_request_too_large(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = csrf_for(fx, fx->cookie);
	const gchar *const extra[] = { "X-CSRF-Token", token, NULL };
	gchar *body = g_malloc(AXIS_WEB_MAX_REQUEST_BODY + 2);
	Reply reply;

	/* Exactly the limit is fine ... */
	memset(body, 'a', AXIS_WEB_MAX_REQUEST_BODY);
	body[0] = 'x';
	body[1] = '=';
	body[AXIS_WEB_MAX_REQUEST_BODY] = '\0';
	reply = request_full(fx, "POST", "/movies/poke", fx->cookie, extra, body);
	g_assert_cmpuint(reply.status, ==, 200);
	reply_clear(&reply);
	g_assert_cmpuint(pokes, ==, 1);

	/* ... a byte more is not, and no handler runs. */
	body[AXIS_WEB_MAX_REQUEST_BODY] = 'a';
	body[AXIS_WEB_MAX_REQUEST_BODY + 1] = '\0';
	reply = request_full(fx, "POST", "/movies/poke", fx->cookie, extra, body);
	g_assert_cmpuint(reply.status, ==, 413);
	assert_security_headers(&reply);
	reply_clear(&reply);
	reply = request_full(fx, "POST", "/login", NULL, NULL, body);
	g_assert_cmpuint(reply.status, ==, 413);
	reply_clear(&reply);
	g_assert_cmpuint(pokes, ==, 1);
	g_free(body);

	/* A body of unknown length is refused before it is read. */
	{
		g_autofree gchar *request = g_strdup_printf(
			"POST /movies/poke HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nCookie: %s\r\n"
			"X-CSRF-Token: %s\r\nContent-Type: application/x-www-form-urlencoded\r\n"
			"Transfer-Encoding: chunked\r\n\r\n3\r\nx=1\r\n0\r\n\r\n",
			(guint)fx->port, fx->cookie, token);
		g_autofree gchar *response = raw_exchange(fx, request);

		g_assert_true(g_str_has_prefix(response, "HTTP/1.1 413"));
	}
	g_assert_cmpuint(pokes, ==, 1);
}

static void
test_layout_user_menu(Fixture *fx, gconstpointer data)
{
	Reply home = get(fx, "/");
	g_autofree gchar *token = page_csrf_token(home.body);
	g_autofree gchar *hidden = g_strdup_printf("value=\"%s\"", token);

	assert_contains(home.body, "action=\"/logout\"");
	assert_contains(home.body, "href=\"/account\"");
	assert_contains(home.body, ">tester</a>");
	/* The logout form carries the same token as the body. */
	assert_contains(home.body, hidden);
	assert_no_inline_code(home.body);

	reply_clear(&home);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/web/asset-version-shape", test_asset_version_shape);
	g_test_add_func("/web/path-parser", test_path_parser);

#define ADD(name, fn, flags) \
	g_test_add(name, Fixture, GUINT_TO_POINTER(flags), fixture_setup, fn, fixture_teardown)

	ADD("/web/layout/full-page", test_layout_full_page, WITH_MOVIES);
	ADD("/web/layout/htmx-fragment", test_layout_htmx_fragment, WITH_MOVIES);
	ADD("/web/layout/history-restore", test_layout_history_restore, WITH_MOVIES);

	ADD("/web/escaping", test_escaping, WITH_MOVIES);
	ADD("/web/no-inline-code", test_no_inline_code, WITH_MOVIES);

	ADD("/web/nav/entry-present", test_nav_entry_present, WITH_MOVIES);
	ADD("/web/nav/absent-module", test_nav_absent_module, WITH_SERIES);
	ADD("/web/nav/order", test_nav_order, WITH_MOVIES | WITH_SERIES);
	ADD("/web/nav/current-page", test_nav_current_page, WITH_MOVIES | WITH_SERIES);
	ADD("/web/nav/tab-order", test_nav_tab_order, WITH_MOVIES | WITH_SERIES | WITH_ALL);
	ADD("/web/nav/media-types", test_nav_media_types, WITH_MOVIES | WITH_SERIES);
	ADD("/web/nav/phone-markup", test_nav_phone_markup, WITH_MOVIES | WITH_SERIES);
	ADD("/web/nav/empty-tab", test_nav_empty_tab, WITH_MOVIES);
	ADD("/web/nav/single-page", test_nav_single_page, WITH_ALL);
	ADD("/web/nav/not-admin", test_nav_not_admin, WITH_MOVIES | WITH_SERIES | AS_USER);
	ADD("/web/nav/admin", test_nav_admin, WITH_MOVIES | WITH_SERIES);
	ADD("/web/nav/more-pages", test_nav_more_pages, WITH_ALL | WITH_EXTRA);

	ADD("/web/tables/line-up", test_tables_line_up, WITH_MOVIES);
	g_test_add_func("/web/tables/roles-once", test_table_roles_once);
	g_test_add_func("/web/tables/cells-carry-labels", test_table_cells_carry_labels);
	g_test_add_func("/web/tables/cut-values-have-titles", test_table_cut_values_have_titles);

	ADD("/web/static/asset", test_static_asset, 0);
	ADD("/web/static/url-matches-pages", test_static_url_matches_pages, 0);
	ADD("/web/static/conditional", test_static_conditional, 0);
	ADD("/web/static/stale-version", test_static_stale_version, 0);
	ADD("/web/static/unknown-and-traversal", test_static_unknown_and_traversal, 0);

	ADD("/web/security-headers/pages", test_security_headers_pages, WITH_MOVIES);
	ADD("/web/security-headers/errors-and-assets", test_security_headers_errors_and_assets, 0);
	ADD("/web/phone/tap-target-rules", test_phone_tap_target_rules, 0);

	ADD("/web/errors/unknown-path", test_error_unknown_path, 0);
	ADD("/web/errors/wrong-method", test_error_wrong_method, WITH_MOVIES);
	ADD("/web/errors/htmx-fragment", test_error_htmx_fragment, 0);
	ADD("/web/errors/internal-hides-details", test_error_internal_hides_details, WITH_MOVIES);

	ADD("/web/dashboard/no-internals", test_dashboard, WITH_MOVIES | WITH_SERIES);
	ADD("/web/dashboard/no-modules", test_dashboard_without_modules, 0);
	ADD("/web/dashboard/parts", test_dashboard_parts, WITH_BOARD);
	ADD("/web/dashboard/parts-for-admins", test_dashboard_parts_for_admins, WITH_BOARD | AS_USER);
	ADD("/web/dashboard/started", test_dashboard_started, WITH_BOARD);

	ADD("/web/auth/deny/page", test_deny_page, WITH_MOVIES);
	ADD("/web/auth/deny/htmx", test_deny_htmx, WITH_MOVIES);
	ADD("/web/auth/deny/routes-without-opting-in", test_deny_routes_without_opting_in, WITH_MOVIES);
	ADD("/web/auth/public-paths", test_public_paths, WITH_MOVIES);
	ADD("/web/auth/public-route-refused", test_public_route_refused, WITH_MOVIES);
	ADD("/web/auth/roles/non-admin-refused", test_admin_route_refused, WITH_MOVIES | AS_USER);
	ADD("/web/auth/roles/admin-allowed", test_admin_route_allowed, WITH_MOVIES);
	ADD("/web/auth/csrf", test_csrf, WITH_MOVIES);
	ADD("/web/auth/user-menu", test_layout_user_menu, WITH_MOVIES);
	ADD("/web/request-too-large", test_request_too_large, WITH_MOVIES);

	return g_test_run();
}
