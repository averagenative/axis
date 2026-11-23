/*
 * test-web-profiles.c - the profile pages: the list and a profile's page
 * (the definition in words, its revisions, the lock said plainly), copy,
 * edit with per-field errors that keep the other values and a preview of
 * the change, a built-in refused, the assignment page with its second
 * confirmation (and nothing converted or started), a stored definition
 * that no longer validates, a custom profile's Savings page, names that
 * are never markup, and every change an administrator's with its CSRF token
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The real shell and the storage, library, probe, browser, analysis,
 * estimate and profiles modules on an ephemeral loopback port, with the
 * library of tests/axis-test-savings.h.
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
#include "axis-test-savings.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-module.h"
#include "core/axis-module-state.h"
#include "library/axis-analysis.h"
#include "library/axis-library.h"
#include "probe/axis-probe.h"
#include "transcode/axis-estimator.h"
#include "transcode/axis-profile.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-analysis.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-library.h"
#include "web/axis-web-probe.h"
#include "web/axis-web-profiles.h"
#include "web/axis-web-savings.h"
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
	AxisEstimator      *estimator;
	AxisWebStorage     *storage_ui;
	AxisWebLibrary     *library_ui;
	AxisWebProbe       *probe_ui;
	AxisWebBrowser     *browser_ui;
	AxisWebAnalysis    *analysis_ui;
	AxisWebSavings     *savings_ui;
	AxisWebProfiles    *profiles_ui;
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
	AxisTestLibrary     view;
	AxisTestSavings     s;
} Fixture;

#define SKIP_WITHOUT_DB(fx) do { if ((fx)->db == NULL) return; } while (0)

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
	fx->estimator = axis_estimator_new(fx->pool, fx->storage, fx->settings);
	fx->storage_ui = axis_web_storage_new(fx->storage, fx->hub);
	fx->library_ui = axis_web_library_new(fx->library, fx->storage);
	fx->probe_ui = axis_web_probe_new(fx->probe, fx->storage);
	fx->browser_ui = axis_web_browser_new(fx->library, fx->probe, fx->storage, fx->hub);
	fx->analysis_ui = axis_web_analysis_new(fx->analysis);
	fx->savings_ui = axis_web_savings_new(fx->estimator);
	fx->profiles_ui = axis_web_profiles_new(fx->estimator);
	axis_web_probe_set_estimator(fx->probe_ui, fx->estimator);
	axis_web_browser_set_estimator(fx->browser_ui, fx->estimator);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(), fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_library_get_module(), fx->library_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_probe_get_module(), fx->probe_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_browser_get_module(), fx->browser_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_analysis_get_module(), fx->analysis_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_savings_get_module(), fx->savings_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_profiles_get_module(), fx->profiles_ui,
		&error));
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
	axis_test_savings_build(&fx->view, &fx->s);
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
	axis_web_profiles_stop(fx->profiles_ui);
	axis_web_savings_stop(fx->savings_ui);
	axis_web_analysis_stop(fx->analysis_ui);
	axis_web_browser_stop(fx->browser_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_probe_stop(fx->probe_ui);
	axis_web_library_stop(fx->library_ui);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_profiles_free(fx->profiles_ui);
	axis_web_savings_free(fx->savings_ui);
	axis_web_analysis_free(fx->analysis_ui);
	axis_web_browser_free(fx->browser_ui);
	axis_web_probe_free(fx->probe_ui);
	axis_web_library_free(fx->library_ui);
	axis_web_storage_free(fx->storage_ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_estimator_unref(fx->estimator);
	axis_analysis_unref(fx->analysis);
	axis_probe_unref(fx->probe);
	axis_library_unref(fx->library);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_settings_unref(fx->settings);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_savings_clear(&fx->s);
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
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply page = get_as(fx, cookie, "/profiles");
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

static void
assert_links_safe(const gchar *body)
{
	g_autoptr(GRegex) regex = g_regex_new("(href|action)=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) allowed = g_regex_new("^[/#A-Za-z0-9_.?=&;%:+-]*$", 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;

	g_regex_match(regex, body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *href = g_match_info_fetch(info, 2);

		if (!g_regex_match(allowed, href, 0, NULL))
			g_error("a link with something other than a path in it: %s", href);
		g_match_info_next(info, NULL);
	}
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	assert_lacks(body, "onclick");
	assert_lacks(body, "javascript:");
}

static gchar *
query(
	Fixture     *fx,
	const gchar *sql
){
	return axis_test_db_query(fx->db, sql);
}

/* How many recorded messages, at any level, mention @text. */
static guint
log_mentions(const gchar *text)
{
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_DEBUG, G_LOG_LEVEL_INFO, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_WARNING
	};
	g_autofree gchar *pattern = g_strdup_printf("*%s*", text);
	guint count = 0;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
		count += axis_test_log_count(levels[i], pattern);
	return count;
}

