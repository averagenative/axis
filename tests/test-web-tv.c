/*
 * test-web-tv.c - the Series pages, a series' title page, the dashboard's
 * parts and the administrators' actions, with the real shell on an
 * ephemeral loopback port, the metadata test provider, the fake Prowlarr,
 * the fixture download client, a real database and a real volume
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tasks of add-tv-module: 5.1 (the Series tab, the grid, a series' page
 * with its seasons, episodes and monitoring), 5.2 (an episode's releases, a
 * season's, the calendar, wanted, history, the dashboard's parts), 6.10
 * (non-administrators look and cannot act; every action is in the series'
 * history with its user).
 *
 * With AXIS_TEST_TV_PAGES set to a directory, the pages are also saved there
 * (series.html, series-page.html, ...), for looking at in a browser (6.11).
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
#include "axis-test-tvdb.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "download/axis-download.h"
#include "indexer/axis-indexer.h"
#include "indexer/axis-indexer-prowlarr.h"
#include "library/axis-library-browse.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "modules/movies/axis-movies.h"
#include "modules/tv/axis-tv.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-activity.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-browser.h"
#include "web/axis-web-downloads.h"
#include "web/axis-web-hub.h"
#include "web/axis-web-indexers.h"
#include "web/axis-web-jobs.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

#define KEY "prowlarr-key-for-series-pages-3f7a"
#define PASSWORD AXIS_TEST_DOWNLOAD_PASSWORD

/* 2026-11-14 12:00:00 UTC. */
#define NOW_US ((gint64)1794657600 * G_USEC_PER_SEC)

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
	AxisTestTvdb         *tvdb;	/* TheTVDB, without its key until a test sets it */
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
	AxisTv               *tv;
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
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series", AXIS_MEDIA_TV);
	axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "films", AXIS_MEDIA_MOVIES);
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
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
	fx->tv = axis_tv_new(fx->t->pool, fx->settings, fx->meta, fx->indexers, fx->downloads, fx->t->storage,
		fx->quality);
	axis_tv_set_clock(fx->tv, NOW_US);

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
	g_assert_true(axis_module_registry_register(fx->registry, axis_tv_get_module(), fx->tv, &error));
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

	/* The standard titles; Harbour Lights; Night Ferry, which airs today;
	 * Quiet Coast, not added. */
	axis_test_meta_fixture_standard(fx->fa);
	axis_test_plex_route_file(fx->fa->api, "/title/series/202", -1, "tests/fixtures/meta/title-series-202.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/202/season/0", -1, "tests/fixtures/meta/season-202-0.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/202/season/1", -1, "tests/fixtures/meta/season-202-1.json");
	axis_test_plex_route_file(fx->fa->api, "/title/series/202/season/2", -1, "tests/fixtures/meta/season-202-2.json");
	set_route(fx, "/title/series/204", "{\"id\":\"204\",\"kind\":\"series\",\"title\":\"Night Ferry\",\"year\":2026,"
		"\"status\":\"continuing\",\"overview\":\"The last boat out, every night.\",\"ids\":{\"tvdb\":\"370004\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-07\",\"episodes\":4}]}");
	set_route(fx, "/title/series/204/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9401\",\"name\":\"Departure\",\"air_date\":\"2026-11-07\"},"
		"{\"number\":2,\"id\":\"9402\",\"name\":\"Crossing\",\"air_date\":\"2026-11-14\"},"
		"{\"number\":3,\"id\":\"9403\",\"name\":\"Fog Bank\",\"air_date\":\"2026-11-21\"},"
		"{\"number\":4,\"id\":\"9404\",\"name\":\"Arrival Hall\",\"air_date\":\"2026-11-28\"}]}");
	set_route(fx, "/title/series/205", "{\"id\":\"205\",\"kind\":\"series\",\"title\":\"Quiet Coast\",\"year\":2019,"
		"\"status\":\"ended\",\"overview\":\"A coastline with nothing to report, reported anyway.\",\"seasons\":[]}");
	set_route(fx, "/search/series", "{\"results\":[{\"id\":\"202\",\"kind\":\"series\",\"title\":\"Harbour Lights\","
		"\"year\":2024},{\"id\":\"201\",\"kind\":\"series\",\"title\":\"Fixture Street\",\"year\":2021},"
		"{\"id\":\"204\",\"kind\":\"series\",\"title\":\"Night Ferry\",\"year\":2026}]}");

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
	g_assert_true(axis_downloads_add_path_map(fx->downloads, fx->client_id, "/data/downloads", fx->volume_id,
		"downloads", &error));
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
	axis_tv_stop(fx->tv);
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
	axis_tv_unref(fx->tv);
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
	axis_test_tvdb_free(fx->tvdb);
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

/* @haystack unbroken, with the entities htmx-glib writes for quotes and
 * ampersands read back. */
static gchar *
plain_of(const gchar *haystack)
{
	static const gchar *const entities[][2] = {
		{ "&#39;", "'" }, { "&#x27;", "'" }, { "&apos;", "'" }, { "&quot;", "\"" }, { "&amp;", "&" }
	};
	g_autofree gchar *unbroken = axis_test_web_unbroken(haystack);
	g_autoptr(GString) plain = g_string_new(unbroken);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(entities); i++)
		g_string_replace(plain, entities[i][0], entities[i][1], 0);
	return g_string_free(g_steal_pointer(&plain), FALSE);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = haystack != NULL ? plain_of(haystack) : NULL;

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

/* How many lines of the log (the audit among them) say @text. */
static guint
logged(const gchar *text)
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
	const gchar *dir = g_getenv("AXIS_TEST_TV_PAGES");
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
	if (value == NULL)
		g_error("%s: no value", sql);
	return g_ascii_strtoll(value, NULL, 10);
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
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", AXIS_META_KIND_SERIES, provider_id, 5000, &answer, &error));
	g_assert_nonnull(answer.title);
	id = answer.title->id;
	axis_meta_title_answer_clear(&answer);
	return id;
}

static gint64
add_series(
	Fixture       *fx,
	const gchar   *provider_id,
	AxisTvMonitor  monitor
){
	AxisTvSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.root_id = fx->root_id;
	spec.monitor = monitor;
	spec.user = "root";
	g_assert_true(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

static gint64
episode_id(
	Fixture *fx,
	gint64   series,
	gint     season,
	gint     number
){
	return qi(fx, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT " AND season_number = %d"
		" AND number = %d", series, season, number);
}

typedef struct
{
	const gchar *title;
	gint64       size;
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
			G_GINT64_FORMAT ",\"indexerId\":3,\"indexer\":\"NewsOwl\",\"publishDate\":\"2026-11-13T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=L%u&file=hearth-nzb\",\"magnetUrl\":null,"
			"\"protocol\":\"usenet\",\"categories\":[{\"id\":5040}]}", i > 0 ? "," : "", i, rels[i].title,
			rels[i].size, origin, KEY, i);
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, NULL, body->str);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

static void
search(
	Fixture     *fx,
	gint64       series,
	AxisTvScope  scope,
	gint64       item
){
	AxisAcquireSearchReport report;
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_tv_run_search(fx->tv, series, scope, item, "manual", "root", NULL, &report, &error));
	g_assert_no_error(error);
}

static void
poll(Fixture *fx)
{
	g_autoptr(GError) error = NULL;

	g_assert_true(axis_downloads_poll_client(fx->downloads, fx->client_id, NULL, NULL, &error));
	g_assert_no_error(error);
}

/* A file of the library holding @episode, of @quality. */
static void
in_library(
	Fixture     *fx,
	gint64       series,
	gint64       episode,
	const gchar *name,
	const gchar *quality
){
	static gint64 serial = 0;
	gint64 file = qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", '%s', 'video',"
		" 1600000000, 1, 0, 1, 0, %" G_GINT64_FORMAT ", 1, false, false, false, 1, 1, 1) RETURNING id", fx->root_id,
		fx->volume_id, name, 1000 + episode + 1000000 * serial++);

	g_assert_cmpint(qi(fx, "INSERT INTO series_file (file_id, series_id, quality, release_name) VALUES (%"
		G_GINT64_FORMAT ", %" G_GINT64_FORMAT ", '%s', 'Harbour.Lights.S01E0x.1080p-GRP') RETURNING file_id", file,
		series, quality), ==, file);
	g_assert_cmpint(qi(fx, "INSERT INTO series_file_episode (file_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ") RETURNING file_id", file, episode), ==, file);
}

#define HIGH_WATER_1_2 "Harbour.Lights.S01E03E04.1080p.BluRay.x264-DEMAND"
#define HIGH_WATER_1   "Harbour.Lights.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define PACK_S02       "Harbour.Lights.S02.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define RETURN_WEBDL   "Harbour.Lights.S02E01.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define STORM_WEBDL    "Harbour.Lights.S02E02.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define STREET_LOW     "Fixture.Street.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb"

/* Series in every state there is to show: the scenario the pages are shot
 * from. */
typedef struct
{
	gint64 harbour;		/* every state but to import */
	gint64 street;		/* the first season: one to import, the rest wanted */
	gint64 ferry;		/* airs today */
	gint64 unmanaged;	/* Quiet Coast's title, not added */
} Scene;

static Scene
scene(Fixture *fx)
{
	static const Rel high_water[] = {
		{ HIGH_WATER_1, 1500000000 },
		{ HIGH_WATER_1_2, 6000000000 }
	};
	static const Rel season_2[] = {
		{ PACK_S02, 6000000000 },
		{ RETURN_WEBDL, 1500000000 },
		{ STORM_WEBDL, 1500000000 },
		{ "Harbour.Lights.S01-S02.1080p.BluRay.x264-DEMAND", 20000000000 }
	};
	static const Rel strange[] = {
		{ "Harbour.Lights.S02E09.1080p.WEB-DL.DDP5.1.H.264-NTb", 1500000000 },
		{ "[SubGroup] Harbour Lights - 105 [1080p]", 1400000000 },
		{ "Fixture.Street.S02E01.1080p.WEB-DL.DDP5.1.H.264-NTb", 1500000000 },
		{ "Harbour.Lights.S02E01.720p.HDTV.x264-KILLERS", 900000000 }
	};
	static const Rel street[] = { { STREET_LOW, 1500000000 } };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ref = NULL;
	gboolean created = FALSE;
	Scene s;

	memset(&s, 0, sizeof(s));
	s.harbour = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	/* Season 1: two in the library (one below the cutoff), the two-part
	 * downloading as one. */
	in_library(fx, s.harbour, episode_id(fx, s.harbour, 1, 1), "Harbour Lights (2024)/Season 01/S01E01.mkv",
		"bluray_1080p");
	in_library(fx, s.harbour, episode_id(fx, s.harbour, 1, 2), "Harbour Lights (2024)/Season 01/S01E02.mkv",
		"webdl_1080p");
	set_releases(fx, high_water, G_N_ELEMENTS(high_water));
	search(fx, s.harbour, AXIS_TV_SCOPE_EPISODE, episode_id(fx, s.harbour, 1, 3));
	ref = q(fx, "SELECT client_ref FROM download");
	axis_test_download_fake_set(fx->fake, ref, "downloading", 1600000000, NULL, NULL);
	poll(fx);
	/* Season 2: a season search found a pack it could not take (and queued
	 * each episode, forgotten here); S02E01's own search found nothing
	 * usable; S02E02's search is queued. */
	set_releases(fx, season_2, G_N_ELEMENTS(season_2));
	search(fx, s.harbour, AXIS_TV_SCOPE_SEASON, 2);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	set_releases(fx, strange, G_N_ELEMENTS(strange));
	search(fx, s.harbour, AXIS_TV_SCOPE_EPISODE, episode_id(fx, s.harbour, 2, 1));
	g_assert_true(axis_tv_request_search(fx->tv, s.harbour, AXIS_TV_SCOPE_EPISODE, episode_id(fx, s.harbour, 2, 2),
		"manual", "root", &created, &error));
	g_assert_no_error(error);

	/* Fixture Street, its first season: one arrived, to import. */
	s.street = add_series(fx, "201", AXIS_TV_MONITOR_FIRST);
	set_releases(fx, street, G_N_ELEMENTS(street));
	search(fx, s.street, AXIS_TV_SCOPE_EPISODE, episode_id(fx, s.street, 1, 2));
	g_clear_pointer(&ref, g_free);
	ref = q(fx, "SELECT d.client_ref FROM download d JOIN series_grab g ON g.download_id = d.id WHERE g.series_id = %"
		G_GINT64_FORMAT, s.street);
	{
		g_autofree gchar *place = g_build_filename(fx->disk, "downloads", STREET_LOW, NULL);
		g_autofree gchar *file = g_build_filename(place, STREET_LOW ".mkv", NULL);

		axis_test_storage_mkdir(place);
		axis_test_storage_write(file, "episode");
	}
	axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/" STREET_LOW, NULL);
	poll(fx);
	g_assert_true(axis_tv_run_follow(fx->tv, NULL, &error));
	g_assert_no_error(error);

	/* Night Ferry: an episode airs today. */
	s.ferry = add_series(fx, "204", AXIS_TV_MONITOR_ALL);
	s.unmanaged = title_of(fx, "205");
	return s;
}

/* ── The pages ──────────────────────────────────────────────────── */

static void
test_pages(Fixture *fx, gconstpointer data)
{
	/* A series shows its worst state. */
	static const gchar *const marks[] = { "film-mark-attention", "film-mark-wanted", NULL };
	Scene s = scene(fx);
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	guint i;

	/* The grid, for a viewer: every series with its state, nothing to act on. */
	reply = get_as(fx, fx->viewer, "/series");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Harbour Lights");
	assert_contains(reply.body, "Fixture Street");
	assert_contains(reply.body, "Night Ferry");
	for (i = 0; marks[i] != NULL; i++)
		assert_contains(reply.body, marks[i]);
	assert_contains(reply.body, "2 of 6 aired episodes in the library");
	/* (The name filter is a GET form: it changes nothing.) */
	assert_lacks(reply.body, "action=\"/series\" method=\"post\"");
	assert_lacks(reply.body, "action=\"/series/");
	assert_contains(reply.body, "<form class=\"list-filter\" action=\"/series\" method=\"get\" role=\"search\">");
	assert_csp_clean(reply.body);
	/* By name, the views, a page. */
	{
		AxisTestReply other = get_as(fx, fx->viewer, "/series?q=harbour&view=table");

		assert_contains(other.body, "Harbour Lights");
		assert_lacks(other.body, "Fixture Street");
		assert_contains(other.body, "data-table kind-titles");
		assert_contains(other.body, "2 of 6 aired episodes");
		axis_test_reply_clear(&other);
		other = get_as(fx, fx->viewer, "/series?sort=title&view=overview");
		assert_contains(other.body, "title-row");
		g_assert_true(strstr(other.body, "Fixture Street") < strstr(other.body, "Harbour Lights"));
		g_assert_true(strstr(other.body, "Harbour Lights") < strstr(other.body, "Night Ferry"));
		save_page("series-overview.html", other.body);
		axis_test_reply_clear(&other);
	}
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series");
	/* One Media tab; Movies then TV in its second row, TV the open one; the
	 * series' pages in the third; the heading says TV. */
	{
		const gchar *media = strstr(reply.body, ">Media</a>");
		const gchar *activity = strstr(reply.body, ">Activity<");
		const gchar *types = strstr(reply.body, "aria-label=\"Media sections\"");
		const gchar *pages = strstr(reply.body, "aria-label=\"TV pages\"");
		const gchar *movies = types != NULL ? strstr(types, ">Movies</a>") : NULL;
		const gchar *tv = types != NULL ? strstr(types, ">TV</a>") : NULL;

		g_assert_nonnull(media);
		g_assert_nonnull(activity);
		g_assert_true(media < activity);
		g_assert_null(strstr(reply.body, ">Series</a>"));
		g_assert_nonnull(movies);
		g_assert_nonnull(tv);
		g_assert_nonnull(pages);
		g_assert_true(movies < tv && tv < pages);
		assert_contains(pages, "href=\"/series/calendar\"");
		assert_contains(pages, "href=\"/series/wanted\"");
		assert_contains(pages, "href=\"/series/history\"");
		assert_contains(reply.body, "<h1>All series</h1>");
	}
	save_page("series.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series?state=attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Harbour Lights");
	assert_lacks(reply.body, "Night Ferry");
	axis_test_reply_clear(&reply);

	/* A series' page: its seasons, each episode with its state and why. */
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT, s.harbour);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	save_page("series-page.html", reply.body);
	assert_contains(reply.body, "Harbour Lights");
	assert_contains(reply.body, "Season 1");
	assert_contains(reply.body, "Season 2");
	assert_contains(reply.body, "Specials");
	assert_contains(reply.body, "High Water (1)");
	assert_contains(reply.body, "film-mark-in_library");
	assert_contains(reply.body, "film-mark-upgrade_wanted");
	assert_contains(reply.body, "film-mark-downloading");
	assert_contains(reply.body, "film-mark-searching");
	assert_contains(reply.body, "film-mark-attention");
	assert_contains(reply.body, "film-mark-not_aired");
	assert_contains(reply.body, "film-mark-unmonitored");
	assert_contains(reply.body, "No air date is known");
	assert_contains(reply.body, "40%");
	assert_contains(reply.body, "numbered for episodes that TMDb's episode list does not have");
	/* The latest season open, the others closed. */
	assert_contains(reply.body, "<details id=\"season-2\" class=\"season\" open=\"open\">");
	assert_contains(reply.body, "<details id=\"season-1\" class=\"season\">");
	assert_contains(reply.body, "<details id=\"season-0\" class=\"season\">");
	assert_contains(reply.body, "action=\"/series/");
	/* Spec (web-shell, tidy-labels): A link read alone names the page it opens. */
	assert_contains(reply.body, ">Series overview</a>");
	assert_lacks(reply.body, "Its title page");
	/* Spec (web-shell, tidy-labels): One state word for movies and TV; an episode that needs attention. */
	assert_contains(reply.body, ">Needs attention<");
	assert_lacks(reply.body, ">Attention<");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), >=, 3);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	/* With one season open, as a link to it asks. */
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=1", s.harbour);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<details id=\"season-1\" class=\"season\" open=\"open\">");
	assert_contains(reply.body, "<details id=\"season-2\" class=\"season\">");
	save_page("series-page-season.html", reply.body);
	axis_test_reply_clear(&reply);
	/* For a viewer: the same facts, nothing to do. */
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "film-mark-attention");
	assert_lacks(reply.body, "action=\"/series/");
	assert_lacks(reply.body, "Search now");
	assert_lacks(reply.body, "Remove");
	axis_test_reply_clear(&reply);

	/* An episode's releases: what each holds and why not. */
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT, s.harbour,
		episode_id(fx, s.harbour, 2, 1));
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "S02E01");
	assert_contains(reply.body, "Harbour.Lights.S02E09.1080p.WEB-DL.DDP5.1.H.264-NTb");
	assert_contains(reply.body, "no such episode in the episode list");
	assert_contains(reply.body, "only an absolute episode number");
	assert_contains(reply.body, "Fixture Street");
	/* The profile's rejection may be grabbed by hand (on its row, and beside
	 * what the search ended in, the one there is); a fixed rule's not. */
	g_assert_cmpuint(count_of(reply.body, ">Grab it anyway</button>"), ==, 2);
	assert_csp_clean(reply.body);
	save_page("releases-episode.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "no such episode in the episode list");
	assert_lacks(reply.body, ">Grab it anyway<");
	axis_test_reply_clear(&reply);

	/* A season's releases, with the pack and why it was not taken. */
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?season=2", s.harbour);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, PACK_S02);
	assert_contains(reply.body, "S02");
	assert_contains(reply.body, "S02E03, which is monitored and not wanted");
	assert_contains(reply.body, "A season search takes the whole season");
	assert_csp_clean(reply.body);
	save_page("releases-season.html", reply.body);
	axis_test_reply_clear(&reply);
	/* Neither: refused. */
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases", s.harbour);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* The calendar: what airs, by day. */
	reply = get_as(fx, fx->admin, "/series/calendar");
	save_page("calendar.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/wanted");
	save_page("wanted.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/history");
	save_page("history.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/");
	save_page("dashboard.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series/calendar");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Crossing");
	assert_contains(reply.body, "Undertow");
	assert_contains(reply.body, "Fog Bank");
	assert_contains(reply.body, "Friday 20 November");
	assert_contains(reply.body, "Saturday 14 November, today");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/calendar");
	save_page("calendar.html", reply.body);
	axis_test_reply_clear(&reply);

	/* Wanted, with the reasons in words. */
	reply = get_as(fx, fx->viewer, "/series/wanted");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Fixture Street");
	assert_contains(reply.body, "Night Ferry");
	assert_contains(reply.body, "Departure");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), ==, 1);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/wanted");
	save_page("wanted.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/wanted?state=attention");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Return");
	assert_lacks(reply.body, "Departure");
	axis_test_reply_clear(&reply);

	/* History, across series. */
	reply = get_as(fx, fx->viewer, "/series/history");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Grabbed");
	assert_contains(reply.body, "Harbour Lights (2024)");
	assert_contains(reply.body, "root");
	g_assert_cmpuint(axis_test_web_assert_tables(reply.body), ==, 1);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/history");
	save_page("history.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A series' title page: its state and a link; a title not added: the
	 * form for an administrator, a line for others. */
	g_free(path);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, qi(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT,
		s.harbour));
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Episodes and downloads");
	assert_contains(reply.body, "film-mark-attention");
	/* Spec (web-shell, tidy-labels): A count with its noun and its whole, here as on the list. */
	assert_contains(reply.body, "2 of 6 aired episodes in the library");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT, s.unmanaged);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "action=\"/series\"");
	assert_contains(reply.body, "name=\"monitor\"");
	assert_contains(reply.body, "name=\"root\"");
	assert_contains(reply.body, "name=\"profile\"");
	assert_contains(reply.body, "Add to TV");
	assert_contains(reply.body, "series on Disk one");
	/* Spec (add-phone-layout): Adding from a phone. */
	{
		const gchar *options = strstr(reply.body, "<details class=\"add-options\"><summary>Options<span "
			"class=\"add-options-chosen\">");
		const gchar *button = strstr(reply.body, ">Add to TV</button>");

		g_assert_nonnull(options);
		g_assert_nonnull(button);
		g_assert_true(options < strstr(reply.body, "name=\"monitor\""));
		g_assert_true(strstr(options, "name=\"search\"") < strstr(options, "</details>"));
		g_assert_true(strstr(options, "</details>") < button);
		g_assert_nonnull(g_strstr_len(options, strstr(options, "</summary>") - options, ", series on Disk one"));
	}
	assert_csp_clean(reply.body);
	save_page("title-add.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "Not among your series. An administrator can add it.");
	assert_lacks(reply.body, "Add to TV");
	axis_test_reply_clear(&reply);

	/* The dashboard's parts. */
	/* (What needs attention is the board's: add-attention-notices, N2; a
	 * series is one, whatever the number of its episodes.) */
	axis_test_web_read_attention(fx->web);
	reply = get_as(fx, fx->admin, "/");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Episodes airing today");
	assert_contains(reply.body, "Night Ferry (2026) S01E02, Crossing");
	assert_contains(reply.body, "attention-level\">Waiting for you</span><a href=\"/series/wanted?state=attention\">");
	assert_contains(reply.body, " attention");
	assert_contains(reply.body, " because ");
	save_page("dashboard.html", reply.body);
	axis_test_reply_clear(&reply);

	/* No such series. */
	reply = get_as(fx, fx->admin, "/series/999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

/* ── Who may act ────────────────────────────────────────────────── */

static void
test_actions(Fixture *fx, gconstpointer data)
{
	static const gchar *const routes[] = {
		"search", "monitor", "grab", "give-up", "blocklist/clear", "edit", "sync", "remove", NULL
	};
	gint64 title = title_of(fx, "202");
	g_autofree gchar *page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
	g_autofree gchar *form = NULL;
	g_autofree gchar *series = NULL;
	g_autofree gchar *series_page = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, fx->root_id);
	g_autofree gchar *n = NULL;
	AxisTestReply reply;
	guint i;

	form = g_strdup_printf("title=%" G_GINT64_FORMAT "&root=%s&monitor=all", title, root);
	/* A viewer cannot add, whatever the token. */
	reply = post_as(fx, fx->viewer, page, "/series", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	/* An administrator without the token cannot either. */
	reply = post_as(fx, fx->admin, NULL, "/series", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT count(*) FROM series");
	g_assert_cmpstr(n, ==, "0");
	/* Not complete: refused, with why. */
	reply = post_as(fx, fx->admin, page, "/series", "monitor=all");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	/* With it: added, and its page says so. */
	reply = post_as(fx, fx->admin, page, "/series", form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=added");
	axis_test_reply_clear(&reply);
	series = q(fx, "SELECT id FROM series");
	g_assert_nonnull(series);
	series_page = g_strdup_printf("/series/%s", series);
	{
		g_autofree gchar *location = g_strdup_printf("%s?done=added", series_page);

		reply = get_as(fx, fx->admin, location);
		assert_contains(reply.body, "The series was added");
		axis_test_reply_clear(&reply);
	}
	/* Twice: refused, with why. */
	reply = post_as(fx, fx->admin, page, "/series", form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "among your series already");
	axis_test_reply_clear(&reply);

	/* A viewer is refused every action, with or without the token. */
	for (i = 0; routes[i] != NULL; i++) {
		g_free(action);
		action = g_strdup_printf("/series/%s/%s", series, routes[i]);
		reply = post_as(fx, fx->viewer, series_page, action, "scope=series&season=1&confirm=yes&candidate=1&grab=1");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, NULL, action, "scope=series&season=1&confirm=yes&candidate=1&grab=1");
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}
	g_free(n);
	n = q(fx, "SELECT string_agg(kind, ',') FROM series_event");
	g_assert_cmpstr(n, ==, "added");

	/* Search: an episode, then the whole series. */
	g_free(action);
	action = g_strdup_printf("/series/%s/search", series);
	g_free(form);
	form = g_strdup_printf("scope=episode&item=%" G_GINT64_FORMAT "&season=2", episode_id(fx, g_ascii_strtoll(series,
		NULL, 10), 2, 2));
	reply = post_as(fx, fx->admin, series_page, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=searching&season=2");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "scope=episode");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "scope=season&item=1");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND state = 'queued'");
	g_assert_cmpstr(n, ==, "2");

	/* Monitoring: a season off, an episode on. */
	g_free(action);
	action = g_strdup_printf("/series/%s/monitor", series);
	reply = post_as(fx, fx->admin, series_page, action, "season=1");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("episode=%" G_GINT64_FORMAT "&monitored=yes", episode_id(fx, g_ascii_strtoll(series, NULL,
		10), 1, 2));
	reply = post_as(fx, fx->admin, series_page, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT string_agg(number::text, ',' ORDER BY number) FROM series_episode WHERE season_number = 1"
		" AND monitored");
	g_assert_cmpstr(n, ==, "2");

	/* Its options; the blocklist; following the metadata. */
	g_free(action);
	action = g_strdup_printf("/series/%s/edit", series);
	g_free(form);
	form = g_strdup_printf("root=%s&profile=%" G_GINT64_FORMAT, root, qi(fx, "SELECT id FROM quality_profile WHERE"
		" slug = 'best_1080p'"));
	reply = post_as(fx, fx->admin, series_page, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT p.slug FROM series s JOIN quality_profile p ON p.id = s.quality_profile_id");
	g_assert_cmpstr(n, ==, "best_1080p");
	g_free(action);
	action = g_strdup_printf("/series/%s/blocklist/clear", series);
	reply = post_as(fx, fx->admin, series_page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(action);
	action = g_strdup_printf("/series/%s/sync", series);
	reply = post_as(fx, fx->admin, series_page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=synced");
	axis_test_reply_clear(&reply);
	/* A release that is not one of its searches': not found. */
	g_free(action);
	action = g_strdup_printf("/series/%s/grab", series);
	reply = post_as(fx, fx->admin, series_page, action, "candidate=424242");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* Removing asks to be confirmed. */
	g_free(action);
	action = g_strdup_printf("/series/%s/remove", series);
	reply = post_as(fx, fx->admin, series_page, action, NULL);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "confirm=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "/series?done=removed");
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT count(*) FROM series");
	g_assert_cmpstr(n, ==, "0");

	/* Every action, with the administrator who took it. */
	g_free(n);
	n = q(fx, "SELECT string_agg(kind || ':' || coalesce(by_user, '-'), ',' ORDER BY id) FROM series_event");
	g_assert_cmpstr(n, ==, "added:root,searched:root,searched:root,monitored:root,monitored:root,changed:root,"
		"removed:root");
}

/* ── With the module off ────────────────────────────────────────── */

/* ── What needs attention, and what puts it right ───────────────── */

#define LANTERN_E01 "Lantern.Bay.S01E01.1080p.WEB-DL.DDP5.1.H.264-NTb"

static gint64
profile_id(
	Fixture     *fx,
	const gchar *slug
){
	return qi(fx, "SELECT id FROM quality_profile WHERE slug = '%s'", slug);
}

static gint64
add_series_as(
	Fixture       *fx,
	const gchar   *provider_id,
	AxisTvMonitor  monitor,
	const gchar   *slug
){
	AxisTvSpec spec;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.title_id = title_of(fx, provider_id);
	spec.profile_id = profile_id(fx, slug);
	spec.root_id = fx->root_id;
	spec.monitor = monitor;
	spec.user = "root";
	g_assert_true(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_no_error(error);
	return id;
}

/* Whether the profile choice of @id on a releases page is marked as
 * accepting what was found (the nearest mark before its input). */
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

/* fix-attention-actionable: an attention statement says what is wrong and
 * links to where it is put right; the releases page offers what puts a
 * profile's rejection right (Grab it anyway, another profile marked by what
 * it accepts, the profile's page); choosing one searches again and the
 * release is accepted; a viewer sees the words and no action; episodes with
 * no air date can be searched for, one or a whole season; a new profile
 * searches again from any form, a new destination alone does not, nor a
 * series that monitors nothing. */
static void
test_attention(Fixture *fx, gconstpointer data)
{
	static const Rel lantern[] = { { LANTERN_E01, 1717986918 } };
	g_autofree gchar *series_page = NULL;
	g_autofree gchar *title_page = NULL;
	g_autofree gchar *releases = NULL;
	g_autofree gchar *releases_href = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *root = g_strdup_printf("%" G_GINT64_FORMAT, fx->root_id);
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	gint64 series;
	gint64 e1;
	gint64 e2;
	gint64 balanced;
	gint64 saver;

	/* Lantern Bay: half-hour episodes, two aired, two with no air date. */
	set_route(fx, "/title/series/206", "{\"id\":\"206\",\"kind\":\"series\",\"title\":\"Lantern Bay\",\"year\":2025,"
		"\"status\":\"continuing\",\"runtime\":30,\"overview\":\"A bay, a lantern, a keeper.\","
		"\"ids\":{\"tvdb\":\"370006\"},\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-06\","
		"\"episodes\":4}]}");
	set_route(fx, "/title/series/206/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9601\",\"name\":\"First Light\",\"air_date\":\"2026-11-06\",\"runtime\":30},"
		"{\"number\":2,\"id\":\"9602\",\"name\":\"Second Watch\",\"air_date\":\"2026-11-13\",\"runtime\":30},"
		"{\"number\":3,\"id\":\"9603\",\"name\":\"Third Bell\",\"runtime\":30},"
		"{\"number\":4,\"id\":\"9604\",\"name\":\"Fourth Tide\",\"runtime\":30}]}");
	balanced = profile_id(fx, "balanced");
	saver = profile_id(fx, "space_saver");
	series = add_series_as(fx, "206", AXIS_TV_MONITOR_ALL, "space_saver");
	e1 = episode_id(fx, series, 1, 1);
	e2 = episode_id(fx, series, 1, 2);
	series_page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	title_page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title_of(fx, "206"));
	releases = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT, series, e1);
	releases_href = g_strdup_printf("href=\"%s\"", releases);
	set_releases(fx, lantern, G_N_ELEMENTS(lantern));
	search(fx, series, AXIS_TV_SCOPE_EPISODE, e1);

	/* Rule 1: the title page says what is wrong and links to the release. */
	reply = get_as(fx, fx->admin, title_page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "S01E01 needs attention.");
	assert_contains(reply.body, "1 release was found and the profile Space saver rejected it: too large (1).");
	assert_contains(reply.body, releases_href);
	assert_contains(reply.body, "See what was found and what you can do");
	assert_csp_clean(reply.body);
	save_page("title-attention.html", reply.body);
	axis_test_reply_clear(&reply);
	/* The TV list says it in words, a link to the same place. */
	reply = get_as(fx, fx->admin, "/series?view=overview");
	assert_contains(reply.body, "S01E01 needs attention: 1 release was found and the profile Space saver rejected it");
	assert_contains(reply.body, releases_href);
	axis_test_reply_clear(&reply);
	/* Wanted, its reason a link there too. */
	reply = get_as(fx, fx->viewer, "/series/wanted?state=attention");
	assert_contains(reply.body, releases_href);
	axis_test_reply_clear(&reply);

	/* The series page: an anchor on each episode, the profile with a way to
	 * change it, the settings card named for what it holds, each value
	 * once, plain labels. */
	reply = get_as(fx, fx->admin, series_page);
	g_assert_cmpuint(reply.status, ==, 200);
	{
		g_autofree gchar *anchor = g_strdup_printf("id=\"episode-%" G_GINT64_FORMAT "\"", e1);

		assert_contains(reply.body, anchor);
	}
	assert_contains(reply.body, "Quality profile: Space saver.");
	assert_contains(reply.body, "href=\"#settings\"");
	assert_contains(reply.body, "Change the quality profile");
	assert_contains(reply.body, "id=\"settings\"");
	assert_contains(reply.body, "Settings for this series");
	assert_contains(reply.body, "Details last updated");
	assert_contains(reply.body, ">Update details now<");
	assert_contains(reply.body, ">Remove this series<");
	assert_lacks(reply.body, ">This series<");
	assert_lacks(reply.body, "Followed the metadata");
	assert_lacks(reply.body, "Follow the metadata now");
	assert_lacks(reply.body, "Remove from series");
	g_assert_cmpuint(count_of(reply.body, "<dt>Quality profile</dt>"), ==, 0);
	g_assert_cmpuint(count_of(reply.body, "name=\"profile\""), ==, 1);
	/* No air date: why, and a search by hand, one or the season's. */
	assert_contains(reply.body, "2 monitored episodes of this season have no air date");
	assert_contains(reply.body, ">Search anyway</button>");
	assert_contains(reply.body, ">Search all 2 with no air date</button>");
	assert_csp_clean(reply.body);
	save_page("series-attention.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, series_page);
	g_assert_cmpuint(count_of(reply.body, "<dt>Quality profile</dt>"), ==, 1);
	assert_contains(reply.body, "S01E01 needs attention.");
	assert_contains(reply.body, "an administrator searches for them by hand");
	assert_lacks(reply.body, "Search anyway");
	assert_lacks(reply.body, "Search all 2");
	assert_lacks(reply.body, "Remove this series");
	axis_test_reply_clear(&reply);

	/* Rule 2: the releases page, next to what the search ended in. */
	reply = get_as(fx, fx->admin, releases);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "What you can do");
	assert_contains(reply.body, "is above the");
	assert_contains(reply.body, "limit for a 30-minute episode at WEB-DL 1080p");
	g_assert_cmpuint(count_of(reply.body, ">Grab it anyway</button>"), ==, 2);
	assert_contains(reply.body, "Use another quality profile for this series");
	assert_contains(reply.body, ">Use this profile and search again</button>");
	assert_contains(reply.body, "Change the limits of Space saver itself");
	/* Marked by the search's own decision: the series' profile rejects it,
	 * Balanced takes it, 4K allows no 1080p. */
	g_assert_false(choice_accepts(reply.body, saver));
	g_assert_true(choice_accepts(reply.body, balanced));
	g_assert_false(choice_accepts(reply.body, profile_id(fx, "uhd")));
	assert_contains(reply.body, ": accepts this release");
	assert_contains(reply.body, " (the series' profile now)");
	assert_csp_clean(reply.body);
	save_page("releases-attention.html", reply.body);
	axis_test_reply_clear(&reply);
	/* Rule 3: a viewer reads why, and has nothing to press. */
	reply = get_as(fx, fx->viewer, releases);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "too large (1)");
	assert_contains(reply.body, "An administrator can grab a release anyway");
	assert_lacks(reply.body, "profile-choice");
	assert_lacks(reply.body, ">Grab it anyway<");
	assert_lacks(reply.body, "Use this profile");
	axis_test_reply_clear(&reply);

	/* Choosing Balanced from there: by the edit's own rules. */
	action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/edit", series);
	{
		g_autofree gchar *back = g_uri_escape_string(releases, NULL, TRUE);

		form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%s&episode=%" G_GINT64_FORMAT "&back=%s", balanced,
			root, e1, back);
	}
	reply = post_as(fx, fx->viewer, releases, action, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, action, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT quality_profile_id FROM series WHERE id = %" G_GINT64_FORMAT, series), ==, saver);
	reply = post_as(fx, fx->admin, releases, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), releases);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=profile");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT quality_profile_id FROM series WHERE id = %" G_GINT64_FORMAT, series), ==, balanced);
	/* Searched again at once, the cause said. */
	n = q(fx, "SELECT payload->>'cause' FROM job WHERE kind = 'tv.search' AND state = 'queued' AND dedupe_key ="
		" 'tv.search:%" G_GINT64_FORMAT ":episode:%" G_GINT64_FORMAT "'", series, e1);
	g_assert_cmpstr(n, ==, "profile");
	g_free(n);
	n = q(fx, "SELECT text FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'changed'", series);
	assert_contains(n, "Changed by root: the quality profile to Balanced.");
	assert_contains(n, "queued for its wanted episodes with it");
	{
		g_autofree gchar *location = g_strdup_printf("%s&done=profile", releases);

		reply = get_as(fx, fx->admin, location);
		assert_contains(reply.body, "The quality profile is Balanced now.");
		axis_test_reply_clear(&reply);
	}
	/* The search the change queued (its job's work): the release is
	 * accepted now, and grabbed. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, e1, "profile", "root", NULL, &report,
		&error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	g_free(n);
	n = q(fx, "SELECT cause || ',' || outcome FROM series_search WHERE series_id = %" G_GINT64_FORMAT
		" ORDER BY id DESC LIMIT 1", series);
	g_assert_cmpstr(n, ==, "profile,grabbed");
	g_free(n);
	n = q(fx, "SELECT release_name FROM series_grab WHERE series_id = %" G_GINT64_FORMAT, series);
	g_assert_cmpstr(n, ==, LANTERN_E01);
	reply = get_as(fx, fx->admin, releases);
	assert_contains(reply.body, ">Grabbed<");
	assert_lacks(reply.body, "What you can do");
	axis_test_reply_clear(&reply);

	/* A new destination alone searches for nothing. */
	{
		gint64 other = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "series-two", AXIS_MEDIA_TV);
		gint64 before = qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'");

		g_free(form);
		form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT, balanced, other);
		reply = post_as(fx, fx->admin, series_page, action, form);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "done=saved");
		axis_test_reply_clear(&reply);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, before);
		g_assert_cmpint(qi(fx, "SELECT root_id FROM series WHERE id = %" G_GINT64_FORMAT, series), ==, other);
	}

	/* Rule 2, nothing found: when the next search is, and one now.  (The
	 * searches the new profile queued, the season's among them, ran.) */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND payload->>'cause' = 'profile'"
		" AND payload->>'scope' = 'season'"), ==, 1);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	set_releases(fx, NULL, 0);
	search(fx, series, AXIS_TV_SCOPE_EPISODE, e2);
	g_free(releases);
	releases = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT, series, e2);
	reply = get_as(fx, fx->admin, releases);
	assert_contains(reply.body, "What you can do");
	assert_contains(reply.body, "Axis searches again by itself after");
	assert_contains(reply.body, ">Search again now</button>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, releases);
	assert_contains(reply.body, "Axis searches again by itself after");
	assert_lacks(reply.body, "Search again now");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, series_page);
	assert_contains(reply.body, "The last search found no release. The next automatic search is after");
	axis_test_reply_clear(&reply);

	/* No air date: the whole season's, each searched for by itself. */
	g_free(action);
	action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/search", series);
	reply = post_as(fx, fx->viewer, series_page, action, "scope=undated&item=1");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "scope=undated&item=1");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=searching_undated");
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT string_agg(payload->>'cause', ',' ORDER BY id) FROM job WHERE kind = 'tv.search' AND dedupe_key IN"
		" ('tv.search:%" G_GINT64_FORMAT ":episode:%" G_GINT64_FORMAT "', 'tv.search:%" G_GINT64_FORMAT ":episode:%"
		G_GINT64_FORMAT "')", series, episode_id(fx, series, 1, 3), series, episode_id(fx, series, 1, 4));
	g_assert_cmpstr(n, ==, "manual,manual");
	g_free(n);
	n = q(fx, "SELECT text FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'searched'"
		" ORDER BY id DESC LIMIT 1", series);
	g_assert_cmpstr(n, ==, "A search of the 2 episodes of season 1 with no air date asked for by root: 2 searches "
		"queued.");
	/* Asked again while they are queued: nothing more. */
	reply = post_as(fx, fx->admin, series_page, action, "scope=undated&item=1");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=already_searching");
	axis_test_reply_clear(&reply);

	/* A series that monitors nothing: a new profile searches for nothing. */
	{
		gint64 ferry = add_series_as(fx, "204", AXIS_TV_MONITOR_NONE, "space_saver");
		g_autofree gchar *ferry_edit = g_strdup_printf("/series/%" G_GINT64_FORMAT "/edit", ferry);
		g_autofree gchar *ferry_page = g_strdup_printf("/series/%" G_GINT64_FORMAT, ferry);

		g_free(form);
		form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%s", balanced, root);
		reply = post_as(fx, fx->admin, ferry_page, ferry_edit, form);
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "done=profile_none");
		axis_test_reply_clear(&reply);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND payload->>'series' = '%"
			G_GINT64_FORMAT "'", ferry), ==, 0);
		g_free(n);
		n = q(fx, "SELECT text FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'changed'", ferry);
		assert_contains(n, "Nothing of it is wanted under it now, so nothing is searched for.");
	}
}

