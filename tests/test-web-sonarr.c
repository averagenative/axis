/*
 * test-web-sonarr.c - "Bring over from Sonarr", slice S1: a Sonarr's
 * connection, its reading (the job arr.read on a real queue) and its preview,
 * with the real shell on an ephemeral loopback port, a stand-in Sonarr, a
 * stand-in details provider and a stand-in TheTVDB on loopback, a real
 * database, and a real library folder for series with its index
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Nothing here brings a series over: slice S1 ends at the preview, and every
 * test that reads or previews checks that the library is as it was.
 *
 * What is proved (openspec/specs/arr-migration/spec.md, and the
 * bring-over scenarios Only GET; Never shown again; A viewer; Not Radarr, for
 * the second app; Nothing changed; Skipped, with reasons; Found at its path;
 * Not seen; Linked elsewhere; An incomplete title; Thousands of titles):
 *   connections/app-kept       a connection is of one app for good: a Radarr
 *                              answering where Sonarr was expected is refused,
 *                              and the reverse; nothing but system/status is
 *                              asked of an address that does not name the
 *                              app; a save with the other app writes nothing
 *   connections/viewer         a viewer is refused every page and post
 *   reading/read-and-preview   one of everything: the monitoring of each
 *                              season and episode carried into the plan, the
 *                              files found and not found, each skip reason,
 *                              the series that would be migrated with their
 *                              profile, folder and numbering
 *   reading/no-tvdb-key        no reading and no series without a TheTVDB key
 *   reading/paging-and-odd     the queue page by page, a series whose list
 *                              cannot be read incomplete alone, odd entries
 *                              counted, the lists of the preview paged
 *   reading/limits             the count of series, the bytes, the deadline
 *                              and the pace; a reading that fails half-way
 *                              leaves no row behind
 *   reading/thousand           1,000 series with 60,000 episodes and as many
 *                              episode files read and paged within a stated
 *                              time
 *   run/refused                no run of a Sonarr is ever queued
 *
 * With AXIS_TEST_ARR_PAGES set to a directory, the pages are also saved
 * there, for looking at in a browser.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <htmx-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "arr/axis-arr.h"
#include "arr/axis-arr-private.h"
#include "axis-test-case.h"
#include "axis-test-db.h"
#include "axis-test-jobs.h"
#include "axis-test-library.h"
#include "axis-test-meta-fixture.h"
#include "axis-test-plex.h"
#include "axis-test-radarr.h"
#include "axis-test-sonarr.h"
#include "axis-test-tvdb.h"
#include "axis-test-web.h"
#include "core/axis-data-dir.h"
#include "core/axis-module-state.h"
#include "jobs/axis-job-queue.h"
#include "jobs/axis-job-registry.h"
#include "meta/axis-meta.h"
#include "meta/axis-meta-tvdb.h"
#include "quality/axis-quality-store.h"
#include "web/axis-session-store.h"
#include "web/axis-web.h"
#include "web/axis-web-arr.h"
#include "web/axis-web-auth.h"
#include "web/axis-web-meta.h"
#include "web/axis-web-quality.h"
#include "web/axis-web-settings.h"

/* Made up: no Sonarr and no Radarr has these keys. */
#define KEY        "sonarr-key-3c9e1a7f5d2b4860"
#define OTHER_KEY  "sonarr-key-0000cccc1111dddd"
#define RADARR_KEY "radarr-key-7f3a9c1e5b2d4f60"

typedef struct
{
	AxisTestLibrary     *t;
	gchar               *disk;
	gchar               *shows_dir;
	gint64               volume_id;
	gint64               root_id;
	AxisDataDir         *data;
	AxisSettings        *settings;
	AxisWebSettings     *web_settings;
	AxisTestMetaFixture *fa;
	AxisTestTvdb        *tvdb;
	AxisMeta            *meta;
	AxisWebMeta         *meta_ui;
	AxisQualityStore    *quality;
	AxisWebQuality      *quality_ui;
	AxisTestSonarr      *sonarr;
	AxisTestRadarr      *radarr;
	AxisArr             *arr;
	AxisWebArr          *ui;
	AxisJobQueue        *queue;
	AxisModuleRegistry  *registry;
	HtmxServer          *server;
	AxisWebAuth         *auth;
	AxisWeb             *web;
	SoupSession         *session;
	guint16              port;
	gchar               *admin;
	gchar               *viewer;
} Fixture;

/* ── The stand-in details provider (in TMDb's place) ────────────── */

/* The series it knows by their TheTVDB identifier.  It does not know 81065,
 * and does not answer for 81091. */
static const struct
{
	const gchar *tvdb;
	const gchar *id;
	const gchar *title;
	gint         year;
} known[] = {
	{ "81061", "7061", "Gullwing Ferry", 2025 },
	{ "81062", "7062", "The Morning Tide Report", 2019 },
	{ "81063", "7063", "Paper Crane Academy", 2014 },
	{ "81066", "7066", "Ash and Ember Street", 1996 },
	{ "81067", "7067", "Tin Sparrow Road", 2001 },
	{ "81068", "7068", "Low Tide Radio Hour", 2015 },
	{ "81070", "7070", "Marrow Lane Mysteries", 1990 },
	{ "81073", "7073", "Copper Kettle Lane", 2003 },
	{ NULL, NULL, NULL, 0 }
};

static void
provider(
	const AxisTestPlexRequest *request,
	AxisTestPlexAnswer        *answer,
	gpointer                   user_data
){
	g_autoptr(GHashTable) query = request->query != NULL ?
		g_uri_parse_params(request->query, -1, "&", G_URI_PARAMS_NONE, NULL) : NULL;
	const gchar *scheme = query != NULL ? g_hash_table_lookup(query, "scheme") : NULL;
	const gchar *value = query != NULL ? g_hash_table_lookup(query, "value") : NULL;
	g_autoptr(GString) json = g_string_new("{\"results\":[");
	guint i;

	if (!g_str_has_prefix(request->path, "/find/series") || scheme == NULL || value == NULL) {
		answer->status = 404;
		return;
	}
	if (strcmp(value, "81091") == 0) {
		answer->status = 500;
		return;
	}
	for (i = 0; known[i].tvdb != NULL; i++) {
		if (strcmp(scheme, "tvdb") == 0 && strcmp(known[i].tvdb, value) == 0)
			g_string_append_printf(json, "{\"id\":\"%s\",\"kind\":\"series\",\"title\":\"%s\",\"year\":%d,"
				"\"ids\":{\"tvdb\":\"%s\"}}", known[i].id, known[i].title, known[i].year, known[i].tvdb);
	}
	g_string_append(json, "]}");
	answer->status = 200;
	answer->content_type = g_strdup("application/json");
	answer->body = g_bytes_new(json->str, json->len);
}

/* TheTVDB's aired order lists series @tvdb: one episode is enough to say so. */
static void
tvdb_lists(
	Fixture     *fx,
	const gchar *tvdb
){
	g_autofree gchar *path = g_strdup_printf("/v4/series/%s/episodes/official/eng", tvdb);
	g_autofree gchar *json = g_strdup_printf("{\"status\":\"success\",\"data\":{\"series\":{\"id\":%s},\"episodes\":["
		"{\"id\":%s001,\"seriesId\":%s,\"name\":\"A Made Up Episode\",\"aired\":\"2025-01-06\",\"runtime\":24,"
		"\"seasonNumber\":1,\"number\":1,\"absoluteNumber\":1}]},\"links\":{\"prev\":null,\"next\":null}}", tvdb, tvdb,
		tvdb);

	axis_test_tvdb_set_answer(fx->tvdb, path, "page=0", json);
}

/* ── The fixture ────────────────────────────────────────────────── */

static void
write_file(
	Fixture     *fx,
	const gchar *folder,
	const gchar *name,
	const gchar *content
){
	g_autofree gchar *dir = g_build_filename(fx->shows_dir, folder, NULL);

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	axis_test_write_named(dir, name, content);
}

/* The tuning the tests read under: the stand-ins are on loopback, so the
 * pace of the requests for each series is not kept (reading/limits keeps
 * it). */
static void
tune(
	Fixture       *fx,
	AxisArrTuning *tuning
){
	memset(tuning, 0, sizeof(*tuning));
	tuning->request_timeout_ms = 3000;
	tuning->list_timeout_ms = 20000;
	tuning->series_timeout_ms = 20000;
	tuning->lookup_wait_ms = 5000;
	tuning->requests_per_second = 100000;
	tuning->provider = "fixture";
}

static void
fixture_setup(Fixture *fx, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisScanResult) scan = NULL;
	g_autofree gchar *path = NULL;
	AxisMetaTuning meta_tuning;
	AxisArrTuning tuning;

	memset(fx, 0, sizeof(*fx));
	axis_test_log_init();
	axis_test_log_reset();
	axis_session_store_set_idle_seconds(30 * 24 * 3600);
	fx->t = axis_test_library_new(16);
	if (fx->t == NULL)
		return;
	fx->disk = axis_test_library_disk(fx->t, "disk1");
	fx->volume_id = axis_test_library_add_volume(fx->t, fx->disk, "Disk one");
	fx->root_id = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "shows", AXIS_MEDIA_TV);
	fx->shows_dir = g_build_filename(fx->disk, "shows", NULL);
	/* What is on the disk, as Axis sees it below its library folder. */
	write_file(fx, "Gullwing Ferry/Season 1", "Gullwing Ferry - S01E01 - Low Water.mkv", "lowwater1");
	write_file(fx, "Gullwing Ferry/Season 1", "Gullwing Ferry - S01E02-E03 - The Ropes.mkv", "ropes12");
	write_file(fx, "The Morning Tide Report/Season 2019", "The Morning Tide Report - 2019-03-04.mkv", "short");
	write_file(fx, "Copper Kettle Lane/Season 1", "Copper Kettle Lane - S01E01.mkv", "kettle123");
	scan = axis_test_library_scan(fx->t, fx->root_id);
	while (g_main_context_iteration(NULL, FALSE))
		;
	path = g_build_filename(fx->t->dir, "data", NULL);
	fx->data = axis_data_dir_open(path, &error);
	g_assert_no_error(error);

	fx->settings = axis_settings_new();
	fx->web_settings = axis_web_settings_new(fx->settings, fx->t->pool);
	fx->meta = axis_meta_new(fx->t->pool, fx->settings, fx->data, &error);
	g_assert_no_error(error);
	fx->fa = axis_test_meta_fixture_new("fixture", 50.0, 10);
	axis_test_plex_set_handler(fx->fa->api, provider, NULL);
	g_assert_true(axis_meta_add_provider(fx->meta, &fx->fa->info, &error));
	/* TheTVDB itself, at a stand-in: the numbering a series from Sonarr
	 * would follow. */
	fx->tvdb = axis_test_tvdb_new();
	g_assert_true(axis_meta_add_provider(fx->meta, axis_test_tvdb_info(fx->tvdb), &error));
	g_assert_no_error(error);
	memset(&meta_tuning, 0, sizeof(meta_tuning));
	meta_tuning.request_timeout_ms = 3000;
	meta_tuning.background_ms = 8000;
	axis_meta_set_tuning(fx->meta, &meta_tuning);
	fx->meta_ui = axis_web_meta_new(fx->meta);
	fx->quality = axis_quality_store_new(fx->t->pool, fx->settings);
	fx->quality_ui = axis_web_quality_new(fx->quality);

	fx->sonarr = axis_test_sonarr_new(KEY);
	fx->radarr = axis_test_radarr_new(RADARR_KEY);
	fx->arr = axis_arr_new(fx->t->pool, fx->meta);
	tune(fx, &tuning);
	axis_arr_set_tuning(fx->arr, &tuning);
	fx->ui = axis_web_arr_new(fx->arr);

	fx->registry = axis_module_registry_new();
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_settings_get_module(), fx->web_settings,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_meta_get_module(fx->meta_ui), fx->meta_ui,
		&error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_quality_get_module(), fx->quality_ui, &error));
	g_assert_true(axis_module_registry_register(fx->registry, axis_web_arr_get_module(), fx->ui, &error));
	g_assert_no_error(error);
	g_assert_true(axis_settings_add_modules(fx->settings, fx->registry, &error));
	g_assert_true(axis_settings_load(fx->settings, fx->t->pool, &error));
	g_assert_true(axis_module_state_load(fx->registry, fx->t->pool, &error));
	g_assert_no_error(error);
	axis_web_settings_sync(fx->web_settings);
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_key", AXIS_TEST_META_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, "metadata.fixture_rate", "50", &error));
	/* Axis has a TheTVDB key (reading/no-tvdb-key takes it away). */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_RATE, "50", &error));
	g_assert_no_error(error);

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
	fx->session = soup_session_new_with_options("timeout", 300, "idle-timeout", 300, NULL);
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
	axis_arr_stop(fx->arr);
	if (fx->queue != NULL)
		g_assert_true(axis_job_queue_stop(fx->queue, 10000));
	axis_web_arr_stop(fx->ui);
	axis_web_meta_stop(fx->meta_ui);
	axis_web_quality_stop(fx->quality_ui);
	axis_module_registry_stop_all(fx->registry);
	axis_web_settings_stop(fx->web_settings);
	axis_meta_stop(fx->meta);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_clear_object(&fx->queue);
	axis_web_free(fx->web);
	axis_web_auth_free(fx->auth);
	g_object_unref(fx->server);
	axis_module_registry_free(fx->registry);
	axis_web_arr_free(fx->ui);
	axis_arr_unref(fx->arr);
	axis_test_sonarr_free(fx->sonarr);
	axis_test_radarr_free(fx->radarr);
	axis_web_quality_free(fx->quality_ui);
	axis_quality_store_unref(fx->quality);
	axis_web_meta_free(fx->meta_ui);
	axis_meta_unref(fx->meta);
	axis_web_settings_free(fx->web_settings);
	axis_test_meta_fixture_free(fx->fa);
	axis_test_tvdb_free(fx->tvdb);
	axis_settings_unref(fx->settings);
	axis_data_dir_unref(fx->data);
	g_free(fx->disk);
	g_free(fx->shows_dir);
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