/* An urlencoded body of name, value pairs ended by NULL. */
static gchar *
form_of(const gchar *first, ...)
{
	GString *out = g_string_new(NULL);
	const gchar *name = first;
	va_list args;

	va_start(args, first);
	while (name != NULL) {
		const gchar *value = va_arg(args, const gchar *);
		g_autofree gchar *n = g_uri_escape_string(name, NULL, FALSE);
		g_autofree gchar *v = g_uri_escape_string(value != NULL ? value : "", NULL, FALSE);

		g_string_append_printf(out, "%s%s=%s", out->len > 0 ? "&" : "", n, v);
		name = va_arg(args, const gchar *);
	}
	va_end(args);
	return g_string_free(out, FALSE);
}

/* The edit form of a copy of Space saver, as it opens, with @crf and
 * @saving in place of its own, for revision @expected. */
static gchar *
space_form(
	const gchar *name,
	const gchar *crf,
	const gchar *saving,
	const gchar *expected,
	const gchar *action
){
	return form_of("name", name, "note", "", "lib_movies", "yes", "lib_tv", "yes", "lib_other", "yes",
		"codec", "hevc", "bit_depth", "10", "hdr", "keep", "encoder1", "software", "encoder2", "nvenc",
		"encoder3", "vaapi", "crf", crf, "rule1_res", "2160p", "rule1_min_gb", "", "rule1_max_gb", "20",
		"rule1_crf", "24", "rule2_res", "2160p", "rule2_min_gb", "50", "rule2_max_gb", "", "rule2_crf", "28",
		"audio", "copy", "stereo_kbps", "", "languages", "", "subtitles", "copy", "container", "mkv",
		"efficient_bpp", "0.150", "min_saving_percent", saving, "min_saving_gb", "0", "expected", expected,
		"action", action, NULL);
}

static gchar *
copy(
	Fixture     *fx,
	const gchar *from,
	const gchar *name
){
	g_autofree gchar *path = g_strdup_printf("/profiles/%s/copy", from);
	g_autofree gchar *body = form_of("name", name, NULL);
	AxisTestReply reply = post_as(fx, fx->admin, path, body);
	const gchar *location;
	gchar *slug;

	g_assert_cmpuint(reply.status, ==, 303);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_true(g_str_has_prefix(location, "/profiles/custom_"));
	g_assert_true(g_str_has_suffix(location, "?done=copied"));
	slug = g_strndup(location + strlen("/profiles/"), strlen(location) - strlen("/profiles/") - strlen("?done=copied"));
	axis_test_reply_clear(&reply);
	return slug;
}

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

/* Every figure of a page that links to the file list: the list states the
 * number the figure shows. */
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

		if (g_ascii_isdigit(text[0])) {
			g_autoptr(GString) plain = g_string_new(raw);
			gint64 shown = g_ascii_strtoll(text, NULL, 10);
			AxisTestReply list;
			gint64 stated;

			g_string_replace(plain, "&amp;", "&", 0);
			list = get_as(fx, fx->viewer, plain->str);
			if (list.status != 200)
				g_error("%s: status %u", plain->str, list.status);
			stated = stated_count(list.body);
			if (stated != shown)
				g_error("%s: the figure says %s, the list says %" G_GINT64_FORMAT, plain->str, text, stated);
			axis_test_reply_clear(&list);
			followed++;
		}
		g_match_info_next(info, NULL);
	}
	return followed;
}