/* ── A manual search ────────────────────────────────────────────── */

#define TIDE_E01     "Tidewater.Inn.S01E01.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define TIDE_E02     "Tidewater.Inn.S01E02.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define TIDE_E03     "Tidewater.Inn.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define TIDE_BEYOND  "Tidewater.Inn.S01E05E06.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define TIDE_PACK    "Tidewater.Inn.S01.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define TIDE_OTHER   "Fixture.Street.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb"

typedef struct
{
	const gchar *title;
	gint64       size;
	const gchar *indexer;
} IndexedRel;

/* What the fake Prowlarr answers from now on: Usenet releases, each from the
 * indexer named. */
static void
set_indexed_releases(
	Fixture          *fx,
	const IndexedRel *rels,
	guint             n
){
	g_autofree gchar *origin = axis_test_prowlarr_base(fx->prowlarr);
	g_autoptr(GString) body = g_string_new("[");
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_printf(body, "%s{\"guid\":\"https://indexer.example/m%u\",\"title\":\"%s\",\"size\":%"
			G_GINT64_FORMAT ",\"indexerId\":3,\"indexer\":\"%s\",\"publishDate\":\"2026-11-13T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=M%u&file=hearth-nzb\",\"magnetUrl\":null,"
			"\"protocol\":\"usenet\",\"categories\":[{\"id\":5040}]}", i > 0 ? "," : "", i, rels[i].title,
			rels[i].size, rels[i].indexer, origin, KEY, i);
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, NULL, body->str);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

/* Where @needle first is in @haystack read plainly, as an offset (-1: not). */
static gssize
offset_of(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *plain = plain_of(haystack);
	const gchar *at = strstr(plain, needle);

	return at != NULL ? at - plain : -1;
}

/* The latest manual search's release named @name. */
static gint64
listed_candidate(
	Fixture     *fx,
	const gchar *name
){
	return qi(fx, "SELECT c.id FROM series_candidate c JOIN series_search s ON s.id = c.search_id WHERE s.cause = 'list'"
		" AND c.name = '%s' ORDER BY c.id DESC LIMIT 1", name);
}

/* add-manual-search: Manual search on every level (a series, a season, an
 * episode) lists everything the indexers answered, grouped, sorted and
 * filtered by links, grabs nothing, moves no schedule and changes no state;
 * a season search that found single episodes lets each be taken; an episode
 * with no air date, not monitored, is filled by hand; another series' is
 * ruled out; a downgrade needs the tick; a release numbered beyond the data
 * is taken for what the data has; a viewer sees the list and no grab. */
static void
test_manual_search(Fixture *fx, gconstpointer data)
{
	static const IndexedRel found[] = {
		{ TIDE_E01, 1500000000, "Harbourmaster" },
		{ TIDE_E02, 1400000000, "Lamplight" },
		{ TIDE_E03, 1300000000, "Harbourmaster" },
		{ TIDE_BEYOND, 2900000000, "Lamplight" },
		{ TIDE_PACK, 7000000000, "Harbourmaster" },
		{ TIDE_OTHER, 1500000000, "Lamplight" }
	};
	g_autofree gchar *series_page = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *n = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	gint64 series;
	gint64 e1;
	gint64 e2;
	gint64 e3;
	gint64 e5;
	gint64 candidate;
	gssize pack_at;
	gssize beyond_at;
	gssize e1_at;

	/* Tidewater Inn: two episodes aired, three with no air date. */
	set_route(fx, "/title/series/207", "{\"id\":\"207\",\"kind\":\"series\",\"title\":\"Tidewater Inn\",\"year\":2026,"
		"\"status\":\"continuing\",\"runtime\":45,\"overview\":\"Rooms by the tide.\",\"ids\":{\"tvdb\":\"370007\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-06\",\"episodes\":5}]}");
	set_route(fx, "/title/series/207/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9701\",\"name\":\"Low Water\",\"air_date\":\"2026-11-06\",\"runtime\":45},"
		"{\"number\":2,\"id\":\"9702\",\"name\":\"Spring Tide\",\"air_date\":\"2026-11-13\",\"runtime\":45},"
		"{\"number\":3,\"id\":\"9703\",\"name\":\"Neap\",\"runtime\":45},"
		"{\"number\":4,\"id\":\"9704\",\"name\":\"Slack Water\",\"runtime\":45},"
		"{\"number\":5,\"id\":\"9705\",\"name\":\"High Water\",\"runtime\":45}]}");
	series = add_series(fx, "207", AXIS_TV_MONITOR_ALL);
	e1 = episode_id(fx, series, 1, 1);
	e2 = episode_id(fx, series, 1, 2);
	e3 = episode_id(fx, series, 1, 3);
	e5 = episode_id(fx, series, 1, 5);
	/* S01E02 is in the library as a BluRay; S01E03 is not monitored. */
	in_library(fx, series, e2, "Tidewater Inn (2026)/Season 01/S01E02.mkv", "bluray_1080p");
	g_assert_true(axis_tv_set_episode_monitored(fx->tv, series, e3, FALSE, "root", &error));
	g_assert_no_error(error);
	set_indexed_releases(fx, found, G_N_ELEMENTS(found));
	series_page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual", series);
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=season&item=1", series);
	grab = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/grab", series);

	/* Rule 1: on every level, for an administrator. */
	reply = get_as(fx, fx->admin, series_page);
	assert_contains(reply.body, ">Manual search of the whole series</button>");
	assert_contains(reply.body, ">Manual search of the season</button>");
	g_assert_cmpuint(count_of(reply.body, ">Manual search</button>"), ==, 5);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, series_page);
	assert_lacks(reply.body, "Manual search");
	axis_test_reply_clear(&reply);

	/* Asked for: queued with a key of its own, said in the history; asked
	 * again, nothing more; a viewer is refused. */
	reply = post_as(fx, fx->viewer, series_page, action, "level=season&item=1");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "level=season&item=1");
	g_assert_cmpuint(reply.status, ==, 303);
	{
		g_autofree gchar *location = g_strdup_printf("%s&done=searching", page);

		assert_contains(axis_test_reply_header(&reply, "Location"), location);
	}
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT payload->>'cause' FROM job WHERE kind = 'tv.search' AND dedupe_key = 'tv.search:%" G_GINT64_FORMAT
		":list:season:1'", series);
	g_assert_cmpstr(n, ==, "list");
	g_free(n);
	n = q(fx, "SELECT text FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'searched'", series);
	assert_contains(n, "A manual search of season 1 asked for by root: it lists what the indexers have and grabs "
		"nothing by itself.");
	reply = post_as(fx, fx->admin, series_page, action, "level=season&item=1");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=already_searching");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "level=season");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "level=album&item=1");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	/* While it runs: said, followed by htmx, a plain reload link; no
	 * episode shows as searching. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	/* (add-activity-feedback) Where it stands: queued, waiting its turn. */
	assert_contains(reply.body, "A manual search of season 1 is waiting for its turn, queued");
	assert_contains(reply.body, "hx-trigger=\"every 5s\"");
	{
		g_autofree gchar *part = g_strdup_printf("hx-get=\"%s&part=results\"", page);

		assert_contains(reply.body, part);
	}
	assert_contains(reply.body, ">Reload this page<");
	assert_csp_clean(reply.body);
	save_page("manual-running.html", reply.body);
	axis_test_reply_clear(&reply);
	/* Followed and reloaded as it is sorted and filtered. */
	g_free(path);
	path = g_strdup_printf("%s&sort=size&indexer=Lamplight", page);
	reply = get_as(fx, fx->admin, path);
	{
		g_autofree gchar *part = g_strdup_printf("hx-get=\"%s&part=results\"", path);
		g_autofree gchar *reload = g_strdup_printf("href=\"%s\">Reload this page<", path);

		assert_contains(reply.body, part);
		assert_contains(reply.body, reload);
	}
	axis_test_reply_clear(&reply);
	/* A season past any number a season has: no such season, not an
	 * error. */
	reply = post_as(fx, fx->admin, series_page, action, "level=season&item=99999999999");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&part=results", page);
	reply = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "id=\"manual-results\"");
	assert_lacks(reply.body, "<html");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, series_page);
	assert_lacks(reply.body, "A search that holds it is queued");
	axis_test_reply_clear(&reply);

	/* Its job's work: everything stored, nothing grabbed, no schedule
	 * moved, nothing narrower queued. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND (last_search_at IS NOT NULL OR next_search_at IS NOT NULL)", series), ==, 0);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");

	/* Rule 2: everything, grouped, each with its reasons. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Manual search of season 1");
	assert_contains(reply.body, "Acceptable under the profile (0)");
	assert_contains(reply.body, "Rejected by the profile (0)");
	assert_contains(reply.body, "Not what this search asked for (5)");
	assert_contains(reply.body, "Ruled out (1)");
	assert_contains(reply.body, "A season search takes the whole season");
	/* (add-force-grab-and-assign, F2) The one ruled out only by its name
	 * (another series) is offered Grab it by force. */
	g_assert_cmpuint(count_of(reply.body, ">Grab it by force</button>"), ==, 1);
	assert_contains(reply.body, "It also names S01E06, which the series' data does not have");
	assert_contains(reply.body, TIDE_OTHER);
	assert_lacks(reply.body, "hx-trigger=\"every 5s\"");
	/* Rule 3: a season search that found single episodes lets each be
	 * taken; the ruled out one has no button. */
	g_assert_cmpuint(count_of(reply.body, ">Grab this one</button>"), ==, 5);
	assert_csp_clean(reply.body);
	save_page("manual-season.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, TIDE_E03);
	assert_contains(reply.body, "Not what this search asked for (5)");
	/* No grab for a viewer (a reason may name the action, never offer it). */
	assert_lacks(reply.body, "/manual/grab");
	assert_lacks(reply.body, ">Grab this one<");
	assert_lacks(reply.body, "Manual search again");
	axis_test_reply_clear(&reply);

	/* Rule 7: sorted and filtered by links. */
	g_free(path);
	path = g_strdup_printf("%s&sort=size", page);
	reply = get_as(fx, fx->admin, path);
	pack_at = offset_of(reply.body, TIDE_PACK);
	beyond_at = offset_of(reply.body, TIDE_BEYOND);
	e1_at = offset_of(reply.body, TIDE_E01);
	g_assert_cmpint(pack_at, >=, 0);
	g_assert_cmpint(pack_at, <, beyond_at);
	g_assert_cmpint(beyond_at, <, e1_at);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpint(offset_of(reply.body, TIDE_E01), <, offset_of(reply.body, TIDE_PACK));
	{
		g_autofree gchar *link = g_strdup_printf("href=\"%s&sort=size\"", page);

		assert_contains(reply.body, link);
	}
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&indexer=Lamplight", page);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, TIDE_E02);
	assert_contains(reply.body, TIDE_OTHER);
	g_assert_cmpint(offset_of(reply.body, TIDE_E01), ==, -1);
	assert_contains(reply.body, "The filter hides 3 releases.");
	assert_contains(reply.body, "Not what this search asked for (2)");
	axis_test_reply_clear(&reply);

	/* An episode with no air date, not monitored: what it fills, the
	 * profile set aside; filled by hand, its monitoring unchanged. */
	candidate = listed_candidate(fx, TIDE_E03);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", grab, candidate);
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Grab " TIDE_E03 " (WEB-DL 1080p) by hand for S01E03 (no air date, not monitored), with "
		"the quality profile Balanced set aside.");
	assert_lacks(reply.body, "name=\"downgrade\"");
	assert_contains(reply.body, ">Grab this one by hand</button>");
	assert_csp_clean(reply.body);
	save_page("manual-confirm.html", reply.body);
	axis_test_reply_clear(&reply);
	/* (A downgrade tick posted where none is needed records nothing.) */
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&downgrade=yes", candidate);
	reply = post_as(fx, fx->viewer, page, grab, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, grab, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=grabbed");
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT set_aside || '|' || replaces_files::text FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT,
		candidate);
	g_assert_cmpstr(n, ==, "hand|[]");
	{
		g_autofree gchar *line = g_strdup_printf("audit: series %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT
			" grabbed by hand from a manual search, the quality profile set aside by=root", series, candidate);

		g_assert_cmpuint(logged(line), ==, 1);
	}
	g_free(n);
	n = q(fx, "SELECT string_agg(ge.episode_id::text, ',') FROM series_grab g JOIN series_grab_episode ge ON"
		" ge.grab_id = g.id WHERE g.candidate_id = %" G_GINT64_FORMAT " AND g.manual AND g.grabbed_by = 'root'",
		candidate);
	{
		g_autofree gchar *only = g_strdup_printf("%" G_GINT64_FORMAT, e3);

		g_assert_cmpstr(n, ==, only);
	}
	g_free(n);
	n = q(fx, "SELECT monitored FROM series_episode WHERE id = %" G_GINT64_FORMAT, e3);
	g_assert_cmpstr(n, ==, "f");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'grabbed' AND by_user = 'root' AND text LIKE 'Grabbed by hand " TIDE_E03 "%%Chosen by root by hand%%'",
		series), ==, 1);
	{
		g_autofree gchar *done = g_strdup_printf("%s&done=grabbed", page);

		reply = get_as(fx, fx->admin, done);
		assert_contains(reply.body, "grabbed by hand with the quality profile set aside");
		axis_test_reply_clear(&reply);
	}

	/* Another series: ruled out, never taken. */
	candidate = listed_candidate(fx, TIDE_OTHER);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", grab, candidate);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "cannot be grabbed");
	assert_contains(reply.body, "A fixed rule rules it out");
	assert_lacks(reply.body, "Grab this one by hand");
	/* The ordinary confirmation does not take it, and says where it is taken
	 * (add-force-grab-and-assign, F2). */
	assert_contains(reply.body, "may grab it by force, from the manual search's list");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes", candidate);
	reply = post_as(fx, fx->admin, page, grab, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT, candidate), ==, 0);

	/* A downgrade: said, and only with the second tick. */
	candidate = listed_candidate(fx, TIDE_E02);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", grab, candidate);
	reply = get_as(fx, fx->admin, confirm);
	assert_contains(reply.body, "replacing S01E02's file (BluRay 1080p: a downgrade)");
	assert_contains(reply.body, "name=\"downgrade\"");
	assert_contains(reply.body, "Replace S01E02's file, which this release is not better than");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes", candidate);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "tick the box");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT, candidate), ==, 0);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&downgrade=yes", candidate);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	/* The tick is recorded for that very file, with the quality shown. */
	g_free(n);
	n = q(fx, "SELECT replaces_files = jsonb_build_array(jsonb_build_object('f', (SELECT file_id FROM"
		" series_file_episode WHERE episode_id = %" G_GINT64_FORMAT "), 'q', 'bluray_1080p', 'r', null))"
		" FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT, e2, candidate);
	g_assert_cmpstr(n, ==, "t");
	{
		g_autofree gchar *line = g_strdup_printf("audit: series %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT
			" grabbed by hand from a manual search, the quality profile set aside, replacing a file it is not"
			" better than as ticked by=root", series, candidate);

		g_assert_cmpuint(logged(line), ==, 1);
	}

	/* Numbered beyond the data: taken for what the data has, with a
	 * warning and a tick of its own. */
	candidate = listed_candidate(fx, TIDE_BEYOND);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", grab, candidate);
	reply = get_as(fx, fx->admin, confirm);
	assert_contains(reply.body, "by hand for S01E05 (no air date), with the quality profile Balanced set aside.");
	assert_contains(reply.body, "This release numbers its episodes differently from the series' data (it names "
		"S01E06, which the data does not have). Files are placed by number, so they may land on the wrong "
		"episodes.");
	assert_contains(reply.body, "name=\"numbering\"");
	assert_lacks(reply.body, "name=\"downgrade\"");
	save_page("manual-confirm-numbering.html", reply.body);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes", candidate);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "tick the box");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT, candidate), ==, 0);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&numbering=yes", candidate);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT numbering_ok FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT, candidate);
	g_assert_cmpstr(n, ==, "t");
	{
		g_autofree gchar *line = g_strdup_printf("audit: series %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT
			" grabbed by hand from a manual search, the quality profile set aside, numbered differently from the"
			" data as ticked by=root", series, candidate);

		g_assert_cmpuint(logged(line), ==, 1);
	}
	g_free(n);
	n = q(fx, "SELECT string_agg(ge.episode_id::text, ',') FROM series_grab g JOIN series_grab_episode ge ON"
		" ge.grab_id = g.id WHERE g.candidate_id = %" G_GINT64_FORMAT, candidate);
	{
		g_autofree gchar *only = g_strdup_printf("%" G_GINT64_FORMAT, e5);

		g_assert_cmpstr(n, ==, only);
	}

	/* The other levels: an episode, the whole series, each its own key. */
	g_free(form);
	form = g_strdup_printf("level=episode&item=%" G_GINT64_FORMAT, e1);
	reply = post_as(fx, fx->admin, series_page, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, series_page, action, "level=series");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "manual?level=series&done=searching");
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT string_agg(substr(dedupe_key, length('tv.search:%" G_GINT64_FORMAT ":') + 1), ',' ORDER BY id)"
		" FROM job WHERE kind = 'tv.search'", series);
	{
		g_autofree gchar *keys = g_strdup_printf("list:episode:%" G_GINT64_FORMAT ",list:series", e1);

		g_assert_cmpstr(n, ==, keys);
	}
	/* The automatic releases page leads to it. */
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?season=1", series);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, ">Manual search</button>");
	assert_contains(reply.body, ">The last manual search<");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, ">The last manual search<");
	assert_lacks(reply.body, ">Manual search</button>");
	axis_test_reply_clear(&reply);
	/* Had an automatic search found these, the release numbered beyond the
	 * data would have no one-click grab there: only Grab this one. */
	axis_test_db_exec(fx->t->db, "UPDATE series_search SET cause = 'manual', outcome = 'none_acceptable' WHERE"
		" cause = 'list' AND scope = 'season'");
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(count_of(reply.body, ">Grab it anyway</button>"), ==, 4);
	assert_contains(reply.body, ">Only by a manual search<");
	axis_test_reply_clear(&reply);
}

/* ── fix-manual-search-table ────────────────────────────────────── */

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
	g_autofree gchar *sql = g_strdup_printf("UPDATE series_candidate SET accepted = false, safety = false,"
		" blocklisted = false, reasons = '[]', size_bytes = %s, published_at = %s, quality = '%s', indexer = %s,"
		" holds = NULL WHERE name = '%s'", size, published, quality, indexer, name);

	axis_test_db_exec(fx->t->db, sql);
}

/* Whether @names are in @body in this order, each after the one before. */
static void
assert_in_order(
	const gchar        *body,
	const gchar *const *names
){
	g_autofree gchar *plain = plain_of(body);
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
 * fix-manual-search-table, for a season's manual search: the Size heading
 * and its cells agree (in the card swapped in while a search runs too); each
 * heading sorts the tables by the column, toggling, from a fixed list,
 * unknown values last either way; the order rides with the page's forms; the
 * ruled out group is a disclosure, open when short, anchored into or just
 * sorted, its rows that can be grabbed by force first.
 */
static void
test_manual_sort(Fixture *fx, gconstpointer data)
{
	static const IndexedRel found[] = {
		{ TIDE_E01, 3221225472, "Beacon" },
		{ TIDE_E02, 2147483648, "Anchor" },
		{ TIDE_E03, 1073741824, "Beacon" },
		{ TIDE_BEYOND, 2147483648, "Cairn" },
		{ TIDE_PACK, 4294967296, "Ember" }
	};
	/* Alpha, Bravo, Charlie, Delta and Echo of the movie test. */
	static const gchar *const by_size_desc[] = { TIDE_PACK, TIDE_E01, TIDE_BEYOND, TIDE_E03, TIDE_E02, NULL };
	static const gchar *const by_size_asc[] = { TIDE_E03, TIDE_BEYOND, TIDE_E01, TIDE_PACK, TIDE_E02, NULL };
	static const gchar *const by_age_asc[] = { TIDE_E03, TIDE_E01, TIDE_BEYOND, TIDE_E02, TIDE_PACK, NULL };
	static const gchar *const by_age_desc[] = { TIDE_BEYOND, TIDE_E01, TIDE_E03, TIDE_E02, TIDE_PACK, NULL };
	static const gchar *const by_quality_desc[] = { TIDE_E03, TIDE_E01, TIDE_E02, TIDE_BEYOND, TIDE_PACK, NULL };
	static const gchar *const by_quality_asc[] = { TIDE_BEYOND, TIDE_E02, TIDE_E01, TIDE_E03, TIDE_PACK, NULL };
	static const gchar *const by_found_asc[] = { TIDE_E02, TIDE_E01, TIDE_BEYOND, TIDE_PACK, TIDE_E03, NULL };
	static const gchar *const by_found_desc[] = { TIDE_PACK, TIDE_BEYOND, TIDE_E01, TIDE_E02, TIDE_E03, NULL };
	static const gchar *const by_name_asc[] = { TIDE_PACK, TIDE_E01, TIDE_E02, TIDE_E03, TIDE_BEYOND, NULL };
	static const gchar *const by_name_desc[] = { TIDE_BEYOND, TIDE_E03, TIDE_E02, TIDE_E01, TIDE_PACK, NULL };
	static const gchar *const by_default[] = { TIDE_E01, TIDE_E02, TIDE_E03, TIDE_BEYOND, TIDE_PACK, NULL };
	static const gchar *const forced_first[] = { TIDE_PACK, TIDE_E01, TIDE_E02, TIDE_E03, TIDE_BEYOND, NULL };
	g_autofree gchar *base = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *form = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	gint64 series;
	gint64 candidate;

	set_route(fx, "/title/series/207", "{\"id\":\"207\",\"kind\":\"series\",\"title\":\"Tidewater Inn\",\"year\":2026,"
		"\"status\":\"continuing\",\"runtime\":45,\"overview\":\"Rooms by the tide.\",\"ids\":{\"tvdb\":\"370007\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-06\",\"episodes\":5}]}");
	set_route(fx, "/title/series/207/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9701\",\"name\":\"Low Water\",\"air_date\":\"2026-11-06\",\"runtime\":45},"
		"{\"number\":2,\"id\":\"9702\",\"name\":\"Spring Tide\",\"air_date\":\"2026-11-13\",\"runtime\":45},"
		"{\"number\":3,\"id\":\"9703\",\"name\":\"Neap\",\"runtime\":45},"
		"{\"number\":4,\"id\":\"9704\",\"name\":\"Slack Water\",\"runtime\":45},"
		"{\"number\":5,\"id\":\"9705\",\"name\":\"High Water\",\"runtime\":45}]}");
	series = add_series(fx, "207", AXIS_TV_MONITOR_ALL);
	set_indexed_releases(fx, found, G_N_ELEMENTS(found));
	base =g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=season&item=1", series);
	grab = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/grab", series);
	action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual", series);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	/* Searched long ago, so that asking again is offered. */
	axis_test_db_exec(fx->t->db, "UPDATE series_search SET started_at = now() - interval '2 hours', finished_at ="
		" now() - interval '1 hour'");
	shape(fx, TIDE_E01, "3221225472", "now() - interval '5 days'", "webdl_1080p", "'Beacon'");
	shape(fx, TIDE_E02, "NULL", "NULL", "hdtv_720p", "'Anchor'");
	shape(fx, TIDE_E03, "1073741824", "now() - interval '1 day'", "bluray_1080p", "NULL");
	shape(fx, TIDE_BEYOND, "2147483648", "now() - interval '30 days'", "dvd", "'Cairn'");
	shape(fx, TIDE_PACK, "4294967296", "NULL", "unknown", "'Ember'");

	/* The Size heading and its cells agree, on the page and in the card. */
	reply = get_as(fx, fx->admin, base);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(matches_of(reply.body, "<th[^>]*col-size"), ==, 1);
	g_assert_cmpuint(matches_of(reply.body, "<td[^>]*col-size"), ==, 5);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	path = g_strdup_printf("%s&part=results", base);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(matches_of(reply.body, "<th[^>]*col-size"), ==, 1);
	g_assert_cmpuint(matches_of(reply.body, "<td[^>]*col-size"), ==, 5);
	g_assert_cmpuint(matches_of(reply.body, "<td[^>]*data-label=\"Size\""), ==, 5);
	axis_test_reply_clear(&reply);

	/* Each order, the unknown last whichever way. */
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size&dir=desc", base)), by_size_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size&dir=asc", base)), by_size_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=age&dir=asc", base)), by_age_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=age&dir=desc", base)), by_age_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=quality&dir=desc", base)), by_quality_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=quality&dir=asc", base)), by_quality_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=indexer&dir=asc", base)), by_found_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=indexer&dir=desc", base)), by_found_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=release&dir=asc", base)), by_name_asc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=release&dir=desc", base)), by_name_desc);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size", base)), by_size_desc);

	/* The toggle. */
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=desc", base);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "aria-sort=\"descending\"");
	g_assert_cmpuint(count_of(reply.body, "aria-sort="), ==, 1);
	{
		g_autofree gchar *link = g_strdup_printf("href=\"%s&sort=size&dir=asc&at=manual-group-1#manual-group-1\"", base);
		g_autofree gchar *first = g_strdup_printf("href=\"%s&sort=quality&dir=desc&at=manual-group-1#manual-group-1\"",
			base);

		assert_contains(reply.body, link);
		assert_contains(reply.body, first);
	}
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc", base);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "aria-sort=\"ascending\"");
	{
		g_autofree gchar *link = g_strdup_printf("href=\"%s&sort=size&dir=desc&at=manual-group-1#manual-group-1\"",
			base);

		assert_contains(reply.body, link);
	}
	axis_test_reply_clear(&reply);

	/* A value not in the fixed list is left out. */
	g_free(path);
	path = g_strdup_printf("%s&sort=bogus&dir=desc", base);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_in_order(reply.body, by_default);
	assert_lacks(reply.body, "aria-sort=");
	assert_lacks(reply.body, "bogus");
	axis_test_reply_clear(&reply);
	assert_page_order(fx, (g_free(path), path = g_strdup_printf("%s&sort=size&dir=sideways", base)), by_size_desc);

	/* The order rides with the forms and the way back. */
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc", base);
	reply = get_as(fx, fx->admin, path);
	/* (add-block-release: each row's Block this release carries it too.) */
	g_assert_cmpuint(count_of(reply.body, "name=\"sort\""), ==, 1 + 5 + 5);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("level=season&item=1&sort=size&dir=asc");
	reply = post_as(fx, fx->admin, path, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	{
		g_autofree gchar *location = g_strdup_printf("%s&sort=size&dir=asc&done=", base);

		assert_contains(axis_test_reply_header(&reply, "Location"), location);
	}
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("level=season&item=1&sort=nonsense&dir=asc");
	reply = post_as(fx, fx->admin, path, action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_lacks(axis_test_reply_header(&reply, "Location"), "sort=");
	axis_test_reply_clear(&reply);
	candidate = listed_candidate(fx, TIDE_E01);
	g_free(path);
	path = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1&sort=size&dir=asc", grab, candidate);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "name=\"sort\"");
	{
		g_autofree gchar *back = g_strdup_printf("href=\"%s&sort=size&dir=asc\"", base);

		assert_contains(reply.body, back);
	}
	axis_test_reply_clear(&reply);
	/* After a grab, the page comes back in the order it was in. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&downgrade=yes&sort=quality"
		"&dir=desc", candidate);
	reply = post_as(fx, fx->admin, path, grab, form);
	g_assert_cmpuint(reply.status, ==, 303);
	{
		g_autofree gchar *location = g_strdup_printf("%s&sort=quality&dir=desc&done=grabbed", base);

		assert_contains(axis_test_reply_header(&reply, "Location"), location);
	}
	axis_test_reply_clear(&reply);

	/* The ruled out group is a disclosure: open while short, the rows that
	 * can be grabbed by force first. */
	axis_test_db_exec(fx->t->db, "UPDATE series_candidate SET safety = true, reasons = '[{\"k\": \"safety\", \"c\":"
		" \"password\", \"t\": \"It is password-protected.\"}]'::jsonb");
	axis_test_db_exec(fx->t->db, "UPDATE series_candidate SET reasons = '[{\"k\": \"safety\", \"c\": \"ambiguous\","
		" \"t\": \"It could be more than one title.\"}]'::jsonb WHERE name = '" TIDE_PACK "'");
	reply = get_as(fx, fx->admin, base);
	assert_contains(reply.body, "<summary class=\"manual-fold\">Ruled out (5)</summary>");
	g_assert_true(group_open(reply.body, "manual-group-3"));
	assert_in_order(reply.body, forced_first);
	g_assert_cmpuint(count_of(reply.body, ">Grab it by force</button>"), ==, 1);
	axis_test_reply_clear(&reply);
	/* Many: closed, unless the address says it is the place (the heading's
	 * link does, after a sort of it). */
	axis_test_db_exec(fx->t->db, "INSERT INTO series_candidate (search_id, position, release_id, source_id, name,"
		" protocol, name_key, quality, match, accepted, safety, reasons) SELECT search_id, 100 + g, release_id,"
		" source_id, 'Tide.Sort.Extra.' || g, protocol, name_key || g, 'unknown', 'none', false, true,"
		" '[{\"k\": \"safety\", \"c\": \"password\", \"t\": \"It is password-protected.\"}]'::jsonb FROM"
		" series_candidate, generate_series(1, 16) g WHERE name = '" TIDE_E01 "'");
	reply = get_as(fx, fx->admin, base);
	assert_contains(reply.body, "<summary class=\"manual-fold\">Ruled out (21)</summary>");
	g_assert_false(group_open(reply.body, "manual-group-3"));
	assert_in_order(reply.body, forced_first);
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc&at=manual-group-3", base);
	reply = get_as(fx, fx->admin, path);
	g_assert_true(group_open(reply.body, "manual-group-3"));
	assert_contains(reply.body, "aria-sort=\"ascending\"");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s&sort=size&dir=asc", base);
	reply = get_as(fx, fx->admin, path);
	g_assert_false(group_open(reply.body, "manual-group-3"));
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->t->db, "DELETE FROM series_candidate WHERE name LIKE 'Tide.Sort.Extra.%' AND position > 105");
	reply = get_as(fx, fx->admin, base);
	assert_contains(reply.body, "<summary class=\"manual-fold\">Ruled out (10)</summary>");
	g_assert_true(group_open(reply.body, "manual-group-3"));
	axis_test_reply_clear(&reply);
}