/* A form posted from a page the user has open: its CSRF token from there. */
static AxisTestReply
post_as(
	Fixture     *fx,
	const gchar *cookie,
	const gchar *page,
	const gchar *path,
	const gchar *form
){
	AxisTestReply opened = get_as(fx, cookie, page);
	g_autofree gchar *token = axis_test_web_csrf_token(opened.body);
	g_autofree gchar *escaped = token != NULL ? g_uri_escape_string(token, NULL, TRUE) : g_strdup("");
	g_autofree gchar *body = g_strdup_printf("_csrf=%s%s%s", escaped, form != NULL && form[0] != '\0' ? "&" : "",
		form != NULL ? form : "");

	axis_test_reply_clear(&opened);
	return axis_test_web_request(fx->session, fx->port, "POST", path, cookie, NULL, body);
}

/* A page as a person reads it: no break opportunities, no escapes. */
static gchar *
plain(const gchar *html)
{
	g_autofree gchar *unbroken = axis_test_web_unbroken(html);
	g_autoptr(GString) text = g_string_new(unbroken);

	g_string_replace(text, "&#39;", "'", 0);
	g_string_replace(text, "&apos;", "'", 0);
	g_string_replace(text, "&#x27;", "'", 0);
	g_string_replace(text, "&quot;", "\"", 0);
	g_string_replace(text, "&amp;", "&", 0);
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static void
assert_contains(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *text = haystack != NULL ? plain(haystack) : NULL;

	if (text == NULL || strstr(text, needle) == NULL)
		g_error("expected to find '%s' in:\n%s", needle, haystack != NULL ? haystack : "(null)");
}

static void
assert_lacks(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *text = haystack != NULL ? plain(haystack) : NULL;

	if (text != NULL && strstr(text, needle) != NULL)
		g_error("did not expect '%s' in:\n%s", needle, haystack);
}

static void
assert_csp_clean(const gchar *body)
{
	g_autoptr(GRegex) inline_script = g_regex_new("<script(?![^>]*\\ssrc=)", 0, 0, NULL);

	assert_lacks(body, "style=\"");
	assert_lacks(body, "<style");
	g_assert_false(g_regex_match(inline_script, body, 0, NULL));
	assert_lacks(body, "javascript:");
}

static void
save_page(
	const gchar *name,
	const gchar *body
){
	const gchar *dir = g_getenv("AXIS_TEST_ARR_PAGES");
	g_autofree gchar *path = NULL;

	if (dir == NULL || dir[0] == '\0')
		return;
	path = g_build_filename(dir, name, NULL);
	g_assert_true(g_file_set_contents(path, body, -1, NULL));
}

/* The main part of a page: what is below the navigation (which names both
 * apps nowhere, but the page's own words are what is checked). */
static const gchar *
main_part(const gchar *body)
{
	const gchar *start = strstr(body, "<main");

	return start != NULL ? start : body;
}

/* A connection made through the layer: of @app, to @address, with @key. */
static gint64
add_source(
	Fixture     *fx,
	const gchar *app,
	const gchar *name,
	const gchar *address,
	const gchar *key
){
	g_autoptr(GError) error = NULL;
	AxisArrSourceSpec spec;
	gint64 id = 0;

	memset(&spec, 0, sizeof(spec));
	spec.app = app;
	spec.name = name;
	spec.address = address;
	spec.secret = key;
	spec.user = "root";
	g_assert_true(axis_arr_save_source(fx->arr, 0, &spec, &id, &error));
	g_assert_no_error(error);
	g_assert_cmpint(id, >, 0);
	return id;
}

/* A connection to the stand-in Sonarr. */
static gint64
add_sonarr(Fixture *fx)
{
	g_autofree gchar *base = axis_test_sonarr_base(fx->sonarr);

	return add_source(fx, "sonarr", "Sonarr", base, KEY);
}

/* The queue that runs arr.read, as axisd has it; the run's handler too, so
 * that a run of a Sonarr, were one ever queued, would be seen to work. */
static void
queue_start(Fixture *fx)
{
	const AxisModuleInfo *module = axis_web_arr_get_module();
	g_autoptr(AxisJobRegistry) registry = axis_job_registry_new();
	g_autoptr(GError) error = NULL;

	if (fx->queue != NULL)
		return;
	g_assert_cmpstr(module->job_handlers[0].kind, ==, AXIS_ARR_READ_KIND);
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[0], fx->ui, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(module->job_handlers[2].kind, ==, AXIS_ARR_APPLY_KIND);
	g_assert_true(axis_job_registry_add(registry, &module->job_handlers[2], fx->ui, &error));
	g_assert_no_error(error);
	fx->queue = axis_job_queue_new(fx->t->pool, registry);
	axis_job_queue_set_poll_interval(fx->queue, 50);
	g_assert_true(axis_job_queue_start(fx->queue, &error));
	g_assert_no_error(error);
}

/* "Read what Sonarr has", from the page; waits until the plan is @state and
 * the job is over.  Returns how long the reading took, in milliseconds. */
static gint64
read_from_page(
	Fixture     *fx,
	gint64       source,
	const gchar *state,
	guint        wait_ms
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT, source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/read", source);
	g_autofree gchar *plan_sql = g_strdup_printf("SELECT state FROM arr_plan WHERE source_id = %" G_GINT64_FORMAT
		" ORDER BY id DESC LIMIT 1", source);
	gint64 start = g_get_monotonic_time();
	AxisTestReply posted;

	queue_start(fx);
	posted = post_as(fx, fx->admin, page, action, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "done=reading");
	axis_test_reply_clear(&posted);
	if (!axis_test_wait_query(fx->t->db, plan_sql, state, wait_ms)) {
		g_autofree gchar *now = axis_test_db_query(fx->t->db, plan_sql);
		g_autofree gchar *why = axis_test_db_query(fx->t->db, "SELECT coalesce(last_error, state::text) FROM job"
			" WHERE kind = 'arr.read' ORDER BY id DESC LIMIT 1");
		g_autofree gchar *problem = axis_test_db_query(fx->t->db, "SELECT problem FROM arr_plan ORDER BY id DESC LIMIT 1");

		g_error("the reading did not become %s: the plan is %s (%s), the job says %s", state, now, problem, why);
	}
	g_assert_true(axis_test_wait_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.read'"
		" AND state IN ('queued', 'running')", "0", 10000));
	return (g_get_monotonic_time() - start) / 1000;
}

static gchar *
plan_problem(
	Fixture *fx,
	gint64   source
){
	return axis_test_db_queryf(fx->t->db, "SELECT problem FROM arr_plan WHERE source_id = %" G_GINT64_FORMAT
		" ORDER BY id DESC LIMIT 1", source);
}

/* One series as Sonarr lists it, appended to @json (an array being written);
 * @seasons is its "seasons" array as text. */
static void
add_series(
	GString     *json,
	gint64       id,
	const gchar *title,
	gint         year,
	gint64       tvdb,
	gboolean     monitored,
	const gchar *type,
	gint64       profile,
	const gchar *root,
	const gchar *seasons
){
	g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"title\":\"%s\",\"year\":%d,\"tvdbId\":%"
		G_GINT64_FORMAT ",\"monitored\":%s,\"seriesType\":\"%s\",\"qualityProfileId\":%" G_GINT64_FORMAT
		",\"monitorNewItems\":\"all\",\"path\":\"%s/%s\",\"rootFolderPath\":\"%s\",\"seasons\":%s,"
		"\"statistics\":{\"episodeCount\":3,\"sizeOnDisk\":12345}}", json->len > 1 ? "," : "", id, title, year, tvdb,
		monitored ? "true" : "false", type, profile, root, title, root, seasons);
}

static void
add_episode(
	GString  *json,
	gint64    id,
	gint64    tvdb,
	gint      season,
	gint      number,
	gboolean  monitored,
	gint64    file
){
	g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"tvdbId\":%" G_GINT64_FORMAT ",\"seasonNumber\":%d,"
		"\"episodeNumber\":%d,\"airDate\":\"2025-01-%02d\",\"title\":\"A Made Up Episode\",\"monitored\":%s,"
		"\"hasFile\":%s,\"episodeFileId\":%" G_GINT64_FORMAT "}", json->len > 1 ? "," : "", id, tvdb, season, number,
		number % 28 + 1, monitored ? "true" : "false", file > 0 ? "true" : "false", file);
}

static void
add_file(
	GString     *json,
	gint64       id,
	const gchar *path,
	gint64       size
){
	g_string_append_printf(json, "%s{\"id\":%" G_GINT64_FORMAT ",\"path\":\"%s\",\"size\":%" G_GINT64_FORMAT
		",\"quality\":{\"quality\":{\"name\":\"WEBDL-1080p\"}},\"sceneName\":\"A.Made.Up.Release-MADEUP\"}",
		json->len > 1 ? "," : "", id, path, size);
}

#define ONE_SEASON "[{\"seasonNumber\":1,\"monitored\":true}]"

/*
 * A Sonarr with one of everything the preview tells apart: thirteen entries,
 * in this order (the one the provider does not answer for last).
 */
