/*
 * test-web-setup.c - the setup guide, with the real shell on an ephemeral
 * loopback port, the real Settings, Storage, Indexers, Download clients,
 * Quality and Identify pages it shows the forms of, the metadata test
 * provider, the fake Prowlarr, the fixture download client, a real database
 * and a scratch directory for the disk
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios (setup): Leave and return; Not used; Wrong key; TMDb.
 * Tasks 2.1 to 2.6:
 *
 *   /web/setup/walk           a fresh installation walked through the guide
 *                             until a film can be searched for and
 *                             downloaded; each step done only by its check
 *   /web/setup/choices        not now, not used, asked for again; the
 *                             dashboard card follows; another browser sees
 *                             the same
 *   /web/setup/both-ways      a value set in the guide is the one on its own
 *                             page, and the reverse
 *   /web/setup/return         a "return" that is not the guide's own address
 *                             is ignored
 *   /web/setup/links          outbound links open in a new tab without a
 *                             referrer; a configured service's link is built
 *                             from its configured address
 *   /web/setup/admin-only     403 for anyone else, and no secret in a page
 *   /web/setup/tmdb           the titles step with TMDb, and a refused key
 *
 * With AXIS_TEST_SETUP_PAGES set to a directory, the pages are also saved
 * there for looking at in a browser (task 2.6).
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
#include "axis-test-download.h"
#include "axis-test-jobs.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-prowlarr.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "acquire/axis-acquire-ready.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "identify/axis-identify.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-newznab.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tmdb.h"
#include "modules/movies/axis-movies.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-identify.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-ready.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-setup.h"
#include "web/axis-web-setup-private.h"
#include "web/axis-web-storage.h"

#define KEY       "prowlarr-key-for-the-guide-7c41"
#define WRONG_KEY "prowlarr-key-that-is-wrong-0000"
#define PASSWORD  AXIS_TEST_DOWNLOAD_PASSWORD

/* A TMDb key in the shape TMDb's short API Key has (32 hex digits). */
#define SHORT_TMDB_KEY "0123456789abcdef0123456789abcdef"

typedef struct
{
	AxisTestDb           *db;
	AxisDbPool           *pool;
	gchar                *dir;
	gchar                *disk;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisStorage          *storage;
	AxisWebStorage       *storage_ui;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisTestProwlarr     *prowlarr;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisIdentify         *identify;
	AxisWebIdentify      *identify_ui;
	AxisMovies           *movies;
	AxisWebReady         *ready_ui;
	AxisWebSetup         *setup_ui;
	AxisModuleRegistry   *registry;
	HtmxServer           *server;
	AxisWebAuth          *auth;
	AxisWeb              *web;
	SoupSession          *session;
	guint16               port;
	gchar                *admin;
	gchar                *admin2;	/* the same administrator in another browser */
	gchar                *viewer;
	gboolean              tmdb;	/* TMDb is the provider (never asked anything) */
} Fixture;

static void
set_route(
	Fixture     *fx,
	const gchar *path,
	guint        status,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->fa->api, path, status, "application/json", body, NULL);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *films = NULL;
	g_autofree gchar *downloads = NULL;
	AxisMetaTuning meta_tuning;
	AxisIndexerTuning indexer_tuning;
	AxisDownloadTuning download_tuning;
	AxisWebSetupLayers layers;

	memset(fx, 0, sizeof(*fx));
	fx->tmdb = g_strcmp0(data, "tmdb") == 0;
	axis_test_log_init();
	axis_test_log_reset();
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*not a mount point*");
	axis_session_store_set_idle_seconds(30 * 24 * 3600);

	fx->dir = axis_test_storage_dir_new();
	fx->db = axis_test_db_new();
	if (fx->db == NULL)
		return;
	fx->pool = axis_test_jobs_pool(fx->db, 12);
	g_assert_true(axis_quality_store_sync_builtins(fx->pool, NULL, &error));
	g_assert_no_error(error);
	fx->disk = g_build_filename(fx->dir, "media", "disk1", NULL);
	films = g_build_filename(fx->disk, "films", NULL);
	downloads = g_build_filename(fx->disk, "downloads", "complete", NULL);
	g_assert_cmpint(g_mkdir_with_parents(films, 0755), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(downloads, 0755), ==, 0);
	path = g_build_filename(fx->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->pool);
	fx->storage = axis_storage_new(fx->pool, fx->settings);
	axis_storage_set_timeouts(fx->storage, 30000, 30000);
	fx->storage_ui = axis_web_storage_new(fx->storage, NULL);
	fx->meta = axis_meta_new(fx->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	if (fx->tmdb) {
		g_assert_true(axis_meta_add_provider(fx->meta, axis_meta_tmdb_get_info(), &error));
	} else {
		fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
		g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	}
	g_assert_no_error(error);
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_newznab_kind(), &error));
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &indexer_tuning);
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->pool, fx->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(fx->downloads, &download_tuning);
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->storage, NULL);
	fx->quality = axis_quality_store_new(fx->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->identify = axis_identify_new(fx->pool, fx->settings, fx->storage, fx->meta);
	fx->identify_ui = axis_web_identify_new(fx->identify, fx->quality);
	fx->movies = axis_movies_new(fx->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->storage,
		fx->quality);
	fx->ready_ui = axis_web_ready_new(fx->meta, fx->indexers, fx->downloads, fx->storage);
	memset(&layers, 0, sizeof(layers));
	layers.settings = fx->settings;
	layers.meta = fx->meta;
	layers.indexers = fx->indexers;
	layers.downloads = fx->downloads;
	layers.storage = fx->storage;
	layers.quality = fx->quality;
	layers.identify = fx->identify;
	fx->setup_ui = axis_web_setup_new(&layers);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_storage_get_module(), fx->storage_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_downloads_get_module(), fx->downloads_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_indexers_get_module(), fx->indexers_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_identify_get_module(), fx->identify_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), fx->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_ready_get_module(), fx->ready_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_setup_get_module(), fx->setup_ui, &error));
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
	g_assert_true(htmx_server_start(fx->server, &error));
	g_assert_true(axis_module_registry_start_all(fx->registry, &error));
	g_assert_no_error(error);
	fx->admin = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->admin2 = axis_web_auth_open_test_session(fx->auth, "root", AXIS_USER_ROLE_ADMIN);
	fx->viewer = axis_web_auth_open_test_session(fx->auth, "viewer", AXIS_USER_ROLE_USER);
	fx->port = axis_test_web_port(fx->server);
	fx->session = soup_session_new_with_options("timeout", 30, "idle-timeout", 30, NULL);

	if (fx->fa != NULL) {
		axis_test_meta_fixture_standard(fx->fa);
		set_route(fx, "/search/film", 200, "{\"results\":[{\"id\":\"101\",\"kind\":\"film\",\"title\":\"" AXIS_WEB_SETUP_CHECK_TITLE "\","
			"\"year\":1999,\"ids\":{\"imdb\":\"tt0600003\"}}]}");
	}
	fx->prowlarr = axis_test_prowlarr_new(KEY, "");
	fx->fake = axis_test_download_fake_new(PASSWORD);
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
	axis_web_stop(fx->web);
	axis_web_setup_stop(fx->setup_ui);
	axis_movies_stop(fx->movies);
	axis_web_identify_stop(fx->identify_ui);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_storage_stop(fx->storage_ui);
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
	axis_web_setup_free(fx->setup_ui);
	axis_web_ready_free(fx->ready_ui);
	axis_movies_unref(fx->movies);
	axis_web_identify_free(fx->identify_ui);
	axis_identify_unref(fx->identify);
	axis_web_quality_free(fx->quality_ui);
	axis_quality_store_unref(fx->quality);
	axis_web_downloads_free(fx->downloads_ui);
	axis_downloads_unref(fx->downloads);
	axis_web_indexers_free(fx->indexers_ui);
	axis_indexers_unref(fx->indexers);
	axis_web_meta_free(fx->meta_ui);
	axis_meta_unref(fx->meta);
	axis_web_storage_free(fx->storage_ui);
	axis_storage_stop(fx->storage);
	axis_storage_unref(fx->storage);
	axis_web_settings_free(fx->web_settings);
	if (fx->fa != NULL)
		axis_test_meta_fixture_free(fx->fa);
	axis_test_prowlarr_free(fx->prowlarr);
	axis_test_download_fake_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	axis_db_pool_free(fx->pool);
	axis_test_db_free(fx->db);
	axis_test_storage_dir_free(fx->dir);
	g_free(fx->disk);
	g_free(fx->admin);
	g_free(fx->admin2);
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

