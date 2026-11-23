/*
 * test-web-quality.c - the Quality pages with the real shell on an
 * ephemeral loopback port and a real database: the list, a profile's page,
 * Try a name, copy, the edit form and its buttons (no script), delete, and
 * who may do what
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (quality-profiles): Try a name; Built-in.  Task 4.8: page:
 * administrators edit, others read; built-ins cannot be changed; a profile
 * in use cannot be deleted.
 *
 * With AXIS_TEST_QUALITY_PAGES set to a directory, the pages are also saved
 * there, for looking at in a browser.
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
#include "axis-test-web.h"
#include "core/axis-module-state.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

typedef struct
{
	AxisTestDb         *db;
	AxisDbPool         *pool;
	AxisSettings       *settings;
	AxisWebSettings    *web_settings;
	AxisQualityStore   *store;
	AxisWebQuality     *ui;
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
	fx->pool = axis_test_jobs_pool(fx->db, 6);
	/* As the daemon does at its start: the built-ins are the code's (the
	 * migration wrote their size limits as decimal megabytes). */
	g_assert_true(axis_quality_store_sync_builtins(fx->pool, NULL, &error));
	g_assert_no_error(error);
	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	fx->store = axis_quality_store_new(fx->pool, fx->settings);
	fx->ui = axis_web_quality_new(fx->store);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->ui, &error));
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
	axis_web_quality_stop(fx->ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_quality_free(fx->ui);
	axis_web_settings_free(fx->web_settings);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_quality_store_unref(fx->store);
	axis_settings_unref(fx->settings);
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

/* A form posted from a page the user may open: its CSRF token from there. */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply opened = get_as(fx, cookie, "/quality");
	g_autofree gchar *token = axis_test_web_csrf_token(opened.body);
	g_autofree gchar *escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	g_autofree gchar *body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");
	AxisTestReply reply;

	g_assert_nonnull(token);
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

/* Whether @body has a <@tag> whose start tag has every attribute of @attrs
 * ("name=\"value\""), in any order (the element builder's order is not
 * fixed). */
static gboolean
has_element(
	const gchar        *body,
	const gchar        *tag,
	const gchar *const *attrs
){
	g_autofree gchar *open = g_strdup_printf("<%s ", tag);
	const gchar *p = body;

	while (p != NULL && (p = strstr(p, open)) != NULL) {
		const gchar *end = strchr(p, '>');
		g_autofree gchar *start = NULL;
		guint i;

		if (end == NULL)
			return FALSE;
		start = g_strndup(p + 1, (gsize)(end - p - 1));
		for (i = 0; attrs[i] != NULL; i++) {
			g_autofree gchar *spaced = g_strdup_printf(" %s", attrs[i]);

			if (strstr(start, spaced) == NULL)
				break;
		}
		if (attrs[i] == NULL)
			return TRUE;
		p = end;
	}
	return FALSE;
}

#define assert_element(body, tag, ...) G_STMT_START { \
		const gchar *const attrs_[] = { __VA_ARGS__, NULL }; \
		if (!has_element((body), (tag), attrs_)) \
			g_error("no <%s> with %s in:\n%s", (tag), attrs_[0], (body)); \
	} G_STMT_END

#define assert_no_element(body, tag, ...) G_STMT_START { \
		const gchar *const attrs_[] = { __VA_ARGS__, NULL }; \
		if (has_element((body), (tag), attrs_)) \
			g_error("a <%s> with %s in:\n%s", (tag), attrs_[0], (body)); \
	} G_STMT_END

/* No style attribute or element and no script of ours. */
static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	assert_lacks(body, " onclick");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_QUALITY_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gchar *
copy_balanced(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *form = g_strdup_printf("name=%s", name);
	AxisTestReply reply = post_as(fx, fx->admin, "/quality/balanced/copy", form);
	const gchar *location;
	gchar *slug;

	g_assert_cmpuint(reply.status, ==, 303);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_true(g_str_has_prefix(location, "/quality/custom_"));
	g_assert_true(g_str_has_suffix(location, "?done=copied"));
	slug = g_strndup(location + strlen("/quality/"), strlen(location) - strlen("/quality/") - strlen("?done=copied"));
	axis_test_reply_clear(&reply);
	return slug;
}

/* The urlencoded body of the edit form for @p, as the page would post it,
 * with @op and @extra (nullable) after it (a later field wins). */