static void
odd_library(Fixture *fx)
{
	g_autoptr(GString) series = g_string_new("[");
	g_autoptr(GString) episodes = g_string_new("[");
	g_autoptr(GString) files = g_string_new("[");
	static const gint64 queued[] = { 2 };

	/* 1: would be migrated.  The specials and season 2 not monitored. */
	add_series(series, 1, "Gullwing Ferry", 2025, 81061, TRUE, "standard", 4, "/tv",
		"[{\"seasonNumber\":0,\"monitored\":false},{\"seasonNumber\":1,\"monitored\":true},"
		"{\"seasonNumber\":2,\"monitored\":false}]");
	/* 2: not monitored; a daily show; Sonarr is downloading it. */
	add_series(series, 2, "The Morning Tide Report", 2019, 81062, FALSE, "daily", 4, "/tv",
		"[{\"seasonNumber\":2019,\"monitored\":false}]");
	/* 3: anime, which is looked up as any series, and which TheTVDB's aired
	 * order does not list either.  4: no TheTVDB identifier.  5: one TMDb does
	 * not know.  6: one TheTVDB's aired order does not list. */
	add_series(series, 3, "Paper Crane Academy", 2014, 81063, TRUE, "anime", 4, "/tv", ONE_SEASON);
	add_series(series, 4, "Salt Meridian Nights", 2021, 0, TRUE, "standard", 4, "/tv", ONE_SEASON);
	add_series(series, 5, "Glass Harbour Diaries", 1987, 81065, TRUE, "standard", 4, "/tv", ONE_SEASON);
	add_series(series, 6, "Ash and Ember Street", 1996, 81066, TRUE, "standard", 4, "/tv", ONE_SEASON);
	/* 7: a profile with no choice.  8: a folder that is not mapped. */
	add_series(series, 7, "Tin Sparrow Road", 2001, 81067, TRUE, "standard", 6, "/tv", ONE_SEASON);
	add_series(series, 8, "Low Tide Radio Hour", 2015, 81068, TRUE, "standard", 4, "/tv-4k", ONE_SEASON);
	/* 9: no id: Sonarr's record of it is incomplete. */
	g_string_append(series, ",{\"title\":\"Hollow Compass Club\",\"year\":1999,\"tvdbId\":81069,\"monitored\":true,"
		"\"qualityProfileId\":4,\"path\":\"/tv/Hollow Compass Club\",\"rootFolderPath\":\"/tv\",\"seasons\":" ONE_SEASON
		"}");
	/* 10: Axis manages it already (the test adds it after the reading). */
	add_series(series, 10, "Marrow Lane Mysteries", 1990, 81070, TRUE, "standard", 4, "/tv", ONE_SEASON);
	/* 11: the first one again, under another of Sonarr's numbers. */
	add_series(series, 11, "Gullwing Ferry", 2025, 81061, TRUE, "standard", 4, "/tv", ONE_SEASON);
	/* 12: would be migrated, with three files none of which can be linked. */
	add_series(series, 12, "Copper Kettle Lane", 2003, 81073, TRUE, "standard", 4, "/tv", ONE_SEASON);
	/* 13: the provider does not answer for it. */
	add_series(series, 13, "Tall Grass County", 2008, 81091, TRUE, "standard", 4, "/tv", ONE_SEASON);
	g_string_append_c(series, ']');
	axis_test_sonarr_set_series(fx->sonarr, series->str);
	axis_test_sonarr_set_profiles(fx->sonarr, "[{\"id\":4,\"name\":\"Balanced\"},{\"id\":6,\"name\":\"Ultra-HD\"}]");
	axis_test_sonarr_set_folders(fx->sonarr, "[{\"path\":\"/tv\",\"accessible\":true},"
		"{\"path\":\"/tv-4k\",\"accessible\":true}]");
	axis_test_sonarr_set_queue(fx->sonarr, queued, G_N_ELEMENTS(queued));

	/* 1: six episodes: a single, a double episode in one file, one without a
	 * file that Sonarr does not monitor, a special, and one of the season
	 * Sonarr does not monitor, whose file Axis's index does not hold. */
	add_episode(episodes, 101, 81061001, 1, 1, TRUE, 501);
	add_episode(episodes, 102, 81061002, 1, 2, TRUE, 502);
	add_episode(episodes, 103, 81061003, 1, 3, TRUE, 502);
	add_episode(episodes, 104, 81061004, 1, 4, FALSE, 0);
	add_episode(episodes, 105, 0, 0, 1, FALSE, 0);
	add_episode(episodes, 106, 81061006, 2, 1, FALSE, 504);
	g_string_append_c(episodes, ']');
	add_file(files, 501, "/tv/Gullwing Ferry/Season 1/Gullwing Ferry - S01E01 - Low Water.mkv", 9);
	add_file(files, 502, "/tv/Gullwing Ferry/Season 1/Gullwing Ferry - S01E02-E03 - The Ropes.mkv", 7);
	add_file(files, 504, "/tv/Gullwing Ferry/Season 2/Gullwing Ferry - S02E01 - Night Crossing.mkv", 11);
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 1, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 1, files->str);

	/* 2: one episode, whose file is of another size than Sonarr recorded. */
	g_string_assign(episodes, "[");
	g_string_assign(files, "[");
	add_episode(episodes, 201, 81062001, 2019, 1, FALSE, 601);
	g_string_append_c(episodes, ']');
	add_file(files, 601, "/tv/The Morning Tide Report/Season 2019/The Morning Tide Report - 2019-03-04.mkv", 22);
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 2, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 2, files->str);

	/* 7: two episodes, no file. */
	g_string_assign(episodes, "[");
	add_episode(episodes, 701, 81067001, 1, 1, TRUE, 0);
	add_episode(episodes, 702, 81067002, 1, 2, TRUE, 0);
	g_string_append_c(episodes, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 7, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 7, "[]");

	/* 10: one episode with a file Axis's index does not hold. */
	g_string_assign(episodes, "[");
	g_string_assign(files, "[");
	add_episode(episodes, 1001, 81070001, 1, 1, TRUE, 801);
	g_string_append_c(episodes, ']');
	add_file(files, 801, "/tv/Marrow Lane Mysteries/Season 1/Marrow Lane Mysteries - S01E01.mkv", 5);
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, 10, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 10, files->str);

	/* 12: three episodes, each with a file that cannot be linked: one linked
	 * in Axis to another series, one whose path has a ".." segment (never
	 * resolved), one Sonarr gives no path for. */
	g_string_assign(episodes, "[");
	g_string_assign(files, "[");
	add_episode(episodes, 1201, 81073001, 1, 1, TRUE, 901);
	add_episode(episodes, 1202, 81073002, 1, 2, TRUE, 902);
	add_episode(episodes, 1203, 81073003, 1, 3, TRUE, 903);
	g_string_append_c(episodes, ']');
	add_file(files, 901, "/tv/Copper Kettle Lane/Season 1/Copper Kettle Lane - S01E01.mkv", 9);
	add_file(files, 902, "/tv/Copper Kettle Lane/../../etc/Copper Kettle Lane - S01E02.mkv", 9);
	g_string_append(files, ",{\"id\":903,\"size\":9}]");
	axis_test_sonarr_set_episodes(fx->sonarr, 12, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, 12, files->str);

	/* TheTVDB's aired order lists all the provider knows but one (81066). */
	tvdb_lists(fx, "81061");
	tvdb_lists(fx, "81062");
	tvdb_lists(fx, "81067");
	tvdb_lists(fx, "81068");
	tvdb_lists(fx, "81070");
	tvdb_lists(fx, "81073");
	/* The provider's failure for the last one, and TheTVDB's "no such
	 * series", are said in the log. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
}

/* Maps Sonarr's /tv to the library folder @root, from the page. */
static AxisTestReply
map_tv(
	Fixture     *fx,
	gint64       source,
	gint64       root,
	const gchar *more
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/preview", source);
	/* (Sonarr's profile 6 has no namesake in Axis: left to itself it would
	 * be set to Axis's default profile, so "none" is chosen for it by hand,
	 * and the series that uses it stays skipped for its profile.) */
	g_autofree gchar *form = g_strdup_printf("profile_ids=6&profile_6=0&folder_rows=1&folder_prefix_0=%%2Ftv"
		"&folder_root_0=%" G_GINT64_FORMAT "&folder_below_0=%s", root, more != NULL ? more : "");

	return post_as(fx, fx->admin, page, page, form);
}

/* Everything a reading and a preview must leave as it was: the series and the
 * movies Axis manages with all that hangs on them, the settings and the
 * index.  (Not in it, and written by a reading: the arr_* tables and the
 * metadata store.) */
static gchar *
library_digest(Fixture *fx)
{
	return axis_test_db_query(fx->t->db,
		"SELECT md5("
		" coalesce((SELECT string_agg(s::text, '|' ORDER BY s.id) FROM series s), '-') ||"
		" coalesce((SELECT string_agg(s::text, '|' ORDER BY s.id) FROM series_season s), '-') ||"
		" coalesce((SELECT string_agg(e::text, '|' ORDER BY e.id) FROM series_episode e), '-') ||"
		" coalesce((SELECT string_agg(f::text, '|' ORDER BY f.file_id) FROM series_file f), '-') ||"
		" coalesce((SELECT string_agg(f::text, '|') FROM series_file_episode f), '-') ||"
		" coalesce((SELECT string_agg(b::text, '|') FROM series_blocklist b), '-') ||"
		" coalesce((SELECT string_agg(g::text, '|') FROM series_grab g), '-') ||"
		" coalesce((SELECT string_agg(s::text, '|') FROM series_search s), '-') ||"
		" coalesce((SELECT string_agg(e::text, '|') FROM series_event e), '-') ||"
		" coalesce((SELECT string_agg(s::text, '|' ORDER BY s.key) FROM setting s), '-') ||"
		" coalesce((SELECT string_agg(f.id::text || f.rel_path || f.size::text || f.state, '|' ORDER BY f.id)"
		"   FROM library_file f), '-') ||"
		" (SELECT count(*) FROM movie)::text || (SELECT count(*) FROM movie_file)::text ||"
		" (SELECT count(*) FROM arr_brought)::text)");
}

/* Every table of the database but those a reading may write to, each with
 * how many rows it holds: nothing else may change by a reading or a preview. */
static gchar *
other_tables(Fixture *fx)
{
	return axis_test_db_query(fx->t->db,
		"SELECT string_agg(c.relname || '=' || (xpath('/row/n/text()', query_to_xml('SELECT count(*) AS n FROM '"
		" || quote_ident(c.relname), false, true, '')))[1]::text, ' ' ORDER BY c.relname)"
		" FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' AND c.relkind = 'r'"
		" AND c.relname NOT LIKE 'arr\\_%' AND c.relname NOT LIKE 'meta\\_%' AND c.relname NOT IN ('job', 'job_event',"
		" 'job_schedule', 'session', 'audit_log')");
}

static void
assert_secret_nowhere(
	Fixture     *fx,
	const gchar *key
){
	static const GLogLevelFlags levels[] = {
		G_LOG_LEVEL_CRITICAL, G_LOG_LEVEL_WARNING, G_LOG_LEVEL_MESSAGE, G_LOG_LEVEL_INFO, G_LOG_LEVEL_DEBUG
	};
	g_autofree gchar *pattern = g_strdup_printf("*%s*", key);
	g_autofree gchar *jobs = NULL;
	g_autofree gchar *rows = NULL;
	guint l;

	jobs = axis_test_db_queryf(fx->t->db, "SELECT count(*) FROM job j WHERE to_jsonb(j)::text LIKE '%%%s%%'", key);
	g_assert_cmpstr(jobs, ==, "0");
	/* It is in its own table, and in no other of this feature's. */
	rows = axis_test_db_queryf(fx->t->db,
		"SELECT (SELECT count(*) FROM arr_source a WHERE to_jsonb(a)::text LIKE '%%%s%%') +"
		" (SELECT count(*) FROM arr_plan p WHERE to_jsonb(p)::text LIKE '%%%s%%') +"
		" (SELECT count(*) FROM arr_plan_item i WHERE to_jsonb(i)::text LIKE '%%%s%%') +"
		" (SELECT count(*) FROM arr_plan_episode e WHERE to_jsonb(e)::text LIKE '%%%s%%') +"
		" (SELECT count(*) FROM arr_plan_file f WHERE to_jsonb(f)::text LIKE '%%%s%%')", key, key, key, key, key);
	g_assert_cmpstr(rows, ==, "0");
	for (l = 0; l < G_N_ELEMENTS(levels); l++)
		g_assert_cmpuint(axis_test_log_count(levels[l], pattern), ==, 0);
}

/* "Test the connection", from the page; the connection's page after it. */
static AxisTestReply
test_from_page(
	Fixture *fx,
	gint64   source
){
	g_autofree gchar *page = g_strdup_printf("/migrate/%" G_GINT64_FORMAT, source);
	g_autofree gchar *action = g_strdup_printf("/migrate/%" G_GINT64_FORMAT "/test", source);
	AxisTestReply posted = post_as(fx, fx->admin, page, action, NULL);

	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	return get_as(fx, fx->admin, page);
}

/* ── The connections ────────────────────────────────────────────── */

/* A connection is of one app for good.  Spec: Not Radarr (and its mirror);
 * Never shown again; Only GET. */
