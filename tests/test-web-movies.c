/*
 * test-web-movies.c - the Films pages, a film's title page, the dashboard's
 * parts and the administrators' actions, with the real shell on an
 * ephemeral loopback port, the metadata test provider, the fake Prowlarr,
 * the fixture download client, a real database and a real volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spec scenarios: (movies) Add; Why not; Viewer; (web-shell) Films tab;
 * Module off.  Tasks 3.1 to 3.4, and 4.9: non-administrators can look and
 * cannot act; every action is in the film's history with its user.  And of
 * show-library-in-movies: the movies on disk that Axis does not manage in
 * the list, on their pages and the dashboard, managed and removed, with a
 * disk away and a file gone (/web/movies/library), and the whole library at
 * scale, a page's statements bounded (/web/movies/library-at-scale).
 *
 * With AXIS_TEST_MOVIES_PAGES set to a directory, the pages are also saved
 * there (films.html, wanted.html, history.html, title-*.html, dashboard.html),
 * for looking at in a browser (task 4.10).
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
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-prowlarr.h"
#include "axis-test-storage.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "meta/axis-meta.h"
#include "modules/movies/axis-movies.h"
#include "quality/axis-quality-store.h"
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
#include "web/axis-web-ready.h"
#include "web/axis-web-settings.h"
#include "web/axis-web-titles.h"

#define KEY "prowlarr-key-for-film-pages-81e2"
#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD

typedef struct
{
	AxisTestLibrary      *t;
	gchar                *disk;
	gint64                volume_id;
	gint64                root_id;
	AxisDataDir          *data;
	AxisSettings         *settings;
	AxisWebSettings      *web_settings;
	AxisTestMetaFixture  *fa;
	AxisMeta             *meta;
	AxisWebMeta          *meta_ui;
	AxisTestProwlarr     *prowlarr;
	AxisIndexers         *indexers;
	AxisWebIndexers      *indexers_ui;
	AxisTestDownloadFake *fake;
	AxisDownloads        *downloads;
	AxisWebDownloads     *downloads_ui;
	gint64                client_id;
	AxisQualityStore     *quality;
	AxisWebQuality       *quality_ui;
	AxisMovies           *movies;
	AxisWebReady         *ready_ui;
	AxisModuleRegistry   *registry;
	AxisWebHub           *hub;
	AxisWebJobs          *jobs;
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
	AxisIndexerTuning indexer_tuning;
	AxisDownloadTuning download_tuning;
	AxisIndexerSourceSpec source;
	AxisDownloadClientSpec client;

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
	memset(&indexer_tuning, 0, sizeof(indexer_tuning));
	indexer_tuning.request_timeout_ms = 3000;
	indexer_tuning.wait_ms = 5000;
	axis_indexers_set_tuning(fx->indexers, &indexer_tuning);
	fx->indexers_ui = axis_web_indexers_new(fx->indexers);
	fx->downloads = axis_downloads_new(fx->t->pool, fx->t->storage, fx->settings);
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_TORRENT), &error));
	g_assert_true(axis_downloads_add_kind(fx->downloads, axis_test_download_kind(AXIS_DOWNLOAD_USENET), &error));
	memset(&download_tuning, 0, sizeof(download_tuning));
	download_tuning.request_timeout_ms = 1500;
	axis_downloads_set_tuning(fx->downloads, &download_tuning);
	fx->downloads_ui = axis_web_downloads_new(fx->downloads, fx->t->storage, NULL);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);
	fx->movies = axis_movies_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	fx->ready_ui = axis_web_ready_new(fx->meta, fx->indexers, fx->downloads, fx->t->storage);

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
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_ready_get_module(), fx->ready_ui, &error));
	/* The hub and the Activity page (add-activity-feedback). */
	fx->hub = axis_web_hub_new(NULL);
	fx->jobs = axis_web_jobs_new(fx->t->pool, fx->hub);
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_jobs_get_module(), fx->jobs, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));

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
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"101\",\"kind\":\"film\",\"title\":\"The Mazework\","
		"\"year\":1999,\"ids\":{\"imdb\":\"tt0600003\"}},{\"id\":\"102\",\"kind\":\"film\",\"title\":\"The Mazework "
		"Reloaded\",\"year\":2003,\"ids\":{\"imdb\":\"tt0680000\"}}]}");
	set_route(fx, "/title/film/105", "{\"id\":\"105\",\"kind\":\"film\",\"title\":\"A Title To Come\",\"year\":2031,"
		"\"runtime\":100,\"status\":\"upcoming\",\"release_date\":\"2031-06-01\",\"overview\":\"Not out yet.\"}");
	set_route(fx, "/title/film/107", "{\"id\":\"107\",\"kind\":\"film\",\"title\":\"Hearth\",\"year\":1995,"
		"\"runtime\":170,\"status\":\"released\",\"release_date\":\"1995-12-15\",\"ids\":{\"imdb\":\"tt0600001\"},"
		"\"overview\":\"A group of professional bank robbers starts to feel the hearth from police.\"}");
	set_route(fx, "/title/film/108", "{\"id\":\"108\",\"kind\":\"film\",\"title\":\"Rokan\",\"year\":1998,"
		"\"runtime\":122,\"status\":\"released\",\"release_date\":\"1998-09-25\",\"ids\":{\"imdb\":\"tt0600002\"}}");
	set_route(fx, "/title/film/109", "{\"id\":\"109\",\"kind\":\"film\",\"title\":\"Colliery\",\"year\":2004,"
		"\"runtime\":120,\"status\":\"released\",\"release_date\":\"2004-08-06\",\"ids\":{\"imdb\":\"tt0600004\"},"
		"\"overview\":\"A cab driver finds himself the hostage of an engaging contract killer.\"}");
	set_route(fx, "/title/film/110", "{\"id\":\"110\",\"kind\":\"film\",\"title\":\"Thistle\",\"year\":1981,"
		"\"runtime\":123,\"status\":\"released\",\"release_date\":\"1981-03-27\",\"ids\":{\"imdb\":\"tt0600005\"}}");

	fx->prowlarr = axis_test_prowlarr_new(KEY, "");
	base = axis_test_prowlarr_base(fx->prowlarr);
	memset(&source, 0, sizeof(source));
	source.name = "Prowlarr";
	source.kind = AXIS_INDEXER_PROWLARR_NAME;
	source.address = base;
	source.enabled = TRUE;
	source.secret = KEY;
	g_assert_true(axis_indexers_save_source(fx->indexers, 0, &source, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&base, g_free);
	fx->fake = axis_test_download_fake_new(PASSWORD);
	base = axis_test_download_fake_base(fx->fake);
	memset(&client, 0, sizeof(client));
	client.name = "SABnzbd";
	client.kind = "fixturenzb";
	client.address = base;
	client.priority = AXIS_DOWNLOAD_PRIORITY_NORMAL;
	client.enabled = TRUE;
	client.secret = PASSWORD;
	g_assert_true(axis_downloads_save_client(fx->downloads, 0, &client, &fx->client_id, &error));
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
	axis_web_jobs_stop(fx->jobs);
	axis_movies_stop(fx->movies);
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
	axis_web_hub_free(fx->hub);
	axis_web_jobs_free(fx->jobs);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_ready_free(fx->ready_ui);
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
	axis_test_meta_fixture_free(fx->fa);
	axis_test_prowlarr_free(fx->prowlarr);
	axis_test_download_fake_free(fx->fake);
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

	/* (The needle again after the page: a long page pushes the first out of a
	 * terminal's view.) */
	if (plain == NULL || strstr(plain, needle) == NULL)
		g_error("expected to find '%s' in:\n%s\n(expected to find '%s')", needle, haystack != NULL ? haystack : "(null)",
			needle);
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	if (haystack != NULL && strstr(haystack, needle) != NULL)
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

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_MOVIES_PAGES");
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

typedef struct
{
	const gchar *title;
	gint64       size;
	gint64       imdb;
} Rel;

static void
set_releases(
	Fixture   *fx,
	const Rel *rels,
	guint      n
){
	g_autofree gchar *origin = axis_test_prowlarr_base(fx->prowlarr);
	g_autoptr(GString) body = g_string_new("[");
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_printf(body, "%s{\"guid\":\"https://indexer.example/%u\",\"title\":\"%s\",\"size\":%"
			G_GINT64_FORMAT ",\"indexerId\":3,\"indexer\":\"NewsOwl\",\"publishDate\":\"2026-11-01T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=L%u&file=hearth-nzb\",\"magnetUrl\":null,"
			"\"protocol\":\"usenet\",\"imdbId\":%" G_GINT64_FORMAT ",\"categories\":[{\"id\":2040}]}", i > 0 ? "," : "",
			i, rels[i].title, rels[i].size, origin, KEY, i, rels[i].imdb);
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, NULL, body->str);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

static void
search(
	Fixture *fx,
	gint64   movie
){
	AxisMovieSearchReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_movies_run_search(fx->movies, movie, "manual", "root", NULL, &report, &error));
	g_assert_no_error(error);
}

static void
poll(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, fx->client_id, NULL, NULL, &error));
	g_assert_no_error(error);
}

static gchar *
ref_of(
	Fixture *fx,
	gint64   movie
){
	return q(fx, "SELECT d.client_ref FROM movie_grab g JOIN download d ON d.id = g.download_id"
		" WHERE g.movie_id = %" G_GINT64_FORMAT " AND g.closed_at IS NULL", movie);
}

/* Films in every state there is to show: the scenario the pages are shot
 * from. */
typedef struct
{
	gint64 downloading;	/* The Mazework: grabbed, 40 % */
	gint64 attention;	/* The Mazework Reloaded: every release rejected */
	gint64 to_import;	/* Hearth: grabbed and arrived */
	gint64 not_available;	/* A Title To Come */
	gint64 wanted;		/* Thistle: never searched */
	gint64 searching;	/* Rokan: its search queued */
	gint64 unmanaged_title;	/* Colliery's title, not added */
} Scene;

static Scene
scene(Fixture *fx)
{
	static const Rel mazework[] = {
		{ "The.Mazework.1999.1080p.WEB-DL.DDP5.1.H.264-WEB", 6000000000, 600003 },
		{ "The.Mazework.1999.1080p.BluRay.x264-GRP", 12000000000, 600003 },
		{ "The.Mazework.Reloaded.2003.1080p.BluRay.x264-GRP", 12000000000, 680000 },
		{ "The.Mazework.1999.2160p.UHD.BluRay.x265-TRK", 60000000000, 600003 },
		{ "The.Mazework.1999.720p.HDTV.x264-OLD", 2000000000, 600003 },
		{ "The.Mazework.1999.1080p.BluRay.x264-SAMPLE.sample", 40000000, 600003 }
	};
	static const Rel reloaded[] = {
		{ "The.Mazework.Reloaded.2003.2160p.UHD.BluRay.x265-TRK", 70000000000, 680000 },
		{ "The.Mazework.Reloaded.2003.CAM.x264-CAM", 1500000000, 680000 },
		{ "The.Mazework.Reloaded.2003.1080p.BluRay.x264-HUGE", 90000000000, 680000 },
		{ "The.Mazework.Revolutions.2003.1080p.BluRay.x264-GRP", 12000000000, 242653 }
	};
	static const Rel hearth[] = {
		{ "Hearth.1995.1080p.BluRay.x264-GRP", 14000000000, 600001 }
	};
	g_autofree gchar *ref = NULL;
	g_autofree gchar *place = NULL;
	g_autofree gchar *file = NULL;
	g_autoptr(GError) error = NULL;
	Scene s;
	gboolean created;

	memset(&s, 0, sizeof(s));
	s.downloading = add_film(fx, "101");
	set_releases(fx, mazework, G_N_ELEMENTS(mazework));
	search(fx, s.downloading);
	ref = ref_of(fx, s.downloading);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1600000000, NULL, NULL);
	poll(fx);

	s.attention = add_film(fx, "102");
	set_releases(fx, reloaded, G_N_ELEMENTS(reloaded));
	search(fx, s.attention);

	s.to_import = add_film(fx, "107");
	set_releases(fx, hearth, G_N_ELEMENTS(hearth));
	search(fx, s.to_import);
	g_clear_pointer(&ref, g_free);
	ref = ref_of(fx, s.to_import);
	place = g_build_filename(fx->disk, "downloads", "complete", "Hearth.1995.1080p.BluRay.x264-GRP", NULL);
	file = g_build_filename(place, "hearth.mkv", NULL);
	axis_test_storage_mkdir(place);
	axis_test_storage_write(file, "film");
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/downloads", fx->volume_id,
		"downloads", &error));
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000,
		"/data/downloads/complete/Hearth.1995.1080p.BluRay.x264-GRP", NULL);
	poll(fx);
	g_assert_true(axis_movies_run_follow(fx->movies, NULL, &error));

	s.not_available = add_film(fx, "105");
	s.wanted = add_film(fx, "110");
	s.searching = add_film(fx, "108");
	g_assert_true(axis_movies_request_search(fx->movies, s.searching, "manual", "root", &created, &error));
	g_assert_no_error(error);
	s.unmanaged_title = title_of(fx, "109");
	return s;
}

static gint64
title_id_of(
	Fixture *fx,
	gint64   movie
){
	g_autofree gchar *id = q(fx, "SELECT title_id FROM movie WHERE id = %" G_GINT64_FORMAT, movie);

	return g_ascii_strtoll(id, NULL, 10);
}

/* ── The pages ──────────────────────────────────────────────────── */

/* Spec: Add (it appears among the films with its state); Why not; Viewer;
 * Films tab */
static void
test_pages(Fixture *fx, gconstpointer data)
{
	Scene s = scene(fx);
	static const gchar *const marks[] = {
		"film-mark-downloading", "film-mark-attention", "film-mark-to_import", "film-mark-not_available",
		"film-mark-wanted", "film-mark-searching", NULL
	};
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	guint i;

	/* The grid, for a viewer: every film with its mark, nothing to act on. */
	reply = get_as(fx, fx->viewer, "/films");
	g_assert_cmpuint(reply.status, ==, 200);
	for (i = 0; marks[i] != NULL; i++)
		assert_contains(reply.body, marks[i]);
	assert_contains(reply.body, "This list is the whole movie library");
	assert_contains(reply.body, "Removing a movie from Axis never deletes its files.");
	/* (The name filter is a GET form: it changes nothing.) */
	assert_lacks(reply.body, "action=\"/films\" method=\"post\"");
	assert_lacks(reply.body, "action=\"/films/");
	assert_contains(reply.body, "<form class=\"list-filter\" action=\"/films\" method=\"get\" role=\"search\">");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films");
	/* The Media tab between the dashboard and Activity; Movies in its second
	 * row, the films' pages in the third; the heading says Movies. */
	{
		const gchar *dash = strstr(reply.body, ">Dashboard<");
		const gchar *media = strstr(reply.body, ">Media</a>");
		const gchar *activity = strstr(reply.body, ">Activity<");
		const gchar *types = strstr(reply.body, "aria-label=\"Media sections\"");
		const gchar *pages = strstr(reply.body, "aria-label=\"Movies pages\"");

		g_assert_nonnull(dash);
		g_assert_nonnull(media);
		g_assert_nonnull(activity);
		g_assert_true(dash < media && media < activity);
		g_assert_nonnull(types);
		g_assert_nonnull(pages);
		g_assert_true(types < pages);
		assert_contains(types, ">Movies</a>");
		assert_contains(pages, ">All</a>");
		assert_contains(reply.body, "href=\"/films/wanted\"");
		assert_contains(reply.body, "href=\"/films/history\"");
		assert_contains(reply.body, "<h1>All movies</h1>");
		assert_contains(reply.body, "<title>All movies - Axis</title>");
	}
	save_page("films.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films?state=attention&sort=title");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "film-mark-attention");
	assert_lacks(reply.body, "film-mark-wanted\"");
	axis_test_reply_clear(&reply);

	/* Wanted, with the reasons in words. */
	reply = get_as(fx, fx->viewer, "/films/wanted");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Thistle (1981)");
	assert_contains(reply.body, "rejected them all");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), ==, 1);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films/wanted");
	save_page("wanted.html", reply.body);
	axis_test_reply_clear(&reply);

	/* History, across films. */
	reply = get_as(fx, fx->viewer, "/films/history");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Grabbed");
	assert_contains(reply.body, "Downloaded");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), ==, 1);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films/history");
	save_page("history.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A title not added: the form for an administrator, a line for others. */
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, s.unmanaged_title);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "action=\"/films\"");
	assert_contains(reply.body, "name=\"profile\"");
	assert_contains(reply.body, "name=\"root\"");
	assert_contains(reply.body, "name=\"availability\"");
	assert_contains(reply.body, "Add to Movies");
	assert_contains(reply.body, "films on Disk one");
	/* Spec (add-phone-layout): Adding from a phone.  The fields are in a
	 * disclosure whose summary names the profile and destination they hold;
	 * the button comes after it. */
	{
		const gchar *options = strstr(reply.body, "<details class=\"add-options\"><summary>Options<span "
			"class=\"add-options-chosen\">");
		const gchar *button = strstr(reply.body, ">Add to Movies</button>");

		g_assert_nonnull(options);
		g_assert_nonnull(button);
		g_assert_true(options < strstr(reply.body, "name=\"profile\""));
		g_assert_true(strstr(options, "name=\"search\"") < strstr(options, "</details>"));
		g_assert_true(strstr(options, "</details>") < button);
		g_assert_nonnull(g_strstr_len(options, strstr(options, "</summary>") - options, ", films on Disk one"));
	}
	assert_csp_clean(reply.body);
	save_page("title-add.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "Not among your movies. An administrator can add it.");
	assert_lacks(reply.body, "Add to Movies");
	axis_test_reply_clear(&reply);
	g_free(path);

	/* Downloading, with the last search's releases and why. */
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, s.downloading));
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "film-mark-downloading");
	assert_contains(reply.body, "40%");
	assert_contains(reply.body, "Accepted, rank 2");
	assert_contains(reply.body, "Grabbed</span>");
	assert_contains(reply.body, "another movie");
	assert_contains(reply.body, ">Ruled out<");
	assert_contains(reply.body, "hx-get=\"/films/");
	assert_contains(reply.body, "Give up on this download");
	/* (fix-first-torrent) It does not promise that the files go with it:
	 * SABnzbd keeps a finished job's. */
	assert_lacks(reply.body, "remove it from the client with its data");
	assert_contains(reply.body, "Axis deletes where the rules allow");
	/* Grabbing is not offered while one is in flight. */
	assert_lacks(reply.body, ">Grab it anyway</button>");
	assert_lacks(reply.body, ">Grab</button>");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), >=, 2);
	assert_csp_clean(reply.body);
	save_page("title-downloading.html", reply.body);
	axis_test_reply_clear(&reply);
	/* After giving up, the notice says what becomes of the files. */
	{
		g_autofree gchar *given_up = g_strdup_printf("%s?done=given_up", path);

		reply = get_as(fx, fx->admin, given_up);
		assert_contains(reply.body, "SABnzbd keeps the files of a job that finished, and Axis deletes them itself where "
			"the rules allow it. The Downloads page says what became of them.");
		axis_test_reply_clear(&reply);
	}
	g_free(path);

	/* Needs attention: every release rejected, each with its reasons; an
	 * administrator may grab a profile's rejection, never a fixed rule's. */
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, s.attention));
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "film-mark-attention");
	/* Spec (web-shell, tidy-labels): One state word for movies and TV. */
	assert_contains(reply.body, ">Needs attention<");
	assert_lacks(reply.body, ">Attention<");
	assert_contains(reply.body, "rejected them all");
	assert_contains(reply.body, ">Grab it anyway</button>");
	assert_contains(reply.body, "Cam");
	/* Three a profile rejected may be grabbed; another film may not. */
	{
		guint n = 0;
		const gchar *p = reply.body;

		while ((p = strstr(p, ">Grab it anyway</button>")) != NULL) {
			n++;
			p++;
		}
		g_assert_cmpuint(n, ==, 3);
	}
	save_page("title-attention.html", reply.body);
	axis_test_reply_clear(&reply);
	/* Spec: Viewer: the same facts, nothing to do. */
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "rejected them all");
	assert_lacks(reply.body, ">Grab it anyway<");
	assert_lacks(reply.body, "<form class=\"film");
	assert_lacks(reply.body, "Search for this movie");
	assert_lacks(reply.body, "Remove this movie");
	/* (add-activity-feedback) Nothing posts; the state block follows the
	 * film by a GET of its own. */
	assert_lacks(reply.body, "action=\"/films/");
	assert_contains(reply.body, "hx-trigger=\"sse:activity delay:400ms, htmx:sseOpen from:body");
	axis_test_reply_clear(&reply);
	g_free(path);

	/* To import: where the files are, and that they stay there until then. */
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, s.to_import));
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "film-mark-to_import");
	assert_contains(reply.body, "downloads/complete/Hearth.1995.1080p.BluRay.x264-GRP");
	assert_contains(reply.body, "Until the import, the files stay where the client put them.");
	/* And what the import does, as it is (import-by-move): moves them, a
	 * torrent's once its client let go, and deletes what it did not take. */
	assert_contains(reply.body, "The import moves the movie");
	assert_contains(reply.body, "first asked to let go of it, without its files");
	assert_contains(reply.body, "a torrent left seeding is hardlinked instead");
	assert_contains(reply.body, "is deleted by Axis with the download");
	assert_contains(reply.body, "the movie is wanted again");
	save_page("title-to-import.html", reply.body);
	axis_test_reply_clear(&reply);
	g_free(path);

	/* Not available yet. */
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, s.not_available));
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Not searched for before 2031-08-30");
	assert_lacks(reply.body, ">Search for this movie<");
	save_page("title-not-available.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The dashboard's parts. */
	/* (What needs attention is the board's: add-attention-notices, N2.) */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	assert_contains(reply.body, "Movies added lately");
	assert_contains(reply.body, "attention-level\">Waiting for you</span><a href=\"/films/wanted?state=attention\">");
	assert_contains(reply.body, " attention");
	assert_contains(reply.body, " because ");
	save_page("dashboard.html", reply.body);
	axis_test_reply_clear(&reply);

	/* The state block alone, as htmx asks for it. */
	g_free(path);
	path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", s.downloading);
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<div id=\"film-state-"));
	axis_test_reply_clear(&reply);
}

/* ── Who may act ────────────────────────────────────────────────── */

/* Spec: Viewer; and every action recorded with its user (4.9). */
static void
test_actions(Fixture *fx, gconstpointer data)
{
	gint64 title = title_of(fx, "110");
	g_autofree gchar *page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
	g_autofree gchar *form = NULL;
	g_autofree gchar *movie = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, fx->root_id);
	g_autofree gchar *n = NULL;
	AxisTestReply reply;

	form = g_strdup_printf("title=%" G_GINT64_FORMAT "&root=%s&availability=released&monitored=yes", title, root);
	/* A viewer cannot add, whatever the token. */
	reply = post_as(fx, fx->viewer, page, "/films", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* An administrator without the token cannot either. */
	reply = post_as(fx, fx->admin, NULL, "/films", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT count(*) FROM movie");
	g_assert_cmpstr(n, ==, "0");
	/* With it: added, and the page says so. */
	reply = post_as(fx, fx->admin, page, "/films", form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=added");
	axis_test_reply_clear(&reply);
	movie = q(fx, "SELECT id FROM movie");
	g_assert_nonnull(movie);
	{
		g_autofree gchar *location = g_strdup_printf("%s?done=added", page);

		reply = get_as(fx, fx->admin, location);
		assert_contains(reply.body, "The movie was added.");
		assert_contains(reply.body, "film-mark-wanted");
		axis_test_reply_clear(&reply);
	}
	/* Twice: refused, with why. */
	reply = post_as(fx, fx->admin, page, "/films", form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "among your movies already");
	axis_test_reply_clear(&reply);

	/* Search now: a viewer is refused; an administrator queues it. */
	action = g_strdup_printf("/films/%s/search", movie);
	reply = post_as(fx, fx->viewer, page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search' AND state = 'queued'");
	g_assert_cmpstr(n, ==, "1");

	/* Changing it, and grabbing: a viewer is refused. */
	g_free(action);
	action = g_strdup_printf("/films/%s/edit", movie);
	reply = post_as(fx, fx->viewer, page, action, "root=1&availability=announced");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("root=%s&availability=announced", root);
	reply = post_as(fx, fx->admin, page, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(action);
	action = g_strdup_printf("/films/%s/grab", movie);
	reply = post_as(fx, fx->viewer, page, action, "candidate=1");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_free(action);
	action = g_strdup_printf("/films/%s/blocklist/clear", movie);
	reply = post_as(fx, fx->viewer, page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	/* Removing: a viewer is refused; it asks to be confirmed. */
	g_free(action);
	action = g_strdup_printf("/films/%s/remove", movie);
	reply = post_as(fx, fx->viewer, page, action, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "/films?done=removed");
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT count(*) FROM movie");
	g_assert_cmpstr(n, ==, "0");

	/* Every action, with the administrator who took it. */
	g_free(n);
	n = q(fx, "SELECT string_agg(kind || ':' || coalesce(by_user, '-'), ',' ORDER BY id) FROM movie_event");
	g_assert_cmpstr(n, ==, "added:root,searched:root,changed:root,removed:root");
}

/* ── What is needed to download ─────────────────────────────────── */

/* The dashboard as @cookie sees it; the Getting started card's markup, or
 * %NULL when the page has none. */
/* ── What needs attention, and what puts it right ───────────────── */

#define THISTLE_WEBDL "Thistle.1981.1080p.WEB-DL.DDP5.1.H.264-NTb"

static gint64
profile_id(
	Fixture     *fx,
	const gchar *slug
){
	g_autofree gchar *id = q(fx, "SELECT id FROM quality_profile WHERE slug = '%s'", slug);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

static gint64
add_film_as(
	Fixture     *fx,
	const gchar *provider_id,
	const gchar *slug,
	gboolean     monitored
){
	AxisMovieSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.profile_id = profile_id(fx, slug);
	spec.root_id = fx->root_id;
	spec.monitored = monitored;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

/* Whether the profile choice of @id is marked as accepting what was found
 * (the nearest mark before its input). */
static gboolean
choice_accepts(
	const gchar *body,
	gint64       id
){
	g_autofree gchar *input = g_strdup_printf("id=\"profile-choice-%" G_GINT64_FORMAT "\"", id);
	const gchar *at = strstr(body, input);
	const gchar *p;
	const gchar *mark = NULL;

	if (at == NULL)
		g_error("no profile choice %" G_GINT64_FORMAT " in:\n%s", id, body);
	for (p = body; (p = strstr(p, "profile-")) != NULL && p < at; p++) {
		if (g_str_has_prefix(p, "profile-accepts") || g_str_has_prefix(p, "profile-rejects"))
			mark = p;
	}
	g_assert_nonnull(mark);
	return g_str_has_prefix(mark, "profile-accepts");
}

#define LANTERN_LARGE "Paper.Lantern.2019.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define LANTERN_OTHER "Paper.Lantern.2019.1080p.BluRay.x264-GRP"

static guint
count_of(
	const gchar *haystack,
	const gchar *needle
){
	const gchar *p = haystack;
	guint n = 0;

	while (p != NULL && (p = strstr(p, needle)) != NULL) {
		n++;
		p++;
	}
	return n;
}

/* The latest manual search's release named @name. */
static gint64
listed_candidate(
	Fixture     *fx,
	const gchar *name
){
	g_autofree gchar *id = q(fx, "SELECT c.id FROM movie_candidate c JOIN movie_search s ON s.id = c.search_id"
		" WHERE s.cause = 'list' AND c.name = '%s' ORDER BY c.id DESC LIMIT 1", name);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

/* add-manual-search: Manual search of a film, beside Search now, lists
 * everything grouped and grabs nothing, moving no schedule and changing no
 * state, whether the film is wanted now or not; Grab this one takes a
 * release the profile rejects, by hand, after its confirmation; one whose
 * indexer says it is another film is ruled out; a viewer sees the list and
 * no grab. */
static void
test_manual_search(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ LANTERN_LARGE, 4294967296, 9100001 },
		{ LANTERN_OTHER, 9000000000, 9999999 }
	};
	g_autofree gchar *page = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *manual = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *n = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	gboolean created = FALSE;
	gint64 film;
	gint64 later;
	gint64 candidate;

	/* Paper Lantern, out; The Long Lantern, not out yet: made-up films. */
	set_route(fx, "/title/film/151", "{\"id\":\"151\",\"kind\":\"film\",\"title\":\"Paper Lantern\",\"year\":2019,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"2019-04-05\",\"ids\":{\"imdb\":\"tt9100001\"}}");
	set_route(fx, "/title/film/152", "{\"id\":\"152\",\"kind\":\"film\",\"title\":\"The Long Lantern\",\"year\":2031,"
		"\"runtime\":100,\"status\":\"upcoming\",\"release_date\":\"2031-06-01\",\"ids\":{\"imdb\":\"tt9100002\"}}");
	film = add_film_as(fx, "151", "space_saver", TRUE);
	set_releases(fx, found, G_N_ELEMENTS(found));
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual", film);
	manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual?level=movie", film);
	grab = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab", film);

	/* Beside Search now, for an administrator; its page linked for all. */
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, ">Manual search</button>");
	assert_contains(reply.body, "The last manual search");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	assert_lacks(reply.body, ">Manual search</button>");
	axis_test_reply_clear(&reply);

	/* Asked for: its own key, the history; a viewer is refused. */
	reply = post_as(fx, fx->viewer, page, action, "level=movie");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, "level=movie");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "manual?level=movie&done=searching");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT payload->>'cause' FROM job WHERE kind = 'movies.search' AND dedupe_key = 'movies.search:%"
		G_GINT64_FORMAT ":list'", film);
	g_assert_cmpstr(n, ==, "list");
	g_free(n);
	n = q(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'searched' AND text ="
		" 'A manual search asked for by root: it lists what the indexers have and grabs nothing by itself.'", film);
	g_assert_cmpstr(n, ==, "1");
	/* While it runs the film is not searching. */
	reply = get_as(fx, fx->admin, manual);
	/* (add-activity-feedback) Where it stands: queued, waiting its turn. */
	assert_contains(reply.body, "A manual search of the movie is waiting for its turn, queued");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page);
	assert_lacks(reply.body, "film-mark-searching");
	axis_test_reply_clear(&reply);

	/* Its job's work: listed, nothing grabbed, no schedule moved. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	g_free(n);
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_free(n);
	n = q(fx, "SELECT last_search_at IS NULL AND next_search_at IS NULL FROM movie WHERE id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "t");
	/* The film's own state and Releases card are its automatic search's. */
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "film-mark-wanted");
	assert_lacks(reply.body, "film-mark-attention");
	assert_contains(reply.body, "Not searched for yet.");
	axis_test_reply_clear(&reply);

	/* Everything, grouped; one may be grabbed, the other is ruled out. */
	reply = get_as(fx, fx->admin, manual);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Manual search of the movie");
	assert_contains(reply.body, "Rejected by the profile (1)");
	assert_contains(reply.body, "Ruled out (1)");
	assert_contains(reply.body, "another movie than this one");
	g_assert_cmpuint(count_of(reply.body, ">Grab this one</button>"), ==, 1);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, manual);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, LANTERN_LARGE);
	assert_lacks(reply.body, "Grab this one");
	axis_test_reply_clear(&reply);

	/* Grab this one: its sentence, then sent by hand. */
	candidate = listed_candidate(fx, LANTERN_LARGE);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", grab, candidate);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Grab " LANTERN_LARGE " (WEB-DL 1080p) by hand for the movie, with the quality profile "
		"Space saver set aside.");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&sort=quality&dir=asc", candidate);
	reply = post_as(fx, fx->viewer, manual, grab, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=grabbed");
	/* (fix-manual-search-table) The order the page was in comes back. */
	assert_contains(axis_test_reply_header(&reply, "Location"), "manual?level=movie&sort=quality&dir=asc&done=grabbed");
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT manual::text || '|' || grabbed_by || '|' || set_aside FROM movie_grab WHERE movie_id = %"
		G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "true|root|hand");
	g_free(n);
	n = q(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'grabbed' AND text LIKE"
		" 'Grabbed by hand " LANTERN_LARGE "%%the quality profile Space saver set aside%%'", film);
	g_assert_cmpstr(n, ==, "1");

	/* Ruled out: said, never taken. */
	candidate = listed_candidate(fx, LANTERN_OTHER);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", grab, candidate);
	reply = get_as(fx, fx->admin, confirm);
	assert_contains(reply.body, "cannot be grabbed");
	assert_lacks(reply.body, "Grab this one by hand");
	axis_test_reply_clear(&reply);

	/* Not out yet: a manual search all the same. */
	later = add_film_as(fx, "152", "space_saver", TRUE);
	g_assert_true(axis_movies_request_manual_search(fx->movies, later, "root", &created, &error));
	g_assert_no_error(error);
	g_assert_true(created);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, later, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_null(report.skipped);
	g_assert_cmpstr(report.outcome, ==, "listed");
}

