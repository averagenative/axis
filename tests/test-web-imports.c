/*
 * test-web-imports.c - the import pages (Activity -> Imports, an import's
 * page, the manual import, the Naming page and its preview), the actions
 * through them, the film's page after its import, and the volume warning,
 * with the real shell on an ephemeral loopback port, a real database and a
 * real volume (a directory of the scratch directory)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-import-pipeline 5.1 to 5.3, 4.4 (the warning on the Download
 * clients and Storage pages) and 6.10: the pages render for an
 * administrator and a viewer, only administrators see and may post the
 * forms, every form carries the session's CSRF token, and every action is
 * recorded with the user.
 *
 * With AXIS_TEST_IMPORTS_PAGES set to a directory, the pages are saved
 * there for looking at in a browser.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-download.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "import/axis-import.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "probe/axis-probe.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-imports.h"
#include "web/axis-web-storage.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

#define PASSWORD     AXIS_TEST_DOWNLOAD_PASSWORD
#define TOOL         "tests/fixtures/import/fake-ffprobe.sh"
#define FILM_LINE    "AXISFAKE duration=10200 width=1920 height=800 vcodec=h264 acodec=dts aprofile=DTS-HD_MA channels=6"

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	gint64                downloads_root;
	gchar                *films;
	gchar                *downloads;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads_layer;
	AxisWebDownloads     *downloads_ui;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisMovies           *movies;
	AxisProbe            *probe;
	AxisImport           *import;
	AxisWebImports       *imports_ui;
	gchar                *tool[3];
	AxisWebHub           *hub;
	AxisWebStorage       *storage_ui;
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
set_route(
	Fixture     *fx,
	const gchar *path,
	const gchar *json
){
	g_autoptr(GBytes) body = g_bytes_new(json, strlen(json));

	axis_test_plex_route_full(fx->fa->api, path, 200, "application/json", body, NULL);
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *base = NULL;
	AxisMetaTuning meta_tuning;
	AxisDownloadTuning download_tuning;
	AxisDownloadClientSpec client;
	AxisProbeTuning probe_tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	fx->downloads_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "downloads", AXIS_MEDIA_DOWNLOADS);
	fx->films = g_build_filename(fx->disk, "films", NULL);
	fx->downloads = g_build_filename(fx->disk, "downloads", NULL);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->indexers = axis_indexers_new(fx->t->pool, fx->settings);
	g_assert_true(axis_indexers_add_kind(fx->indexers, axis_indexer_prowlarr_kind(), &error));
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads_layer = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads_layer, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT),
		&error));
	g_assert_true(axis_downloads_add_kind(fx->downloads_layer, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(fx->downloads_layer, &download_tuning);
	fx->downloads_ui = axis_web_downloads_new(fx->downloads_layer, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads_layer,
		fx->t->storage, fx->quality);
	fx->probe = axis_probe_new(fx->t->pool, fx->t->storage, NULL);
	fx->tool[0] = g_strdup("/bin/sh");
	fx->tool[1] = g_canonicalize_filename(TOOL, NULL);
	memset(&probe_tuning, 0, sizeof(probe_tuning));
	probe_tuning.tool_command = (const gchar *const *)fx->tool;
	axis_probe_set_tuning(fx->probe, &probe_tuning);
	fx->import = axis_import_new(fx->t->pool, fx->settings, fx->t->storage, fx->downloads_layer, fx->probe);
	axis_movies_set_import(fx->movies, fx->import);
	fx->imports_ui = axis_web_imports_new(fx->import, fx->settings);
	fx->hub = axis_web_hub_new(NULL);
	fx->storage_ui = axis_web_storage_new(fx->t->storage, fx->hub);

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
	g_assert_true(axis_module_registry_register(fx->registry, axis_movies_get_module(), fx->movies, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_imports_get_module(), fx->imports_ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	/* The pages show a torrent left with its client; the client holds none. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_TORRENTS, "seed", &error));

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

	axis_test_meta_fixture_standard(fx->fa);
	set_route(fx, "/title/film/107", "{\"id\":\"107\",\"kind\":\"film\",\"title\":\"Hearth\",\"year\":1995,"
		"\"status\":\"released\",\"release_date\":\"1995-12-15\",\"ids\":{\"imdb\":\"tt0600001\",\"tmdb\":\"6001\"},"
		"\"overview\":\"A group of professional bank robbers starts to feel the hearth from police.\"}");
	set_route(fx, "/title/film/108", "{\"id\":\"108\",\"kind\":\"film\",\"title\":\"Rokan\",\"year\":1998,"
		"\"status\":\"released\",\"release_date\":\"1998-09-25\",\"ids\":{\"tmdb\":\"6002\"}}");
	set_route(fx, "/title/film/109", "{\"id\":\"109\",\"kind\":\"film\",\"title\":\"Colliery\",\"year\":2004,"
		"\"status\":\"released\",\"release_date\":\"2004-08-06\",\"ids\":{\"tmdb\":\"6004\"}}");
	set_route(fx, "/title/film/110", "{\"id\":\"110\",\"kind\":\"film\",\"title\":\"Thistle\",\"year\":1981,"
		"\"status\":\"released\",\"release_date\":\"1981-03-27\",\"ids\":{\"tmdb\":\"6005\"}}");

	fx->fake = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(fx->fake);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads_layer, 0, &client, &fx->client_id, &error));
	g_assert_no_error(error);
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
	axis_movies_stop(fx->movies);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_downloads_stop(fx->downloads_ui);
	axis_web_indexers_stop(fx->indexers_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_web_imports_stop(fx->imports_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_storage_stop(fx->storage_ui);
	axis_web_settings_stop(fx->web_settings);
	axis_downloads_stop(fx->downloads_layer);
	axis_indexers_stop(fx->indexers);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	axis_web_free(fx->web);
	axis_web_hub_free(fx->hub);
	axis_web_storage_free(fx->storage_ui);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_imports_free(fx->imports_ui);
	axis_movies_unref(fx->movies);
	axis_import_unref(fx->import);
	axis_probe_unref(fx->probe);
	axis_web_quality_free(fx->quality_ui);
	axis_quality_store_unref(fx->quality);
	axis_web_downloads_free(fx->downloads_ui);
	axis_downloads_unref(fx->downloads_layer);
	axis_web_indexers_free(fx->indexers_ui);
	axis_indexers_unref(fx->indexers);
	axis_web_meta_free(fx->meta_ui);
	axis_meta_unref(fx->meta);
	axis_web_settings_free(fx->web_settings);
	axis_test_meta_fixture_free(fx->fa);
	axis_test_download_fake_free(fx->fake);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->tool[0]);
	g_free(fx->tool[1]);
	g_free(fx->disk);
	g_free(fx->films);
	g_free(fx->downloads);
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
	if (haystack != NULL && strstr(haystack, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
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

/* Every form of the page posts with the session's CSRF field. */
static void
assert_forms_carry_token(const gchar *body)
{
	const gchar *p = body;
	guint forms = 0;

	while ((p = strstr(p, "<form")) != NULL) {
		const gchar *end = strstr(p, "</form>");

		g_assert_nonnull(end);
		if (g_strstr_len(p, end - p, "method=\"post\"") != NULL) {
			g_assert_nonnull(g_strstr_len(p, end - p, "name=\"_csrf\""));
			forms++;
		}
		p = end;
	}
	(void)forms;
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_IMPORTS_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
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
	return axis_test_db_query(fx->t->db, sql);
}

static void
assert_qf(
	Fixture     *fx,
	const gchar *expected,
	const gchar *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
assert_qf(
	Fixture     *fx,
	const gchar *expected,
	const gchar *format,
	...
){
	g_autofree gchar *sql = NULL;
	g_autofree gchar *value = NULL;
	va_list ap;

	va_start(ap, format);
	sql = g_strdup_vprintf(format, ap);
	va_end(ap);
	value = axis_test_db_query(fx->t->db, sql);
	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", sql, expected != NULL ? expected : "NULL", value != NULL ? value : "NULL");
}

static gint64
title_of(
	Fixture     *fx,
	const gchar *provider_id
){
	AxisMetaTitleAnswer answer;
	g_autoptr(GError) error = NULL;
	gint64 id;

	memset(&answer, 0, sizeof(answer));
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", AXIS_META_KIND_FILM, provider_id, 5000, &answer, &error));
	g_assert_nonnull(answer.title);
	id = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return id;
}

static gint64
add_film(
	Fixture     *fx,
	const gchar *provider_id
){
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.root_id = fx->root_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static void
write_media(
	const gchar *path,
	const gchar *line,
	gsize        size,
	guint        seed
){
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree guchar *bytes = g_malloc(size);
	gsize n = strlen(line);
	gsize i;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	memcpy(bytes, line, n);
	bytes[n] = '\n';
	for (i = n + 1; i < size; i++)
		bytes[i] = (guchar)((i * 31 + seed * 7) & 0xff);
	g_assert_true(g_file_set_contents(path, (const gchar *)bytes, (gssize)size, NULL));
}

/* A completed download of @release for @movie, its files named @files
 * (each with @line), and its import requested and run once. */
static gint64
download_and_run(
	Fixture            *fx,
	gint64              movie,
	const gchar        *release,
	const gchar *const *files,
	const gchar        *line
){
	g_autofree gchar *title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, movie);
	g_autofree gchar *download = NULL;
	g_autofree gchar *grab = NULL;
	g_autoptr(GError) error = NULL;
	AxisImportReport report;
	AxisDbConn *conn;
	gint64 import_id = 0;
	guint i;

	for (i = 0; files[i] != NULL; i++) {
		g_autofree gchar *path = g_build_filename(fx->downloads, release, files[i], NULL);

		if (g_str_has_suffix(files[i], ".srt"))
			g_assert_true(g_file_set_contents(path, "1\n00:00:01,000 --> 00:00:02,000\nHello.\n", -1, NULL));
		else if (strstr(files[i], "-sample") != NULL)
			write_media(path, "AXISFAKE duration=60 width=1920 height=800 vcodec=h264", 8 * 1024, i + 1);
		else
			write_media(path, line, 96 * 1024, i + 1);
	}
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'ref-%s', 'torrent', 'file', '%s', %s, '%s',"
		" 'movies', 'completed', 1000, 1000, '/data/downloads/%s', %" G_GINT64_FORMAT ", 'downloads/%s',"
		" now() - interval '2 hours', now() - interval '1 hour') RETURNING id", fx->client_id, release, release, title,
		release, release, fx->volume_id, release);
	grab = q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at, quality) VALUES (%" G_GINT64_FORMAT ", '%s', 'torrent', lower('1:0:%s'), %s,"
		" now() - interval '2 hours', now() - interval '1 hour', 'bluray_1080p') RETURNING id", movie, release,
		release, download);
	g_assert_nonnull(grab);
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_import_request(fx->import, conn, AXIS_MOVIES_MODULE_ID, g_ascii_strtoll(download, NULL, 10),
		&import_id, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_import_run(fx->import, import_id, NULL, &report, &error));
	g_assert_no_error(error);
	return import_id;
}

static gint64
title_id_of(
	Fixture *fx,
	gint64   movie
){
	g_autofree gchar *id = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, movie);

	return g_ascii_strtoll(id, NULL, 10);
}

/* What the pages are shot from: Hearth upgraded (an older file retired),
 * Rokan blocked with two films, Thistle blocked by another film's name,
 * Colliery waiting for a probe to be tried again, a folder in the
 * downloads root, and the client's folder on another volume. */
typedef struct
{
	gint64 hearth;
	gint64 hearth_import;
	gint64 rokan;
	gint64 rokan_import;
	gint64 thistle;
	gint64 thistle_import;
	gint64 colliery;
	gint64 colliery_import;
} Scene;

static Scene
scene(Fixture *fx)
{
	static const gchar *const hearth_files[] = { "Hearth.1995.1080p.BluRay.x264-GRP.mkv",
		"Hearth.1995.1080p.BluRay.x264-GRP.en.srt", "Hearth.1995.1080p.BluRay.x264-GRP-sample.mkv", NULL };
	static const gchar *const rokan_files[] = { "Rokan.1998.1080p.BluRay.x264-GRP.mkv",
		"Rokan.1998.Directors.Cut.1080p.BluRay.x264-GRP.mkv", NULL };
	static const gchar *const thistle_files[] = { "Rokan.1998.1080p.WEB-DL.x264-GRP.mkv", NULL };
	static const gchar *const colliery_files[] = { "Colliery.2004.1080p.BluRay.x264-GRP.mkv", NULL };
	g_autofree gchar *old = NULL;
	g_autofree gchar *file_id = NULL;
	g_autofree gchar *folder = NULL;
	g_autoptr(GError) error = NULL;
	Scene s;

	memset(&s, 0, sizeof(s));
	/* Hearth: an older file in the library, then the upgrade. */
	s.hearth = add_film(fx, "107");
	old = g_build_filename(fx->films, "Hearth (1995) {tmdb-6001}", "Hearth (1995) {tmdb-6001} [WEB-DL 1080p]-OLD.mkv", NULL);
	write_media(old, FILM_LINE, 64 * 1024, 9);
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	file_id = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%-OLD.mkv'");
	axis_test_db_exec(fx->t->db, "UPDATE library_root_state SET complete_generation = generation");
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, release_name)"
			" VALUES (%s, %" G_GINT64_FORMAT ", 'webdl_1080p', 'Hearth.1995.1080p.WEB-DL.x264-OLD')", file_id, s.hearth);

		axis_test_db_exec(fx->t->db, sql);
	}
	s.hearth_import = download_and_run(fx, s.hearth, "Hearth.1995.1080p.BluRay.x264-GRP", hearth_files, FILM_LINE);
	assert_qf(fx, "imported", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.hearth_import);

	s.rokan = add_film(fx, "108");
	s.rokan_import = download_and_run(fx, s.rokan, "Rokan.1998.1080p.BluRay.x264-GRP", rokan_files, FILM_LINE);
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.rokan_import);

	s.thistle = add_film(fx, "110");
	s.thistle_import = download_and_run(fx, s.thistle, "Thistle.1981.1080p.WEB-DL.x264-GRP", thistle_files, FILM_LINE);
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.thistle_import);

	s.colliery = add_film(fx, "109");
	s.colliery_import = download_and_run(fx, s.colliery, "Colliery.2004.1080p.BluRay.x264-GRP",
		colliery_files, "AXISFAKE fail");
	assert_qf(fx, "waiting", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.colliery_import);

	/* A folder put in the downloads root by hand. */
	folder = g_build_filename(fx->downloads, "Some.Film.2001.1080p.WEB-DL.x264-GRP",
		"Some.Film.2001.1080p.WEB-DL.x264-GRP.mkv", NULL);
	write_media(folder, "AXISFAKE duration=6300 width=1920 height=1040 vcodec=hevc acodec=eac3 channels=6", 64 * 1024, 4);
	/* The client puts its downloads on Disk two, the films are on Disk one.
	 * (One filesystem is one volume, so Disk two is only a record here: the
	 * warning reads the database alone.) */
	{
		g_autofree gchar *disk2 = g_build_filename(fx->t->media, "disk2", NULL);
		g_autofree gchar *volume = q(fx, "INSERT INTO volume (uuid, name, path, mount_root, state, state_reason,"
			" marker_created_at) VALUES ('7d3f6b2a-1c4e-4a8b-9f00-2b6c8e1d5a70', 'Disk two', '%s', true, 'offline',"
			" 'unchecked', now()) RETURNING id", disk2);
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO download_path_map (client_id, client_prefix, volume_id,"
			" rel_path) VALUES (%" G_GINT64_FORMAT ", '/mnt/other', %s, '')", fx->client_id, volume);

		axis_test_db_exec(fx->t->db, sql);
	}
	(void)error;
	return s;
}

/* ── The pages ──────────────────────────────────────────────────── */