/* ── Grab it by force (add-force-grab-and-assign, F2) ───────────── */

#define FORCE_NO_EP  "Tidewater.Inn.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define FORCE_S02    "Tidewater.Inn.S02E01.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define FORCE_LODGE  "Tidewater.Lodge.S01.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define FORCE_LOCKED "Tidewater.Lodge.S01E04.1080p.WEB-DL.DDP5.1.H.264-NTb"

/* The digest a confirmation page holds (defined with the numbering tests). */
static gchar *
digest_of(const gchar *body);

/* Every open grab is done with: its episodes are free again. */
static void
close_grabs(Fixture *fx)
{
	axis_test_db_exec(fx->t->db, "UPDATE series_grab SET closed_at = now(), close_reason = 'dismissed'"
		" WHERE closed_at IS NULL");
}

/* The episodes the grab of @candidate claimed, "S01E03,S01E04". */
static gchar *
claimed_by(
	Fixture *fx,
	gint64   candidate
){
	return q(fx, "SELECT string_agg('S' || lpad(e.season_number::text, 2, '0') || 'E' || lpad(e.number::text, 2, '0'),"
		" ',' ORDER BY e.season_number, e.number) FROM series_grab g JOIN series_grab_episode ge ON ge.grab_id = g.id"
		" JOIN series_episode e ON e.id = ge.episode_id WHERE g.candidate_id = %" G_GINT64_FORMAT
		" AND g.closed_at IS NULL", candidate);
}

/*
 * An episode with two files made by hand where the kept one is not the one
 * with the lowest id: each has a control of its own, and "Let Axis upgrade
 * again" posts the kept file, not the first.
 */
static void
test_hand_files_controls(Fixture *fx, gconstpointer data)
{
	AxisTestReply reply;
	g_autofree gchar *want = NULL;
	g_autofree gchar *other = NULL;
	gint64 series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	gint64 episode = episode_id(fx, series, 1, 2);
	gint64 first;
	gint64 second;

	in_library(fx, series, episode, "Harbour Lights/Season 01/S01E02.a.mkv", "hdtv_720p");
	in_library(fx, series, episode, "Harbour Lights/Season 01/S01E02.b.mkv", "hdtv_720p");
	first = qi(fx, "SELECT min(file_id) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, episode);
	second = qi(fx, "SELECT max(file_id) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, episode);
	g_assert_cmpint(first, <, second);
	{
		g_autofree gchar *one = g_strdup_printf("UPDATE series_file SET hand_how = 'linked', hand_by = 'root', hand_at = now() WHERE "
			"file_id = %" G_GINT64_FORMAT, first);
		g_autofree gchar *two = g_strdup_printf("UPDATE series_file SET hand_how = 'forced', hand_by = 'root', hand_at = now(), kept = "
			"true WHERE file_id = %" G_GINT64_FORMAT, second);

		axis_test_db_exec(fx->t->db, one);
		axis_test_db_exec(fx->t->db, two);
	}
	reply = get_as(fx, fx->admin, "/series/1");
	g_assert_cmpuint(reply.status, ==, 200);
	want = g_strdup_printf("name=\"file\" value=\"%" G_GINT64_FORMAT "\" /><input type=\"hidden\" name=\"keep\" "
		"value=\"no\"", second);
	other = g_strdup_printf("name=\"file\" value=\"%" G_GINT64_FORMAT "\" /><input type=\"hidden\" name=\"keep\" "
		"value=\"yes\"", first);
	g_assert_nonnull(strstr(reply.body, want));
	g_assert_nonnull(strstr(reply.body, other));
	axis_test_reply_clear(&reply);
}

#define FORCE_OTHER_S02 "Tidewater.Lodge.S02.1080p.WEB-DL.DDP5.1.H.264-NTb"

/*
 * A release named as season 2 found by a search of season 1: a whole season
 * is offered only from the level searched, so the page does not propose a
 * bound it cannot post; it says why and links to season 2's manual search.
 */
static void
test_force_other_season(Fixture *fx, gconstpointer data)
{
	static const IndexedRel found[] = { { FORCE_OTHER_S02, 7000000000, "Harbourmaster" } };
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *force = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	gint64 series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	gint64 candidate;

	set_indexed_releases(fx, found, G_N_ELEMENTS(found));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	candidate = listed_candidate(fx, FORCE_OTHER_S02);
	force = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/force", series);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", force, candidate);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "a whole season is offered only from a search of that season");
	assert_contains(reply.body, "Manual search of season 2");
	assert_contains(reply.body, "/manual?level=season&item=2");
	assert_lacks(reply.body, "name=\"digest\"");
	assert_lacks(reply.body, "It is grabbed for season 2");
	axis_test_reply_clear(&reply);

	/* A form that names more than the shared limit is refused plainly, never
	 * cut short (its digest could not match what the page showed). */
	{
		g_autoptr(GString) form = g_string_new(NULL);
		guint i;

		g_string_append_printf(form, "candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&checked=yes&digest=x",
			candidate);
		for (i = 1; i <= (guint)AXIS_ACQUIRE_FORCE_PICK_MAX + 1; i++)
			g_string_append_printf(form, "&pick_%u=yes", i);
		reply = post_as(fx, fx->admin, confirm, force, form->str);
		g_assert_cmpuint(reply.status, ==, 400);
		assert_contains(reply.body, "more than Axis takes at once");
		axis_test_reply_clear(&reply);
	}
}

/*
 * add-force-grab-and-assign, F2: a release a fixed rule about its name rules
 * out is grabbed by force for the episodes a person names.  The tv delta's
 * scenarios "The name gives no episode", "Numbered for no episode of the
 * data", "A whole season", "Nothing named", "The numbering changed
 * meanwhile" and "Another series" (the ordinary form refuses); and: a file
 * kept by hand is listed with its own tick and not replaced without it, a
 * release that also carries a rule that protects the system is refused, a
 * stale page is a 409, a viewer is offered nothing, and "Grab this one" of a
 * better release asks for the tick of a file kept by hand.
 */
static void
test_force_grab(Fixture *fx, gconstpointer data)
{
	static const IndexedRel found[] = {
		{ FORCE_NO_EP, 1500000000, "Harbourmaster" },
		{ FORCE_S02, 1400000000, "Lamplight" },
		{ FORCE_LODGE, 7000000000, "Harbourmaster" },
		{ FORCE_LOCKED, 1500000000, "Lamplight" },
		{ TIDE_E02, 1400000000, "Lamplight" }
	};
	g_autofree gchar *page = NULL;
	g_autofree gchar *season_page = NULL;
	g_autofree gchar *force = NULL;
	g_autofree gchar *grab = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *was = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	gint64 series;
	gint64 e2;
	gint64 e3;
	gint64 e4;
	gint64 e5;
	gint64 kept_file;
	gint64 candidate;

	set_route(fx, "/title/series/207", "{\"id\":\"207\",\"kind\":\"series\",\"title\":\"Tidewater Inn\",\"year\":2026,"
		"\"status\":\"continuing\",\"runtime\":45,\"overview\":\"Rooms by the tide.\",\"ids\":{\"tvdb\":\"370007\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-06\",\"episodes\":5}]}");
	set_route(fx, "/title/series/207/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9701\",\"name\":\"Low Water\",\"air_date\":\"2026-11-06\",\"runtime\":45},"
		"{\"number\":2,\"id\":\"9702\",\"name\":\"Spring Tide\",\"air_date\":\"2026-11-13\",\"runtime\":45},"
		"{\"number\":3,\"id\":\"9703\",\"name\":\"Neap\",\"runtime\":45},"
		"{\"number\":4,\"id\":\"9704\",\"name\":\"Slack Water\",\"runtime\":45},"
		"{\"number\":5,\"id\":\"9705\",\"name\":\"High Water\",\"runtime\":45}]}");
	series = add_series(fx, "207", AXIS_TV_MONITOR_ALL);
	e2 = episode_id(fx, series, 1, 2);
	e3 = episode_id(fx, series, 1, 3);
	e4 = episode_id(fx, series, 1, 4);
	e5 = episode_id(fx, series, 1, 5);
	/* S01E02 is in the library as an HDTV file that root keeps by hand. */
	in_library(fx, series, e2, "Tidewater Inn (2026)/Season 01/S01E02.mkv", "hdtv_720p");
	kept_file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e2);
	axis_test_db_exec(fx->t->db, "UPDATE series_file SET kept = true, hand_by = 'root'");
	set_indexed_releases(fx, found, G_N_ELEMENTS(found));
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=episode&item=%" G_GINT64_FORMAT, series, e3);
	season_page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=season&item=1", series);
	force = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/force", series);
	grab = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/grab", series);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, e3, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");

	/* Offered to an administrator on the releases a rule about the name
	 * rules out; a viewer is offered nothing and refused. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(count_of(reply.body, ">Grab it by force</button>"), ==, 4);
	assert_contains(reply.body, "An administrator who can tell may grab it by force.");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, FORCE_NO_EP);
	assert_lacks(reply.body, "/manual/force");
	assert_lacks(reply.body, ">Grab it by force<");
	axis_test_reply_clear(&reply);
	candidate = listed_candidate(fx, FORCE_NO_EP);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT, force,
		candidate, e3);
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&checked=yes&pick_%" G_GINT64_FORMAT "=yes&digest=x", candidate, e3, e3);
	reply = post_as(fx, fx->viewer, page, force, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* Scenario: The name gives no episode.  At an episode's search the
	 * episode searched is proposed; the page says the rule set aside, what it
	 * is bound to and what its import does. */
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis rules this release out:");
	assert_contains(reply.body, "It is grabbed for S01E03 of Tidewater Inn (2026), and for nothing else.");
	assert_contains(reply.body, "its one video file is imported as S01E03 without reading its name");
	assert_contains(reply.body, "I have checked that this release is S01E03 of Tidewater Inn (2026)");
	assert_contains(reply.body, "What you named has no file in the library");
	{
		g_autofree gchar *box = g_strdup_printf("name=\"pick_%" G_GINT64_FORMAT "\"", e3);
		g_autofree gchar *hidden = g_strdup_printf("name=\"pick_%" G_GINT64_FORMAT "\" value=\"yes\"", e3);

		assert_contains(reply.body, box);
		assert_contains(reply.body, "All of season 1 (5 episodes)");
		g_assert_cmpuint(count_of(reply.body, "checked=\"checked\""), ==, 1);
		(void)hidden;
	}
	assert_csp_clean(reply.body);
	save_page("force-episode.html", reply.body);
	digest = digest_of(reply.body);
	g_assert_cmpuint(strlen(digest), ==, 32);
	axis_test_reply_clear(&reply);
	/* Without the tick "I have checked": refused, nothing sent. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&pick_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, e3, e3, digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "you have checked");
	axis_test_reply_clear(&reply);
	/* Scenario: Nothing named.  Refused, nothing sent. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&checked=yes&digest=%s", candidate, e3, digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Say what this release is");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 0);
	/* The picker corrects it: a double episode. */
	{
		g_autofree gchar *both = g_strdup_printf("%s&picked=yes&pick_%" G_GINT64_FORMAT "=yes&pick_%" G_GINT64_FORMAT
			"=yes", confirm, e3, e4);

		reply = get_as(fx, fx->admin, both);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "It is grabbed for S01E03, S01E04 of Tidewater Inn (2026), and for nothing else.");
		g_assert_cmpuint(count_of(reply.body, "checked=\"checked\""), ==, 2);
		assert_contains(reply.body, "One video file of the download holds all of these episodes");
		g_assert_nonnull(strstr(reply.body, "name=\"one_file\""));
		/* Another pick, another digest: a page of one is not a leave for the
		 * other. */
		{
			g_autofree gchar *other = digest_of(reply.body);

			g_assert_cmpstr(other, !=, digest);
		}
		axis_test_reply_clear(&reply);
	}
	/* Confirmed for S01E03: it alone is claimed, forced by root. */
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&checked=yes&pick_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, e3, e3, digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=grabbed");
	axis_test_reply_clear(&reply);
	n = claimed_by(fx, candidate);
	g_assert_cmpstr(n, ==, "S01E03");
	g_free(n);
	n = q(fx, "SELECT manual::text || '|' || set_aside || '|' || bound::text || '|' || coalesce(bound_season::text, '-')"
		" || '|' || grabbed_by || '|' || jsonb_array_length(forced)::text || '|' || (forced->0->>'c') FROM series_grab"
		" WHERE candidate_id = %" G_GINT64_FORMAT, candidate);
	g_assert_cmpstr(n, ==, "true|hand|true|-|root|1|no_match");
	g_free(n);
	/* A bound grab carries the numbering stamp of any grab (item 7). */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab g JOIN series s ON s.id = g.series_id WHERE g.candidate_id"
		" = %" G_GINT64_FORMAT " AND g.bound AND g.source IS NOT NULL AND g.source = s.episode_source", candidate), ==, 1);
	n = q(fx, "SELECT why FROM series_grab WHERE candidate_id = %" G_GINT64_FORMAT, candidate);
	assert_contains(n, "Chosen by root by force from a manual search, for S01E03 of Tidewater Inn (2026) and nothing "
		"else. Set aside:");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'grabbed' AND by_user = 'root' AND text LIKE 'Grabbed by force " FORCE_NO_EP "%%'", series), ==, 1);
	{
		g_autofree gchar *line = g_strdup_printf("audit: series %" G_GINT64_FORMAT " release %" G_GINT64_FORMAT
			" grabbed by force from a manual search, 1 fixed rule about its name set aside", series, candidate);

		g_assert_cmpuint(logged(line), ==, 1);
	}
	/* Posted again: S01E03 is in flight, nothing more is sent. */
	reply = post_as(fx, fx->admin, page, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 1);
	close_grabs(fx);

	/* A file kept by hand: listed with a tick of its own, and not replaced
	 * without it, whether or not the release is better. */
	{
		g_autofree gchar *named = g_strdup_printf("%s&picked=yes&pick_%" G_GINT64_FORMAT "=yes", confirm, e2);
		g_autofree gchar *box = g_strdup_printf("name=\"replace_%" G_GINT64_FORMAT "\"", kept_file);
		g_autofree gchar *stale = g_strdup(digest);

		reply = get_as(fx, fx->admin, named);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "It is grabbed for S01E02 of Tidewater Inn (2026)");
		assert_contains(reply.body, box);
		assert_contains(reply.body, "Replace S01E02's file (HDTV 720p) with this release: this release is better; the "
			"file is kept by hand by root");
		g_free(digest);
		digest = digest_of(reply.body);
		axis_test_reply_clear(&reply);
		/* A stale digest (the page of S01E03, for S01E02): 409, the page
		 * shown again, nothing sent. */
		g_free(form);
		form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
			"&checked=yes&pick_%" G_GINT64_FORMAT "=yes&replace_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, e3, e2,
			kept_file, stale);
		reply = post_as(fx, fx->admin, named, force, form);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "What this page showed changed after you opened it");
		assert_contains(reply.body, "Grab this release by force?");
		axis_test_reply_clear(&reply);
		/* Without the file's tick: refused. */
		g_free(form);
		form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
			"&checked=yes&pick_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, e3, e2, digest);
		reply = post_as(fx, fx->admin, named, force, form);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "which was not ticked");
		axis_test_reply_clear(&reply);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);
		/* With it: sent, and the tick recorded for that very file. */
		g_free(form);
		form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
			"&checked=yes&pick_%" G_GINT64_FORMAT "=yes&replace_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, e3, e2,
			kept_file, digest);
		reply = post_as(fx, fx->admin, named, force, form);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
		g_free(n);
		n = q(fx, "SELECT (replaces_files->0->>'f') || '|' || (replaces_files->0->>'q') FROM series_grab"
			" WHERE closed_at IS NULL");
		{
			g_autofree gchar *expected = g_strdup_printf("%" G_GINT64_FORMAT "|hdtv_720p", kept_file);

			g_assert_cmpstr(n, ==, expected);
		}
		close_grabs(fx);
	}

	/* "Grab this one" of a better release of an episode whose file is kept by
	 * hand: its confirmation asks for the tick that names the file, instead
	 * of the import refusing it later. */
	candidate = listed_candidate(fx, TIDE_E02);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT, grab, candidate,
		e3);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Replace S01E02's file (HDTV 720p), which is kept by hand by root");
	assert_lacks(reply.body, "name=\"downgrade\"");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes",
		candidate, e3);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "kept by hand by root: tick the box that names it");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&replace_%" G_GINT64_FORMAT "=yes", candidate, e3, kept_file);
	reply = post_as(fx, fx->admin, confirm, grab, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT (replaces_files->0->>'f') || '|' || bound::text FROM series_grab WHERE closed_at IS NULL");
	{
		g_autofree gchar *expected = g_strdup_printf("%" G_GINT64_FORMAT "|false", kept_file);

		g_assert_cmpstr(n, ==, expected);
	}
	close_grabs(fx);

	/* A release that also carries a rule that protects the system: no page
	 * offers it, and a post to force it is refused. */
	axis_test_db_exec(fx->t->db, "UPDATE series_candidate SET reasons = reasons || '[{\"k\": \"safety\", \"c\":"
		" \"password\", \"t\": \"It is password-protected.\"}]'::jsonb WHERE name = '" FORCE_LOCKED "'");
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(count_of(reply.body, ">Grab it by force</button>"), ==, 3);
	axis_test_reply_clear(&reply);
	candidate = listed_candidate(fx, FORCE_LOCKED);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT, force,
		candidate, e3);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "cannot be grabbed by force");
	assert_contains(reply.body, "It is password-protected.");
	assert_lacks(reply.body, "name=\"digest\"");
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&checked=yes&pick_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, e3, e4, digest);
	reply = post_as(fx, fx->admin, page, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "password-protected");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);

	/* Scenario: Another series.  The ordinary form keeps refusing a release
	 * a fixed rule rules out. */
	candidate = listed_candidate(fx, FORCE_LODGE);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT "&confirm=yes"
		"&downgrade=yes&numbering=yes", candidate, e3);
	reply = post_as(fx, fx->admin, page, grab, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);

	/* A season's search. */
	set_indexed_releases(fx, found, G_N_ELEMENTS(found));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);

	/* Scenario: Numbered for no episode of the data.  Named S02E01 for a
	 * series with no season 2, forced for S01E05: the confirmation says the
	 * release's own number is set aside. */
	candidate = listed_candidate(fx, FORCE_S02);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1&picked=yes&pick_%" G_GINT64_FORMAT
		"=yes", force, candidate, e5);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "It is grabbed for S01E05 of Tidewater Inn (2026), and for nothing else.");
	assert_contains(reply.body, "The release's own numbering (S02E01) is set aside.");
	g_free(digest);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&checked=yes&pick_%"
		G_GINT64_FORMAT "=yes&digest=%s", candidate, e5, digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = claimed_by(fx, candidate);
	g_assert_cmpstr(n, ==, "S01E05");
	close_grabs(fx);

	/* Scenario: A whole season.  A pack whose name is not the series': its
	 * season is proposed (the data has it), every episode of it is claimed,
	 * and the kept file of S01E02 needs its tick. */
	candidate = listed_candidate(fx, FORCE_LODGE);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", force, candidate);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "It is grabbed for season 1 of Tidewater Inn (2026), and for nothing else.");
	assert_contains(reply.body, "each video file is placed by the episode its own name states in season 1");
	assert_contains(reply.body, "is left in the download for you to assign by hand; nothing is guessed");
	assert_contains(reply.body, "I have checked that this release is season 1 of Tidewater Inn (2026)");
	save_page("force-season.html", reply.body);
	g_free(digest);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&checked=yes&whole=1"
		"&replace_%" G_GINT64_FORMAT "=yes&digest=%s", candidate, kept_file, digest);
	reply = post_as(fx, fx->admin, confirm, force, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = claimed_by(fx, candidate);
	g_assert_cmpstr(n, ==, "S01E01,S01E02,S01E03,S01E04,S01E05");
	g_free(n);
	n = q(fx, "SELECT bound::text || '|' || bound_season::text || '|' || holds FROM series_grab WHERE closed_at IS NULL");
	g_assert_cmpstr(n, ==, "true|1|season 1");
	close_grabs(fx);

	/* Scenario: The numbering changed meanwhile.  The page was shown under
	 * one numbering; the series follows another when it is confirmed:
	 * nothing is sent, and the page is shown again. */
	candidate = listed_candidate(fx, FORCE_S02);
	g_free(confirm);
	confirm = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1&picked=yes&pick_%" G_GINT64_FORMAT
		"=yes", force, candidate, e4);
	reply = get_as(fx, fx->admin, confirm);
	g_free(digest);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	was = q(fx, "SELECT episode_source FROM series WHERE id = %" G_GINT64_FORMAT, series);
	axis_test_db_exec(fx->t->db, "UPDATE series SET episode_source = 'tvdb-dvd'");
	g_free(form);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes&checked=yes&pick_%"
		G_GINT64_FORMAT "=yes&digest=%s", candidate, e4, digest);
	reply = post_as(fx, fx->admin, season_page, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "What this page showed changed after you opened it");
	assert_contains(reply.body, "Search again, then grab it by force from the new list.");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);
	{
		g_autofree gchar *back = g_strdup_printf("UPDATE series SET episode_source = '%s'", was);

		axis_test_db_exec(fx->t->db, back);
	}

	/* Not while an import of the series is open (the predicate placing and
	 * the numbering switch use). */
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, title_id, title_label, source_path, state, kind)"
		" SELECT 'tv', title_id, 'Tidewater Inn (2026)', 'downloads/x', 'blocked', 'manual' FROM series");
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "cannot be grabbed by force");
	assert_contains(reply.body, "Nothing is grabbed by force for the series until then.");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, season_page, force, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 0);
}

static void
test_module_off(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gint64 title = title_of(fx, "205");
	g_autofree gchar *page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
	AxisTestReply reply;

	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "film-panel");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_module_registry_set_enabled(fx->registry, AXIS_TV_MODULE_ID, FALSE, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->admin, "/series");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series/calendar");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "film-panel");
	assert_lacks(reply.body, "href=\"/series\"");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/");
	assert_lacks(reply.body, "href=\"/series\"");
	assert_lacks(reply.body, "Episodes airing today");
	axis_test_reply_clear(&reply);
}

/*
 * Spec (tv): The empty TV list says why.  With a root for TV and nothing
 * listed, it names the root and says what lists its series (Identify, here
 * switched off), not that there is no TV; with a series, nothing of that;
 * with no root for TV, the plain empty list.
 */
static void
test_empty_list(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisTestReply reply;
	gint64 series;

	reply = get_as(fx, fx->viewer, "/series");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "No series are listed yet. Identify is switched off, so the series in the TV library "
		"folder series are not recognised: an administrator can switch it on under Modules.");
	assert_lacks(reply.body, "No series yet.");
	assert_lacks(reply.body, "as if none of its episodes were on disk");
	axis_test_reply_clear(&reply);

	series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	reply = get_as(fx, fx->viewer, "/series");
	assert_lacks(reply.body, "No series are listed yet");
	assert_lacks(reply.body, "not recognise");
	axis_test_reply_clear(&reply);

	/* No root for TV: nothing about disks. */
	g_assert_true(axis_tv_remove(fx->tv, series, "root", &error));
	g_assert_no_error(error);
	g_assert_true(axis_storage_remove_root(fx->t->storage, fx->root_id, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->viewer, "/series");
	assert_contains(reply.body, "No series yet. Find one with the search box above and add it from its page.");
	assert_lacks(reply.body, "TV library folder");
	axis_test_reply_clear(&reply);
}

/* The Library's folders of a series root (axis_tv_folder_titles()): a
 * folder at the top that holds a file linked to a series is that series;
 * one that holds none is not identified, with nowhere to identify it; a
 * season's folder, or the root's top, is not asked about. */
static void
test_folder_titles(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisScanResult) scan = NULL;
	g_autoptr(AxisFolderView) top = NULL;
	g_autoptr(AxisFolderView) inside = NULL;
	g_autoptr(GPtrArray) dirs = g_ptr_array_new();
	g_autoptr(GPtrArray) titles = g_ptr_array_new_with_free_func((GDestroyNotify)axis_web_folder_title_free);
	g_autofree gchar *series_dir = g_build_filename(fx->disk, "series", NULL);
	const AxisWebFolderTitle *harbour_item = NULL;
	const AxisWebFolderTitle *unknown_item = NULL;
	gint64 harbour;
	gint64 file;
	gint64 top_id = 0;
	guint i;

	{
		g_autofree gchar *one = g_build_filename(series_dir, "Harbour Lights (2024)", "Season 01", "S01E01.mkv", NULL);
		g_autofree gchar *two = g_build_filename(series_dir, "Unknown Show", "Season 1", "e1.mkv", NULL);
		g_autofree gchar *one_dir = g_path_get_dirname(one);
		g_autofree gchar *two_dir = g_path_get_dirname(two);

		axis_test_storage_mkdir(one_dir);
		axis_test_storage_mkdir(two_dir);
		axis_test_storage_write(one, "episode");
		axis_test_storage_write(two, "episode");
	}
	scan = axis_test_library_scan_expect(fx->t, fx->root_id, AXIS_SCAN_COMPLETE, AXIS_SCAN_REASON_NONE);
	harbour = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	file = qi(fx, "SELECT id FROM library_file WHERE rel_path = 'Harbour Lights (2024)/Season 01/S01E01.mkv'");
	g_assert_cmpint(qi(fx, "INSERT INTO series_file (file_id, series_id) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ") RETURNING series_id", file, harbour), ==, harbour);

	g_assert_true(axis_library_find_root_dir(fx->t->library, fx->root_id, &top_id, &error));
	g_assert_true(axis_library_get_folder(fx->t->library, top_id, 0, 0, &top, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(top->subdirs->len, ==, 2);
	g_assert_true(axis_library_get_folder(fx->t->library, ((AxisDirRecord *)g_ptr_array_index(top->subdirs, 0))->id,
		0, 0, &inside, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(inside->subdirs->len, ==, 1);
	for (i = 0; i < top->subdirs->len; i++)
		g_ptr_array_add(dirs, g_ptr_array_index(top->subdirs, i));
	g_ptr_array_add(dirs, g_ptr_array_index(inside->subdirs, 0));
	g_ptr_array_add(dirs, top->dir);

	g_assert_true(axis_tv_folder_titles(fx->tv, fx->root_id, dirs, titles, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(titles->len, ==, 2);
	for (i = 0; i < titles->len; i++) {
		const AxisWebFolderTitle *item = g_ptr_array_index(titles, i);

		if (item->dir_id == ((AxisDirRecord *)g_ptr_array_index(top->subdirs, 0))->id)
			harbour_item = item;
		else if (item->dir_id == ((AxisDirRecord *)g_ptr_array_index(top->subdirs, 1))->id)
			unknown_item = item;
	}
	g_assert_nonnull(harbour_item);
	g_assert_nonnull(harbour_item->title);
	g_assert_cmpstr(harbour_item->title->title, ==, "Harbour Lights");
	g_assert_cmpint(harbour_item->title->year, ==, 2024);
	g_assert_cmpint(harbour_item->title->kind, ==, AXIS_META_KIND_SERIES);
	g_assert_cmpint(harbour_item->title->id, ==, qi(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT,
		harbour));
	g_assert_nonnull(unknown_item);
	g_assert_null(unknown_item->title);
	g_assert_cmpint(unknown_item->unit_id, ==, 0);
	g_assert_false(unknown_item->not_title);
}

/* ── Repeated clicks (add-activity-feedback) ────────────────────── */

/* @form posted @n times from @page to @action; the Location of each answer
 * into @locations (nullable). */
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

		if (locations != NULL)
			g_ptr_array_add(locations, g_strdup_printf("%u %s", reply.status, where != NULL ? where : "-"));
		axis_test_reply_clear(&reply);
	}
}

/* The jobs of @series' searches whose keys end in @tail ("" for all). */
static gint64
search_jobs(
	Fixture     *fx,
	gint64       series,
	const gchar *tail
){
	return qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND payload->>'series' = '%" G_GINT64_FORMAT "'"
		" AND dedupe_key LIKE '%%%s'", series, tail);
}

/* Every answer of @locations went back to @place, the first with @first
 * and the others with @then. */
static void
assert_answers(
	GPtrArray   *locations,
	const gchar *first,
	const gchar *then,
	const gchar *place
){
	g_autofree gchar *at = g_strdup_printf("&at=%s#%s", place, place);
	guint i;

	g_assert_cmpuint(locations->len, >, 0);
	for (i = 0; i < locations->len; i++) {
		const gchar *location = g_ptr_array_index(locations, i);
		g_autofree gchar *done = g_strdup_printf("done=%s", i == 0 ? first : then);

		assert_contains(location, "303 ");
		assert_contains(location, done);
		g_assert_true(g_str_has_suffix(location, at));
	}
}

/* The page a Location leads to, as the browser asks for it (no fragment). */
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

/* The notice is drawn inside the element @id, not at the top, and says
 * @text. */
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
 * Spec (tv): A click is answered by the work in hand.  Each search button
 * clicked five times quickly queues one search at most; a wider search in
 * hand covers a narrower one; Search all wanted leaves out what is in hand
 * and says so; a search of the same thing that ended a moment ago answers
 * the click; the button says so and cannot be pressed; every answer comes
 * back to the row or card with the notice there.
 */
static void
test_repeated_clicks(Fixture *fx, gconstpointer data)
{
	g_autoptr(GPtrArray) locations = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	gint64 series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	gint64 e1 = episode_id(fx, series, 1, 1);
	gint64 e2 = episode_id(fx, series, 1, 2);
	g_autofree gchar *page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/search", series);
	g_autofree gchar *manual = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual", series);
	g_autofree gchar *sync = g_strdup_printf("/series/%" G_GINT64_FORMAT "/sync", series);
	g_autofree gchar *episode = g_strdup_printf("scope=episode&item=%" G_GINT64_FORMAT "&season=1", e1);
	g_autofree gchar *episode2 = g_strdup_printf("scope=episode&item=%" G_GINT64_FORMAT "&season=1", e2);
	g_autofree gchar *row = g_strdup_printf("episode-%" G_GINT64_FORMAT, e1);
	g_autofree gchar *row2 = g_strdup_printf("episode-%" G_GINT64_FORMAT, e2);
	g_autofree gchar *act = g_strdup_printf("id=\"act-episode-%" G_GINT64_FORMAT "\"", e1);
	g_autofree gchar *n = NULL;
	AxisTestReply reply;
	guint before;

	/* An episode's Search, five times: one search; the others answered with
	 * it, at its row, and its button says so. */
	click(fx, page, action, episode, 5, locations);
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 1);
	assert_answers(locations, "searching", "already_searching", row);
	reply = follow(fx, g_ptr_array_index(locations, 4));
	assert_notice_at(reply.body, row, "Nothing new was queued: a search of S01E01 is waiting for its turn, queued");
	assert_contains(strstr(reply.body, act), "disabled=\"disabled\"");
	assert_contains(strstr(reply.body, act), ">Waiting for its turn</button>");
	axis_test_reply_clear(&reply);
	g_ptr_array_set_size(locations, 0);
	/* Its season's search: one.  An episode of the season asked meanwhile is
	 * searched for all the same: the season's search takes whole packs only,
	 * and holds nothing back. */
	click(fx, page, action, "scope=season&item=1", 5, locations);
	assert_answers(locations, "searching", "already_searching", "season-1");
	g_assert_cmpint(search_jobs(fx, series, ":season:1"), ==, 1);
	g_ptr_array_set_size(locations, 0);
	click(fx, page, action, episode2, 5, locations);
	assert_answers(locations, "searching", "already_searching", row2);
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 3);
	reply = follow(fx, g_ptr_array_index(locations, 1));
	assert_notice_at(reply.body, row2, "Nothing new was queued: a search of S01E02 is waiting for its turn");
	axis_test_reply_clear(&reply);
	g_ptr_array_set_size(locations, 0);

	/* One the scheduler queued, behind its backlog: the click raises it to a
	 * person's priority instead of waiting behind it. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	{
		gboolean created = FALSE;

		g_assert_true(axis_tv_request_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, e1, "scheduled", NULL, &created,
			&error));
		g_assert_no_error(error);
		g_assert_true(created);
	}
	g_assert_cmpint(qi(fx, "SELECT priority FROM job WHERE kind = 'tv.search'"), ==, 0);
	click(fx, page, action, episode, 1, locations);
	assert_contains(g_ptr_array_index(locations, 0), "done=already_searching");
	g_assert_cmpint(qi(fx, "SELECT priority FROM job WHERE kind = 'tv.search'"), ==, 10);
	g_ptr_array_set_size(locations, 0);

	/* Search all wanted, five times: what the scheduler would queue (the
	 * whole series, nothing on disk); then nothing new, said as such, once in
	 * the history.  A season's and an episode's search asked meanwhile are
	 * made: the series' takes packs only. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	click(fx, page, action, "scope=series", 5, locations);
	assert_answers(locations, "searching", "already_searching", "settings");
	g_assert_cmpint(search_jobs(fx, series, ":series"), ==, 1);
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 1);
	reply = follow(fx, g_ptr_array_index(locations, 1));
	assert_notice_at(reply.body, "settings", "Nothing new was queued: a search of the whole series is waiting for its "
		"turn");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND text LIKE"
		" 'A search of every wanted episode%%'", series), ==, 1);
	g_ptr_array_set_size(locations, 0);
	click(fx, page, action, "scope=season&item=1", 1, locations);
	click(fx, page, action, episode, 1, locations);
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 3);
	assert_contains(g_ptr_array_index(locations, 0), "done=searching&");
	assert_contains(g_ptr_array_index(locations, 1), "done=searching&");
	g_ptr_array_set_size(locations, 0);

	/* A season searched a moment ago: the click is answered with what it
	 * found and when it can be asked again; a minute later it is searched. */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	search(fx, series, AXIS_TV_SCOPE_SEASON, 1);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	click(fx, page, action, "scope=season&item=1", 5, locations);
	assert_answers(locations, "searched_recently", "searched_recently", "season-1");
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 0);
	reply = follow(fx, g_ptr_array_index(locations, 0));
	assert_notice_at(reply.body, "season-1", "Nothing new was queued: a search of season 1 ended a moment ago and "
		"found ");
	assert_contains(reply.body, ". You can ask again from ");
	assert_contains(strstr(reply.body, "id=\"act-season-1\""), ">Searched a moment ago</button>");
	axis_test_reply_clear(&reply);
	g_ptr_array_set_size(locations, 0);
	/* Asked from the season's releases (its box of what puts it right): back
	 * there, at the box, with the answer in it. */
	{
		g_autofree gchar *releases = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?season=1", series);
		g_autofree gchar *form = g_strdup_printf("scope=season&item=1&season=1&back=%%2Fseries%%2F%" G_GINT64_FORMAT
			"%%2Freleases%%3Fseason%%3D1&at=resolve", series);

		reply = get_as(fx, fx->admin, releases);
		assert_contains(reply.body, "id=\"resolve\"");
		axis_test_reply_clear(&reply);
		click(fx, releases, action, form, 1, locations);
		assert_answers(locations, "searched_recently", "searched_recently", "resolve");
		assert_contains(g_ptr_array_index(locations, 0), releases);
		reply = follow(fx, g_ptr_array_index(locations, 0));
		assert_notice_at(reply.body, "resolve", "Nothing new was queued: that was searched for a moment ago.");
		axis_test_reply_clear(&reply);
		g_ptr_array_set_size(locations, 0);
	}
	axis_test_db_exec(fx->t->db, "UPDATE series_search SET finished_at = finished_at - interval '2 minutes'");
	click(fx, page, action, "scope=season&item=1", 1, locations);
	assert_contains(g_ptr_array_index(locations, 0), "done=searching");
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 1);
	g_ptr_array_set_size(locations, 0);

	/* The season's episodes with no air date, five times: one search. */
	/* (Its season's search queued meanwhile holds none of them back: it never
	 * takes an episode without an air date.  Their button is never drawn
	 * busy: the server decides episode by episode.) */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	click(fx, page, action, "scope=season&item=2", 1, NULL);
	reply = get_as(fx, fx->admin, page);
	{
		const gchar *undated = strstr(reply.body, "id=\"act-undated-2\"");
		const gchar *end = undated != NULL ? strstr(undated, "</span>") : NULL;

		g_assert_nonnull(end);
		g_assert_null(g_strstr_len(undated, end - undated, "disabled"));
	}
	axis_test_reply_clear(&reply);
	click(fx, page, action, "scope=undated&item=2", 5, locations);
	assert_answers(locations, "searching_undated", "already_searching", "season-2");
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 2);
	g_ptr_array_set_size(locations, 0);

	/* Manual searches, five times each: one; the series' holds no season's
	 * back (each lists what that level finds). */
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	click(fx, page, manual, "level=series", 5, locations);
	assert_answers(locations, "searching", "already_searching", "manual-results");
	g_assert_cmpint(search_jobs(fx, series, ":list:series"), ==, 1);
	g_ptr_array_set_size(locations, 0);
	click(fx, page, manual, "level=season&item=2", 5, locations);
	assert_answers(locations, "searching", "already_searching", "manual-results");
	g_assert_cmpint(search_jobs(fx, series, ""), ==, 2);
	reply = follow(fx, g_ptr_array_index(locations, 1));
	assert_notice_at(reply.body, "manual-results", "Nothing new was asked for: a manual search of season 2 is waiting "
		"for its turn");
	assert_contains(reply.body, ">Manual search running</button>");
	axis_test_reply_clear(&reply);
	g_ptr_array_set_size(locations, 0);
	/* The series' page: its manual buttons lead to the search in hand. */
	reply = get_as(fx, fx->admin, page);
	assert_contains(strstr(reply.body, "id=\"act-manual-series\""), "Manual search in hand: see the manual search");
	axis_test_reply_clear(&reply);

	/* Update details now, five times: done each time where it was asked,
	 * and nothing asked of the provider for seasons it has. */
	before = axis_test_plex_n_requests(fx->fa->api);
	click(fx, page, sync, NULL, 5, locations);
	assert_answers(locations, "synced", "synced", "settings");
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api) - before, ==, 0);
	n = q(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'episodes'", series);
	g_assert_cmpstr(n, ==, "0");
	g_assert_no_error(error);
}