/* ── fix-manual-search-table ────────────────────────────────────── */

static gint64 add_kite(Fixture *fx);

#define SORT_ALPHA   "Kite.Sort.Alpha"
#define SORT_BRAVO   "Kite.Sort.Bravo"
#define SORT_CHARLIE "Kite.Sort.Charlie"
#define SORT_DELTA   "Kite.Sort.Delta"
#define SORT_ECHO    "Kite.Sort.Echo"

/* Gives the stored release @name what the sort tests need: SQL for its size,
 * its publication time, then its quality's name and its indexer (SQL), as a
 * release the profile rejects. */
static void
shape(
	Fixture     *fx,
	const gchar *name,
	const gchar *size,
	const gchar *published,
	const gchar *quality,
	const gchar *indexer
){
	g_autofree gchar *sql = g_strdup_printf("UPDATE movie_candidate SET accepted = false, safety = false,"
		" blocklisted = false, reasons = '[]', size_bytes = %s, published_at = %s, quality = '%s', indexer = %s"
		" WHERE name = '%s'", size, published, quality, indexer, name);

	axis_test_db_exec(fx->t->db, sql);
}

/* Whether @names are in @body in this order, each after the one before. */
static void
assert_in_order(
	const gchar        *body,
	const gchar *const *names
){
	g_autofree gchar *plain = axis_test_web_unbroken(body);
	const gchar *from = plain;
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		const gchar *at = strstr(from, names[i]);

		if (at == NULL)
			g_error("expected %s after %s in the order given:\n%s", names[i], i > 0 ? names[i - 1] : "(start)", plain);
		from = at + strlen(names[i]);
	}
}

/* The page at @path, for an administrator, its releases in this order. */
static void
assert_page_order(
	Fixture            *fx,
	const gchar        *path,
	const gchar *const *names
){
	AxisTestReply reply = get_as(fx, fx->admin, path);

	g_assert_cmpuint(reply.status, ==, 200);
	assert_in_order(reply.body, names);
	axis_test_reply_clear(&reply);
}

/* How many times the pattern is in @body (a regular expression). */
static guint
matches_of(
	const gchar *body,
	const gchar *pattern
){
	g_autoptr(GRegex) re = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	guint n = 0;

	g_regex_match(re, body, 0, &info);
	while (g_match_info_matches(info)) {
		n++;
		g_match_info_next(info, NULL);
	}
	return n;
}

/* A link of the film's manual search page: "<page><tail>". */
static void
assert_href(
	const gchar *body,
	gint64       film,
	const gchar *tail
){
	g_autofree gchar *href = g_strdup_printf("href=\"/films/%" G_GINT64_FORMAT "/manual?level=movie%s\"", film, tail);
	g_autoptr(GString) escaped = g_string_new(href);

	/* In markup, an ampersand of an address is written as an entity. */
	g_string_replace(escaped, "&", "&amp;", 0);
	assert_contains(body, escaped->str);
}

/* The details element of a group is open when its tag has "open". */
static gboolean
group_open(
	const gchar *body,
	const gchar *id
){
	g_autofree gchar *needle = g_strdup_printf("<details id=\"%s\"", id);
	const gchar *at = strstr(body, needle);
	const gchar *end = at != NULL ? strchr(at, '>') : NULL;

	g_assert_nonnull(at);
	g_assert_nonnull(end);
	return g_strstr_len(at, end - at, "open=\"open\"") != NULL;
}

/*
 * fix-manual-search-table.  The Size heading and its cells agree (the
 * results card swapped in while a search runs too); each heading sorts both
 * tables by the column, toggling, by the server, from a fixed list, unknown
 * values last either way; the order rides with the page's forms; the ruled
 * out group is a disclosure, open when short, anchored into or just sorted,
 * and its rows that can be grabbed by force come first.
 */
static void
test_manual_sort(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ SORT_ALPHA, 3221225472, 9200001 },
		{ SORT_BRAVO, 2147483648, 9200001 },
		{ SORT_CHARLIE, 1073741824, 9200001 },
		{ SORT_DELTA, 2147483648, 9200001 },
		{ SORT_ECHO, 4294967296, 9200001 }
	};
	static const gchar *const by_size_desc[] = { SORT_ECHO, SORT_ALPHA, SORT_DELTA, SORT_CHARLIE, SORT_BRAVO, NULL };
	static const gchar *const by_size_asc[] = { SORT_CHARLIE, SORT_DELTA, SORT_ALPHA, SORT_ECHO, SORT_BRAVO, NULL };
	static const gchar *const by_age_asc[] = { SORT_CHARLIE, SORT_ALPHA, SORT_DELTA, SORT_BRAVO, SORT_ECHO, NULL };
	static const gchar *const by_age_desc[] = { SORT_DELTA, SORT_ALPHA, SORT_CHARLIE, SORT_BRAVO, SORT_ECHO, NULL };
	static const gchar *const by_quality_desc[] = { SORT_CHARLIE, SORT_ALPHA, SORT_BRAVO, SORT_DELTA, SORT_ECHO, NULL };
	static const gchar *const by_quality_asc[] = { SORT_DELTA, SORT_BRAVO, SORT_ALPHA, SORT_CHARLIE, SORT_ECHO, NULL };
	static const gchar *const by_found_asc[] = { SORT_BRAVO, SORT_ALPHA, SORT_DELTA, SORT_ECHO, SORT_CHARLIE, NULL };
	static const gchar *const by_found_desc[] = { SORT_ECHO, SORT_DELTA, SORT_ALPHA, SORT_BRAVO, SORT_CHARLIE, NULL };
	static const gchar *const by_name_asc[] = { SORT_ALPHA, SORT_BRAVO, SORT_CHARLIE, SORT_DELTA, SORT_ECHO, NULL };
	static const gchar *const by_name_desc[] = { SORT_ECHO, SORT_DELTA, SORT_CHARLIE, SORT_BRAVO, SORT_ALPHA, NULL };
	static const gchar *const forced_first[] = { SORT_ECHO, SORT_ALPHA, SORT_BRAVO, SORT_CHARLIE, SORT_DELTA, NULL };
	g_autofree gchar *manual = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *form = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	gint64 film;
	gint64 candidate;

	film = add_kite(fx);
	set_releases(fx, found, G_N_ELEMENTS(found));
	manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual?level=movie", film);
	grab = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab", film);
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual", film);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	/* Searched long ago, so that asking again is offered. */
	axis_test_db_exec(fx->t->db, "UPDATE movie_search SET started_at = now() - interval '2 hours', finished_at ="
		" now() - interval '1 hour'");
	/* Sizes 3, 2, 1, 2, 4 GiB (Bravo's not known), published 5 days, never, 1
	 * day, 30 days and never ago; qualities, from the lowest of the ladder,
	 * dvd, hdtv_720p, webdl_720p, webdl_1080p, bluray_1080p (Echo's not one);
	 * indexers Beacon, Anchor, none, Cairn, Ember. */
	shape(fx, SORT_ALPHA, "3221225472", "now() - interval '5 days'", "webdl_1080p", "'Beacon'");
	shape(fx, SORT_BRAVO, "NULL", "NULL", "hdtv_720p", "'Anchor'");
	shape(fx, SORT_CHARLIE, "1073741824", "now() - interval '1 day'", "bluray_1080p", "NULL");
	shape(fx, SORT_DELTA, "2147483648", "now() - interval '30 days'", "dvd", "'Cairn'");
	shape(fx, SORT_ECHO, "4294967296", "NULL", "unknown", "'Ember'");

	/* Scenario: the Size heading and its cells agree, on the page and in the
	 * results card that is swapped in while a search runs. */
	reply = get_as(fx, fx->admin, manual);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(matches_of(reply.body, "<th[^>]*col-size"), ==, 1);
	g_assert_cmpuint(matches_of(reply.body, "<td[^>]*col-size"), ==, 5);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	path = g_strdup_printf("%s&part=results", manual);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(matches_of(reply.body, "<th[^>]*col-size"), ==, 1);
	g_assert_cmpuint(matches_of(reply.body, "<td[^>]*col-size"), ==, 5);
	g_assert_cmpuint(matches_of(reply.body, "<td[^>]*data-label=\"Size\""), ==, 5);
	axis_test_reply_clear(&reply);

	/* Scenario: each order, the unknown last whichever way. */
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size&dir=desc", manual)), by_size_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size&dir=asc", manual)), by_size_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=age&dir=asc", manual)), by_age_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=age&dir=desc", manual)), by_age_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=quality&dir=desc", manual)), by_quality_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=quality&dir=asc", manual)), by_quality_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=indexer&dir=asc", manual)), by_found_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=indexer&dir=desc", manual)), by_found_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=release&dir=asc", manual)), by_name_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=release&dir=desc", manual)), by_name_desc);
	/* Without a direction, the column's own: the biggest first. */
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size", manual)), by_size_desc);

	/* Scenario: the toggle.  The sorted heading says how, and its link is
	 * the other way; an unsorted heading's is its first way. */
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=desc", manual);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "aria-sort=\"descending\"");
	g_assert_cmpuint(count_of(reply.body, "aria-sort="), ==, 1);
	assert_href(reply.body, film, "&sort=size&dir=asc&at=manual-group-1#manual-group-1");
	assert_href(reply.body, film, "&sort=quality&dir=desc&at=manual-group-1#manual-group-1");
	assert_href(reply.body, film, "&sort=age&dir=asc&at=manual-group-1#manual-group-1");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc", manual);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "aria-sort=\"ascending\"");
	assert_href(reply.body, film, "&sort=size&dir=desc&at=manual-group-1#manual-group-1");
	axis_test_reply_clear(&reply);

	/* Scenario: a value not in the fixed list is left out: the default order,
	 * with no heading marked. */
	g_free(path);
	path = g_strdup_printf("%s&sort=bogus&dir=desc", manual);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_in_order(reply.body, by_name_asc);
	assert_lacks(reply.body, "aria-sort=");
	assert_lacks(reply.body, "bogus");
	axis_test_reply_clear(&reply);
	/* A direction not in it: the column's own. */
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size&dir=sideways", manual)), by_size_desc);

	/* Scenario: the order rides with the forms and the way back. */
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc", manual);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "name=\"sort\"");
	assert_contains(reply.body, "value=\"asc\"");
	/* (add-block-release: each row's Block this release carries it too.) */
	g_assert_cmpuint(count_of(reply.body, "name=\"sort\""), ==, 1 + 5 + 5);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("level=movie&sort=size&dir=asc");
	reply = post_as(fx, fx->admin, path, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "manual?level=movie&sort=size&dir=asc&done=");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("level=movie&sort=nonsense&dir=asc");
	reply = post_as(fx, fx->admin, path, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "manual?level=movie&done=");
	assert_lacks(axis_test_reply_header(&reply, "Location"), "sort=");
	axis_test_reply_clear(&reply);
	/* The confirmation of a grab: its form and its way back. */
	candidate = listed_candidate(fx, SORT_ALPHA);
	g_free(path);
	path = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie&sort=size&dir=asc", grab, candidate);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "name=\"sort\"");
	assert_href(reply.body, film, "&sort=size&dir=asc");
	axis_test_reply_clear(&reply);

	/* Scenario: the ruled out group is a disclosure.  Short, it is open;
	 * what can be grabbed by force comes first; every sort is the same list. */
	axis_test_db_exec(fx->t->db, "UPDATE movie_candidate SET safety = true, reasons = '[{\"k\": \"safety\", \"c\":"
		" \"password\", \"t\": \"It is password-protected.\"}]'::jsonb");
	axis_test_db_exec(fx->t->db, "UPDATE movie_candidate SET reasons = '[{\"k\": \"safety\", \"c\": \"ambiguous\","
		" \"t\": \"It could be more than one title.\"}]'::jsonb WHERE name = '" SORT_ECHO "'");
	reply = get_as(fx, fx->admin, manual);
	assert_contains(reply.body, "<summary class=\"manual-fold\">Ruled out (5)</summary>");
	g_assert_true(group_open(reply.body, "manual-group-3"));
	assert_in_order(reply.body, forced_first);
	g_assert_cmpuint(count_of(reply.body, ">Grab it by force</button>"), ==, 1);
	axis_test_reply_clear(&reply);
	/* Many, it is closed, unless the address says it is the place, or it was
	 * just sorted (the heading's link says where it lands). */
	axis_test_db_exec(fx->t->db, "INSERT INTO movie_candidate (search_id, position, release_id, source_id, name,"
		" protocol, name_key, quality, match, accepted, safety, reasons) SELECT search_id, 100 + g, release_id,"
		" source_id, 'Kite.Sort.Extra.' || g, protocol, name_key || g, 'unknown', 'none', false, true,"
		" '[{\"k\": \"safety\", \"c\": \"password\", \"t\": \"It is password-protected.\"}]'::jsonb FROM"
		" movie_candidate, generate_series(1, 16) g WHERE name = '" SORT_ALPHA "'");
	reply = get_as(fx, fx->admin, manual);
	assert_contains(reply.body, "<summary class=\"manual-fold\">Ruled out (21)</summary>");
	g_assert_false(group_open(reply.body, "manual-group-3"));
	assert_in_order(reply.body, forced_first);
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc&at=manual-group-3", manual);
	reply = get_as(fx, fx->admin, path);
	g_assert_true(group_open(reply.body, "manual-group-3"));
	assert_contains(reply.body, "aria-sort=\"ascending\"");
	assert_href(reply.body, film, "&sort=size&dir=desc&at=manual-group-3#manual-group-3");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc", manual);
	reply = get_as(fx, fx->admin, path);
	g_assert_false(group_open(reply.body, "manual-group-3"));
	axis_test_reply_clear(&reply);
	/* A shorter one is open again. */
	axis_test_db_exec(fx->t->db, "DELETE FROM movie_candidate WHERE name LIKE 'Kite.Sort.Extra.%' AND position > 105");
	reply = get_as(fx, fx->admin, manual);
	assert_contains(reply.body, "<summary class=\"manual-fold\">Ruled out (10)</summary>");
	g_assert_true(group_open(reply.body, "manual-group-3"));
	axis_test_reply_clear(&reply);
}

/* ── add-force-grab-and-assign, slice F1 ────────────────────────── */

#define KITE_FORCED  "Paper.Kite.2019.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define KITE_PROGRAM "Paper.Kite.2019.1080p.WEB-DL.x264-GRP.exe"
#define KITE_LOCKED  "Paper.Kite.2019.720p.WEB-DL.x264-GRP"

/* The value of the hidden field "digest" of a force confirmation. */
static gchar *
digest_of(const gchar *body)
{
	const gchar *field = body != NULL ? strstr(body, "name=\"digest\"") : NULL;
	const gchar *start;
	const gchar *end;
	const gchar *value;

	g_assert_nonnull(field);
	start = g_strrstr_len(body, field - body, "<input");
	end = strchr(field, '>');
	g_assert_nonnull(start);
	g_assert_nonnull(end);
	value = g_strstr_len(start, end - start, "value=\"");
	g_assert_nonnull(value);
	value += strlen("value=\"");
	return g_strndup(value, strcspn(value, "\""));
}

/* Paper Kite (2019), managed, and another film of the same name and year in
 * the store: a release named for it cannot be told apart by its name (the
 * matcher takes the one title of the release's own year, so two of the same
 * year stay ambiguous).  Made-up films. */
static gint64
add_kite(Fixture *fx)
{
	set_route(fx, "/title/film/161", "{\"id\":\"161\",\"kind\":\"film\",\"title\":\"Paper Kite\",\"year\":2019,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"2019-04-05\",\"ids\":{\"imdb\":\"tt9200001\"}}");
	set_route(fx, "/title/film/162", "{\"id\":\"162\",\"kind\":\"film\",\"title\":\"Paper Kite\",\"year\":2019,"
		"\"runtime\":95,\"status\":\"released\",\"release_date\":\"2019-10-11\",\"ids\":{\"imdb\":\"tt9200002\"}}");
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"161\",\"kind\":\"film\",\"title\":\"Paper Kite\","
		"\"year\":2019,\"ids\":{\"imdb\":\"tt9200001\"}},{\"id\":\"162\",\"kind\":\"film\",\"title\":\"Paper Kite\","
		"\"year\":2019,\"ids\":{\"imdb\":\"tt9200002\"}}]}");
	return add_film_as(fx, "161", "space_saver", TRUE);
}

/* A video file of the film in its library folder, linked to it. */
static gint64
add_kite_file(
	Fixture     *fx,
	gint64       film,
	const gchar *quality
){
	g_autofree gchar *id = q(fx, "WITH f AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size,"
		" mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads,"
		" first_seen_generation, last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", 'Paper Kite (2019)/Paper Kite (2019).mkv', 'video', 2147483648, 1, 0, 1, 0, 4100001, 1,"
		" false, false, false, 1, 1, 1) RETURNING id) INSERT INTO movie_file (file_id, movie_id, quality)"
		" SELECT f.id, %" G_GINT64_FORMAT ", '%s' FROM f RETURNING file_id", fx->root_id, fx->volume_id, film,
		quality);

	g_assert_nonnull(id);
	return g_ascii_strtoll(id, NULL, 10);
}

/*
 * add-force-grab-and-assign, F1.  Spec (movies): "A release a rule about its
 * name refuses can be grabbed by force for this movie", scenarios "Several
 * stored titles share the name", "A file would be replaced", "The page is
 * out of date", "Password-protected", "A viewer"; and "An administrator can
 * grab any listed release of this film by hand", scenario "A fixed rule,
 * through the ordinary form".
 */
static void
test_force_grab(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ KITE_FORCED, 4294967296, 9200001 },
		{ KITE_PROGRAM, 3000000000, 9200001 },
		{ KITE_LOCKED, 2000000000, 9200001 }
	};
	g_autofree gchar *manual = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *force = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *tick = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	gint64 film;
	gint64 candidate;
	gint64 locked;
	gint64 program;
	gint64 file;

	film = add_kite(fx);
	set_releases(fx, found, G_N_ELEMENTS(found));
	manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual?level=movie", film);
	grab = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab", film);
	force = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/force", film);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	candidate = listed_candidate(fx, KITE_FORCED);
	locked = listed_candidate(fx, KITE_LOCKED);
	program = listed_candidate(fx, KITE_PROGRAM);
	/* The matcher refuses it for the reason met in use: more than one
	 * stored title fits its name. */
	n = q(fx, "SELECT (safety AND NOT accepted)::text || '|' || (SELECT string_agg(r->>'c', ',') FROM"
		" jsonb_array_elements(reasons) r WHERE r->>'k' = 'safety') FROM movie_candidate WHERE id = %" G_GINT64_FORMAT,
		candidate);
	g_assert_cmpstr(n, ==, "true|ambiguous");
	g_clear_pointer(&n, g_free);
	/* One of them is password-protected as well (no release name says so, so
	 * the stored rule is written here as the quality layer writes it). */
	axis_test_db_exec(fx->t->db, "UPDATE movie_candidate SET reasons = reasons || '[{\"k\": \"safety\", \"c\":"
		" \"password\", \"t\": \"It is password-protected.\"}]'::jsonb WHERE name = '" KITE_LOCKED "'");

	/* The list: all three ruled out; only the one whose every rule is about
	 * its name is offered Grab it by force, and only to an administrator. */
	reply = get_as(fx, fx->admin, manual);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Ruled out (3)");
	assert_contains(reply.body, "it could be more than one title");
	assert_contains(reply.body, "Axis cannot tell that it is this movie. An administrator who can tell may grab it by "
		"force.");
	assert_contains(reply.body, "Ruled out by a fixed rule: no one can grab it.");
	g_assert_cmpuint(count_of(reply.body, ">Grab it by force</button>"), ==, 1);
	g_assert_cmpuint(count_of(reply.body, ">Grab this one</button>"), ==, 0);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	/* Scenario: A viewer. */
	reply = get_as(fx, fx->viewer, manual);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, KITE_FORCED);
	assert_lacks(reply.body, "Grab it by force</button>");
	axis_test_reply_clear(&reply);

	/* Scenario: A fixed rule, through the ordinary form.  Its confirmation
	 * says where to go; a post to it is refused and nothing is sent. */
	other = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", grab, candidate);
	reply = get_as(fx, fx->admin, other);
	assert_contains(reply.body, "cannot be grabbed");
	assert_contains(reply.body, "may grab it by force");
	assert_lacks(reply.body, "Grab this one by hand");
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&downgrade=yes", candidate);
	reply = post_as(fx, fx->admin, manual, grab, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "fixed safety rule rejects this release");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_clear_pointer(&n, g_free);

	/* The force confirmation: the rule in its words, what it is bound to,
	 * what its import does, what it replaces (nothing yet). */
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", force, candidate);
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Grab this release by force?");
	assert_contains(reply.body, "Axis rules this release out: it could be more than one title");
	assert_contains(reply.body, "2 titles fit");
	assert_contains(reply.body, "It is grabbed for the movie Paper Kite (2019), and for nothing else.");
	assert_contains(reply.body, "Its video file is imported as Paper Kite (2019) without judging its name again.");
	assert_contains(reply.body, "The quality profile Space saver is set aside.");
	assert_contains(reply.body, "The movie has no file in the library: nothing is replaced.");
	assert_contains(reply.body, "I have checked that this release is Paper Kite (2019)");
	assert_contains(reply.body, ">Grab it by force</button>");
	assert_csp_clean(reply.body);
	digest = digest_of(reply.body);
	g_assert_cmpuint(strlen(digest), ==, 32);
	axis_test_reply_clear(&reply);

	/* Scenario: A viewer (the post).  And without the tick that says the
	 * person checked the release, nothing is sent. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s", candidate,
		digest);
	reply = post_as(fx, fx->viewer, manual, force, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&digest=%s", candidate, digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "you have checked that this release is what you say it is");
	axis_test_reply_clear(&reply);

	/* Scenario: Password-protected.  No page offers it, and a post to force
	 * it is refused; the same for a name that ends in a program's extension. */
	g_free(other);
	other = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", force, locked);
	reply = get_as(fx, fx->admin, other);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "cannot be grabbed by force");
	assert_contains(reply.body, "A fixed rule that protects the system rules it out, and no one may grab it, by force "
		"either: It is password-protected.");
	assert_lacks(reply.body, ">Grab it by force</button>");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s", locked,
		digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "cannot be grabbed by force");
	assert_contains(reply.body, "password-protected");
	axis_test_reply_clear(&reply);
	g_free(other);
	other = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", force, program);
	reply = get_as(fx, fx->admin, other);
	assert_contains(reply.body, "cannot be grabbed by force");
	assert_contains(reply.body, "extension (.exe and the like).");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_clear_pointer(&n, g_free);

	/* Review finding 3.  The digest covers the movie's title and quality
	 * profile: given another profile after the confirmation was shown, the
	 * tick "I have checked that this release is X" does not apply. */
	axis_test_db_exec(fx->t->db, "UPDATE movie SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug ="
		" 'balanced')");
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s", candidate,
		digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "What this page showed changed after you opened it");
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->t->db, "UPDATE movie SET quality_profile_id = (SELECT id FROM quality_profile WHERE slug ="
		" 'space_saver')");
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_clear_pointer(&n, g_free);

	/* Scenario: The page is out of date.  A file of the movie arrives after
	 * the confirmation was shown: the post sends nothing, and the page is
	 * shown again with the new file. */
	file = add_kite_file(fx, film, "webdl_720p");
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s", candidate,
		digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "What this page showed changed after you opened it");
	assert_contains(reply.body, "Nothing was sent.");
	assert_contains(reply.body, "Replace the file Paper Kite (2019).mkv (WEB-DL 720p) with this release: this release "
		"is better");
	assert_lacks(reply.body, "nothing is replaced");
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_clear_pointer(&n, g_free);
	/* The page as it is now carries a digest of its own. */
	{
		g_autofree gchar *now = digest_of(reply.body);

		g_assert_cmpstr(now, !=, digest);
		g_free(digest);
		digest = g_steal_pointer(&now);
	}
	axis_test_reply_clear(&reply);

	/* Scenario: A file would be replaced.  Although the release is better,
	 * the file needs its own tick: without it nothing is sent. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s", candidate,
		digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "t would replace the file Paper Kite (2019).mkv in the library, which was not ticked: "
		"tick the box to replace it. Nothing was sent.");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_clear_pointer(&n, g_free);

	/* Scenario: Several stored titles share the name.  Confirmed, checked
	 * and the file ticked: sent for this movie, the rule recorded as set
	 * aside, the grab bound, the file recorded as the one to replace. */
	tick = g_strdup_printf("%s&replace_%" G_GINT64_FORMAT "=yes", form, file);
	reply = post_as(fx, fx->admin, confirm, force, tick);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=grabbed");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT manual::text || '|' || grabbed_by || '|' || set_aside || '|' || bound::text || '|' ||"
		" jsonb_array_length(forced) || '|' || (forced->0->>'c') || '|' || ((replaces_files->0->>'f')::bigint = %"
		G_GINT64_FORMAT ")::text || '|' || (replaces_files->0->>'q') FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT,
		file, film);
	g_assert_cmpstr(n, ==, "true|root|hand|true|1|ambiguous|true|webdl_720p");
	g_clear_pointer(&n, g_free);
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE forced->0->>'t' LIKE '%%could be more than one title%%'");
	g_assert_cmpstr(n, ==, "1");
	g_clear_pointer(&n, g_free);
	n = q(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'grabbed' AND by_user ="
		" 'root' AND text LIKE 'Grabbed by force " KITE_FORCED "%%Chosen by root by force from a manual search, for"
		" this movie and nothing else. Set aside:%%could be more than one title%%The quality profile Space saver is"
		" set aside too. Its import does not judge the name again, and replaces the file ticked.'", film);
	g_assert_cmpstr(n, ==, "1");
	g_clear_pointer(&n, g_free);
	/* In flight now: a second force of it is refused, and says so (the grab
	 * in flight is of this very release, so no replace is offered). */
	reply = get_as(fx, fx->admin, confirm);
	assert_contains(reply.body, "cannot be grabbed by force");
	assert_contains(reply.body, "This very release is the one in flight");
	assert_lacks(reply.body, "Replace the download in flight with this one");
	axis_test_reply_clear(&reply);
}