static void
test_pages(Fixture *fx, gconstpointer data)
{
	Scene s = scene(fx);
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;

	/* Activity -> Imports. */
	reply = get_as(fx, fx->admin, "/imports");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Blocked");
	assert_contains(reply.body, "Waiting");
	assert_contains(reply.body, "Recent");
	assert_contains(reply.body, "Hardlinked, upgrade");
	assert_contains(reply.body, "Rokan (1998)");
	assert_contains(reply.body, "Thistle (1981)");
	assert_contains(reply.body, "Colliery (2004)");
	assert_contains(reply.body, "Import a movie by hand");
	assert_contains(reply.body, ">Imports<");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	save_page("imports.html", reply.body);
	axis_test_reply_clear(&reply);
	/* The import settings say what the default does with a torrent. */
	reply = get_as(fx, fx->admin, "/admin/settings");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "After a torrent is imported");
	assert_contains(reply.body, "Axis is not a seedbox");
	assert_contains(reply.body, "private trackers that require a ratio");
	save_page("settings-import.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/imports");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Rokan (1998)");
	assert_lacks(reply.body, "Import a movie by hand");
	axis_test_reply_clear(&reply);

	/* A blocked import: its reasons and what resolves it. */
	path = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.thistle_import);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Ways to resolve this import");
	assert_contains(reply.body, "Import anyway");
	assert_contains(reply.body, "Import this file");
	assert_contains(reply.body, "It is another movie");
	assert_contains(reply.body, "Dismiss");
	assert_contains(reply.body, "Its name reads as another movie");
	assert_csp_clean(reply.body);
	assert_forms_carry_token(reply.body);
	axis_test_web_assert_tables(reply.body);
	save_page("import-blocked.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Its name reads as another movie");
	assert_lacks(reply.body, "Ways to resolve this import");
	assert_lacks(reply.body, "/act\"");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&path, g_free);

	/* Several films: no "import anyway". */
	path = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.rokan_import);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "choose the one that is");
	assert_lacks(reply.body, "I checked: it is this movie");
	save_page("import-several.html", reply.body);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&path, g_free);

	/* The upgrade: placed, how, and what it retired. */
	path = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.hearth_import);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Placed in");
	assert_contains(reply.body, "a hardlink on Disk one");
	assert_contains(reply.body, "Replaced files in the recycle folder");
	assert_contains(reply.body, "Hearth (1995) {tmdb-6001} [WEB-DL 1080p]-OLD.mkv");
	assert_contains(reply.body, "Left with its client (a torrent is left seeding).");
	assert_lacks(reply.body, "Ways to resolve this import");
	axis_test_web_assert_tables(reply.body);
	assert_lacks(reply.body, "Waiting: ");
	save_page("import-upgraded.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A replaced file whose time is up while storage is read-only says why it is still there. */
	axis_test_db_exec(fx->t->db, "UPDATE import_recycle SET remove_after = now() - interval '1 hour'");
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	while (g_main_context_iteration(NULL, FALSE))
		;
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Waiting: Read-only storage is on");
	axis_test_reply_clear(&reply);
	/* Spec (web-shell, tidy-labels): A setting named by its label, its key at most in parentheses. */
	{
		g_autofree gchar *waiting = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.colliery_import);

		reply = get_as(fx, fx->admin, waiting);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Read-only storage is on (Settings, Storage; storage.read_only)");
		assert_lacks(reply.body, "the setting storage.read_only");
		axis_test_reply_clear(&reply);
	}
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	while (g_main_context_iteration(NULL, FALSE))
		;
	reply = get_as(fx, fx->admin, path);
	assert_lacks(reply.body, "Waiting: ");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&path, g_free);

	/* No such import. */
	reply = get_as(fx, fx->admin, "/imports/999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* The manual import (its address is not taken for an import's). */
	reply = get_as(fx, fx->admin, "/imports/manual");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Import a movie by hand");
	assert_contains(reply.body, "Some.Film.2001.1080p.WEB-DL.x264-GRP");
	assert_contains(reply.body, "Colliery.2004.1080p.BluRay.x264-GRP");
	assert_csp_clean(reply.body);
	axis_test_web_assert_tables(reply.body);
	save_page("manual.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/imports/manual");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/imports/manual/inspect?root=%" G_GINT64_FORMAT "&path=Some.Film.2001.1080p.WEB-DL.x264-GRP",
		fx->downloads_root);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Files in this download or folder");
	assert_contains(reply.body, "1920x1040");
	assert_contains(reply.body, "1 h 45 min");
	assert_contains(reply.body, "Let Axis decide");
	assert_forms_carry_token(reply.body);
	axis_test_web_assert_tables(reply.body);
	save_page("manual-inspect.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The Naming page, with its preview. */
	reply = get_as(fx, fx->admin, "/imports/naming");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Hearth (1995) {tmdb-6001}");
	assert_contains(reply.body, "Aurélie (2001) {tmdb-6022}");
	assert_csp_clean(reply.body);
	assert_forms_carry_token(reply.body);
	save_page("naming.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/imports/naming");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/imports/naming/preview?import.rename=true&import.layout=jellyfin"
		"&import.file_format=%7B-group%7D");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Hearth (1995) [tmdbid-6001]-QXR.mkv");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/imports/naming/preview?import.rename=true&import.layout=tmdb"
		"&import.file_format=%7Bnope%7D");
	assert_contains(reply.body, "a token that does not exist");
	assert_contains(reply.body, "role=\"alert\"");
	axis_test_reply_clear(&reply);

	/* The film's page after its import. */
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, s.hearth));
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Files of this movie");
	assert_contains(reply.body, "Hearth (1995) {tmdb-6001} [BluRay 1080p][DTS-HD MA 5.1][x264]-GRP.mkv");
	assert_csp_clean(reply.body);
	save_page("film-in-library.html", reply.body);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, s.thistle));
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	{
		g_autofree gchar *link = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.thistle_import);

		assert_contains(reply.body, link);
	}
	save_page("film-blocked.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The client's folder on another volume than the films. */
	reply = get_as(fx, fx->admin, "/downloads/clients");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "import-volume-warning");
	assert_contains(reply.body, "is copied");
	assert_csp_clean(reply.body);
	save_page("volume-warning.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/storage/roots");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "import-volume-warning");
	assert_csp_clean(reply.body);
	save_page("roots-warning.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The dashboard asks for a person. */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, " blocked: each needs a person to decide what to do with its files.");
	save_page("dashboard.html", reply.body);
	axis_test_reply_clear(&reply);
}

/* ── Acting through the pages ───────────────────────────────────── */

static void
test_actions(Fixture *fx, gconstpointer data)
{
	Scene s = scene(fx);
	g_autofree gchar *page = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.rokan_import);
	g_autofree gchar *act = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/act", s.rokan_import);
	g_autofree gchar *thistle_page = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.thistle_import);
	g_autofree gchar *thistle_act = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/act", s.thistle_import);
	g_autofree gchar *form = NULL;
	AxisTestReply reply;

	/* No token, a viewer, nothing to do: refused, nothing changed. */
	reply = post_as(fx, fx->admin, NULL, act, "action=dismiss");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, page, act, "action=dismiss");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, act, "action=explode");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.rokan_import);
	assert_qf(fx, "0", "SELECT count(*) FROM import_action");

	/* "Import anyway" cannot choose between two films. */
	reply = post_as(fx, fx->admin, page, act, "action=import_anyway&confirm=yes");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "cannot override");
	axis_test_reply_clear(&reply);
	/* Nor anything without the box ticked. */
	reply = post_as(fx, fx->admin, thistle_page, thistle_act, "action=import_anyway");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	/* With it: recorded with the user, queued. */
	reply = post_as(fx, fx->admin, thistle_page, thistle_act, "action=import_anyway&confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "waiting/{wrong_name}/root", "SELECT state || '/' || override_codes::text || '/' || override_by"
		" FROM import WHERE id = %" G_GINT64_FORMAT, s.thistle_import);
	assert_qf(fx, "import_anyway/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
		G_GINT64_FORMAT, s.thistle_import);

	/* Choose one of Rokan's two. */
	{
		g_autofree gchar *position = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
			" AND path LIKE 'Rokan.1998.1080p%%'", s.rokan_import);

		form = g_strdup_printf("action=choose_file&position=%s", position);
	}
	reply = post_as(fx, fx->admin, page, act, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "choose_file/root", "SELECT action || '/' || by_user FROM import_action WHERE import_id = %"
		G_GINT64_FORMAT, s.rokan_import);
	/* Dismissed, then: the download stays where it is. */
	reply = post_as(fx, fx->admin, page, act, "action=dismiss");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "dismissed", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.rokan_import);
	assert_qf(fx, "completed", "SELECT d.state FROM download d JOIN import i ON i.download_id = d.id"
		" WHERE i.id = %" G_GINT64_FORMAT, s.rokan_import);
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "Dismissed");
	assert_contains(reply.body, "What people did");
	axis_test_reply_clear(&reply);

	/* A manual import through its form. */
	g_free(form);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT
		"&path=Some.Film.2001.1080p.WEB-DL.x264-GRP&position=auto", s.rokan, fx->downloads_root);
	reply = post_as(fx, fx->viewer, "/imports", "/imports/manual", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/imports/manual", "/imports/manual", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "manual/root/waiting", "SELECT kind || '/' || requested_by || '/' || state FROM import"
		" WHERE kind = 'manual'");
	/* A path out of the root is refused. */
	g_free(form);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT "&path=..%%2Ffilms", s.hearth,
		fx->downloads_root);
	reply = post_as(fx, fx->admin, "/imports/manual", "/imports/manual", form);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* The naming settings: an invalid format is not saved; a valid one is. */
	reply = post_as(fx, fx->admin, "/imports/naming", "/imports/naming",
		"import.rename=true&import.layout=tmdb&import.file_format=%7Bquality");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Nothing was saved");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, "/imports", "/imports/naming",
		"import.rename=true&import.layout=plain&import.file_format=%7B-group%7D");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/imports/naming", "/imports/naming",
		"import.rename=true&import.layout=plain&import.file_format=%7B-group%7D");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *layout = axis_settings_get_string(fx->settings, AXIS_IMPORT_SETTING_LAYOUT);
		g_autofree gchar *format = axis_settings_get_string(fx->settings, AXIS_IMPORT_SETTING_FORMAT);

		g_assert_cmpstr(layout, ==, "plain");
		g_assert_cmpstr(format, ==, "{-group}");
	}
}

/* A form's answer is a redirect to the element the person acted on, and the
 * page there draws its notice inside that element (the element comes before
 * the first notice in the page). */
static void
assert_back_to_place(
	Fixture       *fx,
	AxisTestReply *reply,
	const gchar   *place
){
	g_autofree gchar *at = g_strdup_printf("at=%s", place);
	g_autofree gchar *tail = g_strdup_printf("#%s", place);
	g_autofree gchar *id = g_strdup_printf("id=\"%s\"", place);
	g_autofree gchar *path = NULL;
	const gchar *location = axis_test_reply_header(reply, "Location");
	const gchar *element;
	const gchar *notice;
	AxisTestReply page;

	g_assert_cmpuint(reply->status, ==, 303);
	g_assert_nonnull(location);
	g_assert_nonnull(strstr(location, at));
	g_assert_true(g_str_has_suffix(location, tail));
	path = g_strndup(location, strlen(location) - strlen(tail));
	page = get_as(fx, fx->admin, path);
	g_assert_cmpuint(page.status, ==, 200);
	element = strstr(page.body, id);
	notice = strstr(page.body, "notice-here");
	if (element == NULL || notice == NULL || element > notice)
		g_error("expected the notice inside %s in:\n%s", place, page.body);
	axis_test_reply_clear(&page);
	axis_test_reply_clear(reply);
}

/* Spec: a form comes back to the card or row it was posted from */
static void
test_back_to_place(Fixture *fx, gconstpointer data)
{
	Scene s = scene(fx);
	g_autofree gchar *page = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.rokan_import);
	g_autofree gchar *act = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/act", s.rokan_import);
	g_autofree gchar *thistle_page = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.thistle_import);
	g_autofree gchar *thistle_act = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/act", s.thistle_import);
	g_autofree gchar *position = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE 'Rokan.1998.1080p%%'", s.rokan_import);
	g_autofree gchar *form = g_strdup_printf("action=choose_file&position=%s", position);
	AxisTestReply reply;

	/* "What resolves it": import anyway, and choosing a file. */
	reply = post_as(fx, fx->admin, thistle_page, thistle_act, "action=import_anyway&confirm=yes");
	assert_back_to_place(fx, &reply, "import-actions");
	reply = post_as(fx, fx->admin, page, act, form);
	assert_back_to_place(fx, &reply, "import-actions");

	/* The naming settings: their card. */
	reply = post_as(fx, fx->admin, "/imports/naming", "/imports/naming",
		"import.rename=true&import.layout=plain&import.file_format=%7B-group%7D");
	assert_back_to_place(fx, &reply, "naming-card");
}

/* An imported download of @protocol with what happened at its client and to
 * its folder, as recorded; its import's id. */
static gint64
imported_with(
	Fixture     *fx,
	const gchar *protocol,
	const gchar *client_removal,
	const gchar *client_text,
	const gchar *folder_removal,
	const gchar *folder_text
){
	g_autofree gchar *download = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *ct = client_text != NULL ? g_strdup_printf("'%s'", client_text) : g_strdup("NULL");
	g_autofree gchar *fr = folder_removal != NULL ? g_strdup_printf("'%s'", folder_removal) : g_strdup("NULL");
	g_autofree gchar *ft = folder_text != NULL ? g_strdup_printf("'%s'", folder_text) : g_strdup("NULL");

	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" client_path, volume_id, rel_path) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'ref-%s-%s', '%s', 'file',"
		" 'Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP', 'imported',"
		" '/usenet/complete/axis/Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP', %" G_GINT64_FORMAT ","
		" 'downloads/axis/Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP') RETURNING id", fx->client_id, protocol,
		folder_removal != NULL ? folder_removal : client_removal, protocol, fx->volume_id);
	id = q(fx, "INSERT INTO import (module, title_label, download_id, source_volume_id, source_path, release_name,"
		" protocol, state, finished_at, client_removal, client_removal_text, folder_removal, folder_removal_text)"
		" VALUES ('movies', 'Marrowfield Lanterns (2011)', %s, %" G_GINT64_FORMAT ","
		" 'downloads/axis/Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP',"
		" 'Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP', '%s', 'imported', now(), '%s', %s, %s, %s)"
		" RETURNING id", download, fx->volume_id, protocol, client_removal, ct, fr, ft);
	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

static void
assert_import_page(
	Fixture     *fx,
	gint64       id,
	const gchar *contains,
	const gchar *lacks
){
	g_autofree gchar *path = g_strdup_printf("/imports/%" G_GINT64_FORMAT, id);
	AxisTestReply reply = get_as(fx, fx->admin, path);

	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, contains);
	if (lacks != NULL)
		assert_lacks(reply.body, lacks);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
}

/* Spec (import): what the import's page says about the client's removal and
 * the job's leftover folder is what happened: removed from the client and its
 * folder deleted; removed and the folder waiting, and why; the folder left,
 * and by which rule; a torrent removed with its files and no folder of
 * Axis's to speak of. */
static void
test_the_folder_on_the_page(Fixture *fx, gconstpointer data)
{
	gint64 id;

	id = imported_with(fx, "usenet", "done", "The job was removed from its client.", "done",
		"Its folder at the client (/usenet/complete/axis/Marrowfield.Lanterns.2011.1080p.BluRay.x264-MADEUP) was"
		" deleted, with the 7 files and folders the client left in it.");
	assert_import_page(fx, id, "The job was removed from its client.", "with its data");
	assert_import_page(fx, id, "Its folder at the client", NULL);
	assert_import_page(fx, id, "was deleted, with the 7 files and folders the client left in it.", NULL);

	id = imported_with(fx, "usenet", "done", "The job was removed from its client.", "waiting",
		"Its folder at the client is still there, and is deleted once it can be: Read-only storage is on:"
		" nothing on disk ''Disk one'' may be changed. It is tried again every hour.");
	assert_import_page(fx, id, "Read-only storage is on", NULL);
	assert_import_page(fx, id, "It is tried again every hour.", NULL);

	id = imported_with(fx, "usenet", "done", NULL, "left",
		"Its folder was left where it is: it is inside the library root Films.");
	/* Without a recorded text: the job was removed, its data not said to be. */
	assert_import_page(fx, id, "The job was removed from its client.", "with its data");
	assert_import_page(fx, id, "Its folder was left where it is: it is inside the library root Films.", NULL);

	/* Still to be removed from the client: the folder after it. */
	id = imported_with(fx, "usenet", "failed", "Its client could not remove it yet (tried again every hour): the"
		" client could not be reached", "pending", NULL);
	assert_import_page(fx, id, "Deleted once its client has removed the job", NULL);

	id = imported_with(fx, "usenet", "done", NULL, "gone", "Its folder at the client was already gone.");
	assert_import_page(fx, id, "Its folder at the client was already gone.", NULL);

	/* A torrent: removed with its files, no folder line. */
	id = imported_with(fx, "torrent", "done", NULL, NULL, NULL);
	assert_import_page(fx, id, "Removed from its client with its downloaded files.", "Its folder at the client");
}