/*
 * Spec (tv): What is happening is shown where it concerns.  The series, its
 * seasons and its episodes each have a line: the search in hand, the
 * download at its client, else what the last search found; the page
 * follows its topics, and its fragment answers the lines out of band; a
 * viewer sees the lines and no button.
 */
static void
test_activity_lines(Fixture *fx, gconstpointer data)
{
	Scene s = scene(fx);
	gint64 e1 = episode_id(fx, s.harbour, 1, 3);
	gint64 storm = episode_id(fx, s.harbour, 2, 2);
	g_autofree gchar *page = g_strdup_printf("/series/%" G_GINT64_FORMAT "?season=1", s.harbour);
	g_autofree gchar *lines = g_strdup_printf("/series/%" G_GINT64_FORMAT "/activity", s.harbour);
	g_autofree gchar *line = g_strdup_printf("id=\"activity-episode-%" G_GINT64_FORMAT "\"", e1);
	g_autofree gchar *storm_line = g_strdup_printf("id=\"activity-episode-%" G_GINT64_FORMAT "\"", storm);
	g_autofree gchar *topic = g_strdup_printf("topic=act:tv.search:%" G_GINT64_FORMAT, s.harbour);
	g_autofree gchar *title_topic = g_strdup_printf("topic=act:title:%" G_GINT64_FORMAT, qi(fx, "SELECT title_id FROM"
		" series WHERE id = %" G_GINT64_FORMAT, s.harbour));
	AxisTestReply reply;

	/* The key families' topics. */
	{
		g_autofree gchar *a = axis_web_activity_topic("tv.search:12:season:1");
		g_autofree gchar *b = axis_web_activity_topic("movies.search:7:list");
		g_autofree gchar *c = axis_web_activity_topic("library.scan:volume:3");
		g_autofree gchar *d = axis_web_activity_topic("schedule:tv.schedule");

		g_assert_cmpstr(a, ==, "act:tv.search:12");
		g_assert_cmpstr(b, ==, "act:movies.search:7");
		g_assert_cmpstr(c, ==, "act:library.scan:volume:3");
		g_assert_null(d);
	}
	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 200);
	/* S01E03 downloads at 40 %: its line says where and how far. */
	assert_contains(strstr(reply.body, line), "Downloading at SABnzbd:");
	/* S02E02's search is queued (the scene): its line says so; S02E01's last
	 * search found releases a moment ago. */
	assert_contains(strstr(reply.body, storm_line), "A search of S02E02 is waiting for its turn, queued");
	assert_contains(reply.body, "id=\"activity-series\"");
	assert_contains(reply.body, "1 search of this series is in hand: 0 running, 1 waiting for its turn. 2 episodes are "
		"downloading.");
	assert_contains(reply.body, "id=\"activity-season-2\"");
	/* It follows its topics; its probe asks for its lines. */
	assert_contains(reply.body, topic);
	assert_contains(reply.body, title_topic);
	{
		g_autofree gchar *probe = g_strdup_printf("hx-get=\"%s\"", lines);

		assert_contains(reply.body, probe);
	}
	/* The last event of a burst wins (a delay, not a throttle, which drops
	 * it); with a search in hand, by time as well; and once when the search
	 * that ended a moment ago may be asked again. */
	assert_contains(reply.body, "hx-trigger=\"sse:activity delay:400ms, htmx:sseOpen from:body, every 30s, load delay:");
	/* A viewer gets no button. */
	assert_lacks(reply.body, "id=\"act-episode-");
	assert_csp_clean(reply.body);
	save_page("series-activity.html", reply.body);
	axis_test_reply_clear(&reply);
	/* The fragment: every line out of band; an administrator's buttons too. */
	reply = get_as(fx, fx->viewer, lines);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "hx-swap-oob=\"true\"");
	assert_contains(reply.body, line);
	assert_lacks(reply.body, "<html");
	assert_lacks(reply.body, "act-episode-");
	/* The probe again, with the trigger of the moment. */
	assert_contains(strstr(reply.body, "id=\"activity-probe\""), "hx-trigger=\"sse:activity delay:400ms");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, lines);
	{
		g_autofree gchar *act = g_strdup_printf("id=\"act-episode-%" G_GINT64_FORMAT "\"", storm);

		assert_contains(strstr(reply.body, act), ">Waiting for its turn</button>");
	}
	axis_test_reply_clear(&reply);

	/* The Activity page names the work in a person's words. */
	/* (A search that failed with what its indexer said; Identify's look at a
	 * folder, whose page is an administrator's.) */
	axis_test_db_exec(fx->t->db, "INSERT INTO job (kind, queue, state, max_attempts, attempts, last_error, finished_at)"
		" VALUES ('tv.search', 'searches', 'failed', 2, 2, 'the indexer answered 503 at tracker.example.com', now())");
	{
		gint64 root = qi(fx, "SELECT min(id) FROM library_root");
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO job (kind, queue, state, max_attempts, dedupe_key, payload)"
			" VALUES ('identify.root', 'identify', 'queued', 1, 'identify.root:%" G_GINT64_FORMAT "', '{\"root\": %"
			G_GINT64_FORMAT "}')", root, root);

		axis_test_db_exec(fx->t->db, sql);
	}
	reply = get_as(fx, fx->viewer, "/activity");
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "<h1>Activity</h1>");
	assert_contains(reply.body, "Waiting for their turn");
	assert_contains(reply.body, "Searching for Harbour Lights (2024), S02E02");
	assert_contains(reply.body, "Downloading");
	assert_contains(reply.body, "topic=act:all");
	/* Who asked, the job, what a job said, the download client and a page a
	 * viewer cannot open are an administrator's. */
	assert_lacks(reply.body, "asked by root");
	assert_lacks(reply.body, "href=\"/jobs/");
	assert_lacks(reply.body, "tracker.example.com");
	assert_lacks(reply.body, "SABnzbd");
	assert_contains(reply.body, "Identifying the movies and series in the library folder");
	assert_lacks(reply.body, "href=\"/identify/");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/activity");
	assert_contains(reply.body, "asked by root");
	assert_contains(reply.body, "href=\"/jobs/");
	assert_contains(reply.body, "tracker.example.com");
	assert_contains(reply.body, "at SABnzbd");
	assert_contains(reply.body, "href=\"/identify/roots/");
	save_page("activity.html", reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/activity?part=list");
	g_assert_true(g_str_has_prefix(reply.body, "<div id=\"activity-list\""));
	axis_test_reply_clear(&reply);
}

/*
 * add-metadata-tvdb, V2: a series follows one source of numbering, its record
 * provider's own by default; its rows are known by that source and remember
 * each identity; another source's seasons stored for its title change none
 * of its episodes; its settings card names the numbering and shows a claim
 * about its identifiers that another series' record makes (Spec: Identifiers
 * that disagree).
 */
static void
test_episode_sources(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *source = NULL;
	g_autoptr(GError) error = NULL;
	AxisTestReply page;
	gint64 series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	gint64 title = qi(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT, series);
	gint64 other;
	guint changes = 0;

	source = q(fx, "SELECT episode_source FROM series WHERE id = %" G_GINT64_FORMAT, series);
	g_assert_cmpstr(source, ==, "fixture");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND source <> 'fixture'", series), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode_identity i JOIN series_episode e ON e.id = i.episode_id"
		" WHERE e.series_id = %" G_GINT64_FORMAT " AND i.source = 'fixture' AND i.season_number = e.season_number"
		" AND i.number = e.number", series), ==, qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %"
		G_GINT64_FORMAT, series));
	before = q(fx, "SELECT string_agg(id || ':' || season_number || 'x' || number || ':' || coalesce(title, '') || ':'"
		" || coalesce(air_date::text, '') || ':' || (gone_at IS NULL)::text, ',' ORDER BY id) FROM series_episode"
		" WHERE series_id = %" G_GINT64_FORMAT, series);

	/* Another order of the same series, stored beside its own: synced again,
	 * the series is as it was. */
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_season (title_id, source, number, episodes_fetched_at)"
		" SELECT title_id, 'fixture-dvd', n, now() FROM series, generate_series(1, 3) AS n");
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_episode (title_id, source, season_number, number, name, air_date)"
		" SELECT s.title_id, s.source, s.number, e, 'Disc ' || s.number || '-' || e, DATE '2001-01-01' + e"
		" FROM meta_season s, generate_series(1, 9) AS e WHERE s.source = 'fixture-dvd'");
	g_assert_true(axis_tv_sync(fx->tv, series, FALSE, &changes, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(changes, ==, 0);
	after = q(fx, "SELECT string_agg(id || ':' || season_number || 'x' || number || ':' || coalesce(title, '') || ':'"
		" || coalesce(air_date::text, '') || ':' || (gone_at IS NULL)::text, ',' ORDER BY id) FROM series_episode"
		" WHERE series_id = %" G_GINT64_FORMAT, series);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_episode_file v JOIN meta_episode m ON m.title_id = v.title_id"
		" AND m.source = 'fixture-dvd'"), ==, 0);

	/* Another series' record claims this one's identifier. */
	other = qi(fx, "INSERT INTO meta_title (kind, title, provider, provider_id, year) VALUES ('series',"
		" 'Harbour Bell', 'tvdb', '81009', 2021) RETURNING id");
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series',"
		" 'tvdb', '81009' FROM meta_title WHERE provider_id = '81009'");
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO meta_id_claim (title_id, provider, scheme, value, held_by)"
			" VALUES (%" G_GINT64_FORMAT ", 'tvdb', 'fixture', '202', %" G_GINT64_FORMAT ")", other, title);

		axis_test_db_exec(fx->t->db, sql);
	}
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	page = get_as(fx, fx->viewer, path);
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Episode numbering");
	assert_contains(page.body, "TheTVDB's record of another series, Harbour Bell (2021), names this series' "
		"fixture identifier 202. The two are kept apart");
	axis_test_reply_clear(&page);
}

/* Review of V1/V2, blocker 1: until a series can follow TheTVDB (V3), none
 * is added following it: a title of TheTVDB's alone is refused with a plain
 * reason, and the database refuses a series that would follow it by
 * default. */
static void
test_no_series_follows_tvdb(Fixture *fx, gconstpointer data)
{
	AxisTvSpec spec;
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) refused = NULL;
	g_autofree gchar *sql = NULL;
	gint64 lone;
	gint64 id = 0;

	lone = qi(fx, "INSERT INTO meta_title (kind, title, provider, provider_id, year) VALUES ('series',"
		" 'Wobbly Lighthouse', 'tvdb', '81001', 2023) RETURNING id");
	memset(&spec, 0, sizeof(spec));
	spec.title_id = lone;
	spec.root_id = fx->root_id;
	spec.monitor = AXIS_TV_MONITOR_ALL;
	spec.user = "root";
	g_assert_false(axis_tv_add(fx->tv, &spec, &id, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "TheTVDB"));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series"), ==, 0);
	sql = g_strdup_printf("INSERT INTO series (title_id, quality_profile_id) VALUES (%" G_GINT64_FORMAT ","
		" (SELECT min(id) FROM quality_profile))", lone);
	g_assert_false(axis_db_pool_exec(fx->t->pool, sql, 0, NULL, NULL, &refused));
	g_assert_nonnull(refused);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series"), ==, 0);

	/* A series of TMDb's (here the fixture's) record follows it, by name. */
	id = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND episode_source ="
		" 'fixture'", id), ==, 1);
}

/* Review of V1/V2, blocker 2: two records of one provider merged hold one
 * id of that provider, and refreshing twice asks it alone: no episode with a
 * file goes, none is doubled. */
static void
test_merge_then_refresh(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	AxisMetaRefreshReport report;
	AxisMetaTitleAnswer answer;
	gint64 series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	gint64 title = qi(fx, "SELECT title_id FROM series WHERE id = %" G_GINT64_FORMAT, series);
	gint64 arrival = qi(fx, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND season_number = 1 AND number = 1", series);
	guint changes = 0;
	guint round;

	in_library(fx, series, arrival, "Harbour Lights (2024)/Season 01/S01E01.mkv", "bluray_1080p");
	/* A second record of the provider's, stored apart, for the same series;
	 * its seasons are numbered with other episode ids. */
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_title (kind, title, provider, provider_id) VALUES ('series',"
		" 'Harbour Lights', 'fixture', '299')");
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series', s,"
		" v FROM meta_title, (VALUES ('fixture', '299'), ('tvdb', '81008')) AS u (s, v) WHERE provider_id = '299'");
	set_route(fx, "/title/series/299/season/1", "{\"number\": 1, \"name\": \"Season 1\", \"episodes\": ["
		"{\"number\": 1, \"id\": \"9901\", \"name\": \"Arrival\", \"air_date\": \"2024-03-01\"},"
		"{\"number\": 2, \"id\": \"9902\", \"name\": \"Low Tide\", \"air_date\": \"2024-03-08\"}]}");
	/* The record of 202 now names the other record's TVDB id: one title. */
	set_route(fx, "/title/series/202", "{\"id\": \"202\", \"kind\": \"series\", \"title\": \"Harbour Lights\","
		" \"year\": 2024, \"status\": \"continuing\", \"first_air_date\": \"2024-03-01\","
		" \"ids\": {\"imdb\": \"tt7654321\", \"tvdb\": \"81008\"},"
		" \"seasons\": [{\"number\": 1, \"name\": \"Season 1\", \"episodes\": 4}]}");
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET detail_fetched_at = now() - interval '400 days'");
	g_assert_true(axis_meta_get_title(fx->meta, title, 5000, &answer, &error));
	g_assert_no_error(error);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM meta_title WHERE provider = 'fixture' AND kind = 'series'"
		" AND provider_id IN ('202', '299')"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM meta_external_id WHERE title_id = %" G_GINT64_FORMAT
		" AND scheme = 'fixture'", title), ==, 1);

	for (round = 0; round < 2; round++) {
		axis_test_db_exec(fx->t->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'");
		axis_test_db_exec(fx->t->db, "UPDATE meta_title SET viewed_at = now()");
		g_assert_true(axis_meta_run_refresh(fx->meta, NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_true(axis_tv_sync(fx->tv, series, FALSE, &changes, &error));
		g_assert_no_error(error);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode e JOIN series_file_episode f ON f.episode_id = e.id"
			" WHERE e.series_id = %" G_GINT64_FORMAT " AND e.gone_at IS NOT NULL", series), ==, 0);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM (SELECT season_number, number FROM series_episode WHERE"
			" series_id = %" G_GINT64_FORMAT " AND gone_at IS NULL GROUP BY 1, 2 HAVING count(*) > 1) d", series), ==, 0);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
			arrival), ==, 1);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM meta_external_id WHERE title_id = %" G_GINT64_FORMAT
		" AND scheme = 'fixture'", title), ==, 1);
}

/* Two records of the provider merged, the answering one not the oldest: the
 * title takes the answer's id, and its episodes are numbered by that id
 * from then on.  The series' rows are keyed anew by season and number,
 * keeping their ids, files, monitoring and history; none goes, none is
 * added, nothing with a file is wanted again. */
static void
test_record_id_changes(Fixture *fx, gconstpointer data)
{
	static const gchar *const seasons[] = {
		"{\"number\": 0, \"name\": \"Specials\", \"episodes\": [{\"number\": 1, \"id\": \"9900\", \"name\": \"Beacon\"}]}",
		"{\"number\": 1, \"name\": \"Season 1\", \"episodes\": [{\"number\": 1, \"id\": \"9911\", \"name\": \"Arrival\","
		" \"air_date\": \"2024-03-01\"}, {\"number\": 2, \"id\": \"9912\", \"name\": \"Low Tide\", \"air_date\":"
		" \"2024-03-08\"}, {\"number\": 3, \"id\": \"9913\", \"name\": \"High Water (1)\", \"air_date\": \"2024-03-15\"},"
		" {\"number\": 4, \"id\": \"9914\", \"name\": \"High Water (2)\", \"air_date\": \"2024-03-15\"}]}",
		"{\"number\": 2, \"name\": \"Season 2\", \"episodes\": [{\"number\": 1, \"id\": \"9921\", \"name\": \"Return\","
		" \"air_date\": \"2026-11-06\"}, {\"number\": 2, \"id\": \"9922\"}, {\"number\": 3, \"id\": \"9923\"},"
		" {\"number\": 4, \"id\": \"9924\"}]}"
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = NULL;
	AxisMetaTitleAnswer answer;
	AxisMetaRefreshReport report;
	gint64 series;
	gint64 arrival;
	gint64 events;
	guint changes = 0;
	guint round;
	guint i;

	/* The other record is the older. */
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_title (kind, title, provider, provider_id) VALUES ('series',"
		" 'Harbour Lights', 'fixture', '299')");
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series', s,"
		" v FROM meta_title, (VALUES ('fixture', '299'), ('tvdb', '81008')) AS u (s, v) WHERE provider_id = '299'");
	series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	arrival = qi(fx, "SELECT id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT " AND season_number = 1"
		" AND number = 1", series);
	in_library(fx, series, arrival, "Harbour Lights (2024)/Season 01/S01E01.mkv", "bluray_1080p");
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET monitored = false WHERE season_number = 2 AND number = 4");
	events = qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series);
	before = q(fx, "SELECT string_agg(id || ':' || season_number || 'x' || number || ':' || monitored::text, ','"
		" ORDER BY id) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);

	/* The other record's answer names this one's TVDB id too: one title, the
	 * older, with the answer's id. */
	set_route(fx, "/title/series/299", "{\"id\": \"299\", \"kind\": \"series\", \"title\": \"Harbour Lights\","
		" \"year\": 2024, \"status\": \"continuing\", \"first_air_date\": \"2024-03-01\","
		" \"ids\": {\"imdb\": \"tt7654321\", \"tvdb\": \"81008\"}}");
	for (i = 0; i < G_N_ELEMENTS(seasons); i++) {
		g_autofree gchar *path = g_strdup_printf("/title/series/299/season/%u", i);

		set_route(fx, path, seasons[i]);
	}
	g_assert_true(axis_meta_lookup(fx->meta, "fixture", AXIS_META_KIND_SERIES, "299", 5000, &answer, &error));
	g_assert_no_error(error);
	axis_meta_title_answer_clear(&answer);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM meta_title WHERE kind = 'series' AND provider_id IN ('202', '299')"),
		==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series s JOIN meta_title t ON t.id = s.title_id WHERE"
		" t.provider_id = '299'"), ==, 1);

	for (round = 0; round < 2; round++) {
		g_autofree gchar *after = NULL;

		axis_test_db_exec(fx->t->db, "UPDATE meta_season SET episodes_fetched_at = now() - interval '400 days'");
		axis_test_db_exec(fx->t->db, "UPDATE meta_title SET viewed_at = now()");
		g_assert_true(axis_meta_run_refresh(fx->meta, NULL, &report, &error));
		g_assert_no_error(error);
		g_assert_true(axis_tv_sync(fx->tv, series, FALSE, &changes, &error));
		g_assert_no_error(error);
		after = q(fx, "SELECT string_agg(id || ':' || season_number || 'x' || number || ':' || monitored::text, ','"
			" ORDER BY id) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);
		g_assert_cmpstr(after, ==, before);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
			" AND gone_at IS NOT NULL", series), ==, 0);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
			arrival), ==, 1);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT
			" AND provider_id = '9911'", arrival), ==, 1);
	}
	/* The history is the series' own, nothing taken from it. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series), >=, events);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_search WHERE series_id = %" G_GINT64_FORMAT, series), ==, 0);
}

/* ── Changing the numbering (add-metadata-tvdb, V3) ─────────────── */

#define TVDB_NOTICE "Metadata provided by TheTVDB. Please consider adding missing information or subscribing."

/* Gullwing Ferry (made up): TMDb 7061 (the fixture provider stands in for
 * it), one season of ten; TheTVDB 81061, the same ten as two seasons of
 * five. */
static void
set_gullwing(Fixture *fx)
{
	static const gchar *const titles[] = {
		"Low Water", "The Ropes", "Gull Count", "Ticket Office", "Fog Horn", "Night Crossing", "The Pier Lamp",
		"Harbour Masters", "Rough Passage", "Last Sailing"
	};
	g_autoptr(GString) tmdb = g_string_new("{\"number\":1,\"name\":\"Season 1\",\"episodes\":[");
	g_autoptr(GString) tvdb = g_string_new("{\"status\":\"success\",\"data\":{\"series\":{\"id\":81061},\"episodes\":[");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(titles); i++) {
		g_autofree gchar *date = g_strdup_printf("2025-%02u-%02u", 1 + (6 + 7 * i) / 31, 1 + (5 + 7 * i) % 31);

		g_string_append_printf(tmdb, "%s{\"number\":%u,\"id\":\"96%03u\",\"name\":\"%s\",\"air_date\":\"%s\","
			"\"runtime\":24}", i > 0 ? "," : "", i + 1, i + 1, titles[i], date);
		g_string_append_printf(tvdb, "%s{\"id\":81061%03u,\"seriesId\":81061,\"name\":\"%s%s\",\"aired\":\"%s\","
			"\"runtime\":24,\"seasonNumber\":%u,\"number\":%u}", i > 0 ? "," : "", i + 1, titles[i], i == 7 ? "." : "",
			date, i < 5 ? 1 : 2, i < 5 ? i + 1 : i - 4);
	}
	g_string_append(tmdb, "]}");
	g_string_append(tvdb, "]},\"links\":{\"prev\":null,\"next\":null}}");
	set_route(fx, "/title/series/7061", "{\"id\":\"7061\",\"kind\":\"series\",\"title\":\"Gullwing Ferry\",\"year\":2025,"
		"\"status\":\"continuing\",\"runtime\":24,\"ids\":{\"tvdb\":\"81061\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"episodes\":10}]}");
	set_route(fx, "/title/series/7061/season/1", tmdb->str);
	axis_test_tvdb_set_answer(fx->tvdb, "/v4/series/81061/episodes/official/eng", "page=0", tvdb->str);
}

/* The digest the confirmation page holds. */
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

/* The form a place page's button posts: @file ("all" or an id) and its digest. */
static gchar *
place_form(
	const gchar *file,
	const gchar *body
){
	g_autofree gchar *digest = digest_of(body);

	return g_strdup_printf("file=%s&digest=%s", file, digest);
}

static gchar *
episode_rows(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT string_agg(id || ':' || season_number || 'x' || number || ':' || source || ':' ||"
		" (gone_at IS NULL)::text, ',' ORDER BY id) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);
}

/*
 * Spec (tv): The table; Nothing before the confirmation; Changed since;
 * Recorded; The key removed.  Spec (metadata): Every page; Beside the data;
 * No key, no notice.  Spec (title-search, in part): the series' page names
 * the numbering it shows.  Only an administrator sees the page and makes the
 * change.
 */
static void
test_numbering_pages(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *page_path = NULL;
	g_autofree gchar *change_path = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series;
	gint64 masters;
	gint64 events;

	set_gullwing(fx);
	series = add_series(fx, "7061", AXIS_TV_MONITOR_ALL);
	masters = episode_id(fx, series, 1, 8);
	in_library(fx, series, masters, "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E08.mkv",
		"webdl_1080p");
	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	page_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	change_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);

	/* No key, no series following it: TheTVDB is offered nowhere, its
	 * notice is on no page; TMDb's (the fixture's) is in every footer. */
	reply = get_as(fx, fx->admin, series_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Numbered as fixture numbers them.");
	assert_lacks(reply.body, "Change the episode numbering");
	assert_lacks(reply.body, TVDB_NOTICE);
	assert_contains(reply.body, "class=\"footer-notices\"");
	assert_contains(reply.body, "Test data from the fixture provider");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series");
	assert_lacks(reply.body, TVDB_NOTICE);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);

	/* With its key: the notice in every page's footer, with its link; the
	 * settings card offers the change to an administrator only. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->viewer, "/series/calendar");
	assert_contains(reply.body, TVDB_NOTICE);
	assert_contains(reply.body, "href=\"https://thetvdb.com/subscribe\"");
	assert_lacks(reply.body, AXIS_TEST_TVDB_KEY);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, series_path);
	assert_lacks(reply.body, "Change the episode numbering");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page_path);
	g_assert_cmpuint(reply.status, !=, 200);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Change the episode numbering");
	assert_contains(reply.body, page_path);
	axis_test_reply_clear(&reply);

	/* The page: what each episode becomes, writing nothing. */
	rows = episode_rows(fx, series);
	events = qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series);
	reply = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Change the episode numbering of Gullwing Ferry (2025)");
	assert_contains(reply.body, "Gullwing Ferry (2025) follows fixture now");
	assert_contains(reply.body, "Episodes with files or downloads");
	assert_contains(reply.body, "data-label=\"Numbering now\"");
	assert_contains(reply.body, "data-label=\"How matched\"");
	assert_contains(reply.body, ">S01E08</td>");
	assert_contains(reply.body, "S02E03 \"Harbour Masters.\", 2025-02-24");
	assert_contains(reply.body, "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E08.mkv");
	assert_contains(reply.body, "Air date and title");
	assert_contains(reply.body, "Files whose names and new episodes disagree");
	assert_contains(reply.body, "10 (10 by air date and title)");
	assert_contains(reply.body, TVDB_NOTICE);
	assert_contains(reply.body, "Change the numbering");
	assert_csp_clean(reply.body);
	save_page("series-numbering.html", reply.body);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	now = episode_rows(fx, series);
	g_assert_cmpstr(now, ==, rows);
	g_clear_pointer(&now, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT, series), ==, events);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE episode_source = 'fixture'"), ==, 1);

	/* Changed since: a file arrives; the confirmation is refused and the
	 * page drawn again, with it. */
	in_library(fx, series, episode_id(fx, series, 1, 2), "Gullwing Ferry (2025)/Season 01/Gullwing Ferry (2025) - S01E02.mkv",
		"webdl_1080p");
	form = g_strdup_printf("to=tvdb&digest=%s", digest);
	reply = post_as(fx, fx->admin, page_path, change_path, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "The series changed after this page was made");
	assert_contains(reply.body, "Gullwing Ferry (2025) - S01E02.mkv");
	g_clear_pointer(&digest, g_free);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE episode_source = 'fixture'"), ==, 1);
	/* A viewer cannot post it. */
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("to=tvdb&digest=%s", digest);
	reply = post_as(fx, fx->viewer, series_path, change_path, form);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE episode_source = 'fixture'"), ==, 1);

	/* Confirmed: the series follows TheTVDB's aired order, and its page says
	 * so beside its seasons, with TheTVDB's notice. */
	reply = post_as(fx, fx->admin, page_path, change_path, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=numbering");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE episode_source = 'tvdb'"), ==, 1);
	g_assert_cmpint(episode_id(fx, series, 2, 3), ==, masters);
	reply = get_as(fx, fx->viewer, series_path);
	assert_contains(reply.body, "Numbered as TheTVDB's aired order numbers them.");
	assert_contains(reply.body, "<dd>TheTVDB's aired order</dd>");
	assert_contains(reply.body, "Season 2");
	g_assert_cmpuint(count_of(reply.body, TVDB_NOTICE), >=, 2);
	assert_contains(reply.body, "Episode numbering changed from fixture to TheTVDB's aired order by root");
	axis_test_reply_clear(&reply);
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=numbering", series_path);

		reply = get_as(fx, fx->admin, done);
		assert_contains(reply.body, "The series follows TheTVDB's aired order now.");
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpuint(logged("now follows tvdb"), >=, 1);

	/* Spec (title-search): A managed series following TheTVDB: its title page
	 * shows TheTVDB's aired order, says so, and offers TMDb's; Looking
	 * changes nothing. */
	{
		g_autofree gchar *title_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, qi(fx, "SELECT title_id FROM series"
			" WHERE id = %" G_GINT64_FORMAT, series));
		g_autofree gchar *other_path = g_strdup_printf("%s?numbering=fixture", title_path);

		g_clear_pointer(&rows, g_free);
		rows = episode_rows(fx, series);
		reply = get_as(fx, fx->viewer, title_path);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "Numbered as TheTVDB's aired order numbers them.");
		assert_contains(reply.body, "Season 2");
		assert_contains(reply.body, TVDB_NOTICE);
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, title_path);
		assert_contains(reply.body, "?numbering=fixture#seasons");
		assert_contains(reply.body, "This series follows this numbering.");
		axis_test_reply_clear(&reply);
		reply = get_as(fx, fx->admin, other_path);
		g_assert_cmpuint(reply.status, ==, 200);
		assert_contains(reply.body, "A preview: this series follows TheTVDB's aired order. Nothing is changed by looking.");
		assert_contains(reply.body, "10 episodes");
		axis_test_reply_clear(&reply);
		now = episode_rows(fx, series);
		g_assert_cmpstr(now, ==, rows);
		g_clear_pointer(&now, g_free);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE episode_source = 'tvdb'"), ==, 1);
	}

	/* The key removed: the series keeps working, says why its numbering is
	 * not refreshed, and offers the way back; TheTVDB's notice stays while
	 * it follows it. */
	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TVDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	g_clear_pointer(&rows, g_free);
	rows = episode_rows(fx, series);
	g_assert_true(axis_tv_sync(fx->tv, series, TRUE, NULL, &error));
	g_assert_no_error(error);
	now = episode_rows(fx, series);
	g_assert_cmpstr(now, ==, rows);
	g_clear_pointer(&now, g_free);
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Its numbering is not being refreshed: TheTVDB has no key");
	assert_contains(reply.body, "/numbering?to=fixture");
	assert_contains(reply.body, TVDB_NOTICE);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/series");
	assert_contains(reply.body, TVDB_NOTICE);
	axis_test_reply_clear(&reply);

	/* Back to TMDb's numbering: no key and no series following TheTVDB, so
	 * its notice is gone from every page. */
	g_clear_pointer(&page_path, g_free);
	page_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=fixture", series);
	reply = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "As before");
	g_clear_pointer(&digest, g_free);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("to=fixture&digest=%s", digest);
	reply = post_as(fx, fx->admin, page_path, change_path, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(episode_id(fx, series, 1, 8), ==, masters);
	reply = get_as(fx, fx->viewer, series_path);
	assert_lacks(reply.body, TVDB_NOTICE);
	assert_contains(reply.body, "Numbered as fixture numbers them.");
	axis_test_reply_clear(&reply);
}

/* Spec (tv): A pack downloading: the page names it and offers no
 * confirmation; a post is refused with it, and nothing changes. */