/*
 * add-force-grab-and-assign, F1.  Spec (movies): "A movie's file kept by
 * hand is not upgraded by itself", scenarios "Below the cutoff" and "Let
 * go".  A file below the cutoff is wanted for an upgrade; kept by hand, the
 * scheduled search and Search now grab nothing and the page says who keeps
 * it; let go by an administrator, it is wanted for an upgrade again.
 */
static void
test_kept_by_hand(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ "Paper.Kite.2019.1080p.WEB-DL.x265-GRP", 1500000000, 9200001 }
	};
	g_autofree gchar *page = NULL;
	g_autofree gchar *keep = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *n = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	gint64 film;
	gint64 file;

	/* One film of the name here: a release of it is matched and acceptable. */
	set_route(fx, "/title/film/161", "{\"id\":\"161\",\"kind\":\"film\",\"title\":\"Paper Kite\",\"year\":2019,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"2019-04-05\",\"ids\":{\"imdb\":\"tt9200001\"}}");
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"161\",\"kind\":\"film\",\"title\":\"Paper Kite\","
		"\"year\":2019,\"ids\":{\"imdb\":\"tt9200001\"}}]}");
	film = add_film_as(fx, "161", "space_saver", TRUE);
	file = add_kite_file(fx, film, "webdl_720p");
	set_releases(fx, found, G_N_ELEMENTS(found));
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	keep = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/keep", film);

	/* Below the cutoff, by the ordinary rules: an upgrade is wanted. */
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "film-mark-upgrade_wanted");
	assert_lacks(reply.body, "Kept by hand");
	axis_test_reply_clear(&reply);

	/* The file came from a grab by force by root: kept by hand. */
	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET hand_how = 'forced', hand_by = 'root', hand_at = now(),"
		" kept = true");
	reply = get_as(fx, fx->admin, page);
	assert_lacks(reply.body, "film-mark-upgrade_wanted");
	assert_contains(reply.body, "kept by hand by root: it is not upgraded by itself");
	assert_contains(reply.body, "From a release grabbed by force by root.");
	assert_contains(reply.body, "Kept by hand: no automatic search and no Search now replaces it.");
	assert_contains(reply.body, ">Let Axis upgrade it again</button>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	assert_contains(reply.body, "Kept by hand: no automatic search and no Search now replaces it.");
	assert_lacks(reply.body, ">Let Axis upgrade it again</button>");
	axis_test_reply_clear(&reply);

	/* Scenario: Below the cutoff.  The scheduled search, and Search now's,
	 * grab nothing, though a better release is there. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.skipped, ==, "its file is kept by hand");
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "manual", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.skipped, ==, "its file is kept by hand");
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");
	g_clear_pointer(&n, g_free);

	/* Scenario: Let go.  An administrator's (a viewer is refused); then the
	 * ordinary rules want the upgrade, and the search grabs it. */
	form = g_strdup_printf("file=%" G_GINT64_FORMAT "&keep=no", file);
	reply = post_as(fx, fx->viewer, page, keep, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, keep, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=let_go");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT kept::text || '|' || hand_how || '|' || hand_by FROM movie_file WHERE file_id = %" G_GINT64_FORMAT,
		file);
	g_assert_cmpstr(n, ==, "false|forced|root");
	g_clear_pointer(&n, g_free);
	n = q(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'hand_link' AND by_user"
		" = 'root' AND text LIKE '%%is no longer kept by hand, let go by root%%'", film);
	g_assert_cmpstr(n, ==, "1");
	g_clear_pointer(&n, g_free);
	{
		/* Where the post came back to: the notice is at the files. */
		g_autofree gchar *back = g_strdup_printf("%s?done=let_go&at=files", page);

		reply = get_as(fx, fx->admin, back);
	}
	assert_contains(reply.body, "film-mark-upgrade_wanted");
	assert_contains(reply.body, "The file is no longer kept by hand");
	assert_contains(reply.body, ">Keep it by hand</button>");
	axis_test_reply_clear(&reply);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	/* Searched for again, and the better release is grabbed now. */
	g_assert_null(report.skipped);
	g_assert_cmpstr(report.outcome, ==, "grabbed");

	/* A file that is not this movie's: refused, nothing changed. */
	g_free(form);
	form = g_strdup_printf("file=%" G_GINT64_FORMAT "&keep=yes", file + 1000);
	reply = post_as(fx, fx->admin, page, keep, form);
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/*
 * Review finding 1 of add-force-grab-and-assign.  A release the profile
 * rejected ("Grab it anyway"), or one it accepts ("Grab"), does not replace
 * a file kept by hand by one click: the buttons are not offered (the row
 * names Manual search), and a post to the grab route is refused with a plain
 * sentence and sends nothing.
 */
static void
test_kept_grab_anyway(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ "Paper.Kite.2019.2160p.WEB-DL.x265-BIG", 4294967296, 9200001 }
	};
	g_autofree gchar *page = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *n = NULL;
	AxisTestReply reply;
	gint64 film;
	gint64 candidate;

	set_route(fx, "/title/film/161", "{\"id\":\"161\",\"kind\":\"film\",\"title\":\"Paper Kite\",\"year\":2019,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"2019-04-05\",\"ids\":{\"imdb\":\"tt9200001\"}}");
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"161\",\"kind\":\"film\",\"title\":\"Paper Kite\","
		"\"year\":2019,\"ids\":{\"imdb\":\"tt9200001\"}}]}");
	film = add_film_as(fx, "161", "space_saver", TRUE);
	set_releases(fx, found, G_N_ELEMENTS(found));
	search(fx, film);
	add_kite_file(fx, film, "webdl_720p");
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/grab", film);
	n = q(fx, "SELECT c.id FROM movie_candidate c JOIN movie_search s ON s.id = c.search_id WHERE s.movie_id = %"
		G_GINT64_FORMAT " ORDER BY c.id DESC LIMIT 1", film);
	candidate = g_ascii_strtoll(n, NULL, 10);
	g_clear_pointer(&n, g_free);

	/* Not kept: the button is there. */
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, ">Grab it anyway</button>");
	axis_test_reply_clear(&reply);

	axis_test_db_exec(fx->t->db, "UPDATE movie_file SET hand_how = 'forced', hand_by = 'root', hand_at = now(),"
		" kept = true");
	reply = get_as(fx, fx->admin, page);
	assert_lacks(reply.body, ">Grab it anyway</button>");
	assert_contains(reply.body, "kept by hand");
	axis_test_reply_clear(&reply);

	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT, candidate);
	reply = post_as(fx, fx->admin, page, action, form);
	g_assert_cmpuint(reply.status, !=, 500);
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, "0");

	/* (add-force-grab-and-assign, F2) "Grab this one" of that better release
	 * asks for the tick that names the kept file on its confirmation, instead
	 * of the import refusing it later: without it nothing is sent; with it
	 * the tick is recorded for that very file. */
	{
		g_autofree gchar *manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab", film);
		g_autofree gchar *confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", manual, candidate);
		g_autofree gchar *file = q(fx, "SELECT file_id FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);
		g_autofree gchar *box = g_strdup_printf("name=\"replace_%s\"", file);

		reply = get_as(fx, fx->admin, confirm);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, box);
		assert_contains(reply.body, "which is kept by hand by root");
		assert_lacks(reply.body, "name=\"downgrade\"");
		axis_test_reply_clear(&reply);
		g_free(form);
		form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes", candidate);
		reply = post_as(fx, fx->admin, confirm, manual, form);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "kept by hand by root: tick the box that names it");
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
		g_assert_cmpstr(n, ==, "0");
		g_free(form);
		form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&replace_%s=yes", candidate, file);
		reply = post_as(fx, fx->admin, confirm, manual, form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT (replaces_files->0->>'f') || '|' || set_aside || '|' || bound::text FROM movie_grab"
			" WHERE movie_id = %" G_GINT64_FORMAT, film);
		{
			g_autofree gchar *expected = g_strdup_printf("%s|hand|false", file);

			g_assert_cmpstr(n, ==, expected);
		}
	}
}

/* fix-attention-actionable: a film that needs attention says why and links
 * to what puts it right on its page: Grab it anyway, another profile marked
 * by what it accepts (the search's own decision), the profile's page;
 * choosing one searches again and the release is accepted; a viewer reads
 * the words and has no action; a new profile searches again, a new
 * destination alone does not, nor a film that is not monitored. */
static void
test_attention(Fixture *fx, gconstpointer data)
{
	static const Rel thistle[] = { { THISTLE_WEBDL, 4294967296, 600005 } };
	g_autofree gchar *page = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *resolve = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	gint64 film;
	gint64 balanced = profile_id(fx, "balanced");
	gint64 saver = profile_id(fx, "space_saver");

	film = add_film_as(fx, "110", "space_saver", TRUE);
	set_releases(fx, thistle, G_N_ELEMENTS(thistle));
	search(fx, film);
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	resolve = g_strdup_printf("href=\"%s#resolve\"", page);

	/* Rule 1 and 2: why, and next to it what puts it right. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "film-mark-attention");
	assert_contains(reply.body, "1 release was found and the profile Space saver rejected it: too large (1).");
	assert_contains(reply.body, "href=\"#resolve\"");
	assert_contains(reply.body, "See what was found and what you can do");
	assert_contains(reply.body, "id=\"resolve\"");
	assert_contains(reply.body, "What you can do");
	assert_contains(reply.body, "is above the");
	assert_contains(reply.body, "limit for a 123-minute movie at WEB-DL 1080p");
	assert_contains(reply.body, ">Grab it anyway</button>");
	assert_contains(reply.body, "Use another quality profile for this movie");
	assert_contains(reply.body, ">Use this profile and search again</button>");
	assert_contains(reply.body, "Change the limits of Space saver itself");
	g_assert_false(choice_accepts(reply.body, saver));
	g_assert_true(choice_accepts(reply.body, balanced));
	g_assert_false(choice_accepts(reply.body, profile_id(fx, "uhd")));
	/* The settings card, named for what it holds, its remove last. */
	assert_contains(reply.body, "id=\"settings\"");
	assert_contains(reply.body, "Settings for this movie");
	assert_contains(reply.body, ">Remove this movie<");
	assert_lacks(reply.body, ">Options<");
	assert_csp_clean(reply.body);
	save_page("title-attention-resolve.html", reply.body);
	axis_test_reply_clear(&reply);
	/* The list's reason is a link there. */
	reply = get_as(fx, fx->admin, "/films?view=overview");
	assert_contains(reply.body, resolve);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/films/wanted?state=attention");
	assert_contains(reply.body, resolve);
	axis_test_reply_clear(&reply);
	/* Rule 3: a viewer reads why, and has nothing to press. */
	reply = get_as(fx, fx->viewer, page);
	assert_contains(reply.body, "too large (1)");
	assert_contains(reply.body, "An administrator can grab a release anyway");
	assert_lacks(reply.body, "profile-choice");
	assert_lacks(reply.body, ">Grab it anyway<");
	assert_lacks(reply.body, "Remove this movie");
	axis_test_reply_clear(&reply);

	/* Choosing Balanced: by the edit's own rules, and searched again. */
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/edit", film);
	form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT "&availability=released&monitored=yes",
		balanced, fx->root_id);
	reply = post_as(fx, fx->viewer, page, action, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=profile");
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT payload->>'cause' FROM job WHERE kind = 'movies.search' AND state = 'queued'");
	g_assert_cmpstr(n, ==, "profile");
	g_free(n);
	n = q(fx, "SELECT text FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'changed'", film);
	assert_contains(n, "Changed by root: the quality profile to Balanced. It is searched for again now with the new "
		"profile.");
	{
		g_autofree gchar *location = g_strdup_printf("%s?done=profile", page);

		reply = get_as(fx, fx->admin, location);
		assert_contains(reply.body, "The quality profile is Balanced now, and the movie is searched for again with it");
		axis_test_reply_clear(&reply);
	}
	/* The search the change queued (its job's work): accepted, grabbed. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "profile", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	g_free(n);
	n = q(fx, "SELECT cause || ',' || outcome FROM movie_search WHERE movie_id = %" G_GINT64_FORMAT
		" ORDER BY id DESC LIMIT 1", film);
	g_assert_cmpstr(n, ==, "profile,grabbed");
	g_free(n);
	n = q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, THISTLE_WEBDL);
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "after its quality profile changed");
	assert_lacks(reply.body, "What you can do");
	axis_test_reply_clear(&reply);

	/* A new destination alone searches for nothing. */
	{
		gint64 other = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films-two", AXIS_MEDIA_MOVIES);

		g_free(form);
		form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT "&availability=released"
			"&monitored=yes", balanced, other);
		reply = post_as(fx, fx->admin, page, action, form);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "?done=saved");
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'");
		g_assert_cmpstr(n, ==, "0");
	}

	/* A film that is not monitored: a new profile searches for nothing. */
	{
		gint64 colliery = add_film_as(fx, "109", "space_saver", FALSE);
		g_autofree gchar *other_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, colliery));
		g_autofree gchar *other_edit = g_strdup_printf("/films/%" G_GINT64_FORMAT "/edit", colliery);

		g_free(form);
		form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT "&availability=released",
			balanced, fx->root_id);
		reply = post_as(fx, fx->admin, other_page, other_edit, form);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "?done=profile_unmonitored");
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'");
		g_assert_cmpstr(n, ==, "0");
		g_free(n);
		n = q(fx, "SELECT text FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'changed'",
			colliery);
		assert_contains(n, "Nothing is searched for now: it is not monitored.");
		{
			g_autofree gchar *location = g_strdup_printf("%s?done=profile_unmonitored", other_page);

			reply = get_as(fx, fx->admin, location);
			assert_contains(reply.body, "The movie is not monitored, so nothing was searched for");
			axis_test_reply_clear(&reply);
		}
	}
}

static gchar *
setup_card(
	Fixture     *fx,
	const gchar *cookie
){
	AxisTestReply board = get_as(fx, cookie, "/");
	const gchar *start;
	const gchar *end;
	gchar *card = NULL;

	g_assert_cmpuint(board.status, ==, 200);
	start = strstr(board.body, "<section id=\"dash-setup\"");
	if (start != NULL) {
		end = strstr(start, "</section>");
		g_assert_nonnull(end);
		card = g_strndup(start, (gsize)(end - start));
		/* First, before every other card. */
		g_assert_true(strstr(board.body, "<section id=\"dash-") == start);
	}
	save_page("dashboard-setup.html", board.body);
	axis_test_reply_clear(&board);
	return card;
}

/* The card lists @item as needed, with its sentence, and every other item
 * asked about as done. */
static void
assert_only_needed(
	const gchar *card,
	const gchar *label,
	const gchar *sentence
){
	g_autofree gchar *needed = g_strdup_printf("<span class=\"badge badge-warn\">Needed</span><span "
		"class=\"ready-what\"><span class=\"ready-label\">%s</span>", label);
	const gchar *first;

	g_assert_nonnull(card);
	assert_contains(card, needed);
	assert_contains(card, sentence);
	first = strstr(card, "ready-needed");
	g_assert_nonnull(first);
	g_assert_null(strstr(first + 1, "ready-needed"));
}

/*
 * Spec (web-shell): What is needed to download is shown until it is all
 * there.  Each missing item alone, everything there (no card), links for
 * administrators only, and a film added with no indexer source and no
 * download client says so at the top of its page; the Add form says so
 * under its button.
 */
static void
test_getting_started(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *card = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *client = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *add_page = NULL;
	AxisTestReply reply;
	gint64 film;

	/* Everything but imports that may write: storage is read-only. */
	card = setup_card(fx, fx->admin);
	g_assert_nonnull(card);
	assert_contains(card, "<h2>Still needed before Axis can download</h2>");
	assert_only_needed(card, "Axis may write to the library", "Storage is in read-only mode");
	assert_contains(card, "<a class=\"ready-link\" href=\"/admin/settings#module-storage\">");
	assert_contains(card, "Prowlarr searches for releases.");
	assert_contains(card, "SABnzbd takes Usenet releases. No client takes torrents, so they are passed over.");
	assert_contains(card, "films, on Disk one, takes new movies.");
	/* Only films: this daemon has no series. */
	assert_lacks(card, "series");
	g_clear_pointer(&card, g_free);
	/* Someone else reads the same, without the links. */
	card = setup_card(fx, fx->viewer);
	assert_only_needed(card, "Axis may write to the library", "Storage is in read-only mode");
	assert_lacks(card, "ready-link");
	assert_lacks(card, "href=\"/admin");
	assert_contains(card, "An administrator can set these up.");
	g_clear_pointer(&card, g_free);

	/* All there: no card. */
	g_assert_true(axis_settings_set(fx->t->settings, AXIS_STORAGE_SETTING_READ_ONLY, "false", &error));
	g_assert_no_error(error);
	card = setup_card(fx, fx->admin);
	g_assert_null(card);

	/* Each missing alone. */
	g_assert_true(axis_settings_clear(fx->settings, "metadata.fixture_key", &error));
	g_assert_no_error(error);
	card = setup_card(fx, fx->admin);
	assert_only_needed(card, "A TMDb token", "No TMDb token is stored");
	/* Spec (web-shell, tidy-labels): One word for the TMDb credential. */
	assert_lacks(card, "metadata key");
	assert_contains(card, "<a class=\"ready-link\" href=\"/admin/settings#module-metadata\">Add a TMDb token in Settings</a>");
	g_clear_pointer(&card, g_free);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));

	axis_test_db_exec(fx->t->db, "UPDATE indexer_source SET enabled = false");
	card = setup_card(fx, fx->admin);
	assert_only_needed(card, "An indexer", "The indexer is switched off");
	assert_contains(card, "<a class=\"ready-link\" href=\"/indexers\">Set up an indexer</a>");
	g_clear_pointer(&card, g_free);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_source SET state = 'refused', enabled = true");
	card = setup_card(fx, fx->admin);
	assert_only_needed(card, "An indexer", "Prowlarr refused its key");
	g_clear_pointer(&card, g_free);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_source SET state = 'ok'");

	axis_test_db_exec(fx->t->db, "UPDATE download_client SET enabled = false");
	card = setup_card(fx, fx->admin);
	assert_only_needed(card, "A download client", "The download client is switched off");
	assert_contains(card, "<a class=\"ready-link\" href=\"/downloads/clients\">Set up a download client</a>");
	g_clear_pointer(&card, g_free);
	axis_test_db_exec(fx->t->db, "UPDATE download_client SET enabled = true");

	g_assert_true(axis_storage_remove_root(fx->t->storage, fx->root_id, &error));
	g_assert_no_error(error);
	card = setup_card(fx, fx->admin);
	assert_only_needed(card, "A library folder for Movies", "No library folder for movies is set up");
	assert_contains(card, "<a class=\"ready-link\" href=\"/storage\">Add a library folder for Movies</a>");
	g_clear_pointer(&card, g_free);
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	card = setup_card(fx, fx->admin);
	g_assert_null(card);

	/* Spec: Nothing configured.  A film added with no indexer source and no
	 * download client: its page says so first, with the links for an
	 * administrator and none for anyone else. */
	source = q(fx, "SELECT id FROM indexer_source");
	client = q(fx, "SELECT id FROM download_client");
	g_assert_true(axis_indexers_delete_source(fx->indexers, g_ascii_strtoll(source, NULL, 10), &error));
	g_assert_true(axis_downloads_delete_client(fx->downloads, g_ascii_strtoll(client, NULL, 10), &error));
	g_assert_no_error(error);
	add_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_of(fx, "109"));
	reply = get_as(fx, fx->admin, add_page);
	/* The Add form still adds, and says what will not happen. */
	assert_contains(reply.body, "class=\"ready-note\"");
	assert_contains(reply.body, "You can add it, but nothing can search for or download a movie yet: no indexer "
		"is set up and no download client is set up.");
	g_assert_true(strstr(reply.body, "Add to Movies") < strstr(reply.body, "ready-note"));
	save_page("title-add-not-ready.html", reply.body);
	axis_test_reply_clear(&reply);

	film = add_film(fx, "110");
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<div id=\"ready-notice\" class=\"notice notice-warning ready-notice\" role=\"status\">");
	assert_contains(reply.body, "Nothing can search for or download this movie yet: no indexer is set up and "
		"no download client is set up.");
	g_assert_true(strstr(reply.body, "id=\"ready-notice\"") < strstr(reply.body, "title-hero"));
	assert_contains(reply.body, "<a href=\"/indexers\">Set up an indexer</a>");
	assert_contains(reply.body, "<a href=\"/downloads/clients\">Set up a download client</a>");
	assert_csp_clean(reply.body);
	save_page("title-not-ready.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	assert_contains(reply.body, "Nothing can search for or download this movie yet: no indexer is set up and "
		"no download client is set up.");
	assert_contains(reply.body, "An administrator can set this up.");
	assert_lacks(reply.body, "Set up an indexer");
	axis_test_reply_clear(&reply);
	card = setup_card(fx, fx->admin);
	g_assert_nonnull(card);
	assert_contains(card, "No indexer is set up");
	assert_contains(card, "No download client is set up");
}

/* ── 4.1, 4.2: the list at scale ────────────────────────────────── */

#define SCALE_FILMS 5000

/*
 * SCALE_FILMS films made by SQL, film i in a state its facts give it: a
 * release date to come (i % 20 == 0: not available), a file in the library
 * (i % 10 == 5: below the profile's cutoff when i % 20 == 5, else above
 * it), a last search that needs attention (i % 10 == 7), a search queued
 * (i % 10 == 3), and wanted otherwise.  Added one second apart, film 1 the
 * latest.  Every 500th is "Aurélie" and every 250th has the original title
 * "Le Film Généré": names with accents for the filter.  Posters from six
 * kept images.
 */
static void
make_films(Fixture *fx)
{
	g_autofree gchar *profile = q(fx, "SELECT id FROM quality_profile WHERE slug = 'balanced'");
	g_autofree gchar *art = g_build_filename(fx->t->dir, "data", "artwork", NULL);
	guint k;

	g_assert_nonnull(profile);
	for (k = 0; k < 6; k++) {
		g_autofree gchar *hash = g_strdup_printf("%064u", k + 1);
		g_autofree gchar *file = g_build_filename(art, hash, NULL);

		g_assert_true(g_file_set_contents(file, "poster", -1, NULL));
		g_free(q(fx, "INSERT INTO meta_artwork (provider, source_ref, size, sha256, bytes, content_type, width,"
			" height) VALUES ('fixture', '/gen%u.png', 'poster-small', '%s', 6, 'image/png', 200, 300) RETURNING 1",
			k, hash));
	}
	g_free(q(fx, "WITH t AS (INSERT INTO meta_title (kind, title, original_title, year, status, release_date,"
		" poster_ref, provider, provider_id) SELECT 'film',"
		" CASE WHEN i %% 500 = 0 THEN 'Aurélie ' || i ELSE 'Generated Film ' || i END,"
		" CASE WHEN i %% 250 = 0 THEN 'Le Film Généré ' || i END, 1950 + i %% 75, 'released',"
		" CASE WHEN i %% 20 = 0 THEN DATE '2099-01-01' ELSE DATE '2000-01-01' + i %% 7000 END,"
		" '/gen' || (i %% 6) || '.png', 'fixture', 'gen' || i FROM generate_series(1, %d) i RETURNING id, provider_id)"
		" INSERT INTO movie (title_id, quality_profile_id, root_id, added_at) SELECT t.id, %s, %" G_GINT64_FORMAT ","
		" now() - make_interval(secs => substr(t.provider_id, 4)::int) FROM t RETURNING 1", SCALE_FILMS, profile,
		fx->root_id));
	axis_test_db_exec(fx->t->db, "CREATE TEMP TABLE gen AS SELECT m.id AS movie_id, substr(t.provider_id, 4)::int AS i"
		" FROM movie m JOIN meta_title t ON t.id = m.title_id WHERE t.provider_id LIKE 'gen%'");
	g_free(q(fx, "WITH f AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec,"
		" mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads,"
		" first_seen_generation, last_seen_generation, changed_generation) SELECT %" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ", 'gen/' || i || '.mkv', 'video', i::bigint * 1048576, 1, 0, 1, 0, 1000000 + i, 1, false,"
		" false, false, 1, 1, 1 FROM gen WHERE i %% 10 = 5 RETURNING id, rel_path)"
		" INSERT INTO movie_file (file_id, movie_id, quality) SELECT f.id, g.movie_id,"
		" CASE WHEN g.i %% 20 = 5 THEN 'webdl_720p' ELSE 'remux_2160p' END"
		" FROM f JOIN gen g ON f.rel_path = 'gen/' || g.i || '.mkv' RETURNING 1", fx->root_id, fx->volume_id));
	axis_test_db_exec(fx->t->db, "INSERT INTO movie_search (movie_id, cause, finished_at, outcome, outcome_text)"
		" SELECT movie_id, 'scheduled', now(), 'no_indexer', 'No indexer source is set up.' FROM gen WHERE i % 10 = 7");
	axis_test_db_exec(fx->t->db, "INSERT INTO job (kind, queue, max_attempts, dedupe_key) SELECT 'movies.search',"
		" 'downloads', 2, 'movies.search:' || movie_id FROM gen WHERE i % 10 = 3");
	axis_test_db_exec(fx->t->db, "ANALYZE");
}

/* Every film's state as axis_movie_derive() has it (the test's own reading
 * of every film's facts), by film id. */
static GHashTable *
derived_states(
	Fixture *fx,
	guint    counts[AXIS_MOVIE_N_STATES]
){
	GHashTable *states = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GPtrArray) films = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieRules rules;
	guint i;

	memset(counts, 0, sizeof(guint) * AXIS_MOVIE_N_STATES);
	g_assert_true(axis_movies_list(fx->movies, &films, &error));
	g_assert_no_error(error);
	axis_movies_get_rules(fx->movies, &rules);
	for (i = 0; i < films->len; i++) {
		const AxisMovieFacts *f = g_ptr_array_index(films, i);
		AxisMovieState state = axis_movie_derive(f, &rules, NULL);
		gint64 *id = g_new(gint64, 1);

		*id = f->id;
		g_hash_table_insert(states, id, GINT_TO_POINTER(state));
		counts[state]++;
	}
	return states;
}