static void
test_app_kept(Fixture *fx, gconstpointer data)
{
	AxisTestPlex *sonarr_server = axis_test_sonarr_server(fx->sonarr);
	AxisTestPlex *radarr_server = axis_test_radarr_server(fx->radarr);
	g_autofree gchar *sonarr_base = axis_test_sonarr_base(fx->sonarr);
	g_autofree gchar *radarr_base = axis_test_radarr_base(fx->radarr);
	g_autofree gchar *escaped = g_uri_escape_string(sonarr_base, NULL, TRUE);
	g_autofree gchar *form = g_strdup_printf("app=sonarr&name=Living+room+Sonarr&address=%s&secret=%s", escaped, KEY);
	g_autofree gchar *row = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *plans = NULL;
	g_autofree gchar *problem = NULL;
	gint64 wrong_sonarr;
	gint64 wrong_radarr;
	AxisTestReply added;
	AxisTestReply page;
	AxisTestReply posted;
	AxisTestReply list;

	/* A failed reading is said in the log. */
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");

	/* The form offers both apps; a Sonarr is added from the page. */
	list = get_as(fx, fx->admin, "/migrate");
	g_assert_cmpuint(list.status, ==, 200);
	assert_csp_clean(list.body);
	assert_contains(list.body, "Migrate from Radarr or Sonarr");
	assert_contains(list.body, "<option value=\"sonarr\">Sonarr</option>");
	assert_contains(list.body, "Axis only reads from Radarr and Sonarr: it changes nothing there.");
	axis_test_reply_clear(&list);
	added = post_as(fx, fx->admin, "/migrate", "/migrate", form);
	g_assert_cmpuint(added.status, ==, 303);
	assert_contains(axis_test_reply_header(&added, "Location"), "/migrate/1?done=added");
	axis_test_reply_clear(&added);
	row = axis_test_db_queryf(fx->t->db, "SELECT app || ' ' || name || ' ' || (origin = '%s') FROM arr_source"
		" WHERE id = 1", sonarr_base);
	g_assert_cmpstr(row, ==, "sonarr Living room Sonarr true");

	/* Its page is a Sonarr's, in Sonarr's words, and never names Radarr; the
	 * key is nowhere on it. */
	page = get_as(fx, fx->admin, "/migrate/1?done=added");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "The connection has been added. Test it, then read what Sonarr has.");
	assert_contains(page.body, "Axis only reads from Sonarr. It changes nothing there.");
	assert_contains(page.body, "Read what Sonarr has");
	assert_contains(page.body, "Series are migrated only when you confirm at the end of the preview.");
	assert_contains(page.body, "API key (stored; type a new one to replace it)");
	assert_lacks(main_part(page.body), "Radarr");
	assert_lacks(main_part(page.body), "movie");
	assert_lacks(page.body, KEY);
	assert_lacks(page.body, "sonarr-key");
	save_page("sonarr-source.html", page.body);
	axis_test_reply_clear(&page);

	/* Sonarr answers as Sonarr: its version, and one request. */
	page = test_from_page(fx, 1);
	assert_contains(page.body, "Sonarr 4.0.9.2244 answered.");
	assert_contains(page.body, "Answers");
	assert_lacks(page.body, KEY);
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(sonarr_server), ==, 1);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/system/status"), ==, 1);

	/* A Radarr answering where Sonarr was expected: refused, its name and
	 * version not shown, and nothing more asked of it.  (The stand-in Radarr
	 * takes this connection's key, so only its name stands in the way.) */
	wrong_sonarr = add_source(fx, "sonarr", "Second connection", radarr_base, RADARR_KEY);
	page = test_from_page(fx, wrong_sonarr);
	assert_contains(page.body, "This address answers, but not as Sonarr.");
	assert_contains(page.body, "Not Sonarr");
	assert_lacks(main_part(page.body), "Radarr");
	assert_lacks(page.body, "5.2.6");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(radarr_server), ==, 1);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/system/status"), ==, 1);
	/* A reading of it asks the same one question and stops: no list of
	 * Radarr's is ever asked for by a Sonarr connection. */
	read_from_page(fx, wrong_sonarr, "failed", 30000);
	problem = plan_problem(fx, wrong_sonarr);
	g_assert_cmpstr(problem, ==, "This address answers, but not as Sonarr.");
	g_assert_cmpuint(axis_test_plex_n_requests(radarr_server), ==, 2);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/system/status"), ==, 2);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/movie"), ==, 0);
	g_assert_cmpuint(axis_test_radarr_count(fx->radarr, "/api/v3/qualityprofile"), ==, 0);
	plans = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan_item");
	g_assert_cmpstr(plans, ==, "0");

	/* The reverse: a Sonarr answering where Radarr was expected. */
	axis_test_plex_reset_requests(sonarr_server);
	wrong_radarr = add_source(fx, "radarr", "Third connection", sonarr_base, KEY);
	page = test_from_page(fx, wrong_radarr);
	assert_contains(page.body, "This address answers, but not as Radarr.");
	assert_contains(page.body, "Not Radarr");
	assert_lacks(main_part(page.body), "Sonarr");
	assert_lacks(page.body, "4.0.9");
	axis_test_reply_clear(&page);
	g_assert_cmpuint(axis_test_plex_n_requests(sonarr_server), ==, 1);
	read_from_page(fx, wrong_radarr, "failed", 30000);
	g_free(problem);
	problem = plan_problem(fx, wrong_radarr);
	g_assert_cmpstr(problem, ==, "This address answers, but not as Radarr.");
	g_assert_cmpuint(axis_test_plex_n_requests(sonarr_server), ==, 2);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/series"), ==, 0);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/queue"), ==, 0);

	/* A save that names the other app: there is no such connection, and
	 * nothing is written: not the name, not the address, not the key. */
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1",
		"app=radarr&name=Now+a+Radarr&address=http%3A%2F%2Fother.example%3A7878&secret=" OTHER_KEY);
	g_assert_cmpuint(posted.status, ==, 404);
	assert_lacks(posted.body, OTHER_KEY);
	axis_test_reply_clear(&posted);
	g_free(row);
	row = axis_test_db_queryf(fx->t->db, "SELECT app || ' ' || name || ' ' || (origin = '%s') FROM arr_source"
		" WHERE id = 1", sonarr_base);
	g_assert_cmpstr(row, ==, "sonarr Living room Sonarr true");
	secret = axis_test_db_query(fx->t->db, "SELECT secret FROM arr_source_secret WHERE source_id = 1");
	g_assert_cmpstr(secret, ==, KEY);
	/* An app that is neither is refused, in fixed words. */
	posted = post_as(fx, fx->admin, "/migrate", "/migrate",
		"app=lidarr&name=Third&address=http%3A%2F%2Flidarr.example%3A8686&secret=" OTHER_KEY);
	g_assert_cmpuint(posted.status, ==, 400);
	assert_contains(posted.body, "Choose the app: Radarr or Sonarr.");
	assert_lacks(posted.body, OTHER_KEY);
	axis_test_reply_clear(&posted);

	/* Every request either app ever received was a GET, with the key in its
	 * header only. */
	axis_test_sonarr_assert_read_only(fx->sonarr);
	axis_test_radarr_assert_read_only(fx->radarr);
	assert_secret_nowhere(fx, KEY);
	assert_secret_nowhere(fx, RADARR_KEY);
	assert_secret_nowhere(fx, OTHER_KEY);
}

/* Spec: A viewer */
static void
test_viewer(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	static const gchar *const pages[] = { "/migrate", "/migrate/1", "/migrate/1/preview",
		"/migrate/1/preview?bp=1", "/migrate/1/status", NULL };
	static const gchar *const posts[] = { "/migrate", "/migrate/1", "/migrate/1/test", "/migrate/1/read",
		"/migrate/1/forget", "/migrate/1/preview", "/migrate/1/run", NULL };
	g_autofree gchar *sources = NULL;
	g_autofree gchar *plans = NULL;
	guint i;

	g_assert_cmpint(source, ==, 1);
	for (i = 0; pages[i] != NULL; i++) {
		AxisTestReply viewer = get_as(fx, fx->viewer, pages[i]);
		AxisTestReply nobody = get_as(fx, NULL, pages[i]);

		g_assert_cmpuint(viewer.status, ==, 403);
		assert_lacks(viewer.body, "Sonarr");
		g_assert_cmpuint(nobody.status, !=, 200);
		axis_test_reply_clear(&viewer);
		axis_test_reply_clear(&nobody);
	}
	for (i = 0; posts[i] != NULL; i++) {
		/* With the viewer's own token from a page of theirs: still refused. */
		AxisTestReply viewer = post_as(fx, fx->viewer, "/", posts[i], "name=x&confirm=yes&app=sonarr"
			"&address=http%3A%2F%2Fsonarr.example%3A8989&secret=" OTHER_KEY);

		g_assert_cmpuint(viewer.status, ==, 403);
		axis_test_reply_clear(&viewer);
	}
	/* Nothing was read or changed, and Sonarr was not asked anything. */
	sources = axis_test_db_query(fx->t->db, "SELECT count(*) || ':' || min(name) FROM arr_source");
	g_assert_cmpstr(sources, ==, "1:Sonarr");
	plans = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan");
	g_assert_cmpstr(plans, ==, "0");
	g_assert_cmpuint(axis_test_plex_n_requests(axis_test_sonarr_server(fx->sonarr)), ==, 0);
}

/* ── The reading and the preview ────────────────────────────────── */

/* Spec: Nothing changed; Skipped, with reasons; Found at its path; Not seen;
 * Linked elsewhere; An incomplete title; Only GET. */