static void
test_numbering_waits(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page_path = NULL;
	g_autofree gchar *change_path = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series;
	gint64 pack;

	set_gullwing(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7061", AXIS_TV_MONITOR_ALL);
	page_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	change_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);
	reply = get_as(fx, fx->admin, page_path);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);

	pack = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, scope, season_number)"
		" VALUES (%" G_GINT64_FORMAT ", 'Gullwing.Ferry.S01.1080p.WEB-DL.DDP5.1.H.264-NTb', 'usenet',"
		" 'gullwing ferry s01', 'season', 1) RETURNING id", series);
	{
		g_autofree gchar *sql = g_strdup_printf("INSERT INTO series_grab_episode (grab_id, episode_id) SELECT %"
			G_GINT64_FORMAT ", id FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, pack, series);

		axis_test_db_exec(fx->t->db, sql);
	}
	reply = get_as(fx, fx->admin, page_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "The change waits for Gullwing.Ferry.S01.1080p.WEB-DL.DDP5.1.H.264-NTb (downloading)");
	assert_contains(reply.body, "once each is imported or dismissed");
	assert_lacks(reply.body, "name=\"digest\"");
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=tvdb&digest=%s", digest);
	reply = post_as(fx, fx->admin, page_path, change_path, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Gullwing.Ferry.S01.1080p.WEB-DL.DDP5.1.H.264-NTb (downloading)");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE episode_source = 'fixture'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'numbering'"), ==, 0);
}

/* Review of V3, item 9: the other numbering of a title page, and its fetch
 * from TheTVDB, are an administrator's; so is why a numbering is not
 * refreshed.  A viewer sees the numbering the series follows, and asks
 * TheTVDB nothing. */
static void
test_numbering_viewer(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autofree gchar *title_path = NULL;
	g_autofree gchar *other_path = NULL;
	g_autofree gchar *series_path = NULL;
	AxisTestReply reply;
	gboolean changed = TRUE;
	gint64 series;
	guint gets;

	set_gullwing(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7061", AXIS_TV_MONITOR_ALL);
	title_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, qi(fx, "SELECT title_id FROM series WHERE id = %"
		G_GINT64_FORMAT, series));
	other_path = g_strdup_printf("%s?numbering=tvdb", title_path);
	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	gets = axis_test_tvdb_gets(fx->tvdb);
	reply = get_as(fx, fx->viewer, other_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Numbered as Fixture numbers them.");
	assert_lacks(reply.body, "aria-label=\"Numbering\"");
	assert_lacks(reply.body, "A preview");
	assert_lacks(reply.body, "Season 2");
	axis_test_reply_clear(&reply);
	g_assert_cmpuint(axis_test_tvdb_gets(fx->tvdb), ==, gets);
	reply = get_as(fx, fx->admin, other_path);
	assert_contains(reply.body, "A preview: this series follows Fixture's numbering.");
	axis_test_reply_clear(&reply);

	/* Following TheTVDB without its key: the administrator is told why it is
	 * not refreshed; a viewer sees the episodes as they are. */
	g_assert_true(axis_tv_plan_numbering(fx->tv, series, "tvdb", TRUE, &plan, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed,
		&error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TVDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->viewer, series_path);
	assert_lacks(reply.body, "not being refreshed");
	assert_contains(reply.body, "Numbered as TheTVDB's aired order numbers them.");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Its numbering is not being refreshed");
	axis_test_reply_clear(&reply);
	/* Review item 10: the notice after a change says what was done. */
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=numbering", series_path);

		reply = get_as(fx, fx->admin, done);
		assert_contains(reply.body, "Every file stays where it was on disk, linked to the episode it holds");
		axis_test_reply_clear(&reply);
	}
}

static void assert_matches(const gchar *haystack, const gchar *pattern);

/*
 * A first switch of numbering, made up: Tidewater Lane (TMDb 7062, TheTVDB
 * 81062).  TMDb's season 1 begins with what TheTVDB files as a year-named
 * season's first episode (S2023E01 "Lamp Room"); TheTVDB's season 1 begins
 * with an episode TMDb lacks ("First Night"); the other four are alike.
 * TheTVDB also has a second episode of the year-named season.
 */
static void
set_tidewater(Fixture *fx)
{
	set_route(fx, "/title/series/7062", "{\"id\":\"7062\",\"kind\":\"series\",\"title\":\"Tidewater Lane\",\"year\":2023,"
		"\"status\":\"continuing\",\"runtime\":24,\"ids\":{\"tvdb\":\"81062\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"episodes\":5}]}");
	set_route(fx, "/title/series/7062/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"97001\",\"name\":\"Lamp Room\",\"air_date\":\"2023-01-02\",\"runtime\":24},"
		"{\"number\":2,\"id\":\"97002\",\"name\":\"Second Beam\",\"air_date\":\"2023-02-06\",\"runtime\":24},"
		"{\"number\":3,\"id\":\"97003\",\"name\":\"Fog Week\",\"air_date\":\"2023-02-13\",\"runtime\":24},"
		"{\"number\":4,\"id\":\"97004\",\"name\":\"Harbour Gate\",\"air_date\":\"2023-02-20\",\"runtime\":24},"
		"{\"number\":5,\"id\":\"97005\",\"name\":\"Low Water\",\"air_date\":\"2023-02-27\",\"runtime\":24}]}");
	axis_test_tvdb_set_answer(fx->tvdb, "/v4/series/81062/episodes/official/eng", "page=0", "{\"status\":\"success\","
		"\"data\":{\"series\":{\"id\":81062},\"episodes\":["
		"{\"id\":81062101,\"name\":\"First Night\",\"aired\":\"2023-01-30\",\"runtime\":24,\"seasonNumber\":1,\"number\":1},"
		"{\"id\":81062102,\"name\":\"Second Beam\",\"aired\":\"2023-02-06\",\"runtime\":24,\"seasonNumber\":1,\"number\":2},"
		"{\"id\":81062103,\"name\":\"Fog Week\",\"aired\":\"2023-02-13\",\"runtime\":24,\"seasonNumber\":1,\"number\":3},"
		"{\"id\":81062104,\"name\":\"Harbour Gate\",\"aired\":\"2023-02-20\",\"runtime\":24,\"seasonNumber\":1,\"number\":4},"
		"{\"id\":81062105,\"name\":\"Low Water\",\"aired\":\"2023-02-27\",\"runtime\":24,\"seasonNumber\":1,\"number\":5},"
		"{\"id\":81062201,\"name\":\"Lamp Room\",\"aired\":\"2023-01-02\",\"runtime\":24,\"seasonNumber\":2023,\"number\":1},"
		"{\"id\":81062202,\"name\":\"Lamp Room Again\",\"aired\":\"2023-12-25\",\"runtime\":24,\"seasonNumber\":2023,"
		"\"number\":2}]},\"links\":{\"prev\":null,\"next\":null}}");
}

/* Tidewater Lane added and keyed, its five files imported from releases
 * named in TheTVDB's numbering (S01E01 to S01E05), each on TMDb's episode
 * of that number; changed to TheTVDB's aired order through its page
 * (the box as it was then: unticked). */
/* The series' files, linked by an import Axis made. */
static void
mark_imported(
	Fixture *fx,
	gint64   series
){
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, title_label, source_path, state, finished_at)"
		" VALUES ('tv', 'Tidewater Lane (2023)', 'downloads/x', 'imported', now())");
	g_assert_cmpint(qi(fx, "WITH m AS (UPDATE series_file SET import_id = (SELECT max(id) FROM import) WHERE series_id = %"
		G_GINT64_FORMAT " RETURNING 1) SELECT count(*) FROM m", series), >, 0);
}

static gint64
tidewater_switched_as(
	Fixture  *fx,
	gboolean  imported)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *change = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series;
	guint i;

	set_tidewater(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
	for (i = 1; i <= 5; i++) {
		g_autofree gchar *rel = g_strdup_printf("Tidewater Lane (2023)/Season 01/Tidewater Lane (2023) - S01E%02u.mkv",
			i);

		in_library(fx, series, episode_id(fx, series, 1, (gint)i), rel, "webdl_1080p");
	}
	if (imported)
		mark_imported(fx, series);
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=tvdb&digest=%s", digest);
	reply = post_as(fx, fx->admin, page, change, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	return series;
}

static gint64
tidewater_switched(Fixture *fx)
{
	return tidewater_switched_as(fx, FALSE);
}

/*
 * The case of design.md, open question 7: after the switch the file
 * named S01E01 followed its row to S2023E01, and S01E01 is wanted.  The
 * series page lists it under "Files whose names say another episode"; placing
 * it by its name (an administrator's, after a page showing what changes)
 * moves only its link, says so in both episodes' history, leaves S2023E01
 * wanted as it is monitored, and S01E01 is no longer wanted.
 */
static void
test_place_by_name(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_switched(fx);
	gint64 lamp = episode_id(fx, series, 2023, 1);
	gint64 first = episode_id(fx, series, 1, 1);
	gint64 file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp);

	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	rel = q(fx, "SELECT rel_path FROM library_file WHERE id = %" G_GINT64_FORMAT, file);
	assert_contains(rel, "S01E01");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, first), ==, 0);

	/* Listed for an administrator, with the way to place it. */
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Named by Axis under the earlier numbering");
	assert_contains(reply.body, "Tidewater Lane (2023) - S01E01.mkv");
	assert_contains(reply.body, place_path);
	assert_lacks(reply.body, "Tidewater Lane (2023) - S01E02.mkv</td>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, series_path);
	assert_lacks(reply.body, "Named by Axis under the earlier numbering");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, place_path);
	g_assert_cmpuint(reply.status, !=, 200);
	axis_test_reply_clear(&reply);

	/* What changes, before it does. */
	reply = get_as(fx, fx->admin, place_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "S01E01 holds the file; S2023E01 is left without one and, monitored and aired, is "
		"wanted and searched for.");
	assert_contains(reply.body, "Place by its name");
	assert_contains(reply.body, "probably just the old number");
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file);

		form = place_form(id, reply.body);
	}
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp), ==, 1);

	/* Placed: the link moved, the file did not. */
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "?done=placed");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT
		" AND file_id = %" G_GINT64_FORMAT, first, file), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp), ==, 0);
	{
		g_autofree gchar *after = q(fx, "SELECT rel_path FROM library_file WHERE id = %" G_GINT64_FORMAT, file);

		g_assert_cmpstr(after, ==, rel);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'changed' AND episode_id IN (%"
		G_GINT64_FORMAT ", %" G_GINT64_FORMAT ") AND text LIKE '%%placed by its name%%'", first, lamp), ==, 2);
	reply = get_as(fx, fx->admin, series_path);
	assert_lacks(reply.body, "Named by Axis under the earlier numbering");
	axis_test_reply_clear(&reply);
}

/* The same case again, with the confirmation page's tick: the disagree
 * card says where placing by its name puts the file (off by default), and
 * ticked, the change places it in the same transaction. */
static void
test_names_on_change(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *change = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series;
	gint64 file;
	guint i;

	set_tidewater(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
	for (i = 1; i <= 5; i++) {
		g_autofree gchar *rel = g_strdup_printf("Tidewater Lane (2023)/Season 01/Tidewater Lane (2023) - S01E%02u.mkv",
			i);

		in_library(fx, series, episode_id(fx, series, 1, (gint)i), rel, "webdl_1080p");
	}
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1));
	/* Read under TheTVDB's numbering already (the series followed it before). */
	axis_test_db_exec(fx->t->db, "UPDATE series_file SET read_source = 'tvdb'");
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "Placed on S01E01");
	assert_contains(reply.body, "After the change, place these files by their names");
	assert_matches(reply.body, "<input [^>]*id=\"numbering-names\"");
	g_assert_false(g_regex_match_simple("(checked=\"checked\"[^>]*id=\"numbering-names\"|id=\"numbering-names\"[^>]*"
		"checked)", reply.body, 0, 0));
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=tvdb&digest=%s&names=yes", digest);
	reply = post_as(fx, fx->admin, page, change, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT
		" AND episode_id = %" G_GINT64_FORMAT, file, episode_id(fx, series, 1, 1)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 2023, 1)), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND episode_source = 'tvdb'",
		series), ==, 1);
}

/* Many new episodes at once, made small: new episodes in a season the series
 * did not have are left unmonitored unless the box is unticked, and the page
 * says first how many become monitored and searched for. */
static void
test_new_season_default(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	AxisTestReply reply;
	gint64 series;

	set_tidewater(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "2 new episodes: 1 in a season this series did not have and 1 in a season it has.");
	assert_contains(reply.body, "The 1 in a season it has follows that season's monitoring: 1 monitored and 1 searched "
		"for at once.");
	assert_contains(reply.body, "The 1 in a new season is left unmonitored with the box at the bottom ticked, as it is "
		"now");
	assert_matches(reply.body, "(checked=\"checked\"[^>]*id=\"numbering-unmonitored\"|id=\"numbering-unmonitored\""
		"[^>]*checked=\"checked\")");
	axis_test_reply_clear(&reply);
}

/* The box leaves unmonitored only the episodes of seasons that did not
 * exist; a new episode of a season the person monitors follows that season. */
static void
test_new_season_only(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *change = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series;

	set_tidewater(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);
	reply = get_as(fx, fx->admin, page);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=tvdb&digest=%s&unmonitored=yes", digest);
	reply = post_as(fx, fx->admin, page, change, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	/* First Night: a new episode of season 1, which is monitored. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND season_number = 1 AND number = 1 AND gone_at IS NULL AND monitored", series), ==, 1);
	/* Lamp Room Again: in the season 2023 the series did not have. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND season_number = 2023 AND number = 2 AND gone_at IS NULL AND NOT monitored", series), ==, 1);
}

/* Blocker 1: a file an import by Axis linked is named by the number its
 * episode had when it was imported.  After the switch it is on the right
 * episode, and "Place all" never moves it. */
static void
test_place_imported_kept(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_switched_as(fx, TRUE);
	gint64 lamp = episode_id(fx, series, 2023, 1);
	gint64 file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp);

	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);

	/* Listed apart, with the sentence; no "Place all". */
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Named by Axis under the earlier numbering");
	assert_contains(reply.body, "probably just the old number");
	assert_lacks(reply.body, "Place all by their names");
	assert_lacks(reply.body, "place?file=all");
	assert_contains(reply.body, place_path);
	axis_test_reply_clear(&reply);

	/* A "Place all" posted anyway (a stale tab) moves nothing. */
	form = g_strdup_printf("file=all&digest=%s", "00000000000000000000000000000000");
	reply = post_as(fx, fx->admin, series_path, place_action, form);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT
		" AND episode_id = %" G_GINT64_FORMAT, file, lamp), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE text LIKE '%%placed by its name%%'"), ==, 0);
}

/* The series: Tidewater Lane following TMDb's numbering, five episodes, none
 * with a file; files linked by identification, named in that numbering. */
static gint64
tidewater_plain(Fixture *fx)
{
	set_tidewater(fx);
	return add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
}

static void
tidewater_file(
	Fixture     *fx,
	gint64       series,
	gint         on,
	const gchar *name
){
	g_autofree gchar *rel = g_strdup_printf("Tidewater Lane (2023)/Season 01/%s", name);

	in_library(fx, series, episode_id(fx, series, 1, on), rel, "webdl_1080p");
}

/* Blocker 2: one episode holds two files, named for two empty episodes:
 * both are placed, the episode is left empty, and the page says so (wanted)
 * before it happens; and two files named for one empty episode never land
 * both on it. */
static void
test_place_two_files(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);

	/* Season 1 episode 1 holds two files named for episodes 4 and 5. */
	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E04.mkv");
	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E05.mkv");
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", series);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	reply = get_as(fx, fx->admin, place_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "leaves 1 episode without a file: S01E01 (wanted, searched for at once)");
	assert_lacks(reply.body, "not wanted");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("file=all&digest=%s", digest);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1)), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 4)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 5)), ==, 1);
}

static void
test_place_same_target(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);

	/* Two files, on two episodes, both named for the empty episode 3. */
	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E03.mkv");
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E03 alt.mkv");
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", series);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	reply = get_as(fx, fx->admin, place_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "has a file of its own");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("file=all&digest=%s", digest);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 3)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 2)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1)), ==, 0);
}

/* The confirmation page's drawn plan, for a row of two files, is what the
 * change does: the same two files, the same empty episode. */
static void
test_names_two_files(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *change = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series;

	set_tidewater(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E04.mkv");
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E05.mkv");
	axis_test_db_exec(fx->t->db, "UPDATE series_file SET read_source = 'tvdb'");
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "leaves 1 episode without a file: S01E02 (wanted, searched for at once)");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=tvdb&digest=%s&names=yes", digest);
	reply = post_as(fx, fx->admin, page, change, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 2)), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 4)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 5)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT " AND monitored",
		episode_id(fx, series, 1, 2)), ==, 1);
}

/* Fix 3: "Place all" carries the digest of the list shown. */
static void
test_place_digest(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);

	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E03.mkv");
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", series);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	reply = get_as(fx, fx->admin, place_path);
	assert_contains(reply.body, "Place all by their names");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	/* What can be placed changes after the page was drawn. */
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E04.mkv");
	form = g_strdup_printf("file=all&digest=%s", digest);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "S01E04");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 3)), ==, 0);
	/* The page redrawn holds the new digest: that one places. */
	reply = get_as(fx, fx->admin, place_path);
	g_free(digest);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	g_free(form);
	form = g_strdup_printf("file=all&digest=%s", digest);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 3)), ==, 1);
}

/* Fix 4: a manual import of the series that runs refuses the placing. */
static void
test_place_manual_import(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 file;

	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E03.mkv");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1));
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, title_id, title_label, source_path, state, kind)"
		" SELECT 'tv', title_id, 'Tidewater Lane (2023)', 'downloads/x', 'running', 'manual' FROM series");
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	/* The refusal names the import by its real state, with where to find it
	 * (waiting and blocked ones are open too). */
	{
		static const gchar *const states[][2] = {
			{ "running", "is running" }, { "waiting", "is waiting to go on" },
			{ "blocked", "is waiting for a person to decide" }
		};
		guint s;

		for (s = 0; s < G_N_ELEMENTS(states); s++) {
			g_autofree gchar *sql = g_strdup_printf("UPDATE import SET state = '%s'", states[s][0]);

			axis_test_db_exec(fx->t->db, sql);
			reply = get_as(fx, fx->admin, place_path);
			assert_contains(reply.body, states[s][1]);
			assert_contains(reply.body, "/imports/");
			assert_lacks(reply.body, "name=\"digest\"");
			axis_test_reply_clear(&reply);
			g_free(form);
			form = g_strdup_printf("file=%" G_GINT64_FORMAT "&digest=%s", file, "00000000000000000000000000000000");
			reply = post_as(fx, fx->admin, place_path, place_action, form);
			g_assert_cmpuint(reply.status, ==, 409);
			axis_test_reply_clear(&reply);
			g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
				episode_id(fx, series, 1, 3)), ==, 0);
		}
	}
	axis_test_db_exec(fx->t->db, "UPDATE import SET state = 'imported', finished_at = now()");
	reply = get_as(fx, fx->admin, place_path);
	g_free(form);
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file);

		form = place_form(id, reply.body);
	}
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 3)), ==, 1);
}

/* The page of one file judges it alone, against the series as it is, as the
 * POST places it: another file's move in "Place all" is not counted. */
static void
test_place_single_alone(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 file;

	/* f (lower id) on E1 named E2; g on E3 named E1.  In sequence both would
	 * go and E1 be filled again; alone, f leaves E1 empty and wanted. */
	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E02.mkv");
	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E01 alt.mkv");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1));
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	reply = get_as(fx, fx->admin, place_path);
	assert_contains(reply.body, "S01E02 holds the file; S01E01 is left without one and, monitored and aired, is wanted "
		"and searched for.");
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file);

		form = place_form(id, reply.body);
	}
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1)), ==, 0);
}

static void
test_place_single_refused(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 file;

	/* g (lower id) on E2 named E3; f on E1 named E2: in sequence g leaves
	 * E2 and f follows; alone, f finds E2 holding g. */
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E03.mkv");
	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E02 alt.mkv");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1));
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	reply = get_as(fx, fx->admin, place_path);
	assert_contains(reply.body, "has a file of its own");
	assert_lacks(reply.body, "S01E02 holds the file");
	axis_test_reply_clear(&reply);
}

/* What "left without a file, wanted" depends on is in the digest, and one
 * file's POST carries one too. */
static void
test_place_digest_state(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *one_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *digest = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 file;

	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E02.mkv");
	file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1));
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", series);
	one_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	/* One file, no digest: refused. */
	form = g_strdup_printf("file=%" G_GINT64_FORMAT, file);
	reply = post_as(fx, fx->admin, one_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, place_path);
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	/* The episode it leaves stops being monitored after the page was drawn. */
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET monitored = false WHERE season_number = 1 AND number = 1");
	g_free(form);
	form = g_strdup_printf("file=all&digest=%s", digest);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 2)), ==, 0);
}

/* A chain (E1's file named E2, E2's named E3) resolves from its end: the
 * page and the placing agree on all of it. */
static void
test_place_chain(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);

	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E02.mkv");
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E03.mkv");
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", series);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	reply = get_as(fx, fx->admin, place_path);
	assert_lacks(reply.body, "has a file of its own");
	assert_contains(reply.body, "leaves 1 episode without a file: S01E01");
	form = place_form("all", reply.body);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 1)), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 2)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 3)), ==, 1);
}

/* The confirmation page says why nothing will be placed while an import of
 * the series is open, instead of "Placed on". */
static void
test_names_busy(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	AxisTestReply reply;
	gint64 series;

	set_tidewater(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7062", AXIS_TV_MONITOR_ALL);
	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E04.mkv");
	axis_test_db_exec(fx->t->db, "UPDATE series_file SET read_source = 'tvdb'");
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, title_id, title_label, source_path, state, kind)"
		" SELECT 'tv', title_id, 'Tidewater Lane (2023)', 'downloads/x', 'waiting', 'manual' FROM series");
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "files are placed once no import is open");
	assert_lacks(reply.body, "Placed on S01E04");
	axis_test_reply_clear(&reply);
}

/* ── Links by hand (add-force-grab-and-assign, C1) ──────────────── */

/* A video file in Tidewater Lane's folder that no episode holds; its id. */
static gint64
loose_file(
	Fixture     *fx,
	const gchar *name
){
	static gint64 serial = 0;

	return qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'Tidewater Lane (2023)/Season 01/%s', 'video', 1600000000, 1, 0, 1, 0, %" G_GINT64_FORMAT ", 1, false, false,"
		" false, 1, 1, 1) RETURNING id", fx->root_id, fx->volume_id, name, 900000000 + serial++);
}

static gint64
file_on_episode(
	Fixture *fx,
	gint64   series,
	gint     season,
	gint     number
){
	return qi(fx, "SELECT min(file_id) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, season, number));
}

/* Every link of the series: "<the file's name after ' - '>>S01E03", by the
 * files' ids (the order they were made in). */
static gchar *
links_of(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT coalesce(string_agg(split_part(lf.rel_path, ' - ', 2) || '>S' || lpad(e.season_number::text, 2,"
		" '0') || 'E' || lpad(e.number::text, 2, '0'), ',' ORDER BY lf.id, e.season_number, e.number), '')"
		" FROM series_file sf JOIN series_file_episode f ON f.file_id = sf.file_id JOIN series_episode e ON e.id ="
		" f.episode_id JOIN library_file lf ON lf.id = sf.file_id WHERE sf.series_id = %" G_GINT64_FORMAT, series);
}

static void
assert_links(
	Fixture     *fx,
	gint64       series,
	const gchar *expected
){
	g_autofree gchar *links = links_of(fx, series);

	g_assert_cmpstr(links, ==, expected);
}

/* The review of @plan ("to_12=S01E03&unlink_13=yes"). */
static AxisTestReply
links_review(
	Fixture     *fx,
	gint64       series,
	const gchar *plan
){
	g_autofree gchar *path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	g_autofree gchar *form = g_strdup_printf("step=review&%s", plan);

	return post_as(fx, fx->admin, path, path, form);
}

/* @plan applied with the digest of its review @body (and @ticks, "" for
 * none). */
static AxisTestReply
links_apply(
	Fixture     *fx,
	gint64       series,
	const gchar *plan,
	const gchar *ticks,
	const gchar *body
){
	g_autofree gchar *path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	g_autofree gchar *digest = digest_of(body);
	g_autofree gchar *form = g_strdup_printf("step=apply&digest=%s&%s%s%s", digest, plan, ticks[0] != '\0' ? "&" : "",
		ticks);

	return post_as(fx, fx->admin, path, path, form);
}

/* Reviewed, then applied as reviewed. */
static void
links_change(
	Fixture     *fx,
	gint64       series,
	const gchar *plan
){
	AxisTestReply review = links_review(fx, series, plan);
	AxisTestReply reply;

	g_assert_cmpuint(review.status, ==, 200);
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=linked");
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
}

/*
 * Spec (tv): Two files swapped; A file with no link (a double episode); The
 * episode has a file already (without the tick, and with it); Out of date; A
 * grab in flight; and an open import, a viewer, a post with no token, a file
 * and an episode that are not the series', "Let Axis manage this file again".
 * Tidewater Lane follows TMDb: S01E03, S01E04 and S01E05 hold a file each.
 */
static void
test_links(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *manage_path = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *ticks = NULL;
	g_autofree gchar *paths = NULL;
	g_autofree gchar *paths_after = NULL;
	AxisTestReply reply;
	AxisTestReply review;
	gint64 series = tidewater_plain(fx);
	gint64 a;
	gint64 b;
	gint64 c;
	gint64 d;
	gint64 e;
	gint64 f;
	gint64 grab;

	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E03.mkv");
	tidewater_file(fx, series, 4, "Tidewater Lane (2023) - S01E04.mkv");
	tidewater_file(fx, series, 5, "Tidewater Lane (2023) - S01E05.mkv");
	a = file_on_episode(fx, series, 1, 3);
	b = file_on_episode(fx, series, 1, 4);
	c = file_on_episode(fx, series, 1, 5);
	d = loose_file(fx, "Tidewater Lane (2023) - S01E01-E02.mkv");
	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	links_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	manage_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links/manage", series);
	paths = q(fx, "SELECT string_agg(id || ':' || rel_path || ':' || state, ',' ORDER BY id) FROM library_file");

	/* A viewer sees nothing new and changes nothing. */
	reply = get_as(fx, fx->viewer, series_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Files and links");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, links_path);
	g_assert_cmpuint(reply.status, !=, 200);
	assert_lacks(reply.body, "S01E03.mkv");
	axis_test_reply_clear(&reply);
	plan = g_strdup_printf("step=review&unlink_%" G_GINT64_FORMAT "=yes", a);
	reply = post_as(fx, fx->viewer, series_path, links_path, plan);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	/* An administrator's post with no token is refused too. */
	reply = post_as(fx, fx->admin, NULL, links_path, plan);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&plan, g_free);

	/* The page: the linked files, the folder's file with no link. */
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Files and links");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, links_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_csp_clean(reply.body);
	assert_contains(reply.body, "Linked files");
	assert_contains(reply.body, "Tidewater Lane (2023) - S01E03.mkv");
	assert_contains(reply.body, "Files with no link");
	assert_contains(reply.body, "Tidewater Lane (2023) - S01E01-E02.mkv");
	assert_contains(reply.body, "A media server reads a file's name");
	assert_contains(reply.body, "data-label=\"Change\"");
	assert_lacks(reply.body, "Let Axis manage this file again");
	axis_test_reply_clear(&reply);

	/* A file or an episode that is not the series' is refused. */
	review = links_review(fx, series, "to_999999=S01E01");
	g_assert_cmpuint(review.status, ==, 400);
	axis_test_reply_clear(&review);
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S09E09", a);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 400);
	assert_contains(review.body, "no episode S09E09");
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);

	/* Two files swapped: reviewed (nothing written), then applied as one. */
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E04&to_%" G_GINT64_FORMAT "=s01e03", a, b);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_csp_clean(review.body);
	assert_contains(review.body, "Its name still says S01E03. A media server reads the name, so it may show this file as "
		"S01E03.");
	assert_contains(review.body, "Its name still says S01E04.");
	assert_contains(review.body, "One file, as before.");
	assert_lacks(review.body, "Left without a file");
	assert_lacks(review.body, "name=\"other_");
	assert_contains(review.body, "data-label=\"On after\"");
	assert_links(fx, series, "S01E03.mkv>S01E03,S01E04.mkv>S01E04,S01E05.mkv>S01E05");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'hand_link'"), ==, 0);
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "/links?done=linked");
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);
	assert_links(fx, series, "S01E03.mkv>S01E04,S01E04.mkv>S01E03,S01E05.mkv>S01E05");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		") AND hand_how = 'linked' AND hand_by = 'root' AND hand_at IS NOT NULL AND kept AND identity_id IS NULL"
		" AND read_source = 'fixture'", a, b), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_at IS NULL"
		" AND NOT kept", c), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'hand_link' AND by_user = 'root'"
		" AND text LIKE '%%by hand by root%%' AND text LIKE '%%The file on disk was not touched.' AND episode_id IN (%"
		G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")", episode_id(fx, series, 1, 3), episode_id(fx, series, 1, 4)), ==, 4);
	g_assert_cmpuint(logged("links of 2 files changed by hand"), ==, 1);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "Linked by hand by root on ");
	assert_contains(reply.body, "; kept by hand");
	assert_contains(reply.body, "Let Axis manage this file again");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Its file was linked by hand.");
	axis_test_reply_clear(&reply);

	/* A file with no link, linked to two episodes (a double episode). */
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E01%%20S01E02", d);
	links_change(fx, series, plan);
	g_clear_pointer(&plan, g_free);
	assert_links(fx, series, "S01E03.mkv>S01E04,S01E04.mkv>S01E03,S01E05.mkv>S01E05,S01E01-E02.mkv>S01E01,"
		"S01E01-E02.mkv>S01E02");

	/* The episode has a file already: without the tick both stay linked, and
	 * the review and the page afterwards say so. */
	e = loose_file(fx, "Tidewater Lane (2023) - S01E05 alt.mkv");
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E05", e);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "S01E05 has a file already. After this it has 2 files linked: nothing is replaced");
	ticks = g_strdup_printf("name=\"other_%" G_GINT64_FORMAT "\"", c);
	assert_contains(review.body, ticks);
	assert_contains(review.body, "from S01E05 instead. It is then on: No episode.");
	g_clear_pointer(&ticks, g_free);
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 5)), ==, 2);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "S01E05 has 2 files linked: ");
	assert_contains(reply.body, "Episodes with more than one file");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'hand_link' AND episode_id = %"
		G_GINT64_FORMAT " AND text LIKE '%%S01E05 now has 2 files linked%%'", episode_id(fx, series, 1, 5)), ==, 1);

	/* With the ticks: the others are unlinked, and remembered as unlinked by
	 * hand; their files stay. */
	f = loose_file(fx, "Tidewater Lane (2023) - S01E05 third.mkv");
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E05", f);
	ticks = g_strdup_printf("other_%" G_GINT64_FORMAT "=yes&other_%" G_GINT64_FORMAT "=yes", c, e);
	{
		/* The ticks are part of the review (and of its digest). */
		g_autofree gchar *with_ticks = g_strdup_printf("%s&%s", plan, ticks);

		review = links_review(fx, series, with_ticks);
		assert_contains(review.body, "You ticked this file");
		assert_contains(review.body, "S01E05 alt.mkv");
		reply = links_apply(fx, series, plan, ticks, review.body);
		g_assert_cmpuint(reply.status, ==, 303);
	}
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);
	g_clear_pointer(&ticks, g_free);
	g_assert_cmpint(qi(fx, "SELECT min(file_id) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 5)), ==, f);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 5)), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT
		")", c, e), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE module = 'tv' AND owner_id = %" G_GINT64_FORMAT
		" AND by_user = 'root' AND file_id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")", series, c, e), ==, 2);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "Unlinked by hand by root on ");
	assert_lacks(reply.body, "Episodes with more than one file");
	axis_test_reply_clear(&reply);

	/* Out of date: a file of the series arrives after the review. */
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", a);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "Left without a file: it is monitored and has aired, so it is wanted and searched for.");
	tidewater_file(fx, series, 4, "Tidewater Lane (2023) - S01E04 arrived.mkv");
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "so nothing was changed");
	assert_contains(reply.body, "S01E04 arrived.mkv");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, a), ==, 1);
	axis_test_reply_clear(&review);
	/* The review shown again is one that can be confirmed. */
	review = reply;
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, a), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, a), ==, 1);

	/* A grab in flight for an episode the change touches: nothing changes,
	 * and the page says to wait for it or dismiss it. */
	grab = qi(fx, "INSERT INTO series_grab (series_id, release_name, protocol, name_key, scope, episode_id)"
		" VALUES (%" G_GINT64_FORMAT ", 'Tidewater.Lane.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb', 'usenet',"
		" 'tidewater lane s01e03', 'episode', %" G_GINT64_FORMAT ") RETURNING id", series, episode_id(fx, series, 1, 3));
	g_assert_cmpint(qi(fx, "INSERT INTO series_grab_episode (grab_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ") RETURNING grab_id", grab, episode_id(fx, series, 1, 3)), ==, grab);
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", b);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "A download for S01E03 is in flight (Tidewater.Lane.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb)."
		" Wait for it to be imported, or dismiss it on the Downloads page (/downloads)");
	assert_lacks(review.body, "Change the links");
	assert_lacks(review.body, "name=\"digest\"");
	axis_test_reply_clear(&review);
	{
		g_autofree gchar *form = g_strdup_printf("step=apply&digest=00000000000000000000000000000000&%s", plan);

		reply = post_as(fx, fx->admin, links_path, links_path, form);
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, b), ==, 1);
	axis_test_db_exec(fx->t->db, "DELETE FROM series_grab");

	/* An open import of the series: refused, saying which and where. */
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, title_id, title_label, source_path, state, kind)"
		" SELECT 'tv', title_id, 'Tidewater Lane (2023)', 'downloads/x', 'blocked', 'manual' FROM series");
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "No link can be changed now. An import of this series (open /imports/");
	assert_lacks(reply.body, "Review the change");
	axis_test_reply_clear(&reply);
	review = links_review(fx, series, plan);
	assert_contains(review.body, "is waiting for a person to decide");
	assert_lacks(review.body, "name=\"digest\"");
	axis_test_reply_clear(&review);
	{
		g_autofree gchar *form = g_strdup_printf("step=apply&digest=00000000000000000000000000000000&%s", plan);

		reply = post_as(fx, fx->admin, links_path, links_path, form);
		g_assert_cmpuint(reply.status, ==, 409);
		axis_test_reply_clear(&reply);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, b), ==, 1);
	axis_test_db_exec(fx->t->db, "UPDATE import SET state = 'imported', finished_at = now()");
	g_clear_pointer(&plan, g_free);

	/* Let Axis manage this file again: the link's mark, and an unlink's. */
	plan = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, b);
	reply = post_as(fx, fx->viewer, series_path, manage_path, plan);
	g_assert_cmpuint(reply.status, !=, 200);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND kept", b), ==, 1);
	/* The file's name gives identification no episode here (it has no
	 * identity): the page says so, offers nothing to confirm, and the link
	 * stays as it is, still marked by hand. */
	reply = post_as(fx, fx->admin, links_path, manage_path, plan);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Identification would give");
	assert_contains(reply.body, "still marked by hand");
	assert_lacks(reply.body, "name=\"digest\"");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&plan, g_free);
	plan = g_strdup_printf("step=apply&file=%" G_GINT64_FORMAT "&digest=00000000000000000000000000000000", b);
	reply = post_as(fx, fx->admin, links_path, manage_path, plan);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&plan, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND kept"
		" AND hand_how = 'linked'", b), ==, 1);
	/* A file unlinked by hand: the review says what the next run does, and the
	 * confirmation lets go of it. */
	plan = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, a);
	reply = post_as(fx, fx->admin, links_path, manage_path, plan);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "is no longer left alone");
	{
		g_autofree gchar *digest = digest_of(reply.body);
		g_autofree gchar *form = g_strdup_printf("step=apply&file=%" G_GINT64_FORMAT "&digest=%s", a, digest);

		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, links_path, manage_path, form);
	}
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=managed");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, a), ==, 0);
	assert_links(fx, series, "S01E04.mkv>S01E03,S01E01-E02.mkv>S01E01,S01E01-E02.mkv>S01E02,S01E05 third.mkv>S01E05,"
		"S01E04 arrived.mkv>S01E04");
	/* A file with neither mark: nothing to let go. */
	reply = post_as(fx, fx->admin, links_path, manage_path, plan);
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);

	/* Nothing on disk, and nothing of the index, was touched by any of it. */
	paths_after = q(fx, "SELECT string_agg(id || ':' || rel_path || ':' || state, ',' ORDER BY id) FROM library_file"
		" WHERE rel_path NOT LIKE '%%alt.mkv' AND rel_path NOT LIKE '%%third.mkv' AND rel_path NOT LIKE '%%arrived.mkv'");
	g_assert_cmpstr(paths_after, ==, paths);
}