/* The page of @query, checked: no more facts read than the page shows. */
static void
page_of(
	Fixture             *fx,
	AxisMoviesListQuery *query,
	AxisMoviesListPage  *page
){
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_movies_list_page(fx->movies, query, page, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(page->facts_read, <=, query->limit);
	g_assert_cmpuint(page->films->len, ==, page->states->len);
}

/* Whether @a comes before @b in @sort (and is not the same film). */
static gboolean
in_order(
	AxisMoviesSort        sort,
	const AxisMovieFacts *a,
	const AxisMovieFacts *b
){
	g_autofree gchar *ta = g_utf8_strdown(a->title, -1);
	g_autofree gchar *tb = g_utf8_strdown(b->title, -1);

	switch (sort) {
	case AXIS_MOVIES_SORT_ADDED:
		return a->added_us > b->added_us || (a->added_us == b->added_us && a->id > b->id);
	case AXIS_MOVIES_SORT_SIZE:
		return a->size_bytes > b->size_bytes || (a->size_bytes == b->size_bytes && a->id > b->id);
	case AXIS_MOVIES_SORT_YEAR:
		if (a->year != b->year)
			return a->year > b->year;
		return strcmp(ta, tb) <= 0;
	default:
		if (strcmp(ta, tb) != 0)
			return strcmp(ta, tb) < 0;
		return a->year <= b->year;
	}
}

/* Every film of the list in @sort, a page of @limit at a time forwards, then
 * back again from the last page; checks the order, that no film is listed
 * twice or missed, and the number of pages.  Returns the films' ids in
 * order. */
static GArray *
walk(
	Fixture        *fx,
	AxisMoviesSort  sort,
	guint           limit,
	gboolean        and_back
){
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	GArray *order = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *cursor = NULL;
	g_autoptr(GPtrArray) kept = g_ptr_array_new_with_free_func((GDestroyNotify)axis_movie_facts_free);
	guint pages = 0;
	guint i;

	for (;;) {
		AxisMoviesListQuery query;
		AxisMoviesListPage page;

		memset(&query, 0, sizeof(query));
		query.sort = sort;
		query.limit = limit;
		query.after = cursor;
		page_of(fx, &query, &page);
		pages++;
		g_assert_cmpuint(page.matching, ==, SCALE_FILMS);
		if (pages > 1)
			g_assert_nonnull(page.previous);
		else
			g_assert_null(page.previous);
		for (i = 0; i < page.films->len; i++) {
			AxisMovieFacts *f = g_ptr_array_index(page.films, i);
			gint64 *key = g_new(gint64, 1);

			*key = f->id;
			if (g_hash_table_contains(seen, key))
				g_error("%s: film %" G_GINT64_FORMAT " listed twice", axis_movies_sort_name(sort), f->id);
			g_hash_table_add(seen, key);
			g_array_append_val(order, f->id);
			if (kept->len > 0 && !in_order(sort, g_ptr_array_index(kept, kept->len - 1), f))
				g_error("%s: film %" G_GINT64_FORMAT " out of order", axis_movies_sort_name(sort), f->id);
		}
		/* The last of this page is the one the next must follow. */
		g_ptr_array_set_size(kept, 0);
		if (page.films->len > 0)
			g_ptr_array_add(kept, g_ptr_array_steal_index(page.films, page.films->len - 1));
		g_free(cursor);
		cursor = g_strdup(page.next);
		if (page.next == NULL)
			g_assert_cmpuint(page.films->len, >, 0);
		axis_movies_list_page_clear(&page);
		if (cursor == NULL)
			break;
		g_assert_cmpuint(pages, <, SCALE_FILMS / limit + 2);
	}
	g_assert_cmpuint(order->len, ==, SCALE_FILMS);
	g_assert_cmpuint(pages, ==, (SCALE_FILMS + limit - 1) / limit);

	/* And back, a page before at a time, from the last page. */
	if (and_back) {
		guint at = order->len;
		AxisMoviesListQuery query;
		AxisMoviesListPage page;
		g_autofree gchar *before = NULL;

		/* The last page, as its address would be reached: by its first
		 * film's previous. */
		memset(&query, 0, sizeof(query));
		query.sort = sort;
		query.limit = limit;
		{
			/* Walk to the last page again, keeping its previous. */
			g_autofree gchar *after = NULL;

			for (;;) {
				query.after = after;
				page_of(fx, &query, &page);
				if (page.next == NULL) {
					before = g_strdup(page.previous);
					at -= page.films->len;
					axis_movies_list_page_clear(&page);
					break;
				}
				g_free(after);
				after = g_strdup(page.next);
				axis_movies_list_page_clear(&page);
			}
		}
		query.after = NULL;
		while (before != NULL) {
			query.before = before;
			page_of(fx, &query, &page);
			g_assert_cmpuint(page.films->len, ==, MIN(limit, at));
			for (i = 0; i < page.films->len; i++) {
				const AxisMovieFacts *f = g_ptr_array_index(page.films, i);

				g_assert_cmpint(f->id, ==, g_array_index(order, gint64, at - page.films->len + i));
			}
			at -= page.films->len;
			g_free(before);
			before = g_strdup(page.previous);
			/* The page before a page always has a next: the page it is
			 * before. */
			g_assert_nonnull(page.next);
			axis_movies_list_page_clear(&page);
		}
		g_assert_cmpuint(at, ==, 0);
	}
	return order;
}

/* The rows the plan read from @relation: every scan of it, its rows and
 * those its filter removed, times its loops. */
static gdouble
rows_read(
	JsonObject  *node,
	const gchar *relation
){
	gdouble rows = 0;

	if (g_strcmp0(json_object_get_string_member_with_default(node, "Relation Name", NULL), relation) == 0) {
		gdouble loops = json_object_get_double_member_with_default(node, "Actual Loops", 1);

		rows += (json_object_get_double_member_with_default(node, "Actual Rows", 0) +
			json_object_get_double_member_with_default(node, "Rows Removed by Filter", 0)) * loops;
	}
	if (json_object_has_member(node, "Plans")) {
		JsonArray *plans = json_object_get_array_member(node, "Plans");
		guint i;

		for (i = 0; i < json_array_get_length(plans); i++)
			rows += rows_read(json_array_get_object_element(plans, i), relation);
	}
	return rows;
}

static void
plan_reads(
	Fixture             *fx,
	AxisMoviesListQuery *query,
	gdouble             *films,
	gdouble             *titles
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *plan = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *top;

	g_assert_true(axis_movies_list_explain(fx->movies, query, &plan, &error));
	g_assert_no_error(error);
	g_assert_true(json_parser_load_from_data(parser, plan, -1, NULL));
	top = json_object_get_object_member(json_array_get_object_element(json_node_get_array(
		json_parser_get_root(parser)), 0), "Plan");
	*films = rows_read(top, "movie");
	*titles = rows_read(top, "meta_title");
}

/*
 * Spec (web-shell): Thousands of films; Name filter.  Spec (movies): A page.
 * 5,000 films page, filter by name and by state, and sort, every film once
 * and in order; the counts are those axis_movie_derive() gives every film;
 * a page reads the facts of its own films and no others, and (in an order
 * an index serves) its ids read no more than a page of films (the plan says
 * so).  Then (4.2) each view shows the same films in the same order, and an
 * account's view is kept per list.
 */
static void
test_list_at_scale(Fixture *fx, gconstpointer data)
{
	g_autoptr(GHashTable) truth = NULL;
	guint expected[AXIS_MOVIE_N_STATES];
	AxisMoviesListQuery query;
	AxisMoviesListPage page;
	gint64 start;
	guint i;
	guint s;

	make_films(fx);
	truth = derived_states(fx, expected);
	g_assert_cmpuint(g_hash_table_size(truth), ==, SCALE_FILMS);
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++)
		g_test_message("%s: %u", axis_movie_state_name(i), expected[i]);
	g_assert_cmpuint(expected[AXIS_MOVIE_NOT_AVAILABLE], ==, SCALE_FILMS / 20);
	g_assert_cmpuint(expected[AXIS_MOVIE_IN_LIBRARY], ==, SCALE_FILMS / 20);
	g_assert_cmpuint(expected[AXIS_MOVIE_UPGRADE_WANTED], ==, SCALE_FILMS / 20);
	g_assert_cmpuint(expected[AXIS_MOVIE_ATTENTION], ==, SCALE_FILMS / 10);
	g_assert_cmpuint(expected[AXIS_MOVIE_SEARCHING], ==, SCALE_FILMS / 10);

	/* add-attention-notices, task 3.3a: the count the page "Needs attention"
	 * makes of the movies (one statement that loads no film), measured here
	 * where the films are many.  It agrees with the list's own count, and
	 * each of five runs ends within the reading's deadline (the function
	 * fails past it); the times are told, not judged further: the server is
	 * a throwaway one and the program runs under the sanitizers. */
	{
		gint64 took[5];
		guint run;

		for (run = 0; run < G_N_ELEMENTS(took); run++) {
			g_autoptr(GError) error = NULL;
			guint64 reasons[AXIS_MOVIES_ATTENTION_KINDS];
			gint64 highest[AXIS_MOVIES_ATTENTION_KINDS];
			guint64 all = 0;

			start = g_get_monotonic_time();
			g_assert_true(axis_movies_count_attention(fx->movies, AXIS_ATTENTION_DEADLINE_MS, reasons, highest, &error));
			g_assert_no_error(error);
			took[run] = g_get_monotonic_time() - start;
			for (i = AXIS_ACQUIRE_ATTENTION_NONE + 1; i < G_N_ELEMENTS(reasons); i++)
				all += reasons[i];
			g_assert_cmpuint(all, ==, expected[AXIS_MOVIE_ATTENTION]);
		}
		g_test_message("attention count of %d films (3.3a), five runs: %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT " ms", SCALE_FILMS, took[0] / 1000,
			took[1] / 1000, took[2] / 1000, took[3] / 1000, took[4] / 1000);
	}

	/* The first page: the counts are every film's, from one grouped
	 * statement; the films are the 60 latest. */
	memset(&query, 0, sizeof(query));
	query.limit = 60;
	start = g_get_monotonic_time();
	page_of(fx, &query, &page);
	g_test_message("the first page of 60 of %d films took %" G_GINT64_FORMAT " ms", SCALE_FILMS,
		(g_get_monotonic_time() - start) / 1000);
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++)
		g_assert_cmpuint(page.counts[i], ==, expected[i]);
	g_assert_cmpuint(page.matching, ==, SCALE_FILMS);
	g_assert_cmpuint(page.films->len, ==, 60);
	g_assert_cmpuint(page.facts_read, ==, 60);
	g_assert_nonnull(page.next);
	g_assert_null(page.previous);
	for (i = 0; i < page.films->len; i++) {
		const AxisMovieFacts *f = g_ptr_array_index(page.films, i);
		g_autofree gchar *name = g_strdup_printf("%u", i + 1);
		AxisMovieState state = g_array_index(page.states, AxisMovieState, i);

		/* Film 1 was added last. */
		g_assert_true(g_str_has_suffix(f->title, name));
		g_assert_cmpint(state, ==, GPOINTER_TO_INT(g_hash_table_lookup(truth, &f->id)));
	}
	axis_movies_list_page_clear(&page);

	/* Every order, every film once and in order, forwards (and back for two
	 * of them). */
	for (s = AXIS_MOVIES_SORT_ADDED; s <= AXIS_MOVIES_SORT_SIZE; s++) {
		g_autoptr(GArray) order = NULL;

		start = g_get_monotonic_time();
		order = walk(fx, (AxisMoviesSort)s, 100, s == AXIS_MOVIES_SORT_TITLE || s == AXIS_MOVIES_SORT_SIZE);
		g_test_message("by %s: %d films in pages of 100 in %" G_GINT64_FORMAT " ms", axis_movies_sort_name(s),
			SCALE_FILMS, (g_get_monotonic_time() - start) / 1000);
	}

	/* By state: the tab's count, and only films of that state. */
	for (s = 0; s < AXIS_MOVIE_N_STATES; s++) {
		g_autofree gchar *cursor = NULL;
		guint seen = 0;

		if (expected[s] == 0)
			continue;
		do {
			memset(&query, 0, sizeof(query));
			query.states = 1u << s;
			query.limit = 100;
			query.sort = AXIS_MOVIES_SORT_TITLE;
			query.after = cursor;
			page_of(fx, &query, &page);
			g_assert_cmpuint(page.matching, ==, expected[s]);
			for (i = 0; i < page.films->len; i++) {
				const AxisMovieFacts *f = g_ptr_array_index(page.films, i);

				g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(truth, &f->id)), ==, s);
				g_assert_cmpint(g_array_index(page.states, AxisMovieState, i), ==, s);
			}
			seen += page.films->len;
			g_free(cursor);
			cursor = g_strdup(page.next);
			axis_movies_list_page_clear(&page);
		} while (cursor != NULL);
		g_assert_cmpuint(seen, ==, expected[s]);
	}

	/* By name: accents, case and spaces aside, the other names too; the
	 * counts are of the films of that name. */
	{
		static const struct
		{
			const gchar *typed;
			guint        n;
		} names[] = {
			{ "aurelie", SCALE_FILMS / 500 },
			{ "AURÉLIE 1000", 1 },
			{ "film genere", SCALE_FILMS / 250 },
			{ "generated film 4999", 1 },
			{ "nothing of the kind", 0 }
		};

		for (i = 0; i < G_N_ELEMENTS(names); i++) {
			guint total = 0;

			memset(&query, 0, sizeof(query));
			query.name = names[i].typed;
			query.limit = 60;
			page_of(fx, &query, &page);
			for (s = 0; s < AXIS_MOVIE_N_STATES; s++)
				total += page.counts[s];
			if (page.matching != names[i].n || total != names[i].n || page.films->len != names[i].n)
				g_error("\"%s\": %u matching, %u counted, %u shown; expected %u", names[i].typed, page.matching,
					total, page.films->len, names[i].n);
			axis_movies_list_page_clear(&page);
		}
	}

	/* The bound: in an order an index serves, a page's ids read a page of
	 * films and titles, not every one (EXPLAIN ANALYZE of the statement). */
	for (s = AXIS_MOVIES_SORT_ADDED; s <= AXIS_MOVIES_SORT_YEAR; s++) {
		gdouble films = 0;
		gdouble titles = 0;

		memset(&query, 0, sizeof(query));
		query.sort = (AxisMoviesSort)s;
		query.limit = 60;
		plan_reads(fx, &query, &films, &titles);
		g_test_message("by %s, the first page's ids read %.0f films and %.0f titles", axis_movies_sort_name(s),
			films, titles);
		g_assert_cmpfloat(films, <=, 61);
		g_assert_cmpfloat(titles, <=, 61);
	}
	{
		gdouble films = 0;
		gdouble titles = 0;

		/* A later page: the same. */
		memset(&query, 0, sizeof(query));
		query.limit = 60;
		page_of(fx, &query, &page);
		query.after = page.next;
		plan_reads(fx, &query, &films, &titles);
		g_test_message("the second page's ids read %.0f films and %.0f titles", films, titles);
		g_assert_cmpfloat(films, <=, 61);
		g_assert_cmpfloat(titles, <=, 61);
		axis_movies_list_page_clear(&page);
		/* What an order no index serves, or a state, costs, for the record. */
		memset(&query, 0, sizeof(query));
		query.limit = 60;
		query.sort = AXIS_MOVIES_SORT_SIZE;
		plan_reads(fx, &query, &films, &titles);
		g_test_message("by size, the first page's ids read %.0f films", films);
		query.sort = AXIS_MOVIES_SORT_TITLE;
		query.states = 1u << AXIS_MOVIE_IN_LIBRARY;
		plan_reads(fx, &query, &films, &titles);
		g_test_message("in the library, by title, the first page's ids read %.0f films", films);
	}
}

/* ── The whole library at scale ─────────────────────────────────── */

#define SCALE_ON_DISK 3000
#define SCALE_HELD    400

/*
 * SCALE_ON_DISK made-up movies on disk that Axis does not manage, made by
 * SQL as identification leaves them: movie j ("Shelf Movie j") is the
 * folder shelf/j with one file of j KiB, linked as sure and found j and a
 * half seconds ago; every seventh has a second copy, the folder copies/j
 * with a file of 1 KiB.  And SCALE_HELD folders held for a person.  Their
 * posters are the six make_films() keeps.
 */
static void
make_on_disk(Fixture *fx)
{
	g_free(q(fx, "WITH t AS (INSERT INTO meta_title (kind, title, year, status, release_date, poster_ref, provider,"
		" provider_id) SELECT 'film', 'Shelf Movie ' || j, 1930 + j %% 90, 'released', DATE '1990-01-01' + j,"
		" '/gen' || (j %% 6) || '.png', 'fixture', 'shelf' || j FROM generate_series(1, %d) j RETURNING id,"
		" substr(provider_id, 6)::int AS j)"
		" INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence, title_id,"
		" linked_how, created_at) SELECT %" G_GINT64_FORMAT ", library_path_hash(p.path), 'folder', p.path, 'linked',"
		" 'sure', t.id, 'auto', now() - make_interval(secs => t.j + 0.5) FROM t,"
		" LATERAL (VALUES ('shelf/' || t.j), (CASE WHEN t.j %% 7 = 0 THEN 'copies/' || t.j END)) p (path)"
		" WHERE p.path IS NOT NULL RETURNING 1", SCALE_ON_DISK, fx->root_id));
	g_free(q(fx, "WITH f AS (INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) SELECT %" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" i.rel_path || '/movie.mkv', 'video', CASE WHEN i.rel_path LIKE 'shelf/%%'"
		" THEN split_part(i.rel_path, '/', 2)::bigint * 1024 ELSE 1024 END, 1, 0, 1, 0, 3000000 + i.id, 1, false,"
		" false, false, 1, 1, 1 FROM library_identity i WHERE i.state = 'linked' RETURNING id, rel_path)"
		" INSERT INTO library_identity_file (file_id, identity_id, best, quality) SELECT f.id, i.id, true,"
		" 'webdl_1080p' FROM f JOIN library_identity i ON f.rel_path = i.rel_path || '/movie.mkv' RETURNING 1",
		fx->root_id, fx->volume_id));
	axis_test_db_exec(fx->t->db, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state,"
		" confidence) SELECT (SELECT id FROM library_root LIMIT 1), library_path_hash('held/' || k), 'folder',"
		" 'held/' || k, 'review', 'unknown' FROM generate_series(1, " G_STRINGIFY(SCALE_HELD) ") k");
	axis_test_db_exec(fx->t->db, "ANALYZE");
}

/* An entry's last key in the orders by date and by size: a film's id, a
 * movie on disk's title, negated. */
static gint64
entry_key(const AxisMovieFacts *f)
{
	return f->library_only ? -f->title_id : f->id;
}

/* Whether @a comes before @b in @sort, films and movies on disk alike. */
static gboolean
entry_in_order(
	AxisMoviesSort        sort,
	const AxisMovieFacts *a,
	const AxisMovieFacts *b
){
	g_autofree gchar *ta = g_utf8_strdown(a->title, -1);
	g_autofree gchar *tb = g_utf8_strdown(b->title, -1);

	switch (sort) {
	case AXIS_MOVIES_SORT_ADDED:
		return a->added_us > b->added_us || (a->added_us == b->added_us && entry_key(a) > entry_key(b));
	case AXIS_MOVIES_SORT_SIZE:
		return a->size_bytes > b->size_bytes || (a->size_bytes == b->size_bytes && entry_key(a) > entry_key(b));
	case AXIS_MOVIES_SORT_YEAR:
		if (a->year != b->year)
			return a->year > b->year;
		return strcmp(ta, tb) < 0 || (strcmp(ta, tb) == 0 && a->title_id < b->title_id);
	default:
		if (strcmp(ta, tb) != 0)
			return strcmp(ta, tb) < 0;
		return a->year < b->year || (a->year == b->year && a->title_id < b->title_id);
	}
}

/* Every entry of the list in @sort, a page of @limit at a time, each read
 * checked: no title twice, in order, the page's facts alone, and at most
 * five statements a page whatever the number of entries; then (@and_back)
 * back again from the last page, a page before at a time, the same entries.
 * How many. */
static guint
walk_entries(
	Fixture        *fx,
	AxisMoviesSort  sort,
	guint           states,
	guint           limit,
	gboolean        and_back,
	guint          *most
){
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GArray) order = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autofree gchar *cursor = NULL;
	g_autofree gchar *back = NULL;
	AxisMovieFacts *last = NULL;
	guint total = 0;

	*most = 0;
	for (;;) {
		AxisMoviesListQuery query;
		AxisMoviesListPage page;
		guint before = axis_db_pool_get_statements_sent(fx->t->pool);
		guint spent;
		guint i;

		memset(&query, 0, sizeof(query));
		query.sort = sort;
		query.states = states;
		query.limit = limit;
		query.after = cursor;
		page_of(fx, &query, &page);
		spent = axis_db_pool_get_statements_sent(fx->t->pool) - before;
		*most = MAX(*most, spent);
		g_assert_cmpuint(spent, <=, 5);
		for (i = 0; i < page.films->len; i++) {
			AxisMovieFacts *f = g_ptr_array_index(page.films, i);
			gint64 *key = g_new(gint64, 1);

			*key = f->title_id;
			if (g_hash_table_contains(seen, key))
				g_error("%s: title %" G_GINT64_FORMAT " listed twice", axis_movies_sort_name(sort), f->title_id);
			g_hash_table_add(seen, key);
			if (last != NULL && !entry_in_order(sort, last, f))
				g_error("%s: \"%s\" after \"%s\" is out of order", axis_movies_sort_name(sort), f->title, last->title);
			if (f->library_only)
				g_assert_cmpint(g_array_index(page.states, AxisMovieState, i), ==, AXIS_MOVIE_IN_LIBRARY);
			g_array_append_val(order, f->title_id);
		}
		total += page.films->len;
		axis_movie_facts_free(last);
		last = page.films->len > 0 ? g_ptr_array_steal_index(page.films, page.films->len - 1) : NULL;
		g_free(cursor);
		cursor = g_strdup(page.next);
		if (cursor == NULL)
			back = g_strdup(page.previous);
		axis_movies_list_page_clear(&page);
		if (cursor == NULL)
			break;
	}
	axis_movie_facts_free(last);
	if (and_back) {
		/* The last page's previous, and each page before. */
		guint at = order->len - ((order->len - 1) % limit + 1);

		while (back != NULL) {
			AxisMoviesListQuery query;
			AxisMoviesListPage page;
			guint i;

			memset(&query, 0, sizeof(query));
			query.sort = sort;
			query.states = states;
			query.limit = limit;
			query.before = back;
			page_of(fx, &query, &page);
			g_assert_cmpuint(page.films->len, ==, MIN(limit, at));
			for (i = 0; i < page.films->len; i++) {
				const AxisMovieFacts *f = g_ptr_array_index(page.films, i);

				g_assert_cmpint(f->title_id, ==, g_array_index(order, gint64, at - page.films->len + i));
			}
			at -= page.films->len;
			g_free(back);
			back = g_strdup(page.previous);
			g_assert_nonnull(page.next);
			axis_movies_list_page_clear(&page);
		}
		g_assert_cmpuint(at, ==, 0);
	}
	return total;
}

/*
 * Spec (movies): The whole library at scale.  5,000 films and 3,000 movies
 * on disk (some with two copies) and 400 folders held for review: every
 * order and the in-library tab list every entry once and in order, the
 * counts are every entry's, a page reads its own entries' facts and costs
 * at most five statements whatever the number of entries (and the page in
 * the browser the same number of statements for 60 posters as for 100),
 * and the movies on disk are read in one pass over the linked units.
 */
static void
test_library_at_scale(Fixture *fx, gconstpointer data)
{
	guint expected[AXIS_MOVIE_N_STATES];
	g_autoptr(GHashTable) truth = NULL;
	AxisMoviesListQuery query;
	AxisMoviesListPage page;
	guint units = SCALE_ON_DISK + SCALE_ON_DISK / 7 + SCALE_HELD;
	guint most = 0;
	gint64 start;
	guint s;
	guint i;

	make_films(fx);
	truth = derived_states(fx, expected);
	make_on_disk(fx);

	/* The first page: the counts are every entry's. */
	memset(&query, 0, sizeof(query));
	query.limit = 60;
	start = g_get_monotonic_time();
	page_of(fx, &query, &page);
	g_test_message("the first page of 60 of %d entries took %" G_GINT64_FORMAT " ms", SCALE_FILMS + SCALE_ON_DISK,
		(g_get_monotonic_time() - start) / 1000);
	g_assert_cmpint((g_get_monotonic_time() - start) / 1000, <, 10000);
	for (i = 0; i < AXIS_MOVIE_N_STATES; i++)
		g_assert_cmpuint(page.counts[i], ==, expected[i] + (i == AXIS_MOVIE_IN_LIBRARY ? SCALE_ON_DISK : 0));
	g_assert_cmpuint(page.library, ==, SCALE_ON_DISK);
	g_assert_cmpuint(page.review, ==, SCALE_HELD);
	g_assert_cmpuint(page.matching, ==, SCALE_FILMS + SCALE_ON_DISK);
	g_assert_cmpuint(page.facts_read, ==, 60);
	axis_movies_list_page_clear(&page);

	/* Every order, every entry once and in order, a few statements a page. */
	for (s = AXIS_MOVIES_SORT_ADDED; s <= AXIS_MOVIES_SORT_SIZE; s++) {
		guint n;

		start = g_get_monotonic_time();
		n = walk_entries(fx, (AxisMoviesSort)s, 0, 100, s == AXIS_MOVIES_SORT_ADDED || s == AXIS_MOVIES_SORT_TITLE,
			&most);
		g_test_message("by %s: %u entries in pages of 100 in %" G_GINT64_FORMAT " ms, at most %u statements a page",
			axis_movies_sort_name(s), n, (g_get_monotonic_time() - start) / 1000, most);
		g_assert_cmpuint(n, ==, SCALE_FILMS + SCALE_ON_DISK);
	}
	/* The in-library tab: the films in the library and every movie on disk. */
	g_assert_cmpuint(walk_entries(fx, AXIS_MOVIES_SORT_TITLE, 1u << AXIS_MOVIE_IN_LIBRARY, 200, FALSE, &most), ==,
		expected[AXIS_MOVIE_IN_LIBRARY] + SCALE_ON_DISK);
	/* Another state: no movie on disk. */
	g_assert_cmpuint(walk_entries(fx, AXIS_MOVIES_SORT_ADDED, 1u << AXIS_MOVIE_WANTED, 200, FALSE, &most), ==,
		expected[AXIS_MOVIE_WANTED]);
	/* The films alone, as the dashboard reads them. */
	memset(&query, 0, sizeof(query));
	query.limit = 5;
	query.managed_only = TRUE;
	page_of(fx, &query, &page);
	g_assert_cmpuint(page.matching, ==, SCALE_FILMS);
	g_assert_cmpuint(page.counts[AXIS_MOVIE_IN_LIBRARY], ==, expected[AXIS_MOVIE_IN_LIBRARY] + SCALE_ON_DISK);
	for (i = 0; i < page.films->len; i++)
		g_assert_false(((AxisMovieFacts *)g_ptr_array_index(page.films, i))->library_only);
	axis_movies_list_page_clear(&page);
	/* By name, a movie on disk among them. */
	memset(&query, 0, sizeof(query));
	query.name = "shelf movie 2999";
	query.limit = 60;
	page_of(fx, &query, &page);
	g_assert_cmpuint(page.matching, ==, 1);
	g_assert_cmpuint(page.films->len, ==, 1);
	g_assert_true(((AxisMovieFacts *)g_ptr_array_index(page.films, 0))->library_only);
	g_assert_cmpuint(((AxisMovieFacts *)g_ptr_array_index(page.films, 0))->n_copies, ==, 1);
	axis_movies_list_page_clear(&page);
	query.name = "shelf movie 2996";
	page_of(fx, &query, &page);
	g_assert_cmpuint(((AxisMovieFacts *)g_ptr_array_index(page.films, 0))->n_copies, ==, 2);
	axis_movies_list_page_clear(&page);

	/* The plan: the movies on disk are read in one pass over the units, not
	 * once per entry. */
	for (s = AXIS_MOVIES_SORT_ADDED; s <= AXIS_MOVIES_SORT_SIZE; s++) {
		g_autofree gchar *plan = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(GError) error = NULL;
		JsonObject *top;
		gdouble read;

		memset(&query, 0, sizeof(query));
		query.sort = (AxisMoviesSort)s;
		query.limit = 60;
		g_assert_true(axis_movies_list_explain(fx->movies, &query, &plan, &error));
		g_assert_no_error(error);
		g_assert_true(json_parser_load_from_data(parser, plan, -1, NULL));
		top = json_object_get_object_member(json_array_get_object_element(json_node_get_array(
			json_parser_get_root(parser)), 0), "Plan");
		read = rows_read(top, "library_identity");
		g_test_message("by %s, the first page's entries read %.0f units, %.0f films, %.0f titles",
			axis_movies_sort_name(s), read, rows_read(top, "movie"), rows_read(top, "meta_title"));
		g_assert_cmpfloat(read, <=, units);
	}

	/* In the browser: the same statements for a page of 60 posters as for a
	 * table of 100 (no statement per poster). */
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (id, username, password_hash, role) OVERRIDING SYSTEM VALUE"
		" VALUES (1, 'root', '$argon2id$test', 'admin'), (2, 'viewer', '$argon2id$test', 'user')");
	{
		static const gchar *const views[] = { "/films?view=posters", "/films?view=table", "/films?view=posters",
			"/films?view=table" };
		guint spent[G_N_ELEMENTS(views)];
		guint v;

		for (v = 0; v < G_N_ELEMENTS(views); v++) {
			guint before = axis_db_pool_get_statements_sent(fx->t->pool);
			AxisTestReply reply = get_as(fx, fx->admin, views[v]);

			g_assert_cmpuint(reply.status, ==, 200);
			spent[v] = axis_db_pool_get_statements_sent(fx->t->pool) - before;
			g_test_message("%s: %u statements", views[v], spent[v]);
			axis_test_reply_clear(&reply);
		}
		/* (The first of each view also keeps the account's choice.) */
		g_assert_cmpuint(spent[2], ==, spent[3]);
		g_assert_cmpuint(spent[2], <=, 12);
	}
}