static void
test_read_and_preview(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	AxisTestPlex *server = axis_test_sonarr_server(fx->sonarr);
	g_autoptr(GPtrArray) requests = NULL;
	g_autofree gchar *untouched = NULL;
	g_autofree gchar *tables = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *managed_href = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	guint finds;
	guint orders;

	odd_library(fx);
	untouched = library_digest(fx);
	tables = other_tables(fx);
	read_from_page(fx, source, "complete", 90000);

	/* What the reading stored: thirteen items, each with how it was matched. */
	text = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan_item");
	g_assert_cmpstr(text, ==, "13");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT state || ' ' || attempts || ' ' || (payload::text NOT LIKE '%" KEY "%')"
		" FROM job WHERE kind = 'arr.read'");
	g_assert_cmpstr(text, ==, "succeeded 1 true");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(match, ' ' ORDER BY seq) FROM arr_plan_item");
	/* (The third is the one Sonarr has as anime: looked up as any series, and
	 * here one TheTVDB's aired order does not list.  It is no longer set aside
	 * unasked: add-anime-support, A1.) */
	g_assert_cmpstr(text, ==, "matched matched not_listed no_id unknown not_listed matched matched incomplete matched"
		" matched matched not_looked_up");
	/* A series by its TheTVDB identifier, with its type and what Sonarr says
	 * of seasons that come later. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT tvdb_id || ' ' || series_type || ' ' || monitor_new || ' ' ||"
		" (tmdb_id IS NULL) || ' ' || has_file FROM arr_plan_item WHERE seq = 2");
	g_assert_cmpstr(text, ==, "81062 daily true true false");
	/* Each season's monitoring, as Sonarr has it. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg((s ->> 'n') || ':' || (s ->> 'm'), ' ' ORDER BY (s ->> 'n')"
		"::integer) FROM arr_plan_item i, jsonb_array_elements(i.seasons) s WHERE i.seq = 1");
	g_assert_cmpstr(text, ==, "0:false 1:true 2:false");
	/* Each episode's monitoring, its place, TheTVDB's id of it and the file
	 * that holds it: a double episode is one file on two episodes. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(season || 'x' || episode || ':' || monitored || ':' ||"
		" coalesce(tvdb_id::text, '-') || ':' || coalesce(file_arr_id::text, '-'), ' ' ORDER BY ord)"
		" FROM arr_plan_episode WHERE seq = 1");
	g_assert_cmpstr(text, ==, "1x1:true:81061001:501 1x2:true:81061002:502 1x3:true:81061003:502 1x4:false:81061004:-"
		" 0x1:false:-:- 2x1:false:81061006:504");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(arr_id || ':' || size, ' ' ORDER BY ord) FROM arr_plan_file"
		" WHERE seq = 1");
	g_assert_cmpstr(text, ==, "501:9 502:7 504:11");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT n_episodes || ' ' || n_episodes_monitored || ' ' ||"
		" n_episodes_with_file || ' ' || n_files || ' ' || n_odd FROM arr_plan_item WHERE seq = 1");
	g_assert_cmpstr(text, ==, "6 3 4 3 0");
	/* The lists of a series that is not matched were not asked for. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(seq::text, ' ' ORDER BY seq) FROM arr_plan_item"
		" WHERE n_episodes IS NOT NULL");
	g_assert_cmpstr(text, ==, "1 2 7 8 10 11 12");
	/* What Sonarr was downloading when it was read. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(title, ',') FROM arr_plan_item WHERE in_queue");
	g_assert_cmpstr(text, ==, "The Morning Tide Report");

	/* One request for each list and the queue, two for each matched series;
	 * the first of all is system/status; nothing but GET. */
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/system/status"), ==, 1);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/series"), ==, 1);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/qualityprofile"), ==, 1);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/rootfolder"), ==, 1);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/queue"), ==, 1);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episode"), ==, 7);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episodefile"), ==, 7);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/movie"), ==, 0);
	requests = axis_test_plex_requests(server);
	g_assert_true(g_str_has_prefix(g_ptr_array_index(requests, 0), "GET /api/v3/system/status"));
	axis_test_sonarr_assert_read_only(fx->sonarr);

	/* The reading wrote to the arr_* tables and the metadata store, and to
	 * nothing else. */
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, untouched);
	g_free(after);
	after = other_tables(fx);
	g_assert_cmpstr(after, ==, tables);

	/* Axis manages one of the series already, and one of Sonarr's files is
	 * that series' in Axis. */
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO series (title_id, quality_profile_id, root_id) SELECT e.title_id,"
		" (SELECT id FROM quality_profile WHERE slug = 'balanced'), %" G_GINT64_FORMAT " FROM meta_external_id e"
		" WHERE e.kind = 'series' AND e.scheme = 'tvdb' AND e.value = '81070' RETURNING id", fx->root_id));
	axis_test_db_exec(fx->t->db, "INSERT INTO series_file (file_id, series_id) SELECT f.id, s.id FROM library_file f,"
		" series s WHERE f.rel_path = 'Copper Kettle Lane/Season 1/Copper Kettle Lane - S01E01.mkv'");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT count(*) FROM series_file");
	g_assert_cmpstr(text, ==, "1");
	before = library_digest(fx);

	/* The connection's page says what was read, and leads to the preview. */
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "by root: 13 series.");
	assert_contains(page.body, "href=\"/migrate/1/preview\"");
	assert_contains(page.body, "Read again");
	assert_lacks(page.body, KEY);
	axis_test_reply_clear(&page);

	/* The preview, with nothing mapped yet: the profile of the same name is
	 * chosen by itself, and no folder is. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Sonarr has 13 series: 11 monitored, 2 not. 1 is already in Axis.");
	assert_contains(page.body, "The episodes of 7 series were read: 13 episodes, 9 monitored in Sonarr, 9 with a file.");
	assert_contains(page.body, "Would migrate 0 series with 0 episodes (0 monitored)");
	assert_contains(page.body, "13 skipped (see below). Nothing would be searched for.");
	assert_contains(page.body, "Sonarr was downloading episodes of 1 series when it was read.");
	/* The profile with no namesake is set to Axis's default profile, and
	 * said to be a suggestion; so no series is skipped for its profile. */
	/* (Five: the series Sonarr does not monitor is brought too until a person
	 * clears the option, so it is skipped for its folder as the others are.) */
	assert_contains(page.body, "Folder not mapped (5)");
	assert_lacks(page.body, "Not monitored in Sonarr (");
	assert_contains(page.body, "Also migrate unmonitored series");
	assert_contains(page.body, "<input id=\"arr-unmonitored\" checked=\"checked\" type=\"checkbox\"");
	assert_lacks(page.body, "is left out: tick the box");
	assert_lacks(page.body, "Quality profile not mapped (");
	assert_contains(page.body, "Axis filled these in from what it already knows. Change any that are wrong.");
	assert_contains(page.body, "Suggested");
	assert_contains(page.body, "Axis has a profile of the same name");
	assert_contains(page.body, "Axis's default profile");
	/* A poor match suggests no folder: of the six files Sonarr names under
	 * /tv with a path that can be read, the index holds three below the one
	 * library folder; and Sonarr has no file under /tv-4k. */
	assert_contains(page.body, "Not mapped. Nothing suggested: of 6 files looked for, no library folder holds more "
		"than 3 at the same paths.");
	assert_contains(page.body, "Not mapped. Nothing suggested: Sonarr has no files under it.");
	assert_contains(page.body, "Would be migrated: 0 series, 0 monitored and 0 unmonitored, as in Sonarr. 0 seasons and "
		"0 episodes would be migrated monitored.");
	/* Nothing is mapped yet: nothing to confirm, and no form. */
	assert_contains(page.body, "There is nothing to migrate now: every series is in Axis already or skipped");
	assert_lacks(main_part(page.body), "Radarr");
	assert_lacks(main_part(page.body), "movie");
	assert_lacks(page.body, "name=\"digest\"");
	assert_lacks(page.body, KEY);
	save_page("sonarr-preview-unmapped.html", page.body);
	axis_test_reply_clear(&page);

	/* Sonarr's /tv is the library folder Axis sees at media/disk1/shows, and
	 * each episode file is looked for below it. */
	posted = map_tv(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/migrate/1/preview?done=saved");
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview?done=saved");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "The choices have been saved.");
	/* What the page is for, first: the monitoring that would come. */
	assert_contains(page.body, "Would be migrated: 2 series, 2 monitored and 0 unmonitored, as in Sonarr. 2 seasons and "
		"6 episodes would be migrated monitored. 1 series that Sonarr does not monitor is left out: tick the box under "
		"Options to have it migrated unmonitored.");
	g_assert_true(strstr(page.body, "id=\"arr-monitoring\"") < strstr(page.body, "Sonarr has 13 series"));
	assert_contains(page.body, "Would migrate 2 series with 9 episodes (6 monitored), each season and episode monitored "
		"as in Sonarr, and link 2 files of their 6 episode files. 11 skipped (see below). Nothing would be searched "
		"for.");
	assert_contains(page.body, "4 files of these series would not be linked");
	/* The numbering a series from Sonarr would follow. */
	assert_contains(page.body, "A series migrated from Sonarr would follow TheTVDB's aired order");
	/* The check of the folder row: every file Sonarr has under it, whether
	 * its series would come or not. */
	assert_contains(page.body, "3 of 7 files found at these paths in Axis. Last scanned ");
	/* Each reason, with its count. */
	assert_contains(page.body, "Already in Axis (1)");
	assert_contains(page.body, "Not monitored in Sonarr (1)");
	assert_contains(page.body, "No match: Sonarr has no TheTVDB identifier for it (1)");
	assert_contains(page.body, "No match: TMDb no longer has the entry Sonarr points to. It was probably removed there; nothing can be migrated for it. (1)");
	assert_contains(page.body, "Not looked up yet: TMDb or TheTVDB did not answer (1)");
	/* (Two: the anime series is one of them, skipped for the reason any series
	 * would be, and never for being anime.) */
	assert_contains(page.body, "TheTVDB's aired order does not list it (2)");
	assert_lacks(page.body, "not supported yet");
	assert_lacks(page.body, "numbered absolutely");
	assert_contains(page.body, "Sonarr lists it twice (1)");
	assert_contains(page.body, "Quality profile not mapped (1)");
	assert_contains(page.body, "Folder not mapped (1)");
	assert_contains(page.body, "Sonarr's answer for it is incomplete (1)");
	assert_lacks(page.body, "No TheTVDB key in Axis");
	/* ... and its title. */
	assert_contains(page.body, "Marrow Lane Mysteries (1990)");
	assert_contains(page.body, "The Morning Tide Report (2019)");
	assert_contains(page.body, "Salt Meridian Nights (2021)");
	assert_contains(page.body, "Glass Harbour Diaries (1987)");
	assert_contains(page.body, "Tall Grass County (2008)");
	assert_contains(page.body, "Ash and Ember Street (1996)");
	assert_contains(page.body, "Paper Crane Academy (2014)");
	assert_contains(page.body, "Tin Sparrow Road (2001)");
	assert_contains(page.body, "Low Tide Radio Hour (2015)");
	assert_contains(page.body, "Hollow Compass Club (1999)");
	/* The managed one links to its page. */
	managed_href = axis_test_db_query(fx->t->db, "SELECT 'href=\"/title/' || title_id || '\"' FROM series");
	assert_contains(page.body, managed_href);

	/* The series that would be migrated, each with how Sonarr monitors it and
	 * its seasons, its episodes, where it would go and its files. */
	assert_contains(page.body, "Series that would be migrated");
	assert_contains(page.body, "Gullwing Ferry (2025)");
	assert_contains(page.body, "Monitored. Seasons monitored: 1. Not monitored: specials, 2.");
	assert_contains(page.body, "6 episodes: 3 monitored, 4 with a file.");
	assert_contains(page.body, "Balanced, in shows. TheTVDB's aired order.");
	assert_contains(page.body, "3 files: 2 found in Axis, 1 not.");
	assert_contains(page.body, "Copper Kettle Lane (2003)");
	assert_contains(page.body, "Monitored. Seasons monitored: 1.");
	assert_contains(page.body, "3 episodes: 3 monitored, 3 with a file.");
	assert_contains(page.body, "3 files: 0 found in Axis, 3 not.");
	assert_contains(page.body, "1 to 2 of 2.");

	/* The files that would not be linked, each with its reason and both
	 * paths.  Nothing is resolved, guessed or looked for nearby. */
	assert_contains(page.body, "Not in Axis's index at that path");
	assert_contains(page.body, "/tv/Gullwing Ferry/Season 2/Gullwing Ferry - S02E01 - Night Crossing.mkv");
	assert_contains(page.body, ": Gullwing Ferry/Season 2/Gullwing Ferry - S02E01 - Night Crossing.mkv");
	assert_contains(page.body, "Linked in Axis to another series: Marrow Lane Mysteries (1990)");
	assert_contains(page.body, "Path cannot be mapped");
	assert_contains(page.body, "/tv/Copper Kettle Lane/../../etc/Copper Kettle Lane - S01E02.mkv");
	assert_contains(page.body, "Sonarr gives no path or size for it");
	/* The files that would be linked are in no list; nor is the file of a
	 * series that would not come (another size than Sonarr recorded). */
	assert_lacks(page.body, "Low Water");
	assert_lacks(page.body, "The Ropes");
	assert_lacks(page.body, "Another size than Sonarr recorded");
	assert_lacks(page.body, "2019-03-04");
	assert_lacks(main_part(page.body), "Radarr");
	assert_lacks(page.body, KEY);
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 5);
	save_page("sonarr-preview.html", page.body);
	axis_test_reply_clear(&page);

	/* The reasons as a filter. */
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=no-id");
	assert_contains(page.body, "Salt Meridian Nights (2021)");
	assert_lacks(page.body, "Paper Crane Academy");
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=not-listed&bp=99999999");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Ash and Ember Street (1996)");
	assert_contains(page.body, "Paper Crane Academy (2014)");
	assert_lacks(page.body, "Salt Meridian Nights");
	axis_test_reply_clear(&page);
	/* "anime" is no reason any more: an old address with it lists every
	 * reason, as any word that is none does. */
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=anime");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Salt Meridian Nights (2021)");
	assert_contains(page.body, "Paper Crane Academy (2014)");
	axis_test_reply_clear(&page);

	/* The option: unmonitored series too.  Its file of another size is then
	 * one of the files that would not be linked. */
	posted = map_tv(fx, source, fx->root_id, "&unmonitored=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 3 series with 10 episodes (6 monitored)");
	assert_contains(page.body, "Would be migrated: 3 series, 2 monitored and 1 unmonitored, as in Sonarr. 2 seasons and "
		"6 episodes would be migrated monitored.");
	assert_lacks(page.body, "is left out");
	assert_contains(page.body, "link 2 files of their 7 episode files. 10 skipped");
	assert_lacks(page.body, "Not monitored in Sonarr (");
	assert_contains(page.body, "Not monitored. Not monitored: 2019.");
	assert_contains(page.body, "Another size than Sonarr recorded");
	assert_contains(page.body, "/tv/The Morning Tide Report/Season 2019/The Morning Tide Report - 2019-03-04.mkv");
	axis_test_reply_clear(&page);

	/* Nothing of the library changed by any of it: no series, season,
	 * episode, link, blocklist entry, grab, search, history line or setting,
	 * and the index as the scan left it. */
	g_free(after);
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, before);

	/* A second reading takes the titles and the aired orders from the store:
	 * the providers are asked again only for what they did not answer and do
	 * not know.  It leaves one plan, and no row of the first. */
	finds = axis_test_plex_count(fx->fa->api, "/find/series");
	orders = axis_test_tvdb_gets(fx->tvdb);
	read_from_page(fx, source, "complete", 90000);
	g_assert_cmpuint(axis_test_plex_count(fx->fa->api, "/find/series") - finds, <=, 2);
	/* (Two orders TheTVDB does not list: the anime series is asked about as
	 * any other now.) */
	g_assert_cmpuint(axis_test_tvdb_gets(fx->tvdb) - orders, <=, 2);
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) || ' ' || count(DISTINCT plan_id) FROM arr_plan_item)"
		" || ' ' || (SELECT count(*) || ' ' || count(DISTINCT plan_id) FROM arr_plan_episode) || ' ' ||"
		" (SELECT count(*) || ' ' || count(DISTINCT plan_id) FROM arr_plan_file)");
	g_assert_cmpstr(text, ==, "13 1 13 1 8 1");
	g_free(after);
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, before);

	/* Forgetting the connection takes its key and everything it read. */
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/forget", "confirm=yes");
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_source) + (SELECT count(*) FROM"
		" arr_source_secret) + (SELECT count(*) FROM arr_plan) + (SELECT count(*) FROM arr_plan_item) +"
		" (SELECT count(*) FROM arr_plan_episode) + (SELECT count(*) FROM arr_plan_file)");
	g_assert_cmpstr(text, ==, "0");
	g_free(after);
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, before);

	axis_test_sonarr_assert_read_only(fx->sonarr);
	assert_secret_nowhere(fx, KEY);
}