static gchar *
form_of(const gchar *first, ...) G_GNUC_NULL_TERMINATED;

/* "name=value&..." of name, value pairs, ended by %NULL. */
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

/* A form posted by @cookie, with the CSRF token of the dashboard it has open. */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *path,
	const gchar *form
){
	AxisTestReply opened = get_as(fx, cookie, "/");
	g_autofree gchar *token = axis_test_web_csrf_token(opened.body);
	g_autofree gchar *escaped = NULL;
	g_autofree gchar *body = NULL;

	g_assert_nonnull(token);
	axis_test_reply_clear(&opened);
	escaped = g_uri_escape_string(token, NULL, TRUE);
	body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

/* Posts and expects a redirect to @location (then, where a page comes back
 * to a place, "&at=" and the place). */
static void
post_expect(
	Fixture     *fx,
	const gchar *path,
	const gchar *form,
	const gchar *location
){
	AxisTestReply reply = post_as(fx, fx->admin, path, form);

	if (reply.status != 303)
		g_error("POST %s answered %u, not a redirect to %s:\n%s", path, reply.status, location, reply.body);
	{
		const gchar *got = axis_test_reply_header(&reply, "Location");

		g_assert_nonnull(got);
		if (!g_str_has_prefix(got, location) ||
		    (got[strlen(location)] != '\0' && !g_str_has_prefix(got + strlen(location), "&at=")))
			g_error("POST %s redirected to %s, not %s", path, got, location);
	}
	axis_test_reply_clear(&reply);
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

/* Whether @html has a tag holding every one of @parts (attributes, in any
 * order; "<name " for the element's name). */
static gboolean
has_tag(
	const gchar        *html,
	const gchar *const *parts
){
	g_autoptr(GRegex) tag = g_regex_new("<[a-z]+[^>]*>", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	g_regex_match(tag, html, 0, &match);
	while (g_match_info_matches(match)) {
		g_autofree gchar *text = g_match_info_fetch(match, 0);
		guint i;

		for (i = 0; parts[i] != NULL && strstr(text, parts[i]) != NULL; i++)
			;
		if (parts[i] == NULL)
			return TRUE;
		g_match_info_next(match, NULL);
	}
	return FALSE;
}

static void
assert_tag(
	const gchar *html,
	...
) G_GNUC_NULL_TERMINATED;

static void
assert_tag(
	const gchar *html,
	...
){
	g_autoptr(GPtrArray) parts = g_ptr_array_new();
	const gchar *part;
	va_list ap;

	va_start(ap, html);
	while ((part = va_arg(ap, const gchar *)) != NULL)
		g_ptr_array_add(parts, (gpointer)part);
	va_end(ap);
	g_ptr_array_add(parts, NULL);
	if (!has_tag(html, (const gchar *const *)parts->pdata)) {
		g_autofree gchar *wanted = g_strjoinv(" ", (gchar **)parts->pdata);

		g_error("expected a tag with %s in:\n%s", wanted, html);
	}
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

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_SETUP_PAGES");
	g_autofree gchar *file = NULL;
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	file = g_strdup_printf("%s.html", name);
	path = g_build_filename(dir, file, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gchar *
q(
	Fixture     *fx,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	return axis_test_db_query(fx->db, sql);
}

/* The value of the input called @name in @html (the first), or %NULL. */
static gchar *
input_value(
	const gchar *html,
	const gchar *name
){
	g_autofree gchar *pattern = g_strdup_printf("<input[^>]*name=\"%s\"[^>]*>", name);
	g_autoptr(GRegex) input = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GRegex) value = g_regex_new("value=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) found = NULL;
	g_autoptr(GMatchInfo) got = NULL;
	g_autofree gchar *tag = NULL;

	if (!g_regex_match(input, html, 0, &found))
		return NULL;
	tag = g_match_info_fetch(found, 0);
	if (!g_regex_match(value, tag, 0, &got))
		return NULL;
	return g_match_info_fetch(got, 1);
}

/* The step's page, as the administrator sees it, saved as @save (nullable). */
static gchar *
step_page(
	Fixture     *fx,
	const gchar *slug,
	const gchar *save
){
	g_autofree gchar *path = g_strdup_printf("/setup-guide/%s", slug);
	AxisTestReply reply = get_as(fx, fx->admin, path);
	gchar *body;

	if (reply.status != 200)
		g_error("GET %s answered %u:\n%s", path, reply.status, reply.body);
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	if (save != NULL)
		save_page(save, reply.body);
	body = g_strdup(reply.body);
	axis_test_reply_clear(&reply);
	return body;
}

/* The state word a step's page shows for it ("done", "needed", ...). */
static gchar *
state_of(const gchar *page)
{
	static const gchar lead[] = "<p class=\"guide-state-line\"><span class=\"badge guide-badge-";
	const gchar *at = strstr(page, lead);
	const gchar *end;

	if (at == NULL)
		g_error("no state on the page:\n%s", page);
	at += strlen(lead);
	end = at + strcspn(at, " \"");
	return g_strndup(at, (gsize)(end - at));
}

static void
assert_page_state(
	const gchar *page,
	const gchar *state
){
	g_autofree gchar *found = state_of(page);

	if (g_strcmp0(found, state) != 0)
		g_error("the step is %s, not %s:\n%s", found, state, page);
}

static void
assert_state(
	Fixture     *fx,
	const gchar *slug,
	const gchar *state,
	const gchar *save
){
	g_autofree gchar *page = step_page(fx, slug, save);
	g_autofree gchar *found = state_of(page);

	if (g_strcmp0(found, state) != 0)
		g_error("the step %s is %s, not %s:\n%s", slug, found, state, page);
}

/* The dashboard's Getting started card as @cookie sees it, or %NULL. */
static gchar *
setup_card(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *save
){
	AxisTestReply board = get_as(fx, cookie, "/");
	const gchar *start;
	gchar *card = NULL;

	g_assert_cmpuint(board.status, ==, 200);
	start = strstr(board.body, "<section id=\"dash-setup\"");
	if (start != NULL)
		card = g_strndup(start, (gsize)(strstr(start, "</section>") - start));
	if (save != NULL)
		save_page(save, board.body);
	axis_test_reply_clear(&board);
	return card;
}

static const gchar *const all_steps[] = {
	"titles", "library", "indexers", "clients", "quality", "players", "writing", "existing", NULL
};

/* Adds the disk as a volume through the guide's form and the Storage page's
 * confirmation, as a browser would: the confirmation's own fields. */
static void
add_disk_through_guide(Fixture *fx)
{
	g_autofree gchar *look = form_of("path", fx->disk, "name", "Disk one", "return", "/setup-guide/library", NULL);
	AxisTestReply reply = post_as(fx, fx->admin, "/storage/volumes/inspect", look);
	static const gchar *const fields[] = { "path", "plan", "dev", "ino", "fsid", "size", "confirm", "return", NULL };
	GString *form = g_string_new(NULL);
	guint i;

	g_assert_cmpuint(reply.status, ==, 200);
	/* The real confirmation, carrying the way back. */
	assert_contains(reply.body, "Write the marker and add the disk");
	assert_tag(reply.body, "type=\"hidden\"", "name=\"return\"", "value=\"/setup-guide/library\"", NULL);
	save_page("library-confirm", reply.body);
	for (i = 0; fields[i] != NULL; i++) {
		g_autofree gchar *value = input_value(reply.body, fields[i]);
		g_autofree gchar *escaped = NULL;

		g_assert_nonnull(value);
		escaped = g_uri_escape_string(value, NULL, FALSE);
		g_string_append_printf(form, "%s%s=%s", form->len > 0 ? "&" : "", fields[i], escaped);
	}
	g_string_append(form, "&name=Disk%20one&not_mount_point_ok=yes");
	axis_test_reply_clear(&reply);
	post_expect(fx, "/storage/volumes", form->str, "/setup-guide/library?done=added");
	g_string_free(form, TRUE);
}

/* ── 2.1: a fresh installation, walked through ──────────────────── */

static void
test_walk(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
	g_autofree gchar *client_base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *form = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *client = NULL;
	g_autofree gchar *volume = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *card = NULL;
	AxisReadiness ready;
	AxisTestReply reply;
	guint i;

	/* Fresh: nothing is set up, and the guide starts at the first step. */
	reply = get_as(fx, fx->admin, "/setup-guide");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<h1>Setup guide</h1><p class=\"page-intro\">");
	/* (What to download has its built-in default from the start.) */
	assert_contains(reply.body, "1 of the 6 steps Axis needs is done");
	assert_contains(reply.body, "Start with step 1: Details from TMDb");
	assert_tag(reply.body, "class=\"guide-overview-link\"", "href=\"/setup-guide\"", "aria-current=\"page\"", NULL);
	assert_csp_clean(reply.body);
	save_page("overview-fresh", reply.body);
	axis_test_reply_clear(&reply);
	for (i = 0; all_steps[i] != NULL; i++) {
		g_autofree gchar *name = g_strdup_printf("%s-fresh", all_steps[i]);
		g_autofree gchar *shown = step_page(fx, all_steps[i], name);
		g_autofree gchar *state = state_of(shown);

		/* Nothing is done by being looked at; quality has its built-in
		 * default from the start, and players are not part of this Axis. */
		if (strcmp(all_steps[i], "quality") == 0)
			g_assert_cmpstr(state, ==, "done");
		else if (strcmp(all_steps[i], "players") == 0)
			g_assert_cmpstr(state, ==, "off");
		else if (strcmp(all_steps[i], "existing") == 0)
			g_assert_cmpstr(state, ==, "optional");
		else
			g_assert_cmpstr(state, ==, "needed");
	}
	/* The dashboard offers the guide, each item linking into its step. */
	card = setup_card(fx, fx->admin, "dashboard-fresh");
	g_assert_nonnull(card);
	assert_contains(card, "<a class=\"ready-link\" href=\"/setup-guide/titles\">Add a TMDb token</a>");
	/* Spec (web-shell, tidy-labels): One word for the TMDb credential. */
	assert_lacks(card, "metadata key");
	assert_contains(card, "<a class=\"ready-link\" href=\"/setup-guide/indexers\">Set up an indexer</a>");
	assert_contains(card, "<a class=\"ready-link\" href=\"/setup-guide/library\">Add a library folder for Movies</a>");
	assert_contains(card, "<a href=\"/setup-guide\">Open the setup guide</a>");
	g_clear_pointer(&card, g_free);

	/* 1. The key, through the Settings page's own handler. */
	page = step_page(fx, "titles", NULL);
	assert_contains(page, "action=\"/admin/settings/metadata\"");
	assert_contains(page, "name=\"metadata.fixture_key\"");
	assert_tag(page, "type=\"hidden\"", "name=\"return\"", "value=\"/setup-guide/titles\"", NULL);
	g_clear_pointer(&page, g_free);
	form = form_of("metadata.fixture_key", AXIS_TEST_META_KEY, "return", "/setup-guide/titles", NULL);
	post_expect(fx, "/admin/settings/metadata", form, "/setup-guide/titles?done=saved");
	g_clear_pointer(&form, g_free);
	assert_state(fx, "titles", "done", NULL);
	/* Its own test: a lookup through the layer, said in words. */
	reply = post_as(fx, fx->admin, "/setup-guide/titles/check", NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "answered: it knows 1 movie called " AXIS_WEB_SETUP_CHECK_TITLE " from 1999. The token "
		"works.");
	save_page("titles-done", reply.body);
	axis_test_reply_clear(&reply);

	/* 2. The disk and a root, through the Storage pages' handlers. */
	add_disk_through_guide(fx);
	page = step_page(fx, "library", NULL);
	assert_page_state(page, "needed");
	assert_contains(page, "No library folder for movies is set up");
	g_clear_pointer(&page, g_free);
	volume = q(fx, "SELECT id FROM volume");
	form = form_of("volume", volume, "path", "films", "type", "movies", "name", "", "return", "/setup-guide/library",
		NULL);
	post_expect(fx, "/storage/roots", form, "/setup-guide/library?done=root-added");
	g_clear_pointer(&form, g_free);
	assert_state(fx, "library", "done", "library-done");

	/* 3. add-indexer-newznab: the step offers a Usenet indexer, with where
	 * its key is, beside Prowlarr, and says both are possible. */
	page = step_page(fx, "indexers", "indexers-start");
	assert_page_state(page, "needed");
	assert_contains(page, "Add a Usenet indexer");
	assert_contains(page, "in your profile or on its API page");
	assert_contains(page, "https://api.example.com");
	assert_contains(page, "Or connect Prowlarr");
	assert_contains(page, "your Usenet indexers, which Axis asks itself with the API key each gives you, and "
		"Prowlarr if you use it");
	assert_lacks(page, "never the indexers");
	assert_contains(page, "<input type=\"hidden\" name=\"kind\" value=\"newznab\" />");
	assert_contains(page, "<input type=\"hidden\" name=\"kind\" value=\"prowlarr\" />");
	g_clear_pointer(&page, g_free);

	/* Then Prowlarr, first with a key it refuses: not done, and it says so. */
	form = form_of("name", "Prowlarr", "kind", AXIS_INDEXER_PROWLARR_NAME, "address", base, "base_path", "",
		"secret", WRONG_KEY, "enabled", "yes", "return", "/setup-guide/indexers", NULL);
	post_expect(fx, "/indexers", form, "/setup-guide/indexers?done=added");
	g_clear_pointer(&form, g_free);
	page = step_page(fx, "indexers", NULL);
	assert_page_state(page, "needed");
	assert_contains(page, "Prowlarr is saved but not tested yet");
	g_clear_pointer(&page, g_free);
	source = q(fx, "SELECT id FROM indexer_source");
	path = g_strdup_printf("/indexers/%s/test", source);
	post_expect(fx, path, "return=%2Fsetup-guide%2Findexers", "/setup-guide/indexers?done=tested");
	page = step_page(fx, "indexers", "indexers-failed");
	assert_page_state(page, "needed");
	assert_contains(page, "Prowlarr refused the key");
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	/* The right key, through the source's own form, and its test. */
	path = g_strdup_printf("/indexers/%s", source);
	form = form_of("name", "Prowlarr", "address", base, "base_path", "", "secret", KEY, "enabled", "yes", "return",
		"/setup-guide/indexers", NULL);
	post_expect(fx, path, form, "/setup-guide/indexers?done=saved");
	g_clear_pointer(&form, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/indexers/%s/test", source);
	post_expect(fx, path, "return=%2Fsetup-guide%2Findexers", "/setup-guide/indexers?done=tested");
	g_clear_pointer(&path, g_free);
	page = step_page(fx, "indexers", "indexers-done");
	assert_page_state(page, "done");
	assert_contains(page, "Prowlarr answered and reports");
	g_clear_pointer(&page, g_free);

	/* 4. A download client: tested, then a folder of its that maps nowhere,
	 * then its mapping. */
	form = form_of("name", "SABnzbd", "kind", "fixturenzb", "address", client_base, "base_path", "", "secret",
		PASSWORD, "category", "axis", "priority", "normal", "enabled", "yes", "return", "/setup-guide/clients", NULL);
	post_expect(fx, "/downloads/clients", form, "/setup-guide/clients?done=added");
	g_clear_pointer(&form, g_free);
	client = q(fx, "SELECT id FROM download_client");
	path = g_strdup_printf("/downloads/clients/%s/test", client);
	post_expect(fx, path, "return=%2Fsetup-guide%2Fclients", "/setup-guide/clients?done=tested");
	g_clear_pointer(&path, g_free);
	page = step_page(fx, "clients", NULL);
	assert_page_state(page, "needed");
	assert_contains(page, "No folder of its is mapped yet");
	/* The step names qBittorrent: its login, its documentation, and a plain note about bans. */
	assert_contains(page, "Deluge or qBittorrent for torrents");
	assert_contains(page, "qBittorrent&#39;s user name and password");
	assert_contains(page, "make qBittorrent ban Axis&#39;s address for an hour");
	assert_contains(page, "https://github.com/qbittorrent/qBittorrent/wiki");
	g_clear_pointer(&page, g_free);
	/* What the client reported for a finished download, as tracking records
	 * it when the path maps nowhere. */
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO download (client_id, client_name, client_ref, protocol, "
			"source, release_name, state, client_path, problem) VALUES (%s, 'SABnzbd', 'nzo_walk', 'usenet', 'file', "
			"'Hearth.1995.1080p.BluRay.x264-GRP', 'downloading', '/downloads/complete/Hearth.1995.1080p.BluRay.x264-GRP', "
			"'Finished at the download client, but cannot be imported yet: no mapping contains it.')", client);

		axis_test_db_exec(fx->db, sql);
	}
	page = step_page(fx, "clients", "clients-failed");
	assert_page_state(page, "needed");
	assert_contains(page, "The client&#39;s folder /downloads/complete maps to no disk Axis knows: add a mapping.");
	g_clear_pointer(&page, g_free);
	path = g_strdup_printf("/downloads/clients/%s/maps", client);
	form = form_of("prefix", "/downloads", "volume", volume, "rel_path", "downloads", "return", "/setup-guide/clients",
		NULL);
	post_expect(fx, path, form, "/setup-guide/clients?done=map-added");
	g_clear_pointer(&form, g_free);
	g_clear_pointer(&path, g_free);
	page = step_page(fx, "clients", "clients-done");
	assert_page_state(page, "done");
	assert_contains(page, "Same disk as films: imports from here are renames (or hardlinks), not copies.");
	g_clear_pointer(&page, g_free);

	/* 5. The default profile, through the Settings handler. */
	form = form_of("quality.default_profile", "best_1080p", "return", "/setup-guide/quality", NULL);
	post_expect(fx, "/admin/settings/quality", form, "/setup-guide/quality?done=saved");
	g_clear_pointer(&form, g_free);
	page = step_page(fx, "quality", "quality-done");
	assert_page_state(page, "done");
	assert_contains(page, "Best 1080p is the default for new titles.");
	g_clear_pointer(&page, g_free);

	/* 7. Writing, through the setting's own form. */
	page = step_page(fx, "writing", NULL);
	assert_contains(page, "action=\"/admin/settings/storage\"");
	assert_contains(page, "name=\"storage.read_only\"");
	g_clear_pointer(&page, g_free);
	form = form_of("storage.read_only", "false", "return", "/setup-guide/writing", NULL);
	post_expect(fx, "/admin/settings/storage", form, "/setup-guide/writing?done=saved");
	g_clear_pointer(&form, g_free);
	assert_state(fx, "writing", "done", "writing-done");

	/* 8. Recognising: queued through Identify's own handler. */
	root = q(fx, "SELECT id FROM library_root");
	path = g_strdup_printf("/identify/roots/%s/run", root);
	post_expect(fx, path, "return=%2Fsetup-guide%2Fexisting", "/setup-guide/existing?done=run");
	g_clear_pointer(&path, g_free);
	assert_state(fx, "existing", "optional", "existing-queued");

	/* A film can now be searched for and downloaded, by the layer's own
	 * answer; the dashboard's card is gone and the guide says so. */
	memset(&ready, 0, sizeof(ready));
	g_assert_true(axis_acquire_check_ready(fx->meta, fx->indexers, fx->downloads, fx->storage, AXIS_READY_FILMS,
		&ready, &error));
	g_assert_no_error(error);
	g_assert_true(axis_readiness_can_get(&ready, AXIS_READY_FILMS));
	g_assert_true(axis_readiness_complete(&ready));
	axis_readiness_clear(&ready);
	card = setup_card(fx, fx->admin, "dashboard-done");
	g_assert_null(card);
	reply = get_as(fx, fx->admin, "/setup-guide");
	assert_contains(reply.body, "Everything Axis needs is set up");
	assert_contains(reply.body, "Carry on with step 8: Identifying the movies already on disk");
	save_page("overview-done", reply.body);
	axis_test_reply_clear(&reply);
}

/* ── Back to the place: a step asked for again ──────────────────── */

static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	AxisTestReply page;
	const gchar *location;
	const gchar *element;
	const gchar *notice;
	g_autofree gchar *path = NULL;

	post_expect(fx, "/setup-guide/titles/dismiss", NULL, "/setup-guide/library?done=dismissed");
	reply = post_as(fx, fx->admin, "/setup-guide/titles/restore", NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	location = axis_test_reply_header(&reply, "Location");
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, "at=step-actions"));
	g_assert_true(g_str_has_suffix(location, "#step-actions"));
	path = g_strndup(location, strrchr(location, '#') - location);
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, "id=\"step-actions\"");
	notice = strstr(page.body, "notice-here");
	g_assert_nonnull(element);
	g_assert_nonnull(notice);
	g_assert_true(element < notice);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(&reply);
}

/* ── 2.2: not now, not used, and another browser ────────────────── */

static void
test_choices(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *card = NULL;
	AxisTestReply reply;

	/* Not now: the guide moves on, and comes back to it last. */
	post_expect(fx, "/setup-guide/titles/skip", NULL, "/setup-guide/library?done=skipped");
	reply = get_as(fx, fx->admin2, "/setup-guide");
	assert_contains(reply.body, "Carry on with step 2: Disks and library folders");
	assert_contains(reply.body, "guide-badge-skipped\">Skipped for now</span>");
	axis_test_reply_clear(&reply);
	assert_state(fx, "titles", "skipped", "titles-skipped");
	/* The dashboard still asks for it. */
	card = setup_card(fx, fx->admin, NULL);
	assert_contains(card, "A TMDb token");
	g_clear_pointer(&card, g_free);

	/* Not used: the guide and the dashboard stop asking. */
	post_expect(fx, "/setup-guide/titles/dismiss", NULL, "/setup-guide/library?done=dismissed");
	card = setup_card(fx, fx->admin2, "dashboard-dismissed");
	g_assert_nonnull(card);
	assert_lacks(card, "A TMDb token");
	assert_contains(card, "An indexer");
	g_clear_pointer(&card, g_free);
	reply = get_as(fx, fx->admin2, "/setup-guide");
	assert_contains(reply.body, "guide-badge-dismissed\">Not used</span>");
	assert_contains(reply.body, "Carry on with step 2: Disks and library folders");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *row = q(fx, "SELECT choice || ' by ' || changed_by FROM setup_step WHERE step = 'titles'");

		g_assert_cmpstr(row, ==, "dismissed by root");
	}
	assert_state(fx, "titles", "dismissed", "titles-dismissed");

	/* Asked for again. */
	post_expect(fx, "/setup-guide/titles/restore", NULL, "/setup-guide/titles?done=restored");
	card = setup_card(fx, fx->admin2, NULL);
	assert_contains(card, "A TMDb token");
	g_clear_pointer(&card, g_free);
	assert_state(fx, "titles", "needed", NULL);

	/* A step done is done whatever was said of it. */
	post_expect(fx, "/setup-guide/writing/dismiss", NULL, "/setup-guide/existing?done=dismissed");
	g_assert_true(axis_settings_set(fx->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", NULL));
	assert_state(fx, "writing", "done", NULL);

	/* Unknown steps are not there. */
	reply = post_as(fx, fx->admin, "/setup-guide/nothing/skip", NULL);
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/setup-guide/nothing");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/* ── 2.3: one value, wherever it is set ─────────────────────────── */

static void
test_both_ways(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
	g_autofree gchar *form = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;

	/* The default profile, set in the guide: Settings and the Quality page
	 * show it. */
	form = form_of("quality.default_profile", "best_1080p", "return", "/setup-guide/quality", NULL);
	post_expect(fx, "/admin/settings/quality", form, "/setup-guide/quality?done=saved");
	g_clear_pointer(&form, g_free);
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_tag(reply.body, "name=\"quality.default_profile\"", "value=\"best_1080p\"", NULL);
	axis_test_reply_clear(&reply);
	/* Set on the Settings page: the guide shows it chosen. */
	post_expect(fx, "/admin/settings/quality", "quality.default_profile=uhd", "/admin/settings?done=saved");
	page = step_page(fx, "quality", NULL);
	assert_tag(page, "name=\"quality.default_profile\"", "value=\"uhd\"", "checked=\"checked\"", NULL);
	{
		static const gchar *const picked[] = { "value=\"best_1080p\"", "checked=\"checked\"", NULL };

		g_assert_false(has_tag(page, picked));
	}
	assert_contains(page, "4K is the default for new titles.");
	g_clear_pointer(&page, g_free);

	/* Read-only storage, each way. */
	post_expect(fx, "/admin/settings/storage", "storage.read_only=false", "/admin/settings?done=saved");
	assert_state(fx, "writing", "done", NULL);
	post_expect(fx, "/admin/settings/storage", "storage.read_only=true&return=%2Fsetup-guide%2Fwriting",
		"/setup-guide/writing?done=saved");
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_tag(reply.body, "<option ", "value=\"true\"", "selected", NULL);
	axis_test_reply_clear(&reply);
	g_assert_true(axis_storage_is_read_only_mode(fx->storage));
	assert_state(fx, "writing", "needed", NULL);

	/* The metadata key: set on the Settings page, the guide is done; removed
	 * there, it is not. */
	post_expect(fx, "/admin/settings/metadata", "metadata.fixture_key=" AXIS_TEST_META_KEY,
		"/admin/settings?done=saved");
	assert_state(fx, "titles", "done", NULL);
	post_expect(fx, "/admin/settings/metadata/clear", "key=metadata.fixture_key", "/admin/settings?done=cleared");
	assert_state(fx, "titles", "needed", NULL);

	/* An indexer source added in the guide is the Indexers page's; renamed
	 * there, the guide shows the new name. */
	form = form_of("name", "Prowlarr", "kind", AXIS_INDEXER_PROWLARR_NAME, "address", base, "secret", KEY, "enabled",
		"yes", "return", "/setup-guide/indexers", NULL);
	post_expect(fx, "/indexers", form, "/setup-guide/indexers?done=added");
	g_clear_pointer(&form, g_free);
	reply = get_as(fx, fx->admin, "/indexers");
	assert_contains(reply.body, ">Prowlarr</");
	axis_test_reply_clear(&reply);
	source = q(fx, "SELECT id FROM indexer_source");
	path = g_strdup_printf("/indexers/%s", source);
	form = form_of("name", "Prowlarr at home", "address", base, "enabled", "yes", NULL);
	{
		g_autofree gchar *location = g_strdup_printf("/indexers/%s?done=saved", source);

		post_expect(fx, path, form, location);
	}
	g_clear_pointer(&form, g_free);
	page = step_page(fx, "indexers", NULL);
	assert_contains(page, "Prowlarr at home is saved but not tested yet");
	/* Its key was kept, and is in no page. */
	assert_lacks(page, KEY);
	{
		g_autofree gchar *kept = q(fx, "SELECT count(*) FROM indexer_source_secret");

		g_assert_cmpstr(kept, ==, "1");
	}
	g_clear_pointer(&page, g_free);

	/* A root added on the Library roots page: the guide's library step sees it. */
	add_disk_through_guide(fx);
	{
		g_autofree gchar *volume = q(fx, "SELECT id FROM volume");
		g_autofree gchar *root_form = form_of("volume", volume, "path", "films", "type", "movies", NULL);

		post_expect(fx, "/storage/roots", root_form, "/storage/roots?done=root-added&at=roots-add#roots-add");
	}
	assert_state(fx, "library", "done", NULL);
}

/* ── The return rule ────────────────────────────────────────────── */

static void
test_return_rule(Fixture *fx, gconstpointer data)
{
	static const gchar *const foreign[] = {
		"https://evil.example/", "//evil.example/setup-guide", "/admin/users", "/setup-guide/../admin/users",
		"/setup-guide/titles?x=1", "/setup-guide/titles/", "/setup-guide/titlesx", " /setup-guide/titles",
		"/SETUP-GUIDE/titles", "/setup-guide/titles\r\nSet-Cookie: x=1", "/setup-guide%2Ftitles", "", "volume",
		"javascript:alert(1)", NULL
	};
	guint i;

	/* The rule itself: the list's own strings, nothing else. */
	g_assert_cmpstr(axis_web_setup_return_path("/setup-guide"), ==, "/setup-guide");
	g_assert_cmpstr(axis_web_setup_return_path("/setup-guide/writing"), ==, "/setup-guide/writing");
	for (i = 0; foreign[i] != NULL; i++)
		g_assert_null(axis_web_setup_return_path(foreign[i]));
	g_assert_null(axis_web_setup_return_path(NULL));

	/* Through a handler: a foreign or malformed value is ignored, and the
	 * handler goes where it always goes. */
	for (i = 0; foreign[i] != NULL; i++) {
		g_autofree gchar *form = form_of("storage.read_only", i % 2 == 0 ? "false" : "true", "return", foreign[i],
			NULL);

		post_expect(fx, "/admin/settings/storage", form, "/admin/settings?done=saved");
	}
	post_expect(fx, "/admin/settings/storage", "storage.read_only=true&return=%2Fsetup-guide%2Fwriting",
		"/setup-guide/writing?done=unchanged");
	/* And for the storage, indexer and identify handlers. */
	{
		g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
		g_autofree gchar *form = form_of("name", "Prowlarr", "kind", AXIS_INDEXER_PROWLARR_NAME, "address", base,
			"enabled", "yes", "return", "https://evil.example/", NULL);
		g_autofree gchar *source = NULL;
		g_autofree gchar *location = NULL;
		AxisTestReply reply = post_as(fx, fx->admin, "/indexers", form);

		g_assert_cmpuint(reply.status, ==, 303);
		source = q(fx, "SELECT id FROM indexer_source");
		location = g_strdup_printf("/indexers/%s?done=added", source);
		g_assert_cmpstr(axis_test_reply_header(&reply, "Location"), ==, location);
		axis_test_reply_clear(&reply);
	}
	/* A form refused on its page keeps the way back, and says so. */
	{
		AxisTestReply reply = post_as(fx, fx->admin, "/indexers", "name=&kind=prowlarr&address=nonsense&enabled=yes"
			"&return=%2Fsetup-guide%2Findexers");

		g_assert_cmpuint(reply.status, ==, 400);
		assert_tag(reply.body, "type=\"hidden\"", "name=\"return\"", "value=\"/setup-guide/indexers\"", NULL);
		assert_contains(reply.body, "This form came from the setup guide");
		axis_test_reply_clear(&reply);
	}
}

/* ── 2.4: links ─────────────────────────────────────────────────── */

static void
test_links(Fixture *fx, gconstpointer data)
{
	g_autoptr(GRegex) anchor = g_regex_new("<a [^>]*>", 0, 0, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
	g_autofree gchar *client_base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *expected = NULL;
	g_autofree gchar *page = NULL;
	AxisIndexerSourceSpec source;
	AxisDownloadClientSpec client;
	guint links = 0;
	guint i;

	/* A source served under a path, and a client: the guide links to their
	 * own settings pages, from what was configured. */
	memset(&source, 0, sizeof(source));
	source.name = "Prowlarr";
	source.kind = AXIS_INDEXER_PROWLARR_NAME;
	source.address = base;
	source.base_path = "/prowlarr";
	source.enabled = TRUE;
	source.secret = KEY;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &source, NULL, &error));
	g_assert_no_error(error);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = client_base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &client, NULL, &error));
	g_assert_no_error(error);

	page = step_page(fx, "indexers", NULL);
	expected = g_strdup_printf("href=\"%s/prowlarr/settings/general\"", base);
	assert_tag(page, expected, "target=\"_blank\"", "rel=\"noreferrer noopener\"", NULL);
	assert_contains(page, "Open Prowlarr&#39;s General settings");
	g_clear_pointer(&page, g_free);

	for (i = 0; all_steps[i] != NULL; i++) {
		g_autofree gchar *shown = step_page(fx, all_steps[i], NULL);
		g_autoptr(GMatchInfo) match = NULL;

		g_regex_match(anchor, shown, 0, &match);
		while (g_match_info_matches(match)) {
			g_autofree gchar *tag = g_match_info_fetch(match, 0);

			/* Every link out of Axis opens in a new tab, with no referrer and
			 * no opener; every link that opens a new tab is one. */
			if (strstr(tag, "href=\"http") != NULL || strstr(tag, "target=") != NULL) {
				links++;
				assert_contains(tag, "target=\"_blank\"");
				assert_contains(tag, "rel=\"noreferrer noopener\"");
			}
			g_match_info_next(match, NULL);
		}
	}
	/* The documentation links at least, and the configured one. */
	g_assert_cmpuint(links, >=, 4);
}