/* 4.2: each view shows the same films in the same order; the view chosen is
 * kept per account and per list. */
static void
test_list_views(Fixture *fx, gconstpointer data)
{
	static const gchar *const views[] = { "posters", "overview", "table" };
	g_autoptr(GPtrArray) orders = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	AxisWebView chosen;
	AxisTestReply reply;
	guint v;
	guint i;

	make_films(fx);
	/* Accounts the views are kept for (the auth here has no database). */
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (id, username, password_hash, role) OVERRIDING SYSTEM VALUE"
		" VALUES (1, 'root', '$argon2id$test', 'admin'), (2, 'viewer', '$argon2id$test', 'user')");

	for (v = 0; v < G_N_ELEMENTS(views); v++) {
		g_autofree gchar *path = g_strdup_printf("/films?sort=title&view=%s", views[v]);
		GPtrArray *hrefs = g_ptr_array_new_with_free_func(g_free);
		const gchar *p;

		reply = get_as(fx, fx->admin, path);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_csp_clean(reply.body);
		/* The view marked as the current one. */
		{
			g_autofree gchar *current = g_strdup_printf("aria-current=\"true\" href=\"/films?sort=title&amp;view=%s\"",
				views[v]);

			assert_contains(reply.body, current);
		}
		/* Each film's title page, in order (the overview links each twice:
		 * its poster and its name; a film that needs attention links a
		 * place on its page too, which is not another film). */
		for (p = reply.body; (p = strstr(p, "href=\"/title/")) != NULL; p++) {
			g_autofree gchar *href = g_strndup(p, (gsize)(strchr(p + 6, '"') - p));

			if (strchr(href, '#') != NULL)
				continue;
			if (hrefs->len == 0 || strcmp(g_ptr_array_index(hrefs, hrefs->len - 1), href) != 0)
				g_ptr_array_add(hrefs, g_steal_pointer(&href));
		}
		g_ptr_array_add(orders, hrefs);
		if (v == 2)
			g_assert_cmpuint(axis_test_web_assert_tables(reply.body), ==, 1);
		{
			g_autofree gchar *name = g_strdup_printf("films-%s.html", views[v]);

			save_page(name, reply.body);
		}
		axis_test_reply_clear(&reply);
	}
	/* 60 a page as posters and as an overview, 100 in a table; the same
	 * films in the same order. */
	g_assert_cmpuint(((GPtrArray *)g_ptr_array_index(orders, 0))->len, ==, 60);
	g_assert_cmpuint(((GPtrArray *)g_ptr_array_index(orders, 1))->len, ==, 60);
	g_assert_cmpuint(((GPtrArray *)g_ptr_array_index(orders, 2))->len, ==, 100);
	for (i = 0; i < 60; i++) {
		const gchar *poster = g_ptr_array_index((GPtrArray *)g_ptr_array_index(orders, 0), i);

		g_assert_cmpstr(poster, ==, g_ptr_array_index((GPtrArray *)g_ptr_array_index(orders, 1), i));
		g_assert_cmpstr(poster, ==, g_ptr_array_index((GPtrArray *)g_ptr_array_index(orders, 2), i));
	}

	/* The last view chosen is the list's view next time, for that account
	 * and that list only. */
	reply = get_as(fx, fx->admin, "/films");
	assert_contains(reply.body, "data-table kind-titles");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/films");
	assert_contains(reply.body, "title-grid");
	assert_lacks(reply.body, "kind-titles");
	axis_test_reply_clear(&reply);
	axis_web_view_choose(fx->t->pool, 1, "series", NULL, &chosen);
	g_assert_cmpint(chosen, ==, AXIS_WEB_VIEW_POSTERS);
	axis_web_view_choose(fx->t->pool, 1, "films", NULL, &chosen);
	g_assert_cmpint(chosen, ==, AXIS_WEB_VIEW_TABLE);
	{
		g_autofree gchar *kept = q(fx, "SELECT string_agg(user_id || ':' || list || ':' || view, ',' ORDER BY user_id)"
			" FROM user_list_view");

		g_assert_cmpstr(kept, ==, "1:films:table");
	}

	/* A name filter, kept by the links; an overview with the reason of a
	 * film that needs attention. */
	reply = get_as(fx, fx->admin, "/films?q=aurelie&view=overview");
	assert_contains(reply.body, "value=\"aurelie\"");
	assert_contains(reply.body, "10 movies");
	assert_contains(reply.body, "q=aurelie&amp;view=overview");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films?state=attention&view=overview");
	assert_contains(reply.body, "title-row-reason title-row-attention");
	save_page("films-overview-attention.html", reply.body);
	axis_test_reply_clear(&reply);
	/* Paging by links: the second page of the table. */
	reply = get_as(fx, fx->admin, "/films?view=table");
	{
		const gchar *next = strstr(reply.body, ">Next page</a>");
		const gchar *start;
		g_autofree gchar *raw = NULL;
		g_auto(GStrv) parts = NULL;
		AxisTestReply second;

		g_assert_nonnull(next);
		for (start = next; start > reply.body && strncmp(start, "href=\"", 6) != 0; start--)
			;
		raw = g_strndup(start + 6, (gsize)(strchr(start + 6, '"') - start - 6));
		parts = g_strsplit(raw, "&amp;", -1);
		g_free(raw);
		raw = g_strjoinv("&", parts);
		second = get_as(fx, fx->admin, raw);
		g_assert_cmpuint(second.status, ==, 200);
		assert_contains(second.body, ">Previous page</a>");
		assert_contains(second.body, ">First page</a>");
		assert_contains(second.body, "Generated Film 101<");
		axis_test_reply_clear(&second);
	}
	axis_test_reply_clear(&reply);
}

/* ── Movies on disk that Axis does not manage ───────────────────── */

/* @size bytes at @rel under the films root of @disk. */
static void
put(
	const gchar *disk,
	const gchar *rel,
	gsize        size
){
	g_autofree gchar *path = g_build_filename(disk, "films", rel, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autofree gchar *content = g_strnfill(MAX(size, 1), 'x');

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_assert_true(g_file_set_contents(path, content, (gssize)MAX(size, 1), NULL));
}

static void
scan_root(
	Fixture *fx,
	gint64   root
){
	AxisScanResult *result = axis_test_library_scan(fx->t, root);

	axis_scan_result_free(result);
}

/*
 * The unit identification makes of the folder @folder of @root: linked as
 * sure to @title (with every file under it, the first the best, of the
 * ladder quality @quality, an SQL literal), or, with @title 0, left in
 * @state ("review": for a person; "waiting": for the provider).
 */
static gint64
unit(
	Fixture     *fx,
	gint64       root,
	const gchar *folder,
	gint64       title,
	const gchar *state,
	const gchar *quality
){
	g_autofree gchar *id = NULL;
	g_autofree gchar *sql = NULL;

	if (title > 0)
		id = q(fx, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence,"
			" title_id, linked_how) VALUES (%" G_GINT64_FORMAT ", library_path_hash('%s'), 'folder', '%s', 'linked',"
			" 'sure', %" G_GINT64_FORMAT ", 'auto') RETURNING id", root, folder, folder, title);
	else
		id = q(fx, "INSERT INTO library_identity (root_id, scope_hash, unit_kind, rel_path, state, confidence)"
			" VALUES (%" G_GINT64_FORMAT ", library_path_hash('%s'), 'folder', '%s', '%s', %s) RETURNING id", root,
			folder, folder, state, g_strcmp0(state, "review") == 0 ? "'unknown'" : "NULL");
	g_assert_nonnull(id);
	if (title > 0) {
		sql = g_strdup_printf("INSERT INTO library_identity_file (file_id, identity_id, best, quality)"
			" SELECT f.id, %s, f.rel_path = min(f.rel_path) OVER (), %s FROM library_file f"
			" WHERE f.root_id = %" G_GINT64_FORMAT " AND f.rel_path LIKE '%s/%%'", id, quality, root, folder);
		axis_test_db_exec(fx->t->db, sql);
	}
	return g_ascii_strtoll(id, NULL, 10);
}

/* A made-up movie of the fixture provider, stored as the title page and
 * identification store one. */
static gint64
made_up(
	Fixture     *fx,
	const gchar *id,
	const gchar *title,
	gint         year
){
	g_autofree gchar *path = g_strdup_printf("/title/film/%s", id);
	g_autofree gchar *json = g_strdup_printf("{\"id\":\"%s\",\"kind\":\"film\",\"title\":\"%s\",\"year\":%d,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"%d-04-08\",\"overview\":\"Made up for the "
		"tests.\"}", id, title, year, year);

	set_route(fx, path, json);
	return title_of(fx, id);
}

/* How many of the list's entries are in @state, and in all, as the list
 * counts them (with how many are on disk and not managed, and held). */
static void
list_counts(
	Fixture            *fx,
	AxisMoviesListPage *page
){
	AxisMoviesListQuery query;
	g_autoptr(GError) error = NULL;

	memset(&query, 0, sizeof(query));
	query.limit = 60;
	g_assert_true(axis_movies_list_page(fx->movies, &query, page, &error));
	g_assert_no_error(error);
}

static guint
total_of(const AxisMoviesListPage *page)
{
	guint total = 0;
	guint i;

	for (i = 0; i < AXIS_MOVIE_N_STATES; i++)
		total += page->counts[i];
	return total;
}

/*
 * Spec (movies): The whole library is listed; Nothing is asked of a movie on
 * disk; Managing one; Removing a managed one with its file on disk; One
 * entry per movie; A disk away; A file gone; Held back for a person.  The
 * list, its counts and tabs, the posters and the overview, the title page
 * of each kind, the dashboard, Wanted, the scheduler, and that nothing on
 * disk or in the film tables was written by looking.
 */
static void
test_library(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *disk2 = NULL;
	gint64 volume2;
	gint64 root2;
	gint64 lantern = made_up(fx, "901", "Lantern Harbour", 2011);
	gint64 copper = made_up(fx, "902", "Copper Orchard", 2004);
	gint64 comet = made_up(fx, "903", "Paper Comet", 2015);
	gint64 meadow = made_up(fx, "904", "Glass Meadow", 2008);
	gint64 atlas_title = made_up(fx, "905", "Silver Atlas", 2019);
	g_autofree gchar *lantern_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, lantern);
	g_autofree gchar *copper_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, copper);
	g_autofree gchar *comet_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, comet);
	g_autofree gchar *lantern_file = g_build_filename(fx->disk, "films", "Lantern Harbour (2011)",
		"Lantern.Harbour.2011.1080p.BluRay.mkv", NULL);
	g_autofree gchar *meadow_file = g_build_filename(fx->disk, "films", "Glass Meadow (2008)", "Glass.Meadow.2008.mkv",
		NULL);
	g_autofree gchar *n = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *digest2 = NULL;
	AxisMoviesListPage page;
	AxisMovieScheduleReport report;
	AxisMovieSpec spec;
	AxisTestReply reply;
	gint64 atlas = 0;

	/* A second disk (the scratch directory is one filesystem, which can be
	 * one volume only: axis_test_library_add_volume_beside(); the first is
	 * away while it is added). */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*is offline at*");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*was given up*");
	axis_test_library_unplug(fx->t, fx->disk, "away-one");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	disk2 = axis_test_library_disk(fx->t, "disk2");
	volume2 = axis_test_library_add_volume_beside(fx->t, disk2, "Disk two");
	root2 = axis_test_library_add_root(fx->t, volume2, disk2, "films", AXIS_MEDIA_MOVIES);
	axis_test_library_plug(fx->t, fx->disk, "away-one");
	g_assert_true(axis_storage_check_volume(fx->t->storage, fx->volume_id, NULL, &error));
	g_assert_no_error(error);

	/* What is on the two disks, scanned, and what identification made of
	 * it: four movies linked (one with a copy on each disk), two folders it
	 * holds for a person and one it waits for the provider about. */
	put(fx->disk, "Lantern Harbour (2011)/Lantern.Harbour.2011.1080p.BluRay.mkv", 3000);
	put(fx->disk, "Copper Orchard (2004)/Copper.Orchard.2004.720p.WEB-DL.mkv", 2000);
	put(disk2, "Copper Orchard (2004)/Copper.Orchard.2004.2160p.BluRay.mkv", 5000);
	put(disk2, "Paper Comet (2015)/Paper.Comet.2015.1080p.WEB-DL.mkv", 2500);
	put(fx->disk, "Glass Meadow (2008)/Glass.Meadow.2008.mkv", 1500);
	put(fx->disk, "Unclear Folder/video.mkv", 900);
	put(fx->disk, "Another Unclear/video.mkv", 900);
	put(fx->disk, "Slow Answer (2001)/video.mkv", 900);
	scan_root(fx, fx->root_id);
	scan_root(fx, root2);
	unit(fx, fx->root_id, "Lantern Harbour (2011)", lantern, "linked", "'bluray_1080p'");
	unit(fx, fx->root_id, "Copper Orchard (2004)", copper, "linked", "'webdl_720p'");
	unit(fx, root2, "Copper Orchard (2004)", copper, "linked", "'bluray_2160p'");
	unit(fx, root2, "Paper Comet (2015)", comet, "linked", "'webdl_1080p'");
	unit(fx, fx->root_id, "Glass Meadow (2008)", meadow, "linked", "NULL");
	unit(fx, fx->root_id, "Unclear Folder", 0, "review", NULL);
	unit(fx, fx->root_id, "Another Unclear", 0, "review", NULL);
	unit(fx, fx->root_id, "Slow Answer (2001)", 0, "waiting", NULL);
	/* And one movie a person added, not on disk. */
	memset(&spec, 0, sizeof(spec));
	spec.title_id = atlas_title;
	spec.root_id = fx->root_id;
	spec.monitored = TRUE;
	spec.availability = AXIS_MOVIE_RELEASED;
	spec.user = "root";
	g_assert_true(axis_movies_add(fx->movies, &spec, &atlas, &error));
	g_assert_no_error(error);
	digest = axis_test_library_index(fx->t, fx->root_id);
	digest2 = axis_test_library_index(fx->t, root2);

	/* The counts: every movie, the four on disk in the library. */
	list_counts(fx, &page);
	g_assert_cmpuint(total_of(&page), ==, 5);
	g_assert_cmpuint(page.counts[AXIS_MOVIE_IN_LIBRARY], ==, 4);
	g_assert_cmpuint(page.counts[AXIS_MOVIE_WANTED], ==, 1);
	g_assert_cmpuint(page.library, ==, 4);
	g_assert_cmpuint(page.review, ==, 2);
	g_assert_cmpuint(page.waiting, ==, 1);
	g_assert_cmpuint(page.matching, ==, 5);
	g_assert_cmpuint(page.films->len, ==, 5);
	{
		guint i;

		for (i = 0; i < page.films->len; i++) {
			const AxisMovieFacts *f = g_ptr_array_index(page.films, i);

			/* One entry per movie: two copies on two disks are one. */
			if (f->title_id == copper) {
				g_assert_true(f->library_only);
				g_assert_cmpuint(f->n_copies, ==, 2);
				g_assert_cmpuint(f->n_files, ==, 2);
				g_assert_cmpint(f->size_bytes, ==, 7000);
				g_assert_cmpint(f->file_quality, ==, AXIS_QUALITY_BLURAY_2160P);
			}
			if (f->title_id == lantern) {
				g_assert_true(f->library_only);
				g_assert_cmpint(f->id, ==, 0);
				g_assert_false(f->monitored);
				g_assert_cmpuint(f->n_copies, ==, 1);
				g_assert_cmpint(f->root_id, ==, fx->root_id);
			}
			if (f->title_id == atlas_title)
				g_assert_false(f->library_only);
		}
	}
	axis_movies_list_page_clear(&page);

	/* The list, for someone who is not an administrator: every movie on
	 * disk in the library, with what identification holds back, and the
	 * tab counts. */
	reply = get_as(fx, fx->viewer, "/films?view=overview&sort=title");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Lantern Harbour<");
	assert_contains(reply.body, ">Copper Orchard<");
	assert_contains(reply.body, ">Paper Comet<");
	assert_contains(reply.body, ">Glass Meadow<");
	assert_contains(reply.body, ">Silver Atlas<");
	assert_contains(reply.body, "film-mark-in_library");
	assert_contains(reply.body, "On disk, WEB-DL 1080p");
	assert_contains(reply.body, ", 2 copies, not managed");
	/* One line carries it all: the reason's near copy of it is not repeated under it. */
	assert_lacks(reply.body, "On disk as BluRay 1080p, not managed by Axis.");
	assert_contains(reply.body, "2 movie folders and files on disk are not listed yet: identification was not sure "
		"which movie each is, and waits for a person to say.");
	assert_contains(reply.body, "1 more waits for TMDb");
	/* (Identify is not part of this daemon.) */
	assert_contains(reply.body, "Identify is switched off");
	assert_contains(reply.body, "This list is the whole movie library");
	assert_contains(reply.body, ">In library</span><span class=\"tab-count\">4</span>");
	assert_contains(reply.body, ">All</span><span class=\"tab-count\">5</span>");
	assert_csp_clean(reply.body);
	save_page("films-library.html", reply.body);
	axis_test_reply_clear(&reply);
	/* The posters say which has two copies. */
	reply = get_as(fx, fx->viewer, "/films?view=posters");
	assert_contains(reply.body, "Movie, 2004, 2 copies");
	axis_test_reply_clear(&reply);
	/* Filtered by state and by name, the same entries. */
	reply = get_as(fx, fx->viewer, "/films?state=in_library");
	assert_contains(reply.body, ">Paper Comet<");
	assert_lacks(reply.body, ">Silver Atlas<");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/films?state=wanted");
	assert_contains(reply.body, ">Silver Atlas<");
	assert_lacks(reply.body, ">Paper Comet<");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/films?q=copper");
	assert_contains(reply.body, ">Copper Orchard<");
	assert_lacks(reply.body, ">Lantern Harbour<");
	axis_test_reply_clear(&reply);

	/* A movie on disk's page: in the library, not managed, its file, and for
	 * an administrator the form that has Axis manage it (no search, no
	 * releases, nothing to remove). */
	reply = get_as(fx, fx->admin, lantern_page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "film-mark-in_library");
	assert_contains(reply.body, "On disk, not managed by Axis");
	assert_contains(reply.body, "<h2>Files of this movie</h2>");
	assert_contains(reply.body, "Lantern.Harbour.2011.1080p.BluRay.mkv");
	assert_contains(reply.body, ">Identified</a>");
	assert_contains(reply.body, "Manage this movie");
	assert_contains(reply.body, "name=\"profile\"");
	assert_contains(reply.body, "name=\"root\"");
	assert_lacks(reply.body, "name=\"search\"");
	assert_lacks(reply.body, "Search for this movie");
	assert_lacks(reply.body, "Remove this movie");
	assert_lacks(reply.body, "<h2>Releases found by the last search</h2>");
	assert_lacks(reply.body, "Add to Movies");
	assert_lacks(reply.body, "id=\"ready-notice\"");
	assert_csp_clean(reply.body);
	save_page("title-on-disk.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, lantern_page);
	assert_contains(reply.body, "On disk, not managed by Axis");
	assert_contains(reply.body, "An administrator can have Axis manage it.");
	assert_lacks(reply.body, "Manage this movie");
	assert_lacks(reply.body, "<form class=\"film");
	axis_test_reply_clear(&reply);
	/* Two copies, shown and not hidden. */
	reply = get_as(fx, fx->viewer, copper_page);
	assert_contains(reply.body, "Copper.Orchard.2004.720p.WEB-DL.mkv");
	assert_contains(reply.body, "Copper.Orchard.2004.2160p.BluRay.mkv");
	assert_contains(reply.body, "2 copies of this movie are on disk.");
	axis_test_reply_clear(&reply);
	/* A search's result card carries the mark of a movie on disk too. */
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"901\",\"kind\":\"film\",\"title\":\"Lantern Harbour\","
		"\"year\":2011}]}");
	reply = get_as(fx, fx->viewer, "/search?q=lantern&kind=film");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Lantern Harbour");
	assert_contains(reply.body, "film-mark-in_library");
	axis_test_reply_clear(&reply);

	/* Looking asked nothing of them and wrote nothing: no film, no link, no
	 * job; Wanted has the one movie a person added; the scheduler considers
	 * that one alone and queues its search, none of the others'. */
	n = q(fx, "SELECT count(*) || '/' || (SELECT count(*) FROM movie_file) || '/' || (SELECT count(*) FROM job"
		" WHERE kind = 'movies.search') FROM movie");
	g_assert_cmpstr(n, ==, "1/0/0");
	reply = get_as(fx, fx->viewer, "/films/wanted");
	assert_contains(reply.body, "Silver Atlas (2019)");
	assert_lacks(reply.body, "Lantern Harbour");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_movies_run_schedule(fx->movies, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(report.considered, ==, 1);
	g_assert_cmpuint(report.queued, ==, 1);
	g_free(n);
	n = q(fx, "SELECT string_agg(dedupe_key, ',') FROM job WHERE kind = 'movies.search'");
	{
		g_autofree gchar *expected = g_strdup_printf("movies.search:%" G_GINT64_FORMAT, atlas);

		g_assert_cmpstr(n, ==, expected);
	}
	/* The dashboard counts the whole library; what was added lately is what
	 * a person added. */
	reply = get_as(fx, fx->admin, "/");
	assert_contains(reply.body, "5 movies: 4 in the library, 1 wanted, 0 downloading, 0 to import.");
	assert_contains(reply.body, "Movies added lately");
	assert_contains(reply.body, "Silver Atlas (2019)");
	assert_lacks(reply.body, "Lantern Harbour (2011)");
	axis_test_reply_clear(&reply);

	/* A file gone (a scan of its disk, verified online, found it missing): no
	 * longer on disk, so no longer listed. */
	g_assert_cmpint(g_unlink(meadow_file), ==, 0);
	scan_root(fx, fx->root_id);
	reply = get_as(fx, fx->viewer, "/films");
	assert_lacks(reply.body, ">Glass Meadow<");
	assert_contains(reply.body, ">Lantern Harbour<");
	axis_test_reply_clear(&reply);
	g_free(digest);
	digest = axis_test_library_index(fx->t, fx->root_id);

	/* A disk away: an offline disk never means a movie is gone.  Paper Comet
	 * stays, and says its disk is away; Copper Orchard keeps both copies. */
	axis_test_library_unplug(fx->t, disk2, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, volume2, NULL, &error));
	g_assert_no_error(error);
	{
		AxisScanResult *offline = axis_test_library_scan_expect(fx->t, root2, AXIS_SCAN_UNAVAILABLE,
			AXIS_SCAN_REASON_VOLUME_UNAVAILABLE);

		axis_scan_result_free(offline);
	}
	list_counts(fx, &page);
	g_assert_cmpuint(page.library, ==, 3);
	g_assert_cmpuint(page.counts[AXIS_MOVIE_IN_LIBRARY], ==, 3);
	axis_movies_list_page_clear(&page);
	reply = get_as(fx, fx->viewer, comet_page);
	assert_contains(reply.body, "Its disk, Disk two, is offline: it stays listed, since a disk that is away never "
		"means a movie is gone.");
	assert_contains(reply.body, ">Disk offline<");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, copper_page);
	assert_contains(reply.body, "One of its disks, Disk two, is offline.");
	assert_contains(reply.body, "2 copies of this movie are on disk.");
	axis_test_reply_clear(&reply);
	axis_test_library_plug(fx->t, disk2, "away");
	g_assert_true(axis_storage_check_volume(fx->t->storage, volume2, NULL, &error));
	g_assert_no_error(error);

	/* Managing one: an administrator's choice, from its page.  It keeps its
	 * file (linked from identification), is in the library, not monitored,
	 * and nothing is searched for. */
	{
		g_autofree gchar *profile = q(fx, "SELECT id FROM quality_profile WHERE slug = 'balanced'");
		g_autofree gchar *form = g_strdup_printf("title=%" G_GINT64_FORMAT "&profile=%s&root=%" G_GINT64_FORMAT
			"&availability=released", lantern, profile, fx->root_id);
		g_autofree gchar *done = g_strdup_printf("%s?done=managed", lantern_page);

		reply = post_as(fx, fx->viewer, lantern_page, "/films", form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, lantern_page, "/films", form);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), done);
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT m.monitored || '/' || count(f.file_id) || '/' || count(f.identity_id) FROM movie m"
			" JOIN movie_file f ON f.movie_id = m.id WHERE m.title_id = %" G_GINT64_FORMAT " GROUP BY m.monitored",
			lantern);
		g_assert_cmpstr(n, ==, "false/1/1");
		g_free(n);
		n = q(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'");
		g_assert_cmpstr(n, ==, "1");
		reply = get_as(fx, fx->admin, done);
		assert_contains(reply.body, "Axis manages this movie now.");
		assert_contains(reply.body, "film-mark-in_library");
		assert_contains(reply.body, "Balanced, not monitored");
		assert_contains(reply.body, "Remove this movie");
		assert_contains(reply.body, "while its file is on disk and recognised by identification, it stays in Movies "
			"as In library, not managed");
		assert_lacks(reply.body, "Manage this movie");
		save_page("title-managed-from-disk.html", reply.body);
		axis_test_reply_clear(&reply);
	}
	/* The counts are the same movies: one fewer on disk alone, the same in
	 * the library. */
	list_counts(fx, &page);
	g_assert_cmpuint(total_of(&page), ==, 4);
	g_assert_cmpuint(page.library, ==, 2);
	g_assert_cmpuint(page.counts[AXIS_MOVIE_IN_LIBRARY], ==, 3);
	axis_movies_list_page_clear(&page);

	/* Removing it: Axis stops managing it, its file stays, and it stays in
	 * the list as a movie on disk (it does not vanish and come back). */
	{
		g_autofree gchar *movie = q(fx, "SELECT id FROM movie WHERE title_id = %" G_GINT64_FORMAT, lantern);
		g_autofree gchar *action = g_strdup_printf("/films/%s/remove", movie);
		g_autofree gchar *done = g_strdup_printf("%s?done=unmanaged", lantern_page);

		reply = post_as(fx, fx->admin, lantern_page, action, "confirm=yes");
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), done);
		axis_test_reply_clear(&reply);
		g_assert_true(g_file_test(lantern_file, G_FILE_TEST_IS_REGULAR));
		g_free(n);
		n = q(fx, "SELECT count(*) FROM movie WHERE title_id = %" G_GINT64_FORMAT, lantern);
		g_assert_cmpstr(n, ==, "0");
		g_free(n);
		n = q(fx, "SELECT text FROM movie_event WHERE kind = 'removed' AND title_id = %" G_GINT64_FORMAT, lantern);
		assert_contains(n, "its file is still on disk, so it stays in Movies as In library");
		reply = get_as(fx, fx->admin, done);
		assert_contains(reply.body, "Axis no longer manages this movie.");
		assert_contains(reply.body, "On disk, not managed by Axis");
		assert_contains(reply.body, "Manage this movie");
		/* Its history, kept with its title. */
		assert_contains(reply.body, "<h2>History</h2>");
		assert_contains(reply.body, "Removed by root.");
		axis_test_reply_clear(&reply);
	}
	list_counts(fx, &page);
	g_assert_cmpuint(total_of(&page), ==, 4);
	g_assert_cmpuint(page.library, ==, 3);
	axis_movies_list_page_clear(&page);

	/* Nothing on either disk was changed by any of it (the one file gone
	 * was removed by the test). */
	{
		g_autofree gchar *after = axis_test_library_index(fx->t, fx->root_id);
		g_autofree gchar *after2 = axis_test_library_index(fx->t, root2);

		g_assert_cmpstr(after, ==, digest);
		g_assert_cmpstr(after2, ==, digest2);
	}

	/* A film whose file an import linked and identification has not looked
	 * at yet: removed, it is not said to stay, and it does not. */
	{
		gint64 tin_title = made_up(fx, "906", "Tin Lighthouse", 2021);
		gint64 tin = 0;
		g_autofree gchar *action = NULL;
		g_autofree gchar *tin_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, tin_title);

		put(fx->disk, "Tin Lighthouse (2021)/Tin.Lighthouse.2021.1080p.WEB-DL.mkv", 1200);
		scan_root(fx, fx->root_id);
		spec.title_id = tin_title;
		g_assert_true(axis_movies_add(fx->movies, &spec, &tin, &error));
		g_assert_no_error(error);
		g_free(q(fx, "INSERT INTO movie_file (file_id, movie_id, quality) SELECT id, %" G_GINT64_FORMAT ","
			" 'webdl_1080p' FROM library_file WHERE rel_path LIKE 'Tin Lighthouse (2021)/%%' RETURNING 1", tin));
		action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/remove", tin);
		reply = post_as(fx, fx->admin, tin_page, action, "confirm=yes");
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "/films?done=removed");
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT text FROM movie_event WHERE kind = 'removed' AND title_id = %" G_GINT64_FORMAT, tin_title);
		assert_lacks(n, "stays in Movies");
		reply = get_as(fx, fx->viewer, "/films");
		assert_lacks(reply.body, ">Tin Lighthouse<");
		axis_test_reply_clear(&reply);
	}
}