/* Spec (import): a torrent already gone from its client: the page says so,
 * never that it was removed with its files, and says on its own line what
 * became of what it left: to be looked at, deleted, or left and why and
 * where.  Left seeding: no such line. */
static void
test_a_torrent_gone_on_the_page(Fixture *fx, gconstpointer data)
{
	gint64 id;

	id = imported_with(fx, "torrent", "gone", NULL, "pending", NULL);
	assert_import_page(fx, id, "It was already gone from its client: something else removed it, and the client "
		"deleted none of its files.", "Removed from its client with its downloaded files.");
	assert_import_page(fx, id, "Files left at the download client", "Its folder at the client");
	assert_import_page(fx, id, "To be looked at: the torrent is gone from its client", NULL);

	id = imported_with(fx, "torrent", "gone", "The torrent was already gone from its client: something else removed it"
		" (a person, a plugin of the client or another program), and the client said it no longer has it, so the client"
		" deleted none of its files now.", "left", "Its folder was left where it is: its client keeps the torrents of"
		" Axis in no folder of their own (the label has none). It is at /torrents/done/Marrowfield.Lanterns.2011 at"
		" the client and can be deleted there by hand.");
	assert_import_page(fx, id, "The torrent was already gone from its client", "with its downloaded files;");
	assert_import_page(fx, id, "It is at /torrents/done/Marrowfield.Lanterns.2011 at the client and can be deleted "
		"there by hand.", NULL);

	id = imported_with(fx, "torrent", "gone", NULL, "done", "Its file at the client"
		" (/torrents/done/Marrowfield.Lanterns.2011.mkv) was deleted: the one file of the torrent.");
	assert_import_page(fx, id, "(/torrents/done/Marrowfield.Lanterns.2011.mkv) was deleted", NULL);

	id = imported_with(fx, "torrent", "kept", NULL, NULL, NULL);
	assert_import_page(fx, id, "Left with its client (a torrent is left seeding).", "Files left at the download client");
}

/* Spec (import): a move is said as one, the files' state on the import's page
 * and in the list; an import whose download's files disappeared before it
 * says so, final, where and when. */
static void
test_a_move_on_the_page(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	gint64 id;

	id = imported_with(fx, "torrent", "done", "The torrent was removed from its client without its files, the client "
		"saying it no longer has it, before its files were moved into the library.", "done", "Its folder at the client"
		" (/torrents/done/axis/Marrowfield.Lanterns.2011) was deleted, with the 2 files and folders left in it that the "
		"import did not take (an .nfo, a sample and the like); what it took had been moved into the library.");
	g_free(q(fx, "UPDATE import SET mode = 'move', mode_text = 'a move on Disk one: the download''s file was renamed into"
		" the library, so nothing of it is left in the download''s folder' WHERE id = %" G_GINT64_FORMAT " RETURNING id",
		id));
	assert_import_page(fx, id, "removed from its client without its files", "with its downloaded files");
	assert_import_page(fx, id, "file was renamed into the library, so nothing of it is left in the", NULL);
	assert_import_page(fx, id, "that the import did not take", NULL);
	reply = get_as(fx, fx->admin, "/imports");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Moved");
	assert_contains(reply.body, "Its files are moved there");
	axis_test_reply_clear(&reply);

	/* Files gone before the import. */
	g_free(q(fx, "UPDATE import SET state = 'dismissed', reason = 'files_gone', reason_text = 'The download''s files"
		" were no longer at Disk one: downloads/axis/Marrowfield.Lanterns.2011 when Axis came to import them.',"
		" mode = NULL WHERE id = %" G_GINT64_FORMAT " RETURNING id", id));
	path = g_strdup_printf("/imports/%" G_GINT64_FORMAT, id);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Files gone");
	assert_contains(reply.body, "were no longer at Disk one: downloads/axis/Marrowfield.Lanterns.2011");
	axis_test_reply_clear(&reply);
}

/* Spec (import): Downloads, Leftovers lists what is in a client's folder for
 * Axis that no download and no import accounts for (administrators only), and
 * deletes one entry on a confirmed, CSRF-checked post through the strict
 * removal; an entry gone since the list is said to be gone on an ordinary
 * page; nothing is deleted while storage is read-only; the Downloads page and
 * the dashboard link to it while it is not empty. */
static void
test_leftovers_page(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	g_autofree gchar *old_job = g_build_filename(category, "Quillfeather.Junction.2014.1080p-MADEUP", NULL);
	g_autofree gchar *old_file = g_build_filename(old_job, "Quillfeather.Junction.2014.1080p-MADEUP.mkv", NULL);
	g_autofree gchar *loose = g_build_filename(category, "Brackwater.Lights.2016.nfo", NULL);
	g_autofree gchar *live = g_build_filename(category, "Saltmarsh.Signal.2019.1080p-MADEUP", NULL);
	g_autofree gchar *live_file = g_build_filename(live, "Saltmarsh.Signal.2019.1080p-MADEUP.mkv", NULL);
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *ino = NULL;
	AxisTestReply reply;
	GStatBuf st;

	g_assert_cmpint(g_mkdir_with_parents(old_job, 0755), ==, 0);
	g_assert_true(g_file_set_contents(old_file, "an old download nobody imported", -1, NULL));
	g_assert_true(g_file_set_contents(loose, "a loose nfo", -1, NULL));
	g_assert_cmpint(g_mkdir_with_parents(live, 0755), ==, 0);
	g_assert_true(g_file_set_contents(live_file, "a download still at its client", -1, NULL));
	g_assert_true(axis_downloads_add_path_map(fx->downloads_layer, fx->client_id, "/usenet/complete", fx->volume_id,
		"downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(fx->fake, "/usenet/complete/axis");
	/* A download Axis follows accounts for its folder. */
	g_free(q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" client_path, volume_id, rel_path) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'nzo_live', 'usenet', 'file',"
		" 'Saltmarsh.Signal.2019.1080p-MADEUP', 'completed', '/usenet/complete/axis/Saltmarsh.Signal.2019.1080p-MADEUP',"
		" %" G_GINT64_FORMAT ", 'downloads/axis/Saltmarsh.Signal.2019.1080p-MADEUP') RETURNING id", fx->client_id,
		fx->volume_id));

	/* Not for a viewer. */
	reply = get_as(fx, fx->viewer, "/downloads/leftovers");
	g_assert_cmpuint(reply.status, !=, 200);
	axis_test_reply_clear(&reply);

	reply = get_as(fx, fx->admin, "/downloads/leftovers");
	g_assert_cmpuint(reply.status, ==, 200);
	save_page("leftovers.html", reply.body);
	axis_test_web_assert_tables(reply.body);
	assert_csp_clean(reply.body);
	assert_contains(reply.body, "Leftovers in the download folders");
	assert_contains(reply.body, "Quillfeather.Junction.2014.1080p-MADEUP");
	assert_contains(reply.body, "Brackwater.Lights.2016.nfo");
	assert_contains(reply.body, "2 files and folders in it belong to no download and no import.");
	assert_contains(reply.body, "/usenet/complete/axis");
	assert_lacks(reply.body, "Saltmarsh.Signal.2019");
	assert_contains(reply.body, "/downloads/leftovers/delete?client=");
	axis_test_reply_clear(&reply);
	/* Nothing was deleted by looking. */
	g_assert_true(g_file_test(old_file, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "2", "SELECT leftover_count FROM download_client WHERE id = %" G_GINT64_FORMAT, fx->client_id);

	/* The Downloads page and the dashboard link to it now. */
	reply = get_as(fx, fx->admin, "/downloads");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "/downloads/leftovers");
	assert_contains(reply.body, "2 files and folders in the download clients");
	axis_test_reply_clear(&reply);
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<a href=\"/downloads/leftovers\"><span>2 files and folders are left in the download "
		"folders and were never imported.</span></a>");
	axis_test_reply_clear(&reply);

	/* The confirmation, then the deletion: CSRF checked, the box ticked. */
	g_assert_cmpint(g_lstat(old_job, &st), ==, 0);
	ino = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)st.st_ino);
	confirm = g_strdup_printf("/downloads/leftovers/delete?client=%" G_GINT64_FORMAT
		"&name=Quillfeather.Junction.2014.1080p-MADEUP&inode=%s", fx->client_id, ino);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_forms_carry_token(reply.body);
	assert_contains(reply.body, "Delete Quillfeather.Junction.2014.1080p-MADEUP for good");
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("client=%" G_GINT64_FORMAT "&name=Quillfeather.Junction.2014.1080p-MADEUP&inode=%s&confirm=yes",
		fx->client_id, ino);
	reply = post_as(fx, fx->admin, NULL, "/downloads/leftovers/delete", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(old_file, G_FILE_TEST_IS_REGULAR));
	{
		g_autofree gchar *unticked = g_strdup_printf("client=%" G_GINT64_FORMAT
			"&name=Quillfeather.Junction.2014.1080p-MADEUP&inode=%s", fx->client_id, ino);

		reply = post_as(fx, fx->admin, "/imports/naming", "/downloads/leftovers/delete", unticked);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	/* Storage read-only: nothing is deleted. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	while (g_main_context_iteration(NULL, FALSE))
		;
	reply = post_as(fx, fx->admin, "/imports/naming","/downloads/leftovers/delete", form);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Nothing was deleted");
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(old_file, G_FILE_TEST_IS_REGULAR));
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	while (g_main_context_iteration(NULL, FALSE))
		;
	reply = post_as(fx, fx->admin, "/imports/naming","/downloads/leftovers/delete", form);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "was deleted, with the 1 files and folders in it.");
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(old_job, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));
	g_assert_true(g_file_test(live_file, G_FILE_TEST_IS_REGULAR));

	/* Gone since (deleted already, or by a cleanup of the person's own): said
	 * so on an ordinary page, nothing else touched. */
	reply = post_as(fx, fx->admin, "/imports/naming","/downloads/leftovers/delete", form);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "was already gone");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "nothing was deleted");
	axis_test_reply_clear(&reply);

	/* What a download accounts for is refused, whatever is posted. */
	g_assert_cmpint(g_lstat(live, &st), ==, 0);
	g_free(form);
	form = g_strdup_printf("client=%" G_GINT64_FORMAT "&name=Saltmarsh.Signal.2019.1080p-MADEUP&inode=%" G_GUINT64_FORMAT
		"&confirm=yes", fx->client_id, (guint64)st.st_ino);
	reply = post_as(fx, fx->admin, "/imports/naming","/downloads/leftovers/delete", form);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Nothing was deleted: a download Axis follows");
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(live_file, G_FILE_TEST_IS_REGULAR));
	/* So is a name that climbs out. */
	g_free(form);
	form = g_strdup_printf("client=%" G_GINT64_FORMAT "&name=..&inode=1&confirm=yes", fx->client_id);
	reply = post_as(fx, fx->admin, "/imports/naming","/downloads/leftovers/delete", form);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "That is not one entry of a client");
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(category, G_FILE_TEST_IS_DIR));

	/* A client that keeps Axis's downloads in no folder of their own: nothing
	 * listed, saying why. */
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	reply = get_as(fx, fx->admin, "/downloads/leftovers");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "downloads in no folder of their own");
	assert_lacks(reply.body, "Brackwater.Lights.2016.nfo");
	axis_test_reply_clear(&reply);
}

/* The entry @name of the client's folder on the Leftovers list, or %NULL. */
static const AxisImportLeftover *
leftover_named(
	GPtrArray   *folders,
	const gchar *name
){
	guint i;
	guint k;

	for (i = 0; i < folders->len; i++) {
		const AxisImportLeftoverFolder *folder = g_ptr_array_index(folders, i);

		for (k = 0; folder->entries != NULL && k < folder->entries->len; k++) {
			const AxisImportLeftover *entry = g_ptr_array_index(folder->entries, k);

			if (g_strcmp0(entry->name, name) == 0)
				return entry;
		}
	}
	return NULL;
}

static GPtrArray *
leftovers_now(Fixture *fx)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *folders = NULL;

	g_assert_true(axis_import_leftovers(fx->import, fx->client_id, TRUE, &folders, &error));
	g_assert_no_error(error);
	return folders;
}

/* Review of import-by-move, finding 3: every download Axis follows at the
 * client that is not finally closed accounts for its entry, by what the client
 * says of it now, before it completes; a torrent left seeding is not a
 * leftover; a name the client uses for a download it has not finished is never
 * offered for deletion; and while a download Axis follows has no place known
 * yet, nothing that could be it is offered. */