/* ── Pages ──────────────────────────────────────────────────────── */

static void
test_list_and_profile(Fixture *fx, gconstpointer data)
{
	AxisTestReply page;
	guint i;

	SKIP_WITHOUT_DB(fx);
	page = get_as(fx, fx->viewer, "/profiles");
	g_assert_cmpuint(page.status, ==, 200);
	for (i = 0; i < AXIS_PROFILE_N_BUILTINS; i++) {
		g_autofree gchar *href = g_strdup_printf("href=\"/profiles/%s\"", axis_profile_builtins()[i].slug);

		assert_contains(page.body, href);
	}
	assert_contains(page.body, "Locked: trials only");
	assert_contains(page.body, "Built-in profiles and your copies");
	assert_contains(page.body, "Every change makes a new version");
	assert_contains(page.body, "Dolby Vision");
	assert_contains(page.body, "not estimated yet");
	assert_csp_clean(page.body);
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);

	/* Spec: New profiles start locked (the page says so); the definition in
	 * words, its revisions. */
	page = get_as(fx, fx->viewer, "/profiles/space_saver");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Conversion profile: Video: Space saver");
	assert_contains(page.body, "Locked: this version can make trials of single files but cannot replace a file");
	assert_contains(page.body, "CRF 26; but, the first that applies: CRF 24 for 2160p sources under 20 GB; CRF 28 for "
		"2160p sources of 50 GB or more");
	assert_contains(page.body, "Versions");
	assert_contains(page.body, "State of this profile");
	assert_contains(page.body, "The definition as stored");
	assert_lacks(page.body, "action=\"/profiles/space_saver/copy\"");
	assert_csp_clean(page.body);
	assert_links_safe(page.body);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/profiles/space_saver");
	assert_contains(page.body, "action=\"/profiles/space_saver/copy\"");
	assert_contains(page.body, "cannot be edited or archived");
	assert_lacks(page.body, "/profiles/space_saver/edit");
	axis_test_reply_clear(&page);

	page = get_as(fx, fx->viewer, "/profiles/nosuch");
	g_assert_cmpuint(page.status, ==, 404);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->viewer, "/profiles?x=1");
	g_assert_cmpuint(page.status, ==, 400);
	axis_test_reply_clear(&page);
}

/* Spec: Built-in cannot be edited; Non-administrator refused. */
static void
test_refusals(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *body = space_form("Mine", "24", "15", "1", "save");
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	before = query(fx, "SELECT count(*) || '/' || (SELECT count(*) FROM conversion_profile) ||"
		" '/' || (SELECT count(*) FROM conversion_profile_assignment) FROM conversion_profile_revision");
	reply = get_as(fx, fx->admin, "/profiles/space_saver/edit");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/profiles/space_saver/edit", body);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/profiles/space_saver/archive", "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* A non-administrator: 403 for every change, nothing changes. */
	reply = post_as(fx, fx->viewer, "/profiles/space_saver/copy", "name=Mine");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/profiles/assign");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *assign = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=space_saver&confirm=yes",
			fx->s.films_a);

		reply = post_as(fx, fx->viewer, "/profiles/assign", assign);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}
	reply = post_as(fx, fx->viewer, "/profiles/unassign", "root=1&kind=video");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/profiles/space_saver/edit");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Without the CSRF token, even an administrator. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", "/profiles/space_saver/copy", fx->admin, NULL,
		"name=Mine");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	after = query(fx, "SELECT count(*) || '/' || (SELECT count(*) FROM conversion_profile) ||"
		" '/' || (SELECT count(*) FROM conversion_profile_assignment) FROM conversion_profile_revision");
	g_assert_cmpstr(before, ==, after);
}

/* Spec: Copy; Out of bounds (beside the field); Minimum saving floor;
 * Edit makes a revision; No change; the preview says what kind of change it
 * is before saving; names are text. */