/*
 * The same case, made up: after the switch to TheTVDB's order the file
 * named S01E01 sits on the year-named season's first episode (S2023E01), and
 * S01E01 holds a second copy downloaded later.  The administrator unlinks the
 * stray file: S2023E01 has no file then and, not monitored, is not wanted;
 * nothing on disk changes; and the file is listed as unlinked by hand.  (That
 * identification's next run leaves it unlinked is /identify/tv/hand-link-kept.)
 * Spec (tv): A link on an episode the numbering lacks.
 */
static void
test_links_stray(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *rel = NULL;
	g_autofree gchar *after = NULL;
	AxisTestReply review;
	AxisTestReply reply;
	gint64 series = tidewater_switched(fx);
	gint64 lamp = episode_id(fx, series, 2023, 1);
	gint64 first = episode_id(fx, series, 1, 1);
	gint64 stray = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp);
	gint64 low;
	gint64 gone_file;
	gint64 jobs;

	links_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	rel = q(fx, "SELECT rel_path FROM library_file WHERE id = %" G_GINT64_FORMAT, stray);
	assert_contains(rel, "S01E01.mkv");
	in_library(fx, series, first, "Tidewater Lane (2023)/Season 01/Tidewater Lane (2023) - S01E01 WEBDL.mkv",
		"webdl_1080p");
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET monitored = false WHERE season_number = 2023 AND number = 1");

	jobs = qi(fx, "SELECT count(*) FROM job");
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", stray);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "S2023E01");
	assert_contains(review.body, "Left without a file: not wanted (not monitored, or not aired), so not searched for.");
	assert_contains(review.body, "No episode: unlinked, and left alone by identification");
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, lamp), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, stray), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, stray), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, first), ==, 1);
	after = q(fx, "SELECT rel_path || ':' || state FROM library_file WHERE id = %" G_GINT64_FORMAT, stray);
	{
		g_autofree gchar *expected = g_strdup_printf("%s:present", rel);

		g_assert_cmpstr(after, ==, expected);
	}
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'hand_link' AND episode_id = %" G_GINT64_FORMAT
		" AND text LIKE '%%it has no file now and is not wanted%%'", lamp), ==, 1);
	/* Not wanted: nothing was queued for it. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job"), ==, jobs);
	reply = get_as(fx, fx->admin, series_path);
	g_assert_cmpuint(reply.status, ==, 200);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "Unlinked by hand by root on ");
	assert_contains(reply.body, "Tidewater Lane (2023) - S01E01.mkv");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&plan, g_free);

	/* A file on an episode kept as gone: listed as such, and moved to a live
	 * episode it is no longer. */
	low = episode_id(fx, series, 1, 5);
	gone_file = qi(fx, "SELECT file_id FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, low);
	g_assert_cmpint(qi(fx, "WITH g AS (UPDATE series_episode SET gone_at = now() WHERE id = %" G_GINT64_FORMAT
		" RETURNING 1) SELECT count(*) FROM g", low), ==, 1);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "On an episode the numbering this series follows does not have");
	axis_test_reply_clear(&reply);
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S2023E02", gone_file);
	links_change(fx, series, plan);
	g_assert_cmpint(qi(fx, "SELECT episode_id FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT, gone_file), ==,
		episode_id(fx, series, 2023, 2));
	reply = get_as(fx, fx->admin, links_path);
	assert_lacks(reply.body, "On an episode the numbering this series follows does not have");
	axis_test_reply_clear(&reply);
}

/*
 * The ticks "unlink the other" are part of the review and of its digest: a
 * confirmation that does not post the ticks the review showed changes
 * nothing; the review lists every file a tick touches, with its before and
 * after; a ticked file that keeps another episode is not stamped as linked by
 * hand and keeps how it was linked (only the episode goes); and the ticks
 * stay on the review shown again after a change (409).
 */
static void
test_links_ticks(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *ticks = NULL;
	g_autofree gchar *both = NULL;
	g_autofree gchar *pattern = NULL;
	AxisTestReply review;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 a;
	gint64 x;
	gint64 l;

	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E03.mkv");
	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E03-E04.mkv");
	a = qi(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%S01E03.mkv'");
	x = qi(fx, "SELECT id FROM library_file WHERE rel_path LIKE '%%S01E03-E04.mkv'");
	g_assert_cmpint(qi(fx, "INSERT INTO series_file_episode (file_id, episode_id) VALUES (%" G_GINT64_FORMAT ", %"
		G_GINT64_FORMAT ") RETURNING file_id", x, episode_id(fx, series, 1, 4)), ==, x);
	l = loose_file(fx, "Tidewater Lane (2023) - S01E03 extra.mkv");
	links_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E03", l);
	ticks = g_strdup_printf("other_%" G_GINT64_FORMAT "=yes&other_%" G_GINT64_FORMAT "=yes", a, x);
	both = g_strdup_printf("%s&%s", plan, ticks);

	/* Ticked: both files are listed with their before and after. */
	review = links_review(fx, series, both);
	g_assert_cmpuint(review.status, ==, 200);
	assert_contains(review.body, "Tidewater Lane (2023) - S01E03-E04.mkv");
	assert_contains(review.body, "You ticked this file: it leaves the episodes named in the tick, and only those. It is then on its other episodes, kept by hand");
	assert_contains(review.body, "You ticked this file: it has no episode left, so it is unlinked and left alone by "
		"identification.");
	pattern = g_strdup_printf("name=\"other_%" G_GINT64_FORMAT "\"", a);
	g_assert_cmpuint(count_of(review.body, pattern), ==, 2);
	/* The ticks are in the digest: confirming without them changes nothing. */
	reply = links_apply(fx, series, plan, "", review.body);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id IN (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ")",
		a, x), ==, 2);
	axis_test_reply_clear(&review);

	/* A file arrives after the review; the ticks are still there. */
	review = links_review(fx, series, both);
	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E03 arrived.mkv");
	reply = links_apply(fx, series, plan, ticks, review.body);
	g_assert_cmpuint(reply.status, ==, 409);
	{
		g_autoptr(GRegex) checked = g_regex_new("<input[^>]*checked[^>]*>", 0, 0, NULL);
		g_autoptr(GMatchInfo) found = NULL;
		guint n = 0;

		g_regex_match(checked, reply.body, 0, &found);
		while (g_match_info_matches(found)) {
			g_autofree gchar *tag = g_match_info_fetch(found, 0);

			g_assert_nonnull(strstr(tag, "other_"));
			n++;
			g_match_info_next(found, NULL);
		}
		g_assert_cmpuint(n, ==, 2);
	}
	axis_test_reply_clear(&review);
	review = reply;
	reply = links_apply(fx, series, plan, ticks, review.body);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	axis_test_reply_clear(&review);

	/* The ticked file that keeps S01E04 is not stamped, nor its identity
	 * dropped; the one with no episode left is unlinked and remembered. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_how = 'linked'"
		" AND hand_by = 'root' AND hand_at IS NOT NULL AND kept AND identity_id IS NULL", x), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT episode_id FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT, x), ==,
		episode_id(fx, series, 1, 4));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, a), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, a), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_how = 'linked'",
		l), ==, 1);
}

/*
 * What the links page reaches: a file of another series' folder is neither
 * listed (?file=) nor accepted in a plan (to_<id>), the files with no link
 * are listed a page at a time, and a mark of an unlink made for another
 * series (a removed one) can be cleared here.
 */
static void
test_links_scope(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *manage_path = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	AxisTestReply review;
	gint64 series = tidewater_plain(fx);
	gint64 a;
	gint64 far;

	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E03.mkv");
	tidewater_file(fx, series, 4, "Tidewater Lane (2023) - S01E04.mkv");
	a = file_on_episode(fx, series, 1, 3);
	far = qi(fx, "INSERT INTO library_file (root_id, volume_id, rel_path, kind, size, mtime_sec, mtime_nsec,"
		" ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads, first_seen_generation,"
		" last_seen_generation, changed_generation) VALUES (%" G_GINT64_FORMAT ", %" G_GINT64_FORMAT ","
		" 'Faraway Show (2019)/Season 01/Faraway Show - S01E01.mkv', 'video', 1600000000, 1, 0, 1, 0, 990000001, 1,"
		" false, false, false, 1, 1, 1) RETURNING id", fx->root_id, fx->volume_id);
	links_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	manage_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links/manage", series);

	/* Another series' folder: not listed, not accepted. */
	path = g_strdup_printf("%s?file=%" G_GINT64_FORMAT, links_path, far);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Faraway Show");
	axis_test_reply_clear(&reply);
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E01", far);
	review = links_review(fx, series, plan);
	g_assert_cmpuint(review.status, ==, 400);
	axis_test_reply_clear(&review);
	g_clear_pointer(&plan, g_free);
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E01", far);
	reply = links_apply(fx, series, plan, "", "name=\"digest\" value=\"00000000000000000000000000000000\"");
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT, far), ==, 0);
	g_clear_pointer(&plan, g_free);

	/* The files with no link, a page at a time. */
	{
		g_autofree gchar *bulk = g_strdup_printf("INSERT INTO library_file (root_id, volume_id, rel_path, kind, size,"
			" mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, inode, nlink, pinned, pinned_links, pinned_downloads,"
			" first_seen_generation, last_seen_generation, changed_generation) SELECT %" G_GINT64_FORMAT ", %"
			G_GINT64_FORMAT ", 'Tidewater Lane (2023)/Season 01/Tidewater Lane (2023) - Bulk ' || lpad(n::text, 3, '0')"
			" || '.mkv', 'video', 1600000000, 1, 0, 1, 0, 970000000 + n, 1, false, false, false, 1, 1, 1"
			" FROM generate_series(1, 105) AS n", fx->root_id, fx->volume_id);

		axis_test_db_exec(fx->t->db, bulk);
	}
	reply = get_as(fx, fx->admin, links_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Bulk 001.mkv");
	assert_contains(reply.body, "Bulk 100.mkv");
	assert_lacks(reply.body, "Bulk 101.mkv");
	assert_contains(reply.body, "Files 1 to 100 of 105.");
	assert_contains(reply.body, "Later files");
	axis_test_reply_clear(&reply);
	g_free(path);
	path = g_strdup_printf("%s?page=1", links_path);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Bulk 105.mkv");
	assert_lacks(reply.body, "Bulk 100.mkv");
	assert_contains(reply.body, "Earlier files");
	axis_test_reply_clear(&reply);

	/* A mark made for another series: it is listed here and can be cleared. */
	g_free(plan);
	plan = g_strdup_printf("unlink_%" G_GINT64_FORMAT "=yes", a);
	links_change(fx, series, plan);
	axis_test_db_exec(fx->t->db, "UPDATE library_hand_unlink SET owner_id = owner_id + 1000");
	g_free(plan);
	plan = g_strdup_printf("step=review&file=%" G_GINT64_FORMAT, a);
	reply = post_as(fx, fx->admin, links_path, manage_path, plan);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "is no longer left alone");
	{
		g_autofree gchar *digest = digest_of(reply.body);
		g_autofree gchar *form = g_strdup_printf("step=apply&file=%" G_GINT64_FORMAT "&digest=%s", a, digest);

		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, links_path, manage_path, form);
	}
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_hand_unlink WHERE file_id = %" G_GINT64_FORMAT, a), ==, 0);
}

/*
 * Spec (tv): Its name says another episode (not listed, not moved by Place
 * all); A change of numbering (listed in a group of its own with the episode
 * before and after; on that episode after it, there and back).
 */
static void
test_hand_link_kept(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *change = NULL;
	g_autofree gchar *plan = NULL;
	g_autofree gchar *digest = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 fog;
	gint64 file;

	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	/* On S01E04, named for S01E05, which has no file: listed, placeable. */
	tidewater_file(fx, series, 4, "Tidewater Lane (2023) - S01E05.mkv");
	file = file_on_episode(fx, series, 1, 4);
	fog = episode_id(fx, series, 1, 3);
	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	reply = get_as(fx, fx->admin, series_path);
	assert_contains(reply.body, "Files whose names say another episode");
	axis_test_reply_clear(&reply);

	/* Linked by hand to S01E03 ("Fog Week"): its name still says S01E05. */
	plan = g_strdup_printf("to_%" G_GINT64_FORMAT "=S01E03", file);
	links_change(fx, series, plan);
	g_assert_cmpint(qi(fx, "SELECT episode_id FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT, file), ==, fog);
	reply = get_as(fx, fx->admin, series_path);
	assert_lacks(reply.body, "Files whose names say another episode");
	assert_lacks(reply.body, "Named by Axis under the earlier numbering");
	assert_lacks(reply.body, "/place?file=");
	axis_test_reply_clear(&reply);
	/* Place all, posted anyway, and its own page: it is not moved. */
	form = g_strdup_printf("file=all&digest=%s", "00000000000000000000000000000000");
	reply = post_as(fx, fx->admin, series_path, place_action, form);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("file=%" G_GINT64_FORMAT "&digest=%s", file, "00000000000000000000000000000000");
	reply = post_as(fx, fx->admin, series_path, place_action, form);
	g_assert_cmpuint(reply.status, !=, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	g_assert_cmpint(qi(fx, "SELECT episode_id FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT, file), ==, fog);

	/* A change of numbering: listed in a group of its own, with the episode
	 * before and after; never among the files the box places; after the
	 * change it is on that episode.  And back. */
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	back = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=fixture", series);
	change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering", series);
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Files linked by hand");
	assert_contains(reply.body, "Linked by hand by root");
	assert_matches(reply.body, "kind-numbering-hand[\\s\\S]*>S01E03</td>[\\s\\S]*>S01E03</td>");
	assert_lacks(reply.body, "Files whose names and new episodes disagree");
	assert_lacks(reply.body, "After the change, place these files by their names");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=tvdb&digest=%s&names=yes", digest);
	reply = post_as(fx, fx->admin, page, change, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	g_clear_pointer(&digest, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND episode_source = 'tvdb'",
		series), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT episode_id FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT, file), ==, fog);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_how = 'linked'"
		" AND kept", file), ==, 1);
	reply = get_as(fx, fx->admin, series_path);
	assert_lacks(reply.body, "/place?file=");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, back);
	assert_contains(reply.body, "Files linked by hand");
	digest = digest_of(reply.body);
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("to=fixture&digest=%s&names=yes", digest);
	reply = post_as(fx, fx->admin, back, change, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT episode_id FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT, file), ==, fog);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE id = %" G_GINT64_FORMAT " AND season_number = 1"
		" AND number = 3 AND gone_at IS NULL AND source = 'fixture'", fog), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND hand_how = 'linked'"
		" AND kept", file), ==, 1);
}

/*
 * Spec (tv): Two files named for each other's place in a chain.  A is on
 * S01E02 and named for S01E03; B is on S01E03 and named for S01E04, which has
 * no file.  A's row says what placing A alone does (S01E03 has a file of its
 * own), B's row offers placing it, and Place all's confirmation says both
 * are placed.
 */
static void
test_place_row_status(Fixture *fx, gconstpointer data)
{
	g_autofree gchar *series_path = NULL;
	g_autofree gchar *all_path = NULL;
	g_autofree gchar *a_link = NULL;
	g_autofree gchar *b_link = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 a;
	gint64 b;

	tidewater_file(fx, series, 2, "Tidewater Lane (2023) - S01E03.mkv");
	tidewater_file(fx, series, 3, "Tidewater Lane (2023) - S01E04.mkv");
	a = file_on_episode(fx, series, 1, 2);
	b = file_on_episode(fx, series, 1, 3);
	series_path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	all_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=all", series);
	a_link = g_strdup_printf("/place?file=%" G_GINT64_FORMAT "\"", a);
	b_link = g_strdup_printf("/place?file=%" G_GINT64_FORMAT "\"", b);
	reply = get_as(fx, fx->admin, series_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "S01E03 has a file of its own");
	assert_lacks(reply.body, a_link);
	assert_contains(reply.body, b_link);
	assert_contains(reply.body, "Place all by their names");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, all_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "has a file of its own");
	assert_contains(reply.body, "S01E03 holds the file");
	assert_contains(reply.body, "S01E04 holds the file");
	axis_test_reply_clear(&reply);
}

/*
 * Spec (tv): Placing while a pack is partly imported.  An import that ended
 * partly imported is open: nothing is placed, and the page names the import
 * and says to retry or dismiss it; a change of numbering waits for it too.
 */
static void
test_import_open_partial(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *place_path = NULL;
	g_autofree gchar *place_action = NULL;
	g_autofree gchar *numbering = NULL;
	g_autofree gchar *links_path = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply reply;
	gint64 series = tidewater_plain(fx);
	gint64 file;

	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	tidewater_file(fx, series, 1, "Tidewater Lane (2023) - S01E03.mkv");
	file = file_on_episode(fx, series, 1, 1);
	axis_test_db_exec(fx->t->db, "INSERT INTO import (module, title_id, title_label, source_path, state, finished_at)"
		" SELECT 'tv', title_id, 'Tidewater Lane (2023)', 'downloads/pack', 'partial', now() FROM series");
	place_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place?file=%" G_GINT64_FORMAT, series, file);
	place_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/place", series);
	numbering = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);
	links_path = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	reply = get_as(fx, fx->admin, place_path);
	assert_contains(reply.body, "is partly imported: retry it or dismiss it there");
	assert_contains(reply.body, "/imports/");
	assert_lacks(reply.body, "name=\"digest\"");
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("file=%" G_GINT64_FORMAT "&digest=%s", file, "00000000000000000000000000000000");
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 409);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT,
		episode_id(fx, series, 1, 3)), ==, 0);
	/* The numbering switch and the links page wait for it as well. */
	reply = get_as(fx, fx->admin, numbering);
	assert_contains(reply.body, "The change waits for");
	assert_contains(reply.body, "an import that is partly imported: retry it or dismiss it");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, links_path);
	assert_contains(reply.body, "No link can be changed now.");
	assert_contains(reply.body, "partly imported");
	axis_test_reply_clear(&reply);
	/* Dismissed: placed. */
	axis_test_db_exec(fx->t->db, "UPDATE import SET state = 'dismissed'");
	reply = get_as(fx, fx->admin, place_path);
	g_free(form);
	{
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, file);

		form = place_form(id, reply.body);
	}
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, place_path, place_action, form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
}

static void
assert_matches(
	const gchar *haystack,
	const gchar *pattern
){
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);

	if (haystack == NULL || !g_regex_match(regex, haystack, 0, NULL))
		g_error("expected to match '%s' in:\n%s", pattern, haystack != NULL ? haystack : "(null)");
}

/*
 * On the title page: the numbering shown is a visible choice of
 * two (the site's strip of views), the one shown marked; looking at the one
 * the series does not follow says it is a preview, with the way to change it
 * for an administrator of the managed series; a viewer is offered no choice.
 */
static void
test_numbering_title_choice(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *title_path = NULL;
	g_autofree gchar *tvdb_path = NULL;
	g_autofree gchar *change = NULL;
	AxisTestReply reply;
	gint64 series;

	set_gullwing(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7061", AXIS_TV_MONITOR_ALL);
	title_path = g_strdup_printf("/title/%" G_GINT64_FORMAT, qi(fx, "SELECT title_id FROM series WHERE id = %"
		G_GINT64_FORMAT, series));
	tvdb_path = g_strdup_printf("%s?numbering=tvdb", title_path);
	change = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);

	/* The numbering it follows: marked, said in a muted line, no preview. */
	reply = get_as(fx, fx->admin, title_path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "aria-label=\"Numbering\"");
	assert_matches(reply.body, "aria-current=\"true\"[^>]*numbering=fixture#seasons\">Fixture</a>");
	assert_matches(reply.body, "<a href=\"[^\"]*numbering=tvdb#seasons\">TheTVDB</a>");
	assert_contains(reply.body, "This series follows this numbering.");
	assert_lacks(reply.body, "A preview");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);

	/* The other: marked, a preview, the way to change it. */
	reply = get_as(fx, fx->admin, tvdb_path);
	assert_matches(reply.body, "aria-current=\"true\"[^>]*numbering=tvdb#seasons\">TheTVDB</a>");
	assert_matches(reply.body, "<a href=\"[^\"]*numbering=fixture#seasons\">Fixture</a>");
	assert_contains(reply.body, "A preview: this series follows Fixture's numbering. Nothing is changed by looking.");
	assert_contains(reply.body, change);
	assert_contains(reply.body, "Change the episode numbering");
	assert_lacks(reply.body, "This series follows this numbering.");
	save_page("title-numbering-preview.html", reply.body);
	axis_test_reply_clear(&reply);

	/* A viewer: no choice, no preview, no way to change it. */
	reply = get_as(fx, fx->viewer, tvdb_path);
	assert_lacks(reply.body, "aria-label=\"Numbering\"");
	assert_lacks(reply.body, "A preview");
	assert_lacks(reply.body, change);
	assert_contains(reply.body, "Numbered as Fixture numbers them.");
	axis_test_reply_clear(&reply);
}

/* Re-review, item 2: a season opened by itself (htmx) of a series following
 * TheTVDB is TheTVDB's season, for a viewer too. */
static void
test_numbering_viewer_season(Fixture *fx, gconstpointer data)
{
	static const gchar *const htmx[] = { "HX-Request", "true", NULL };
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autofree gchar *path = NULL;
	AxisTestReply reply;
	gboolean changed = TRUE;
	gint64 series;

	set_gullwing(fx);
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	series = add_series(fx, "7061", AXIS_TV_MONITOR_ALL);
	g_assert_true(axis_tv_plan_numbering(fx->tv, series, "tvdb", TRUE, &plan, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed,
		&error));
	g_assert_no_error(error);
	path = g_strdup_printf("/title/%" G_GINT64_FORMAT "/season/2", qi(fx, "SELECT title_id FROM series WHERE id = %"
		G_GINT64_FORMAT, series));
	reply = axis_test_web_request(fx->session, fx->port, "GET", path, fx->viewer, (const gchar **)htmx, NULL);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Night Crossing");
	assert_lacks(reply.body, "Low Water");
	axis_test_reply_clear(&reply);
}

/*
 * A series brought over and held (import-from-arr, slice S2), as its rows
 * are after a run: the series held, two of its episodes held apart because
 * Sonarr has files for them that Axis does not see.  Its page says so, and
 * which buttons start searching for the whole series; "Let Axis search for
 * it" is a page that asks first and writes nothing by being opened, whose
 * box (not ticked) is the only way to the episodes held apart; and a new
 * profile that finds nothing to search for says why.
 */
static void
test_brought_hold(Fixture *fx, gconstpointer data)
{
	gint64 series = tidewater_plain(fx);
	g_autofree gchar *page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	g_autofree gchar *start = g_strdup_printf("/series/%" G_GINT64_FORMAT "/start-searching", series);
	g_autofree gchar *post_form = g_strdup_printf("action=\"%s\"", start);
	g_autofree gchar *edit = g_strdup_printf("/series/%" G_GINT64_FORMAT "/edit", series);
	g_autofree gchar *form = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *root = q(fx, "SELECT root_id FROM series WHERE id = %" G_GINT64_FORMAT, series);
	AxisTestReply reply;

	axis_test_db_exec(fx->t->db, "UPDATE series SET search_hold = 'brought';"
		" UPDATE series_episode SET search_hold = 'file_unseen' WHERE number IN (1, 2)");

	/* The series' page: held, and what each button does, before any press. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Migrated from Sonarr: Axis does not search for it until you start searching.");
	assert_contains(reply.body, "\"Search for every wanted episode\" below starts searching for the whole series");
	assert_contains(reply.body, "A search of one episode or one season, and a manual search, are that search only: the "
		"series stays held.");
	assert_contains(reply.body, "2 episodes are held apart: Sonarr has a file for each that Axis does not see");
	assert_contains(reply.body, "Starting to search for the series leaves them held.");
	assert_contains(reply.body, "Let Axis search for it");
	assert_contains(reply.body, start);
	/* No button on the page ends the holds with one press. */
	assert_lacks(reply.body, post_form);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	/* A viewer reads the same, without the way on. */
	reply = get_as(fx, fx->viewer, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Migrated from Sonarr: Axis does not search for it until you start searching.");
	assert_lacks(reply.body, start);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, start);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);

	/* The confirmation: how many, the box not ticked, and nothing written. */
	reply = get_as(fx, fx->admin, start);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "This series was migrated from Sonarr and is held");
	assert_contains(reply.body, "Sonarr has a file for 2 episodes that Axis does not see");
	assert_contains(reply.body, "They stay held, so that no second copy is downloaded, unless you tick the box.");
	assert_contains(reply.body, "Search for those too; a second copy may be downloaded");
	assert_contains(reply.body, "name=\"unseen\"");
	assert_lacks(reply.body, "checked");
	assert_contains(reply.body, "Opening this page changed nothing.");
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT (SELECT count(*) FROM series WHERE search_hold = 'brought') * 10 +"
		" (SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen')"), ==, 12);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE text LIKE 'Searching for it was started%%'"), ==, 0);

	/* Confirmed without the tick: the series' hold ends, the two stay held,
	 * and nothing is queued at once. */
	reply = post_as(fx, fx->admin, start, start, NULL);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=started");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT (SELECT count(*) FROM series WHERE search_hold IS NOT NULL) * 10 +"
		" (SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen')"), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
	text = q(fx, "SELECT text FROM series_event WHERE text LIKE 'Searching for it was started%%' ORDER BY id DESC LIMIT 1");
	assert_contains(text, "2 episodes Sonarr has a file for that Axis does not see stay held");
	reply = get_as(fx, fx->admin, page);
	assert_lacks(reply.body, "Migrated from Sonarr: Axis does not search for it until you start searching.");
	assert_contains(reply.body, "2 episodes are held apart");
	axis_test_reply_clear(&reply);

	/* A new quality profile, with nothing else wanted (the other episodes are
	 * not monitored): nothing is searched for, and the notice and the history
	 * say it is because of the two held apart, not that nothing is wanted. */
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET monitored = false WHERE number > 2");
	form = g_strdup_printf("profile=%" G_GINT64_FORMAT "&root=%s", qi(fx, "SELECT id FROM quality_profile WHERE id <>"
		" (SELECT quality_profile_id FROM series WHERE id = %" G_GINT64_FORMAT ") AND slug IN ('balanced', 'space_saver')"
		" ORDER BY id LIMIT 1", series), root);
	reply = post_as(fx, fx->admin, page, edit, form);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=profile_none");
	{
		AxisTestReply landed = follow(fx, axis_test_reply_header(&reply, "Location"));

		assert_contains(landed.body, "Nothing was searched for: 2 episodes are held apart because Sonarr has a file for "
			"each that Axis does not see");
		assert_lacks(landed.body, "Nothing of the series is wanted under it");
		axis_test_reply_clear(&landed);
	}
	axis_test_reply_clear(&reply);
	g_free(text);
	text = q(fx, "SELECT text FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'changed'"
		" ORDER BY id DESC LIMIT 1", series);
	assert_contains(text, "2 episodes are held apart because Sonarr has a file for each that Axis does not see");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);

	/* Search now for the whole series, which is not held any more, while all
	 * that is missing is held apart: nothing is queued, and the answer says
	 * why, not that nothing is wanted. */
	{
		g_autofree gchar *search_action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/search", series);
		AxisTestReply landed;

		reply = post_as(fx, fx->admin, page, search_action, "scope=series");
		g_assert_cmpuint(reply.status, ==, 303);
		assert_contains(axis_test_reply_header(&reply, "Location"), "done=nothing_wanted");
		landed = follow(fx, axis_test_reply_header(&reply, "Location"));
		assert_contains(landed.body, "Nothing was searched for: 2 episodes are held because Sonarr has files for them "
			"that Axis does not see");
		axis_test_reply_clear(&landed);
		axis_test_reply_clear(&reply);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE search_hold = 'file_unseen'"), ==, 2);
	}

	/* The confirmation again: only the episodes are held now.  With the tick
	 * they are let go, and the history says a second copy may come. */
	reply = get_as(fx, fx->admin, start);
	assert_contains(reply.body, "Axis searches for this series by itself already. Only the episodes below are held.");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, start, start, "unseen=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE search_hold IS NOT NULL"), ==, 0);
	g_free(text);
	text = q(fx, "SELECT text FROM series_event WHERE text LIKE 'Searching for it was started%%' ORDER BY id DESC LIMIT 1");
	assert_contains(text, "2 episodes Sonarr has a file for that Axis does not see are searched for too, as ticked: a "
		"second copy may be fetched.");
	/* Nothing is held: the page says so, and offers nothing to confirm. */
	reply = get_as(fx, fx->admin, start);
	assert_contains(reply.body, "Nothing of this series is held");
	assert_lacks(reply.body, "name=\"unseen\"");
	axis_test_reply_clear(&reply);
}

/* ── add-block-release ──────────────────────────────────────────── */

#define TIDE_E01_DISC "Tidewater.Inn.S01E01.1080p.BluRay.x264-GRP"

/* What the fake Prowlarr answers from now on: these names, from the indexer
 * with this number and name. */
static void
set_releases_from(
	Fixture            *fx,
	const gchar *const *names,
	guint               n,
	guint               indexer_id,
	const gchar        *indexer
){
	g_autofree gchar *origin = axis_test_prowlarr_base(fx->prowlarr);
	g_autoptr(GString) body = g_string_new("[");
	guint i;

	for (i = 0; i < n; i++)
		g_string_append_printf(body, "%s{\"guid\":\"https://indexer.example/b%u-%u\",\"title\":\"%s\","
			"\"size\":1500000000,\"indexerId\":%u,\"indexer\":\"%s\",\"publishDate\":\"2026-11-13T10:00:00Z\","
			"\"downloadUrl\":\"%s/3/download?apikey=%s&link=B%u&file=hearth-nzb\",\"magnetUrl\":null,"
			"\"protocol\":\"usenet\",\"categories\":[{\"id\":5040}]}", i > 0 ? "," : "", indexer_id, i, names[i],
			indexer_id, indexer, origin, KEY, i);
	g_string_append_c(body, ']');
	axis_test_prowlarr_set_search(fx->prowlarr, NULL, body->str);
	axis_test_db_exec(fx->t->db, "UPDATE indexer_search SET expires_at = now() - interval '1 second'");
}

/*
 * add-block-release.  Spec (tv): "An administrator can block a release",
 * scenarios "From a manual search", "A download that is not wanted", "A
 * release that is in the library already", "One release taken off", "A
 * viewer".
 *
 * Block this release on an episode's manual search blocks the release for
 * the series by the person, from every indexer that lists that name; a
 * scheduled search of the episode takes the other release.  Block and search
 * again on its download gives up on it through the failure path.  For an
 * episode's file whose release is known it only blocks: the file, its link,
 * the grab and the jobs stay as they were.  A viewer, and a post without its
 * token, are refused.
 */