static void
test_leftovers_live(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	g_autofree gchar *progress = g_build_filename(category, "Tidewater.Hollow.2020.1080p-MADEUP", NULL);
	g_autofree gchar *progress_file = g_build_filename(progress, "Tidewater.Hollow.2020.1080p-MADEUP.mkv", NULL);
	g_autofree gchar *unpack = g_build_filename(category, "_UNPACK_Wren.Crossing.2018.1080p-MADEUP", NULL);
	g_autofree gchar *unpack_file = g_build_filename(unpack, "part.mkv", NULL);
	g_autofree gchar *seeding = g_build_filename(category, "Gorse.Meadow.2015.1080p-MADEUP", NULL);
	g_autofree gchar *seeding_file = g_build_filename(seeding, "Gorse.Meadow.2015.1080p-MADEUP.mkv", NULL);
	g_autofree gchar *old = g_build_filename(category, "Ember.Quay.2012.1080p-MADEUP", NULL);
	g_autofree gchar *old_file = g_build_filename(old, "Ember.Quay.2012.1080p-MADEUP.mkv", NULL);
	g_autofree gchar *download = NULL;
	g_autofree gchar *text = NULL;
	const AxisImportLeftover *entry;
	AxisImportLeftoverOutcome outcome;
	GStatBuf st;

	g_assert_cmpint(g_mkdir_with_parents(progress, 0755), ==, 0);
	g_assert_true(g_file_set_contents(progress_file, "a download in progress", -1, NULL));
	g_assert_cmpint(g_mkdir_with_parents(unpack, 0755), ==, 0);
	g_assert_true(g_file_set_contents(unpack_file, "a job being unpacked", -1, NULL));
	g_assert_cmpint(g_mkdir_with_parents(seeding, 0755), ==, 0);
	g_assert_true(g_file_set_contents(seeding_file, "a torrent left seeding", -1, NULL));
	g_assert_cmpint(g_mkdir_with_parents(old, 0755), ==, 0);
	g_assert_true(g_file_set_contents(old_file, "a download nobody imported", -1, NULL));
	g_assert_true(axis_downloads_add_path_map(fx->downloads_layer, fx->client_id, "/usenet/complete", fx->volume_id,
		"downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(fx->fake, "/usenet/complete/axis");
	/* In progress: no place recorded, the client says its name. */
	g_free(q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state)"
		" VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'nzo_tide', 'usenet', 'file', 'Tidewater.Hollow.2020.1080p-MADEUP',"
		" 'downloading') RETURNING id", fx->client_id));
	axis_test_download_fake_put(fx->fake, "nzo_tide", "Tidewater.Hollow.2020.1080p-MADEUP", "axis", NULL, "downloading",
		1000, 10, NULL, NULL);
	/* Imported and left seeding. */
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" client_path, volume_id, rel_path) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'hash_gorse', 'torrent', 'magnet',"
		" 'Gorse.Meadow.2015.1080p-MADEUP', 'imported', '/usenet/complete/axis/Gorse.Meadow.2015.1080p-MADEUP',"
		" %" G_GINT64_FORMAT ", 'downloads/axis/Gorse.Meadow.2015.1080p-MADEUP') RETURNING id", fx->client_id,
		fx->volume_id);
	g_free(q(fx, "INSERT INTO import (module, title_label, download_id, source_volume_id, source_path, release_name,"
		" protocol, state, finished_at, client_removal) VALUES ('movies', 'Gorse Meadow (2015)', %s, %" G_GINT64_FORMAT
		", 'downloads/axis/Gorse.Meadow.2015.1080p-MADEUP', 'Gorse.Meadow.2015.1080p-MADEUP', 'torrent', 'imported',"
		" now(), 'kept') RETURNING id", download, fx->volume_id));

	folders = leftovers_now(fx);
	g_assert_null(leftover_named(folders, "Tidewater.Hollow.2020.1080p-MADEUP"));
	g_assert_null(leftover_named(folders, "Gorse.Meadow.2015.1080p-MADEUP"));
	entry = leftover_named(folders, "_UNPACK_Wren.Crossing.2018.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	assert_contains(entry->why, "not finished");
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_true(entry->deletable);
	g_clear_pointer(&folders, g_ptr_array_unref);
	/* Nor deleted by asking: the download in progress, the client's own name. */
	g_assert_cmpint(g_lstat(progress, &st), ==, 0);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Tidewater.Hollow.2020.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(g_lstat(unpack, &st), ==, 0);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "_UNPACK_Wren.Crossing.2018.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	g_clear_pointer(&text, g_free);
	g_assert_true(g_file_test(progress_file, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(unpack_file, G_FILE_TEST_IS_REGULAR));

	/* A download Axis follows whose place nobody knows yet (being sent):
	 * nothing that could be it is offered. */
	g_free(q(fx, "INSERT INTO download (client_id, client_name, protocol, source, release_name, state) VALUES (%"
		G_GINT64_FORMAT ", 'SABnzbd', 'usenet', 'file', 'Saltmarsh.Signal.2019.1080p-MADEUP', 'sending') RETURNING id",
		fx->client_id));
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	assert_contains(entry->why, "may belong to Saltmarsh.Signal.2019.1080p-MADEUP");
	g_clear_pointer(&folders, g_ptr_array_unref);
	g_assert_cmpint(g_lstat(old, &st), ==, 0);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Ember.Quay.2012.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	assert_contains(text, "may belong to");
	g_clear_pointer(&text, g_free);
	g_assert_true(g_file_test(old_file, G_FILE_TEST_IS_REGULAR));
	/* Once it is closed (it failed), the old folder may go. */
	g_free(q(fx, "UPDATE download SET state = 'failed' WHERE state = 'sending' RETURNING id"));
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Ember.Quay.2012.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_DELETED);
	g_assert_false(g_file_test(old, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(progress_file, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(seeding_file, G_FILE_TEST_IS_REGULAR));
}

/* The category's folder of the fake client, mapped. */
static void
leftover_setup(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_add_path_map(fx->downloads_layer, fx->client_id, "/usenet/complete", fx->volume_id,
		"downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(fx->fake, "/usenet/complete/axis");
}

/* A folder in it that no download accounts for; its path on disk. */
static gchar *
leftover_folder(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	gchar *dir = g_build_filename(category, name, NULL);
	g_autofree gchar *file = g_build_filename(dir, "file.mkv", NULL);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(file, "a leftover", -1, NULL));
	return dir;
}

/* Second review, item 1: when the client's list cannot be had, or holds
 * what Axis could not read, or names a download only a little differently
 * from an entry (by case), nothing that could be it is offered for deletion. */
static void
test_leftovers_unknown(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gchar *old = NULL;
	g_autofree gchar *text = NULL;
	const AxisImportLeftover *entry;
	AxisImportLeftoverOutcome outcome;
	GStatBuf st;

	leftover_setup(fx);
	old = leftover_folder(fx, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_cmpint(g_lstat(old, &st), ==, 0);
	/* The list fails. */
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_LIST_FAILS);
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	assert_contains(entry->why, "could not be asked what it has now");
	g_clear_pointer(&folders, g_ptr_array_unref);
	assert_qf(fx, "0", "SELECT leftover_count FROM download_client WHERE id = %" G_GINT64_FORMAT, fx->client_id);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Ember.Quay.2012.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	g_clear_pointer(&text, g_free);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);

	/* An item whose place Axis cannot read (a control character in it). */
	axis_test_download_fake_put(fx->fake, "nzo_unreadable", "Odd.Name.2019-MADEUP", "axis", NULL, "downloading", 1000,
		10, "/usenet/complete/axis/Odd\001Name", NULL);
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	/* Which entry, and why (review of the fixes, item 7). */
	assert_contains(entry->why, "cannot read");
	assert_contains(entry->why, "nzo_unreadable");
	assert_contains(entry->why, "Odd.Name.2019-MADEUP");
	assert_contains(entry->why, "its place cannot be read exactly");
	g_clear_pointer(&folders, g_ptr_array_unref);
	axis_test_download_fake_drop(fx->fake, "nzo_unreadable");

	/* A download whose name differs from the entry's only by case. */
	axis_test_download_fake_put(fx->fake, "nzo_case", "ember.quay.2012.1080p-madeup", "axis", NULL, "downloading", 1000,
		10, NULL, NULL);
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	assert_contains(entry->why, "differ only by case");
	g_clear_pointer(&folders, g_ptr_array_unref);
	axis_test_download_fake_drop(fx->fake, "nzo_case");

	/* Nothing in doubt: it may go. */
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_true(entry->deletable);
	assert_qf(fx, "1", "SELECT leftover_count FROM download_client WHERE id = %" G_GINT64_FORMAT, fx->client_id);
}

/* Second review, item 9: the count the Downloads page and the dashboard show
 * is of what a person can delete: not a job being unpacked; a failed job's
 * folder that its client no longer lists may be deleted, one it still lists
 * is its client's. */
static void
test_leftovers_count(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gchar *old = NULL;
	g_autofree gchar *unpack = NULL;
	g_autofree gchar *failed = NULL;
	const AxisImportLeftover *entry;

	leftover_setup(fx);
	old = leftover_folder(fx, "Ember.Quay.2012.1080p-MADEUP");
	unpack = leftover_folder(fx, "_UNPACK_Wren.Crossing.2018.1080p-MADEUP");
	failed = leftover_folder(fx, "_FAILED_Gull.Rock.2010.1080p-MADEUP");
	folders = leftovers_now(fx);
	assert_qf(fx, "2", "SELECT leftover_count FROM download_client WHERE id = %" G_GINT64_FORMAT, fx->client_id);
	entry = leftover_named(folders, "_UNPACK_Wren.Crossing.2018.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	entry = leftover_named(folders, "_FAILED_Gull.Rock.2010.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_true(entry->deletable);
	g_clear_pointer(&folders, g_ptr_array_unref);
	/* The client still lists the failed job: its folder is its own. */
	axis_test_download_fake_put(fx->fake, "nzo_gull", "Gull.Rock.2010.1080p-MADEUP", "axis", NULL, "failed", 1000, 10,
		NULL, NULL);
	folders = leftovers_now(fx);
	g_assert_null(leftover_named(folders, "_FAILED_Gull.Rock.2010.1080p-MADEUP"));
	assert_qf(fx, "1", "SELECT leftover_count FROM download_client WHERE id = %" G_GINT64_FORMAT, fx->client_id);
}

/* Spec (import): an entry of a client's folder for Axis is offered for
 * deletion only when no client and no record accounts for it: every other
 * client whose path mappings reach the folder is asked what it has there, and
 * when one cannot be asked nothing is offered; two clients whose folders for
 * Axis are the same folder, or one holds the other, are refused, the page and
 * the client's test saying why (finding 1 of the stage 2 bug hunt: a torrent
 * still downloading at a second client sharing the folder was offered and
 * deleted as a leftover of the first). */
static void
test_leftovers_other_clients(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gchar *category = g_build_filename(fx->downloads, "axis", NULL);
	g_autofree gchar *torrent = g_build_filename(category, "Tidewater.Hollow.2020.1080p-MADEUP", NULL);
	g_autofree gchar *torrent_file = g_build_filename(torrent, "Tidewater.Hollow.2020.1080p-MADEUP.mkv", NULL);
	g_autofree gchar *old = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *second_text = NULL;
	AxisTestDownloadFake *second;
	AxisDownloadClientSpec client;
	AxisDownloadTestResult result;
	const AxisImportLeftover *entry;
	const AxisImportLeftoverFolder *folder;
	AxisImportLeftoverOutcome outcome;
	gint64 second_id = 0;
	GStatBuf st;

	leftover_setup(fx);
	old = leftover_folder(fx, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_cmpint(g_mkdir_with_parents(torrent, 0755), ==, 0);
	g_assert_true(g_file_set_contents(torrent_file, "a torrent still downloading at the other client", -1, NULL));
	/* A second client, a torrent client, whose folder for Axis is the same. */
	second = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(second);
	memset(&client, 0, sizeof(client));
	client.name = "Deluge";
	client.kind = "fixture";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads_layer, 0, &client, &second_id, &error));
	g_assert_no_error(error);
	g_assert_true(axis_downloads_add_path_map(fx->downloads_layer, second_id, "/usenet/complete", fx->volume_id,
		"downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(second, "/usenet/complete/axis");
	/* A torrent of Axis's, in progress there, in that folder. */
	second_text = g_strdup_printf("%" G_GINT64_FORMAT, second_id);
	g_free(q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state)"
		" VALUES (%s, 'Deluge', 'tor_tide', 'torrent', 'magnet', 'Tidewater.Hollow.2020.1080p-MADEUP', 'downloading')"
		" RETURNING id", second_text));
	axis_test_download_fake_put(second, "tor_tide", "Tidewater.Hollow.2020.1080p-MADEUP", "axis", NULL, "downloading",
		1000, 10, "/usenet/complete/axis/Tidewater.Hollow.2020.1080p-MADEUP", NULL);

	/* The same folder for Axis: refused, nothing listed, the page saying why. */
	folders = leftovers_now(fx);
	g_assert_cmpuint(folders->len, ==, 1);
	folder = g_ptr_array_index(folders, 0);
	g_assert_nonnull(folder->problem);
	assert_contains(folder->problem, "not its own");
	assert_contains(folder->problem, "Deluge");
	g_assert_null(leftover_named(folders, "Tidewater.Hollow.2020.1080p-MADEUP"));
	g_assert_null(leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP"));
	g_clear_pointer(&folders, g_ptr_array_unref);
	/* Nor deleted when asked. */
	g_assert_cmpint(g_lstat(torrent, &st), ==, 0);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Tidewater.Hollow.2020.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	assert_contains(text, "not its own");
	g_clear_pointer(&text, g_free);
	g_assert_true(g_file_test(torrent_file, G_FILE_TEST_IS_REGULAR));
	/* The client's test says so too. */
	g_assert_true(axis_downloads_test_client(fx->downloads_layer, second_id, &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result.note);
	assert_contains(result.note, "not its own");
	assert_contains(result.note, "SABnzbd");
	axis_download_test_result_clear(&result);
	/* And each client's page says it with its problems, after its next look. */
	{
		g_autoptr(AxisDownloadClientInfo) info = NULL;
		AxisDownloadPollReport report;

		g_assert_true(axis_downloads_poll_client(fx->downloads_layer, fx->client_id, NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_true(axis_downloads_get_client(fx->downloads_layer, fx->client_id, &info, &error));
		g_assert_nonnull(info->problem);
		assert_contains(info->problem, "not its own");
	}

	/* A folder of its own beside it, but this torrent saved in the first
	 * client's folder (as a client's manual placement can): it accounts for
	 * its entry, and the old folder may go. */
	axis_test_download_fake_set_category_dir(second, "/usenet/complete/torrents");
	folders = leftovers_now(fx);
	folder = g_ptr_array_index(folders, 0);
	g_assert_null(folder->problem);
	{
		g_autoptr(AxisDownloadClientInfo) info = NULL;
		AxisDownloadPollReport report;

		g_assert_true(axis_downloads_poll_client(fx->downloads_layer, fx->client_id, NULL, &report, &error));
		g_assert_true(axis_downloads_get_client(fx->downloads_layer, fx->client_id, &info, &error));
		g_assert_null(info->problem);
	}
	g_assert_null(leftover_named(folders, "Tidewater.Hollow.2020.1080p-MADEUP"));
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_true(entry->deletable);
	g_clear_pointer(&folders, g_ptr_array_unref);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Tidewater.Hollow.2020.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	g_clear_pointer(&text, g_free);
	g_assert_true(g_file_test(torrent_file, G_FILE_TEST_IS_REGULAR));

	/* The second client cannot be asked: nothing is offered. */
	axis_test_download_fake_set_mode(second, AXIS_TEST_DOWNLOAD_LIST_FAILS);
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	assert_contains(entry->why, "Deluge");
	g_clear_pointer(&folders, g_ptr_array_unref);
	g_assert_cmpint(g_lstat(old, &st), ==, 0);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Ember.Quay.2012.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_REFUSED);
	g_clear_pointer(&text, g_free);
	g_assert_true(g_file_test(old, G_FILE_TEST_IS_DIR));

	/* Answering again: the old folder goes, the torrent's stays. */
	axis_test_download_fake_set_mode(second, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_true(axis_import_leftover_delete(fx->import, fx->client_id, "Ember.Quay.2012.1080p-MADEUP",
		(guint64)st.st_ino, &outcome, &text, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, AXIS_IMPORT_LEFTOVER_DELETED);
	g_assert_false(g_file_test(old, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(torrent_file, G_FILE_TEST_IS_REGULAR));
	axis_test_download_fake_free(second);
}

/* Review of the fixes, items 1, 4, 7 and 8: whether anything of a client's is
 * at a path is asked of every client configured, one switched off in Axis
 * too; a torrent a person took over (moved to a category of their own, so its
 * download is gone to Axis, imported by hardlink and left at its client) is
 * accounted for whatever its download's state says; an entry Axis cannot read
 * is named, with why; another client whose folder for Axis cannot be worked
 * out makes nothing deletable, the page and the client's page saying so; and
 * the client's page stops saying so once the client has no folder of its own
 * (the Leftovers page then says that). */
static void
test_leftovers_taken_over(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gchar *old = NULL;
	g_autofree gchar *taken = NULL;
	g_autofree gchar *elsewhere = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *base = NULL;
	AxisTestDownloadFake *second;
	AxisDownloadClientSpec client;
	const AxisImportLeftover *entry;
	const AxisImportLeftoverFolder *folder;
	gint64 second_id = 0;

	leftover_setup(fx);
	old = leftover_folder(fx, "Ember.Quay.2012.1080p-MADEUP");
	taken = leftover_folder(fx, "Gorse.Meadow.2015.1080p-MADEUP");
	elsewhere = leftover_folder(fx, "Tidewater.Hollow.2020.1080p-MADEUP");
	/* A torrent a person moved to a category of their own after it was
	 * imported by hardlink: its download gone to Axis, its import keeping it
	 * at its client. */
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state,"
		" client_path, volume_id, rel_path, finished_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'hash_gorse',"
		" 'torrent', 'magnet', 'Gorse.Meadow.2015.1080p-MADEUP', 'gone',"
		" '/usenet/complete/axis/Gorse.Meadow.2015.1080p-MADEUP', %" G_GINT64_FORMAT ","
		" 'downloads/axis/Gorse.Meadow.2015.1080p-MADEUP', now()) RETURNING id", fx->client_id, fx->volume_id);
	g_free(q(fx, "INSERT INTO import (module, title_label, download_id, source_volume_id, source_path, release_name,"
		" protocol, state, finished_at, client_removal) VALUES ('movies', 'Gorse Meadow (2015)', %s, %" G_GINT64_FORMAT
		", 'downloads/axis/Gorse.Meadow.2015.1080p-MADEUP', 'Gorse.Meadow.2015.1080p-MADEUP', 'torrent', 'imported',"
		" now(), 'kept') RETURNING id", download, fx->volume_id));
	/* A second client, switched off in Axis, its program still running, with
	 * a download of another program's in the first client's folder. */
	second = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(second);
	memset(&client, 0, sizeof(client));
	client.name = "Deluge";
	client.kind = "fixture";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = FALSE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads_layer, 0, &client, &second_id, &error));
	g_assert_no_error(error);
	g_assert_true(axis_downloads_add_path_map(fx->downloads_layer, second_id, "/usenet/complete", fx->volume_id,
		"downloads", &error));
	g_assert_no_error(error);
	axis_test_download_fake_set_category_dir(second, "/usenet/complete/torrents");
	axis_test_download_fake_put(second, "tor_tide", "Tidewater.Hollow.2020.1080p-MADEUP", "tv", NULL, "completed",
		1000, 1000, "/usenet/complete/axis/Tidewater.Hollow.2020.1080p-MADEUP", NULL);

	/* The taken-over torrent and the switched-off client's download are not
	 * leftovers; the old folder is. */
	folders = leftovers_now(fx);
	folder = g_ptr_array_index(folders, 0);
	g_assert_null(folder->problem);
	g_assert_null(leftover_named(folders, "Gorse.Meadow.2015.1080p-MADEUP"));
	g_assert_null(leftover_named(folders, "Tidewater.Hollow.2020.1080p-MADEUP"));
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_true(entry->deletable);
	g_clear_pointer(&folders, g_ptr_array_unref);

	/* An entry of the client's that Axis reads by its identifier only: the
	 * page says which, and why. */
	axis_test_download_fake_put(fx->fake, "nzo_odd", "Odd.Entry.2019-MADEUP", "axis", NULL, "downloading", 1000, 10,
		NULL, NULL);
	axis_test_download_fake_set_unreadable(fx->fake, "nzo_odd", "it has no name Axis can read");
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP");
	g_assert_nonnull(entry);
	g_assert_false(entry->deletable);
	assert_contains(entry->why, "nzo_odd");
	assert_contains(entry->why, "it has no name Axis can read");
	g_clear_pointer(&folders, g_ptr_array_unref);
	axis_test_download_fake_drop(fx->fake, "nzo_odd");

	/* The second client keeps Axis's downloads in no folder of their own: it
	 * may keep them in this one, so nothing is listed, and both pages say so. */
	axis_test_download_fake_set_category_dir(second, NULL);
	folders = leftovers_now(fx);
	folder = g_ptr_array_index(folders, 0);
	g_assert_nonnull(folder->problem);
	assert_contains(folder->problem, "may not be its own");
	assert_contains(folder->problem, "Deluge");
	g_assert_null(leftover_named(folders, "Ember.Quay.2012.1080p-MADEUP"));
	g_clear_pointer(&folders, g_ptr_array_unref);
	{
		g_autoptr(AxisDownloadClientInfo) info = NULL;
		AxisDownloadPollReport report;

		g_assert_true(axis_downloads_poll_client(fx->downloads_layer, fx->client_id, NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_true(axis_downloads_get_client(fx->downloads_layer, fx->client_id, &info, &error));
		g_assert_nonnull(info->problem);
		assert_contains(info->problem, "may not be its own");
	}

	/* The first client keeps Axis's downloads in no folder of their own any
	 * more: the Leftovers page says that, and its page no longer says the
	 * folder is shared. */
	axis_test_download_fake_set_category_dir(fx->fake, NULL);
	folders = leftovers_now(fx);
	folder = g_ptr_array_index(folders, 0);
	g_assert_nonnull(folder->problem);
	assert_contains(folder->problem, "no folder of their own");
	g_clear_pointer(&folders, g_ptr_array_unref);
	{
		g_autoptr(AxisDownloadClientInfo) info = NULL;
		AxisDownloadPollReport report;

		g_assert_true(axis_downloads_poll_client(fx->downloads_layer, fx->client_id, NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_true(axis_downloads_get_client(fx->downloads_layer, fx->client_id, &info, &error));
		g_assert_true(info->problem == NULL || strstr(info->problem, "may not be its own") == NULL);
	}
	axis_test_download_fake_free(second);
}

/* Spec (import): a download the client names but whose place it does not say
 * accounts for the entry of exactly its name, white space and all (finding 5
 * of the stage 2 bug hunt: its name was compared cleaned, white space
 * collapsed, so a folder of its name with two spaces in it was offered). */
static void
test_leftovers_exact_names(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) folders = NULL;
	g_autofree gchar *spaced = NULL;
	const AxisImportLeftover *entry;

	leftover_setup(fx);
	spaced = leftover_folder(fx, "Saltmarsh  Signal (2019)");
	g_free(q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name, state)"
		" VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', 'nzo_salt', 'usenet', 'file', 'Saltmarsh  Signal (2019)',"
		" 'downloading') RETURNING id", fx->client_id));
	/* No path: the fixture client says no place of it. */
	axis_test_download_fake_put(fx->fake, "nzo_salt", "Saltmarsh  Signal (2019)", "axis", NULL, "downloading", 1000,
		10, NULL, NULL);
	folders = leftovers_now(fx);
	entry = leftover_named(folders, "Saltmarsh  Signal (2019)");
	g_assert_null(entry);
}

/* ── Assign by hand (add-force-grab-and-assign, slice B) ────────── */

static gchar *
digest_of(const gchar *body)
{
	g_autoptr(GRegex) after = g_regex_new("name=\"digest\"[^>]*value=\"([0-9a-f]{32})\"", 0, 0, NULL);
	g_autoptr(GRegex) before = g_regex_new("value=\"([0-9a-f]{32})\"[^>]*name=\"digest\"", 0, 0, NULL);
	g_autoptr(GMatchInfo) first = NULL;
	g_autoptr(GMatchInfo) second = NULL;

	if (g_regex_match(after, body, 0, &first))
		return g_match_info_fetch(first, 1);
	if (g_regex_match(before, body, 0, &second))
		return g_match_info_fetch(second, 1);
	g_error("no digest in:\n%s", body);
	return NULL;
}

/* One run of the import's job, as the queue would make it. */
static void
run_again(
	Fixture *fx,
	gint64   import_id
){
	g_autoptr(GError) error = NULL;
	AxisImportReport report;

	memset(&report, 0, sizeof(report));
	g_assert_true(axis_import_run(fx->import, import_id, NULL, &report, &error));
	g_assert_no_error(error);
}

/*
 * add-force-grab-and-assign, slice B: "Assign by hand" through its pages.
 * The import delta's scenarios "A movie's file chosen", "It would replace a
 * file", "Not a video", "Out of date" and "A viewer": the form and the
 * confirmation write nothing; the one post that writes is an
 * administrator's, with the token, the digest and the ticks.
 */
static void
test_assign(Fixture *fx, gconstpointer data)
{
	Scene s = scene(fx);
	g_autofree gchar *thistle_page = g_strdup_printf("/imports/%" G_GINT64_FORMAT, s.thistle_import);
	g_autofree gchar *thistle_assign = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", s.thistle_import);
	g_autofree gchar *rokan_assign = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", s.rokan_import);
	g_autofree gchar *hearth_assign = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/assign", s.hearth_import);
	g_autofree gchar *position = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND kind = 'video'", s.thistle_import);
	g_autofree gchar *rokan_position = q(fx, "SELECT position FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND path LIKE 'Rokan.1998.1080p%%'", s.rokan_import);
	g_autofree gchar *choice = g_strdup_printf("step=review&file=%s", position);
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *old = NULL;
	g_autofree gchar *old_id = NULL;
	AxisTestReply reply;

	/* The import's page offers it to an administrator, not to a viewer. */
	reply = get_as(fx, fx->admin, thistle_page);
	assert_contains(reply.body, "Assign by hand");
	assert_contains(reply.body, thistle_assign);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, thistle_page);
	assert_lacks(reply.body, thistle_assign);
	axis_test_reply_clear(&reply);

	/* Scenario: A viewer.  Neither the form nor a post is a viewer's, and a
	 * post without the token is no one's. */
	reply = get_as(fx, fx->viewer, thistle_assign);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->viewer, thistle_page, thistle_assign, choice);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, thistle_assign, choice);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* The form: every video file with its choice; it writes nothing. */
	reply = get_as(fx, fx->admin, thistle_assign);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "This file is the movie");
	assert_contains(reply.body, "Rokan.1998.1080p.WEB-DL.x264-GRP.mkv");
	assert_csp_clean(reply.body);
	assert_forms_carry_token(reply.body);
	save_page("assign-form.html", reply.body);
	axis_test_reply_clear(&reply);
	/* An import that is not waiting for a person cannot be assigned. */
	reply = get_as(fx, fx->admin, hearth_assign);
	assert_contains(reply.body, "not waiting for a person");
	axis_test_reply_clear(&reply);

	/* Scenario: Not a video.  A file that is no video of the import is
	 * refused. */
	reply = post_as(fx, fx->admin, thistle_assign, thistle_assign, "step=review&file=77");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "not a video file");
	axis_test_reply_clear(&reply);
	/* An apply that was never reviewed is not one. */
	reply = post_as(fx, fx->admin, thistle_assign, thistle_assign, "step=apply&file=0&digest=nope");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* Scenario: A movie's file chosen.  The confirmation says what the file
	 * becomes, and writes nothing. */
	reply = post_as(fx, fx->admin, thistle_assign, thistle_assign, choice);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Confirm the assignment");
	assert_contains(reply.body, "is imported as Thistle (1981)");
	assert_contains(reply.body, "Its name in the library: Thistle (1981)");
	assert_csp_clean(reply.body);
	assert_forms_carry_token(reply.body);
	save_page("assign-confirm.html", reply.body);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "blocked/0", "SELECT state || '/' || (SELECT count(*) FROM import_assignment) FROM import"
		" WHERE id = %" G_GINT64_FORMAT, s.thistle_import);
	assert_qf(fx, "0", "SELECT count(*) FROM import_action WHERE action = 'assign_files'");

	/* Scenario: Out of date.  A file arrives for the movie after the
	 * confirmation was shown: the post applies nothing and shows it again. */
	old = g_build_filename(fx->films, "Thistle (1981) {tmdb-6005}", "Thistle (1981) {tmdb-6005} [WEB-DL 720p]-OLD.mkv",
		NULL);
	write_media(old, FILM_LINE, 64 * 1024, 11);
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	old_id = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE 'Thistle%%-OLD.mkv'");
	g_assert_nonnull(old_id);
	axis_test_db_exec(fx->t->db, "UPDATE library_root_state SET complete_generation = generation");
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, release_name)"
			" VALUES (%s, %" G_GINT64_FORMAT ", 'webdl_1080p', 'Thistle.1981.1080p.WEB-DL.x264-OLD')", old_id, s.thistle);

		axis_test_db_exec(fx->t->db, sql);
	}
	form = g_strdup_printf("step=apply&file=%s&digest=%s", position, digest);
	reply = post_as(fx, fx->admin, thistle_assign, thistle_assign, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "changed since this page was shown");
	/* Scenario: It would replace a file.  The confirmation names the file
	 * and asks for its tick. */
	assert_contains(reply.body, "Replace ");
	assert_contains(reply.body, "-OLD.mkv");
	g_free(digest);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "blocked/0", "SELECT state || '/' || (SELECT count(*) FROM import_assignment) FROM import"
		" WHERE id = %" G_GINT64_FORMAT, s.thistle_import);
	/* Without the tick nothing is applied, and the page says which is
	 * missing. */
	g_free(form);
	form = g_strdup_printf("step=apply&file=%s&digest=%s", position, digest);
	reply = post_as(fx, fx->admin, thistle_assign, thistle_assign, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Tick replacing");
	axis_test_reply_clear(&reply);
	assert_qf(fx, "blocked/0", "SELECT state || '/' || (SELECT count(*) FROM import_assignment) FROM import"
		" WHERE id = %" G_GINT64_FORMAT, s.thistle_import);
	g_assert_true(g_file_test(old, G_FILE_TEST_EXISTS));
	/* With it: recorded with the user, queued, and imported as the movie
	 * although its name reads as another; the old file is in the recycle
	 * folder, not deleted. */
	g_free(form);
	form = g_strdup_printf("step=apply&file=%s&digest=%s&replace_%s=yes", position, digest, old_id);
	reply = post_as(fx, fx->admin, thistle_assign, thistle_assign, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "waiting/1/root", "SELECT state || '/' || (SELECT count(*) FROM import_assignment) || '/' ||"
		" (SELECT by_user FROM import_action WHERE action = 'assign_files') FROM import WHERE id = %" G_GINT64_FORMAT,
		s.thistle_import);
	run_again(fx, s.thistle_import);
	assert_qf(fx, "imported", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.thistle_import);
	assert_qf(fx, "assigned/root/true", "SELECT mf.hand_how || '/' || mf.hand_by || '/' || mf.kept::text"
		" FROM movie_file mf JOIN library_file lf ON lf.id = mf.file_id WHERE mf.movie_id = %" G_GINT64_FORMAT
		" AND lf.state = 'present'", s.thistle);
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE import_id = %" G_GINT64_FORMAT " AND state = 'kept'",
		s.thistle_import);
	g_assert_false(g_file_test(old, G_FILE_TEST_EXISTS));
	reply = get_as(fx, fx->admin, thistle_page);
	assert_contains(reply.body, "Assigned by hand:");
	axis_test_reply_clear(&reply);

	/* One of two files that could each be the movie: the one chosen is
	 * imported, the other is left alone, in the download. */
	g_free(form);
	form = g_strdup_printf("step=review&file=%s", rokan_position);
	reply = post_as(fx, fx->admin, rokan_assign, rokan_assign, form);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "1 other video file is left alone");
	g_free(digest);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("step=apply&file=%s&digest=%s", rokan_position, digest);
	reply = post_as(fx, fx->admin, rokan_assign, rokan_assign, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	run_again(fx, s.rokan_import);
	assert_qf(fx, "imported", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, s.rokan_import);
	assert_qf(fx, "1", "SELECT count(*) FROM import_file WHERE import_id = %" G_GINT64_FORMAT
		" AND reasons::text LIKE '%%Left alone when the files were assigned by hand%%'", s.rokan_import);
	{
		g_autofree gchar *left = g_build_filename(fx->downloads, "Rokan.1998.1080p.BluRay.x264-GRP",
			"Rokan.1998.Directors.Cut.1080p.BluRay.x264-GRP.mkv", NULL);

		g_assert_true(g_file_test(left, G_FILE_TEST_EXISTS));
	}
}