static void
test_copy_and_edit(Fixture *fx, gconstpointer data)
{
	const gchar *name = "<script>alert(1)</script> & \"Kids\"";
	g_autofree gchar *slug = NULL;
	g_autofree gchar *edit = NULL;
	g_autofree gchar *profile = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *first = NULL;
	AxisTestReply reply;
	g_autoptr(GRegex) in_attribute = g_regex_new("=\"[^\"]*alert\\(1\\)", 0, 0, NULL);

	SKIP_WITHOUT_DB(fx);
	slug = copy(fx, "space_saver", name);
	edit = g_strdup_printf("/profiles/%s/edit", slug);
	profile = g_strdup_printf("/profiles/%s", slug);

	/* Its page: the name as text, never markup, never in an attribute. */
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=copied", profile);

		reply = get_as(fx, fx->admin, done);
	}
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "<script>alert");
	assert_contains(reply.body, "&lt;script&gt;alert(1)&lt;/script&gt;");
	g_assert_false(g_regex_match(in_attribute, reply.body, 0, NULL));
	assert_contains(reply.body, "The copy has been made");
	assert_contains(reply.body, "an administrator&#39;s copy");
	assert_contains(reply.body, edit);
	assert_csp_clean(reply.body);
	assert_links_safe(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/profiles");
	assert_lacks(reply.body, "<script>alert");
	assert_contains(reply.body, "a copy of Video: Space saver");
	g_assert_false(g_regex_match(in_attribute, reply.body, 0, NULL));
	axis_test_reply_clear(&reply);

	/* The form, as it opens. */
	reply = get_as(fx, fx->admin, edit);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "name=\"crf\" value=\"26\"");
	assert_contains(reply.body, "name=\"rule2_min_gb\" value=\"50\"");
	assert_contains(reply.body, "&lt;script&gt;alert(1)");
	assert_lacks(reply.body, "<script>alert");
	g_assert_false(g_regex_match(in_attribute, reply.body, 0, NULL));
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	first = query(fx, "SELECT definition FROM conversion_profile_revision r JOIN conversion_profile p"
		" ON p.id = r.profile_id WHERE p.slug LIKE 'custom_%' AND r.number = 1");

	/* Out of bounds: beside the field, the other values kept. */
	g_free(body);
	body = space_form("Kids", "27", "33", "1", "save");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 422);
	assert_contains(reply.body, "id=\"profile-crf-error\"");
	assert_contains(reply.body, "For HEVC the quality is an even CRF from 20 to 32.");
	assert_contains(reply.body, "name=\"min_saving_percent\" value=\"33\"");
	assert_contains(reply.body, "name=\"crf\" value=\"27\"");
	assert_contains(reply.body, ">Kids</textarea>");
	axis_test_reply_clear(&reply);
	/* Spec: Minimum saving floor. */
	g_free(body);
	body = space_form("Kids", "26", "4", "1", "save");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 422);
	assert_contains(reply.body, "id=\"profile-min_saving_percent-error\"");
	assert_contains(reply.body, "never below 5%");
	axis_test_reply_clear(&reply);
	/* Text where a number goes, reflected only as what a number is made
	 * of. */
	g_free(body);
	body = space_form("Kids", "2<b>6", "15", "1", "save");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 422);
	assert_contains(reply.body, "name=\"crf\" value=\"2b6\"");
	assert_lacks(reply.body, "2<b>6");
	axis_test_reply_clear(&reply);

	/* The preview: what kind of change, the lock, the counts; nothing
	 * saved. */
	g_free(body);
	body = space_form("Kids", "24", "15", "1", "preview");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "What this change would do");
	assert_contains(reply.body, "This change can alter the files produced (Quality rule)");
	assert_contains(reply.body, "the new version is locked");
	assert_contains(reply.body, "The current version is locked, so the new one will be too");
	assert_contains(reply.body, "Files that would be converted");
	assert_contains(reply.body, "Every file in the library");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	g_free(body);
	body = space_form("Kids", "26", "25", "1", "preview");
	reply = post_as(fx, fx->admin, edit, body);
	assert_contains(reply.body, "only decide which files are converted, never what is made of one: Minimum saving");
	assert_lacks(reply.body, "This change can alter the files produced");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *count = query(fx, "SELECT count(*) FROM conversion_profile_revision r JOIN conversion_profile p"
			" ON p.id = r.profile_id WHERE NOT p.built_in");

		g_assert_cmpstr(count, ==, "1");
	}

	/* Saved: a new revision, the first unchanged. */
	g_free(body);
	body = space_form("Kids", "24", "15", "1", "save");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_suffix(axis_test_reply_header(&reply, "Location"), "?done=saved"));
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *count = query(fx, "SELECT count(*) FROM conversion_profile_revision r JOIN conversion_profile p"
			" ON p.id = r.profile_id WHERE NOT p.built_in");
		g_autofree gchar *still = query(fx, "SELECT definition FROM conversion_profile_revision r JOIN"
			" conversion_profile p ON p.id = r.profile_id WHERE p.slug LIKE 'custom_%' AND r.number = 1");

		g_assert_cmpstr(count, ==, "2");
		g_assert_cmpstr(still, ==, first);
	}
	/* Saved again unchanged: nothing added. */
	g_free(body);
	body = space_form("Kids", "24", "15", "2", "save");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_true(g_str_has_suffix(axis_test_reply_header(&reply, "Location"), "?done=unchanged"));
	axis_test_reply_clear(&reply);
	/* From a stale page: a conflict, nothing added. */
	g_free(body);
	body = space_form("Kids", "28", "15", "1", "save");
	reply = post_as(fx, fx->admin, edit, body);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "changed by someone else");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *count = query(fx, "SELECT count(*) FROM conversion_profile_revision r JOIN conversion_profile p"
			" ON p.id = r.profile_id WHERE NOT p.built_in");

		g_assert_cmpstr(count, ==, "2");
	}
	/* The page names both, and audit lines were written. */
	reply = get_as(fx, fx->admin, profile);
	assert_contains(reply.body, "2 (current)");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(log_mentions("revision 2 saved (locked) by=root"), ==, 1);
	g_assert_cmpuint(log_mentions("saved without a change: no revision added by=root"), ==, 1);
}