static gchar *
edit_body(
	const AxisQualityProfile *p,
	const gchar              *name,
	gint                      revision,
	const gchar              *op,
	const gchar              *extra
){
	GString *out = g_string_new(NULL);
	g_autofree gchar *escaped_name = g_uri_escape_string(name, NULL, TRUE);
	guint i;
	guint q;

	g_string_append_printf(out, "name=%s&revision=%d&qualities=", escaped_name, revision);
	for (i = 0; i < p->n_allowed; i++) {
		if (i > 0)
			g_string_append(out, p->tier[i] == p->tier[i - 1] ? "%2C" : "%3B");
		g_string_append(out, axis_quality_name(p->allowed[i]));
	}
	for (q = 1; q <= AXIS_QUALITY_N_LADDER; q++)
		g_string_append_printf(out, "&min_%s=%d&max_%s=%d", axis_quality_name((AxisQuality)q), p->sizes[q].min,
			axis_quality_name((AxisQuality)q), p->sizes[q].max);
	g_string_append_printf(out, "&cutoff=%s&max_size_gb=%" G_GINT64_FORMAT "&players=%d", axis_quality_name(p->cutoff),
		p->max_size_mb / 1000, p->players);
	if (p->allow_unknown)
		g_string_append(out, "&unknown=yes");
	if (p->allow_unwanted)
		g_string_append(out, "&unwanted=yes");
	if (p->upgrades)
		g_string_append(out, "&upgrades=yes");
	if (p->upgrade_revisions)
		g_string_append(out, "&revisions=yes");
	if (p->upgrade_score)
		g_string_append_printf(out, "&score=yes&min_score_gain=%d", p->min_score_gain);
	g_string_append_printf(out, "&n_rules=%u", p->n_rules);
	for (i = 0; i < p->n_rules; i++) {
		const AxisQualityRule *r = &p->rules[i];
		guint k;

		g_string_append_printf(out, "&rule%u_if=%s&rule%u_then=%s&rule%u_weight=%d&rule%u_is=", i + 1,
			axis_quality_condition_name(r->condition), i + 1, axis_quality_effect_name(r->effect), i + 1, r->weight,
			i + 1);
		for (k = 0; k < r->n_values; k++)
			g_string_append_printf(out, "%s%s", k > 0 ? "%2C" : "", r->values[k]);
	}
	g_string_append_printf(out, "&op=%s", op);
	if (extra != NULL)
		g_string_append_printf(out, "&%s", extra);
	return g_string_free(out, FALSE);
}

static AxisQualityStored *
stored(
	Fixture     *fx,
	const gchar *slug
){
	g_autoptr(GError) error = NULL;
	AxisQualityStored *s = NULL;

	g_assert_true(axis_quality_store_get(fx->store, slug, &s, &error));
	g_assert_no_error(error);
	return s;
}

/* ── The list and a profile ─────────────────────────────────────── */

static void
test_list(Fixture *fx, gconstpointer data)
{
	const AxisQualityBuiltin *b = axis_quality_builtins();
	AxisTestReply admin = get_as(fx, fx->admin, "/quality");
	AxisTestReply viewer = get_as(fx, fx->viewer, "/quality");
	AxisTestReply nobody = get_as(fx, NULL, "/quality");
	guint i;

	g_assert_cmpuint(admin.status, ==, 200);
	g_assert_cmpuint(viewer.status, ==, 200);
	g_assert_cmpuint(nobody.status, !=, 200);
	for (i = 0; i < AXIS_QUALITY_N_BUILTINS; i++) {
		g_autofree gchar *link = g_strdup_printf("href=\"/quality/%s\"", b[i].slug);

		assert_contains(viewer.body, link);
		assert_contains(viewer.body, b[i].name);
	}
	/* The navigation: Library, Quality, open. */
	assert_contains(admin.body, ">Library<");
	assert_element(admin.body, "a", "href=\"/quality\"", "aria-current=\"page\"");
	/* What the list says of a profile. */
	assert_contains(admin.body, "Default");
	assert_contains(admin.body, "BluRay 1080p, WEB-DL 1080p = WEBRip 1080p, HDTV 1080p");
	assert_contains(admin.body, "the default for new titles (a setting)");
	assert_contains(admin.body, "action=\"/quality/try\"");
	assert_contains(admin.body, "never taken");
	g_assert_cmpuint(axis_test_web_assert_tables(admin.body), ==, 1);
	assert_csp_clean(admin.body);
	save_page("quality-list.html", admin.body);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&nobody);
}