/* ── With the module off ────────────────────────────────────────── */

/* Spec: Module off */
static void
test_module_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 title = title_of(fx, "109");
	g_autofree gchar *page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
	AxisTestReply reply;

	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "film-panel");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_MOVIES_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->admin, "/films");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films/wanted");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "film-panel");
	/* No Movies in the navigation, and with no other media type no Media
	 * tab (the search box still offers to search for movies: the metadata
	 * answers that, whether films are managed or not). */
	assert_lacks(reply.body, ">Movies</a>");
	assert_lacks(reply.body, ">Media</a>");
	assert_contains(reply.body, ">Movies</option>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/");
	assert_lacks(reply.body, "href=\"/films\"");
	axis_test_reply_clear(&reply);
}

/* ── The word ───────────────────────────────────────────────────── */

/*
 * What a person reads of @html: its text, and the words of the attributes
 * that are shown or spoken (a placeholder, a tooltip, a label, an image's
 * words).  Not the markup itself: addresses (/films), classes (film-mark)
 * and form values (kind=film) are names in code.
 */
static gchar *
visible_text(const gchar *html)
{
	g_autoptr(GRegex) hidden = g_regex_new("<(script|style)\\b[^>]*>.*?</\\1>", G_REGEX_DOTALL, 0, NULL);
	g_autoptr(GRegex) shown = g_regex_new("\\s(placeholder|title|aria-label|alt)=\"([^\"]*)\"", 0, 0, NULL);
	g_autoptr(GRegex) tags = g_regex_new("<[^>]*>", 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	g_autofree gchar *body = g_regex_replace_literal(hidden, html, -1, 0, " ", 0, NULL);
	g_autofree gchar *text = NULL;
	GString *out = g_string_new(NULL);

	g_regex_match(shown, body, 0, &match);
	while (g_match_info_matches(match)) {
		g_autofree gchar *words = g_match_info_fetch(match, 2);

		g_string_append_printf(out, "%s\n", words);
		g_match_info_next(match, NULL);
	}
	text = g_regex_replace_literal(tags, body, -1, 0, " ", 0, NULL);
	g_string_append(out, text);
	return g_string_free(out, FALSE);
}

/* @what never says "film" or "films", in any case, as a word. */
static void
assert_says_movie(
	const gchar *what,
	const gchar *html
){
	g_autoptr(GRegex) word = g_regex_new("\\bfilms?\\b", G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;
	g_autofree gchar *text = visible_text(html);

	if (g_regex_match(word, text, 0, &match)) {
		gint start = 0;
		gint end = 0;
		g_autofree gchar *around = NULL;

		g_match_info_fetch_pos(match, 0, &start, &end);
		around = g_strndup(text + MAX(start - 80, 0), (gsize)(MIN(end + 80, (gint)strlen(text)) - MAX(start - 80, 0)));
		g_error("%s says \"film\" where a person reads it: ...%s...", what, around);
	}
}

/*
 * The interface's word is "movie": the main Movies pages (the list in each
 * view, a title's page with its notice and its releases, one to add, Wanted,
 * History, the dashboard with its Getting started card) never say "film"
 * where a person reads them.  The root is named for this test, not "films".
 */
static void
test_says_movie(Fixture *fx, gconstpointer data)
{
	static const gchar *const lists[] = { "/films?view=posters", "/films?view=overview", "/films?view=table",
		"/films?state=attention", "/films/wanted", "/films/history", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *card = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	Scene s;
	guint i;

	/* What is read is the text and the shown attributes, not the markup. */
	{
		g_autofree gchar *seen = visible_text("<a href=\"/films\" class=\"film-mark\" title=\"A Film tip\">Some "
			"<b>films</b></a><script src=\"/film.js\">film()</script>");

		g_assert_nonnull(strstr(seen, "A Film tip"));
		g_assert_nonnull(strstr(seen, "films"));
		g_assert_null(strstr(seen, "/films"));
		g_assert_null(strstr(seen, "film-mark"));
		g_assert_null(strstr(seen, "film()"));
	}

	g_assert_true(axis_storage_remove_root(fx->t->storage, fx->root_id, &error));
	g_assert_no_error(error);
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "cinema", AXIS_MEDIA_MOVIES);
	s = scene(fx);

	for (i = 0; lists[i] != NULL; i++) {
		reply = get_as(fx, fx->admin, lists[i]);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_says_movie(lists[i], reply.body);
		axis_test_reply_clear(&reply);
	}
	/* Each title's page, with its state, its releases and why (storage is
	 * read-only here, so each wanted one has its notice too). */
	{
		const gint64 movies[] = { s.downloading, s.attention, s.to_import, s.not_available, s.wanted };

		for (i = 0; i < G_N_ELEMENTS(movies); i++) {
			g_free(path);
			path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, movies[i]));
			reply = get_as(fx, fx->admin, path);
			g_assert_cmpuint(reply.status, ==, 200);
			if (movies[i] == s.wanted)
				assert_contains(reply.body, "id=\"ready-notice\"");
			if (movies[i] == s.attention)
				assert_contains(reply.body, "rejected them all");
			assert_says_movie(path, reply.body);
			axis_test_reply_clear(&reply);
			reply = get_as(fx, fx->viewer, path);
			assert_says_movie(path, reply.body);
			axis_test_reply_clear(&reply);
		}
	}
	/* A title not added yet: the form, and the line for anyone else. */
	g_free(path);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, s.unmanaged_title);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Add to Movies");
	assert_says_movie(path, reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	assert_says_movie(path, reply.body);
	axis_test_reply_clear(&reply);
	/* The dashboard, with what is still needed (imports may not write). */
	card = setup_card(fx, fx->admin);
	g_assert_nonnull(card);
	assert_says_movie("the Getting started card", card);
	reply = get_as(fx, fx->admin, "/");
	assert_contains(reply.body, "Movies added lately");
	assert_says_movie("the dashboard", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/");
	assert_says_movie("the dashboard of someone else", reply.body);
	axis_test_reply_clear(&reply);
}

/* ── Repeated clicks (add-activity-feedback) ────────────────────── */

static void
click(
	Fixture     *fx,
	const gchar *page,
	const gchar *action,
	const gchar *form,
	guint        n,
	GPtrArray   *locations
){
	guint i;

	for (i = 0; i < n; i++) {
		AxisTestReply reply = post_as(fx, fx->admin, page, action, form);
		const gchar *where = axis_test_reply_header(&reply, "Location");

		g_ptr_array_add(locations, g_strdup_printf("%u %s", reply.status, where != NULL ? where : "-"));
		axis_test_reply_clear(&reply);
	}
}

static gint64
search_jobs(
	Fixture     *fx,
	gint64       film
){
	g_autofree gchar *n = q(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search' AND payload->>'movie' = '%"
		G_GINT64_FORMAT "'", film);

	return g_ascii_strtoll(n, NULL, 10);
}

/* Every answer went back to @place, the first with @first, the others with
 * @then. */
static void
assert_answers(
	GPtrArray   *locations,
	const gchar *first,
	const gchar *then,
	const gchar *place
){
	g_autofree gchar *at = g_strdup_printf("&at=%s#%s", place, place);
	guint i;

	for (i = 0; i < locations->len; i++) {
		const gchar *location = g_ptr_array_index(locations, i);
		g_autofree gchar *done = g_strdup_printf("done=%s", i == 0 ? first : then);

		assert_contains(location, "303 ");
		assert_contains(location, done);
		g_assert_true(g_str_has_suffix(location, at));
	}
}

static AxisTestReply
follow(
	Fixture     *fx,
	const gchar *location
){
	g_autofree gchar *path = g_strdup(strchr(location, '/'));
	gchar *hash = strchr(path, '#');

	if (hash != NULL)
		*hash = '\0';
	return get_as(fx, fx->admin, path);
}

static void
assert_notice_at(
	const gchar *body,
	const gchar *id,
	const gchar *text
){
	g_autofree gchar *attribute = g_strdup_printf("id=\"%s\"", id);
	const gchar *element = strstr(body, attribute);
	const gchar *notice = strstr(body, "notice-here");

	if (element == NULL || notice == NULL || notice < element)
		g_error("expected the notice inside #%s in:\n%s", id, body);
	assert_contains(notice, text);
}

/*
 * Spec (movies): A click is answered by the work in hand.  Search now and
 * Manual search, five times quickly, queue one search each; the clicks after
 * the first say where it stands, back at the film, and its button says so;
 * one that ended a moment ago answers the click; the film's line says what
 * is happening and its page follows its topics.
 */
static void
test_repeated_clicks(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) locations = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *page = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *manual = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *actions = NULL;
	AxisTestReply reply;
	gint64 film;

	/* Paper Lantern: a made-up film. */
	set_route(fx, "/title/film/151", "{\"id\":\"151\",\"kind\":\"film\",\"title\":\"Paper Lantern\",\"year\":2019,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"2019-04-05\",\"ids\":{\"imdb\":\"tt0000151\"}}");
	film = add_film(fx, "151");
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	action = g_strdup_printf("/films/%" G_GINT64_FORMAT "/search", film);
	manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual", film);
	line = g_strdup_printf("id=\"activity-film-%" G_GINT64_FORMAT "\"", film);
	actions = g_strdup_printf("id=\"film-actions-%" G_GINT64_FORMAT "\"", film);

	click(fx, page, action, NULL, 5, locations);
	g_assert_cmpint(search_jobs(fx, film), ==, 1);
	assert_answers(locations, "searching", "already_searching", "film");
	reply = follow(fx, g_ptr_array_index(locations, 3));
	assert_notice_at(reply.body, "film", "Nothing new was queued: a search of the movie is waiting for its turn, "
		"queued");
	assert_contains(strstr(reply.body, line), "A search of the movie is waiting for its turn");
	assert_contains(strstr(reply.body, actions), ">Waiting for its turn</button>");
	assert_lacks(reply.body, ">Search for this movie</button>");
	axis_test_reply_clear(&reply);
	g_ptr_array_set_size(locations, 0);

	/* Its own job is read by its key, not by a prefix that other films' keys
	 * share ("movies.search:1" begins "movies.search:10"): 510 of theirs
	 * queued first no longer push it out of the read. */
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO job (kind, queue, state, max_attempts, dedupe_key)"
			" SELECT 'movies.search', 'other', 'queued', 1, 'movies.search:%" G_GINT64_FORMAT "' || n"
			" FROM generate_series(0, 509) n", film);

		axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
		axis_test_db_exec(fx->t->db, sql);
		click(fx, page, action, NULL, 1, locations);
		assert_contains(g_ptr_array_index(locations, 0), "done=searching");
		reply = get_as(fx, fx->admin, page);
		assert_contains(strstr(reply.body, actions), ">Waiting for its turn</button>");
		axis_test_reply_clear(&reply);
		g_ptr_array_set_size(locations, 0);
		axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE queue = 'other'");
	}
	/* One the scheduler queued, behind its backlog: the click raises it. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *before = NULL;
		g_autofree gchar *after = NULL;
		gboolean created = FALSE;

		axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
		g_assert_true(axis_movies_request_search(fx->movies, film, "scheduled", NULL, &created, &error));
		g_assert_no_error(error);
		before = q(fx, "SELECT priority FROM job WHERE kind = 'movies.search'");
		g_assert_cmpstr(before, ==, "0");
		click(fx, page, action, NULL, 1, locations);
		assert_contains(g_ptr_array_index(locations, 0), "done=already_searching");
		after = q(fx, "SELECT priority FROM job WHERE kind = 'movies.search'");
		g_assert_cmpstr(after, ==, "10");
		g_ptr_array_set_size(locations, 0);
	}

	/* Searched a moment ago: answered with what it found; a minute later,
	 * searched again. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	search(fx, film);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	click(fx, page, action, NULL, 5, locations);
	assert_answers(locations, "searched_recently", "searched_recently", "film");
	g_assert_cmpint(search_jobs(fx, film), ==, 0);
	reply = follow(fx, g_ptr_array_index(locations, 0));
	assert_notice_at(reply.body, "film", "Nothing new was queued: a search of the movie ended a moment ago and ");
	assert_contains(reply.body, "You can ask again from ");
	assert_contains(strstr(reply.body, line), "Searched a moment ago: it ");
	assert_contains(strstr(reply.body, actions), ">Searched a moment ago</button>");
	/* The block asks again by itself once the minute is over. */
	assert_contains(reply.body, ", load delay:");
	axis_test_reply_clear(&reply);
	g_ptr_array_set_size(locations, 0);
	axis_test_db_exec(fx->t->db, "UPDATE movie_search SET finished_at = finished_at - interval '2 minutes'");
	click(fx, page, action, NULL, 1, locations);
	assert_contains(g_ptr_array_index(locations, 0), "done=searching");
	g_assert_cmpint(search_jobs(fx, film), ==, 1);
	g_ptr_array_set_size(locations, 0);

	/* Manual search, five times: one, its own; the film's page leads to it. */
	click(fx, page, manual, "level=movie", 5, locations);
	assert_answers(locations, "searching", "already_searching", "manual-results");
	g_assert_cmpint(search_jobs(fx, film), ==, 2);
	reply = follow(fx, g_ptr_array_index(locations, 1));
	assert_notice_at(reply.body, "manual-results", "Nothing new was asked for: a manual search of the movie is waiting "
		"for its turn");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page);
	assert_contains(strstr(reply.body, actions), "Manual search in hand: see it");
	/* The page follows the film's searches and downloads. */
	{
		g_autofree gchar *topic = g_strdup_printf("topic=act:movies.search:%" G_GINT64_FORMAT, film);
		g_autofree gchar *title = g_strdup_printf("topic=act:title:%" G_GINT64_FORMAT, title_id_of(fx, film));

		assert_contains(reply.body, topic);
		assert_contains(reply.body, title);
	}
	axis_test_reply_clear(&reply);
	/* Its state block, as the hub has it fetched: the buttons out of band. */
	{
		g_autofree gchar *state = g_strdup_printf("/films/%" G_GINT64_FORMAT "/state", film);

		reply = get_as(fx, fx->admin, state);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, line);
		assert_contains(reply.body, "hx-swap-oob=\"true\"");
		assert_contains(reply.body, actions);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->viewer, state);
		assert_lacks(reply.body, actions);
		axis_test_reply_clear(&reply);
	}
	/* The Activity page names them. */
	reply = get_as(fx, fx->viewer, "/activity");
	assert_contains(reply.body, "Searching for Paper Lantern (2019)");
	assert_contains(reply.body, "Manual search of Paper Lantern (2019)");
	axis_test_reply_clear(&reply);

	/* Spec (web-shell): Add from the search results comes back to the
	 * results, at the card, with the notice there naming it; its title links
	 * to its page with the results to come back to. */
	set_route(fx, "/search/film", "{\"results\":[{\"id\":\"152\",\"kind\":\"film\",\"title\":\"Night Kite\","
		"\"year\":2020}]}");
	reply = get_as(fx, fx->admin, "/search?q=kite&kind=film");
	{
		const gchar *card = strstr(reply.body, "id=\"title-");
		g_autofree gchar *place = NULL;
		g_autofree gchar *link = NULL;
		g_autofree gchar *location = NULL;
		g_autofree gchar *form = NULL;
		gint64 kite;
		AxisTestReply added;

		const gchar *profile;
		const gchar *root;

		g_assert_nonnull(card);
		kite = g_ascii_strtoll(card + strlen("id=\"title-"), NULL, 10);
		place = g_strdup_printf("title-%" G_GINT64_FORMAT, kite);
		link = g_strdup_printf("href=\"/title/%" G_GINT64_FORMAT "?from=%%2Fsearch%%3Fq%%3Dkite%%26kind%%3Dfilm\"", kite);
		assert_contains(reply.body, link);
		/* Its Add, as on a Discover card: the card's defaults, back here. */
		assert_contains(card, "name=\"back\" value=\"/search?q=kite&amp;kind=film\"");
		profile = strstr(card, "name=\"profile\" value=\"");
		root = strstr(card, "name=\"root\" value=\"");
		g_assert_nonnull(profile);
		g_assert_nonnull(root);
		form = g_strdup_printf("title=%" G_GINT64_FORMAT "&profile=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT
			"&availability=released&monitored=yes&search=yes&back=%%2Fsearch%%3Fq%%3Dkite%%26kind%%3Dfilm&at=%s", kite,
			g_ascii_strtoll(profile + strlen("name=\"profile\" value=\""), NULL, 10),
			g_ascii_strtoll(root + strlen("name=\"root\" value=\""), NULL, 10), place);
		axis_test_reply_clear(&reply);
		added = post_as(fx, fx->admin, "/search?q=kite&kind=film", "/films", form);
		g_assert_cmpuint(added.status, ==, 303);
		location = g_strdup_printf("/search?q=kite&kind=film&done=added&at=%s#%s", place, place);
		g_assert_cmpstr(axis_test_reply_header(&added, "Location"), ==, location);
		axis_test_reply_clear(&added);
		reply = follow(fx, location);
		assert_notice_at(reply.body, place, "Added Night Kite (2020) to Movies.");
		assert_contains(strstr(reply.body, place), "film-mark");
		axis_test_reply_clear(&reply);
	}
}

/* ── Links by hand (add-force-grab-and-assign, slice C2) ─────────── */

static gint64
qi(
	Fixture     *fx,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

static gint64
qi(
	Fixture     *fx,
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
	return value != NULL ? g_ascii_strtoll(value, NULL, 10) : 0;
}

/* A video file at @rel in the movies' library folder, as a scan indexes it;
 * linked to @film when that is not 0 (as identification links one). */
static gint64
links_file(
	Fixture     *fx,
	const gchar *rel,
	gint64       film
){
	static gint64 inode = 4200000;
	gint64 id = qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", '%s', 'video',"
		" 2147483648, 1, 0, 1, 0, %" G_GINT64_FORMAT ", 1, false, false, false, 1, 1, 1) RETURNING id", fx->root_id,
		fx->volume_id, rel, ++inode);

	g_assert_cmpint(id, >, 0);
	if (film > 0) {
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO movie_file (file_id, movie_id, quality) VALUES (%"
			G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", 'webdl_720p')", id, film);

		axis_test_db_exec(fx->t->db, sql);
	}
	return id;
}

/* The files linked to @film, by their ids: "12:linked:true,14:-:false". */
static gchar *
links_of(
	Fixture *fx,
	gint64   film
){
	g_autofree gchar *found = q(fx, "SELECT string_agg(file_id || ':' || coalesce(hand_how, '-') || ':' || kept::text, ','"
		" ORDER BY file_id) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, film);

	return g_strdup(found != NULL ? found : "");
}

static void
assert_links(
	Fixture     *fx,
	gint64       film,
	const gchar *format,
	...
) G_GNUC_PRINTF(3, 4);

static void
assert_links(
	Fixture     *fx,
	gint64       film,
	const gchar *format,
	...
){
	g_autofree gchar *expected = NULL;
	g_autofree gchar *links = links_of(fx, film);
	va_list ap;

	va_start(ap, format);
	expected = g_strdup_vprintf(format, ap);
	va_end(ap);
	g_assert_cmpstr(links, ==, expected);
}

/* The review of @plan ("to_12=yes&unlink_13=yes", with its ticks). */
static AxisTestReply
links_review(
	Fixture     *fx,
	gint64       film,
	const gchar *plan
){
	g_autofree gchar *path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", film);
	g_autofree gchar *form = g_strdup_printf("step=review&%s", plan);

	return post_as(fx, fx->admin, path, path, form);
}

/* @plan (with its ticks) applied with the digest of its review @body. */
static AxisTestReply
links_apply(
	Fixture     *fx,
	gint64       film,
	const gchar *plan,
	const gchar *body
){
	g_autofree gchar *path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", film);
	g_autofree gchar *digest = digest_of(body);
	g_autofree gchar *form = g_strdup_printf("step=apply&digest=%s&%s", digest, plan);

	return post_as(fx, fx->admin, path, path, form);
}

/*
 * Slice C2.  Spec (movies): A file identification does not match; The movie
 * already has a file (without the tick, and with it, for a file with no link
 * and for one of another movie); Unlinked; and out of date, an open import,
 * a viewer, a post with no token, a file that is no library file, "Let Axis
 * manage this file again".  Lantern Quay (2014) has one file; Copper Wren
 * (2011) and Salt Meridian (2008) have one each; one video lies unmatched in
 * another folder.
 */
static void
test_links(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *title_path = NULL;
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *manage_path = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *shown = NULL;
	g_autofree gchar *disk = NULL;
	g_autofree gchar *disk_after = NULL;
	AxisTestReply reply;
	AxisTestReply review;
	gint64 lantern;
	gint64 wren;
	gint64 salt;
	gint64 a;
	gint64 b;
	gint64 c;
	gint64 d;
	gint64 e;

	made_up(fx, "171", "Lantern Quay", 2014);
	made_up(fx, "172", "Copper Wren", 2011);
	made_up(fx, "173", "Salt Meridian", 2008);
	lantern = add_film_as(fx, "171", "space_saver", TRUE);
	wren = add_film_as(fx, "172", "space_saver", TRUE);
	salt = add_film_as(fx, "173", "space_saver", FALSE);
	a = links_file(fx, "Lantern Quay (2014)/Lantern Quay (2014).mkv", lantern);
	b = links_file(fx, "Lantern Quay (2014)/Lantern.Quay.2014.Extended.mkv", 0);
	c = links_file(fx, "unsorted/lq-final-cut.mkv", 0);
	d = links_file(fx, "Copper Wren (2011)/Copper Wren (2011).mkv", wren);
	e = links_file(fx, "Salt Meridian (2008)/Salt Meridian (2008).mkv", salt);
	title_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, lantern));
	links_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links", lantern);
	manage_path = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links/manage", lantern);
	disk = q(fx, "SELECT string_agg(id || ':' || rel_path || ':' || state || ':' || inode, ',' ORDER BY id)"
		" FROM library_file");

	/* A viewer sees nothing new and changes nothing. */
	reply = get_as(fx, fx->viewer, title_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Files and links");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, links_path);
	g_assert_cmpuint(reply.status, !=, 200);
	assert_lacks(reply.body, "Lantern Quay (2014).mkv");
	axis_test_reply_clear(&reply);
	plan = g_strdup_printf("step=review&unlink_%" G_GINT64_FORMAT "=yes", a);
	reply = post_as(fx, fx->viewer, title_path, links_path, plan);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, a);
	reply = post_as(fx, fx->viewer, title_path, manage_path, form);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	/* An administrator's post with no token is refused too. */
	reply = post_as(fx, fx->admin, NULL, links_path, plan);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&plan, g_free);

	/* The page: the linked file, the folder's file with no link; a file from
	 * elsewhere only when it is asked for. */
	reply = get_as(fx, fx->admin, title_path);
	assert_contains(reply.body, "Files and links");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, links_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_csp_clean(reply.body);
	assert_contains(reply.body, "Linked files");
	assert_contains(reply.body, "Lantern Quay (2014)/Lantern Quay (2014).mkv");
	assert_contains(reply.body, "Files with no link to this movie");
	assert_contains(reply.body, "Lantern.Quay.2014.Extended.mkv");
	assert_contains(reply.body, "A file from elsewhere in the library");
	assert_contains(reply.body, "no file on disk is moved, renamed or deleted. A media server reads a file");
	assert_lacks(reply.body, "lq-final-cut.mkv");
	assert_lacks(reply.body, "Copper Wren");
	assert_lacks(reply.body, "Let Axis manage this file again");
	axis_test_reply_clear(&reply);
	shown = g_strdup_printf("%s?file=%" G_GINT64_FORMAT, links_path, c);
	reply = get_as(fx, fx->admin, shown);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "unsorted/lq-final-cut.mkv");
	assert_contains(reply.body, "Link to this movie");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&shown, g_free);
	shown = g_strdup_printf("%s?file=%" G_GINT64_FORMAT, links_path, d);
	reply = get_as(fx, fx->admin, shown);
	assert_contains(reply.body, "Linked to Copper Wren (2011)");
	assert_contains(reply.body, "Link to this movie (it leaves Copper Wren (2011))");
	axis_test_reply_clear(&reply);

	g_clear_pointer(&shown, g_free);
	shown = g_strdup_printf("%s?file=999999", links_path);
	reply = get_as(fx, fx->admin, shown);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "File 999999 is not a video file that is present in a library folder for movies");
	axis_test_reply_clear(&reply);

	/* A file that is no library file, and a link that is not there, are refused. */
	review = links_review(fx, lantern, "to_999999=yes");
	g_assert_cmpuint(review.status, ==, 400);
	axis_test_reply_clear(&review);
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", b);
	review = links_review(fx, lantern, plan);
	g_assert_cmpuint(review.status, ==, 400);
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);

	/* Scenarios: A file identification does not match; The movie already has
	 * a file, without the tick.  The review writes nothing; both stay linked. */
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=yes", c);
	review = links_review(fx, lantern, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_csp_clean(review.body);
	assert_contains(review.body, "Lantern Quay (2014) has a file already. After this it has 2 files linked: nothing is "
		"replaced, and nothing is unlinked unless you tick it below.");
	{
		g_autofree gchar *tick = g_strdup_printf("name=\"other_%" G_GINT64_FORMAT "\"", a);

		assert_contains(review.body, tick);
	}
	assert_contains(review.body, "Unlink Lantern Quay (2014)/Lantern Quay (2014).mkv from Lantern Quay (2014) instead.");
	assert_links(fx, lantern, "%" G_GINT64_FORMAT ":-:false", a);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE kind = 'hand_link'"), ==, 0);
	reply = links_apply(fx, lantern, plan, review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=linked");
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	assert_links(fx, lantern, "%" G_GINT64_FORMAT ":-:false,%" G_GINT64_FORMAT ":linked:true", a, c);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_by = 'root'"
		" AND hand_at IS NOT NULL AND identity_id IS NULL", c), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE kind = 'hand_link' AND by_user = 'root' AND movie_id = %"
		G_GINT64_FORMAT " AND text LIKE '%%lq-final-cut.mkv was linked to Lantern Quay (2014) by hand by root.%%now has 2"
		" files linked%%The file on disk was not touched.'", lantern), ==, 1);
	g_clear_pointer(&shown, g_free);
	shown = g_strdup_printf("%s?done=linked", links_path);
	reply = get_as(fx, fx->admin, shown);
	assert_contains(reply.body, "The links were changed. No file on disk was touched.");
	assert_contains(reply.body, "Lantern Quay (2014) has 2 files linked");
	assert_contains(reply.body, "Linked by hand by root");
	assert_contains(reply.body, "Let Axis manage this file again");
	axis_test_reply_clear(&reply);
	/* The movie's page says who linked it, and that it is kept. */
	reply = get_as(fx, fx->admin, title_path);
	assert_contains(reply.body, "Linked by hand by root.");
	assert_contains(reply.body, "Kept by hand");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&plan, g_free);

	/* Out of date: the links changed after the review.  Nothing is done, the
	 * review is shown again (409); confirmed again, it is applied.  Scenario:
	 * Unlinked. */
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", c);
	review = links_review(fx, lantern, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "No movie: unlinked, and left alone by identification");
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE movie_file SET kept = true WHERE file_id = %" G_GINT64_FORMAT, a);

		axis_test_db_exec(fx->t->db, sql);
	}
	reply = links_apply(fx, lantern, plan, review.body);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "The movie changed after this review was made");
	axis_test_reply_clear(&review);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, lantern), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink"), ==, 0);
	review = reply;
	reply = links_apply(fx, lantern, plan, review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	assert_links(fx, lantern, "%" G_GINT64_FORMAT ":-:true", a);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT " AND module ="
		" 'movies' AND owner_id = %" G_GINT64_FORMAT " AND by_user = 'root'", c, lantern), ==, 1);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "unsorted/lq-final-cut.mkv");
	assert_contains(reply.body, "Unlinked by hand by root");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE movie_file SET kept = false WHERE file_id = %" G_GINT64_FORMAT, a);

		axis_test_db_exec(fx->t->db, sql);
	}
	g_clear_pointer(&plan, g_free);

	/* A file of another movie, without the tick: it leaves that movie (the
	 * review says so, and that the movie is then wanted), and this movie's
	 * present file stays linked beside it. */
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=yes", d);
	review = links_review(fx, lantern, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "It is linked to Copper Wren (2011) now, and leaves it.");
	assert_contains(review.body, "Left without a file: it is monitored, so it is wanted and searched for again");
	assert_contains(review.body, "has a file already. After this it has 2 files linked");
	assert_links(fx, wren, "%" G_GINT64_FORMAT ":-:false", d);
	reply = links_apply(fx, lantern, plan, review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	assert_links(fx, lantern, "%" G_GINT64_FORMAT ":-:false,%" G_GINT64_FORMAT ":linked:true", a, d);
	assert_links(fx, wren, "%s", "");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE kind = 'hand_link' AND movie_id = %" G_GINT64_FORMAT
		" AND text LIKE '%%was unlinked from Copper Wren (2011) by hand by root: it is linked to Lantern Quay (2014) now.%%"
		"it is wanted again.'", wren), ==, 1);
	g_clear_pointer(&plan, g_free);

	/* With the tick, for the one file ticked: that file is unlinked and
	 * remembered; the other present file, not ticked, stays.  The tick is
	 * part of the review: applied without the review having shown it, it is
	 * out of date. */
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=yes", e);
	review = links_review(fx, lantern, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "Left without a file: it is not monitored, so it is not searched for.");
	assert_lacks(review.body, "You ticked this file");
	form = g_strdup_printf("%s&other_%" G_GINT64_FORMAT "=yes", plan, a);
	reply = links_apply(fx, lantern, form, review.body);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	assert_links(fx, salt, "%" G_GINT64_FORMAT ":-:false", e);
	review = links_review(fx, lantern, form);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "You ticked this file: it is unlinked from this movie");
	reply = links_apply(fx, lantern, form, review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	assert_links(fx, lantern, "%" G_GINT64_FORMAT ":linked:true,%" G_GINT64_FORMAT ":linked:true", d, e);
	assert_links(fx, salt, "%s", "");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, a), ==, 1);
	g_clear_pointer(&plan, g_free);
	g_clear_pointer(&form, g_free);

	/* While an import of the movie is open, no link is changed. */
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO import (module, owner_id, title_label, source_path, state)"
			" VALUES ('movies', %" G_GINT64_FORMAT ", 'Lantern Quay (2014)', 'downloads/lantern', 'waiting')", lantern);

		axis_test_db_exec(fx->t->db, sql);
	}
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", d);
	review = links_review(fx, lantern, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "No link can be changed now. An import of this movie");
	assert_lacks(review.body, ">Change the links</button>");
	axis_test_reply_clear(&review);
	axis_test_db_exec(fx->t->db, "DELETE FROM import");
	/* ... and one that opens after the review refuses the confirmation. */
	review = links_review(fx, lantern, plan);
	assert_contains(review.body, ">Change the links</button>");
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO import (module, owner_id, title_label, source_path, state)"
			" VALUES ('movies', %" G_GINT64_FORMAT ", 'Lantern Quay (2014)', 'downloads/lantern', 'waiting')", lantern);

		axis_test_db_exec(fx->t->db, sql);
	}
	reply = links_apply(fx, lantern, plan, review.body);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	axis_test_db_exec(fx->t->db, "DELETE FROM import");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_file WHERE movie_id = %" G_GINT64_FORMAT, lantern), ==, 2);
	g_clear_pointer(&plan, g_free);

	/* Let Axis manage this file again.  The file unlinked by hand: reviewed
	 * (nothing written), then the mark goes, with a stale digest refused. */
	form = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, c);
	review = post_as(fx, fx->admin, links_path, manage_path, form);
	g_assert_cmpuint(review.status, ==, 200);
	assert_csp_clean(review.body);
	assert_contains(review.body, "unsorted/lq-final-cut.mkv is no longer left alone.");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, c), ==, 1);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("step=apply&file=%" G_GINT64_FORMAT "&digest=0123456789abcdef0123456789abcdef", c);
	reply = post_as(fx, fx->admin, links_path, manage_path, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, c), ==, 1);
	g_clear_pointer(&form, g_free);
	{
		g_autofree gchar *digest = digest_of(review.body);

		form = g_strdup_printf("step=apply&file=%" G_GINT64_FORMAT "&digest=%s", c, digest);
	}
	axis_test_reply_clear(&review);
	reply = post_as(fx, fx->admin, links_path, manage_path, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=managed");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, c), ==, 0);
	g_clear_pointer(&form, g_free);
	/* A file a person linked that identification does not hold as this movie:
	 * there is nothing to hand back, and nothing can be confirmed. */
	form = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, e);
	review = post_as(fx, fx->admin, links_path, manage_path, form);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "so there is nothing to hand back");
	assert_lacks(review.body, ">Let Axis manage this file</button>");
	axis_test_reply_clear(&review);
	g_clear_pointer(&form, g_free);
	/* A file that is no one's by hand cannot be handed back. */
	form = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, b);
	review = post_as(fx, fx->admin, links_path, manage_path, form);
	g_assert_cmpuint(review.status, ==, 404);
	axis_test_reply_clear(&review);
	assert_links(fx, lantern, "%" G_GINT64_FORMAT ":linked:true,%" G_GINT64_FORMAT ":linked:true", d, e);

	/* No file was touched by any of it. */
	disk_after = q(fx, "SELECT string_agg(id || ':' || rel_path || ':' || state || ':' || inode, ',' ORDER BY id)"
		" FROM library_file");
	g_assert_cmpstr(disk_after, ==, disk);
}