/* Spec: Assign; Risky or unknown; Archive (no longer offered). */
static void
test_assign(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *index_before = NULL;
	g_autofree gchar *index_after = NULL;
	g_autofree gchar *jobs_before = NULL;
	g_autofree gchar *jobs_after = NULL;
	g_autofree gchar *pairing = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *slug = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	index_before = axis_test_library_index(&fx->view, fx->s.films_a);
	jobs_before = query(fx, "SELECT count(*) FROM job");
	reply = get_as(fx, fx->admin, "/profiles/assign");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Films");
	assert_contains(reply.body, "converts nothing, changes no file and starts no job");
	assert_contains(reply.body, "No conversion profile is for what this library folder holds.");
	assert_csp_clean(reply.body);
	assert_links_safe(reply.body);
	axis_test_reply_clear(&reply);

	/* The pairing: its saving and its verdict, the confirmation stating it. */
	pairing = g_strdup_printf("/profiles/assign?root=%" G_GINT64_FORMAT "&profile=space_saver", fx->s.films_a);
	reply = get_as(fx, fx->admin, pairing);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Video: Space saver on Films");
	assert_contains(reply.body, "Unknown");
	assert_contains(reply.body, "The verdict for this pairing is unknown");
	assert_contains(reply.body, "name=\"confirm\"");
	axis_test_reply_clear(&reply);

	/* Without the second confirmation: asked for it, nothing assigned. */
	body = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=space_saver", fx->s.films_a);
	reply = post_as(fx, fx->admin, "/profiles/assign", body);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "This verdict is not safe: confirm it to assign the profile.");
	assert_contains(reply.body, "The verdict for this pairing is unknown");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *count = query(fx, "SELECT count(*) FROM conversion_profile_assignment");

		g_assert_cmpstr(count, ==, "0");
	}
	/* Confirmed: assigned, and nothing else. */
	g_free(body);
	body = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=space_saver&confirm=yes", fx->s.films_a);
	reply = post_as(fx, fx->admin, "/profiles/assign", body);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "/profiles/assign?done=assigned&at=assign-row-");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *row = query(fx, "SELECT p.slug || '/' || a.kind || '/' || a.verdict || '/' || a.confirmed"
			" || '/' || a.assigned_by FROM conversion_profile_assignment a JOIN conversion_profile p ON p.id ="
			" a.profile_id");

		g_assert_cmpstr(row, ==, "space_saver/video/unknown/true/root");
	}
	jobs_after = query(fx, "SELECT count(*) FROM job");
	g_assert_cmpstr(jobs_after, ==, jobs_before);
	index_after = axis_test_library_index(&fx->view, fx->s.films_a);
	g_assert_cmpstr(index_after, ==, index_before);
	reply = get_as(fx, fx->admin, "/profiles/assign?done=assigned");
	assert_contains(reply.body, "Nothing has been converted");
	assert_contains(reply.body, "href=\"/profiles/space_saver\"");
	assert_contains(reply.body, "playback verdict then: Unknown (confirmed)");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/profiles");
	assert_contains(reply.body, "Films");
	axis_test_reply_clear(&reply);

	/* An archived profile is not offered; unassigning goes back. */
	slug = copy(fx, "balanced", "Spare");
	{
		g_autofree gchar *archive = g_strdup_printf("/profiles/%s/archive", slug);
		g_autofree gchar *option = g_strdup_printf("value=\"%s\"", slug);

		reply = get_as(fx, fx->admin, "/profiles/assign");
		assert_contains(reply.body, option);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, archive, "");
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, "/profiles/assign");
		assert_lacks(reply.body, option);
		axis_test_reply_clear(&reply);
		g_free(body);
		body = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=%s&confirm=yes", fx->s.shows_a, slug);
		reply = post_as(fx, fx->admin, "/profiles/assign", body);
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	g_free(body);
	body = g_strdup_printf("root=%" G_GINT64_FORMAT "&kind=video", fx->s.films_a);
	reply = post_as(fx, fx->admin, "/profiles/unassign", body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *count = query(fx, "SELECT count(*) FROM conversion_profile_assignment");

		g_assert_cmpstr(count, ==, "0");
	}
}