/* ── Duplicates (add-force-grab-and-assign, slice D) ────────────── */

/* A movie with two files in its folder, both linked: @good (WEB-DL 1080p,
 * the larger) and @poor (HDTV 720p, which the Balanced profile does not
 * allow), or two of the same quality and size when @same.  Their library
 * file ids. */
static gint64
two_files(
	Fixture     *fx,
	const gchar *provider_id,
	const gchar *folder,
	gboolean     same,
	gint64      *good,
	gint64      *poor
){
	g_autofree gchar *a = g_strdup_printf("%s/%s/%s [WEB-DL 1080p].mkv", fx->films, folder, folder);
	g_autofree gchar *b = g_strdup_printf("%s/%s/%s [%s].mkv", fx->films, folder, folder, same ? "WEB-DL 1080p] [2" :
		"HDTV 720p");
	g_autofree gchar *a_id = NULL;
	g_autofree gchar *b_id = NULL;
	g_autofree gchar *sql = NULL;
	gint64 movie = add_film(fx, provider_id);

	write_media(a, FILM_LINE, 96 * 1024, 3);
	write_media(b, FILM_LINE, same ? 96 * 1024 : 64 * 1024, 5);
	axis_scan_result_free(axis_test_library_scan(fx->t, fx->root_id));
	axis_test_db_exec(fx->t->db, "UPDATE library_root_state SET complete_generation = generation");
	a_id = q(fx, "SELECT id FROM library_file WHERE rel_path = '%s/%s [WEB-DL 1080p].mkv'", folder, folder);
	b_id = q(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%s/%%' AND id <> %s AND kind = 'video'", folder, a_id);
	g_assert_nonnull(a_id);
	g_assert_nonnull(b_id);
	sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality, release_name) VALUES"
		" (%s, %" G_GINT64_FORMAT ", 'webdl_1080p', 'A.1080p.WEB-DL.x264-GRP'),"
		" (%s, %" G_GINT64_FORMAT ", '%s', '%s')", a_id, movie, b_id, movie, same ? "webdl_1080p" : "hdtv_720p",
		same ? "A.1080p.WEB-DL.x264-GRP" : "B.x264-GRP");
	axis_test_db_exec(fx->t->db, sql);
	/* Both analysed as they are, an hour and a half each: without that the
	 * page recommends nothing, and no file may stay on its own. */
	axis_test_db_exec(fx->t->db, "INSERT INTO media_info (file_id, status, probe_version, file_size, mtime_sec,"
		" mtime_nsec, ctime_sec, ctime_nsec, inode, container, duration_ms, resolution_class) SELECT f.id, 'ok', 1,"
		" f.size, f.mtime_sec, f.mtime_nsec, f.ctime_sec, f.ctime_nsec, f.inode, 'matroska', 90 * 60000, '1080p'"
		" FROM library_file f WHERE f.kind = 'video' ON CONFLICT (file_id) DO NOTHING");
	*good = g_ascii_strtoll(a_id, NULL, 10);
	*poor = g_ascii_strtoll(b_id, NULL, 10);
	return movie;
}

