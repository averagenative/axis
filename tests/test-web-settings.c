/*
 * test-web-settings.c - the Settings and Modules pages
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each test makes its own database, migrates it and runs the real shell,
 * the real Settings and Modules pages and a module registry with a few
 * test modules (one required, one that is switched on and off, one that
 * depends on it) on an ephemeral loopback port.  Sessions are opened in an
 * auth without a database.  Secrets are searched for in every page body and
 * in everything logged.
 */

#include <glib.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-web.h"
#include "core/axis-error.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "core/axis-outbound.h"
#include "core/axis-settings.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-theme.h"

#define SECRET "s3cr3t-provider-key-90210"

static const gchar *const qualities[] = { "sd", "hd", "uhd", NULL };
static const gchar *const scan_modes[] = { "fast", "slow", NULL };

static const AxisSettingInfo tv_settings[] = {
	{ "tv.title", AXIS_SETTING_STRING, "Display title", "Shown at the top of the TV pages.",
	  0, "My TV", 1, 40, 0, 0, NULL },
	{ "tv.api_key", AXIS_SETTING_STRING, "Provider API key", "Used to talk to the metadata provider.",
	  AXIS_SETTING_SECRET, NULL, 0, 64, 0, 0, NULL },
	{ "tv.retention", AXIS_SETTING_DURATION, "Retention", "How long finished downloads are kept.",
	  0, "7d", 3600, 365 * 86400, 0, 0, NULL },
	{ "tv.max_size", AXIS_SETTING_BYTE_SIZE, "Largest download", NULL,
	  0, "10GiB", 1 << 20, G_GINT64_CONSTANT(1) << 40, 0, 0, NULL },
	{ "tv.languages", AXIS_SETTING_STRING_LIST, "Languages", "One language code per line.",
	  0, NULL, 0, 5, 0, 0, NULL },
	{ "tv.quality", AXIS_SETTING_CHOICE, "Quality", NULL, 0, "hd", 0, 0, 0, 0, qualities },
	{ "tv.strict", AXIS_SETTING_BOOLEAN, "Strict matching", NULL, 0, "false", 0, 0, 0, 0, NULL },
	{ "tv.ratio", AXIS_SETTING_DECIMAL, "Minimum ratio", NULL, 0, "0.5", 0, 0, 0.0, 1.0, NULL },
	{ "tv.threads", AXIS_SETTING_INTEGER, "Scan threads", NULL, 0, "2", 1, 16, 0, 0, NULL },
	{ "tv.mode", AXIS_SETTING_CHOICE, "Scan mode", "How the library is scanned.",
	  AXIS_SETTING_RESTART, "fast", 0, 0, 0, 0, scan_modes },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

static const AxisSettingInfo subs_settings[] = {
	{ "subs.language", AXIS_SETTING_STRING, "Subtitle language", NULL, 0, "en", 1, 8, 0, 0, NULL },
	{ NULL, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0, NULL }
};

static const AxisNavEntry tv_nav[] = {
	{ "TVShows", "/tv", 60, 0, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};
static const AxisNavEntry subs_nav[] = {
	{ "Subtitles", "/subs", 61, 0, FALSE, NULL, 0 },
	{ NULL, NULL, 0, 0, FALSE, NULL, 0 }
};
static const gchar *const subs_needs[] = { "tv", NULL };

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisModuleRegistry *registry;
	HtmxServer         *server;
	AxisWebAuth        *auth;
	AxisWeb            *web;
	SoupSession        *session;
	GString            *events;	/* module hooks */
	guint16             port;
	gchar              *admin;
	gchar              *viewer;
} Fixture;

static gboolean
module_start(gpointer user_data, GError **error)
{
	Fixture *fx = user_data;

	g_string_append(fx->events, "start;");
	return TRUE;
}

static void
module_stop(gpointer user_data)
{
	Fixture *fx = user_data;

	g_string_append(fx->events, "stop;");
}

static HtmxResponse *
handle_tv(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
	g_autoptr(HtmxP) text = htmx_p_new();

	htmx_node_set_text_content(HTMX_NODE(text), "the tv page");
	return axis_web_page_respond(request, "TV", HTMX_NODE(text));
}

static void
tv_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_get(web, "/tv", handle_tv, user_data);
}

static void
subs_routes(AxisWeb *web, gpointer user_data)
{
	axis_web_get(web, "/subs", handle_tv, user_data);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisModuleInfo base;
	AxisModuleInfo tv;
	AxisModuleInfo subs;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	axis_web_theme_set_default(NULL);

	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 8);
	fx->events = g_string_new(NULL);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);

	memset(&base, 0, sizeof(base));
	base.id = "base";
	base.name = "Base";
	base.description = "The part that cannot be switched off.";
	base.flags = AXIS_MODULE_REQUIRED;
	memset(&tv, 0, sizeof(tv));
	tv.id = "tv";
	tv.name = "TV shows";
	tv.description = "Follows series.";
	tv.start = module_start;
	tv.stop = module_stop;
	tv.register_routes = tv_routes;
	tv.nav_entries = tv_nav;
	tv.settings = tv_settings;
	memset(&subs, 0, sizeof(subs));
	subs.id = "subs";
	subs.name = "Subtitles";
	subs.description = "Fetches subtitles for TV shows.";
	subs.register_routes = subs_routes;
	subs.nav_entries = subs_nav;
	subs.settings = subs_settings;
	subs.depends_on = subs_needs;

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, &base, fx, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(),
		fx->web_settings, &error));
	g_assert_true(axis_module_registry_register(fx->registry, &tv, fx, &error));
	g_assert_true(axis_module_registry_register(fx->registry, &subs, fx, &error));
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
	g_assert_no_error(error);
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);

	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 20, "idle-timeout", 20, NULL);
	g_string_truncate(fx->events, 0);
}