/* ── 2.5: administrators only, no secret shown ──────────────────── */

static void
test_admin_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
	g_autofree gchar *client_base = axis_test_download_fake_base(fx->fake);
	g_autofree gchar *card = NULL;
	AxisIndexerSourceSpec source;
	AxisDownloadClientSpec client;
	AxisTestReply reply;
	guint i;

	reply = get_as(fx, fx->viewer, "/setup-guide");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	for (i = 0; all_steps[i] != NULL; i++) {
		g_autofree gchar *path = g_strdup_printf("/setup-guide/%s", all_steps[i]);
		g_autofree gchar *skip = g_strdup_printf("/setup-guide/%s/skip", all_steps[i]);

		reply = get_as(fx, fx->viewer, path);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->viewer, skip, NULL);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}
	reply = post_as(fx, fx->viewer, "/setup-guide/titles/check", NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *rows = q(fx, "SELECT count(*) FROM setup_step");

		g_assert_cmpstr(rows, ==, "0");
	}
	/* Nor does anyone else's dashboard, or its navigation, lead there. */
	card = setup_card(fx, fx->viewer, NULL);
	g_assert_nonnull(card);
	assert_lacks(card, "/setup-guide");
	assert_contains(card, "An administrator can set these up.");
	reply = get_as(fx, fx->viewer, "/");
	assert_lacks(reply.body, "href=\"/setup-guide\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/admin/settings");
	assert_contains(reply.body, "<a href=\"/setup-guide\">Setup guide</a>");
	axis_test_reply_clear(&reply);

	/* Every secret stored, and in no page of the guide. */
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	memset(&source, 0, sizeof(source));
	source.name = "Prowlarr";
	source.kind = AXIS_INDEXER_PROWLARR_NAME;
	source.address = base;
	source.enabled = TRUE;
	source.secret = KEY;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &source, NULL, &error));
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = client_base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &client, NULL, &error));
	g_assert_no_error(error);
	for (i = 0; all_steps[i] != NULL; i++) {
		g_autofree gchar *page = step_page(fx, all_steps[i], NULL);

		assert_lacks(page, AXIS_TEST_META_KEY);
		assert_lacks(page, KEY);
		assert_lacks(page, PASSWORD);
		if (strcmp(all_steps[i], "titles") == 0)
			assert_contains(page, "A value is stored. Type a new one to replace it; leave this empty to keep it.");
		if (strcmp(all_steps[i], "indexers") == 0 || strcmp(all_steps[i], "clients") == 0)
			assert_contains(page, "is stored. Type a new one to replace it; leave this empty to keep it.");
	}
	reply = get_as(fx, fx->admin, "/setup-guide");
	assert_lacks(reply.body, KEY);
	axis_test_reply_clear(&reply);
}