/* Spec: Stored definition no longer valid: its pages say why. */
static void
test_unusable(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slug = NULL;
	g_autofree gchar *sql = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	slug = copy(fx, "space_saver", "Damaged");
	sql = g_strdup_printf(
		"INSERT INTO conversion_profile_revision (profile_id, number, name, definition, hash, origin, created_by)"
		" SELECT id, 2, 'Damaged', d, encode(sha256(convert_to(d, 'UTF8')), 'hex'), 'user', 'root' FROM"
		" (SELECT p.id, replace(r.definition, '\"crf\":28', '\"crf\":27') AS d FROM conversion_profile p"
		"  JOIN conversion_profile_revision r ON r.id = p.current_revision_id WHERE p.slug = '%s') x;"
		"UPDATE conversion_profile SET current_revision_id = (SELECT id FROM conversion_profile_revision r"
		" WHERE r.profile_id = conversion_profile.id AND r.number = 2) WHERE slug = '%s'", slug, slug);
	axis_test_db_exec(fx->db, sql);
	path = g_strdup_printf("/profiles/%s", slug);
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "does not pass the checks of this Axis release (quality.rules.2.crf: ");
	assert_contains(reply.body, "Not usable");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("/savings/target/%s", slug);
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "does not pass the checks of this Axis release");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("/profiles/%s/edit", slug);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
}

/* Spec: Custom profile (its Savings page, figures that link to lists with
 * matching counts); Revision named. */