static void
fixture_teardown(Fixture *fx, gconstpointer data)
{
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	axis_web_theme_set_default(NULL);
	if (fx->db == NULL) {
		axis_test_log_reset();
		return;
	}

	g_object_unref(fx->session);
	htmx_server_stop(fx->server);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	g_string_free(fx->events, TRUE);
	g_free(fx->admin);
	g_free(fx->viewer);
	axis_test_log_reset();
}

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

/* ── Helpers ────────────────────────────────────────────────────── */

static AxisTestReply
get_as(Fixture *fx, const gchar *cookie, const gchar *path)
{
	return axis_test_web_request(fx->session, fx->port, "GET", path, cookie, NULL, NULL);
}

/* The CSRF token a page of this session carries. */
static gchar *
csrf_for(Fixture *fx, const gchar *cookie)
{
	AxisTestReply page = get_as(fx, cookie, "/");
	gchar *token = axis_test_web_csrf_token(page.body);

	g_assert_nonnull(token);
	axis_test_reply_clear(&page);

	return token;
}

/* "name=value&..." from name and value pairs ended by NULL, values escaped. */
static gchar *
form_of(const gchar *first, ...)
{
	GString *form = g_string_new(NULL);
	const gchar *name = first;
	va_list ap;

	va_start(ap, first);
	while (name != NULL) {
		const gchar *value = va_arg(ap, const gchar *);
		g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

		g_string_append_printf(form, "%s%s=%s", form->len > 0 ? "&" : "", name, escaped);
		name = va_arg(ap, const gchar *);
	}
	va_end(ap);

	return g_string_free(form, FALSE);
}

static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *csrf,
	const gchar *form
){
	const gchar *extra[4];
	guint n = 0;

	if (csrf != NULL) {
		extra[n++] = "X-CSRF-Token";
		extra[n++] = csrf;
	}
	extra[n] = NULL;

	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, extra, form);
}

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

/* How many recorded log messages, at any level, contain @text. */
static guint
log_mentions(const gchar *text)
{
	g_autofree gchar *pattern = g_strdup_printf("*%s*", text);
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING,
		G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_ERROR
	};
	guint count = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		count += axis_test_log_count(levels[i], pattern);

	return count;
}

/* The address and word, then (as the pages come back to a place) the place. */
static void
assert_redirect(AxisTestReply *reply, const gchar *location)
{
	const gchar *got = axis_test_reply_header(reply, "Location");

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(got);
	g_assert_true(g_str_has_prefix(got, location));
	g_assert_true(got[strlen(location)] == '\0' || g_str_has_prefix(got + strlen(location), "&at="));
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

static void
assert_no_inline_code(const gchar *html)
{
	assert_lacks(html, "style=\"");
	assert_lacks(html, "<style");
	assert_lacks(html, "onclick");
	assert_lacks(html, "javascript:");
}

static gchar *
count_of_settings(Fixture *fx)
{
	return axis_test_db_query(fx->db, "SELECT count(*) FROM setting");
}

/* ── Settings page ──────────────────────────────────────────────── */

/* Spec: Settings page; Default when unset */
static void
test_page_lists_declared_settings(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = get_as(fx, fx->admin, "/admin/settings");
	g_assert_cmpuint(reply.status, ==, 200);

	/* Grouped by module: a form and heading each, in registration order. */
	assert_contains(reply.body, "action=\"/admin/settings/web\"");
	assert_contains(reply.body, "action=\"/admin/settings/tv\"");
	assert_contains(reply.body, "action=\"/admin/settings/subs\"");
	assert_lacks(reply.body, "action=\"/admin/settings/base\"");
	g_assert_true(strstr(reply.body, "/admin/settings/web\"") < strstr(reply.body, "/admin/settings/tv\""));

	/* Label, help, a control named for the key, and the value in use. */
	assert_contains(reply.body, "Retention");
	assert_contains(reply.body, "How long finished downloads are kept.");
	assert_contains(reply.body, "name=\"tv.retention\"");
	assert_contains(reply.body, "value=\"7d\"");
	assert_contains(reply.body, "Default: 7d.");
	assert_contains(reply.body, "name=\"tv.max_size\"");
	assert_contains(reply.body, "value=\"10GiB\"");
	assert_contains(reply.body, "name=\"web.session_idle\"");
	assert_contains(reply.body, "value=\"30d\"");
	assert_contains(reply.body, "name=\"subs.language\"");
	assert_contains(reply.body, "value=\"en\"");

	/* Controls fit the types. */
	assert_contains(reply.body, "<select");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"hd\">");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"false\">");
	assert_contains(reply.body, "Default: Off.");
	assert_contains(reply.body, "<textarea");
	assert_contains(reply.body, "name=\"tv.languages\"");

	/* A restart setting says so. */
	assert_contains(reply.body, "A change takes effect after a restart.");

	/* Every form carries the token, and nothing is inline. */
	assert_contains(reply.body, "name=\"_csrf\"");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);
}