/* One series with @n monitored episodes of season 1, each with a file
 * "TITLE - S01E0N.mkv" of @first_size + N - 1 bytes below @root. */
static void
suggested_series(
	Fixture     *fx,
	GString     *series,
	gint64       id,
	const gchar *title,
	gint         year,
	gint64       tvdb,
	gint64       profile,
	const gchar *root,
	guint        n,
	gint64       first_size
){
	g_autoptr(GString) episodes = g_string_new("[");
	g_autoptr(GString) files = g_string_new("[");
	g_autofree gchar *tvdb_text = g_strdup_printf("%" G_GINT64_FORMAT, tvdb);
	guint i;

	add_series(series, id, title, year, tvdb, TRUE, "standard", profile, root, ONE_SEASON);
	for (i = 1; i <= n; i++) {
		g_autofree gchar *path = g_strdup_printf("%s/%s/Season 1/%s - S01E0%u.mkv", root, title, title, i);

		add_episode(episodes, id * 100 + i, tvdb * 1000 + i, 1, (gint)i, TRUE, id * 1000 + i);
		add_file(files, id * 1000 + i, path, first_size + i - 1);
	}
	g_string_append_c(episodes, ']');
	g_string_append_c(files, ']');
	axis_test_sonarr_set_episodes(fx->sonarr, id, episodes->str);
	axis_test_sonarr_set_episode_files(fx->sonarr, id, files->str);
	tvdb_lists(fx, tvdb_text);
}

/*
 * The preview arrives filled in from what Axis already knows.  A folder of
 * Sonarr's whose files the index holds below a library folder, or below a
 * folder in one, is set to that place; one whose files are in two library
 * folders is not.  A profile is set to the one of the same name, else to the
 * reader's default for new series, else to Axis's default profile.  The run
 * is offered on the first view, viewing writes nothing, and a saved choice
 * ("not mapped" and "none" too) is never replaced by a suggestion.
 */
static void
test_suggested(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autoptr(GString) series = g_string_new("[");
	g_autoptr(AxisScanResult) first = NULL;
	g_autoptr(AxisScanResult) second = NULL;
	g_autofree gchar *other_dir = g_build_filename(fx->disk, "more-shows", "Low Tide Radio Hour", "Season 1", NULL);
	g_autofree gchar *untouched = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *uhd = NULL;
	g_autofree gchar *uhd_line = NULL;
	g_autofree gchar *form = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	gint64 other_root;

	/* On the disk: a series in a folder of the library folder, and one that
	 * is in two library folders alike. */
	write_file(fx, "Copper Kettle Lane/Season 1", "Copper Kettle Lane - S01E02.mkv", "kettle-two");
	write_file(fx, "kids/Tin Sparrow Road/Season 1", "Tin Sparrow Road - S01E01.mkv", "sparrow-01");
	write_file(fx, "kids/Tin Sparrow Road/Season 1", "Tin Sparrow Road - S01E02.mkv", "sparrow-002");
	write_file(fx, "kids/Tin Sparrow Road/Season 1", "Tin Sparrow Road - S01E03.mkv", "sparrow-0003");
	write_file(fx, "Low Tide Radio Hour/Season 1", "Low Tide Radio Hour - S01E01.mkv", "radio-hour-0001");
	write_file(fx, "Low Tide Radio Hour/Season 1", "Low Tide Radio Hour - S01E02.mkv", "radio-hour-00002");
	write_file(fx, "Low Tide Radio Hour/Season 1", "Low Tide Radio Hour - S01E03.mkv", "radio-hour-000003");
	other_root = axis_test_library_add_root(fx->t, fx->volume_id, fx->disk, "more-shows", AXIS_MEDIA_TV);
	g_assert_cmpint(g_mkdir_with_parents(other_dir, 0755), ==, 0);
	axis_test_write_named(other_dir, "Low Tide Radio Hour - S01E01.mkv", "radio-hour-0001");
	axis_test_write_named(other_dir, "Low Tide Radio Hour - S01E02.mkv", "radio-hour-00002");
	axis_test_write_named(other_dir, "Low Tide Radio Hour - S01E03.mkv", "radio-hour-000003");
	first = axis_test_library_scan(fx->t, fx->root_id);
	second = axis_test_library_scan(fx->t, other_root);
	while (g_main_context_iteration(NULL, FALSE))
		;

	/* Sonarr: three root folders, two profiles (one with a namesake in Axis). */
	suggested_series(fx, series, 1, "Copper Kettle Lane", 2003, 81073, 4, "/tv", 2, 9);
	suggested_series(fx, series, 2, "The Morning Tide Report", 2019, 81062, 6, "/tv", 0, 0);
	suggested_series(fx, series, 3, "Tin Sparrow Road", 2001, 81067, 6, "/kidtv", 3, 10);
	suggested_series(fx, series, 4, "Low Tide Radio Hour", 2015, 81068, 4, "/tv2", 3, 15);
	g_string_append_c(series, ']');
	axis_test_sonarr_set_series(fx->sonarr, series->str);
	axis_test_sonarr_set_profiles(fx->sonarr, "[{\"id\":4,\"name\":\"BALANCED\"},{\"id\":6,\"name\":\"Ultra-HD\"}]");
	axis_test_sonarr_set_folders(fx->sonarr, "[{\"path\":\"/kidtv\",\"accessible\":true},"
		"{\"path\":\"/tv\",\"accessible\":true},{\"path\":\"/tv2\",\"accessible\":true}]");
	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	read_from_page(fx, source, "complete", 90000);
	untouched = library_digest(fx);

	/* The first view, with nothing saved. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_csp_clean(page.body);
	assert_contains(page.body, "Axis filled these in from what it already knows. Change any that are wrong.");
	/* What would come, first: the monitoring. */
	assert_contains(page.body, "Would be migrated: 3 series, 3 monitored and 0 unmonitored, as in Sonarr. 3 seasons and "
		"5 episodes would be migrated monitored.");
	/* /tv is the library folder itself, /kidtv a folder in it: each with what
	 * was found there. */
	assert_contains(page.body, "2 of 2 files found here. Last scanned ");
	assert_contains(page.body, "3 of 3 files found here. Last scanned ");
	assert_contains(page.body, "value=\"kids\"");
	/* /tv2's files are in two library folders: nothing is suggested. */
	assert_contains(page.body, "3 files of Sonarr's. Not mapped. Nothing suggested: its files are in Axis's index in "
		"more than one place. Choose one.");
	assert_contains(page.body, "Folder not mapped (1)");
	/* The profiles: the namesake whatever its case, and Axis's default. */
	assert_contains(page.body, "Axis has a profile of the same name");
	assert_contains(page.body, "Axis's default profile");
	assert_lacks(page.body, "Quality profile not mapped (");
	assert_contains(page.body, "Balanced, in shows. TheTVDB's aired order.");
	/* The run is offered. */
	assert_contains(page.body, "name=\"digest\"");
	save_page("sonarr-preview-suggested.html", page.body);
	axis_test_reply_clear(&page);

	/* The reader saved defaults for new series: their profile, not Axis's. */
	uhd = axis_test_db_query(fx->t->db, "SELECT name FROM quality_profile WHERE slug = 'uhd'");
	uhd_line = g_strdup_printf("%s, in shows. TheTVDB's aired order.", uhd);
	axis_test_db_exec(fx->t->db, "INSERT INTO app_user (username, password_hash, role, add_defaults) SELECT 'root',"
		" '$argon2id$made-up', 'admin', jsonb_build_object('series', jsonb_build_object('profile', id))"
		" FROM quality_profile WHERE slug = 'uhd'");
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "root's default for new series");
	assert_lacks(page.body, "Axis's default profile");
	assert_contains(page.body, uhd_line);
	axis_test_reply_clear(&page);

	/* Viewing wrote nothing: not to the library, and no choice. */
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, untouched);
	text = axis_test_db_query(fx->t->db, "SELECT mappings::text FROM arr_source");
	g_assert_cmpstr(text, ==, "{}");

	/* A person's choices: "none" for the profile, "not mapped" for /kidtv and
	 * /tv2, /tv as it was suggested.  They stay, and no suggestion takes the
	 * place of any of them. */
	form = g_strdup_printf("profile_ids=6&profile_6=0&folder_rows=3&folder_prefix_0=%%2Fkidtv&folder_root_0=0"
		"&folder_below_0=kids&folder_prefix_1=%%2Ftv&folder_root_1=%" G_GINT64_FORMAT "&folder_below_1="
		"&folder_prefix_2=%%2Ftv2&folder_root_2=0&folder_below_2=", fx->root_id);
	posted = post_as(fx, fx->admin, "/migrate/1/preview", "/migrate/1/preview", form);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_lacks(page.body, "value=\"kids\"");
	assert_lacks(page.body, "Nothing suggested");
	assert_lacks(page.body, "root's default for new series");
	assert_lacks(page.body, "files found here");
	assert_contains(page.body, "2 of 2 files found at these paths in Axis. Last scanned ");
	assert_contains(page.body, "Quality profile not mapped (2)");
	assert_contains(page.body, "Folder not mapped (1)");
	assert_contains(page.body, "Would be migrated: 1 series, 1 monitored and 0 unmonitored");
	axis_test_reply_clear(&page);

	/* ... nor after a later reading. */
	read_from_page(fx, source, "complete", 90000);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_lacks(page.body, "value=\"kids\"");
	assert_lacks(page.body, "Nothing suggested");
	assert_contains(page.body, "Quality profile not mapped (2)");
	assert_contains(page.body, "Folder not mapped (1)");
	axis_test_reply_clear(&page);
	g_free(after);
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, untouched);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* Without a TheTVDB key in Axis no Sonarr is read and no series is brought,
 * and the pages say why and where the key goes. */