/* ── Spec: TMDb ─────────────────────────────────────────────────── */

static void
test_tmdb(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *page = step_page(fx, "titles", "titles-tmdb");
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	/* What TMDb is for, which of its two values to paste, and its page. */
	assert_contains(page, "come from TMDb, The Movie Database");
	assert_contains(page, "API Read Access Token");
	assert_contains(page, "starts with eyJ");
	assert_contains(page, "Not the API Key");
	assert_tag(page, "href=\"https://www.themoviedb.org/settings/api\"", "target=\"_blank\"",
		"rel=\"noreferrer noopener\"", NULL);
	assert_contains(page, "name=\"metadata.tmdb_key\"");
	assert_contains(page, "No value is stored.");
	/* Nothing contacts TMDb on a page: there is no key, and no check. */
	assert_lacks(page, "Check the token");

	/* The short key is refused by the setting's own check, on the Settings
	 * page, which keeps the way back to the guide. */
	form = form_of(AXIS_META_TMDB_SETTING_KEY, SHORT_TMDB_KEY, "return", "/setup-guide/titles", NULL);
	reply = post_as(fx, fx->admin, "/admin/settings/metadata", form);
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "looks like TMDb&#39;s short");
	assert_tag(reply.body, "type=\"hidden\"", "name=\"return\"", "value=\"/setup-guide/titles\"", NULL);
	assert_contains(reply.body, "This form came from the setup guide");
	assert_lacks(reply.body, SHORT_TMDB_KEY);
	save_page("titles-refused", reply.body);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&page, g_free);
	page = step_page(fx, "titles", NULL);
	assert_page_state(page, "needed");
}