static void
test_markup_in_values_is_escaped(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	g_assert_true(axis_settings_set(fx->settings, "tv.title", "\"><script>alert(1)</script>", &error));
	g_assert_true(axis_settings_set(fx->settings, "tv.languages", "</textarea><b>x</b>\n\"q\" & 'z'", &error));
	g_assert_no_error(error);

	reply = get_as(fx, fx->admin, "/admin/settings");
	g_assert_cmpuint(reply.status, ==, 200);

	/* In an attribute, the quote is what could end it, and it is escaped. */
	assert_lacks(reply.body, "\"><script>");
	assert_contains(reply.body, "value=\"&quot;>");
	/* In the text of the textarea, every angle bracket is. */
	assert_lacks(reply.body, "<b>x</b>");
	assert_lacks(reply.body, "</textarea><b>");
	assert_contains(reply.body, "&lt;/textarea&gt;&lt;b&gt;x&lt;/b&gt;");
	assert_contains(reply.body, "&quot;q&quot; &amp; &#39;z&#39;");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);
}

/* Spec: Change recorded; Change visible */
static void
test_save_settings(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *title = NULL;
	g_auto(GStrv) languages = NULL;
	g_autofree gchar *line = NULL;
	AxisTestReply reply;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	form = form_of("tv.title", "Evening-Special", "tv.retention", "36h", "tv.max_size", "2GiB",
		"tv.languages", "en\r\nfr\r\n", "tv.quality", "uhd", "tv.strict", "true", "tv.ratio", "0.75",
		"tv.threads", "4", NULL);

	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);

	/* Visible to the next read, and in the table. */
	title = axis_settings_get_string(fx->settings, "tv.title");
	g_assert_cmpstr(title, ==, "Evening-Special");
	g_assert_cmpint(axis_settings_get_duration(fx->settings, "tv.retention"), ==, 36 * 3600);
	g_assert_cmpint(axis_settings_get_byte_size(fx->settings, "tv.max_size"), ==, G_GINT64_CONSTANT(2) << 30);
	languages = axis_settings_get_string_list(fx->settings, "tv.languages");
	g_assert_cmpuint(g_strv_length(languages), ==, 2);
	g_assert_true(axis_settings_get_boolean(fx->settings, "tv.strict"));
	g_assert_cmpint(axis_settings_get_integer(fx->settings, "tv.threads"), ==, 4);
	g_assert_true(axis_test_wait_query(fx->db, "SELECT count(*) FROM setting", "8", 2000));

	/* The page confirms and shows the new values, as typed back. */
	page = get_as(fx, fx->admin, "/admin/settings?done=saved");
	assert_contains(page.body, "The settings have been saved.");
	assert_contains(page.body, "value=\"1d 12h\"");
	assert_contains(page.body, "value=\"2GiB\"");
	assert_contains(page.body, "value=\"Evening-Special\"");
	axis_test_reply_clear(&page);

	/* Logged: who and which key, never the value. */
	line = axis_test_log_find(G_LOG_LEVEL_MESSAGE, "*setting changed: key=tv.retention by=root*");
	g_assert_nonnull(line);
	g_assert_nonnull(strstr(line, "audit:"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*setting changed: key=tv.*by=root*"), ==, 8);
	g_assert_cmpuint(log_mentions("Evening-Special"), ==, 0);

	/* The same values again change nothing and log nothing more. */
	axis_test_log_reset();
	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	assert_redirect(&reply, "/admin/settings?done=unchanged");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*setting changed*"), ==, 0);
}

/* Spec: Field error */
static void
test_field_errors_keep_the_other_values(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *count = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	form = form_of("tv.title", "keep-this-title", "tv.retention", "soon", "tv.threads", "99",
		"tv.quality", "uhd", "tv.languages", "de\nnl", NULL);

	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	g_assert_cmpuint(reply.status, ==, 400);

	/* An error beside each refused field, saying what is wrong. */
	assert_contains(reply.body, "id=\"setting-tv-retention-error\"");
	assert_contains(reply.body, "must be a length of time");
	assert_contains(reply.body, "id=\"setting-tv-threads-error\"");
	assert_contains(reply.body, "must be at most 16");
	assert_lacks(reply.body, "id=\"setting-tv-title-error\"");
	assert_contains(reply.body, "aria-describedby=\"setting-tv-retention-help setting-tv-retention-error\"");

	/* What else was typed is still there. */
	assert_contains(reply.body, "value=\"keep-this-title\"");
	assert_contains(reply.body, "value=\"soon\"");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"uhd\">");
	assert_contains(reply.body, "de\nnl");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);

	/* Nothing from the submission was stored, not even the valid values. */
	count = count_of_settings(fx);
	g_assert_cmpstr(count, ==, "0");
	g_assert_false(axis_settings_is_set(fx->settings, "tv.title"));
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*setting changed*"), ==, 0);

	/* A field of another module is not touched by this form. */
	g_free(form);
	form = form_of("subs.language", "nederlands-vlaams", NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/subs", token, form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "id=\"setting-subs-language-error\"");
	axis_test_reply_clear(&reply);
}