static void
test_block_release(Fixture *fx, gconstpointer data)
{
	static const gchar *const first[] = { TIDE_E01, TIDE_E01_DISC };
	static const Rel second[] = {
		{ TIDE_E02, 1400000000 }
	};
	g_autofree gchar *page = NULL;
	g_autofree gchar *block = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *best = NULL;
	g_autofree gchar *row = NULL;
	g_autofree gchar *landed = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *n = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *links = NULL;
	g_autofree gchar *releases = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	const gchar *other;
	gint64 series;
	gint64 e1;
	gint64 e2;
	gint64 candidate;
	gint64 grab;
	gint64 download;
	gint64 import;
	gint64 library_file;
	gint64 jobs;

	/* Tidewater Inn: two episodes aired. */
	set_route(fx, "/title/series/207", "{\"id\":\"207\",\"kind\":\"series\",\"title\":\"Tidewater Inn\",\"year\":2026,"
		"\"status\":\"continuing\",\"runtime\":45,\"overview\":\"Rooms by the tide.\",\"ids\":{\"tvdb\":\"370007\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-06\",\"episodes\":2}]}");
	set_route(fx, "/title/series/207/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9701\",\"name\":\"Low Water\",\"air_date\":\"2026-11-06\",\"runtime\":45},"
		"{\"number\":2,\"id\":\"9702\",\"name\":\"Spring Tide\",\"air_date\":\"2026-11-13\",\"runtime\":45}]}");
	series = add_series(fx, "207", AXIS_TV_MONITOR_ALL);
	e1 = episode_id(fx, series, 1, 1);
	e2 = episode_id(fx, series, 1, 2);
	set_releases_from(fx, first, G_N_ELEMENTS(first), 3, "Harbourmaster");
	page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=episode&item=%" G_GINT64_FORMAT, series, e1);
	block = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/block", series);
	releases = g_strdup_printf("/series/%" G_GINT64_FORMAT "/releases?episode=%" G_GINT64_FORMAT, series, e1);
	links = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, e1, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	/* Both are acceptable; the one a search would take is the one blocked. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_candidate c JOIN series_search s ON s.id = c.search_id"
		" WHERE s.cause = 'list' AND c.accepted"), ==, 2);
	best = q(fx, "SELECT c.name FROM series_candidate c JOIN series_search s ON s.id = c.search_id"
		" WHERE s.cause = 'list' AND c.accepted ORDER BY c.rank LIMIT 1");
	g_assert_nonnull(best);
	other = strcmp(best, TIDE_E01) == 0 ? TIDE_E01_DISC : TIDE_E01;
	candidate = listed_candidate(fx, best);
	row = g_strdup_printf("manual-release-%" G_GINT64_FORMAT, candidate);

	/* 1. Block this release: an administrator's, with its token. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(count_of(reply.body, ">Block this release</button>"), ==, 2);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, page);
	assert_lacks(reply.body, "Block this release");
	axis_test_reply_clear(&reply);
	form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=episode&item=%" G_GINT64_FORMAT, candidate, e1);
	reply = post_as(fx, fx->viewer, page, block, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, block, form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_blocklist"), ==, 0);
	reply = post_as(fx, fx->admin, page, block, form);
	g_assert_cmpuint(reply.status, ==, 303);
	landed = g_strdup_printf("done=blocked&at=%s#%s", row, row);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT reason || '|' || created_by || '|' || (created_at > now() - interval '1 minute')::text || '|' ||"
		" release_name FROM series_blocklist WHERE series_id = %" G_GINT64_FORMAT, series);
	g_free(landed);
	landed = g_strdup_printf("blocked|root|true|%s", best);
	g_assert_cmpstr(n, ==, landed);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'blocklisted' AND by_user = 'root' AND text LIKE '%%blocked by root from a manual search%%'", series), ==, 1);
	/* The row says who, from then on. */
	g_free(landed);
	landed = g_strdup_printf("%s&done=blocked&at=%s", page, row);
	reply = get_as(fx, fx->admin, landed);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpuint(count_of(reply.body, "On the blocklist (blocked by root): take it off the blocklist to grab it."),
		==, 1);
	assert_contains(reply.body, "Blocked: this release is on the blocklist now");
	assert_contains(reply.body, "/releases#blocklist\"");
	assert_contains(reply.body, "Ruled out (1)");
	g_assert_cmpuint(count_of(reply.body, ">Block this release</button>"), ==, 1);
	axis_test_reply_clear(&reply);
	/* Its origin on the series' blocklist, with Unblock. */
	reply = get_as(fx, fx->admin, releases);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Blocked by root");
	assert_contains(reply.body, ">Unblock</button>");
	axis_test_reply_clear(&reply);

	/* 2. A scheduled search of the episode takes the other release, though
	 * another indexer lists the blocked name now: a person's block is of the
	 * release, not of one indexer's copy. */
	set_releases_from(fx, first, G_N_ELEMENTS(first), 4, "Lamplight");
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, series, AXIS_TV_SCOPE_EPISODE, e1, "scheduled", NULL, NULL, &report,
		&error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "grabbed");
	g_free(n);
	n = q(fx, "SELECT release_name FROM series_grab WHERE series_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", series);
	g_assert_cmpstr(n, ==, other);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_candidate c JOIN series_search s ON s.id = c.search_id"
		" WHERE s.cause = 'scheduled' AND c.name = '%s' AND c.indexer = 'Lamplight' AND c.indexer_id = 4"
		" AND c.blocklisted AND c.reasons::text LIKE '%%blocked by root%%'", best), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_blocklist b JOIN series_candidate c ON c.name_key = b.name_key"
		" JOIN series_search s ON s.id = c.search_id WHERE s.cause = 'scheduled'"), ==, 0);

	/* 3. Block and search again, on the download of the one it took. */
	grab = qi(fx, "SELECT id FROM series_grab WHERE series_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", series);
	download = qi(fx, "SELECT download_id FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_assert_cmpint(download, >, 0);
	confirm = g_strdup_printf("/series/block-release?download=%" G_GINT64_FORMAT, download);
	reply = get_as(fx, fx->admin, "/downloads");
	assert_contains(reply.body, "/series/block-release?download=");
	assert_contains(reply.body, ">Block and search again</a>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, "/downloads");
	assert_lacks(reply.body, "block-release");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, confirm);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Block this release and search again?");
	assert_contains(reply.body, other);
	assert_contains(reply.body, "Tidewater Inn");
	assert_contains(reply.body, "gives up on the download");
	axis_test_reply_clear(&reply);
	/* It wrote nothing. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_blocklist"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT, series, grab);
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 400);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes", series, grab);
	reply = post_as(fx, fx->viewer, page, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 1);
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_free(landed);
	landed = g_strdup_printf("/series/block-release?grab=%" G_GINT64_FORMAT "&done=cancelled", grab);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	/* The failure path's work. */
	g_free(n);
	n = q(fx, "SELECT close_reason || '|' || coalesce(removal, '-') FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_assert_cmpstr(n, ==, "given_up|done");
	g_free(n);
	n = q(fx, "SELECT reason || '|' || created_by FROM series_blocklist WHERE release_name = '%s'", other);
	g_assert_cmpstr(n, ==, "blocked|root");
	g_free(n);
	n = q(fx, "SELECT state FROM download WHERE id = %" G_GINT64_FORMAT, download);
	g_assert_cmpstr(n, ==, "removed");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND payload->>'cause' = 'retry'"
		" AND state = 'queued'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab_episode WHERE grab_id = %" G_GINT64_FORMAT " AND open",
		grab), ==, 0);
	reply = get_as(fx, fx->admin, landed);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis gave up on its download");
	axis_test_reply_clear(&reply);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");

	/* 4. Unblock: one entry, the other stays. */
	{
		g_autofree gchar *clear = g_strdup_printf("/series/%" G_GINT64_FORMAT "/blocklist/clear", series);
		g_autofree gchar *entry = g_strdup_printf("entry=%" G_GINT64_FORMAT, qi(fx, "SELECT id FROM series_blocklist"
			" WHERE release_name = '%s'", best));

		reply = post_as(fx, fx->viewer, releases, clear, entry);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, releases, clear, entry);
		g_assert_cmpuint(reply.status, ==, 303);
		axis_test_reply_clear(&reply);
	}
	g_free(n);
	n = q(fx, "SELECT string_agg(release_name, ',') FROM series_blocklist WHERE series_id = %" G_GINT64_FORMAT, series);
	g_assert_cmpstr(n, ==, other);

	/* 5. An episode's file whose release is known: blocked for the future,
	 * and nothing else. */
	set_releases(fx, second, G_N_ELEMENTS(second));
	search(fx, series, AXIS_TV_SCOPE_EPISODE, e2);
	grab = qi(fx, "SELECT id FROM series_grab WHERE series_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", series);
	g_assert_cmpint(grab, >, 0);
	download = qi(fx, "SELECT download_id FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	g_assert_cmpint(download, >, 0);
	dir = g_build_filename(fx->disk, "series", "Tidewater Inn (2026)", "Season 01", NULL);
	file = g_build_filename(dir, "S01E02.mkv", NULL);
	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	axis_test_storage_write(file, "episode");
	import = qi(fx, "INSERT INTO import (module, owner_id, title_label, source_path, state, download_id, finished_at)"
		" VALUES ('tv', %" G_GINT64_FORMAT ", 'Tidewater Inn (2026)', 'downloads/tidewater', 'imported', %"
		G_GINT64_FORMAT ", now()) RETURNING id", series, download);
	g_assert_cmpint(import, >, 0);
	in_library(fx, series, e2, "Tidewater Inn (2026)/Season 01/S01E02.mkv", "webdl_1080p");
	library_file = qi(fx, "SELECT min(file_id) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e2);
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE series_file SET import_id = %" G_GINT64_FORMAT " WHERE file_id = %"
			G_GINT64_FORMAT, import, library_file);
		g_autofree gchar *close = g_strdup_printf("UPDATE series_grab SET completed_at = now(), closed_at = now(),"
			" close_reason = 'imported' WHERE id = %" G_GINT64_FORMAT, grab);

		axis_test_db_exec(fx->t->db, sql);
		axis_test_db_exec(fx->t->db, close);
	}
	jobs = qi(fx, "SELECT count(*) FROM job");
	/* Offered on the file's row of the series' Files and links page. */
	g_free(confirm);
	confirm = g_strdup_printf("/series/block-release?import=%" G_GINT64_FORMAT, import);
	reply = get_as(fx, fx->admin, links);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "/series/block-release?import=");
	assert_contains(reply.body, ">Block this release</a>");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "This release is in the library already.");
	assert_contains(reply.body, "only puts the release on the blocklist");
	assert_contains(reply.body, ">Block this release</button>");
	assert_lacks(reply.body, "Block and search again");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_blocklist"), ==, 1);
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes", series, grab);
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_free(landed);
	landed = g_strdup_printf("/series/block-release?grab=%" G_GINT64_FORMAT "&done=blocked", grab);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	g_free(n);
	n = q(fx, "SELECT reason || '|' || created_by FROM series_blocklist WHERE release_name = '" TIDE_E02 "'");
	g_assert_cmpstr(n, ==, "blocked|root");
	/* The file on disk, its row, its link to the episode, the import, the
	 * grab and the jobs are as they were. */
	g_assert_true(g_file_test(file, G_FILE_TEST_IS_REGULAR));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND state = 'present'",
		library_file), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND series_id = %"
		G_GINT64_FORMAT " AND import_id = %" G_GINT64_FORMAT, library_file, series, import), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT
		" AND episode_id = %" G_GINT64_FORMAT, library_file, e2), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import WHERE id = %" G_GINT64_FORMAT " AND state = 'imported'", import),
		==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE id = %" G_GINT64_FORMAT " AND close_reason ="
		" 'imported' AND removal IS NULL AND retry_at IS NULL", grab), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job"), ==, jobs);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'blocklisted' AND by_user = 'root' AND text LIKE '%%its file in the library stays as it is%%'", series), ==, 1);
	reply = get_as(fx, fx->admin, landed);
	assert_contains(reply.body, "Blocked for the future.");
	axis_test_reply_clear(&reply);
}

/*
 * add-block-release, the lesser faults of its review.  A season pack part
 * placed (a partial import) and still downloading is blocked from the library
 * link: the files it placed stay as they are, no import is left open, and the
 * link says it stops the rest of the download; an import that is placing is
 * refused; a title that is not monitored is told Axis does not search again,
 * and queues nothing; a person's block keeps the reason of the failure entry
 * it replaces, and a second post writes no second line of history.
 */
static void
test_block_release_faults(Fixture *fx, gconstpointer data)
{
	static const Rel second[] = {
		{ TIDE_E02, 1400000000 }
	};
	static const Rel first[] = {
		{ TIDE_E01, 1400000000 }
	};
	g_autofree gchar *links = NULL;
	g_autofree gchar *confirm = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *landed = NULL;
	g_autofree gchar *n = NULL;
	AxisTestReply reply;
	gint64 series;
	gint64 e1;
	gint64 e2;
	gint64 grab;
	gint64 download;
	gint64 import;
	gint64 library_file;

	set_route(fx, "/title/series/207", "{\"id\":\"207\",\"kind\":\"series\",\"title\":\"Tidewater Inn\",\"year\":2026,"
		"\"status\":\"continuing\",\"runtime\":45,\"overview\":\"Rooms by the tide.\",\"ids\":{\"tvdb\":\"370007\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-11-06\",\"episodes\":2}]}");
	set_route(fx, "/title/series/207/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9701\",\"name\":\"Low Water\",\"air_date\":\"2026-11-06\",\"runtime\":45},"
		"{\"number\":2,\"id\":\"9702\",\"name\":\"Spring Tide\",\"air_date\":\"2026-11-13\",\"runtime\":45}]}");
	series = add_series(fx, "207", AXIS_TV_MONITOR_ALL);
	e1 = episode_id(fx, series, 1, 1);
	e2 = episode_id(fx, series, 1, 2);
	links = g_strdup_printf("/series/%" G_GINT64_FORMAT "/links", series);

	/* 1. A partial import whose grab is open: eight placed, the rest not. */
	set_releases(fx, second, G_N_ELEMENTS(second));
	search(fx, series, AXIS_TV_SCOPE_EPISODE, e2);
	grab = qi(fx, "SELECT id FROM series_grab WHERE series_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", series);
	g_assert_cmpint(grab, >, 0);
	download = qi(fx, "SELECT download_id FROM series_grab WHERE id = %" G_GINT64_FORMAT, grab);
	import = qi(fx, "INSERT INTO import (module, owner_id, title_label, source_path, state, download_id, finished_at)"
		" VALUES ('tv', %" G_GINT64_FORMAT ", 'Tidewater Inn (2026)', 'downloads/tidewater', 'partial', %"
		G_GINT64_FORMAT ", now()) RETURNING id", series, download);
	g_assert_cmpint(import, >, 0);
	in_library(fx, series, e2, "Tidewater Inn (2026)/Season 01/S01E02.mkv", "webdl_1080p");
	library_file = qi(fx, "SELECT min(file_id) FROM series_file_episode WHERE episode_id = %" G_GINT64_FORMAT, e2);
	{
		g_autofree gchar *sql = g_strdup_printf("UPDATE series_file SET import_id = %" G_GINT64_FORMAT " WHERE file_id = %"
			G_GINT64_FORMAT, import, library_file);

		axis_test_db_exec(fx->t->db, sql);
	}
	/* The link says what it will do: the grab is open. */
	reply = get_as(fx, fx->admin, links);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, ">Block and stop the rest of this download</a>");
	assert_lacks(reply.body, ">Block this release</a>");
	axis_test_reply_clear(&reply);
	confirm = g_strdup_printf("/series/block-release?import=%" G_GINT64_FORMAT, import);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes", series, grab);

	/* 2. An import that is placing: refused, nothing changed. */
	axis_test_db_exec(fx->t->db, "UPDATE import SET state = 'running', finished_at = NULL");
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "Axis is importing this download right now. Try again when it has finished.");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE id = %" G_GINT64_FORMAT " AND closed_at IS NULL", grab),
		==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_blocklist"), ==, 0);
	axis_test_db_exec(fx->t->db, "UPDATE import SET state = 'partial', finished_at = now()");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");

	/* 3. Blocked while partial: the placed file and its links stay, no import
	 * is left open, the grab is closed. */
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	landed = g_strdup_printf("/series/block-release?grab=%" G_GINT64_FORMAT "&done=cancelled", grab);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import WHERE state IN ('waiting', 'running', 'blocked', 'partial')"),
		==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM import WHERE id = %" G_GINT64_FORMAT " AND state = 'dismissed'", import),
		==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM library_file WHERE id = %" G_GINT64_FORMAT " AND state = 'present'",
		library_file), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file WHERE file_id = %" G_GINT64_FORMAT " AND series_id = %"
		G_GINT64_FORMAT " AND import_id = %" G_GINT64_FORMAT, library_file, series, import), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_file_episode WHERE file_id = %" G_GINT64_FORMAT
		" AND episode_id = %" G_GINT64_FORMAT, library_file, e2), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE id = %" G_GINT64_FORMAT " AND close_reason ="
		" 'given_up'", grab), ==, 1);
	/* The title is monitored: the next best is looked for. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search' AND payload->>'cause' = 'retry'"
		" AND state = 'queued'"), ==, 1);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");

	/* 4. A title that is not monitored: Axis says it does not search again, and
	 * queues nothing. */
	set_releases(fx, first, G_N_ELEMENTS(first));
	search(fx, series, AXIS_TV_SCOPE_EPISODE, e1);
	g_free(form);
	grab = qi(fx, "SELECT id FROM series_grab WHERE series_id = %" G_GINT64_FORMAT " AND closed_at IS NULL", series);
	g_assert_cmpint(grab, >, 0);
	form = g_strdup_printf("owner=%" G_GINT64_FORMAT "&grab=%" G_GINT64_FORMAT "&confirm=yes", series, grab);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET monitored = false");
	g_free(confirm);
	confirm = g_strdup_printf("/series/block-release?grab=%" G_GINT64_FORMAT, grab);
	reply = get_as(fx, fx->admin, confirm);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis does not search for another release afterwards");
	assert_contains(reply.body, "Block this release and give up on it?");
	assert_lacks(reply.body, "and lets Axis search for the next best release");
	assert_lacks(reply.body, "Block and search again");
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	g_free(landed);
	landed = g_strdup_printf("/series/block-release?grab=%" G_GINT64_FORMAT "&done=cancelled-held", grab);
	assert_contains(axis_test_reply_header(&reply, "Location"), landed);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE id = %" G_GINT64_FORMAT " AND close_reason ="
		" 'given_up'", grab), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
	reply = get_as(fx, fx->admin, landed);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Axis does not search for another release");
	axis_test_reply_clear(&reply);

	/* 5. A person's block that takes the place of a failure's entry keeps its
	 * reason; a second post of it writes no second line of history. */
	axis_test_db_exec(fx->t->db, "UPDATE series_blocklist SET reason = 'failed', reason_text = 'It failed to import.',"
		" created_by = NULL WHERE release_name = '" TIDE_E01 "'");
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	n = q(fx, "SELECT reason || '|' || reason_text FROM series_blocklist WHERE release_name = '" TIDE_E01 "'");
	g_assert_cmpstr(n, ==, "blocked|Blocked by root. Before: It failed to import.");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'blocklisted' AND by_user = 'root'", series), ==, 1);
	reply = post_as(fx, fx->admin, confirm, "/series/block-release", form);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'blocklisted' AND by_user = 'root'", series), ==, 1);
	g_free(n);
	n = q(fx, "SELECT reason_text FROM series_blocklist WHERE release_name = '" TIDE_E01 "'");
	g_assert_cmpstr(n, ==, "Blocked by root. Before: It failed to import.");
}

/* ── Anime: the kind of a series (add-anime-support, A1) ────────── */

/* (The name is slice A1's, when this said what Axis did not do yet; since A3
 * it says what marking a series does.) */
#define NOT_YET "Axis recognises a release or a file named by an absolute episode number for a series marked as anime, " \
	"when TheTVDB's absolute numbers for it can be used. It then also searches for each episode by that number, and " \
	"searches in the anime category."

/* Tenkuu Rangers (made up): TMDb 7071 (the fixture provider stands in for
 * it), one season of six; TheTVDB 81071, the same six as two seasons of
 * three, numbered 1 to 6 absolutely, and a special TheTVDB gives the number 2
 * (never counted).  Kumo Academy (7072, TheTVDB 81072): TheTVDB lists
 * nothing for it. */
static void
set_tenkuu(Fixture *fx)
{
	g_autoptr(GString) tmdb = g_string_new("{\"number\":1,\"name\":\"Season 1\",\"episodes\":[");
	g_autoptr(GString) tvdb = g_string_new("{\"status\":\"success\",\"data\":{\"series\":{\"id\":81071},\"episodes\":["
		"{\"id\":81071900,\"seriesId\":81071,\"name\":\"A Special\",\"aired\":\"2024-01-01\",\"runtime\":24,"
		"\"seasonNumber\":0,\"number\":1,\"absoluteNumber\":2}");
	guint i;

	for (i = 0; i < 6; i++) {
		g_string_append_printf(tmdb, "%s{\"number\":%u,\"id\":\"97%03u\",\"name\":\"Ranger %u\",\"air_date\":"
			"\"2024-02-%02u\",\"runtime\":24}", i > 0 ? "," : "", i + 1, i + 1, i + 1, i + 1);
		g_string_append_printf(tvdb, ",{\"id\":81071%03u,\"seriesId\":81071,\"name\":\"Ranger %u\",\"aired\":"
			"\"2024-02-%02u\",\"runtime\":24,\"seasonNumber\":%u,\"number\":%u,\"absoluteNumber\":%u}", i + 1, i + 1, i + 1,
			i < 3 ? 1 : 2, i < 3 ? i + 1 : i - 2, i + 1);
	}
	g_string_append(tmdb, "]}");
	g_string_append(tvdb, "]},\"links\":{\"prev\":null,\"next\":null}}");
	set_route(fx, "/title/series/7071", "{\"id\":\"7071\",\"kind\":\"series\",\"title\":\"Tenkuu Rangers\",\"year\":2024,"
		"\"status\":\"ended\",\"runtime\":24,\"ids\":{\"tvdb\":\"81071\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"episodes\":6}]}");
	set_route(fx, "/title/series/7071/season/1", tmdb->str);
	axis_test_tvdb_set_answer(fx->tvdb, "/v4/series/81071/episodes/official/eng", "page=0", tvdb->str);
	set_route(fx, "/title/series/7072", "{\"id\":\"7072\",\"kind\":\"series\",\"title\":\"Kumo Academy\",\"year\":2023,"
		"\"status\":\"ended\",\"runtime\":24,\"ids\":{\"tvdb\":\"81072\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"episodes\":1}]}");
	set_route(fx, "/title/series/7072/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":[{\"number\":1,"
		"\"id\":\"98001\",\"name\":\"Enrolment\",\"air_date\":\"2023-04-01\",\"runtime\":24}]}");
}

/* Everything of a series but its kind and its history: its row's other
 * columns, its episodes, its files and their links. */
static gchar *
series_fingerprint(
	Fixture *fx,
	gint64   series
){
	return q(fx, "SELECT md5((SELECT s.episode_source || '|' || s.quality_profile_id || '|' || s.root_id || '|' ||"
		" s.monitor_choice || '|' || s.monitor_new || '|' || coalesce(s.search_hold, '-') FROM series s WHERE s.id = %"
		G_GINT64_FORMAT ") || (SELECT string_agg(e.id || ':' || e.season_number || 'x' || e.number || ':' ||"
		" coalesce(e.absolute_number::text, '-') || ':' || e.monitored || ':' || e.source || ':' || (e.gone_at IS NULL),"
		" ',' ORDER BY e.id) FROM series_episode e WHERE e.series_id = %" G_GINT64_FORMAT ") || coalesce((SELECT"
		" string_agg(sfe.file_id || '>' || sfe.episode_id || ':' || sf.kept || ':' || coalesce(sf.hand_how, '-'), ','"
		" ORDER BY sfe.file_id, sfe.episode_id) FROM series_file sf JOIN series_file_episode sfe ON sfe.file_id ="
		" sf.file_id WHERE sf.series_id = %" G_GINT64_FORMAT "), ''))", series, series, series);
}

/* The tag of the input @id on a page, to see whether it is ticked. */
static gchar *
input_of(
	const gchar *body,
	const gchar *id
){
	g_autofree gchar *pattern = g_strdup_printf("<input[^>]*id=\"%s\"[^>]*>", id);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (!g_regex_match(regex, body, 0, &match))
		g_error("no input '%s' in:\n%s", id, body);
	return g_match_info_fetch(match, 0);
}

/*
 * Spec (tv): "A series has a kind, and Anime is set only by a person or by
 * Sonarr" (A viewer; recorded in the history; no file, link or numbering
 * changes); "An anime series' absolute numbers are TheTVDB's, shown, and used
 * only when they can be trusted" (TMDb's numbering; Usable; A number on two
 * episodes), on the series' page.  And what a migration's comparison with
 * Sonarr left on the series is shown there.
 */
static void
test_anime_kind(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisTvRenumberPlan) plan = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *action = NULL;
	g_autofree gchar *numbering = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *input = NULL;
	AxisTestReply reply;
	gboolean changed = FALSE;
	gint64 series;

	set_tenkuu(fx);
	series = add_series(fx, "7071", AXIS_TV_MONITOR_ALL);
	in_library(fx, series, episode_id(fx, series, 1, 2), "Tenkuu Rangers (2024)/Season 01/Tenkuu Rangers (2024) - "
		"S01E02.mkv", "webdl_1080p");
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/kind", series);
	numbering = g_strdup_printf("/series/%" G_GINT64_FORMAT "/numbering?to=tvdb", series);

	/* A standard series, as every series is: no card, no column; the box is
	 * offered to an administrator only, and is not ticked. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE kind = 'standard'"), ==, 1);
	reply = get_as(fx, fx->admin, path);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_lacks(reply.body, "Anime numbering");
	assert_lacks(reply.body, ">Absolute<");
	assert_contains(reply.body, "Anime: its releases are often numbered 1, 2, 3 and on across seasons");
	assert_contains(reply.body, NOT_YET);
	input = input_of(reply.body, "series-anime");
	assert_lacks(input, "checked");
	g_clear_pointer(&input, g_free);
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->viewer, path);
	assert_lacks(reply.body, "series-anime");
	assert_lacks(reply.body, "/kind");
	axis_test_reply_clear(&reply);

	/* A viewer cannot mark it, whatever the token; nor an administrator
	 * without the token.  Nothing changed. */
	before = series_fingerprint(fx, series);
	reply = post_as(fx, fx->viewer, path, action, "anime=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, action, "anime=yes");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE kind = 'anime'"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE text LIKE '%%anime%%'"), ==, 0);

	/* An administrator marks it: one write, one history line, and nothing
	 * else of the series: no episode, file, link, monitoring or numbering. */
	reply = post_as(fx, fx->admin, path, action, "anime=yes");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=anime");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE kind = 'anime' AND episode_source = 'fixture'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'changed' AND text LIKE"
		" 'Marked as anime by root. Nothing else changed%%'"), ==, 1);
	after = series_fingerprint(fx, series);
	g_assert_cmpstr(after, ==, before);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind LIKE 'tv.%%'"), ==, 0);
	g_assert_cmpuint(logged("kind changed"), >=, 1);

	/* Its page: the card, with the state of its numbers.  It follows TMDb's
	 * numbering (the fixture's), which has none; without a TheTVDB key, the
	 * key first. */
	{
		g_autofree gchar *done = g_strdup_printf("%s?done=anime", path);

		reply = get_as(fx, fx->admin, done);
	}
	assert_contains(reply.body, "The series is marked as anime. Nothing else changed");
	assert_contains(reply.body, "Anime numbering");
	assert_contains(reply.body, "This series is marked as anime. Axis numbers it by season, as media servers do (S06E12).");
	assert_contains(reply.body, "Absolute numbers come from TheTVDB's aired order, and this series is numbered as fixture "
		"numbers it, which has none. Set a TheTVDB key on the Movie and TV details page first");
	/* What is matched (slice A2) and what is searched (A3): while its numbers
	 * cannot be used, nothing is matched through them and nothing is asked
	 * for by them. */
	assert_contains(reply.body, "While this series' absolute numbers cannot be used, a release named \"Title - 137\" is "
		"ruled out");
	assert_contains(reply.body, "Axis searches for it by season and episode, in the anime category as well as in TV, and "
		"not by absolute number.");
	assert_lacks(reply.body, "Searched for by absolute number too.");
	assert_lacks(reply.body, "is matched to the one aired episode");
	assert_contains(reply.body, ">Absolute<");
	assert_lacks(reply.body, "Change the episode numbering");
	input = input_of(reply.body, "series-anime");
	assert_contains(input, "checked");
	g_clear_pointer(&input, g_free);
	assert_csp_clean(reply.body);
	save_page("series-anime.html", reply.body);
	axis_test_reply_clear(&reply);
	/* A viewer sees the card and the column, and no form. */
	reply = get_as(fx, fx->viewer, path);
	assert_contains(reply.body, "Anime numbering");
	assert_lacks(reply.body, "/kind");
	axis_test_reply_clear(&reply);

	/* With a key: how to have numbers, with the page that changes the
	 * numbering (an administrator's). */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "which has none. To have absolute numbers for it, change its numbering to TheTVDB's aired "
		"order.");
	assert_contains(reply.body, numbering);
	axis_test_reply_clear(&reply);

	/* Under TheTVDB's aired order its episodes carry TheTVDB's numbers: usable,
	 * whole and in order, the special's number counted nowhere; each shown in
	 * the episode table.  The kind stayed. */
	g_assert_true(axis_tv_plan_numbering(fx->tv, series, "tvdb", TRUE, &plan, &error));
	g_assert_no_error(error);
	g_assert_true(axis_tv_change_numbering(fx->tv, series, "tvdb", plan->digest, FALSE, "root", NULL, &changed, &error));
	g_assert_no_error(error);
	g_free(after);
	after = q(fx, "SELECT string_agg(season_number || 'x' || number || '=' || coalesce(absolute_number::text, '-'), ' '"
		" ORDER BY season_number, number) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND gone_at IS NULL AND source = 'tvdb'", series);
	g_assert_cmpstr(after, ==, "0x1=2 1x1=1 1x2=2 1x3=3 2x1=4 2x2=5 2x3=6");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE kind = 'anime' AND episode_source = 'tvdb'"), ==, 1);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "TheTVDB gives absolute numbers for 6 of 6 episodes. They equal a plain count of the "
		"episodes in aired order.");
	assert_contains(reply.body, "<td class=\"episode-absolute col-text\" data-label=\"Absolute\">5</td>");
	/* Usable: what is matched, what is refused, and what a search asks for
	 * (slice A3: no "not yet" is left on the card). */
	assert_contains(reply.body, "is matched to the one aired episode TheTVDB gives that number");
	assert_contains(reply.body, "Not read, each with its reason on the manual search");
	assert_contains(reply.body, "Searched for by absolute number too. For an episode that has one, Axis asks each "
		"indexer by season and episode, as for any series, and by the series' title with the number");
	assert_contains(reply.body, "A batch (\"Title - 01-12\") is taken as exactly the episodes its numbers map to");
	assert_lacks(reply.body, "Not searched yet");
	assert_lacks(reply.body, "comes later");
	assert_lacks(reply.body, "cannot be used, a release named");
	axis_test_reply_clear(&reply);

	/* A number on two episodes: not usable, both named. */
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET absolute_number = 2 WHERE season_number = 2 AND number = 1"
		" AND source = 'tvdb'");
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "TheTVDB gives two episodes of this series the same absolute number (S01E02 and S02E01 "
		"are both 2). Axis does not use this series' absolute numbers.");
	axis_test_reply_clear(&reply);
	/* Numbers out of order. */
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET absolute_number = 9 WHERE season_number = 2 AND number = 1"
		" AND source = 'tvdb'");
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "TheTVDB's absolute numbers for this series are not in order (S02E01 is 9 and S02E02 is "
		"5). Axis does not use them.");
	axis_test_reply_clear(&reply);
	/* None given. */
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET absolute_number = NULL WHERE season_number > 0");
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "TheTVDB gives no absolute numbers for this series.");
	axis_test_reply_clear(&reply);

	/* What a migration's comparison with Sonarr left on the series: its
	 * sentence and every episode that differs. */
	axis_test_db_exec(fx->t->db, "UPDATE series SET brought_absolute = '{\"agree\":4,\"differ\":2,\"one_side\":0,"
		"\"list\":[[2,2,9,5],[2,3,10,6]]}'::jsonb");
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "When it was migrated from Sonarr: Absolute numbers: Sonarr and TheTVDB differ for 2 of 6 "
		"episodes, the first S02E02 (Sonarr 9, TheTVDB 5), and agree for 4.");
	assert_contains(reply.body, "Where they differ: S02E02 (Sonarr 9, TheTVDB 5); S02E03 (Sonarr 10, TheTVDB 6).");
	axis_test_reply_clear(&reply);

	/* Cleared by an administrator: one line more, and the card is gone.  The
	 * same again changes nothing and writes nothing. */
	g_free(before);
	before = series_fingerprint(fx, series);
	reply = post_as(fx, fx->admin, path, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=not_anime");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE kind = 'standard' AND episode_source = 'tvdb'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE text LIKE 'No longer marked as anime by root.%%'"),
		==, 1);
	g_free(after);
	after = series_fingerprint(fx, series);
	g_assert_cmpstr(after, ==, before);
	reply = post_as(fx, fx->admin, path, action, "");
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=kind_same");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE text LIKE '%%arked as anime%%'"), ==, 2);
	reply = get_as(fx, fx->admin, path);
	assert_lacks(reply.body, "Anime numbering");
	assert_lacks(reply.body, ">Absolute<");
	axis_test_reply_clear(&reply);
	/* No such series. */
	reply = post_as(fx, fx->admin, path, "/series/999999/kind", "anime=yes");
	g_assert_cmpuint(reply.status, ==, 404);
	axis_test_reply_clear(&reply);
}

static gint64
add_from_page(
	Fixture     *fx,
	const gchar *provider_id,
	const gchar *more	/* "&anime=yes", or "" */
){
	gint64 title = title_of(fx, provider_id);
	g_autofree gchar *page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);
	g_autofree gchar *form = g_strdup_printf("title=%" G_GINT64_FORMAT "&root=%" G_GINT64_FORMAT "&monitor=all%s", title,
		fx->root_id, more);
	AxisTestReply reply = post_as(fx, fx->admin, page, "/series", form);

	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	return qi(fx, "SELECT id FROM series WHERE title_id = %" G_GINT64_FORMAT, title);
}

/*
 * Spec (tv): "A series has a kind" (Marked on the Add form; Never by itself);
 * "An anime series added by hand follows the numbering that carries absolute
 * numbers" (With a key; Without a key; and TheTVDB not listing it).
 */
static void
test_anime_add(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *rows = NULL;
	g_autofree gchar *input = NULL;
	AxisTestReply reply;
	gint64 title;
	gint64 series;

	set_tenkuu(fx);
	set_gullwing(fx);
	title = title_of(fx, "7071");
	page = g_strdup_printf("/title/%" G_GINT64_FORMAT, title);

	/* The Add form: the box, never ticked; without a TheTVDB key, what that
	 * means for its numbering; no hint, since the details say nothing. */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Anime: its releases are often numbered 1, 2, 3 and on across seasons");
	assert_contains(reply.body, "Axis has no TheTVDB key, so ticked or not the series follows the numbering of any new "
		"series");
	assert_contains(reply.body, NOT_YET);
	assert_lacks(reply.body, "The details say this is animation from Japan.");
	input = input_of(reply.body, "series-add-anime");
	assert_lacks(input, "checked");
	g_clear_pointer(&input, g_free);
	axis_test_reply_clear(&reply);
	/* A viewer is offered nothing. */
	reply = get_as(fx, fx->viewer, page);
	assert_lacks(reply.body, "series-add-anime");
	axis_test_reply_clear(&reply);

	/* Without a key: ticked, the series is anime under the numbering any new
	 * series gets, and its page says a key is needed for absolute numbers. */
	series = add_from_page(fx, "7061", "&anime=yes");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND kind = 'anime'"
		" AND episode_source = 'fixture'", series), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT
		" AND absolute_number IS NOT NULL", series), ==, 0);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Anime numbering");
	assert_contains(reply.body, "which has none. Set a TheTVDB key on the Movie and TV details page first");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'added'"
		" AND text LIKE 'Added by root%%Marked as anime, numbered as fixture numbers it, which has no absolute"
		" numbers.%%'", series), ==, 1);

	/* With a key, and details that say animation from Japan: the form says
	 * both, and the box is still not ticked. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "UPDATE meta_title SET genres = '{Drama,Animation}', original_language = 'ja'"
		" WHERE provider_id IN ('7071', '202')");
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "The details say this is animation from Japan.");
	assert_contains(reply.body, "Ticked, the series follows TheTVDB's aired order from the start, which carries absolute "
		"numbers, when TheTVDB lists it");
	input = input_of(reply.body, "series-add-anime");
	assert_lacks(input, "checked");
	g_clear_pointer(&input, g_free);
	axis_test_reply_clear(&reply);

	/* Never by itself: added without the tick, a series whose details say
	 * animation from Japan is standard, under the numbering of any series. */
	series = add_from_page(fx, "202", "");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND kind = 'standard'"
		" AND episode_source = 'fixture'", series), ==, 1);

	/* Ticked, with a key, and TheTVDB lists it: TheTVDB's aired order from
	 * the start (never added under one numbering and switched), its episodes
	 * with TheTVDB's absolute numbers. */
	series = add_from_page(fx, "7071", "&anime=yes");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND kind = 'anime'"
		" AND episode_source = 'tvdb'", series), ==, 1);
	rows = q(fx, "SELECT string_agg(season_number || 'x' || number || '=' || coalesce(absolute_number::text, '-') || ':'"
		" || source, ' ' ORDER BY season_number, number) FROM series_episode WHERE series_id = %" G_GINT64_FORMAT, series);
	g_assert_cmpstr(rows, ==, "0x1=2:tvdb 1x1=1:tvdb 1x2=2:tvdb 1x3=3:tvdb 2x1=4:tvdb 2x2=5:tvdb 2x3=6:tvdb");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT
		" AND kind = 'numbering'", series), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind = 'added'"
		" AND text LIKE 'Added by root%%Marked as anime, numbered as TheTVDB''s aired order numbers it.%%'", series), ==,
		1);
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Anime numbering");
	assert_contains(reply.body, "TheTVDB gives absolute numbers for 6 of 6 episodes. They equal a plain count of the "
		"episodes in aired order.");
	assert_contains(reply.body, "Numbered as TheTVDB's aired order numbers them.");
	/* Usable from the start: what is matched, and what a search asks for. */
	assert_contains(reply.body, "is matched to the one aired episode TheTVDB gives that number");
	assert_contains(reply.body, "Searched for by absolute number too.");
	assert_contains(reply.body, "<td class=\"episode-absolute col-text\" data-label=\"Absolute\">6</td>");
	axis_test_reply_clear(&reply);

	/* Ticked, with a key, and TheTVDB lists nothing for it: the numbering of
	 * any new series, and its page says its numbers have no source. */
	series = add_from_page(fx, "7072", "&anime=yes");
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE id = %" G_GINT64_FORMAT " AND kind = 'anime'"
		" AND episode_source = 'fixture'", series), ==, 1);
	g_free(path);
	path = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	reply = get_as(fx, fx->admin, path);
	assert_contains(reply.body, "Absolute numbers come from TheTVDB's aired order, and this series is numbered as fixture "
		"numbers it, which has none.");
	axis_test_reply_clear(&reply);
}