static void
test_no_tvdb_key(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	AxisTestPlex *server = axis_test_sonarr_server(fx->sonarr);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	AxisTestReply page;
	AxisTestReply posted;
	gint64 plan = 0;

	odd_library(fx);
	g_assert_true(axis_arr_series_ready(fx->arr));
	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TVDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	g_assert_false(axis_arr_series_ready(fx->arr));

	/* The connection's page: the reading is not offered, and the page says
	 * why and where to enter the key.  Testing the connection still works. */
	page = get_as(fx, fx->admin, "/migrate/1");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Sonarr numbers its series as TheTVDB does. Set a TheTVDB key on the Movie and TV "
		"details page to migrate them: until then Axis does not read Sonarr, and migrates no series.");
	assert_contains(page.body, "href=\"/metadata\"");
	assert_contains(page.body, "Open Movie and TV details to set the TheTVDB key");
	assert_lacks(page.body, "/migrate/1/read");
	assert_contains(page.body, "Test the connection");
	save_page("sonarr-source-no-key.html", page.body);
	axis_test_reply_clear(&page);

	/* The post itself is refused: nothing is asked of Sonarr, no plan is
	 * made and no job is queued. */
	queue_start(fx);
	posted = post_as(fx, fx->admin, "/migrate/1", "/migrate/1/read", NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	assert_contains(axis_test_reply_header(&posted, "Location"), "/migrate/1?done=no-tvdb-key");
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1?done=no-tvdb-key");
	assert_contains(page.body, "Nothing was read: Axis has no TheTVDB key.");
	axis_test_reply_clear(&page);
	g_assert_false(axis_arr_request_read(fx->arr, source, "root", &plan, &error));
	g_assert_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_NO_TVDB_KEY);
	g_clear_error(&error);
	text = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_plan) + (SELECT count(*) FROM job WHERE kind"
		" = 'arr.read')");
	g_assert_cmpstr(text, ==, "0");
	g_assert_cmpuint(axis_test_plex_n_requests(server), ==, 0);
	/* The preview has nothing, and says the same. */
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "There is nothing to preview");
	assert_contains(page.body, "Axis has no TheTVDB key, so no series would be migrated.");
	assert_contains(page.body, "href=\"/metadata\"");
	axis_test_reply_clear(&page);

	/* With the key, Sonarr is read; and a Radarr is read without one. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "/migrate/1/read");
	assert_lacks(page.body, "Set a TheTVDB key");
	axis_test_reply_clear(&page);
	read_from_page(fx, source, "complete", 90000);
	posted = map_tv(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	/* (No series is managed here, so the one of the other test is brought.) */
	assert_contains(page.body, "Would migrate 3 series with 10 episodes (7 monitored)");
	assert_lacks(page.body, "No TheTVDB key in Axis");
	axis_test_reply_clear(&page);

	/* The key goes after the reading: the plan is still there, and nothing of
	 * it would be brought.  Every series a reason of its own does not skip
	 * first (an incomplete answer, a second entry) is skipped for the key. */
	g_assert_true(axis_settings_clear(fx->settings, AXIS_META_TVDB_SETTING_KEY, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Axis has no TheTVDB key, so no series would be migrated.");
	assert_contains(page.body, "Set a TheTVDB key on the Movie and TV details page, then read Sonarr again.");
	assert_contains(page.body, "href=\"/metadata\"");
	assert_contains(page.body, "Would migrate 0 series with 0 episodes (0 monitored)");
	assert_contains(page.body, "No TheTVDB key in Axis (11)");
	assert_contains(page.body, "Sonarr's answer for it is incomplete (1)");
	assert_contains(page.body, "Sonarr lists it twice (1)");
	assert_contains(page.body, "No series would be migrated now");
	assert_lacks(page.body, "Already in Axis (");
	save_page("sonarr-preview-no-key.html", page.body);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=no-tvdb-key");
	assert_contains(page.body, "Gullwing Ferry (2025)");
	assert_lacks(page.body, "Hollow Compass Club");
	axis_test_reply_clear(&page);

	/* The key back: as before. */
	g_assert_true(axis_settings_set(fx->settings, AXIS_META_TVDB_SETTING_KEY, AXIS_TEST_TVDB_KEY, &error));
	g_assert_no_error(error);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	assert_contains(page.body, "Would migrate 3 series with 10 episodes (7 monitored)");
	axis_test_reply_clear(&page);
	axis_test_sonarr_assert_read_only(fx->sonarr);
	assert_secret_nowhere(fx, KEY);
}

/* The store holds @n generated series as after identification, each with its
 * TheTVDB identifier (700000 + N) and a season of TheTVDB's aired order. */
static void
seed_store(
	Fixture *fx,
	guint    n
){
	g_free(axis_test_db_queryf(fx->t->db, "INSERT INTO meta_title (kind, title, year, provider, provider_id) SELECT"
		" 'series', 'Made Up Series ' || g, 2021, 'fixture', (700000 + g)::text FROM generate_series(1, %u) g RETURNING 1",
		n));
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_external_id (title_id, kind, scheme, value) SELECT id, 'series',"
		" 'tvdb', provider_id FROM meta_title WHERE provider = 'fixture' AND kind = 'series'");
	axis_test_db_exec(fx->t->db, "INSERT INTO meta_season (title_id, number, source, episodes_fetched_at) SELECT id, 1,"
		" 'tvdb', now() FROM meta_title WHERE provider = 'fixture' AND kind = 'series'");
}

/* Paged lists: Sonarr's queue page by page up to its count; a series whose
 * list cannot be read is incomplete alone; odd entries are left out and
 * counted; the preview's lists a hundred rows at a time. */
static void
test_paging_and_odd(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autofree gchar *text = NULL;
	gint64 ids[7];
	AxisArrTuning tuning;
	AxisTestReply page;
	AxisTestReply posted;
	guint i;

	tune(fx, &tuning);
	tuning.page_size = 3;
	axis_arr_set_tuning(fx->arr, &tuning);
	/* 150 series, two episodes each, all in the store: none is looked up. */
	seed_store(fx, 150);
	axis_test_sonarr_generate(fx->sonarr, 150, 2);
	axis_test_sonarr_set_profiles(fx->sonarr, "[{\"id\":4,\"name\":\"Balanced\"}]");
	for (i = 0; i < G_N_ELEMENTS(ids); i++)
		ids[i] = 10 + i;
	axis_test_sonarr_set_queue(fx->sonarr, ids, G_N_ELEMENTS(ids));
	/* Series 5: its episodes are not a list.  Series 6: its files are not.
	 * Series 7: entries that are not episodes, or not files, among those that
	 * are; an episode whose day is no day; a file without a path. */
	axis_test_sonarr_set_episodes(fx->sonarr, 5, "{\"message\":\"NotFound\"}");
	axis_test_sonarr_set_episode_files(fx->sonarr, 6, "<html>Welcome</html>");
	axis_test_sonarr_set_episodes(fx->sonarr, 7, "[{\"id\":71,\"tvdbId\":9071,\"seasonNumber\":1,\"episodeNumber\":1,"
		"\"airDate\":\"2021-02-31\",\"monitored\":true,\"episodeFileId\":7001},42,{\"id\":72,\"episodeNumber\":2,"
		"\"monitored\":true},{\"id\":73,\"seasonNumber\":1,\"episodeNumber\":3,\"monitored\":\"yes\"},null,"
		"{\"id\":74,\"seasonNumber\":1,\"episodeNumber\":4,\"monitored\":false,\"episodeFileId\":null}]");
	axis_test_sonarr_set_episode_files(fx->sonarr, 7, "[{\"id\":7001,\"size\":12},{\"path\":\"/tv/no number.mkv\","
		"\"size\":3},\"not a file\"]");

	read_from_page(fx, source, "complete", 120000);
	/* Seven records, three a page: three pages, and no fourth. */
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/queue"), ==, 3);
	text = axis_test_db_query(fx->t->db, "SELECT count(*) || ' ' || min(arr_id) || ' ' || max(arr_id) FROM"
		" arr_plan_item WHERE in_queue");
	g_assert_cmpstr(text, ==, "7 10 16");
	/* The two whose list could not be read are incomplete, alone; for the
	 * first of them the second list was not even asked for. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(arr_id::text, ' ' ORDER BY seq) FROM arr_plan_item"
		" WHERE match = 'incomplete'");
	g_assert_cmpstr(text, ==, "5 6");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT count(*) FROM arr_plan_item WHERE match = 'matched'");
	g_assert_cmpstr(text, ==, "148");
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episode"), ==, 150);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episodefile"), ==, 149);
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_plan_episode e JOIN arr_plan_item i"
		" ON i.plan_id = e.plan_id AND i.seq = e.seq WHERE i.arr_id IN (5, 6)) + (SELECT count(*) FROM arr_plan_file f"
		" JOIN arr_plan_item i ON i.plan_id = f.plan_id AND i.seq = f.seq WHERE i.arr_id IN (5, 6))");
	g_assert_cmpstr(text, ==, "0");
	/* Series 7: two episodes and one file read, six entries that could not be
	 * (four that are no episode, two that are no file); the day that is none
	 * is no day, and the episode is kept.  Of the one whose place can be read
	 * and whose monitoring is no yes or no (episode 3), the place alone is
	 * kept, marked, so that a run leaves the episode there unmonitored and
	 * held: it is counted among the odd, not among the episodes. */
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT n_episodes || ' ' || n_episodes_monitored || ' ' ||"
		" n_episodes_with_file || ' ' || n_files || ' ' || n_odd FROM arr_plan_item WHERE arr_id = 7");
	g_assert_cmpstr(text, ==, "2 1 1 1 6");
	g_free(text);
	text = axis_test_db_query(fx->t->db, "SELECT string_agg(e.episode || ':' || coalesce(e.air_date::text, '-') || ':'"
		" || coalesce(e.file_arr_id::text, '-') || CASE WHEN e.odd THEN ':odd:' || e.monitored ELSE '' END, ' '"
		" ORDER BY e.ord) FROM arr_plan_episode e JOIN arr_plan_item i"
		" ON i.plan_id = e.plan_id AND i.seq = e.seq WHERE i.arr_id = 7");
	g_assert_cmpstr(text, ==, "1:-:7001 3:-:-:odd:false 4:-:-");

	posted = map_tv(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	g_assert_cmpuint(page.status, ==, 200);
	/* (The two incomplete ones count with those Sonarr does not monitor: what
	 * they are is not known.) */
	assert_contains(page.body, "Sonarr has 150 series: 133 monitored, 17 not.");
	assert_contains(page.body, "Sonarr was downloading episodes of 7 series when it was read.");
	assert_contains(page.body, "6 entries of Sonarr's lists of episodes and files could not be read and were left out.");
	assert_contains(page.body, "Sonarr's answer for it is incomplete (2)");
	/* 150, less the two incomplete and the fifteen Sonarr does not monitor. */
	assert_contains(page.body, "Would migrate 133 series");
	assert_contains(page.body, "1 to 100 of 133.");
	assert_contains(page.body, "Made Up Series 1 (2021)");
	assert_contains(page.body, "Monitored. Seasons monitored: 1, 2.");
	assert_contains(page.body, "Monitored. Seasons monitored: 1. Not monitored: 2.");
	assert_contains(page.body, "Some entries could not be read.");
	assert_contains(page.body, "/migrate/1/preview?bp=1#arr-bringing");
	assert_lacks(page.body, "Made Up Series 149 ");
	axis_test_reply_clear(&page);
	/* The second page of the series, and the third of the files that would
	 * not be linked (none is on the disk: two of each series but the one with
	 * a single file). */
	page = get_as(fx, fx->admin, "/migrate/1/preview?bp=1&fp=2");
	assert_contains(page.body, "101 to 133 of 133.");
	assert_contains(page.body, "Made Up Series 149 (2021)");
	assert_lacks(page.body, "Made Up Series 1 (2021)");
	assert_contains(page.body, "201 to 265 of 265.");
	assert_lacks(page.body, "Next page");
	axis_test_reply_clear(&page);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* What every failed reading must leave: a plan that cannot be previewed, and
 * no item, episode or file of it. */
static void
assert_failed_clean(
	Fixture     *fx,
	gint64       source,
	const gchar *problem
){
	g_autofree gchar *said = plan_problem(fx, source);
	g_autofree gchar *rows = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_plan_item) || ' ' ||"
		" (SELECT count(*) FROM arr_plan_episode) || ' ' || (SELECT count(*) FROM arr_plan_file) || ' ' ||"
		" (SELECT count(*) FROM arr_plan WHERE state = 'complete')");

	g_assert_cmpstr(said, ==, problem);
	g_assert_cmpstr(rows, ==, "0 0 0 0");
}

/* Spec: Thousands of titles (the limits); Sonarr stops answering.  The lists
 * of each series are read within a count of series, the bytes of all of them
 * and one deadline, at a pace; a reading that fails keeps nothing. */