/* Spec: Restart-required setting */
static void
test_restart_setting_says_so(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *mode = NULL;
	AxisTestReply reply;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	form = form_of("tv.mode", "slow", NULL);

	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	assert_redirect(&reply, "/admin/settings?done=saved-restart");
	axis_test_reply_clear(&reply);

	page = get_as(fx, fx->admin, "/admin/settings?done=saved-restart");
	assert_contains(page.body, "takes effect after the next restart");
	assert_contains(page.body, "The stored value takes effect after the next restart.");
	assert_contains(page.body, "<option selected=\"selected\" value=\"slow\">");
	axis_test_reply_clear(&page);

	/* Still the old value in use. */
	mode = axis_settings_get_string(fx->settings, "tv.mode");
	g_assert_cmpstr(mode, ==, "fast");
}

/* A message key the page does not know shows nothing, and nothing from the
 * request is reflected. */
static void
test_messages_are_fixed_keys(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = get_as(fx, fx->admin, "/admin/settings?done=%3Cscript%3Ealert(1)%3C/script%3E");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "<script>alert");
	assert_lacks(reply.body, "alert(1)");
	axis_test_reply_clear(&reply);
}

/* ── Secrets ────────────────────────────────────────────────────── */

/* Spec: Secret not shown; Secret not logged; Secret cleared */
static void
test_secret_settings(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *stored = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *too_long = g_strnfill(70, 'k');
	AxisTestReply reply;
	AxisTestReply page;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);

	/* Not set yet. */
	page = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(page.body, "No value is stored.");
	assert_lacks(page.body, "/admin/settings/tv/clear");
	axis_test_reply_clear(&page);

	/* Set it. */
	form = form_of("tv.title", "with-a-key", "tv.api_key", SECRET, NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	assert_redirect(&reply, "/admin/settings?done=saved");
	assert_lacks(reply.body, SECRET);
	axis_test_reply_clear(&reply);
	stored = axis_settings_get_string(fx->settings, "tv.api_key");
	g_assert_cmpstr(stored, ==, SECRET);

	/* The page shows that it is set, and the value appears nowhere. */
	page = get_as(fx, fx->admin, "/admin/settings");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "A value is stored.");
	assert_contains(page.body, "name=\"tv.api_key\"");
	assert_contains(page.body, "type=\"password\"");
	assert_contains(page.body, "action=\"/admin/settings/tv/clear\"");
	assert_lacks(page.body, SECRET);
	assert_lacks(page.body, "s3cr3t");
	axis_test_reply_clear(&page);

	/* Left empty in a later submission: kept. */
	g_free(form);
	form = form_of("tv.title", "second-title", "tv.api_key", "", NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);
	g_free(stored);
	stored = axis_settings_get_string(fx->settings, "tv.api_key");
	g_assert_cmpstr(stored, ==, SECRET);

	/* A refused secret (too long) is not echoed back, and the old one stays. */
	g_free(form);
	form = form_of("tv.api_key", too_long, NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "id=\"setting-tv-api_key-error\"");
	assert_lacks(reply.body, "kkkkkkkk");
	assert_lacks(reply.body, SECRET);
	axis_test_reply_clear(&reply);
	g_free(stored);
	stored = axis_settings_get_string(fx->settings, "tv.api_key");
	g_assert_cmpstr(stored, ==, SECRET);

	/* A replacement. */
	g_free(form);
	form = form_of("tv.api_key", "second-key-0987654321", NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, form);
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);

	/* Nothing ever logged it. */
	g_assert_cmpuint(log_mentions(SECRET), ==, 0);
	g_assert_cmpuint(log_mentions("second-key-0987654321"), ==, 0);
	g_assert_cmpuint(log_mentions("kkkkkkkk"), ==, 0);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*setting changed: key=tv.api_key by=root*"), ==, 2);

	/* Forged clears: not a secret, another module's, unknown, no key. */
	{
		g_autofree gchar *not_secret = form_of("key", "tv.title", NULL);
		g_autofree gchar *foreign = form_of("key", "subs.language", NULL);
		g_autofree gchar *unknown = form_of("key", "tv.nothing", NULL);
		g_autofree gchar *wrong_module = form_of("key", "tv.api_key", NULL);

		reply = post_as(fx, fx->admin, "/admin/settings/tv/clear", token, not_secret);
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, "/admin/settings/tv/clear", token, foreign);
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, "/admin/settings/tv/clear", token, unknown);
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, "/admin/settings/tv/clear", token, "");
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, "/admin/settings/subs/clear", token, wrong_module);
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, "/admin/settings/nothing/clear", token, wrong_module);
		g_assert_cmpuint(reply.status, ==, 404);
		axis_test_reply_clear(&reply);
	}
	g_assert_true(axis_settings_is_set(fx->settings, "tv.api_key"));
	g_assert_true(axis_settings_is_set(fx->settings, "tv.title"));

	/* Cleared: removed from the table, shown as not set. */
	g_free(form);
	form = form_of("key", "tv.api_key", NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/tv/clear", token, form);
	assert_redirect(&reply, "/admin/settings?done=cleared");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_settings_is_set(fx->settings, "tv.api_key"));
	count = axis_test_db_query(fx->db, "SELECT count(*) FROM setting WHERE key = 'tv.api_key'");
	g_assert_cmpstr(count, ==, "0");
	page = get_as(fx, fx->admin, "/admin/settings?done=cleared");
	assert_contains(page.body, "The stored value has been removed.");
	assert_contains(page.body, "No value is stored.");
	assert_lacks(page.body, "/admin/settings/tv/clear");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*setting cleared: key=tv.api_key by=root*"), ==, 1);
	g_assert_cmpuint(log_mentions("second-key"), ==, 0);
}