/* Whether the review's tick for library file @id starts ticked. */
static gboolean
tick_set(
	const gchar *body,
	gint64       id
){
	g_autofree gchar *needle = g_strdup_printf("id=\"dup-remove-%" G_GINT64_FORMAT "\"", id);
	const gchar *at = strstr(body, needle);
	const gchar *start;
	const gchar *end;

	if (at == NULL)
		g_error("no tick for file %" G_GINT64_FORMAT " in:\n%s", id, body);
	start = g_strrstr_len(body, at - body, "<input");
	end = strchr(at, '>');
	g_assert_nonnull(start);
	g_assert_nonnull(end);
	return g_strstr_len(start, end - start, "checked") != NULL;
}

static gchar *
path_of(
	Fixture *fx,
	gint64   file
){
	g_autofree gchar *rel = q(fx, "SELECT rel_path FROM library_file WHERE id = %" G_GINT64_FORMAT, file);

	g_assert_nonnull(rel);
	return g_build_filename(fx->films, rel, NULL);
}

static guint64
inode_of(const gchar *path)
{
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	return (guint64)st.st_ino;
}

/* How many tries of a removal's record, and of its history, the hook still
 * fails (atomic: the hook runs on the page's worker). */
static gint dup_fail_record;
static gint dup_fail_history;

static AxisImportHookVerdict
dup_hook(
	const gchar *step,
	gint64       import_id,
	gint64       file_id,
	gpointer     data
){
	gint *left = g_strcmp0(step, "duplicate_moved") == 0 ? &dup_fail_record :
		g_strcmp0(step, "duplicate_history") == 0 ? &dup_fail_history : NULL;

	if (left == NULL || g_atomic_int_get(left) <= 0)
		return AXIS_IMPORT_HOOK_GO;
	g_atomic_int_add(left, -1);
	return AXIS_IMPORT_HOOK_FAIL;
}

/* The digest of @movie's review as it stands. */
static gchar *
review_digest(
	Fixture *fx,
	gint64   movie
){
	g_autofree gchar *path = g_strdup_printf("/duplicates/review?module=movies&owner=%" G_GINT64_FORMAT "&item=0", movie);
	AxisTestReply reply = get_as(fx, fx->admin, path);
	gchar *digest;

	g_assert_cmpuint(reply.status, ==, 200);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	return digest;
}

/* A removal posted for @movie with @ticks ("remove_12=yes&remove_13=yes")
 * and @digest: refused with 409, saying @says, and nothing moved. */