/* ── fix-prowlarr-live 2.1: redirects that are not followed ─────── */

#define REDIRECTING_USENET \
	"{\"id\":27,\"name\":\"NewsOwl\",\"protocol\":\"usenet\",\"enable\":true,\"redirect\":true," \
	"\"capabilities\":{\"movieSearchParams\":[\"q\",\"imdbId\"]}}"
#define PLAIN_TORRENT \
	"{\"id\":3,\"name\":\"The Pirate Bay\",\"protocol\":\"torrent\",\"enable\":true,\"redirect\":false," \
	"\"capabilities\":{\"movieSearchParams\":[\"q\"]}}"

/* The checklist and the guide when a source's indexers redirect their
 * downloads and following is switched off: every one of them, and the
 * source finds nothing that can be fetched; some, of one protocol, and that
 * protocol needs attention. */
static void
test_redirects_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *base = axis_test_prowlarr_base(fx->prowlarr);
	g_autofree gchar *page = NULL;
	AxisIndexerSourceSpec spec;
	AxisReadiness ready;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.name = "Prowlarr";
	spec.kind = AXIS_INDEXER_PROWLARR_NAME;
	spec.address = base;
	spec.enabled = TRUE;
	spec.secret = KEY;
	spec.refuse_redirects = TRUE;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &spec, &id, &error));

	/* Every indexer that is on redirects: nothing can be fetched. */
	axis_test_prowlarr_set_indexers(fx->prowlarr, "[" REDIRECTING_USENET "]");
	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	memset(&ready, 0, sizeof(ready));
	g_assert_true(axis_acquire_check_ready(fx->meta, fx->indexers, fx->downloads, fx->storage, AXIS_READY_FILMS,
		&ready, &error));
	g_assert_false(ready.checks[AXIS_READY_INDEXER].done);
	assert_contains(ready.checks[AXIS_READY_INDEXER].text, "Prowlarr redirects every download to its indexers, and "
		"following download redirects is switched off, so nothing it finds can be fetched.");
	axis_readiness_clear(&ready);

	/* Its Usenet indexer redirects and its torrent one does not: it works,
	 * and Usenet needs attention. */
	axis_test_prowlarr_set_indexers(fx->prowlarr, "[" REDIRECTING_USENET "," PLAIN_TORRENT "]");
	g_assert_true(axis_indexers_test_source(fx->indexers, id, NULL, &error));
	memset(&ready, 0, sizeof(ready));
	g_assert_true(axis_acquire_check_ready(fx->meta, fx->indexers, fx->downloads, fx->storage, AXIS_READY_FILMS,
		&ready, &error));
	g_assert_true(ready.checks[AXIS_READY_INDEXER].done);
	assert_contains(ready.checks[AXIS_READY_INDEXER].text, "Some of Prowlarr's Usenet indexers redirect their "
		"downloads and following those redirects is switched off, so their releases cannot be fetched");
	axis_readiness_clear(&ready);

	/* The guide's step says it too, and what to switch on. */
	page = step_page(fx, "indexers", "indexers-redirects-off");
	assert_contains(page, "Some of its indexers redirect their downloads to their own sites and following those "
		"redirects is switched off, so their releases cannot be fetched: switch on &quot;Follow download redirects "
		"to the indexers&quot; in its settings.");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	AXIS_TEST_ADD_DB_CASE("/web/setup/redirects-refused", Fixture, NULL, test_redirects_refused);
	AXIS_TEST_ADD_DB_CASE("/web/setup/walk", Fixture, NULL, test_walk);
	AXIS_TEST_ADD_DB_CASE("/web/setup/choices", Fixture, NULL, test_choices);
	AXIS_TEST_ADD_DB_CASE("/web/setup/back-to-place", Fixture, NULL, test_back_to_place);
	AXIS_TEST_ADD_DB_CASE("/web/setup/both-ways", Fixture, NULL, test_both_ways);
	AXIS_TEST_ADD_DB_CASE("/web/setup/return", Fixture, NULL, test_return_rule);
	AXIS_TEST_ADD_DB_CASE("/web/setup/links", Fixture, NULL, test_links);
	AXIS_TEST_ADD_DB_CASE("/web/setup/admin-only", Fixture, NULL, test_admin_only);
	AXIS_TEST_ADD_DB_CASE("/web/setup/tmdb", Fixture, "tmdb", test_tmdb);
	return g_test_run();
}