/* ── Who may use them ───────────────────────────────────────────── */

/* Spec: Non-administrator refused (settings and modules) */
static void
test_non_administrator_is_refused(Fixture *fx, gconstpointer data)
{
	static const gchar *const gets[] = { "/admin/settings", "/admin/modules", "/admin/network", NULL };
	static const gchar *const posts[] = { "/admin/settings/tv", "/admin/settings/tv/clear",
		"/admin/modules/tv/disable", "/admin/modules/tv/enable", "/admin/modules/base/disable", NULL };
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *count = NULL;
	g_autofree gchar *states = NULL;
	AxisTestReply reply;
	guint i;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->viewer);
	form = form_of("tv.title", "by-a-viewer", "key", "tv.api_key", NULL);

	for (i = 0; gets[i] != NULL; i++) {
		reply = get_as(fx, fx->viewer, gets[i]);
		g_assert_cmpuint(reply.status, ==, 403);
		assert_lacks(reply.body, "name=\"tv.title\"");
		axis_test_reply_clear(&reply);
	}
	for (i = 0; posts[i] != NULL; i++) {
		/* With a perfectly good token: the role is what refuses. */
		reply = post_as(fx, fx->viewer, posts[i], token, form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}

	count = count_of_settings(fx);
	g_assert_cmpstr(count, ==, "0");
	states = axis_test_db_query(fx->db, "SELECT count(*) FROM module_state");
	g_assert_cmpstr(states, ==, "0");
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "tv"));

	/* Not in the navigation of an ordinary user, but in an administrator's:
	 * the System tab leads to Settings, and its row has both. */
	reply = get_as(fx, fx->viewer, "/");
	assert_lacks(reply.body, "/admin/settings");
	assert_lacks(reply.body, "/admin/modules");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/");
	assert_contains(reply.body, "href=\"/admin/settings\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(reply.body, "href=\"/admin/settings\"");
	assert_contains(reply.body, "href=\"/admin/modules\"");
	assert_contains(reply.body, "href=\"/admin/network\"");
	axis_test_reply_clear(&reply);
}

/* ── The Outbound network page ──────────────────────────────────── */

/* Spec (outbound-binding): The state is shown, and what is not covered */
static void
test_outbound_page(Fixture *fx, gconstpointer data)
{
	AxisOutboundTarget target;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "outbound connections: *");

	/* Off: how to turn it on, and what nothing covers. */
	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
	reply = get_as(fx, fx->admin, "/admin/network");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<h1>Outbound network and VPN</h1><p class=\"page-intro\">");
	assert_contains(reply.body, "Not set: Axis");
	assert_contains(reply.body, "AXIS_OUTBOUND_BIND");
	assert_contains(reply.body, "kind-outbound-traffic");
	assert_contains(reply.body, "Name lookups (DNS)");
	assert_contains(reply.body, "The database");
	assert_lacks(reply.body, ">Bound<");
	axis_test_reply_clear(&reply);

	/* Set to an interface that is not here: missing, refused, and which
	 * traffic that is. */
	g_assert_true(axis_outbound_target_parse("axis-absent0", &target, NULL));
	axis_outbound_configure(&target, AXIS_OUTBOUND_SCOPE_INTERNET);
	reply = get_as(fx, fx->admin, "/admin/network");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "axis-absent0 (an interface)");
	assert_contains(reply.body, "Connections to public addresses only: indexers and TMDb (AXIS_OUTBOUND_BIND_SCOPE=internet)");
	assert_contains(reply.body, ">Missing</span><span> The outbound interface axis-absent0 does not exist.</span>");
	assert_contains(reply.body, "role=\"alert\"");
	assert_contains(reply.body, "nothing is sent another way");
	assert_contains(reply.body, ">Indexer searches and release files</td><td class=\"col-state\" data-label=\"Bound now\"><span "
		"class=\"badge badge-succeeded\">Bound</span>");
	assert_contains(reply.body, ">Download clients</td><td class=\"col-state\" data-label=\"Bound now\"><span class=\"badge badge-note\">"
		"Not bound</span>");
	assert_contains(reply.body, "AXIS_OUTBOUND_BIND_SCOPE=all");
	assert_contains(reply.body, ">Name lookups (DNS)</td><td class=\"col-state\" data-label=\"Bound now\"><span class=\"badge badge-warn\">"
		"Never bound</span>");
	axis_test_reply_clear(&reply);

	/* The loopback device, scope all: up, its address, and the download
	 * clients covered too. */
	g_assert_true(axis_outbound_target_parse("lo", &target, NULL));
	axis_outbound_configure(&target, AXIS_OUTBOUND_SCOPE_ALL);
	reply = get_as(fx, fx->admin, "/admin/network");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Up</span><span> lo is up.</span>");
	assert_contains(reply.body, "127.0.0.1");
	assert_contains(reply.body, ">Download clients</td><td class=\"col-state\" data-label=\"Bound now\"><span class=\"badge badge-succeeded\">"
		"Bound</span>");
	assert_lacks(reply.body, "role=\"alert\"");
	axis_test_reply_clear(&reply);

	axis_outbound_configure(NULL, AXIS_OUTBOUND_SCOPE_INTERNET);
}