/* GET /library/file/:id/link: from a file to the movie's links page. */
static void
test_file_link(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *search = NULL;
	g_autofree gchar *want = NULL;
	g_autofree gchar *sound_path = NULL;
	g_autofree gchar *rows = NULL;
	AxisTestReply reply;
	gint64 lantern;
	gint64 wren;
	gint64 file;
	gint64 sound;

	made_up(fx, "181", "Harbour Kite", 2012);
	made_up(fx, "182", "Copper Heron", 2016);
	lantern = add_film_as(fx, "181", "space_saver", TRUE);
	wren = add_film_as(fx, "182", "space_saver", TRUE);
	file = links_file(fx, "unsorted/hk-final-cut.mkv", 0);
	sound = links_file(fx, "unsorted/hk-score.flac", 0);
	rows = g_strdup_printf("UPDATE library_file SET kind = 'audio' WHERE id = %" G_GINT64_FORMAT, sound);
	axis_test_db_exec(fx->t->db, rows);
	path = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/link", file);

	/* The file, and a form: nothing is searched until it is asked. */
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "hk-final-cut.mkv");
	assert_contains(reply.body, "unsorted");
	assert_contains(reply.body, "GiB</dd>");
	assert_contains(reply.body, "Which movie is it?");
	assert_contains(reply.body, "file-link-q");
	assert_lacks(reply.body, "file-link-results");
	axis_test_reply_clear(&reply);

	/* The search: results are links to the movie's links page for this file. */
	search = g_strdup_printf("%s?q=harbour", path);
	reply = get_as(fx, fx->admin, search);
	g_assert_cmpuint(reply.status, ==, 200);
	want = g_strdup_printf("href=\"/films/%" G_GINT64_FORMAT "/links?file=%" G_GINT64_FORMAT "\"", lantern, file);
	assert_contains(reply.body, want);
	assert_contains(reply.body, "Harbour Kite (2012)");
	assert_lacks(reply.body, "Copper Heron");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&search, g_free);
	search = g_strdup_printf("%s?q=HERON", path);
	reply = get_as(fx, fx->admin, search);
	g_assert_cmpuint(reply.status, ==, 200);
	g_clear_pointer(&want, g_free);
	want = g_strdup_printf("href=\"/films/%" G_GINT64_FORMAT "/links?file=%" G_GINT64_FORMAT "\"", wren, file);
	assert_contains(reply.body, want);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&search, g_free);
	search = g_strdup_printf("%s?q=nothing-like-it", path);
	reply = get_as(fx, fx->admin, search);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "manages no movie with that in its name");
	axis_test_reply_clear(&reply);

	/* The hand-over address lands on the movie's links page, which lists the file. */
	g_free(rows);
	rows = g_strdup_printf("/films/%" G_GINT64_FORMAT "/links?file=%" G_GINT64_FORMAT, lantern, file);
	reply = get_as(fx, fx->admin, rows);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "hk-final-cut.mkv");
	axis_test_reply_clear(&reply);

	/* Not a present video file: a sentence, no form. */
	sound_path = g_strdup_printf("/library/file/%" G_GINT64_FORMAT "/link", sound);
	reply = get_as(fx, fx->admin, sound_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Only a video file that is present");
	assert_lacks(reply.body, "file-link-q");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/library/file/999999/link");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* A viewer is refused. */
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, !=, 200);
	assert_lacks(reply.body, "hk-final-cut.mkv");
	axis_test_reply_clear(&reply);

}

/* ── add-block-release ──────────────────────────────────────────── */

#define HEARTH_BLURAY "Hearth.1995.1080p.BluRay.x264-GRP"
#define HEARTH_WEB    "Hearth.1995.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define ROKAN_BLURAY  "Rokan.1998.1080p.BluRay.x264-GRP"

/*
 * add-block-release.  Spec (movies): "An administrator can block a release",
 * scenarios "From a manual search", "A download that is not wanted", "A
 * release that is in the library already", "One release taken off", "A
 * viewer".
 *
 * Block this release on a manual search's row puts the release on the
 * movie's blocklist by the person, the row says who, and a scheduled search
 * takes the next release instead.  Block and search again on the download of
 * that one shows a confirmation that writes nothing, then gives up on the
 * download through the failure path (the grab closed, the download removed
 * from its client, a search queued).  For a release that is in the library
 * it only blocks: the file, its link, the download and the jobs stay as they
 * were.  Unblock takes one entry off.  A viewer, and a post without its
 * token, are refused everywhere.
 */
static void
test_block_release(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ HEARTH_BLURAY, 14000000000, 600001 },
		{ HEARTH_WEB, 6000000000, 600001 }
	};
	static const Rel rokan[] = {
		{ ROKAN_BLURAY, 12000000000, 600002 }
	};
	g_autofree gchar *page = NULL;
	g_autofree gchar *manual = NULL;
	g_autofree gchar *block = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *best = NULL;
	g_autofree gchar *row = NULL;
	g_autofree gchar *landed = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *file = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	const gchar *other;
	gboolean created = FALSE;
	gint64 film;
	gint64 second;
	gint64 candidate;
	gint64 grab;
	gint64 download;
	gint64 import;
	gint64 library_file;
	gint64 jobs;

	film = add_film(fx, "107");
	set_releases(fx, found, G_N_ELEMENTS(found));
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, film));
	manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual?level=movie", film);
	block = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/block", film);
	g_assert_true(axis_movies_request_manual_search(fx->movies, film, "root", &created, &error));
	g_assert_no_error(error);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	/* Both are acceptable; the one a search would take is the one blocked. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_candidate c JOIN movie_search s ON s.id = c.search_id"
		" WHERE s.cause = 'list' AND c.accepted"), ==, 2);
	best = q(fx, "SELECT c.name FROM movie_candidate c JOIN movie_search s ON s.id = c.search_id"
		" WHERE s.cause = 'list' AND c.accepted ORDER BY c.rank LIMIT 1");
	g_assert_nonnull(best);
	other = strcmp(best, HEARTH_BLURAY) == 0 ? HEARTH_WEB : HEARTH_BLURAY;
	candidate = listed_candidate(fx, best);
	row = g_strdup_printf("manual-release-%" G_GINT64_FORMAT, candidate);

	/* 1. Block this release: on each row for an administrator alone. */
	reply = get_as(fx, fx->admin, manual);
	g_assert_cmpuint(count_of(reply.body, ">Block this release</button>"), ==, 2);
	assert_contains(reply.body, row);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, manual);
	assert_lacks(reply.body, "Block this release");
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie", candidate);
	reply = post_as(fx, fx->viewer, manual, block, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, block, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_blocklist"), ==, 0);
	reply = post_as(fx, fx->admin, manual, block, "level=movie");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, manual, block, "candidate=999999&level=movie");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, manual, block, form);
	g_assert_cmpuint(reply.status, ==, 303);
	/* Back at its row. */
	landed = g_strdup_printf("manual?level=movie&done=blocked&at=%s#%s", row, row);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	/* By whom and when; nothing grabbed. */
	n = q(fx, "SELECT reason || '|' || created_by || '|' || (created_at > now() - interval '1 minute')::text || '|' ||"
		" release_name FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_free(landed);
	landed = g_strdup_printf("blocked|root|true|%s", best);
	g_assert_cmpstr(n, ==, landed);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind ="
		" 'blocklisted' AND by_user = 'root' AND text LIKE '%%blocked by root from a manual search%%'", film), ==, 1);
	/* The row says so from then on, with the way to the blocklist. */
	g_free(landed);
	landed = g_strdup_printf("%s&done=blocked&at=%s", manual, row);
	reply = get_as(fx, fx->admin, landed);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "On the blocklist (blocked by root): take it off the blocklist to grab it.");
	assert_contains(reply.body, "Blocked: this release is on the blocklist now");
	assert_contains(reply.body, ">See the blocklist</a>");
	assert_contains(reply.body, "#blocklist\"");
	assert_contains(reply.body, "Ruled out (1)");
	g_assert_cmpuint(count_of(reply.body, ">Block this release</button>"), ==, 1);
	g_assert_cmpuint(count_of(reply.body, ">Grab this one</button>"), ==, 1);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, manual);
	assert_contains(reply.body, "On the blocklist (blocked by root)");
	axis_test_reply_clear(&reply);
	/* Blocked twice: nothing more. */
	reply = post_as(fx, fx->admin, manual, block, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_blocklist"), ==, 1);
	/* Grabbing it by hand is refused, with who blocked it. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab?candidate=%" G_GINT64_FORMAT "&level=movie", film,
		candidate);
	reply = get_as(fx, fx->admin, form);
	assert_contains(reply.body, "It is on the blocklist: blocked by root. Take it off the blocklist first to grab it.");
	axis_test_reply_clear(&reply);
	/* Its origin on the movie's blocklist, with Unblock. */
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "Blocked by root");
	assert_contains(reply.body, ">Unblock</button>");
	axis_test_reply_clear(&reply);

	/* 2. A scheduled search does not take it: the next one instead. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	g_free(n);
	n = q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", film);
	g_assert_cmpstr(n, ==, other);
	g_free(n);
	n = q(fx, "SELECT c.blocklisted::text || '|' || (c.reasons::text LIKE '%%blocked by root%%')::text"
		" FROM movie_candidate c JOIN movie_search s ON s.id = c.search_id WHERE s.cause = 'scheduled' AND c.name = '%s'",
		best);
	g_assert_cmpstr(n, ==, "true|true");

	/* 3. Block and search again, on the download of the one it took. */
	grab = qi(fx, "SELECT id FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", film);
	download = qi(fx, "SELECT download_id FROM movie_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_assert_cmpint(download, >, 0);
	confirm = g_strdup_printf("/films/block-release?download=%" G_GINT64_FORMAT, download);
	reply = get_as(fx, fx->admin, "/downloads");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "/films/block-release?download=");
	assert_contains(reply.body, ">Block and search again</a>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/downloads");
	assert_lacks(reply.body, "block-release");
	axis_test_reply_clear(&reply);
	/* The confirmation: an administrator's, and it writes nothing. */
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Block this release and search again?");
	assert_contains(reply.body, other);
	assert_contains(reply.body, "gives up on the download");
	assert_contains(reply.body, ">Block and search again</button>");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_blocklist"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab WHERE closed_at IS NULL"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 0);
	reply = get_as(fx, fx->admin, "/films/block-release?download=999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/films/block-release");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	/* The post: confirmed, an administrator's, with its token. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT, film, grab);
	reply = post_as(fx, fx->admin, confirm, "/films/block-release", form);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes", film, grab);
	reply = post_as(fx, fx->viewer, manual, "/films/block-release", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, "/films/block-release", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab WHERE closed_at IS NULL"), ==, 1);
	/* Another movie's number with this grab: refused. */
	{
		g_autofree gchar *wrong = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes",
			film + 1000, grab);

		reply = post_as(fx, fx->admin, confirm, "/films/block-release", wrong);
		g_assert_cmpuint(reply.status, ==, 400);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab WHERE closed_at IS NULL"), ==, 1);
	reply = post_as(fx, fx->admin, confirm, "/films/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_free(landed);
	landed = g_strdup_printf("/films/block-release?grab=%" G_GINT64_FORMAT "&done=cancelled", grab);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	/* The failure path's work: the grab closed as given up on, the release
	 * on the blocklist by the person, the download removed from its client,
	 * and one search queued for the next best. */
	g_free(n);
	n = q(fx, "SELECT close_reason || '|' || coalesce(removal, '-') FROM movie_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_assert_cmpstr(n, ==, "given_up|done");
	g_free(n);
	n = q(fx, "SELECT reason || '|' || created_by || '|' || grab_id FROM movie_blocklist WHERE release_name = '%s'",
		other);
	g_free(row);
	row = g_strdup_printf("blocked|root|%" G_GINT64_FORMAT, grab);
	g_assert_cmpstr(n, ==, row);
	g_free(n);
	n = q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	g_assert_cmpstr(n, ==, "removed");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search' AND payload->>'cause' = 'retry'"
		" AND state = 'queued'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind ="
		" 'abandoned' AND by_user = 'root' AND text LIKE '%%Blocked by root%%blocklist now%%'", film), ==, 1);
	reply = get_as(fx, fx->admin, landed);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis gave up on its download");
	assert_contains(reply.body, "This release is on the blocklist of Hearth (1995)");
	assert_lacks(reply.body, ">Block and search again</button>");
	assert_lacks(reply.body, ">Block this release</button>");
	axis_test_reply_clear(&reply);

	/* 4. Unblock: one entry, the other stays. */
	{
		g_autofree gchar *clear = g_strdup_printf("/films/%" G_GINT64_FORMAT "/blocklist/clear", film);
		g_autofree gchar *entry = g_strdup_printf("entry=%" G_GINT64_FORMAT, qi(fx, "SELECT id FROM movie_blocklist"
			" WHERE release_name = '%s'", best));

		reply = post_as(fx, fx->viewer, page, clear, entry);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, page, clear, entry);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
	g_free(n);
	n = q(fx, "SELECT string_agg(release_name, ',') FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, film);
	g_assert_cmpstr(n, ==, other);
	/* The manual search's row is as the blocklist is now: it can be grabbed
	 * again, and blocked again. */
	reply = get_as(fx, fx->admin, manual);
	g_assert_cmpuint(count_of(reply.body, "On the blocklist (blocked by root)"), ==, 1);
	assert_contains(reply.body, "Acceptable under the profile (1)");
	g_assert_cmpuint(count_of(reply.body, ">Grab this one</button>"), ==, 1);
	g_assert_cmpuint(count_of(reply.body, ">Block this release</button>"), ==, 1);
	axis_test_reply_clear(&reply);

	/* 5. A release that is in the library already: blocked for the future,
	 * and nothing else. */
	second = add_film(fx, "108");
	set_releases(fx, rokan, G_N_ELEMENTS(rokan));
	search(fx, second);
	grab = qi(fx, "SELECT id FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", second);
	download = qi(fx, "SELECT download_id FROM movie_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_assert_cmpint(download, >, 0);
	dir = g_build_filename(fx->disk, "films", "Rokan (1998)", NULL);
	file = g_build_filename(dir, "Rokan (1998).mkv", NULL);
	axis_test_storage_mkdir(dir);
	axis_test_storage_write(file, "film");
	import = qi(fx, "INSERT INTO import (module, owner_id, title_label, source_path, state, download_id, finished_at)"
		" VALUES ('movies', %" G_GINT64_FORMAT ", 'Rokan (1998)', 'downloads/rokan', 'imported', %" G_GINT64_FORMAT
		", now()) RETURNING id", second, download);
	g_assert_cmpint(import, >, 0);
	library_file = links_file(fx, "Rokan (1998)/Rokan (1998).mkv", second);
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE movie_file SET import_id = %" G_GINT64_FORMAT " WHERE file_id = %"
			G_GINT64_FORMAT, import, library_file);
		g_autofree gchar *close = g_strdup_printf("UPDATE movie_grab SET completed_at = now(), closed_at = now(),"
			" close_reason = 'imported' WHERE id = %" G_GINT64_FORMAT, grab);

		axis_test_db_exec(fx->t->db, sql);
		axis_test_db_exec(fx->t->db, close);
	}
	jobs = qi(fx, "SELECT count(*) FROM job");
	g_free(n);
	n = q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	/* Offered on the file's row of the movie's page, to an administrator. */
	g_free(page);
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_id_of(fx, second));
	g_free(confirm);
	confirm = g_strdup_printf("/films/block-release?import=%" G_GINT64_FORMAT, import);
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "/films/block-release?import=");
	assert_contains(reply.body, ">Block this release</a>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	assert_lacks(reply.body, "block-release");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "This release is in the library already.");
	assert_contains(reply.body, "only puts the release on the blocklist");
	assert_contains(reply.body, "no file in the library and no link is touched");
	assert_contains(reply.body, ">Block this release</button>");
	assert_lacks(reply.body, "Block and search again");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, second), ==, 0);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes", second, grab);
	reply = post_as(fx, fx->admin, confirm, "/films/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_free(landed);
	landed = g_strdup_printf("/films/block-release?grab=%" G_GINT64_FORMAT "&done=blocked", grab);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	/* Blocked by the person... */
	g_free(row);
	row = q(fx, "SELECT reason || '|' || created_by || '|' || release_name FROM movie_blocklist WHERE movie_id = %"
		G_GINT64_FORMAT, second);
	g_assert_cmpstr(row, ==, "blocked|root|" ROKAN_BLURAY);
	/* ... and nothing else: the file on disk, its row, its link, the import,
	 * the grab, the download and the jobs are as they were. */
	g_assert_true(g_file_test(file, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND state = 'present'",
		library_file), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_file WHERE file_id = %" G_GINT64_FORMAT " AND movie_id = %"
		G_GINT64_FORMAT " AND import_id = %" G_GINT64_FORMAT, library_file, second, import), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import WHERE id = %" G_GINT64_FORMAT " AND state = 'imported'", import),
		==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab WHERE id = %" G_GINT64_FORMAT " AND close_reason ="
		" 'imported' AND removal IS NULL AND retry_at IS NULL", grab), ==, 1);
	{
		g_autofree gchar *state = q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);

		g_assert_cmpstr(state, ==, n);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job"), ==, jobs);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind ="
		" 'blocklisted' AND by_user = 'root' AND text LIKE '%%its file in the library stays as it is%%'", second), ==, 1);
	reply = get_as(fx, fx->admin, landed);
	assert_contains(reply.body, "Blocked for the future.");
	assert_contains(reply.body, "no file in the library and no link was touched");
	axis_test_reply_clear(&reply);
	/* A search does not take it again. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, second, "scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
		second), ==, 0);
}

/* ── Replace the download in flight (add-replace-grab-and-delete-download, A) ── */

/* Lantern Harbor (2011), a made-up film, and three made-up releases of it. */
#define HARBOR_TV   "Lantern.Harbor.2011.720p.HDTV.x264-MADEUP"
#define HARBOR_WEB  "Lantern.Harbor.2011.1080p.WEB-DL.x264-MADEUP"
#define HARBOR_DISC "Lantern.Harbor.2011.1080p.BluRay.x264-MADEUP"

/* The value of the hidden field @name of a page's form ("" when the page has
 * no such field). */
static gchar *
hidden_of(
	const gchar *body,
	const gchar *name
){
	g_autofree gchar *needle = g_strdup_printf("name=\"%s\"", name);
	const gchar *field = body != NULL ? strstr(body, needle) : NULL;
	const gchar *start;
	const gchar *end;
	const gchar *value;

	if (field == NULL)
		return g_strdup("");
	start = g_strrstr_len(body, field - body, "<input");
	end = strchr(field, '>');
	g_assert_nonnull(start);
	g_assert_nonnull(end);
	value = g_strstr_len(start, end - start, "value=\"");
	g_assert_nonnull(value);
	value += strlen("value=\"");
	return g_strndup(value, strcspn(value, "\""));
}

/* How many lines of the log (the audit among them) say @text. */
static guint
logged_line(const gchar *text)
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

typedef struct
{
	Fixture *fx;
	gint64   film;
	gchar   *manual;	/* the manual search's page */
	gchar   *grab;		/* the route of Grab this one */
	gchar   *force;
} Harbor;

static void
harbor_clear(Harbor *h)
{
	g_free(h->manual);
	g_free(h->grab);
	g_free(h->force);
}

/* The film with a manual search's list of the three releases. */
static void
harbor_setup(
	Fixture *fx,
	Harbor  *h
){
	static const Rel found[] = {
		{ HARBOR_TV, 2000000000, 9300001 },
		{ HARBOR_WEB, 6000000000, 9300001 },
		{ HARBOR_DISC, 12000000000, 9300001 }
	};
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;

	memset(h, 0, sizeof(*h));
	h->fx = fx;
	set_route(fx, "/title/film/171", "{\"id\":\"171\",\"kind\":\"film\",\"title\":\"Lantern Harbor\",\"year\":2011,"
		"\"runtime\":100,\"status\":\"released\",\"release_date\":\"2011-04-05\",\"ids\":{\"imdb\":\"tt9300001\"}}");
	h->film = add_film(fx, "171");
	set_releases(fx, found, G_N_ELEMENTS(found));
	h->manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual?level=movie", h->film);
	h->grab = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab", h->film);
	h->force = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/force", h->film);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, h->film, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
}

/* The confirmation of a grab by hand of @release. */
static AxisTestReply
harbor_page(
	Harbor      *h,
	const gchar *release
){
	g_autofree gchar *path = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", h->grab,
		listed_candidate(h->fx, release));

	return get_as(h->fx, h->fx->admin, path);
}

/* Grab this one for @release, posted with @more ("" for none) after the
 * confirmation was opened. */
static AxisTestReply
harbor_post(
	Harbor      *h,
	const gchar *release,
	const gchar *more
){
	g_autofree gchar *form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes%s",
		listed_candidate(h->fx, release), more);

	return post_as(h->fx, h->fx->admin, h->manual, h->grab, form);
}

/* The fields of a replace, as the confirmation @body offers it, ticked. */
static gchar *
harbor_swap(const gchar *body)
{
	g_autofree gchar *grab = hidden_of(body, "replace");
	g_autofree gchar *digest = hidden_of(body, "replace_digest");

	g_assert_cmpstr(grab, !=, "");
	g_assert_cmpuint(strlen(digest), ==, 32);
	return g_strdup_printf("&replace=%s&replace_digest=%s&replace_ok=yes", grab, digest);
}