/* ── Replace the download in flight (add-replace-grab-and-delete-download, A) ── */

/* Marsh Lantern (2026), a made-up series, and made-up releases of it. */
#define MARSH_E03      "Marsh.Lantern.S01E03.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define MARSH_E03_DISC "Marsh.Lantern.S01E03.1080p.BluRay.x264-GRP"
#define MARSH_E04      "Marsh.Lantern.S01E04.1080p.WEB-DL.DDP5.1.H.264-NTb"
#define MARSH_PACK     "Marsh.Lantern.S01.1080p.WEB-DL.DDP5.1.H.264-NTb"

/* The value of the hidden field @name of a page's form ("" for none). */
static gchar *
field_of(
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

typedef struct
{
	Fixture *fx;
	gint64   series;
	gchar   *page;
	gchar   *grab;
} Marsh;

/* The confirmation of a grab by hand of @release. */
static AxisTestReply
marsh_page(
	Marsh       *m,
	const gchar *release
){
	g_autofree gchar *path = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", m->grab,
		listed_candidate(m->fx, release));

	return get_as(m->fx, m->fx->admin, path);
}

static AxisTestReply
marsh_post(
	Marsh       *m,
	const gchar *release,
	const gchar *more
){
	g_autofree gchar *form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes%s",
		listed_candidate(m->fx, release), more);

	return post_as(m->fx, m->fx->admin, m->page, m->grab, form);
}

/* The open grab of @release downloads, a quarter done; its client's id. */
static gchar *
marsh_downloading(
	Marsh       *m,
	const gchar *release
){
	gchar *ref = q(m->fx, "SELECT d.client_ref FROM series_grab g JOIN download d ON d.id = g.download_id"
		" WHERE g.release_name = '%s' AND g.closed_at IS NULL", release);

	g_assert_nonnull(ref);
	axis_test_download_fake_set(m->fx->fake, ref, "downloading", 1000000000, NULL, NULL);
	poll(m->fx);
	return ref;
}

/* "S01E03,S01E04": the episodes that have an open grab. */
static gchar *
marsh_open(Marsh *m)
{
	return q(m->fx, "SELECT coalesce(string_agg('S' || lpad(e.season_number::text, 2, '0') || 'E' ||"
		" lpad(e.number::text, 2, '0'), ',' ORDER BY e.season_number, e.number), '') FROM series_grab_episode ge"
		" JOIN series_episode e ON e.id = ge.episode_id WHERE ge.open AND e.series_id = %" G_GINT64_FORMAT, m->series);
}

/*
 * Spec (tv): "An administrator can replace a series' download in flight",
 * scenarios "An episode in place of an episode", "A pack in flight",
 * "Without the tick for the others", "Two grabs in the way", "It has
 * completed"; and a viewer.
 */
static void
test_replace_grab(Fixture *fx, gconstpointer data)
{
	static const Rel found[] = {
		{ MARSH_E03, 1500000000 },
		{ MARSH_E03_DISC, 4000000000 },
		{ MARSH_E04, 1500000000 },
		{ MARSH_PACK, 7000000000 }
	};
	g_autofree gchar *ref = NULL;
	g_autofree gchar *swap = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	AxisAcquireSearchReport report;
	AxisTestReply reply;
	Marsh m;

	set_route(fx, "/title/series/231", "{\"id\":\"231\",\"kind\":\"series\",\"title\":\"Marsh Lantern\",\"year\":2026,"
		"\"status\":\"continuing\",\"runtime\":45,\"overview\":\"A light on the marsh.\",\"ids\":{\"tvdb\":\"370031\"},"
		"\"seasons\":[{\"number\":1,\"name\":\"Season 1\",\"air_date\":\"2026-10-02\",\"episodes\":5}]}");
	set_route(fx, "/title/series/231/season/1", "{\"number\":1,\"name\":\"Season 1\",\"episodes\":["
		"{\"number\":1,\"id\":\"9311\",\"name\":\"Reed Bed\",\"air_date\":\"2026-10-02\",\"runtime\":45},"
		"{\"number\":2,\"id\":\"9312\",\"name\":\"Low Mist\",\"air_date\":\"2026-10-09\",\"runtime\":45},"
		"{\"number\":3,\"id\":\"9313\",\"name\":\"Causeway\",\"air_date\":\"2026-10-16\",\"runtime\":45},"
		"{\"number\":4,\"id\":\"9314\",\"name\":\"Sluice\",\"air_date\":\"2026-10-23\",\"runtime\":45},"
		"{\"number\":5,\"id\":\"9315\",\"name\":\"High Ground\",\"air_date\":\"2026-10-30\",\"runtime\":45}]}");
	memset(&m, 0, sizeof(m));
	m.fx = fx;
	m.series = add_series(fx, "231", AXIS_TV_MONITOR_ALL);
	m.page = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual?level=season&item=1", m.series);
	m.grab = g_strdup_printf("/series/%" G_GINT64_FORMAT "/manual/grab", m.series);
	set_releases(fx, found, G_N_ELEMENTS(found));
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, m.series, AXIS_TV_SCOPE_SEASON, 1, "list", "root", NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(report.outcome, ==, "listed");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");

	/* Scenario: An episode in place of an episode. */
	reply = marsh_post(&m, MARSH_E03, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	ref = marsh_downloading(&m, MARSH_E03);
	reply = marsh_page(&m, MARSH_E03_DISC);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "A download is in flight");
	assert_contains(reply.body, "Downloading now: " MARSH_E03 ", at SABnzbd, 25 % of ");
	assert_contains(reply.body, "It holds S01E03.");
	assert_contains(reply.body, "Replace the download in flight with this one");
	assert_lacks(reply.body, "name=\"replace_others\"");
	assert_lacks(reply.body, "cannot be grabbed");
	assert_csp_clean(reply.body);
	save_page("replace-offer.html", reply.body);
	{
		g_autofree gchar *grab = field_of(reply.body, "replace");
		g_autofree gchar *digest = field_of(reply.body, "replace_digest");

		g_assert_cmpuint(strlen(digest), ==, 32);
		swap = g_strdup_printf("&replace=%s&replace_digest=%s", grab, digest);
	}
	axis_test_reply_clear(&reply);
	/* A viewer is refused the page and the post. */
	{
		g_autofree gchar *path = g_strdup_printf("%s?candidate=%" G_GINT64_FORMAT "&level=season&item=1", m.grab,
			listed_candidate(fx, MARSH_E03_DISC));
		g_autofree gchar *form = g_strdup_printf("candidate=%" G_GINT64_FORMAT "&level=season&item=1&confirm=yes%s"
			"&replace_ok=yes", listed_candidate(fx, MARSH_E03_DISC), swap);

		reply = get_as(fx, fx->viewer, path);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->viewer, m.page, m.grab, form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
		reply = post_as(fx, fx->admin, NULL, m.grab, form);
		g_assert_cmpuint(reply.status, ==, 403);
		axis_test_reply_clear(&reply);
	}
	/* Without the tick: nothing. */
	reply = marsh_post(&m, MARSH_E03_DISC, swap);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "tick the box to replace the download in flight with this release");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE closed_at IS NULL"), ==, 1);
	text = g_strdup_printf("%s&replace_ok=yes", swap);
	reply = marsh_post(&m, MARSH_E03_DISC, text);
	g_assert_cmpuint(reply.status, ==, 303);
	assert_contains(axis_test_reply_header(&reply, "Location"), "done=grabbed");
	axis_test_reply_clear(&reply);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", ref), ==, 1);
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT string_agg(release_name || '|' || coalesce(close_reason, 'open') || '|' || coalesce(removal,"
		" '-') || '|' || coalesce(close_text, '-'), ',' ORDER BY id) FROM series_grab");
	g_assert_cmpstr(text, ==, MARSH_E03 "|replaced|done|Replaced by root with " MARSH_E03_DISC "."
		"," MARSH_E03_DISC "|open|-|-");
	g_clear_pointer(&text, g_free);
	text = marsh_open(&m);
	g_assert_cmpstr(text, ==, "S01E03");
	g_clear_pointer(&text, g_free);
	/* Not blocked, and no search queued by it. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_blocklist"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE series_id = %" G_GINT64_FORMAT " AND kind ="
		" 'abandoned' AND by_user = 'root' AND text LIKE '" MARSH_E03 ": Replaced by root with " MARSH_E03_DISC ".%%'",
		m.series), ==, 1);

	/* Scenario: Two grabs in the way.  A pack is chosen while two of its
	 * episodes download as two grabs: no offer, each named, and a link to
	 * the Downloads page. */
	reply = marsh_post(&m, MARSH_E04, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(marsh_downloading(&m, MARSH_E03_DISC));
	g_free(marsh_downloading(&m, MARSH_E04));
	reply = marsh_page(&m, MARSH_PACK);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "cannot be grabbed");
	assert_contains(reply.body, "2 downloads are in the way: " MARSH_E03_DISC " (S01E03) and " MARSH_E04 " (S01E04). "
		"Axis does not replace more than one in one step. Give up on each of them on the Downloads page first");
	assert_contains(reply.body, "href=\"/downloads\"");
	assert_lacks(reply.body, "Replace the download in flight with this one");
	assert_lacks(reply.body, "name=\"replace\"");
	axis_test_reply_clear(&reply);
	/* A post that asks for a replace of one of them all the same: nothing. */
	{
		g_autofree gchar *forged = g_strdup_printf("&replace=%" G_GINT64_FORMAT "&replace_ok=yes&replace_others=yes"
			"&replace_digest=0123456789abcdef0123456789abcdef", qi(fx, "SELECT id FROM series_grab WHERE release_name = '"
			MARSH_E04 "'"));

		reply = marsh_post(&m, MARSH_PACK, forged);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "The download in flight changed after you opened this page");
		axis_test_reply_clear(&reply);
	}
	text = marsh_open(&m);
	g_assert_cmpstr(text, ==, "S01E03,S01E04");
	g_clear_pointer(&text, g_free);
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 2);

	/* Scenario: A pack in flight.  The pack is ended whole: the episodes the
	 * release chosen does not hold are named, need a tick of their own, and
	 * are wanted again afterwards. */
	g_assert_true(axis_tv_give_up(fx->tv, m.series, qi(fx, "SELECT id FROM series_grab WHERE release_name = '"
		MARSH_E03_DISC "' AND closed_at IS NULL"), "root", &error));
	g_assert_true(axis_tv_give_up(fx->tv, m.series, qi(fx, "SELECT id FROM series_grab WHERE release_name = '"
		MARSH_E04 "' AND closed_at IS NULL"), "root", &error));
	g_assert_no_error(error);
	axis_test_db_exec(fx->t->db, "DELETE FROM series_blocklist");
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 0);
	reply = marsh_post(&m, MARSH_PACK, "");
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_free(ref);
	ref = marsh_downloading(&m, MARSH_PACK);
	text = marsh_open(&m);
	g_assert_cmpstr(text, ==, "S01E01,S01E02,S01E03,S01E04,S01E05");
	g_clear_pointer(&text, g_free);
	reply = marsh_page(&m, MARSH_E03);
	g_assert_cmpuint(reply.status, ==, 200);
	assert_contains(reply.body, "Downloading now: " MARSH_PACK ", at SABnzbd, 25 % of ");
	assert_contains(reply.body, "The download in flight is ended whole. It also holds S01E01, S01E02, S01E04, S01E05, "
		"which this release does not: they stop downloading too and go back to ordinary searching.");
	assert_contains(reply.body, "S01E01, S01E02, S01E04, S01E05 stop downloading too");
	assert_contains(reply.body, "name=\"replace_others\"");
	assert_csp_clean(reply.body);
	save_page("replace-offer-pack.html", reply.body);
	g_free(swap);
	{
		g_autofree gchar *grab = field_of(reply.body, "replace");
		g_autofree gchar *digest = field_of(reply.body, "replace_digest");

		swap = g_strdup_printf("&replace=%s&replace_digest=%s&replace_ok=yes", grab, digest);
	}
	axis_test_reply_clear(&reply);
	/* Scenario: Without the tick for the others. */
	reply = marsh_post(&m, MARSH_E03, swap);
	g_assert_cmpuint(reply.status, ==, 409);
	assert_contains(reply.body, "he download in flight also holds S01E01, S01E02, S01E04, S01E05, which this release "
		"does not: tick the box that says they stop downloading too. Nothing was ended and nothing was sent.");
	axis_test_reply_clear(&reply);
	g_assert_true(axis_test_download_fake_has(fx->fake, ref));
	text = marsh_open(&m);
	g_assert_cmpstr(text, ==, "S01E01,S01E02,S01E03,S01E04,S01E05");
	g_clear_pointer(&text, g_free);
	/* With both ticks. */
	text = g_strdup_printf("%s&replace_others=yes", swap);
	reply = marsh_post(&m, MARSH_E03, text);
	g_assert_cmpuint(reply.status, ==, 303);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&text, g_free);
	g_assert_false(axis_test_download_fake_has(fx->fake, ref));
	g_assert_cmpuint(axis_test_download_fake_n_items(fx->fake), ==, 1);
	text = marsh_open(&m);
	g_assert_cmpstr(text, ==, "S01E03");
	g_clear_pointer(&text, g_free);
	text = q(fx, "SELECT close_reason || '|' || removal FROM series_grab WHERE release_name = '" MARSH_PACK "'");
	g_assert_cmpstr(text, ==, "replaced|done");
	g_clear_pointer(&text, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);
	/* The other episodes are wanted again: a search of one of them is not
	 * skipped as in flight. */
	memset(&report, 0, sizeof(report));
	g_assert_true(axis_tv_run_search(fx->tv, m.series, AXIS_TV_SCOPE_EPISODE, episode_id(fx, m.series, 1, 4),
		"scheduled", NULL, NULL, &report, &error));
	g_assert_no_error(error);
	g_assert_null(report.skipped);
	axis_test_db_exec(fx->t->db, "DELETE FROM job WHERE kind = 'tv.search'");

	/* Scenario: It has completed.  No replace is offered, and the page says
	 * that it has arrived. */
	g_free(ref);
	ref = marsh_downloading(&m, MARSH_E03);
	{
		g_autofree gchar *place = g_build_filename(fx->disk, "downloads", "complete", MARSH_E03, NULL);
		g_autofree gchar *file = g_build_filename(place, "marsh-lantern.mkv", NULL);
		guint removes = axis_test_download_fake_count(fx->fake, "remove", NULL);

		reply = marsh_page(&m, MARSH_E03_DISC);
		g_free(swap);
		{
			g_autofree gchar *grab = field_of(reply.body, "replace");
			g_autofree gchar *digest = field_of(reply.body, "replace_digest");

			swap = g_strdup_printf("&replace=%s&replace_digest=%s&replace_ok=yes", grab, digest);
		}
		axis_test_reply_clear(&reply);
		axis_test_storage_mkdir(place);
		axis_test_storage_write(file, "an episode");
		/* (The fixture maps the client's /data/downloads to the disk.) */
		axis_test_download_fake_set(fx->fake, ref, "completed", 4000000000, "/data/downloads/complete/" MARSH_E03, NULL);
		poll(fx);
		reply = marsh_page(&m, MARSH_E03_DISC);
		assert_contains(reply.body, "cannot be grabbed");
		assert_contains(reply.body, MARSH_E03 " has arrived");
		assert_contains(reply.body, "href=\"/downloads\"");
		assert_lacks(reply.body, "Replace the download in flight with this one");
		axis_test_reply_clear(&reply);
		/* It completed while the person confirmed: the post ends nothing. */
		reply = marsh_post(&m, MARSH_E03_DISC, swap);
		g_assert_cmpuint(reply.status, ==, 409);
		assert_contains(reply.body, "The download in flight changed after you opened this page");
		axis_test_reply_clear(&reply);
		g_assert_true(axis_test_download_fake_has(fx->fake, ref));
		g_assert_cmpuint(axis_test_download_fake_count(fx->fake, "remove", NULL), ==, removes);
		g_assert_true(g_file_test(file, G_FILE_TEST_EXISTS));
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE release_name = '" MARSH_E03 "' AND closed_at IS"
			" NULL"), ==, 1);
		g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_grab WHERE release_name = '" MARSH_E03_DISC "' AND closed_at"
			" IS NULL"), ==, 0);
	}
	g_free(m.page);
	g_free(m.grab);
}

/* ── The series' own mark, and the monitoring of a whole series ─── */

/* The line under a series' title: its mark and its sentence. */
static gchar *
state_line(const gchar *body)
{
	const gchar *line = strstr(body, "film-state-line");
	const gchar *end = line != NULL ? strstr(line, "</div>") : NULL;

	if (end == NULL)
		g_error("expected the series' state line in:\n%s", body);
	return g_strndup(line, (gsize)(end - line));
}

static gchar *
monitoring_links(Fixture *fx)
{
	return q(fx, "SELECT (SELECT count(*) FROM library_file) || '|' || (SELECT count(*) FROM series_file) || '|' ||"
		" (SELECT string_agg(file_id || ':' || episode_id, ',' ORDER BY file_id, episode_id) FROM series_file_episode)");
}

/* The whole-series form posted by an administrator from the series' page;
 * the Location it answers with. */
static gchar *
monitoring_post(
	Fixture     *fx,
	gint64       series,
	const gchar *form
){
	g_autofree gchar *page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/monitor", series);
	AxisTestReply reply = post_as(fx, fx->admin, page, action, form);
	gchar *location;

	g_assert_cmpuint(reply.status, ==, 303);
	location = g_strdup(axis_test_reply_header(&reply, "Location"));
	axis_test_reply_clear(&reply);
	g_assert_nonnull(location);
	return location;
}

/*
 * Spec (tv, series-monitoring-control): A series is "Not monitored" only
 * when nothing of it is monitored; The monitoring of a whole series is
 * changed in one place.  The series that read "Not monitored": both seasons
 * monitored and wholly in the library, the specials not monitored, new
 * seasons monitored.
 */
static void
test_series_monitoring(Fixture *fx, gconstpointer data)
{
	gint64 series = add_series(fx, "202", AXIS_TV_MONITOR_ALL);
	g_autofree gchar *page = g_strdup_printf("/series/%" G_GINT64_FORMAT, series);
	g_autofree gchar *action = g_strdup_printf("/series/%" G_GINT64_FORMAT "/monitor", series);
	g_autofree gchar *links = NULL;
	g_autofree gchar *now = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *n = NULL;
	AxisTestReply reply;
	gint season;
	gint number;

	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET air_date = DATE '2024-05-01' WHERE season_number = 2"
		" AND number > 2");
	for (season = 1; season <= 2; season++) {
		for (number = 1; number <= 4; number++) {
			g_autofree gchar *name = g_strdup_printf("Harbour Lights (2024)/Season %02d/S%02dE%02d.mkv", season, season,
				number);

			in_library(fx, series, episode_id(fx, series, season, number), name, "bluray_1080p");
		}
	}
	links = monitoring_links(fx);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_season WHERE monitored"), ==, 2);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE monitor_new AND monitor_choice = 'all'"), ==, 1);

	/* Its own page: in the library, with what it does about new episodes;
	 * the specials' row alone says "Not monitored". */
	reply = get_as(fx, fx->admin, page);
	g_assert_cmpuint(reply.status, ==, 200);
	line = state_line(reply.body);
	assert_contains(line, "film-mark-in_library");
	assert_lacks(line, "film-mark-unmonitored");
	assert_lacks(line, "Not monitored");
	assert_contains(line, "Every monitored episode that has aired is in the library.");
	assert_contains(line, "New seasons and episodes are monitored as they are announced.");
	g_clear_pointer(&line, g_free);
	assert_contains(reply.body, "8 of 8 aired episodes in the library");
	/* The card: how it stands in words, and one form for all of it. */
	assert_contains(reply.body, "id=\"monitoring\"");
	assert_contains(reply.body, "2 of 2 seasons monitored; the specials are not monitored; new seasons are monitored "
		"when they are announced.");
	assert_contains(reply.body, "name=\"whole\"");
	assert_contains(reply.body, "Monitor every season");
	assert_contains(reply.body, "Monitor the latest season only");
	assert_contains(reply.body, "Stop monitoring every season");
	assert_contains(reply.body, "Monitor new seasons and episodes as they are announced");
	assert_contains(reply.body, "With every season, monitor the specials too");
	assert_contains(reply.body, "Save monitoring of the whole series");
	assert_contains(reply.body, "No file is deleted, moved or unlinked");
	assert_lacks(reply.body, "Monitored as in Sonarr");
	/* (The seasons' own ticks stay.) */
	assert_contains(reply.body, "Save monitoring of this season");
	g_assert_true(strstr(reply.body, "id=\"monitoring\"") < strstr(reply.body, "id=\"season-2\""));
	assert_csp_clean(reply.body);
	axis_test_reply_clear(&reply);
	/* A viewer reads how it stands, and has no form. */
	reply = get_as(fx, fx->viewer, page);
	assert_contains(reply.body, "2 of 2 seasons monitored");
	assert_lacks(reply.body, "Save monitoring of the whole series");
	assert_lacks(reply.body, "name=\"whole\"");
	axis_test_reply_clear(&reply);
	/* The list says the same of it, in both views. */
	reply = get_as(fx, fx->admin, "/series");
	assert_contains(reply.body, "Harbour Lights");
	assert_contains(reply.body, "film-mark-in_library");
	assert_lacks(reply.body, "film-mark-unmonitored");
	axis_test_reply_clear(&reply);
	reply = get_as(fx, fx->admin, "/series?view=table");
	assert_contains(reply.body, "film-mark-in_library");
	assert_lacks(reply.body, "film-mark-unmonitored");
	axis_test_reply_clear(&reply);

	/* A viewer cannot change it, with the token; an administrator cannot
	 * without it (a forged form); a choice that is none is refused. */
	reply = post_as(fx, fx->viewer, page, action, "whole=none");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, NULL, action, "whole=none");
	g_assert_cmpuint(reply.status, ==, 403);
	axis_test_reply_clear(&reply);
	reply = post_as(fx, fx->admin, page, action, "whole=sideways&new=yes");
	g_assert_cmpuint(reply.status, ==, 400);
	assert_contains(reply.body, "Choose what to do with the seasons.");
	axis_test_reply_clear(&reply);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE monitored"), ==, 8);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE monitor_new AND monitor_choice = 'all'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'monitored'"), ==, 0);

	/* The box alone, cleared: new seasons are not monitored; the seasons,
	 * the episodes and the choice are as they were, and the series is still
	 * in the library. */
	location = monitoring_post(fx, series, "whole=keep");
	g_assert_true(g_str_has_suffix(location, "done=monitoring_saved&at=monitoring#monitoring"));
	reply = follow(fx, location);
	assert_notice_at(reply.body, "monitoring", "The monitoring of the whole series was saved.");
	assert_contains(reply.body, "2 of 2 seasons monitored; the specials are not monitored; new seasons are not "
		"monitored until you choose them.");
	line = state_line(reply.body);
	assert_contains(line, "film-mark-in_library");
	assert_lacks(line, "New seasons and episodes are monitored");
	g_clear_pointer(&line, g_free);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE NOT monitor_new AND monitor_choice = 'all'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE monitored"), ==, 8);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'monitored' AND by_user = 'root' AND text LIKE"
		" 'New seasons are no longer monitored when they are announced by root.%%'"), ==, 1);

	/* Stop monitoring every season: now, and only now, it is not monitored.
	 * The history says who, and its files are where they were. */
	location = monitoring_post(fx, series, "whole=none");
	g_assert_true(g_str_has_suffix(location, "done=monitoring&at=monitoring#monitoring"));
	reply = follow(fx, location);
	assert_notice_at(reply.body, "monitoring", "The monitoring of the whole series was changed.");
	assert_contains(reply.body, "No file was touched: an episode that has a file keeps it, monitored or not.");
	line = state_line(reply.body);
	assert_contains(line, "film-mark-unmonitored");
	assert_contains(line, ">Not monitored<");
	assert_contains(line, "No season is monitored, and new seasons are not: Axis searches for nothing of it. 8 episodes "
		"are in the library.");
	g_clear_pointer(&line, g_free);
	assert_contains(reply.body, "0 of 2 seasons monitored; the specials are not monitored; new seasons are not monitored "
		"until you choose them.");
	assert_contains(reply.body, "No season monitored any more, the specials neither by root (2 seasons and 8 episodes "
		"changed).");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE monitored"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_season WHERE monitored"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE NOT monitor_new AND monitor_choice = 'none'"), ==, 1);
	reply = get_as(fx, fx->admin, "/series");
	assert_contains(reply.body, "film-mark-unmonitored");
	assert_lacks(reply.body, "film-mark-in_library");
	axis_test_reply_clear(&reply);
	now = monitoring_links(fx);
	g_assert_cmpstr(now, ==, links);
	g_clear_pointer(&now, g_free);
	/* The same again: nothing changed, and the page says so. */
	location = monitoring_post(fx, series, "whole=none");
	g_assert_true(g_str_has_suffix(location, "done=monitoring_same&at=monitoring#monitoring"));
	reply = follow(fx, location);
	assert_notice_at(reply.body, "monitoring", "Nothing changed: the series was monitored that way already.");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'monitored'"), ==, 2);

	/* The latest season only, and new seasons again: one season left out,
	 * the rest in the library. */
	location = monitoring_post(fx, series, "whole=latest&new=yes");
	g_assert_true(g_str_has_suffix(location, "done=monitoring&at=monitoring#monitoring"));
	reply = follow(fx, location);
	assert_contains(reply.body, "1 of 2 seasons monitored; the specials are not monitored; new seasons are monitored "
		"when they are announced.");
	line = state_line(reply.body);
	assert_contains(line, "film-mark-in_library");
	assert_lacks(line, "Not monitored");
	g_clear_pointer(&line, g_free);
	assert_contains(reply.body, "Only the latest season (season 2) monitored by root (1 season and 4 episodes changed).");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);
	n = q(fx, "SELECT string_agg(DISTINCT season_number::text, ',') FROM series_episode WHERE monitored");
	g_assert_cmpstr(n, ==, "2");
	g_clear_pointer(&n, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE monitor_new AND monitor_choice = 'latest'"), ==, 1);
	reply = get_as(fx, fx->admin, "/series");
	assert_contains(reply.body, "film-mark-in_library");
	assert_lacks(reply.body, "film-mark-unmonitored");
	axis_test_reply_clear(&reply);

	/* A series migrated says "As in Sonarr" until a person chooses for the
	 * whole of it: the box alone leaves that; a choice replaces it. */
	axis_test_db_exec(fx->t->db, "UPDATE series SET monitor_choice = 'brought'");
	reply = get_as(fx, fx->admin, page);
	assert_contains(reply.body, "Monitored as in Sonarr: each season and episode as it was there");
	axis_test_reply_clear(&reply);
	location = monitoring_post(fx, series, "whole=keep&new=yes");
	g_assert_true(g_str_has_suffix(location, "done=monitoring_same&at=monitoring#monitoring"));
	g_clear_pointer(&location, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE monitor_choice = 'brought'"), ==, 1);
	/* Every season: the specials are left as they are. */
	location = monitoring_post(fx, series, "whole=all&new=yes");
	g_assert_true(g_str_has_suffix(location, "done=monitoring&at=monitoring#monitoring"));
	reply = follow(fx, location);
	assert_lacks(reply.body, "Monitored as in Sonarr: each season");
	assert_contains(reply.body, "2 of 2 seasons monitored; the specials are not monitored; new seasons are monitored "
		"when they are announced.");
	assert_contains(reply.body, "Every season monitored, the specials left as they were by root (1 season and 4 episodes "
		"changed). What to monitor was \"As in Sonarr\" until now.");
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series WHERE monitor_choice = 'all'"), ==, 1);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE monitored AND season_number > 0"), ==, 8);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE monitored AND season_number = 0"), ==, 0);

	/* With the specials' box: the special, aired and without a file, is
	 * wanted from now on; its wait is cleared and nothing is queued at once. */
	axis_test_db_exec(fx->t->db, "UPDATE series_episode SET air_date = DATE '2024-02-01', next_search_at = now() +"
		" interval '1 day' WHERE season_number = 0");
	location = monitoring_post(fx, series, "whole=all&specials=yes&new=yes");
	g_assert_true(g_str_has_suffix(location, "done=monitoring&at=monitoring#monitoring"));
	reply = follow(fx, location);
	assert_contains(reply.body, "2 of 2 seasons monitored; the specials are monitored; new seasons are monitored when "
		"they are announced.");
	assert_contains(reply.body, "Every season monitored, the specials too by root (1 season and 1 episode changed).");
	line = state_line(reply.body);
	assert_contains(line, "film-mark-wanted");
	g_clear_pointer(&line, g_free);
	axis_test_reply_clear(&reply);
	g_clear_pointer(&location, g_free);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE monitored"), ==, 9);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_episode WHERE next_search_at IS NOT NULL"), ==, 0);
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM job WHERE kind = 'tv.search'"), ==, 0);

	/* Every change with the administrator who made it; not one file or link
	 * changed through all of it. */
	g_assert_cmpint(qi(fx, "SELECT count(*) FROM series_event WHERE kind = 'monitored' AND by_user = 'root'"), ==, 5);
	now = monitoring_links(fx);
	g_assert_cmpstr(now, ==, links);
	g_assert_cmpuint(logged("monitoring changed"), >=, 5);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	AXIS_TEST_ADD_DB_CASE("/web/tv/series-monitoring", Fixture, NULL, test_series_monitoring);
	AXIS_TEST_ADD_DB_CASE("/web/tv/replace-grab", Fixture, NULL, test_replace_grab);
	AXIS_TEST_ADD_DB_CASE("/web/tv/anime-kind", Fixture, NULL, test_anime_kind);
	AXIS_TEST_ADD_DB_CASE("/web/tv/anime-add", Fixture, NULL, test_anime_add);
	AXIS_TEST_ADD_DB_CASE("/web/tv/block-release", Fixture, NULL, test_block_release);
	AXIS_TEST_ADD_DB_CASE("/web/tv/block-release-faults", Fixture, NULL, test_block_release_faults);
	AXIS_TEST_ADD_DB_CASE("/web/tv/brought-hold", Fixture, NULL, test_brought_hold);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-by-name", Fixture, NULL, test_place_by_name);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/names-on-change", Fixture, NULL, test_names_on_change);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/new-season-default", Fixture, NULL, test_new_season_default);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/new-season-only", Fixture, NULL, test_new_season_only);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-imported-kept", Fixture, NULL, test_place_imported_kept);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-two-files", Fixture, NULL, test_place_two_files);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-same-target", Fixture, NULL, test_place_same_target);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/names-two-files", Fixture, NULL, test_names_two_files);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-digest", Fixture, NULL, test_place_digest);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-manual-import", Fixture, NULL, test_place_manual_import);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-single-alone", Fixture, NULL, test_place_single_alone);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-single-refused", Fixture, NULL, test_place_single_refused);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-digest-state", Fixture, NULL, test_place_digest_state);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/place-chain", Fixture, NULL, test_place_chain);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/names-busy", Fixture, NULL, test_names_busy);
	AXIS_TEST_ADD_DB_CASE("/web/tv/links", Fixture, NULL, test_links);
	AXIS_TEST_ADD_DB_CASE("/web/tv/links-stray", Fixture, NULL, test_links_stray);
	AXIS_TEST_ADD_DB_CASE("/web/tv/links-ticks", Fixture, NULL, test_links_ticks);
	AXIS_TEST_ADD_DB_CASE("/web/tv/links-scope", Fixture, NULL, test_links_scope);
	AXIS_TEST_ADD_DB_CASE("/web/tv/hand-link-kept", Fixture, NULL, test_hand_link_kept);
	AXIS_TEST_ADD_DB_CASE("/web/tv/place-row-status", Fixture, NULL, test_place_row_status);
	AXIS_TEST_ADD_DB_CASE("/web/tv/import-open-partial", Fixture, NULL, test_import_open_partial);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/title-choice", Fixture, NULL, test_numbering_title_choice);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/viewer-season", Fixture, NULL, test_numbering_viewer_season);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/viewer", Fixture, NULL, test_numbering_viewer);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/pages", Fixture, NULL, test_numbering_pages);
	AXIS_TEST_ADD_DB_CASE("/web/tv/numbering/waits", Fixture, NULL, test_numbering_waits);
	AXIS_TEST_ADD_DB_CASE("/web/tv/record-id-changes", Fixture, NULL, test_record_id_changes);
	AXIS_TEST_ADD_DB_CASE("/web/tv/episode-sources", Fixture, NULL, test_episode_sources);
	AXIS_TEST_ADD_DB_CASE("/web/tv/no-series-follows-tvdb", Fixture, NULL, test_no_series_follows_tvdb);
	AXIS_TEST_ADD_DB_CASE("/web/tv/merge-then-refresh", Fixture, NULL, test_merge_then_refresh);
	AXIS_TEST_ADD_DB_CASE("/web/tv/repeated-clicks", Fixture, NULL, test_repeated_clicks);
	AXIS_TEST_ADD_DB_CASE("/web/tv/activity", Fixture, NULL, test_activity_lines);
	AXIS_TEST_ADD_DB_CASE("/web/tv/pages", Fixture, NULL, test_pages);
	AXIS_TEST_ADD_DB_CASE("/web/tv/folder-titles", Fixture, NULL, test_folder_titles);
	AXIS_TEST_ADD_DB_CASE("/web/tv/actions", Fixture, NULL, test_actions);
	AXIS_TEST_ADD_DB_CASE("/web/tv/attention", Fixture, NULL, test_attention);
	AXIS_TEST_ADD_DB_CASE("/web/tv/manual-search", Fixture, NULL, test_manual_search);
	AXIS_TEST_ADD_DB_CASE("/web/tv/manual-sort", Fixture, NULL, test_manual_sort);
	AXIS_TEST_ADD_DB_CASE("/web/tv/force-grab", Fixture, NULL, test_force_grab);
	AXIS_TEST_ADD_DB_CASE("/web/tv/force-other-season", Fixture, NULL, test_force_other_season);
	AXIS_TEST_ADD_DB_CASE("/web/tv/hand-files-controls", Fixture, NULL, test_hand_files_controls);
	AXIS_TEST_ADD_DB_CASE("/web/tv/module-off", Fixture, NULL, test_module_off);
	AXIS_TEST_ADD_DB_CASE("/web/tv/empty-list", Fixture, NULL, test_empty_list);
	return g_test_run();
}