static void
test_profile_page(Fixture *fx, gconstpointer data)
{
	AxisTestReply admin = get_as(fx, fx->admin, "/quality/uhd");
	AxisTestReply viewer = get_as(fx, fx->viewer, "/quality/uhd");
	AxisTestReply missing = get_as(fx, fx->admin, "/quality/custom_999");
	AxisTestReply bad = get_as(fx, fx->admin, "/quality/Not-A-Slug");

	g_assert_cmpuint(admin.status, ==, 200);
	assert_contains(admin.body, "Quality profile: 4K");
	assert_contains(admin.body, "Upgraded until");
	assert_contains(admin.body, "WEB-DL 2160p (the cutoff)");
	assert_contains(admin.body, "Avoid Dolby Vision without an HDR10 fallback, by 100");
	assert_contains(admin.body, "Qualities and sizes");
	assert_contains(admin.body, "action=\"/quality/uhd/copy\"");
	assert_contains(admin.body, "A built-in profile cannot be changed or deleted");
	assert_lacks(admin.body, "action=\"/quality/uhd/delete\"");
	assert_lacks(admin.body, "href=\"/quality/uhd/edit\"");
	g_assert_cmpuint(axis_test_web_assert_tables(admin.body), ==, 1);
	assert_csp_clean(admin.body);
	/* Anyone may read it; only an administrator gets the forms. */
	g_assert_cmpuint(viewer.status, ==, 200);
	assert_contains(viewer.body, "Quality profile: 4K");
	assert_lacks(viewer.body, "action=\"/quality/uhd/copy\"");
	g_assert_cmpuint(missing.status, ==, 404);
	g_assert_cmpuint(bad.status, ==, 404);
	axis_test_reply_clear(&admin);
	axis_test_reply_clear(&viewer);
	axis_test_reply_clear(&missing);
	axis_test_reply_clear(&bad);
}

/* ── Who may do what ────────────────────────────────────────────── */

/* Administrators edit, others read; every change is a POST with its CSRF
 * token. */
static void
test_roles(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slug = copy_balanced(fx, "Mine");
	g_autofree gchar *edit = g_strdup_printf("/quality/%s/edit", slug);
	g_autofree gchar *delete_path = g_strdup_printf("/quality/%s/delete", slug);
	AxisTestReply reply;

	reply = get_as(fx, fx->viewer, edit);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, "/quality/balanced/copy", "name=Theirs");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, delete_path, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, edit, "op=save");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Without the token, not even an administrator. */
	reply = axis_test_web_request(fx->session, fx->port, "POST", delete_path, fx->admin, NULL, "");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* Nothing changes by a GET. */
	reply = get_as(fx, fx->admin, delete_path);
	g_assert_cmpuint(reply.status, ==, 405);
	axis_test_reply_clear(&reply);
	{
		g_autoptr(AxisQualityStored) s = stored(fx, slug);

		g_assert_nonnull(s);
	}
	/* Everyone may try a name. */
	reply = get_as(fx, fx->viewer, "/quality/try?name=Some.Film.2021.1080p.BluRay.x264-GRP&profile=balanced");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Accepted");
	axis_test_reply_clear(&reply);
}

/* Scenario: Built-in.  Changing one is refused, and copying it is offered. */
static void
test_builtin_refused(Fixture *fx, gconstpointer data)
{
	AxisQualityProfile balanced;
	g_autofree gchar *body = NULL;
	AxisTestReply reply;

	reply = get_as(fx, fx->admin, "/quality/balanced/edit");
	g_assert_cmpuint(reply.status, ==, 403);
	assert_contains(reply.body, "A built-in profile cannot be changed");
	assert_contains(reply.body, "action=\"/quality/balanced/copy\"");
	axis_test_reply_clear(&reply);

	g_assert_true(axis_quality_builtin_profile("balanced", &balanced));
	balanced.cutoff = AXIS_QUALITY_WEBDL_1080P;
	body = edit_body(&balanced, "Balanced", 1, "save", NULL);
	reply = post_as(fx, fx->admin, "/quality/balanced/edit", body);
	g_assert_cmpuint(reply.status, ==, 403);
	assert_contains(reply.body, "action=\"/quality/balanced/copy\"");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/quality/balanced/delete", NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	{
		g_autoptr(AxisQualityStored) s = stored(fx, "balanced");

		g_assert_cmpint(s->profile.cutoff, ==, AXIS_QUALITY_BLURAY_1080P);
		/* Revision 1 is what the migration wrote; the fixture's start-up sync
		 * made it 2, and the refused edit did not make it 3. */
		g_assert_cmpint(s->revision, ==, 2);
	}
}