static void
test_forged_requests_are_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *count = NULL;
	const gchar *foreign[] = { "X-CSRF-Token", NULL, "Origin", "http://evil.example", NULL };
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	form = form_of("tv.title", "forged", NULL);

	/* An administrator's session, but no token, a wrong one, or another site. */
	reply = post_as(fx, fx->admin, "/admin/settings/tv", NULL, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/admin/settings/tv", "not-the-token", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	foreign[1] = token;
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/admin/settings/tv", fx->admin,
		foreign, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	reply = post_as(fx, fx->admin, "/admin/modules/subs/disable", NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/admin/modules/subs/disable", "not-the-token", "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/admin/modules/base/disable", NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	count = count_of_settings(fx);
	g_assert_cmpstr(count, ==, "0");
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "subs"));

	/* A GET changes nothing and these are not GET routes. */
	reply = get_as(fx, fx->admin, "/admin/modules/subs/disable");
	g_assert_cmpuint(reply.status, ==, 405);
	axis_test_reply_clear(&reply);
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "subs"));

	/* Not logged in at all. */
	reply = post_as(fx, NULL, "/admin/settings/tv", token, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
}

/* An unknown module is nothing to save */
static void
test_unknown_and_unsettable_modules(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);

	reply = post_as(fx, fx->admin, "/admin/settings/nothing", token, "x=1");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	/* Has no settings. */
	reply = post_as(fx, fx->admin, "/admin/settings/base", token, "x=1");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	/* A field of another module in this one's form is ignored. */
	reply = post_as(fx, fx->admin, "/admin/settings/subs", token, "tv.title=sneaky&subs.language=nl");
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_settings_is_set(fx->settings, "tv.title"));
	g_assert_true(axis_settings_is_set(fx->settings, "subs.language"));
}

/* ── Modules page ───────────────────────────────────────────────── */

/* Spec: Required module (no switch); Enable blocked / Disable blocked (shown) */
static void
test_modules_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	reply = get_as(fx, fx->admin, "/admin/modules");
	g_assert_cmpuint(reply.status, ==, 200);

	assert_contains(reply.body, "Base");
	assert_contains(reply.body, "The part that cannot be switched off.");
	assert_contains(reply.body, "Fetches subtitles for TV shows.");
	assert_contains(reply.body, "Web interface");

	/* Required modules have no switch. */
	assert_contains(reply.body, "Always on");
	assert_lacks(reply.body, "/admin/modules/base/");
	assert_lacks(reply.body, "/admin/modules/web/");

	/* Subtitles can be switched off; TV cannot, and the page says why. */
	assert_contains(reply.body, "action=\"/admin/modules/subs/disable\"");
	assert_lacks(reply.body, "action=\"/admin/modules/tv/disable\"");
	assert_contains(reply.body, "Subtitles is switched on and needs it");
	assert_contains(reply.body, "name=\"_csrf\"");
	assert_no_inline_code(reply.body);
	axis_test_reply_clear(&reply);
}

/* The System tab's row (the entries of these modules state no group). */
static void
assert_nav(Fixture *fx, gboolean tv, gboolean subs)
{
	AxisTestReply reply = get_as(fx, fx->admin, "/admin/modules");

	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpint(strstr(reply.body, "href=\"/tv\"") != NULL, ==, tv);
	g_assert_cmpint(strstr(reply.body, "href=\"/subs\"") != NULL, ==, subs);
	axis_test_reply_clear(&reply);
}

static void
assert_status(Fixture *fx, const gchar *path, guint status)
{
	AxisTestReply reply = get_as(fx, fx->admin, path);

	g_assert_cmpuint(reply.status, ==, status);
	axis_test_reply_clear(&reply);
}

/*
 * Spec: Routes and navigation; Dependencies; Disabling removes nothing;
 * Re-enable resumes; State persists; Required module (forged request)
 */