static void
assert_refused(
	Fixture     *fx,
	gint64       movie,
	const gchar *review,
	const gchar *digest,
	const gchar *ticks,
	const gchar *says,
	const gchar *first,
	const gchar *second
){
	g_autofree gchar *form = g_strdup_printf("module=movies&owner=%" G_GINT64_FORMAT "&item=0&digest=%s%s%s", movie,
		digest, ticks[0] != '\0' ? "&" : "", ticks);
	AxisTestReply reply = post_as(fx, fx->admin, review, "/duplicates/remove", form);

	if (reply.status != 409)
		g_error("expected 409 for '%s', got %u:\n%s", ticks, reply.status, reply.body);
	assert_contains(reply.body, says);
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(first, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(second, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "2", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, movie);
	assert_qf(fx, "0", "SELECT count(*) FROM import_recycle");
}

/*
 * add-force-grab-and-assign, slice D: the Duplicates page, the review of one
 * movie and the removal.  The import delta's scenarios "A movie with two
 * files", "Axis cannot tell", "Kept by hand", "A viewer looks", "The
 * recommended file removed", "Every file ticked", "A pinned file", "Its disk
 * cannot be written", "An import is open", "The page is out of date", "A
 * viewer posts" and "Nothing is kept in the recycle folder".  The page and
 * the review write nothing; the one post that removes is an administrator's,
 * with the token, the digest and the ticks, and it removes the ticked files
 * only, into the recycle folder.
 */
static void
test_duplicates(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *review = NULL;
	g_autofree gchar *thistle_review = NULL;
	g_autofree gchar *colliery_review = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *thistle_digest = NULL;
	g_autofree gchar *good_path = NULL;
	g_autofree gchar *poor_path = NULL;
	g_autofree gchar *pinned_path = NULL;
	g_autofree gchar *link_path = NULL;
	g_autofree gchar *recycled = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *ticks = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *href = NULL;
	gint64 hearth_good = 0, hearth_poor = 0;
	gint64 rokan_a = 0, rokan_b = 0;
	gint64 colliery_good = 0, colliery_kept = 0;
	gint64 thistle_good = 0, thistle_pinned = 0;
	gint64 hearth = two_files(fx, "107", "Hearth (1995) {tmdb-6001}", FALSE, &hearth_good, &hearth_poor);
	gint64 rokan = two_files(fx, "108", "Rokan (1998) {tmdb-6002}", TRUE, &rokan_a, &rokan_b);
	gint64 colliery = two_files(fx, "109", "Colliery (2004) {tmdb-6004}", FALSE, &colliery_good, &colliery_kept);
	gint64 thistle = two_files(fx, "110", "Thistle (1981) {tmdb-6005}", FALSE, &thistle_good, &thistle_pinned);
	guint64 good_inode;
	gsize good_size = 0;
	gsize after_size = 0;
	AxisTestReply reply;

	good_path = path_of(fx, hearth_good);
	poor_path = path_of(fx, hearth_poor);
	pinned_path = path_of(fx, thistle_pinned);
	/* Colliery's worse file was linked by hand and is kept; Thistle's has
	 * another name on the disk. */
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET hand_how = 'linked', hand_by = 'root', hand_at = now(),"
		" kept = true WHERE quality = 'hdtv_720p' AND movie_id = (SELECT m.id FROM movie m JOIN meta_title t"
		" ON t.id = m.title_id WHERE t.title = 'Colliery')");
	link_path = g_build_filename(fx->downloads, "thistle-other-name.mkv", NULL);
	g_assert_cmpint(link(pinned_path, link_path), ==, 0);
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE library_file SET pinned = true, pinned_links = true, nlink = 2"
			" WHERE id = %" G_GINT64_FORMAT, thistle_pinned);

		axis_test_db_exec(fx->t->db, sql);
	}
	review = g_strdup_printf("/duplicates/review?module=movies&owner=%" G_GINT64_FORMAT "&item=0", hearth);
	thistle_review = g_strdup_printf("/duplicates/review?module=movies&owner=%" G_GINT64_FORMAT "&item=0", thistle);
	colliery_review = g_strdup_printf("/duplicates/review?module=movies&owner=%" G_GINT64_FORMAT "&item=0", colliery);
	href = g_strdup_printf("owner=%" G_GINT64_FORMAT "&", hearth);

	/* Scenario: A viewer looks.  The groups and the recommendations, and no
	 * way to review or remove. */
	reply = get_as(fx, fx->viewer, "/duplicates");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_csp_clean(reply.body);
	save_page("duplicates.html", reply.body);
	/* Scenario: A movie with two files. */
	assert_contains(reply.body, "Hearth (1995)");
	assert_contains(reply.body, "Axis would keep Hearth (1995) {tmdb-6001} [WEB-DL 1080p].mkv");
	assert_contains(reply.body, "its quality is higher");
	assert_lacks(reply.body, "allows its quality and not that of the other");
	assert_contains(reply.body, "Remove. Of the file to keep");
	/* Each file's running time, as the analysis read it. */
	assert_contains(reply.body, "Running time");
	assert_contains(reply.body, "1 h 30 min");
	assert_contains(reply.body, "/duplicates/recycled");
	/* Scenario: Axis cannot tell. */
	assert_contains(reply.body, "Axis cannot tell which file is better");
	assert_contains(reply.body, "have the same quality and the same size");
	assert_contains(reply.body, "No recommendation.");
	/* Scenario: Kept by hand. */
	assert_contains(reply.body, "Axis would remove it");
	assert_contains(reply.body, "root linked it by hand, and it is kept by hand");
	/* Pinned: never offered. */
	assert_contains(reply.body, "Stays, whatever Axis would recommend: it has another name on its disk");
	assert_contains(reply.body, "/analysis/duplicates");
	assert_lacks(reply.body, "/duplicates/review");
	assert_lacks(reply.body, "/duplicates/remove");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/duplicates");
	assert_contains(reply.body, "/duplicates/review?module=movies");
	assert_contains(reply.body, href);
	axis_test_reply_clear(&reply);

	/* Scenario: A viewer posts.  Neither the review nor a post is a
	 * viewer's, and a post without the token is no one's. */
	reply = get_as(fx, fx->viewer, review);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* The review: the recommended file ticked, the file to keep not; it
	 * writes nothing. */
	reply = get_as(fx, fx->admin, review);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_csp_clean(reply.body);
	assert_forms_carry_token(reply.body);
	save_page("duplicates-review.html", reply.body);
	g_assert_true(tick_set(reply.body, hearth_poor));
	g_assert_false(tick_set(reply.body, hearth_good));
	assert_contains(reply.body, "runs 1 h 30 min");
	assert_contains(reply.body, "deleted from there after 2 days");
	assert_contains(reply.body, "At least one file must stay");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "2", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, hearth);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_event WHERE kind = 'hand_link'");

	ticks = g_strdup_printf("remove_%" G_GINT64_FORMAT "=yes", hearth_poor);
	form = g_strdup_printf("module=movies&owner=%" G_GINT64_FORMAT "&item=0&digest=%s&%s", hearth, digest, ticks);
	reply = post_as(fx, fx->viewer, "/duplicates", "/duplicates/remove", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, "/duplicates/remove", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(poor_path, G_FILE_TEST_IS_REGULAR));

	/* Scenario: Kept by hand.  Its tick is not set for the person. */
	reply = get_as(fx, fx->admin, colliery_review);
	g_assert_false(tick_set(reply.body, colliery_kept));
	g_assert_false(tick_set(reply.body, colliery_good));
	assert_contains(reply.body, "tick it yourself only if you mean to remove it");
	axis_test_reply_clear(&reply);

	/* Not copies of one thing (the review's blocker B2).  Rokan's two files
	 * run for different lengths, as the two parts of a movie do: the page
	 * says so, recommends nothing and ticks nothing.  Then one of them could
	 * not be read. */
	{
		g_autofree gchar *shorter = g_strdup_printf("UPDATE media_info SET duration_ms = 45 * 60000 WHERE file_id = %"
			G_GINT64_FORMAT, rokan_b);
		g_autofree gchar *unread = g_strdup_printf("UPDATE media_info SET status = 'failed', error = 'cut short',"
			" duration_ms = NULL, container = NULL, resolution_class = NULL WHERE file_id = %" G_GINT64_FORMAT, rokan_b);
		g_autofree gchar *whole = g_strdup_printf("UPDATE media_info SET status = 'ok', error = NULL,"
			" duration_ms = 90 * 60000, container = 'matroska', resolution_class = '1080p' WHERE file_id = %"
			G_GINT64_FORMAT, rokan_b);
		g_autofree gchar *rokan_review = g_strdup_printf("/duplicates/review?module=movies&owner=%" G_GINT64_FORMAT
			"&item=0", rokan);

		axis_test_db_exec(fx->t->db, shorter);
		reply = get_as(fx, fx->viewer, "/duplicates");
		assert_contains(reply.body, "These files run for different lengths, so they may not be copies of the same thing");
		assert_contains(reply.body, "runs 45 min");
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, rokan_review);
		g_assert_false(tick_set(reply.body, rokan_a));
		g_assert_false(tick_set(reply.body, rokan_b));
		assert_contains(reply.body, "Nothing is recommended and nothing is ticked for you");
		axis_test_reply_clear(&reply);
		axis_test_db_exec(fx->t->db, unread);
		reply = get_as(fx, fx->viewer, "/duplicates");
		assert_contains(reply.body, "Axis could not read one of these files");
		assert_contains(reply.body, "Could not be read");
		axis_test_reply_clear(&reply);
		axis_test_db_exec(fx->t->db, whole);
	}
	/* The file with the better name is the one that cannot be read (cut
	 * short when it was copied): it is not recommended, the complete file is
	 * not ticked, and a person who ticks the complete file all the same is
	 * refused, as the unreadable one cannot be the file that stays.  The same
	 * for a file analysed before it last changed. */
	{
		g_autofree gchar *unread = g_strdup_printf("UPDATE media_info SET status = 'failed', error = 'cut short',"
			" duration_ms = NULL, container = NULL, resolution_class = NULL WHERE file_id = %" G_GINT64_FORMAT,
			hearth_good);
		g_autofree gchar *stale = g_strdup_printf("UPDATE media_info SET status = 'ok', error = NULL,"
			" duration_ms = 90 * 60000, container = 'matroska', resolution_class = '1080p', file_size = file_size + 1"
			" WHERE file_id = %" G_GINT64_FORMAT, hearth_good);
		g_autofree gchar *whole = g_strdup_printf("UPDATE media_info SET file_size = file_size - 1 WHERE file_id = %"
			G_GINT64_FORMAT, hearth_good);
		g_autofree gchar *fresh = NULL;

		axis_test_db_exec(fx->t->db, unread);
		reply = get_as(fx, fx->admin, review);
		g_assert_false(tick_set(reply.body, hearth_poor));
		g_assert_false(tick_set(reply.body, hearth_good));
		assert_contains(reply.body, "Axis could not read one of these files");
		assert_contains(reply.body, "could not be read by the analysis");
		fresh = digest_of(reply.body);
		axis_test_reply_clear(&reply);
		assert_refused(fx, hearth, review, fresh, ticks, "could not be read as a video when it was analysed", good_path,
			poor_path);
		g_clear_pointer(&fresh, g_free);
		axis_test_db_exec(fx->t->db, stale);
		fresh = review_digest(fx, hearth);
		assert_refused(fx, hearth, review, fresh, ticks, "has not been analysed since it was added or last changed",
			good_path, poor_path);
		axis_test_db_exec(fx->t->db, whole);
	}
	/* The digest covers who linked a file by hand (L4): the review named
	 * root, and the link is another person's by the time of the post. */
	{
		g_autofree gchar *colliery_digest = review_digest(fx, colliery);
		g_autofree gchar *kept_tick = g_strdup_printf("remove_%" G_GINT64_FORMAT "=yes", colliery_kept);
		g_autofree gchar *other = g_strdup_printf("UPDATE movie_file SET hand_by = 'someone' WHERE file_id = %"
			G_GINT64_FORMAT, colliery_kept);
		g_autofree gchar *back = g_strdup_printf("UPDATE movie_file SET hand_by = 'root' WHERE file_id = %"
			G_GINT64_FORMAT, colliery_kept);
		g_autofree gchar *first = path_of(fx, colliery_good);
		g_autofree gchar *second = path_of(fx, colliery_kept);

		axis_test_db_exec(fx->t->db, other);
		assert_refused(fx, colliery, colliery_review, colliery_digest, kept_tick, "changed since this page was shown",
			first, second);
		axis_test_db_exec(fx->t->db, back);
	}

	/* Scenario: Every file ticked. */
	{
		g_autofree gchar *all = g_strdup_printf("remove_%" G_GINT64_FORMAT "=yes&remove_%" G_GINT64_FORMAT "=yes",
			hearth_good, hearth_poor);
		g_autofree gchar *other = g_strdup_printf("remove_%" G_GINT64_FORMAT "=yes", rokan_a);
		g_autoptr(GString) many = g_string_new(NULL);
		guint n;

		/* More files than one request moves while the title is locked. */
		for (n = 0; n < 21; n++)
			g_string_append_printf(many, "%sremove_%u=yes", n > 0 ? "&" : "", 900001 + n);
		assert_refused(fx, hearth, review, digest, many->str, "More than 20 files were ticked", good_path, poor_path);
		assert_refused(fx, hearth, review, digest, all, "One must stay", good_path, poor_path);
		/* Nothing ticked, and a file that is another movie's. */
		assert_refused(fx, hearth, review, digest, "", "Nothing was ticked", good_path, poor_path);
		assert_refused(fx, hearth, review, digest, other, "is not one of the files of Hearth (1995)", good_path,
			poor_path);
	}

	/* Scenario: A pinned file.  It has no tick on the review, and a post
	 * that names it all the same is refused whole. */
	reply = get_as(fx, fx->admin, thistle_review);
	{
		g_autofree gchar *field = g_strdup_printf("remove_%" G_GINT64_FORMAT, thistle_pinned);

		assert_lacks(reply.body, field);
	}
	assert_contains(reply.body, "These stay whatever you tick");
	thistle_digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *pinned = g_strdup_printf("remove_%" G_GINT64_FORMAT "=yes", thistle_pinned);
		g_autofree gchar *good = path_of(fx, thistle_good);

		assert_refused(fx, thistle, thistle_review, thistle_digest, pinned, "cannot be removed here: it has another name",
			good, pinned_path);
	}

	/* Scenario: The page is out of date.  The file was kept by hand after
	 * the review was shown. */
	{
		g_autofree gchar *keep = g_strdup_printf("UPDATE movie_file SET kept = true WHERE file_id = %" G_GINT64_FORMAT,
			hearth_poor);
		g_autofree gchar *let = g_strdup_printf("UPDATE movie_file SET kept = false WHERE file_id = %" G_GINT64_FORMAT,
			hearth_poor);

		axis_test_db_exec(fx->t->db, keep);
		assert_refused(fx, hearth, review, digest, ticks, "changed since this page was shown", good_path, poor_path);
		axis_test_db_exec(fx->t->db, let);
	}

	/* Scenario: An import is open; and a grab of the movie. */
	{
		g_autofree gchar *open = g_strdup_printf("INSERT INTO import (module, owner_id, title_id, title_label,"
			" source_path, state) VALUES ('movies', %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'Hearth (1995)',"
			" 'downloads/Hearth.Other', 'blocked')", hearth, title_id_of(fx, hearth));
		g_autofree gchar *grab = g_strdup_printf("INSERT INTO movie_grab (movie_id, release_name, protocol, name_key)"
			" VALUES (%" G_GINT64_FORMAT ", 'Hearth.1995.2160p.WEB-DL.x265-GRP', 'torrent', 'hearth-open-grab')", hearth);

		axis_test_db_exec(fx->t->db, open);
		assert_refused(fx, hearth, review, digest, ticks, "An import of Hearth (1995) is open", good_path, poor_path);
		reply = get_as(fx, fx->admin, review);
		assert_contains(reply.body, "An import of Hearth (1995) is open");
		assert_lacks(reply.body, "/duplicates/remove");
		axis_test_reply_clear(&reply);
		axis_test_db_exec(fx->t->db, "DELETE FROM import WHERE source_path = 'downloads/Hearth.Other'");
		axis_test_db_exec(fx->t->db, grab);
		assert_refused(fx, hearth, review, digest, ticks, "A download for this movie is on the way", good_path,
			poor_path);
		axis_test_db_exec(fx->t->db, "DELETE FROM movie_grab WHERE name_key = 'hearth-open-grab'");
	}

	/* Scenario: Its disk cannot be written.  Read-only storage: no file is
	 * offered, and the post made from the earlier review is refused. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "true", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
	reply = get_as(fx, fx->admin, review);
	assert_contains(reply.body, "Read-only storage is on");
	assert_contains(reply.body, "No file of it can be removed now");
	assert_lacks(reply.body, "/duplicates/remove");
	axis_test_reply_clear(&reply);
	assert_refused(fx, hearth, "/duplicates", digest, ticks, "changed since this page was shown", good_path, poor_path);
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
	/* The disk gone since the review: no file that stays can be seen. */
	axis_test_library_unplug(fx->t, fx->disk, "disk1");
	reply = post_as(fx, fx->admin, "/duplicates", "/duplicates/remove", form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "can be seen on its disk now");
	axis_test_reply_clear(&reply);
	axis_test_library_plug(fx->t, fx->disk, "disk1");
	g_assert_true(g_file_test(good_path, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(poor_path, G_FILE_TEST_IS_REGULAR));
	assert_qf(fx, "0", "SELECT count(*) FROM import_recycle");

	/* Scenario: Nothing is kept in the recycle folder. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_RETENTION, "0", &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->admin, review);
	assert_contains(reply.body, "deleted for good");
	assert_contains(reply.body, "It cannot be brought back");
	assert_contains(reply.body, "Delete the ticked files for good");
	axis_test_reply_clear(&reply);
	/* The digest of the review made under another retention does not hold. */
	assert_refused(fx, hearth, review, digest, ticks, "changed since this page was shown", good_path, poor_path);
	g_assert_true(axis_settings_set(fx->settings, AXIS_IMPORT_SETTING_RETENTION, "2d", &error));
	g_assert_no_error(error);

	/* Scenario: The recommended file removed.  The ticked file is in the
	 * recycle folder of its disk and linked to nothing; the other is the very
	 * file it was (its inode and its content); nothing else moved. */
	good_inode = inode_of(good_path);
	g_assert_true(g_file_get_contents(good_path, &before, &good_size, NULL));
	/* The database is lost once after the file moved, and once when the
	 * history is written (L1): each is tried again on another connection, and
	 * the removal ends as if nothing had happened. */
	axis_import_set_hook(fx->import, dup_hook, NULL);
	g_atomic_int_set(&dup_fail_record, 1);
	g_atomic_int_set(&dup_fail_history, 1);
	reply = post_as(fx, fx->admin, review, "/duplicates/remove", form);
	if (reply.status != 303)
		g_error("expected a redirect, got %u:\n%s", reply.status, reply.body);
	g_assert_cmpint(g_atomic_int_get(&dup_fail_record), ==, 0);
	g_assert_cmpint(g_atomic_int_get(&dup_fail_history), ==, 0);
	g_assert_nonnull(strstr(axis_test_reply_header(&reply, "Location"), "/duplicates?done=1"));
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(poor_path, G_FILE_TEST_EXISTS));
	recycled = g_build_filename(fx->disk, ".axis-recycle", "films", "Hearth (1995) {tmdb-6001}",
		"Hearth (1995) {tmdb-6001} [HDTV 720p].mkv", NULL);
	g_assert_true(g_file_test(recycled, G_FILE_TEST_IS_REGULAR));
	g_assert_true(g_file_test(good_path, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpuint(inode_of(good_path), ==, good_inode);
	g_assert_true(g_file_get_contents(good_path, &after, &after_size, NULL));
	g_assert_cmpmem(before, good_size, after, after_size);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, hearth);
	assert_qf(fx, "0", "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT, hearth_poor);
	assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND state = 'present'",
		hearth_good);
	assert_qf(fx, "kept|true|true|moved","SELECT state || '|' || (import_id IS NULL)::text || '|' || (remove_after >"
		" now() + interval '1 day')::text || '|' || how FROM import_recycle");
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle");
	assert_qf(fx, "root", "SELECT by_user FROM movie_event WHERE kind = 'hand_link' AND movie_id = %" G_GINT64_FORMAT,
		hearth);
	assert_qf(fx, "true", "SELECT (text LIKE 'root removed a duplicate file of Hearth (1995)%%' AND text LIKE"
		" '%%[HDTV 720p].mkv%%' AND text LIKE '%%Stays linked:%%[WEB-DL 1080p].mkv%%')::text FROM movie_event"
		" WHERE kind = 'hand_link'");
	/* Every other movie's files are where they were. */
	assert_qf(fx, "6", "SELECT count(*) FROM movie_file WHERE movie_id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		", %" G_GINT64_FORMAT ")", rokan, colliery, thistle);
	g_assert_true(g_file_test(pinned_path, G_FILE_TEST_IS_REGULAR));
	/* The movie is no duplicate any more, and the same post again finds none. */
	reply = get_as(fx, fx->admin, "/duplicates?done=1");
	assert_contains(reply.body, "1 file was removed from the library");
	assert_lacks(reply.body, href);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, "/duplicates", "/duplicates/remove", form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	assert_qf(fx, "1", "SELECT count(*) FROM import_recycle");

	/* A removed duplicate can be found again (L5): the history names where
	 * it is and when it is deleted, and a list anyone signed in may read says
	 * the same.  A list only: it has no form. */
	assert_qf(fx, "true", "SELECT (text LIKE '%%now .axis-recycle/films/Hearth (1995) {tmdb-6001}/%%[HDTV 720p].mkv"
		" on Disk one%%' AND text LIKE '%%from where it is deleted after 20%% UTC%%')::text FROM movie_event"
		" WHERE kind = 'hand_link'");
	assert_qf(fx, "Hearth (1995)|root", "SELECT label || '|' || by_user FROM import_recycle");
	reply = get_as(fx, fx->viewer, "/duplicates/recycled");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_csp_clean(reply.body);
	save_page("duplicates-recycled.html", reply.body);
	assert_contains(reply.body, "Recycled duplicates");
	assert_contains(reply.body, "Hearth (1995)");
	assert_contains(reply.body, ".axis-recycle/films/Hearth (1995) {tmdb-6001}/");
	assert_contains(reply.body, "on Disk one");
	assert_contains(reply.body, "by root");
	assert_contains(reply.body, "In the recycle folder");
	assert_lacks(reply.body, "<form method=\"post\" action=\"/duplicates");
	axis_test_reply_clear(&reply);

	/* The database stays lost after a file moved (L1): the record is tried
	 * twice and fails twice.  The page says where the file is, not "not
	 * removed"; the history says the same; and the clean-up job finishes the
	 * record, once the row is old enough not to be a removal under way. */
	{
		g_autofree gchar *colliery_digest = review_digest(fx, colliery);
		g_autofree gchar *post = g_strdup_printf("module=movies&owner=%" G_GINT64_FORMAT "&item=0&digest=%s&remove_%"
			G_GINT64_FORMAT "=yes", colliery, colliery_digest, colliery_kept);
		g_autofree gchar *kept_path = path_of(fx, colliery_kept);
		g_autofree gchar *stays_path = path_of(fx, colliery_good);
		g_autofree gchar *in_recycle = g_build_filename(fx->disk, ".axis-recycle", "films", "Colliery (2004) {tmdb-6004}",
			"Colliery (2004) {tmdb-6004} [HDTV 720p].mkv", NULL);
		AxisImportCleanupReport report;

		axis_test_log_allow(G_LOG_LEVEL_WARNING, "*duplicates: a file was moved into the recycle folder*");
		g_atomic_int_set(&dup_fail_record, 2);
		reply = post_as(fx, fx->admin, colliery_review, "/duplicates/remove", post);
		if (reply.status != 200)
			g_error("expected the page of what was done, got %u:\n%s", reply.status, reply.body);
		assert_contains(reply.body, "moved, but not recorded");
		assert_contains(reply.body, "It was moved into the recycle folder of its disk");
		assert_contains(reply.body, "the library still lists it");
		assert_lacks(reply.body, "not removed, and still linked as it was");
		axis_test_reply_clear(&reply);
		g_assert_cmpint(g_atomic_int_get(&dup_fail_record), ==, 0);
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*duplicates: a file was moved into the recycle "
			"folder*"), ==, 1);
		g_assert_false(g_file_test(kept_path, G_FILE_TEST_EXISTS));
		g_assert_true(g_file_test(in_recycle, G_FILE_TEST_IS_REGULAR));
		g_assert_true(g_file_test(stays_path, G_FILE_TEST_IS_REGULAR));
		assert_qf(fx, "moving|true", "SELECT state || '|' || (import_id IS NULL AND remove_after IS NULL)::text"
			" FROM import_recycle WHERE label = 'Colliery (2004)'");
		assert_qf(fx, "2", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, colliery);
		assert_qf(fx, "true", "SELECT (text LIKE '%%the record of it could not be written%%')::text FROM movie_event"
			" WHERE kind = 'hand_link' AND movie_id = %" G_GINT64_FORMAT, colliery);
		reply = get_as(fx, fx->viewer, "/duplicates/recycled");
		assert_contains(reply.body, "Being recorded");
		axis_test_reply_clear(&reply);

		/* Too young to be taken for a record cut short. */
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_recover_duplicates(fx->import, 300, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.recovered, ==, 0);
		assert_qf(fx, "moving", "SELECT state FROM import_recycle WHERE label = 'Colliery (2004)'");
		/* Old enough: the file is in the recycle folder and not at its
		 * place, so the record is finished. */
		g_assert_true(axis_import_recover_duplicates(fx->import, 0, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.recovered, ==, 1);
		assert_qf(fx, "kept|true|true", "SELECT state || '|' || (remove_after > now() + interval '1 day')::text || '|'"
			" || (recycle_path = '.axis-recycle/films/Colliery (2004) {tmdb-6004}/Colliery (2004) {tmdb-6004}"
			" [HDTV 720p].mkv')::text FROM import_recycle WHERE label = 'Colliery (2004)'");
		assert_qf(fx, "0", "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT, colliery_kept);
		assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, colliery);
		assert_qf(fx, "1", "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND state = 'present'",
			colliery_good);
		g_assert_true(g_file_test(in_recycle, G_FILE_TEST_IS_REGULAR));
		g_assert_true(g_file_test(stays_path, G_FILE_TEST_IS_REGULAR));
		/* Done once: a second look finds nothing left to do. */
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_recover_duplicates(fx->import, 0, &report, &error));
		g_assert_cmpuint(report.recovered, ==, 0);
	}
	/* A row whose file never moved (the daemon died between the row and the
	 * move): the file is still at its place, so the row is forgotten and the
	 * file, its index row and its link are left as they are. */
	{
		g_autofree gchar *row = g_strdup_printf("INSERT INTO import_recycle (volume_id, original_path, inode, size, how,"
			" label) SELECT lf.volume_id, r.rel_path || '/' || lf.rel_path, lf.inode, lf.size, 'moved', 'Thistle (1981)'"
			" FROM library_file lf JOIN library_root r ON r.id = lf.root_id WHERE lf.id = %" G_GINT64_FORMAT, thistle_good);
		g_autofree gchar *place = path_of(fx, thistle_good);
		g_autofree gchar *orphan = g_strdup_printf("INSERT INTO import_recycle (volume_id, original_path, inode, size,"
			" how) SELECT lf.volume_id, r.rel_path || '/' || lf.rel_path, lf.inode, lf.size, 'moved'"
			" FROM library_file lf JOIN library_root r ON r.id = lf.root_id WHERE lf.id = %" G_GINT64_FORMAT, thistle_good);
		AxisImportCleanupReport report;

		/* A row with no import and no label is a deleted import's (R3), not a
		 * duplicate's: the recovery leaves it alone. */
		axis_test_db_exec(fx->t->db, orphan);
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_recover_duplicates(fx->import, 0, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.recovered, ==, 0);
		assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE label IS NULL AND state = 'moving'");
		axis_test_db_exec(fx->t->db, "DELETE FROM import_recycle WHERE label IS NULL AND state = 'moving'");
		axis_test_db_exec(fx->t->db, row);
		assert_qf(fx, "1", "SELECT count(*) FROM import_recycle WHERE label = 'Thistle (1981)' AND state = 'moving'");
		memset(&report, 0, sizeof(report));
		g_assert_true(axis_import_recover_duplicates(fx->import, 0, &report, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(report.recovered, ==, 1);
		assert_qf(fx, "0", "SELECT count(*) FROM import_recycle WHERE label = 'Thistle (1981)'");
		g_assert_true(g_file_test(place, G_FILE_TEST_IS_REGULAR));
		assert_qf(fx, "2", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, thistle);
	}
	/* The history cannot be written at all (L1): the file went all the same,
	 * and the page says so rather than failing as if nothing had happened. */
	{
		g_autofree gchar *rokan_digest = review_digest(fx, rokan);
		g_autofree gchar *rokan_review = g_strdup_printf("/duplicates/review?module=movies&owner=%" G_GINT64_FORMAT
			"&item=0", rokan);
		g_autofree gchar *post = g_strdup_printf("module=movies&owner=%" G_GINT64_FORMAT "&item=0&digest=%s&remove_%"
			G_GINT64_FORMAT "=yes", rokan, rokan_digest, rokan_b);
		g_autofree gchar *gone = path_of(fx, rokan_b);

		axis_test_log_allow(G_LOG_LEVEL_WARNING, "*duplicates: the history of Rokan (1998) could not be written*");
		g_atomic_int_set(&dup_fail_history, 2);
		reply = post_as(fx, fx->admin, rokan_review, "/duplicates/remove", post);
		if (reply.status != 200)
			g_error("expected the page of what was done, got %u:\n%s", reply.status, reply.body);
		assert_contains(reply.body, "history could not be written (the database could not be reached)");
		assert_contains(reply.body, "removed from the library");
		axis_test_reply_clear(&reply);
		g_assert_cmpint(g_atomic_int_get(&dup_fail_history), ==, 0);
		g_assert_false(g_file_test(gone, G_FILE_TEST_EXISTS));
		assert_qf(fx, "1", "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, rokan);
		assert_qf(fx, "0", "SELECT count(*) FROM movie_event WHERE kind = 'hand_link' AND movie_id = %" G_GINT64_FORMAT,
			rokan);
		assert_qf(fx, "kept", "SELECT state FROM import_recycle WHERE label = 'Rokan (1998)'");
		g_assert_cmpuint(axis_test_log_count(G_LOG_LEVEL_WARNING, "*duplicates: the history of Rokan (1998) could not "
			"be written*"), ==, 1);
	}
	axis_import_set_hook(fx->import, NULL, NULL);
}

/* ── Delete this download (add-replace-grab-and-delete-download, B) ── */

#define JUNK_RELEASE "Thistle.1981.1080p.BluRay.x264-MADEUP"
#define JUNK_REF     "SABnzbd_nzo_thistle"
#define JUNK_CLIENT  "/usenet/complete/axis/" JUNK_RELEASE
#define JUNK_REL     "downloads/axis/" JUNK_RELEASE

/* A Usenet job of Thistle that completed in its own folder inside the
 * client's folder for Axis, whose import waits for a person (blocked).  Its
 * folder on the disk in *@dir. */
static gint64
junk_import(
	Fixture  *fx,
	gint64   *movie_out,
	gint64   *download_out,
	gchar   **dir
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *film = NULL;
	g_autofree gchar *nfo = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *download = NULL;
	g_autofree gchar *mapped = NULL;
	GStatBuf st;
	AxisDbConn *conn;
	gint64 movie = add_film(fx, "110");
	gint64 import_id = 0;

	*dir = g_build_filename(fx->downloads, "axis", JUNK_RELEASE, NULL);
	film = g_build_filename(*dir, JUNK_RELEASE ".mkv", NULL);
	nfo = g_build_filename(*dir, JUNK_RELEASE ".nfo", NULL);
	write_media(film, FILM_LINE, 96 * 1024, 3);
	g_assert_true(g_file_set_contents(nfo, "an nfo\n", -1, NULL));
	mapped = q(fx, "SELECT count(*) FROM download_path_map WHERE client_id = %" G_GINT64_FORMAT, fx->client_id);
	if (g_strcmp0(mapped, "0") == 0) {
		g_assert_true(axis_downloads_add_path_map(fx->downloads_layer, fx->client_id, "/usenet/complete", fx->volume_id,
			"downloads", &error));
		g_assert_no_error(error);
	}
	axis_test_download_fake_set_category_dir(fx->fake, "/usenet/complete/axis");
	axis_test_download_fake_keep_completed(fx->fake, TRUE);
	axis_test_download_fake_put(fx->fake, JUNK_REF, JUNK_RELEASE, "axis", NULL, "completed", 1000, 1000, JUNK_CLIENT,
		NULL);
	g_assert_cmpint(g_lstat(*dir, &st), ==, 0);
	title = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, movie);
	download = q(fx, "INSERT INTO download (client_id, client_name, client_ref, protocol, source, release_name,"
		" title_id, title_label, module, state, size_bytes, done_bytes, client_path, volume_id, rel_path,"
		" location_inode, sent_at, completed_at) VALUES (%" G_GINT64_FORMAT ", 'SABnzbd', '" JUNK_REF "', 'usenet',"
		" 'file', '" JUNK_RELEASE "', %s, 'Thistle (1981)', 'movies', 'completed', 1000, 1000, '" JUNK_CLIENT "', %"
		G_GINT64_FORMAT ", '" JUNK_REL "', %" G_GUINT64_FORMAT ", now(), now()) RETURNING id", fx->client_id, title,
		fx->volume_id, (guint64)st.st_ino);
	g_free(q(fx, "INSERT INTO movie_grab (movie_id, release_name, protocol, name_key, download_id, sent_at,"
		" completed_at) VALUES (%" G_GINT64_FORMAT ", '" JUNK_RELEASE "', 'usenet', lower('1:0:" JUNK_RELEASE "'), %s,"
		" now(), now()) RETURNING id", movie, download));
	conn = axis_db_pool_acquire(fx->t->pool, &error);
	g_assert_no_error(error);
	g_assert_true(axis_import_request(fx->import, conn, AXIS_MOVIES_MODULE_ID, g_ascii_strtoll(download, NULL, 10),
		&import_id, NULL, &error));
	g_assert_no_error(error);
	axis_db_pool_release(fx->t->pool, conn);
	g_free(q(fx, "UPDATE import SET state = 'blocked', reason = 'wrong_name', reason_text = 'The file names another"
		" film.' WHERE id = %" G_GINT64_FORMAT " RETURNING id", import_id));
	if (movie_out != NULL)
		*movie_out = movie;
	if (download_out != NULL)
		*download_out = g_ascii_strtoll(download, NULL, 10);
	return import_id;
}

static gchar *
junk_digest(
	Fixture *fx,
	gint64   import_id
){
	g_autoptr(AxisImportDeletePreview) preview = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_import_delete_preview(fx->import, import_id, &preview, &error));
	g_assert_no_error(error);
	return g_strdup(preview->digest);
}