/* ── Copy, edit, delete ─────────────────────────────────────────── */

static void
test_copy_and_edit(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slug = copy_balanced(fx, "My+profile");
	g_autofree gchar *page = g_strdup_printf("/quality/%s", slug);
	g_autofree gchar *edit = g_strdup_printf("/quality/%s/edit", slug);
	g_autoptr(AxisQualityStored) before = stored(fx, slug);
	AxisQualityProfile p = before->profile;
	AxisTestReply reply;

	g_assert_cmpstr(before->name, ==, "My profile");
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=copied", page);

		reply = get_as(fx, fx->admin, done);
	}
	assert_contains(reply.body, "The copy has been made");
	assert_contains(reply.body, "a copy of balanced");
	assert_contains(reply.body, edit);
	assert_contains(reply.body, "Delete this profile");
	axis_test_reply_clear(&reply);

	/* The form as it opens. */
	reply = get_as(fx, fx->admin, edit);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_element(reply.body, "input", "name=\"qualities\"", "value=\"bluray_1080p;webdl_1080p,webrip_1080p;hdtv_1080p\"");
	assert_contains(reply.body, "value=\"up:webdl_1080p\"");
	assert_contains(reply.body, "value=\"allow:remux_1080p\"");
	assert_element(reply.body, "input", "name=\"min_webdl_1080p\"", "value=\"4\"");
	assert_element(reply.body, "input", "name=\"n_rules\"", "value=\"3\"");
	/* Enter saves: the first button of the form is Save. */
	{
		const gchar *form = strstr(reply.body, "form-wide quality-form");
		const gchar *first = form != NULL ? strstr(form, "<button") : NULL;
		const gchar *end = first != NULL ? strchr(first, '>') : NULL;
		g_autofree gchar *tag = end != NULL ? g_strndup(first, (gsize)(end - first)) : NULL;

		g_assert_nonnull(tag);
		g_assert_nonnull(strstr(tag, " value=\"save\""));
		g_assert_nonnull(strstr(tag, " name=\"op\""));
	}
	assert_csp_clean(reply.body);
	save_page("quality-edit.html", reply.body);
	axis_test_reply_clear(&reply);

	/* Up: WEB-DL leaves its tier of equals and goes above it. */
	{
		g_autofree gchar *body = edit_body(&p, "My profile", 1, "up%3Awebdl_1080p", NULL);

		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "value=\"bluray_1080p;webdl_1080p;webrip_1080p;hdtv_1080p\"");
		axis_test_reply_clear(&reply);
	}
	/* Up again: above the BluRay. */
	{
		AxisQualityProfile moved = p;
		g_autofree gchar *body = NULL;

		moved.allowed[0] = AXIS_QUALITY_BLURAY_1080P;
		moved.allowed[1] = AXIS_QUALITY_WEBDL_1080P;
		moved.allowed[2] = AXIS_QUALITY_WEBRIP_1080P;
		moved.tier[0] = 0;
		moved.tier[1] = 1;
		moved.tier[2] = 2;
		moved.tier[3] = 3;
		body = edit_body(&moved, "My profile", 1, "up%3Awebdl_1080p", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		assert_contains(reply.body, "value=\"webdl_1080p;bluray_1080p;webrip_1080p;hdtv_1080p\"");
		axis_test_reply_clear(&reply);
		/* Equal to the one above. */
		g_free(body);
		body = edit_body(&moved, "My profile", 1, "join%3Ahdtv_1080p", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		assert_contains(reply.body, "value=\"bluray_1080p;webdl_1080p;webrip_1080p,hdtv_1080p\"");
		axis_test_reply_clear(&reply);
		/* Down from a tier of equals, and Remove, and Allow. */
		g_free(body);
		body = edit_body(&p, "My profile", 1, "down%3Awebdl_1080p", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		assert_contains(reply.body, "value=\"bluray_1080p;webrip_1080p;webdl_1080p;hdtv_1080p\"");
		axis_test_reply_clear(&reply);
		g_free(body);
		body = edit_body(&p, "My profile", 1, "remove%3Ahdtv_1080p", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		assert_contains(reply.body, "value=\"bluray_1080p;webdl_1080p,webrip_1080p\"");
		assert_contains(reply.body, "value=\"allow:hdtv_1080p\"");
		axis_test_reply_clear(&reply);
		g_free(body);
		body = edit_body(&p, "My profile", 1, "allow%3Aremux_1080p", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		assert_contains(reply.body, "value=\"bluray_1080p;webdl_1080p,webrip_1080p;hdtv_1080p;remux_1080p\"");
		axis_test_reply_clear(&reply);
	}
	/* Rules are added and removed; nothing is stored by these buttons. */
	{
		g_autofree gchar *body = edit_body(&p, "My profile", 1, "add_rule", NULL);

		reply = post_as(fx, fx->admin, edit, body);
		assert_element(reply.body, "input", "name=\"n_rules\"", "value=\"4\"");
		assert_contains(reply.body, "name=\"rule4_if\"");
		axis_test_reply_clear(&reply);
		g_free(body);
		body = edit_body(&p, "My profile", 1, "remove_rule%3A1", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		assert_element(reply.body, "input", "name=\"n_rules\"", "value=\"2\"");
		/* What was rule 2 (3D) is rule 1 now, and the codec rule is gone. */
		assert_element(reply.body, "option", "value=\"3d\"", "selected=\"selected\"");
		assert_element(reply.body, "option", "value=\"hardsub\"", "selected=\"selected\"");
		assert_no_element(reply.body, "option", "value=\"codec\"", "selected=\"selected\"");
		assert_lacks(reply.body, "name=\"rule3_if\"");
		axis_test_reply_clear(&reply);
		g_free(body);
		body = edit_body(&p, "My profile", 1, "launch%3Amissiles", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	{
		g_autoptr(AxisQualityStored) s = stored(fx, slug);

		g_assert_cmpint(s->revision, ==, 1);
	}

	/* A save with problems: each beside its field, every value kept. */
	{
		g_autofree gchar *body = edit_body(&p, "My profile", 1, "save",
			"min_webdl_1080p=abc&rule1_is=x266&cutoff=remux_2160p&max_size_gb=lots");

		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 422);
		assert_contains(reply.body, "The profile was not saved");
		assert_contains(reply.body, "A size is a whole number of MiB per minute");
		assert_element(reply.body, "input", "name=\"min_webdl_1080p\"", "value=\"abc\"");
		assert_contains(reply.body, "A value is not one this condition takes.");
		assert_contains(reply.body, "The largest release is a number of GiB");
		assert_contains(reply.body, "aria-invalid=\"true\"");
		assert_csp_clean(reply.body);
		save_page("quality-edit-invalid.html", reply.body);
		axis_test_reply_clear(&reply);
	}
	/* A good save. */
	{
		AxisQualityProfile changed = p;
		g_autofree gchar *body = NULL;
		g_autoptr(AxisQualityStored) s = NULL;

		changed.cutoff = AXIS_QUALITY_WEBDL_1080P;
		changed.allow_unknown = TRUE;
		changed.upgrade_score = TRUE;
		changed.min_score_gain = 40;
		changed.players = 60;
		body = edit_body(&changed, "Renamed", 1, "save", "rule1_is=h265%2Cav1");
		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 303);
		g_assert_true(g_str_has_suffix(axis_test_reply_header(&reply, "Location"), "?done=saved"));
		axis_test_reply_clear(&reply);
		s = stored(fx, slug);
		g_assert_cmpint(s->revision, ==, 2);
		g_assert_cmpstr(s->name, ==, "Renamed");
		g_assert_cmpint(s->profile.cutoff, ==, AXIS_QUALITY_WEBDL_1080P);
		g_assert_true(s->profile.allow_unknown);
		g_assert_cmpint(s->profile.min_score_gain, ==, 40);
		g_assert_cmpint(s->profile.players, ==, 60);
		g_assert_cmpuint(s->profile.rules[0].n_values, ==, 2);
		/* The same again: nothing changes. */
		g_free(body);
		body = edit_body(&s->profile, "Renamed", 2, "save", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 303);
		g_assert_true(g_str_has_suffix(axis_test_reply_header(&reply, "Location"), "?done=unchanged"));
		axis_test_reply_clear(&reply);
		/* A form opened before the save is refused, with the reason. */
		g_free(body);
		body = edit_body(&p, "Stale", 1, "save", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "saved by someone else");
		axis_test_reply_clear(&reply);
	}
	/* A name that is markup is text. */
	{
		AxisQualityProfile again;
		g_autofree gchar *body = NULL;
		g_autoptr(AxisQualityStored) s = stored(fx, slug);

		again = s->profile;
		body = edit_body(&again, "<b>bold</b> & co", 2, "save", NULL);
		reply = post_as(fx, fx->admin, edit, body);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, "/quality");
		assert_lacks(reply.body, "<b>bold</b>");
		assert_contains(reply.body, "&lt;b&gt;bold&lt;/b&gt; &amp; co");
		axis_test_reply_clear(&reply);
	}
}

/* A profile in use cannot be deleted; one that nothing uses can. */
static void
test_delete(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *slug = copy_balanced(fx, "Doomed");
	g_autofree gchar *kept = copy_balanced(fx, "Used");
	g_autofree gchar *delete_path = g_strdup_printf("/quality/%s/delete", slug);
	g_autofree gchar *delete_kept = g_strdup_printf("/quality/%s/delete", kept);
	g_autofree gchar *kept_page = g_strdup_printf("/quality/%s", kept);
	g_autofree gchar *id = axis_test_db_queryf(fx->db, "SELECT id FROM quality_profile WHERE slug = '%s'", kept);
	g_autofree gchar *insert = g_strdup_printf("INSERT INTO test_film (profile_id) VALUES (%s)", id);
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	/* A module that uses profiles. */
	axis_test_db_exec(fx->db, "CREATE TABLE test_film (profile_id bigint NOT NULL REFERENCES quality_profile (id))");
	g_assert_true(axis_quality_store_add_user(fx->store, "film", "films", "SELECT profile_id FROM test_film"));
	axis_test_db_exec(fx->db, insert);

	reply = get_as(fx, fx->admin, kept_page);
	assert_contains(reply.body, "1 film");
	assert_contains(reply.body, "It cannot be deleted while something uses it.");
	assert_lacks(reply.body, delete_kept);
	save_page("quality-profile.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, delete_kept, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "The profile is in use: 1 film.");
	axis_test_reply_clear(&reply);

	/* The default for new titles is in use too. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_QUALITY_SETTING_DEFAULT, slug, &error));
	reply = post_as(fx, fx->admin, delete_path, NULL);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "the default for new titles");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_settings_set(fx->settings, AXIS_QUALITY_SETTING_DEFAULT, "balanced", &error));

	reply = post_as(fx, fx->admin, delete_path, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, "/quality?done=deleted");
	axis_test_reply_clear(&reply);
	{
		g_autoptr(AxisQualityStored) gone = stored(fx, slug);
		g_autoptr(AxisQualityStored) still = stored(fx, kept);

		g_assert_null(gone);
		g_assert_nonnull(still);
	}
	reply = get_as(fx, fx->admin, "/quality?done=deleted");
	assert_contains(reply.body, "The profile has been deleted.");
	axis_test_reply_clear(&reply);
}

/* ── Try a name ─────────────────────────────────────────────────── */

static gchar *
try_path(
	const gchar *name,
	const gchar *profile,
	const gchar *runtime,
	const gchar *size
){
	g_autofree gchar *n = g_uri_escape_string(name, NULL, TRUE);
	g_autofree gchar *s = size != NULL ? g_uri_escape_string(size, NULL, TRUE) : g_strdup("");

	return g_strdup_printf("/quality/try?name=%s&profile=%s&runtime=%s&size=%s", n, profile, runtime != NULL ?
		runtime : "", s);
}

/* Scenario: Try a name.  The page shows what was read and the decision with
 * its reasons. */
static void
test_try(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *accepted = try_path("Some.Film.2021.1080p.WEB-DL.DDP5.1.x265-GRP", "space_saver", "97",
		"2 GiB");
	g_autofree gchar *rejected = try_path("Some.Film.2021.HDCAM.x264-GRP-sample", "space_saver", "97", "2.9 GiB");
	g_autofree gchar *too_big = try_path("Some.Film.2021.1080p.WEB-DL.DDP5.1.H.264-GRP", "space_saver", "97",
		"2.9 GiB");
	g_autofree gchar *decimal = try_path("Some.Film.2021.1080p.WEB-DL.DDP5.1.H.264-GRP", "space_saver", "97",
		"3.1 GB");
	g_autofree gchar *unit_less = try_path("Some.Film.2021.1080p.WEB-DL.DDP5.1.H.264-GRP", "space_saver", "97",
		"2.9");
	g_autofree gchar *markup = try_path("<script>alert(1)</script>.2021.1080p.BluRay.x264-GRP", "balanced", NULL, NULL);
	g_autofree gchar *bad_runtime = try_path("Some.Film.2021.1080p.BluRay.x264-GRP", "balanced", "soon", NULL);
	g_autofree gchar *bad_size = try_path("Some.Film.2021.1080p.BluRay.x264-GRP", "balanced", "97", "big");
	g_autofree gchar *no_profile = try_path("Some.Film.2021.1080p.BluRay.x264-GRP", "custom_999", NULL, NULL);
	AxisTestReply reply;

	reply = get_as(fx, fx->viewer, accepted);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "What the release name says");
	assert_contains(reply.body, "Some Film");
	assert_contains(reply.body, "<dt>Rank among qualities</dt><dd>WEB-DL 1080p</dd>");
	assert_contains(reply.body, "<dt>Source</dt><dd>WEB-DL</dd>");
	assert_contains(reply.body, "HEVC (x265)");
	assert_contains(reply.body, "The decision with Small 1080p");
	assert_contains(reply.body, ">Accepted<");
	assert_contains(reply.body, "+100 for HEVC (x265), which this profile prefers");
	assert_contains(reply.body, "Not matched against a title");
	/* The form keeps what was tried. */
	assert_contains(reply.body, "value=\"Some.Film.2021.1080p.WEB-DL.DDP5.1.x265-GRP\"");
	assert_element(reply.body, "option", "value=\"space_saver\"", "selected=\"selected\"");
	assert_csp_clean(reply.body);
	save_page("quality-try-accepted.html", reply.body);
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, rejected);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Rejected<");
	assert_contains(reply.body, "it is a sample, not the whole movie");
	assert_contains(reply.body, "Cam, by its source alone, whatever its resolution");
	assert_contains(reply.body, "Fixed rule");
	assert_contains(reply.body, "Cam is a cinema recording or a pre-release copy");
	assert_contains(reply.body, "Cam is not one of this profile&#39;s qualities");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, too_big);
	assert_contains(reply.body, ">Rejected<");
	assert_contains(reply.body, "Rejected, for 1 reason:");
	assert_contains(reply.body, "2.9\302\240GiB is above the 2.4\302\240GiB limit for a 97-minute movie at WEB-DL 1080p");
	assert_lacks(reply.body, " GB ");
	assert_lacks(reply.body, " MB ");
	axis_test_reply_clear(&reply);

	/* A size typed in decimal units is what it says (3.1 GB is 2.9 GiB), and
	 * a bare number is in GiB; the sentences are in binary units either way. */
	reply = get_as(fx, fx->admin, decimal);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "2.9\302\240GiB is above the 2.4\302\240GiB limit for a 97-minute movie at WEB-DL 1080p");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, unit_less);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "2.9\302\240GiB is above the 2.4\302\240GiB limit for a 97-minute movie at WEB-DL 1080p");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, rejected);
	save_page("quality-try-rejected.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A name is text, never markup. */
	reply = get_as(fx, fx->admin, markup);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "<script>alert");
	assert_contains(reply.body, "&lt;script&gt;");
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, bad_runtime);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "The runtime is a whole number of minutes");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, bad_size);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "The size is a number with its unit");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, no_profile);
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	/* The empty page, and a question it does not take. */
	reply = get_as(fx, fx->admin, "/quality/try");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "What the release name says");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/quality/try?grab=yes");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web/quality/" path, Fixture, NULL, func)
	ADD("list", test_list);
	ADD("profile", test_profile_page);
	ADD("roles", test_roles);
	ADD("built-in", test_builtin_refused);
	ADD("copy-edit", test_copy_and_edit);
	ADD("delete", test_delete);
	ADD("try", test_try);
#undef ADD
	return g_test_run();
}