static void
test_switching_modules(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *retention = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	assert_nav(fx, TRUE, TRUE);
	assert_status(fx, "/tv", 200);
	assert_status(fx, "/subs", 200);

	/* Some settings of a module that will be switched off. */
	g_assert_true(axis_settings_set(fx->settings, "tv.retention", "3d", NULL));
	g_assert_true(axis_settings_set(fx->settings, "tv.api_key", SECRET, NULL));

	/* Disable blocked: TV is needed by Subtitles. The page names it. */
	reply = post_as(fx, fx->admin, "/admin/modules/tv/disable", token, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Subtitles");
	assert_contains(reply.body, "cannot be switched off");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "tv"));
	assert_status(fx, "/tv", 200);

	/* Required: refused, and nothing changes. */
	reply = post_as(fx, fx->admin, "/admin/modules/base/disable", token, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "required");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/admin/modules/web/disable", token, "");
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "base"));
	g_assert_true(axis_module_registry_is_enabled(fx->registry, "web"));
	assert_status(fx, "/admin/settings", 200);

	/* Unknown. */
	reply = post_as(fx, fx->admin, "/admin/modules/nothing/disable", token, "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* Subtitles off: navigation entry gone, routes 404, hook called. */
	reply = post_as(fx, fx->admin, "/admin/modules/subs/disable", token, "");
	assert_redirect(&reply, "/admin/modules?done=disabled");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "subs"));
	assert_nav(fx, TRUE, FALSE);
	assert_status(fx, "/subs", 404);
	assert_status(fx, "/tv", 200);

	/* The wrong method on a hidden route is as much "not found" as any. */
	reply = post_as(fx, fx->admin, "/subs", token, "");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* Now TV can go too: hook, nav, routes, settings page, state stored. */
	reply = post_as(fx, fx->admin, "/admin/modules/tv/disable", token, "");
	assert_redirect(&reply, "/admin/modules?done=disabled");
	axis_test_reply_clear(&reply);
	assert_nav(fx, FALSE, FALSE);
	assert_status(fx, "/tv", 404);
	g_assert_cmpstr(fx->events->str, ==, "stop;");
	rows = axis_test_db_query(fx->db,
		"SELECT string_agg(module_id || '=' || enabled, ' ' ORDER BY module_id) FROM module_state");
	g_assert_cmpstr(rows, ==, "subs=false tv=false");

	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_lacks(reply.body, "name=\"tv.retention\"");
	assert_lacks(reply.body, "name=\"subs.language\"");
	assert_contains(reply.body, "name=\"web.session_idle\"");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, "tv.title=while-off");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	g_assert_false(axis_settings_is_set(fx->settings, "tv.title"));

	reply = get_as(fx, fx->admin, "/admin/modules");
	assert_contains(reply.body, "action=\"/admin/modules/tv/enable\"");
	axis_test_reply_clear(&reply);

	/* Enable blocked: Subtitles needs TV, which is off. */
	reply = post_as(fx, fx->admin, "/admin/modules/subs/enable", token, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "needs TV shows");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_module_registry_is_enabled(fx->registry, "subs"));

	/* Nothing was removed, not the settings nor the secret. */
	retention = axis_test_db_query(fx->db, "SELECT value::text FROM setting WHERE key = 'tv.retention'");
	g_assert_cmpstr(retention, ==, "259200");
	g_assert_true(axis_settings_is_set(fx->settings, "tv.api_key"));

	/* Back on, bottom up: resumes as it was. */
	g_string_truncate(fx->events, 0);
	reply = post_as(fx, fx->admin, "/admin/modules/tv/enable", token, "");
	assert_redirect(&reply, "/admin/modules?done=enabled");
	axis_test_reply_clear(&reply);
	g_assert_cmpstr(fx->events->str, ==, "start;");
	reply = post_as(fx, fx->admin, "/admin/modules/subs/enable", token, "");
	assert_redirect(&reply, "/admin/modules?done=enabled");
	axis_test_reply_clear(&reply);
	assert_nav(fx, TRUE, TRUE);
	assert_status(fx, "/tv", 200);
	assert_status(fx, "/subs", 200);
	g_assert_cmpint(axis_settings_get_duration(fx->settings, "tv.retention"), ==, 3 * 86400);
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(reply.body, "value=\"3d\"");
	axis_test_reply_clear(&reply);

	/* Switching to the state it is in is not an error. */
	reply = post_as(fx, fx->admin, "/admin/modules/tv/enable", token, "");
	assert_redirect(&reply, "/admin/modules?done=enabled");
	axis_test_reply_clear(&reply);

	/* Logged with the user and the module. */
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*module disabled: module=tv by=root*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*module enabled: module=subs by=root*"), ==, 1);
	g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_MESSAGE, "*module change refused: module=base*"), ==, 1);
	g_assert_cmpuint(log_mentions(SECRET), ==, 0);
}

/* The state stored by the page is what a restart starts with */
static void
test_page_state_survives_restart(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	AxisTestReply reply;
	g_autoptr(AxisModuleRegistry) again = axis_module_registry_new();
	g_autoptr(GError) error = NULL;
	AxisModuleInfo info;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	reply = post_as(fx, fx->admin, "/admin/modules/subs/disable", token, "");
	assert_redirect(&reply, "/admin/modules?done=disabled");
	axis_test_reply_clear(&reply);

	memset(&info, 0, sizeof(info));
	info.id = "subs";
	g_assert_true(axis_module_registry_register(again, &info, NULL, &error));
	g_assert_true(axis_module_state_load(again, fx->pool, &error));
	g_assert_false(axis_module_registry_is_enabled(again, "subs"));
}

/* ── The session idle setting ───────────────────────────────────── */

static void
test_session_idle_setting(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);
	g_assert_cmpuint(axis_session_store_get_idle_seconds(), ==, 30 * 24 * 3600);

	/* Limits are the declaration's. */
	reply = post_as(fx, fx->admin, "/admin/settings/web", token, "web.session_idle=30m");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/admin/settings/web", token, "web.session_idle=100d");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_session_store_get_idle_seconds(), ==, 30 * 24 * 3600);

	reply = post_as(fx, fx->admin, "/admin/settings/web", token, "web.session_idle=2h");
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);
	axis_test_pump_for(50);
	g_assert_cmpuint(axis_session_store_get_idle_seconds(), ==, 2 * 3600);

	/* An hour on, the session is fine; hours later, it has run out. */
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(3600) * G_USEC_PER_SEC);
	assert_status(fx, "/admin/settings", 200);
	axis_web_auth_set_time_offset(fx->auth, G_GINT64_CONSTANT(4 * 3600) * G_USEC_PER_SEC);
	reply = get_as(fx, fx->admin, "/admin/settings");
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_prefix(axis_test_reply_header(&reply, "Location"), "/login"));
	axis_test_reply_clear(&reply);
	axis_web_auth_set_time_offset(fx->auth, 0);
}