static void
test_custom_savings(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *slug = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *refresh_action = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	slug = copy(fx, "space_saver", "Mine");
	path = g_strdup_printf("/savings/target/%s", slug);
	refresh_action = g_strdup_printf("action=\"/savings/target/%s/refresh\"", slug);
	g_assert_true(axis_estimator_refresh(fx->estimator, "direct", 0, NULL, NULL, &error));
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "No estimates for this conversion profile yet");
	assert_contains(reply.body, refresh_action);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *post = g_strdup_printf("/savings/target/%s/refresh", slug);
		g_autofree gchar *location = g_strdup_printf("/savings/target/%s?done=refresh-requested", slug);

		reply = post_as(fx, fx->viewer, post, "");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, post, "");
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), location);
		axis_test_reply_clear(&reply);
	}
	{
		g_autofree gchar *requests = query(fx, "SELECT count(*) FROM estimate_refresh_request");
		g_autofree gchar *jobs = query(fx, "SELECT count(*) FROM job WHERE kind = 'estimate.refresh'");

		g_assert_cmpstr(requests, ==, "1");
		g_assert_cmpstr(jobs, ==, "1");
	}
	g_assert_true(axis_estimator_refresh(fx->estimator, "direct", 0, NULL, NULL, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "What converting with Video: Mine would save");
	assert_contains(reply.body, "These estimates were computed for version 1 of this conversion profile, its current one.");
	assert_contains(reply.body, "rev=1");
	assert_csp_clean(reply.body);
	assert_links_safe(reply.body);
	g_assert_cmpuint(follow_figures(fx, reply.body), >=, 3);
	axis_test_reply_clear(&reply);
	/* The overview lists it, with its revision, until the next scheduled
	 * refresh leaves it out (it is not assigned). */
	reply = get_as(fx, fx->viewer, "/savings");
	assert_contains(reply.body, path);
	assert_contains(reply.body, "version 1");
	axis_test_reply_clear(&reply);
}

/* The answer of a form that stays on its page: it names the place in "at"
 * and ends "#place", and that page shows the notice inside the element. */
static void
assert_place(
	Fixture        *fx,
	AxisTestReply  *reply,
	const gchar    *place
){
	const gchar *location = axis_test_reply_header(reply, "Location");
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *path = NULL;
	const gchar *hash;
	const gchar *element;
	const gchar *notice;
	AxisTestReply page;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	hash = strrchr(location, '#');
	path = g_strndup(location, hash - location);
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

/* The forms of the profile pages come back to the card or row they were on. */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slug = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *row = NULL;
	g_autofree gchar *archive = NULL;
	g_autofree gchar *restore = NULL;
	AxisTestReply reply;

	SKIP_WITHOUT_DB(fx);
	slug = copy(fx, "balanced", "Spare");
	archive = g_strdup_printf("/profiles/%s/archive", slug);
	restore = g_strdup_printf("/profiles/%s/restore", slug);

	/* Archive and restore: the card "Change it". */
	reply = post_as(fx, fx->admin, archive, "");
	assert_place(fx, &reply, "change-it");
	reply = post_as(fx, fx->admin, restore, "");
	assert_place(fx, &reply, "change-it");

	/* Assign and unassign: the root's row. */
	row = g_strdup_printf("assign-row-%" G_GINT64_FORMAT, fx->s.films_a);
	body = g_strdup_printf("root=%" G_GINT64_FORMAT "&profile=space_saver&confirm=yes", fx->s.films_a);
	reply = post_as(fx, fx->admin, "/profiles/assign", body);
	assert_place(fx, &reply, row);
	g_free(body);
	body = g_strdup_printf("root=%" G_GINT64_FORMAT "&kind=video", fx->s.films_a);
	reply = post_as(fx, fx->admin, "/profiles/unassign", body);
	assert_place(fx, &reply, row);
}

#define ADD(name, func) g_test_add("/web/profiles/" name, Fixture, NULL, fixture_setup, func, fixture_teardown)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	ADD("list-and-profile", test_list_and_profile);
	ADD("refusals", test_refusals);
	ADD("copy-and-edit", test_copy_and_edit);
	ADD("assign", test_assign);
	ADD("unusable", test_unusable);
	ADD("custom-savings", test_custom_savings);
	ADD("back-to-place", test_back_to_place);
	return g_test_run();
}