static void
test_limits(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	AxisTestPlex *server = axis_test_sonarr_server(fx->sonarr);
	g_autoptr(GArray) times = NULL;
	g_autoptr(GPtrArray) requests = NULL;
	g_autofree gchar *text = NULL;
	AxisArrTuning tuning;
	AxisTestReply page;
	gint64 first = 0;
	gint64 last = 0;
	guint n = 0;
	guint i;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	seed_store(fx, 4);
	axis_test_sonarr_generate(fx->sonarr, 4, 40);

	/* More series than one reading asks about: refused as a whole, before any
	 * series' list is asked for. */
	tune(fx, &tuning);
	tuning.series_read_max = 3;
	axis_arr_set_tuning(fx->arr, &tuning);
	read_from_page(fx, source, "failed", 60000);
	assert_failed_clean(fx, source, "Sonarr's library is too large for this version of Axis to migrate (more than 3 "
		"series to read the episodes of).");
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episode"), ==, 0);

	/* The bytes of all the lists: the second series' passes them. */
	tune(fx, &tuning);
	tuning.series_total_bytes = 30000;
	axis_arr_set_tuning(fx->arr, &tuning);
	read_from_page(fx, source, "failed", 60000);
	assert_failed_clean(fx, source, "Sonarr's library is too large for this version of Axis to migrate (its lists of "
		"episodes are over 0 MB in all).");
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episode"), <, 4);

	/* One list larger than a list may be. */
	tune(fx, &tuning);
	tuning.series_max_bytes = 2000;
	axis_arr_set_tuning(fx->arr, &tuning);
	read_from_page(fx, source, "failed", 60000);
	assert_failed_clean(fx, source, "Sonarr's library is too large for this version of Axis to migrate (its answer "
		"for one series is over 0 MB).");

	/* The one deadline of all the lists, at the pace of four requests a
	 * second: the third request would start after it. */
	tune(fx, &tuning);
	tuning.requests_per_second = 4;
	tuning.series_deadline_ms = 1500;
	axis_arr_set_tuning(fx->arr, &tuning);
	read_from_page(fx, source, "failed", 60000);
	assert_failed_clean(fx, source, "Sonarr took too long to read: the whole reading passed its time limit. Read again.");

	/* Sonarr stops answering in the middle (its episode files answer 404, as
	 * another app would): the reading fails, and the first series' episodes
	 * are not left behind. */
	tune(fx, &tuning);
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_plex_route(server, "/api/v3/episodefile", -1, AXIS_TEST_PLEX_NORMAL, 404,
		"{\"message\":\"episodes-are-gone\"}", 0);
	read_from_page(fx, source, "failed", 60000);
	assert_failed_clean(fx, source, "This address answers, but not as Sonarr.");
	axis_test_plex_clear_routes(server);
	page = get_as(fx, fx->admin, "/migrate/1");
	assert_contains(page.body, "The last reading failed");
	assert_contains(page.body, "This address answers, but not as Sonarr.");
	assert_lacks(page.body, "episodes-are-gone");
	assert_lacks(page.body, "href=\"/migrate/1/preview\"");
	axis_test_reply_clear(&page);

	/* The pace: four series are eight requests, at most four a second, one at
	 * a time: no two of them start closer than a quarter of a second, and the
	 * eight take at least seven quarters. */
	tune(fx, &tuning);
	tuning.requests_per_second = 4;
	axis_arr_set_tuning(fx->arr, &tuning);
	axis_test_plex_reset_requests(server);
	read_from_page(fx, source, "complete", 60000);
	times = axis_test_plex_request_times(server);
	requests = axis_test_plex_requests(server);
	g_assert_cmpuint(times->len, ==, requests->len);
	for (i = 0; i < requests->len; i++) {
		const gchar *line = g_ptr_array_index(requests, i);
		gint64 at = g_array_index(times, gint64, i);

		if (!g_str_has_prefix(line, "GET /api/v3/episode"))
			continue;
		if (n > 0)
			/* (20 ms of grace for the clock of the stand-in's own thread.) */
			g_assert_cmpint(at - last, >=, 230 * 1000);
		else
			first = at;
		last = at;
		n++;
	}
	g_assert_cmpuint(n, ==, 8);
	g_assert_cmpint(last - first, >=, 7 * 230 * 1000);
	text = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM arr_plan_item) || ' ' || (SELECT count(*) FROM"
		" arr_plan_episode) || ' ' || (SELECT count(*) FROM arr_plan_file)");
	g_assert_cmpstr(text, ==, "4 160 160");
	axis_test_sonarr_assert_read_only(fx->sonarr);
	assert_secret_nowhere(fx, KEY);
}

/* One series' list that fails once (a 503) is asked for again and the series is
 * read whole; one that always fails makes that series incomplete alone, and
 * the reading completes.  A kind of series Axis does not know is a reported
 * skip. */
static void
test_patient(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autofree gchar *text = NULL;
	AxisArrTuning tuning;
	AxisTestReply page;

	axis_test_log_allow(G_LOG_LEVEL_WARNING, "*");
	axis_test_log_allow(G_LOG_LEVEL_MESSAGE, "*");
	seed_store(fx, 4);
	axis_test_sonarr_generate(fx->sonarr, 4, 40);
	tune(fx, &tuning);
	tuning.series_retry_ms = 100;
	axis_arr_set_tuning(fx->arr, &tuning);

	/* Once: asked for again, and the series is complete. */
	axis_test_sonarr_fail_series(fx->sonarr, 2, 503, 1);
	read_from_page(fx, source, "complete", 60000);
	text = axis_test_db_query(fx->t->db, "SELECT count(*) FILTER (WHERE match = 'matched') || ' ' || count(*) FILTER"
		" (WHERE match = 'incomplete') || ' ' || (SELECT count(*) FROM arr_plan_episode) FROM arr_plan_item");
	g_assert_cmpstr(text, ==, "4 0 160");
	g_clear_pointer(&text, g_free);

	/* Always: that series is incomplete, the others are read. */
	axis_test_sonarr_fail_series(fx->sonarr, 2, 503, -1);
	read_from_page(fx, source, "complete", 60000);
	text = axis_test_db_query(fx->t->db, "SELECT count(*) FILTER (WHERE match = 'matched') || ' ' || count(*) FILTER"
		" (WHERE match = 'incomplete') || ' ' || (SELECT count(*) FROM arr_plan_episode) || ' ' || (SELECT string_agg("
		"arr_id::text, ',') FROM arr_plan_item WHERE match = 'incomplete') FROM arr_plan_item");
	g_assert_cmpstr(text, ==, "3 1 120 2");
	axis_test_sonarr_fail_series(fx->sonarr, 0, 200, 0);

	/* A word for the kind of series that Axis does not know. */
	axis_test_db_exec(fx->t->db, "UPDATE arr_plan_item SET series_type = NULL WHERE arr_id = 3");
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=unknown-kind");
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Sonarr gives a kind of series Axis does not know");
	axis_test_reply_clear(&page);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* Task 5.4: 1,000 generated series with 60,000 episodes (and as many episode
 * files) read, matched from the store and paged within a stated time: the
 * reading within three minutes and a page of the preview within thirty
 * seconds, under the sanitizers. */
static void
test_thousand(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autofree gchar *counts = NULL;
	AxisTestReply posted;
	AxisTestReply page;
	gint64 read_ms;
	gint64 start;
	gint64 page_ms;
	guint n = 1000;

	seed_store(fx, n);
	axis_test_sonarr_generate(fx->sonarr, n, 60);
	axis_test_sonarr_set_profiles(fx->sonarr, "[{\"id\":4,\"name\":\"Balanced\"}]");

	read_ms = read_from_page(fx, source, "complete", 300000);
	counts = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) || ' ' || count(*) FILTER (WHERE match = 'matched')"
		" FROM arr_plan_item) || ' ' || (SELECT count(*) FROM arr_plan_episode) || ' ' || (SELECT count(*) FROM"
		" arr_plan_file)");
	g_assert_cmpstr(counts, ==, "1000 1000 60000 60000");
	/* All from the store: neither provider was asked once. */
	g_assert_cmpuint(axis_test_plex_n_requests(fx->fa->api), ==, 0);
	g_assert_cmpuint(axis_test_tvdb_gets(fx->tvdb), ==, 0);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episode"), ==, 1000);
	g_assert_cmpuint(axis_test_sonarr_count(fx->sonarr, "/api/v3/episodefile"), ==, 1000);

	posted = map_tv(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	start = g_get_monotonic_time();
	page = get_as(fx, fx->admin, "/migrate/1/preview");
	page_ms = (g_get_monotonic_time() - start) / 1000;
	g_assert_cmpuint(page.status, ==, 200);
	assert_contains(page.body, "Sonarr has 1,000 series: 900 monitored, 100 not.");
	assert_contains(page.body, "The episodes of 1,000 series were read: 60,000 episodes, 48,000 monitored in Sonarr, "
		"60,000 with a file.");
	assert_contains(page.body, "Would migrate 900 series with 54,000 episodes (43,200 monitored)");
	assert_contains(page.body, "link 0 files of their 54,000 episode files. 100 skipped");
	assert_contains(page.body, "0 of 60,000 files found at these paths in Axis.");
	assert_contains(page.body, "1 to 100 of 900.");
	assert_contains(page.body, "1 to 100 of 54,000.");
	assert_contains(page.body, "60 episodes: 48 monitored, 60 with a file.");
	g_assert_cmpuint(axis_test_web_assert_tables(page.body), ==, 5);
	axis_test_reply_clear(&page);
	page = get_as(fx, fx->admin, "/migrate/1/preview?skip=unmonitored&bp=8&fp=539");
	assert_contains(page.body, "801 to 900 of 900.");
	assert_contains(page.body, "53,901 to 54,000 of 54,000.");
	assert_contains(page.body, "1 to 100 of 100.");
	axis_test_reply_clear(&page);

	g_test_message("1,000 series, 60,000 episodes and 60,000 episode files: read in %" G_GINT64_FORMAT " ms, a page of "
		"the preview in %" G_GINT64_FORMAT " ms", read_ms, page_ms);
	g_assert_cmpint(read_ms, <, 180000);
	g_assert_cmpint(page_ms, <, 30000);
	axis_test_sonarr_assert_read_only(fx->sonarr);
}

/* No run of a Sonarr is ever queued: the post is refused, by the layer too,
 * and nothing is added. */
static void
test_run_refused(Fixture *fx, gconstpointer data)
{
	gint64 source = add_sonarr(fx);
	g_autoptr(GError) error = NULL;
	g_autoptr(AxisArrPreview) preview = NULL;
	g_autoptr(GPtrArray) decisions = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autofree gchar *jobs = NULL;
	AxisTestReply posted;

	odd_library(fx);
	read_from_page(fx, source, "complete", 90000);
	posted = map_tv(fx, source, fx->root_id, NULL);
	g_assert_cmpuint(posted.status, ==, 303);
	axis_test_reply_clear(&posted);
	before = library_digest(fx);

	/* The preview would bring three series, and says so to a run (slice S2:
	 * tests/test-web-sonarr-run.c brings them).  This fixture has no TV
	 * module: a run has nothing to bring series through. */
	g_assert_true(axis_arr_preview_decide(fx->arr, source, AXIS_ARR_SKIP_NONE, 0, 0, &preview, &decisions, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(preview->counts[AXIS_ARR_SKIP_NONE], ==, 3);
	g_assert_nonnull(preview->digest);
	g_assert_cmpuint(decisions->len, ==, 3);

	/* With the very digest of the preview: refused, in words. */
	{
		g_autofree gchar *form = g_strdup_printf("digest=%s", preview->digest);

		posted = post_as(fx, fx->admin, "/migrate/1/preview", "/migrate/1/run", form);
	}
	g_assert_cmpuint(posted.status, ==, 409);
	assert_contains(posted.body, "Series cannot be migrated: the TV module is not there. Nothing was migrated.");
	axis_test_reply_clear(&posted);
	g_assert_false(axis_arr_request_apply(fx->arr, source, preview->digest, NULL, "root", &error));
	g_assert_error(error, AXIS_ARR_ERROR, AXIS_ARR_ERROR_FAILED);
	jobs = axis_test_db_query(fx->t->db, "SELECT count(*) FROM job WHERE kind = 'arr.apply'");
	g_assert_cmpstr(jobs, ==, "0");
	after = library_digest(fx);
	g_assert_cmpstr(after, ==, before);
	g_free(after);
	after = axis_test_db_query(fx->t->db, "SELECT (SELECT count(*) FROM series) + (SELECT count(*) FROM movie) +"
		" (SELECT count(*) FROM series_file) + (SELECT count(*) FROM arr_brought)");
	g_assert_cmpstr(after, ==, "0");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) AXIS_TEST_ADD_DB_CASE("/web-sonarr/" path, Fixture, NULL, func)
	ADD("connections/app-kept", test_app_kept);
	ADD("connections/viewer", test_viewer);
	ADD("reading/read-and-preview", test_read_and_preview);
	ADD("reading/suggested", test_suggested);
	ADD("reading/no-tvdb-key", test_no_tvdb_key);
	ADD("reading/paging-and-odd", test_paging_and_odd);
	ADD("reading/limits", test_limits);
	ADD("reading/patient", test_patient);
	ADD("reading/thousand", test_thousand);
	ADD("run/refused", test_run_refused);
#undef ADD
	return g_test_run();
}