/* ── The default theme setting ──────────────────────────────────── */

/* The theme id written on the root element of @html. */
static gchar *
html_theme(const gchar *html)
{
	const gchar *start = strstr(html, "<html");
	const gchar *end;
	const gchar *attr;
	const gchar *quote;

	g_assert_nonnull(start);
	end = strchr(start, '>');
	g_assert_nonnull(end);
	attr = strstr(start, " data-theme=\"");
	g_assert_true(attr != NULL && attr < end);
	attr += strlen(" data-theme=\"");
	quote = strchr(attr, '"');
	g_assert_nonnull(quote);

	return g_strndup(attr, quote - attr);
}

static void
assert_theme(Fixture *fx, const gchar *cookie, const gchar *path, const gchar *expected)
{
	AxisTestReply page = get_as(fx, cookie, path);
	g_autofree gchar *id = NULL;

	g_assert_cmpuint(page.status, ==, 200);
	id = html_theme(page.body);
	g_assert_cmpstr(id, ==, expected);
	axis_test_reply_clear(&page);
}

/* Spec: Changing the default */
static void
test_default_theme_setting(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);

	/* It is a choice among the built-in themes, dark unless changed. */
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(reply.body, "name=\"web.default_theme\"");
	assert_contains(reply.body, "<option selected=\"selected\" value=\"midnight\">");
	assert_contains(reply.body, "value=\"paper\">");
	assert_contains(reply.body, "value=\"auto\">");
	/* (tidy-labels) Each choice shows its label; the stored value stays the id. */
	assert_contains(reply.body, "value=\"midnight\">Midnight</option>");
	assert_contains(reply.body, "value=\"contrast\">High contrast</option>");
	assert_contains(reply.body, "value=\"auto\">Match my device</option>");
	assert_lacks(reply.body, ">contrast</option>");
	axis_test_reply_clear(&reply);
	assert_theme(fx, NULL, "/login", "midnight");
	assert_theme(fx, fx->viewer, "/", "midnight");

	/* A name that is not a theme is refused and changes nothing. */
	reply = post_as(fx, fx->admin, "/admin/settings/web", token, "web.default_theme=neon");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	assert_theme(fx, NULL, "/login", "midnight");

	/* The viewer has chosen "forest"; then the default is changed. */
	axis_web_auth_set_test_theme(fx->auth, "viewer", "forest");
	reply = post_as(fx, fx->admin, "/admin/settings/web", token, "web.default_theme=paper");
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);
	axis_test_pump_for(50);

	/* The login page and users with no choice use it; a user who chose
	 * a theme keeps theirs. */
	assert_theme(fx, NULL, "/login", "paper");
	assert_theme(fx, fx->admin, "/", "paper");
	assert_theme(fx, fx->viewer, "/", "forest");
	g_assert_cmpstr(axis_web_theme_get_default()->id, ==, "paper");

	/* And back. */
	reply = post_as(fx, fx->admin, "/admin/settings/web", token, "web.default_theme=midnight");
	assert_redirect(&reply, "/admin/settings?done=saved");
	axis_test_reply_clear(&reply);
	axis_test_pump_for(50);
	assert_theme(fx, NULL, "/login", "midnight");
}

/* The forms of Settings and Modules come back to the module's card or row. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *token = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	token = csrf_for(fx, fx->admin);

	reply = post_as(fx, fx->admin, "/admin/settings/tv", token, "tv.title=Spot&tv.api_key=a-made-up-key-12345");
	assert_place(fx, &reply, "module-tv");
	reply = post_as(fx, fx->admin, "/admin/settings/tv/clear", token, "key=tv.api_key");
	assert_place(fx, &reply, "module-tv");

	reply = post_as(fx, fx->admin, "/admin/modules/subs/disable", token, "");
	assert_place(fx, &reply, "module-subs");
	reply = post_as(fx, fx->admin, "/admin/modules/subs/enable", token, "");
	assert_place(fx, &reply, "module-subs");
}

#define ADD(name, func) \
	g_test_add("/web-settings/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("settings/lists-declared-settings", test_page_lists_declared_settings);
	ADD("settings/markup-in-values-is-escaped", test_markup_in_values_is_escaped);
	ADD("settings/save", test_save_settings);
	ADD("settings/field-errors", test_field_errors_keep_the_other_values);
	ADD("settings/restart-setting", test_restart_setting_says_so);
	ADD("settings/messages-are-fixed", test_messages_are_fixed_keys);
	ADD("settings/secrets", test_secret_settings);
	ADD("settings/unknown-modules", test_unknown_and_unsettable_modules);
	ADD("settings/session-idle", test_session_idle_setting);
	ADD("settings/default-theme", test_default_theme_setting);

	ADD("access/non-administrator", test_non_administrator_is_refused);
	ADD("access/forged-requests", test_forged_requests_are_refused);

	ADD("modules/page", test_modules_page);
	ADD("modules/switching", test_switching_modules);
	ADD("modules/state-survives-restart", test_page_state_survives_restart);

	ADD("outbound/page", test_outbound_page);

	g_test_add("/web/settings/back-to-place", Fixture, NULL, fixture_setup, test_back_to_place, fixture_teardown);

	return g_test_run();
}