/* The film's open grab downloads: its client says so, 20 % done. */
static gchar *
harbor_downloading(Harbor *h)
{
	gchar *ref = ref_of(h->fx, h->film);

	g_assert_nonnull(ref);
	axis_test_download_fake_set(h->fx->fake, ref, "downloading", 800000000, NULL, NULL);
	poll(h->fx);
	return ref;
}

static void
assert_film(
	Harbor      *h,
	const gchar *expected,
	const gchar *select
){
	g_autofree gchar *sql = g_strdup_printf("SELECT %s FROM movie_grab g WHERE g.movie_id = %" G_GINT64_FORMAT, select,
		h->film);
	g_autofree gchar *value = axis_test_db_query(h->fx->t->db, sql);

	if (g_strcmp0(value, expected) != 0)
		g_error("%s: expected %s, got %s", select, expected, value != NULL ? value : "NULL");
}

/* What a search that claims in the moment of a replace does, from the claim's
 * hook: once. */
typedef struct
{
	Fixture  *fx;
	gint64    film;
	gboolean  done;
} Rival;

static void
rival_search(
	gint64   owner_id,
	gpointer user_data
){
	Rival *rival = user_data;

	if (rival->done || owner_id != rival->film)
		return;
	rival->done = TRUE;
	search(rival->fx, rival->film);
}

/* A second post of the same replace, in the moment the first has ended the
 * download in flight and has not claimed yet. */
typedef struct
{
	Fixture               *fx;
	gint64                 film;
	gint64                 candidate;
	gint64                 grab;
	gchar                 *digest;
	gboolean               done;
	AxisAcquireGrabResult  result;
} Twin;

static void
twin_post(
	gint64   owner_id,
	gpointer user_data
){
	Twin *twin = user_data;
	g_autoptr(GError) error = NULL;
	AxisAcquireReplaceTicks swap;
	AxisAcquireHandTicks ticks;

	if (twin->done || owner_id != twin->film)
		return;
	twin->done = TRUE;
	memset(&swap, 0, sizeof(swap));
	memset(&ticks, 0, sizeof(ticks));
	swap.grab_id = twin->grab;
	swap.ok = TRUE;
	swap.digest = twin->digest;
	ticks.in_flight = &swap;
	g_assert_false(axis_movies_grab_by_hand(twin->fx->movies, twin->film, twin->candidate, &ticks, "root", NULL,
		&twin->result, &error));
}

/*
 * Spec (movies): "An administrator can replace a movie's download in flight",
 * scenarios "Replaced", "Without the tick", "The client cannot remove it",
 * "The new release cannot be sent", "It has completed", "It completes while
 * the person confirms", "The release replaced is not blocked", "No search in
 * between", "A search claimed first", "Another obstacle", "A viewer".  And:
 * a form that is out of date or forged, a check of the new grab that fails
 * before anything is ended, and two posts of one replace.
 */
static void
test_replace_grab(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *third = NULL;
	g_autofree gchar *swap = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *place = NULL;
	g_autofree gchar *file = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	Harbor h;
	guint adds;

	harbor_setup(fx, &h);

	/* A dead end met in use: a download Axis started is in flight. */
	reply = harbor_post(&h, HARBOR_TV, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	first = harbor_downloading(&h);

	/* The confirmation says which download is in the way, where and how far,
	 * and offers to replace it behind a tick.  It writes nothing and asks the
	 * client nothing. */
	reply = harbor_page(&h, HARBOR_WEB);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "A download is in flight");
	assert_contains(reply.body, "Downloading now: " HARBOR_TV ", at SABnzbd, 20 % of ");
	assert_contains(reply.body, "Axis then removes " HARBOR_TV " from its client with its data, and sends this "
		"release. The release replaced is not blocked");
	assert_contains(reply.body, "Axis asks the client where the download is first: if the client does not answer, or "
		"the download is somewhere Axis does not let a client delete, nothing is ended and nothing is sent. If the "
		"client then fails to remove it, the download is ended, nothing is sent, and Axis keeps asking the client.");
	assert_contains(reply.body, "Replace the download in flight with this one");
	assert_contains(reply.body, ">Grab this one by hand</button>");
	assert_lacks(reply.body, "cannot be grabbed");
	assert_lacks(reply.body, "in flight already");
	assert_lacks(reply.body, "name=\"replace_others\"");
	assert_csp_clean(reply.body);
	save_page("replace-offer.html", reply.body);
	swap = harbor_swap(reply.body);
	axis_test_reply_clear(&reply);
	assert_film(&h, "1|1", "count(*)::text || '|' || count(*) FILTER (WHERE g.closed_at IS NULL)");
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* Scenario: A viewer; and a post with no token. */
	{
		g_autofree gchar *path = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", h.grab,
			listed_candidate(fx, HARBOR_WEB));
		g_autofree gchar *form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes%s",
			listed_candidate(fx, HARBOR_WEB), swap);

		reply = get_as(fx, fx->viewer, path);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->viewer, h.manual, h.grab, form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, NULL, h.grab, form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}

	/* Scenario: Without the tick.  And without any of the replace's fields,
	 * the grab is refused as it always was. */
	{
		g_autofree gchar *unticked = g_strndup(swap, strlen(swap) - strlen("&replace_ok=yes"));

		reply = harbor_post(&h, HARBOR_WEB, unticked);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "tick the box to replace the download in flight with this release. Nothing was "
			"ended and nothing was sent.");
		axis_test_reply_clear(&reply);
	}
	reply = harbor_post(&h, HARBOR_WEB, "");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "A grab of the movie is in flight already.");
	axis_test_reply_clear(&reply);

	/* A forged or stale form: another grab named, or another digest.  Nothing
	 * is written, and the page is shown again. */
	reply = harbor_post(&h, HARBOR_WEB, "&replace=999999&replace_ok=yes&replace_digest=0123456789abcdef0123456789abcdef");
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "The download in flight changed after you opened this page");
	assert_contains(reply.body, "Replace the download in flight with this one");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *grab = q(fx, "SELECT id FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, h.film);
		g_autofree gchar *forged = g_strdup_printf("&replace=%s&replace_ok=yes&replace_digest=0123456789abcdef"
			"0123456789abcdef", grab);

		reply = harbor_post(&h, HARBOR_WEB, forged);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "The download in flight changed after you opened this page");
		axis_test_reply_clear(&reply);
		/* The digest is of this release's page: it does not carry to another. */
		reply = harbor_post(&h, HARBOR_DISC, swap);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "The download in flight changed after you opened this page");
		axis_test_reply_clear(&reply);
	}

	/* Every check of the new grab comes first: a file in the library that the
	 * release is not better than, and its tick not given.  The download in
	 * flight is untouched. */
	{
		gint64 kept = add_kite_file(fx, h.film, "bluray_1080p");
		g_autofree gchar *now = NULL;

		reply = harbor_page(&h, HARBOR_WEB);
		assert_contains(reply.body, "name=\"downgrade\"");
		now = harbor_swap(reply.body);
		axis_test_reply_clear(&reply);
		reply = harbor_post(&h, HARBOR_WEB, now);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "it is not better than: tick the box to replace it. Nothing was sent.");
		axis_test_reply_clear(&reply);
		axis_test_db_exec(fx->t->db, "DELETE FROM movie_file");
		{
			g_autofree gchar *gone = q(fx, "DELETE FROM library_file WHERE id = %" G_GINT64_FORMAT " RETURNING id", kept);

			g_assert_nonnull(gone);
		}
	}

	/* Scenario: Another obstacle.  A release on the blocklist gets no offer,
	 * and the page says that obstacle; a post with a replace's fields is
	 * refused for it. */
	{
		g_autofree gchar *block = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/block", h.film);
		g_autofree gchar *form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie",
			listed_candidate(fx, HARBOR_DISC));

		reply = post_as(fx, fx->admin, h.manual, block, form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		reply = harbor_page(&h, HARBOR_DISC);
		assert_contains(reply.body, "cannot be grabbed");
		assert_contains(reply.body, "It is on the blocklist");
		assert_lacks(reply.body, "Replace the download in flight with this one");
		assert_lacks(reply.body, "name=\"replace\"");
		axis_test_reply_clear(&reply);
		reply = harbor_post(&h, HARBOR_DISC, swap);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "blocklist");
		axis_test_reply_clear(&reply);
	}
	/* Nothing above ended anything, sent anything or asked the client to
	 * remove anything. */
	assert_film(&h, "1|1", "count(*)::text || '|' || count(*) FILTER (WHERE g.closed_at IS NULL)");
	g_assert_true(axis_test_download_fake_has(fx->fake, first));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, 1);

	/* Scenario: Replaced.  The old grab is closed as replaced by the person,
	 * its download removed from the client with its data, the release chosen
	 * sent, and the movie has one open grab. */
	reply = harbor_post(&h, HARBOR_WEB, swap);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=grabbed");
	axis_test_reply_clear(&reply);
	assert_film(&h, "replaced|Replaced by root with " HARBOR_WEB ".|done", "string_agg(g.close_reason || '|' ||"
		" g.close_text || '|' || g.removal, ',') FILTER (WHERE g.release_name = '" HARBOR_TV "')");
	assert_film(&h, HARBOR_WEB "|hand|root", "string_agg(g.release_name || '|' || g.set_aside || '|' || g.grabbed_by, ',')"
		" FILTER (WHERE g.closed_at IS NULL)");
	g_assert_false(axis_test_download_fake_has(fx->fake, first));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", first), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind ="
		" 'abandoned' AND by_user = 'root' AND text = '" HARBOR_TV ": Replaced by root with " HARBOR_WEB ". Axis asks"
		" the client to remove it with its data.'", h.film), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_event WHERE movie_id = %" G_GINT64_FORMAT " AND kind = 'grabbed'"
		" AND text LIKE 'Grabbed by hand " HARBOR_WEB "%%It is sent in place of " HARBOR_TV ", which was downloading.'",
		h.film), ==, 1);
	g_assert_cmpuint(logged_line("in place of the download in flight"), ==, 1);
	/* Scenario: The release replaced is not blocked (the one blocked above is
	 * the only entry).  Scenario: No search in between. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT, h.film), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_blocklist WHERE movie_id = %" G_GINT64_FORMAT
		" AND release_name = '" HARBOR_TV "'", h.film), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 0);
	/* The form of a moment ago, posted again: nothing is in the way that it
	 * names, so nothing is ended and nothing sent. */
	reply = harbor_post(&h, HARBOR_WEB, swap);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "The download in flight changed after you opened this page");
	axis_test_reply_clear(&reply);
	assert_film(&h, "2|1", "count(*)::text || '|' || count(*) FILTER (WHERE g.closed_at IS NULL)");
	/* The release in flight itself: there is nothing to replace it with. */
	reply = harbor_page(&h, HARBOR_WEB);
	assert_contains(reply.body, "cannot be grabbed");
	assert_contains(reply.body, "This very release is the one in flight");
	assert_lacks(reply.body, "name=\"replace\"");
	axis_test_reply_clear(&reply);

	/* Scenario: The client cannot remove it.  Nothing is sent; the old grab
	 * stays closed with its removal failed; no search grabs meanwhile; the
	 * client is asked again at the next follow-up. */
	second = harbor_downloading(&h);
	reply = harbor_page(&h, HARBOR_TV);
	g_free(swap);
	swap = harbor_swap(reply.body);
	axis_test_reply_clear(&reply);
	adds = axis_test_download_fake_count(fx->fake, "add", NULL);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_DOWN);
	reply = harbor_post(&h, HARBOR_TV, swap);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, HARBOR_WEB " was ended, but it is still at its client. The client could not remove it");
	assert_contains(reply.body, "Nothing was sent. Axis keeps asking the client; nothing else is grabbed for the movie "
		"until it is gone. Then grab this release again from the manual search, or leave it to the next search.");
	axis_test_reply_clear(&reply);
	assert_film(&h, "replaced|failed", "string_agg(g.close_reason || '|' || g.removal, ',') FILTER (WHERE g.release_name"
		" = '" HARBOR_WEB "')");
	assert_film(&h, "0", "count(*) FILTER (WHERE g.closed_at IS NULL)");
	g_assert_true(axis_test_download_fake_has(fx->fake, second));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, adds);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, h.film, "scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.skipped, ==, "a grab of it is in flight");
	reply = harbor_page(&h, HARBOR_TV);
	assert_contains(reply.body, "cannot be grabbed");
	assert_contains(reply.body, "Axis is still removing an earlier download (" HARBOR_WEB ") from the client. Nothing "
		"is grabbed until it is gone.");
	assert_contains(reply.body, "href=\"/downloads\"");
	assert_lacks(reply.body, "name=\"replace\"");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_movies_run_follow(fx->movies, NULL, &error));
	g_assert_no_error(error);
	g_assert_false(axis_test_download_fake_has(fx->fake, second));
	assert_film(&h, "replaced|done", "string_agg(g.close_reason || '|' || g.removal, ',') FILTER (WHERE g.release_name"
		" = '" HARBOR_WEB "')");
	/* No search follows a replace, then either: the release is grabbed again
	 * by the person. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'movies.search'"), ==, 0);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, adds);
	reply = harbor_post(&h, HARBOR_TV, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);

	/* Scenario: The new release cannot be sent.  The old download is removed,
	 * the client refuses the new one: the page says both, and the movie is
	 * back to ordinary searching. */
	third = harbor_downloading(&h);
	reply = harbor_page(&h, HARBOR_WEB);
	g_free(swap);
	swap = harbor_swap(reply.body);
	axis_test_reply_clear(&reply);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_ADD_REFUSED);
	reply = harbor_post(&h, HARBOR_WEB, swap);
	axis_test_download_fake_set_mode(fx->fake, AXIS_TEST_DOWNLOAD_NORMAL);
	g_assert_cmpuint(reply.status, ==, 502);
	assert_contains(reply.body, "The download in flight (" HARBOR_TV ") was removed, but this release could not be sent: "
		"the download client refused it");
	assert_contains(reply.body, "Nothing is downloading for the movie now. A scheduled search takes it up again if it is "
		"monitored; otherwise search for it yourself.");
	assert_lacks(reply.body, "back to ordinary searching");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_test_download_fake_has(fx->fake, third));
	assert_film(&h, "0", "count(*) FILTER (WHERE g.closed_at IS NULL)");
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 0);
	/* (The client's refusal put that release on the blocklist, as it does for
	 * any grab: taken off again for what follows.) */
	axis_test_db_exec(fx->t->db, "DELETE FROM movie_blocklist WHERE release_name = '" HARBOR_WEB "'");
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, h.film, "scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_null(report.skipped);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");

	/* Two posts of one replace: the second arrives when the first has ended
	 * the download in flight and has not claimed yet.  It ends nothing and
	 * sends nothing; the first sends its release. */
	{
		g_autofree gchar *held = NULL;
		const gchar *chosen;
		Twin twin;

		if (qi(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", h.film)
		    == 0) {
			reply = harbor_post(&h, HARBOR_TV, "");
			g_assert_cmpuint(reply.status, ==, 303);
			axis_test_reply_clear(&reply);
		}
		g_free(third);
		third = harbor_downloading(&h);
		held = q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
			h.film);
		chosen = strcmp(held, HARBOR_TV) == 0 ? HARBOR_WEB : HARBOR_TV;
		reply = harbor_page(&h, chosen);
		g_free(swap);
		swap = harbor_swap(reply.body);
		memset(&twin, 0, sizeof(twin));
		twin.fx = fx;
		twin.film = h.film;
		twin.candidate = listed_candidate(fx, chosen);
		twin.digest = hidden_of(reply.body, "replace_digest");
		{
			g_autofree gchar *grab = hidden_of(reply.body, "replace");

			twin.grab = g_ascii_strtoll(grab, NULL, 10);
		}
		twin.result = AXIS_ACQUIRE_GRAB_SENT;
		axis_test_reply_clear(&reply);
		adds = axis_test_download_fake_count(fx->fake, "add", NULL);
		axis_movies_set_claim_hook(fx->movies, twin_post, &twin);
		reply = harbor_post(&h, chosen, swap);
		axis_movies_set_claim_hook(fx->movies, NULL, NULL);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_assert_true(twin.done);
		g_assert_cmpint(twin.result, ==, AXIS_ACQUIRE_GRAB_CHANGED);
		g_free(twin.digest);
		assert_film(&h, "1", "count(*) FILTER (WHERE g.closed_at IS NULL)");
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, adds + 1);
		g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 1);
	}

	/* Scenario: A search claimed first.  In the moment between the removal
	 * and the person's claim a search grabs a release: the person's is not
	 * sent, the movie has one open grab and one download, and the page says
	 * which release was grabbed. */
	{
		g_autofree gchar *held = NULL;
		g_autofree gchar *took = NULL;
		const gchar *chosen;
		Rival rival;

		g_free(third);
		third = harbor_downloading(&h);
		held = q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
			h.film);
		chosen = strcmp(held, HARBOR_TV) == 0 ? HARBOR_WEB : HARBOR_TV;
		reply = harbor_page(&h, chosen);
		g_free(swap);
		swap = harbor_swap(reply.body);
		axis_test_reply_clear(&reply);
		memset(&rival, 0, sizeof(rival));
		rival.fx = fx;
		rival.film = h.film;
		adds = axis_test_download_fake_count(fx->fake, "add", NULL);
		axis_movies_set_claim_hook(fx->movies, rival_search, &rival);
		reply = harbor_post(&h, chosen, swap);
		axis_movies_set_claim_hook(fx->movies, NULL, NULL);
		g_assert_true(rival.done);
		g_assert_cmpuint(reply.status, ==, 409);
		took = q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL"
			" AND NOT manual", h.film);
		g_assert_nonnull(took);
		g_free(text);
		text = g_strdup_printf("%s was removed, and a search grabbed %s for the movie in the same moment. Nothing else "
			"was sent. You can replace that one from the manual search.", held, took);
		assert_contains(reply.body, text);
		axis_test_reply_clear(&reply);
		assert_film(&h, "1", "count(*) FILTER (WHERE g.closed_at IS NULL)");
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, adds + 1);
		g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 1);
		axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'movies.search'");
	}

	/* Scenario: It completes while the person confirms.  The page was shown
	 * while it downloaded; it arrives before the post: nothing is ended,
	 * nothing sent, and the page points to the import.  Scenario: It has
	 * completed: no offer. */
	{
		g_autofree gchar *held = NULL;
		const gchar *chosen;
		guint removes;

		g_free(third);
		third = harbor_downloading(&h);
		held = q(fx, "SELECT release_name FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
			h.film);
		chosen = strcmp(held, HARBOR_TV) == 0 ? HARBOR_WEB : HARBOR_TV;
		reply = harbor_page(&h, chosen);
		g_free(swap);
		swap = harbor_swap(reply.body);
		axis_test_reply_clear(&reply);
		place = g_build_filename(fx->disk, "downloads", "complete", held, NULL);
		file = g_build_filename(place, "lantern-harbor.mkv", NULL);
		axis_test_storage_mkdir(place);
		axis_test_storage_write(file, "film");
		g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/downloads", fx->volume_id,
			"downloads", &error));
		g_assert_no_error(error);
		g_free(text);
		text = g_strdup_printf("/data/downloads/complete/%s", held);
		axis_test_download_fake_set(fx->fake, third, "completed", 2000000000, text, NULL);
		poll(fx);
		adds = axis_test_download_fake_count(fx->fake, "add", NULL);
		removes = axis_test_download_fake_count(fx->fake, "remove", NULL);
		reply = harbor_post(&h, chosen, swap);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "The download in flight changed after you opened this page");
		assert_contains(reply.body, "cannot be grabbed");
		g_free(text);
		text = g_strdup_printf("%s has arrived", held);
		assert_contains(reply.body, text);
		assert_lacks(reply.body, "name=\"replace\"");
		axis_test_reply_clear(&reply);
		assert_film(&h, "1", "count(*) FILTER (WHERE g.closed_at IS NULL)");
		g_assert_true(axis_test_download_fake_has(fx->fake, third));
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "add", NULL), ==, adds);
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, removes);
		g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
		/* The follow-up records the completion; no import of it exists yet:
		 * the page says it has arrived and points to Downloads. */
		g_assert_true(axis_movies_run_follow(fx->movies, NULL, &error));
		g_assert_no_error(error);
		reply = harbor_page(&h, chosen);
		assert_contains(reply.body, "cannot be grabbed");
		g_free(text);
		text = g_strdup_printf("%s has arrived, and its import is about to start. It is not replaced: resolve its "
			"import first.", held);
		assert_contains(reply.body, text);
		assert_contains(reply.body, "href=\"/downloads\"");
		assert_lacks(reply.body, "Replace the download in flight with this one");
		axis_test_reply_clear(&reply);
		/* Its import exists (blocked, as one that waits for a person is): the
		 * page links to it. */
		{
			gint64 import = qi(fx, "INSERT INTO import (module, owner_id, title_label, source_path, state, download_id)"
				" SELECT 'movies', %" G_GINT64_FORMAT ", 'Lantern Harbor (2011)', 'downloads/complete', 'blocked',"
				" g.download_id FROM movie_grab g WHERE g.movie_id = %" G_GINT64_FORMAT " AND g.closed_at IS NULL"
				" RETURNING id", h.film, h.film);

			g_free(text);
			text = g_strdup_printf("href=\"/imports/%" G_GINT64_FORMAT "\"", import);
		}
		reply = harbor_page(&h, chosen);
		assert_contains(reply.body, "cannot be grabbed");
		assert_contains(reply.body, text);
		g_free(text);
		text = g_strdup_printf("%s has arrived and waits for its import. Resolve the import first.", held);
		assert_contains(reply.body, text);
		assert_lacks(reply.body, "Replace the download in flight with this one");
		axis_test_reply_clear(&reply);
		/* A post that asks for the replace all the same ends nothing. */
		reply = harbor_post(&h, chosen, swap);
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
		assert_film(&h, "1", "count(*) FILTER (WHERE g.closed_at IS NULL)");
		g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, removes);
	}
	harbor_clear(&h);
}

/* The same offer on the confirmation of a grab by force: the force digest
 * and the replace's digest are both checked. */
static void
test_replace_grab_force(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ KITE_FORCED, 4294967296, 9200001 },
		{ "Paper.Kite.2019.720p.HDTV.x264-MADEUP", 2000000000, 9200001 }
	};
	g_autofree gchar *manual = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *force = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *swap = NULL;
	g_autofree gchar *ref = NULL;
	g_autoptr(GError) error = NULL;
	AxisMovieSearchReport report;
	AxisTestReply reply;
	gint64 film;
	gint64 forced;
	gint64 plain;

	film = add_kite(fx);
	set_releases(fx, found, G_N_ELEMENTS(found));
	manual = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual?level=movie", film);
	grab = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/grab", film);
	force = g_strdup_printf("/films/%" G_GINT64_FORMAT "/manual/force", film);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_movies_run_search(fx->movies, film, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	forced = listed_candidate(fx, KITE_FORCED);
	plain = listed_candidate(fx, "Paper.Kite.2019.720p.HDTV.x264-MADEUP");
	/* Both names fit two stored titles, so both are ruled out by their name:
	 * one is grabbed by force and downloads. */
	form = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", force, plain);
	reply = get_as(fx, fx->admin, form);
	g_assert_cmpuint(reply.status, ==, 200);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s", plain, digest);
	reply = post_as(fx, fx->admin, manual, force, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	ref = ref_of(fx, film);
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1000000000, NULL, NULL);
	poll(fx);

	/* The force confirmation of the other release offers the replace. */
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=movie", force, forced);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Grab this release by force?");
	assert_contains(reply.body, "A download is in flight");
	assert_contains(reply.body, "Downloading now: Paper.Kite.2019.720p.HDTV.x264-MADEUP, at SABnzbd, 25 % of");
	assert_contains(reply.body, "Replace the download in flight with this one");
	assert_contains(reply.body, ">Grab it by force</button>");
	assert_csp_clean(reply.body);
	g_free(digest);
	digest = digest_of(reply.body);
	swap = harbor_swap(reply.body);
	axis_test_reply_clear(&reply);

	/* The replace's digest wrong, the force digest right: shown again. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s&replace=%"
		G_GINT64_FORMAT "&replace_ok=yes&replace_digest=0123456789abcdef0123456789abcdef", forced, digest,
		qi(fx, "SELECT id FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film));
	reply = post_as(fx, fx->admin, manual, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "What this page showed changed after you opened it");
	axis_test_reply_clear(&reply);
	/* The force digest wrong, the replace's right: shown again, too. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes"
		"&digest=0123456789abcdef0123456789abcdef%s", forced, swap);
	reply = post_as(fx, fx->admin, manual, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "What this page showed changed after you opened it");
	axis_test_reply_clear(&reply);
	/* Both right, the replace not ticked: nothing. */
	{
		g_autofree gchar *unticked = g_strndup(swap, strlen(swap) - strlen("&replace_ok=yes"));

		g_free(form);
		form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s%s", forced,
			digest, unticked);
		reply = post_as(fx, fx->admin, manual, force, form);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "tick the box to replace the download in flight with this release");
		axis_test_reply_clear(&reply);
	}
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT " AND closed_at IS NULL",
		film), ==, 1);
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, 0);

	/* Both right and ticked: replaced, by force. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=movie&confirm=yes&checked=yes&digest=%s%s", forced,
		digest, swap);
	reply = post_as(fx, fx->admin, manual, force, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	{
		g_autofree gchar *now = q(fx, "SELECT string_agg(release_name || '|' || coalesce(close_reason, 'open') || '|' ||"
			" bound::text, ',' ORDER BY id) FROM movie_grab WHERE movie_id = %" G_GINT64_FORMAT, film);

		g_assert_cmpstr(now, ==, "Paper.Kite.2019.720p.HDTV.x264-MADEUP|replaced|true," KITE_FORCED "|open|true");
	}
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	AXIS_TEST_ADD_DB_CASE("/web/movies/replace-grab", Fixture, NULL, test_replace_grab);
	AXIS_TEST_ADD_DB_CASE("/web/movies/replace-grab-force", Fixture, NULL, test_replace_grab_force);
	AXIS_TEST_ADD_DB_CASE("/web/movies/block-release", Fixture, NULL, test_block_release);
	AXIS_TEST_ADD_DB_CASE("/web/movies/links", Fixture, NULL, test_links);
	AXIS_TEST_ADD_DB_CASE("/web/movies/file-link", Fixture, NULL, test_file_link);
	AXIS_TEST_ADD_DB_CASE("/web/movies/repeated-clicks", Fixture, NULL, test_repeated_clicks);
	AXIS_TEST_ADD_DB_CASE("/web/movies/says-movie", Fixture, NULL, test_says_movie);
	AXIS_TEST_ADD_DB_CASE("/web/movies/pages", Fixture, NULL, test_pages);
	AXIS_TEST_ADD_DB_CASE("/web/movies/actions", Fixture, NULL, test_actions);
	AXIS_TEST_ADD_DB_CASE("/web/movies/attention", Fixture, NULL, test_attention);
	AXIS_TEST_ADD_DB_CASE("/web/movies/manual-search", Fixture, NULL, test_manual_search);
	AXIS_TEST_ADD_DB_CASE("/web/movies/manual-sort", Fixture, NULL, test_manual_sort);
	AXIS_TEST_ADD_DB_CASE("/web/movies/force-grab", Fixture, NULL, test_force_grab);
	AXIS_TEST_ADD_DB_CASE("/web/movies/kept-by-hand", Fixture, NULL, test_kept_by_hand);
	AXIS_TEST_ADD_DB_CASE("/web/movies/kept-grab-anyway", Fixture, NULL, test_kept_grab_anyway);
	AXIS_TEST_ADD_DB_CASE("/web/movies/module-off", Fixture, NULL, test_module_off);
	AXIS_TEST_ADD_DB_CASE("/web/movies/getting-started", Fixture, NULL, test_getting_started);
	AXIS_TEST_ADD_DB_CASE("/web/movies/list-at-scale", Fixture, NULL, test_list_at_scale);
	AXIS_TEST_ADD_DB_CASE("/web/movies/list-views", Fixture, NULL, test_list_views);
	AXIS_TEST_ADD_DB_CASE("/web/movies/library", Fixture, NULL, test_library);
	AXIS_TEST_ADD_DB_CASE("/web/movies/library-at-scale", Fixture, NULL, test_library_at_scale);
	return g_test_run();
}