/* Nothing of the junk download changed: the import, the grab, the download,
 * the client and the disk are as they were. */
static void
assert_junk_untouched(
	Fixture     *fx,
	gint64       import_id,
	gint64       download,
	const gchar *dir,
	const gchar *before
){
	g_autofree gchar *after = axis_test_storage_digest(dir);

	g_assert_cmpstr(after, ==, before);
	assert_qf(fx, "blocked", "SELECT state FROM import WHERE id = %" G_GINT64_FORMAT, import_id);
	assert_qf(fx, "completed", "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "1", "SELECT count(*) FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
		download);
	assert_qf(fx, "0", "SELECT count(*) FROM import_action WHERE import_id = %" G_GINT64_FORMAT
		" AND action = 'delete_download'", import_id);
	g_assert_true(axis_test_download_fake_has(fx->fake, JUNK_REF));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
}

/* Spec (import): "Delete this download" on the page of a blocked import: the
 * link for an administrator only; the confirmation writes nothing and says
 * what goes; a viewer, a post with no token and a post from a page that is
 * out of date are refused and change nothing; a rule that refuses shows no
 * button; confirmed, the download is deleted and the import's page says so,
 * with the person's name; the row on the Downloads page links to the
 * confirmation, for an administrator only. */
static void
test_delete_download(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *link = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *stale = NULL;
	gint64 movie = 0;
	gint64 download = 0;
	gint64 id = junk_import(fx, &movie, &download, &dir);
	AxisTestReply reply;

	before = axis_test_storage_digest(dir);
	page = g_strdup_printf("/imports/%" G_GINT64_FORMAT, id);
	confirm = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/delete-download", id);
	link = g_strdup_printf("href=\"%s\"", confirm);

	/* The import's page: the link beside Dismiss, for an administrator. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, link);
	assert_contains(reply.body, "Delete this download");
	assert_contains(reply.body, "Dismiss this import (the download stays where it is)");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "delete-download");
	axis_test_reply_clear(&reply);
	/* The Downloads page: the row's link, for an administrator only. */
	reply = get_as(fx, fx->admin, "/downloads");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, link);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/downloads");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "delete-download");
	axis_test_reply_clear(&reply);

	/* A viewer gets neither the confirmation nor the deletion. */
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	digest = junk_digest(fx, id);
	form = g_strdup_printf("confirm=yes&digest=%s", digest);
	reply = post_as(fx, fx->viewer, "/imports", confirm, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	assert_junk_untouched(fx, id, download, dir, before);

	/* The confirmation: what goes, who deletes, what stays; and nothing
	 * written. */
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_csp_clean(reply.body);
	assert_forms_carry_token(reply.body);
	assert_contains(reply.body, "Delete this download?");
	assert_contains(reply.body, "Thistle (1981)");
	assert_contains(reply.body, "Disk one: " JUNK_REL);
	assert_contains(reply.body, JUNK_CLIENT);
	assert_contains(reply.body, "A folder with everything in it: 2 files");
	assert_contains(reply.body, "The Usenet client removes the job and keeps its folder; Axis deletes the folder.");
	assert_contains(reply.body, "Deleting cannot be undone");
	assert_contains(reply.body, "no search is started now");
	assert_contains(reply.body, "Also put this release on the blocklist, so that no search takes it again");
	assert_contains(reply.body, digest);
	/* The tick is off by default. */
	assert_lacks(reply.body, " checked");
	axis_test_reply_clear(&reply);
	assert_junk_untouched(fx, id, download, dir, before);

	/* A post with no token (forged); one that does not confirm; one whose
	 * digest is not what is so now (a page that is out of date): the page
	 * again, with 409. */
	reply = post_as(fx, fx->admin, NULL, confirm, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, confirm, confirm, "digest=0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, confirm, confirm,
		"confirm=yes&digest=0000000000000000000000000000000000000000000000000000000000000000");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "changed since this page was made");
	assert_contains(reply.body, "Delete this download?");
	assert_contains(reply.body, digest);
	axis_test_reply_clear(&reply);
	assert_junk_untouched(fx, id, download, dir, before);
	/* Something arrived in the folder after the page was made. */
	stale = g_build_filename(dir, "arrived-later.txt", NULL);
	g_assert_true(g_file_set_contents(stale, "later\n", -1, NULL));
	reply = post_as(fx, fx->admin, confirm, confirm, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "A folder with everything in it: 3 files");
	axis_test_reply_clear(&reply);
	g_assert_true(g_file_test(stale, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(g_unlink(stale), ==, 0);
	/* (The test's own file came and went: the folder's time changed.) */
	g_free(before);
	before = axis_test_storage_digest(dir);
	assert_junk_untouched(fx, id, download, dir, before);

	/* A rule refuses (the client keeps Axis's category somewhere else now):
	 * which rule, where the files are, and no button. */
	axis_test_download_fake_set_category_dir(fx->fake, "/usenet/complete/other");
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis does not delete this download");
	assert_contains(reply.body, "which is not inside the folder of");
	assert_contains(reply.body, "The files are at " JUNK_CLIENT " at the client.");
	assert_lacks(reply.body, "name=\"digest\"");
	assert_lacks(reply.body, "button-danger");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, confirm, confirm, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Axis does not delete this download");
	axis_test_reply_clear(&reply);
	assert_junk_untouched(fx, id, download, dir, before);
	axis_test_download_fake_set_category_dir(fx->fake, "/usenet/complete/axis");

	/* A job is placing the import's files: refused, as a dismissal is. */
	g_free(q(fx, "UPDATE import SET state = 'running', reason = NULL WHERE id = %" G_GINT64_FORMAT " RETURNING id", id));
	g_free(q(fx, "UPDATE job SET state = 'running', lease_token = gen_random_uuid(), lease_expires_at = now() +"
		" interval '5 minutes', started_at = now(), attempts = 1 WHERE dedupe_key = 'import:%" G_GINT64_FORMAT "'"
		" RETURNING id", id));
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "A job is placing that import");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, confirm, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_free(q(fx, "UPDATE job SET state = 'queued', lease_token = NULL, lease_expires_at = NULL WHERE dedupe_key ="
		" 'import:%" G_GINT64_FORMAT "' RETURNING id", id));
	g_free(q(fx, "UPDATE import SET state = 'blocked', reason = 'wrong_name' WHERE id = %" G_GINT64_FORMAT
		" RETURNING id", id));
	assert_junk_untouched(fx, id, download, dir, before);

	/* Confirmed: deleted, and the import's page says what happened. */
	reply = post_as(fx, fx->admin, confirm, confirm, form);
	if (reply.status != 303)
		g_error("expected a redirect, got %u:\n%s", reply.status, reply.body);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=download_deleted");
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	g_assert_false(axis_test_download_fake_has(fx->fake, JUNK_REF));
	assert_qf(fx, "dismissed/download_deleted", "SELECT state || '/' || reason FROM import WHERE id = %" G_GINT64_FORMAT,
		id);
	assert_qf(fx, "deleted", "SELECT close_reason FROM movie_grab WHERE download_id = %" G_GINT64_FORMAT, download);
	assert_qf(fx, "0", "SELECT count(*) FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, movie);
	{
		g_autofree gchar *done = g_strdup_printf("/imports/%" G_GINT64_FORMAT "?done=download_deleted", id);

		reply = get_as(fx, fx->admin, done);
	}
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The download was deleted, and the import is closed.");
	assert_contains(reply.body, "Download deleted");
	assert_contains(reply.body, "The download was deleted by root.");
	assert_contains(reply.body, "The job was removed from its client.");
	assert_contains(reply.body, "Its folder at the client (" JUNK_CLIENT ") was deleted");
	assert_contains(reply.body, "Asked to delete the download, on disk and at its client.");
	assert_lacks(reply.body, "/delete-download");
	axis_test_reply_clear(&reply);
	/* Nothing to delete twice. */
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/imports/999999/delete-download");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/* Spec (import): The blocklist tick, posted from the page: the release is on
 * the title's blocklist as blocked by the person who deleted the download. */
static void
test_delete_download_block(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	gint64 movie = 0;
	gint64 download = 0;
	gint64 id = junk_import(fx, &movie, &download, &dir);
	AxisTestReply reply;

	confirm = g_strdup_printf("/imports/%" G_GINT64_FORMAT "/delete-download", id);
	digest = junk_digest(fx, id);
	form = g_strdup_printf("confirm=yes&block=yes&digest=%s", digest);
	reply = post_as(fx, fx->admin, confirm, confirm, form);
	if (reply.status != 303)
		g_error("expected a redirect, got %u:\n%s", reply.status, reply.body);
	axis_test_reply_clear(&reply);
	g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
	assert_qf(fx, "blocked/root", "SELECT reason || '/' || created_by FROM movie_blocklist WHERE movie_id = %"
		G_GINT64_FORMAT " AND release_name = '" JUNK_RELEASE "'", movie);
	assert_qf(fx, "root", "SELECT by_user FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'removal'",
		movie);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
#define ADD(path, func) AXIS_TEST_ADD_DB_CASE(path, Fixture, NULL, func)
	ADD("/web/imports/delete-download/page", test_delete_download);
	ADD("/web/imports/delete-download/block", test_delete_download_block);
	ADD("/web/imports/duplicates", test_duplicates);
	ADD("/web/imports/assign", test_assign);
	ADD("/web/imports/pages", test_pages);
	ADD("/web/imports/actions", test_actions);
	ADD("/web/imports/back-to-place", test_back_to_place);
	ADD("/web/imports/folder", test_the_folder_on_the_page);
	ADD("/web/imports/torrent-gone", test_a_torrent_gone_on_the_page);
	ADD("/web/imports/move", test_a_move_on_the_page);
	ADD("/web/imports/leftovers", test_leftovers_page);
	ADD("/web/imports/leftovers-live", test_leftovers_live);
	ADD("/web/imports/leftovers-unknown", test_leftovers_unknown);
	ADD("/web/imports/leftovers-count", test_leftovers_count);
	ADD("/web/imports/leftovers-other-clients", test_leftovers_other_clients);
	ADD("/web/imports/leftovers-exact-names", test_leftovers_exact_names);
	ADD("/web/imports/leftovers-taken-over", test_leftovers_taken_over);
#undef ADD
	return g_test_run();
}
